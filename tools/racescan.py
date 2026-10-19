#!/usr/bin/env python3
"""Phase 63.1: candidate lists for the concurrency review.

Parses every translation unit of one build (its compile_commands.json) with
libclang and works out, for each piece of writable static storage, which
execution contexts can reach the functions that touch it:

  kernel          kernel_main() and everything it calls (the boot and the shell)
  task:<name>     a task_create*() entry, named by its string literal
  task:<name>     a driver task's serve callback (driver_task_spec_t .serve)
  chan:<name>     a chan_register() handler -- runs in whichever task calls it
  isr:<handler>   a devirq_attach() handler

A function address taken anywhere else (an ops table, a callback argument)
counts as a call from whoever takes it, and a variable's initializer passes
its function addresses to whoever references the variable. That
over-approximates on purpose: this produces candidates for a person to judge,
not verdicts.

Three lists:
  1. static storage reached from two or more contexts (shared mutable state)
  2. files whose raw register accesses are reached from two or more contexts
     (a driver bypassed by someone other than its owner)
  3. chan_call() / chan_call_wait() sites: a plain chan_call() in a loop is a
     hand-written retry, which chan_call_wait() replaces

A declaration preceded by a comment starting with one of
"single task:", "guarded by:", "interrupt-shared:" or "accepted:" counts as
triaged (63.2) and is listed separately.

Usage:
  tools/racescan.py [--build build/esp32p4-tab5] [--out build/racescan]
"""
from __future__ import annotations

import argparse
import json
import re
import shlex
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import clang.cindex as ci

REPO = Path(__file__).resolve().parents[1]
K = ci.CursorKind

TASK_CREATORS = {"task_create", "task_create_sized", "task_create_driver", "task_create_pinned"}
LOCK_TYPES = re.compile(r"\b(ylock_t|spinlock_t|chan_endpoint_t)\b")
MARK = re.compile(r"(single task|guarded by|interrupt-shared|accepted):", re.I)
DECL_LINE = re.compile(r"^(?!(return|goto|break|continue|case|if|else|while|for|do|switch)\b)(static\s|extern\s|volatile\s|const\s)?[\w\s\*\[\](),?:+\-/]+[\w\]\)]\s*(=[^;]*)?;(\s*/[/*].*)?$")
STRUCT_OPEN = re.compile(r"^(static\s+|typedef\s+)?(volatile\s+)?(struct|union)\b[\w\s]*\{$")
CLOCK_FUNCS = {"time_get_ms", "time_get_us", "time_get_ns"}
LOCK_FUNCS = {"spin_lock_at", "spin_lock_irqsave_at", "spin_lock_irqsave_bottom_at",
              "ylock_acquire_at", "ylock_try_acquire", "console_lock"}
# Selftest and demo tasks, started by a shell command to exercise the kernel:
# real code, listed after the system's own contexts.
# A handler that reaches one of these can block in interrupt context: listed on
# its own (section 4), and not followed further for that handler, so the
# scheduler behind it does not count as reached from the interrupt.
BLOCKING_FUNCS = {"sched_yield", "task_block", "task_block_until_ms", "task_sleep_ms",
                  "ylock_acquire_at", "chan_call", "chan_call_wait"}
TEST_CONTEXTS = {"task:demoA", "task:demoB", "task:sized1", "task:smpload", "task:smpw",
                 "task:spinner", "task:stressA", "task:stressB", "task:hog", "task:urgent",
                 "task:locktest", "task:clicspin", "task:perftw", "task:cyclewait",
                 "task:chanecho", "task:uprobe", "task:llpsoak", "task:chanq",
                 "task:stress", "task:stressrun", "task:stressrot"}


@dataclass
class Func:
    key: str
    name: str
    file: str
    line: int
    calls: set[str] = field(default_factory=set)        # callee names (resolved later)
    task_calls: set[str] = field(default_factory=set)   # callees behind a devirq_in_handler() guard
    locks: bool = False                                 # calls a lock acquire
    addr_of: set[str] = field(default_factory=set)      # function names whose address it takes
    var_refs: set[str] = field(default_factory=set)     # variable keys
    var_writes: set[str] = field(default_factory=set)   # variable keys it may write
    mmio: int = 0
    chan_calls: list[dict] = field(default_factory=list)
    tu: str = ""                                        # the translation unit it was compiled in


@dataclass
class Var:
    key: str
    name: str
    file: str
    line: int
    kind: str          # global / file-static / local-static
    type: str
    size: int
    volatile: bool
    marked: str        # the triage comment, or ""
    readonly: bool = False
    tu: str = ""
    init_funcs: set[str] = field(default_factory=set)
    init_vars: set[str] = field(default_factory=set)


def rel(path: str | None) -> str:
    if not path:
        return "?"
    try:
        return str(Path(path).resolve().relative_to(REPO))
    except ValueError:
        return path


def in_repo(cur: ci.Cursor) -> bool:
    f = cur.location.file
    return bool(f) and str(Path(f.name).resolve()).startswith(str(REPO))


def clang_args(entry: dict) -> list[str]:
    argv = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    keep: list[str] = []
    it = iter(argv[1:])
    for a in it:
        if a in ("-o", "-MF", "-MT", "-MQ"):
            next(it, None)
            continue
        if a.startswith(("-D", "-I", "-std=", "-include", "-isystem")):
            keep.append(a)
            if a in ("-include", "-isystem"):
                keep.append(next(it, ""))
    return keep + ["--target=riscv32-unknown-elf", "-ffreestanding", "-Wno-everything"]


class Scanner:
    def __init__(self) -> None:
        self.edges: dict[str, set[str]] = {}
        self.task_edges: dict[str, set[str]] = defaultdict(set)   # edges a handler does not take
        self.isr_blocking: dict[str, set[str]] = defaultdict(set)   # isr context -> blocking funcs
        self.all_roots: dict[str, set[str]] = {}
        self.funcs: dict[str, Func] = {}
        self.vars: dict[str, Var] = {}
        self.roots: dict[str, set[str]] = defaultdict(set)   # context -> function names
        self.root_args: set[tuple[str, str]] = set()          # (caller key, fn) excluded from edges
        self.static_funcs: dict[str, dict[str, str]] = defaultdict(dict)  # file -> name -> key
        self.src_lines: dict[str, list[str]] = {}
        self.cur_tu = ""

    # --- keys -----------------------------------------------------------------

    def func_key(self, cur: ci.Cursor) -> str:
        f = rel(cur.location.file.name if cur.location.file else None)
        if cur.linkage == ci.LinkageKind.INTERNAL:
            return f"{f}::{cur.spelling}"
        return cur.spelling

    def var_key(self, cur: ci.Cursor) -> str:
        f = rel(cur.location.file.name if cur.location.file else None)
        par = cur.semantic_parent
        if par is not None and par.kind == K.FUNCTION_DECL:
            return f"{f}::{par.spelling}::{cur.spelling}"
        if cur.linkage == ci.LinkageKind.INTERNAL:
            return f"{f}::{cur.spelling}"
        return cur.spelling

    def marked(self, cur: ci.Cursor) -> str:
        f = cur.location.file
        if not f:
            return ""
        lines = self.src_lines.get(f.name)
        if lines is None:
            try:
                lines = Path(f.name).read_text(errors="replace").splitlines()
            except OSError:
                lines = []
            self.src_lines[f.name] = lines
        i = cur.location.line - 1
        # the declaration's own line, then the comment block just above it
        if 0 <= i < len(lines) and MARK.search(lines[i]):
            return MARK.search(lines[i]).group(0)
        # One comment covers the run of declarations below it, up to a blank line.
        j = i - 1
        while j >= 0 and j >= i - 40:
            s = lines[j].strip()
            if not s:
                break
            m = MARK.search(s)
            if m:
                return m.group(0)
            if not (s.startswith(("/*", "*", "//", "#")) or s.endswith("*/") or DECL_LINE.match(s)
                    or STRUCT_OPEN.match(s)):
                break
            j -= 1
        return ""

    # --- the walk -------------------------------------------------------------

    def add_var(self, cur: ci.Cursor) -> str | None:
        if cur.storage_class not in (ci.StorageClass.STATIC, ci.StorageClass.NONE):
            return None
        par = cur.semantic_parent
        local = par is not None and par.kind == K.FUNCTION_DECL
        if local and cur.storage_class != ci.StorageClass.STATIC:
            return None
        t = cur.type
        tname = t.spelling
        # Read-only storage is kept for the function addresses in its
        # initializer (a const ops or builtins table is how most callbacks are
        # reached), and left out of the lists: nothing writes it.
        readonly = t.is_const_qualified() or (t.kind == ci.TypeKind.CONSTANTARRAY and
                                              t.element_type.is_const_qualified())
        if LOCK_TYPES.search(tname):
            return None
        key = self.var_key(cur)
        if key in self.vars:
            return key
        # `static T g;` is a tentative definition, not a definition, to clang;
        # only an `extern` declaration leaves the definition to another unit.
        if cur.storage_class == ci.StorageClass.EXTERN and not cur.is_definition():
            return key
        kind = "local-static" if local else ("file-static" if cur.linkage == ci.LinkageKind.INTERNAL else "global")
        size = t.get_size()
        v = Var(key, cur.spelling, rel(cur.location.file.name), cur.location.line, kind, tname,
                size if size > 0 else 0, t.is_volatile_qualified() or "volatile" in tname,
                "" if readonly else self.marked(cur), readonly=readonly, tu=self.cur_tu)
        for d in cur.walk_preorder():
            if d.kind == K.DECL_REF_EXPR and d.referenced is not None:
                r = d.referenced
                if r.kind == K.FUNCTION_DECL:
                    v.init_funcs.add(r.spelling)
                elif r.kind == K.VAR_DECL and r != cur:
                    v.init_vars.add(self.var_key(r))
        self.vars[key] = v
        return key

    def call_name(self, call: ci.Cursor) -> str | None:
        r = call.referenced
        if r is not None and r.kind == K.FUNCTION_DECL:
            return r.spelling
        return None

    def string_arg(self, cur: ci.Cursor) -> str | None:
        for d in cur.walk_preorder():
            if d.kind == K.STRING_LITERAL:
                return d.spelling.strip('"')
        return None

    def func_arg(self, cur: ci.Cursor) -> str | None:
        for d in cur.walk_preorder():
            if d.kind == K.DECL_REF_EXPR and d.referenced is not None and d.referenced.kind == K.FUNCTION_DECL:
                return d.referenced.spelling
        return None

    def note_registration(self, fkey: str, call: ci.Cursor, name: str) -> None:
        args = list(call.get_arguments())
        if name in TASK_CREATORS and len(args) >= 2:
            entry = self.func_arg(args[1])
            if entry:
                label = self.string_arg(args[0]) or entry
                self.roots[f"task:{label}"].add(entry)
                self.root_args.add((fkey, entry))
        elif name == "devirq_attach" and len(args) >= 2:
            h = self.func_arg(args[1])
            if h:
                self.roots[f"isr:{h}"].add(h)
                self.root_args.add((fkey, h))
        elif name == "chan_register" and len(args) >= 2:
            h = self.func_arg(args[1])
            if h:
                label = self.string_arg(args[0]) or h
                self.roots[f"chan:{label}"].add(h)
                self.root_args.add((fkey, h))

    def note_spec(self, cur: ci.Cursor) -> None:
        """driver_task_spec_t initializers: .name and .serve."""
        if "driver_task_spec_t" not in cur.type.spelling:
            return
        name = serve = None
        for d in cur.walk_preorder():
            if d.kind == K.STRING_LITERAL and name is None:
                name = d.spelling.strip('"')
            if d.kind == K.DECL_REF_EXPR and d.referenced is not None and d.referenced.kind == K.FUNCTION_DECL:
                serve = serve or d.referenced.spelling
        if serve:
            self.roots[f"task:{name or serve}"].add(serve)
            self.root_args.add(("<spec>", serve))

    def walk_function(self, cur: ci.Cursor) -> None:
        key = self.func_key(cur)
        if cur.linkage == ci.LinkageKind.INTERNAL:
            # By its own file and by every unit that compiles it (a static
            # inline in a header, a table's callbacks in an included file).
            self.static_funcs[rel(cur.location.file.name)][cur.spelling] = key
            self.static_funcs[self.cur_tu].setdefault(cur.spelling, key)
        if key in self.funcs:
            return
        fn = Func(key, cur.spelling, rel(cur.location.file.name), cur.location.line, tu=self.cur_tu)
        self.funcs[key] = fn

        def param_is_const_ptr(call: ci.Cursor, i: int) -> bool | None:
            r = call.referenced
            if r is None:
                return None
            try:
                params = list(r.type.argument_types())
            except Exception:
                return None
            if i >= len(params):
                return None            # variadic: a value, read
            t = params[i]
            if t.kind != ci.TypeKind.POINTER:
                return True            # by value: read
            return t.get_pointee().is_const_qualified()

        def is_array(e: ci.Cursor) -> bool:
            while e.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR):
                kids = list(e.get_children())
                if len(kids) != 1:
                    return False
                e = kids[0]
            return e.type.kind in (ci.TypeKind.CONSTANTARRAY, ci.TypeKind.INCOMPLETEARRAY)

        # `write`: this subtree is on an lvalue path (an assignment's target,
        # ++/--, an address handed to something that may write through it).
        # It follows parens, implicit casts, an array's base and a `.` member,
        # and stops at arithmetic, a dereference and `->`: REG(base + off) = v
        # writes the register, not `base`.
        # `arg`: the path began at a call argument, where a pointer variable is
        # passed by value (not written) and an array decays (may be written).
        # `tonly`: not reached from a device handler -- the then-branch of
        # `if (!devirq_in_handler())`, or what follows `if (devirq_in_handler()) return;`.
        def visit(node: ci.Cursor, loops: list[ci.Cursor], write: bool = False, arg: bool = False,
                  tonly: bool = False) -> None:
            kids = list(node.get_children())
            nk = node.kind
            rest_tonly = False
            for pos, ch in enumerate(kids):
                k = ch.kind
                w = False
                a = arg
                t = tonly or rest_tonly
                if nk == K.IF_STMT and pos == 1 and handler_guard(kids[0]) == "not":
                    t = True
                if nk == K.BINARY_OPERATOR and pos == 0 and \
                        node.binary_operator == ci.BinaryOperator.Assign:
                    w = True
                elif nk == K.COMPOUND_ASSIGNMENT_OPERATOR and pos == 0:
                    w = True
                elif nk == K.UNARY_OPERATOR:
                    toks = [t.spelling for t in node.get_tokens()]
                    if toks and (toks[0] in ("++", "--") or toks[-1] in ("++", "--")):
                        w = True
                    elif toks and toks[0] == "&":
                        w = write              # an address goes where its consumer sends it
                        a = False              # ...and it is the variable's own address
                elif nk == K.CALL_EXPR and pos > 0:
                    const = param_is_const_ptr(node, pos - 1)
                    w = const is False         # a non-const pointer parameter may write
                    a = True
                elif write and nk in (K.PAREN_EXPR, K.UNEXPOSED_EXPR):
                    w = True
                elif write and nk == K.ARRAY_SUBSCRIPT_EXPR and pos == 0:
                    # an array's base arrives through its decay to a pointer
                    w = ch.type.kind != ci.TypeKind.POINTER or is_array(ch)
                elif write and nk == K.MEMBER_REF_EXPR:
                    w = ch.type.kind != ci.TypeKind.POINTER
                elif nk == K.VAR_DECL and node.type.kind == ci.TypeKind.POINTER and \
                        not node.type.get_pointee().is_const_qualified():
                    # T *p = &var / T *p = array: the address escapes into p,
                    # which may write through it (a pointer copied is a read)
                    w = True
                    a = True
                elif nk in (K.BINARY_OPERATOR,) and node.binary_operator == ci.BinaryOperator.Assign and pos == 1:
                    # p = &var / p = array: the address escapes into p
                    w = False
                    if ch.kind == K.UNARY_OPERATOR or ch.type.kind == ci.TypeKind.POINTER:
                        w = True
                if k == K.VAR_DECL:
                    vk = self.add_var(ch)
                    if vk:
                        fn.var_refs.add(vk)
                    if "driver_task_spec_t" in ch.type.spelling:
                        self.note_spec(ch)
                elif k == K.CALL_EXPR:
                    nm = self.call_name(ch)
                    if nm:
                        (fn.task_calls if t else fn.calls).add(nm)
                        self.note_registration(key, ch, nm)
                        if nm in ("chan_call", "chan_call_wait"):
                            loop = loops[-1] if loops else None
                            reads_clock = False
                            if loop is not None:
                                for d in loop.walk_preorder():
                                    if d.kind == K.CALL_EXPR and self.call_name(d) in CLOCK_FUNCS:
                                        reads_clock = True
                                        break
                            fn.chan_calls.append({"line": ch.location.line, "in_loop": loop is not None,
                                                  "loop_reads_clock": reads_clock,
                                                  "waits": nm == "chan_call_wait"})
                elif k == K.DECL_REF_EXPR and ch.referenced is not None:
                    r = ch.referenced
                    if r.kind == K.VAR_DECL and (r.storage_class == ci.StorageClass.STATIC or
                                                 r.semantic_parent is None or
                                                 r.semantic_parent.kind == K.TRANSLATION_UNIT):
                        vk = self.add_var(r)
                        if vk:
                            fn.var_refs.add(vk)
                            if w and not (arg and r.type.kind == ci.TypeKind.POINTER):
                                fn.var_writes.add(vk)
                    elif r.kind == K.FUNCTION_DECL:
                        par = node
                        if not (par.kind == K.CALL_EXPR and next(iter(par.get_children()), None) == ch):
                            fn.addr_of.add(r.spelling)
                elif k == K.CSTYLE_CAST_EXPR:
                    t = ch.type
                    if t.kind == ci.TypeKind.POINTER and t.get_pointee().is_volatile_qualified():
                        inner = list(ch.get_children())
                        if inner and inner[-1].type.kind not in (ci.TypeKind.POINTER,):
                            fn.mmio += 1
                if k in (K.FOR_STMT, K.WHILE_STMT, K.DO_STMT):
                    visit(ch, loops + [ch], w, a and w, t)
                else:
                    visit(ch, loops, w, a and w, t)
                if nk == K.COMPOUND_STMT and k == K.IF_STMT:
                    ik = list(ch.get_children())
                    if len(ik) == 2 and handler_guard(ik[0]) == "is" and returns(ik[1]):
                        rest_tonly = True

        def handler_guard(cond: ci.Cursor) -> str | None:
            toks = [x.spelling for x in cond.get_tokens()]
            while toks[:1] == ["("] and toks[-1:] == [")"]:
                toks = toks[1:-1]
            if toks == ["devirq_in_handler", "(", ")"]:
                return "is"
            if toks == ["!", "devirq_in_handler", "(", ")"]:
                return "not"
            return None

        def returns(st: ci.Cursor) -> bool:
            if st.kind == K.RETURN_STMT:
                return True
            kids = list(st.get_children())
            return st.kind == K.COMPOUND_STMT and len(kids) == 1 and kids[0].kind == K.RETURN_STMT

        visit(cur, [])
        # unexposed callee wrappers: a call's first child is the callee reference
        fn.addr_of -= fn.calls | fn.task_calls
        fn.locks = bool(fn.calls & LOCK_FUNCS)

    def scan_tu(self, path: str, args: list[str], idx: ci.Index) -> int:
        self.cur_tu = rel(path)
        tu = idx.parse(path, args=args)
        errs = sum(1 for d in tu.diagnostics if d.severity >= 3)
        for cur in tu.cursor.walk_preorder():
            if not in_repo(cur):
                continue
            if cur.kind == K.FUNCTION_DECL and cur.is_definition():
                self.walk_function(cur)
            elif cur.kind == K.VAR_DECL and cur.semantic_parent is not None and \
                    cur.semantic_parent.kind == K.TRANSLATION_UNIT:
                self.add_var(cur)
                if "driver_task_spec_t" in cur.type.spelling:
                    self.note_spec(cur)
        return errs

    # --- reachability ---------------------------------------------------------

    def resolve(self, file: str, name: str, tu: str = "") -> str | None:
        sk = self.static_funcs.get(file, {}).get(name) or self.static_funcs.get(tu, {}).get(name)
        if sk:
            return sk
        return name if name in self.funcs else None

    def contexts(self) -> dict[str, set[str]]:
        edges: dict[str, set[str]] = defaultdict(set)
        self.task_edges = defaultdict(set)
        for fk, fn in self.funcs.items():
            for c in fn.calls:
                t = self.resolve(fn.file, c, fn.tu)
                if t:
                    edges[fk].add(t)
            for c in fn.task_calls - fn.calls:
                t = self.resolve(fn.file, c, fn.tu)
                if t:
                    edges[fk].add(t)
                    self.task_edges[fk].add(t)
            for a in fn.addr_of:
                if (fk, a) in self.root_args:
                    continue
                t = self.resolve(fn.file, a, fn.tu)
                if t:
                    edges[fk].add(t)
            # function addresses in the initializers of variables it touches
            seen: set[str] = set()
            stack = list(fn.var_refs)
            while stack:
                vk = stack.pop()
                if vk in seen or vk not in self.vars:
                    continue
                seen.add(vk)
                v = self.vars[vk]
                for f in v.init_funcs:
                    if ("<spec>", f) in self.root_args:
                        continue
                    t = self.resolve(v.file, f, v.tu)
                    if t:
                        edges[fk].add(t)
                stack.extend(v.init_vars)

        self.edges = edges
        roots = dict(self.roots)
        roots["kernel"] = {"kernel_main"}
        self.all_roots = roots
        reach: dict[str, set[str]] = defaultdict(set)   # function key -> contexts
        for ctx, starts in roots.items():
            stack = []
            for s in starts:
                for f in self.funcs:
                    if f == s or f.endswith("::" + s):
                        stack.append(f)
            seen: set[str] = set()
            isr = ctx.startswith("isr:")
            while stack:
                f = stack.pop()
                if f in seen:
                    continue
                seen.add(f)
                reach[f].add(ctx)
                if isr and self.funcs[f].name in BLOCKING_FUNCS:
                    self.isr_blocking[ctx].add(f)
                    continue
                nxt = edges.get(f, set())
                stack.extend(nxt - self.task_edges.get(f, set()) if isr else nxt)
        return reach

    def why(self, ctx: str, target: str) -> list[str] | None:
        """The shortest call path from a context's entry to a function (by name)."""
        starts = [f for s in self.all_roots.get(ctx, ()) for f in self.funcs
                  if f == s or f.endswith("::" + s)]
        prev: dict[str, str | None] = {f: None for f in starts}
        queue = list(starts)
        while queue:
            f = queue.pop(0)
            if f == target or f.endswith("::" + target):
                path = []
                cur: str | None = f
                while cur is not None:
                    path.append(cur)
                    cur = prev[cur]
                return path[::-1]
            if ctx.startswith("isr:") and self.funcs[f].name in BLOCKING_FUNCS:
                continue
            nxt = self.edges.get(f, set())
            if ctx.startswith("isr:"):
                nxt = nxt - self.task_edges.get(f, set())
            for t in sorted(nxt):
                if t not in prev:
                    prev[t] = f
                    queue.append(t)
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", default="build/esp32p4-tab5")
    ap.add_argument("--out", default="build/racescan")
    ap.add_argument("--only", default="", help="substring filter on source paths (debugging)")
    ap.add_argument("--why", nargs=2, action="append", default=[], metavar=("CONTEXT", "FUNC"),
                    help="print the shortest call path from CONTEXT (e.g. isr:uart_isr) to FUNC")
    a = ap.parse_args()

    cc = json.loads((REPO / a.build / "compile_commands.json").read_text())
    sc = Scanner()
    idx = ci.Index.create()
    bad = 0
    for e in cc:
        p = e["file"]
        if a.only and a.only not in p:
            continue
        if not str(Path(p).resolve()).startswith(str(REPO)) or not p.endswith(".c"):
            continue
        bad += sc.scan_tu(p, clang_args(e), idx) > 0
    reach = sc.contexts()
    if a.why:
        for ctx, fn in a.why:
            path = sc.why(ctx, fn)
            print(f"{ctx} -> {fn}: " + (" -> ".join(path) if path else "unreachable"))
        return 0

    # 1. shared static storage: who writes, who reads
    w_ctx: dict[str, set[str]] = defaultdict(set)
    r_ctx: dict[str, set[str]] = defaultdict(set)
    writers: dict[str, set[str]] = defaultdict(set)
    var_users: dict[str, set[str]] = defaultdict(set)
    for fk, fn in sc.funcs.items():
        ctx = reach.get(fk, set())
        for vk in fn.var_refs:
            var_users[vk].add(fn.name)
            r_ctx[vk] |= ctx
        for vk in fn.var_writes:
            w_ctx[vk] |= ctx
            writers[vk].add(fn.name)
    locked = {fn.name: fn.locks for fn in sc.funcs.values()}
    shared = []
    for vk, v in sc.vars.items():
        if v.readonly:
            continue
        wc, rc = w_ctx.get(vk, set()), r_ctx.get(vk, set())
        if not wc or len(wc | rc) < 2:
            continue                       # never written after its initializer, or one context
        sys_w = {c for c in wc if c not in TEST_CONTEXTS}
        # S: written only by shells (the kernel task and the shell workers) --
        # a race needs two terminals running the same command at once.
        shells = {"kernel", "task:lsh-worker"}
        group = "B" if len(wc) < 2 else ("S" if wc <= shells else "A")
        isr = any(c.startswith("isr:") for c in wc | rc)
        shared.append((group, 0 if isr else 1, 0 if len(sys_w) >= 2 or group == "B" else 1,
                       -len(wc), vk, v, wc, rc))
    order = {"A": 0, "S": 1, "B": 2}
    shared.sort(key=lambda t: (order[t[0]], t[1], t[2], t[3], t[4]))

    # 2. register access by file
    mmio_files: dict[str, set[str]] = defaultdict(set)
    mmio_funcs: dict[str, int] = defaultdict(int)
    for fk, fn in sc.funcs.items():
        if fn.mmio:
            mmio_files[fn.file] |= reach.get(fk, set())
            mmio_funcs[fn.file] += 1

    # 3. chan_call sites
    chan_sites = []
    for fk, fn in sc.funcs.items():
        for s in fn.chan_calls:
            chan_sites.append((fn.file, s["line"], fn.name, s["waits"], s["in_loop"],
                               s["loop_reads_clock"], sorted(reach.get(fk, set()))))
    chan_sites.sort()

    out = REPO / a.out
    out.mkdir(parents=True, exist_ok=True)
    unmarked = [s for s in shared if not s[5].marked]
    marked = [s for s in shared if s[5].marked]
    md: list[str] = [
        f"# racescan: {a.build}",
        "",
        f"{len(cc)} translation units ({bad} with parse errors), {len(sc.funcs)} functions, "
        f"{sum(1 for v in sc.vars.values() if not v.readonly)} writable static variables, {len(sc.roots) + 1} contexts.",
        "",
        f"## 1. Static storage written after its initializer and reached from two or more contexts: "
        f"{len(unmarked)} unmarked, {len(marked)} marked",
        "",
        "**A**: written from two or more contexts. **S**: written only by shells (kernel task, "
        "shell workers). **B**: written in one context, read in others.",
        "`all writers lock` sees only a lock taken in the writing function itself, not by its caller.",
        "Interrupt contexts first. Selftest/demo tasks are named in TEST_CONTEXTS.",
        "",
        "| grp | variable | kind | size | at | written in | also read in | writers | all writers lock |",
        "| :--- | :--- | :--- | ---: | :--- | :--- | :--- | :--- | :--- |",
    ]
    def ctxlist(cs: set[str]) -> str:
        sysc = sorted(c for c in cs if c not in TEST_CONTEXTS)
        tst = len([c for c in cs if c in TEST_CONTEXTS])
        if len(sysc) > 8:
            return f"{len(sysc)} contexts" + (f" (+{tst} test)" if tst else "")
        return ", ".join(sysc) + (f" (+{tst} test)" if tst else "")
    for grp, _, _, _, vk, v, wc, rc in unmarked:
        ws = sorted(writers[vk])
        md.append(f"| {grp} | `{v.name}`{' (volatile)' if v.volatile else ''} | {v.kind} | {v.size} | "
                  f"{v.file}:{v.line} | {ctxlist(wc)} | {ctxlist(rc - wc)} | "
                  f"{', '.join(ws[:5])}{' …' if len(ws) > 5 else ''} | "
                  f"{'yes' if all(locked.get(x, False) for x in ws) else 'no'} |")
    if marked:
        md += ["", "### Marked (63.2)", ""]
        md += [f"- `{v.name}` {v.file}:{v.line}: {v.marked}" for *_, v, _, _ in marked]
    md += ["", "## 2. Files with raw register access reached from two or more contexts", "",
           "| file | functions with MMIO | contexts |", "| :--- | ---: | :--- |"]
    for f in sorted(mmio_files, key=lambda f: -len(mmio_files[f])):
        if len(mmio_files[f]) >= 2:
            md.append(f"| {f} | {mmio_funcs[f]} | {', '.join(sorted(mmio_files[f]))} |")
    md += ["", "## 3. chan_call() sites", "",
           "`waits`: chan_call_wait(), a turn in the endpoint's FIFO. A plain chan_call() in a",
           "loop is a hand-written retry.", "",
           "| at | function | call | in a loop | loop reads the clock | contexts |",
           "| :--- | :--- | :--- | :--- | :--- | :--- |"]
    for f, ln, nm, waits, loop, clock, ctxs in chan_sites:
        md.append(f"| {f}:{ln} | {nm} | {'waits' if waits else 'refuses'} | {'yes' if loop else 'no'} | "
                  f"{'yes' if clock else ('no' if loop else '-')} | {', '.join(ctxs)} |")
    md += ["", "## 4. Interrupt handlers that can reach a blocking call", "",
           "Each path is the shortest one; a guard on the way (a check for a held spinlock, a",
           "consumer task that exists) is for a person to confirm.", ""]
    isr_block = []
    for ctx in sorted(sc.isr_blocking):
        for f in sorted(sc.isr_blocking[ctx]):
            path = sc.why(ctx, f) or [f]
            isr_block.append({"context": ctx, "function": sc.funcs[f].name,
                              "path": [p.split("::")[-1] for p in path]})
            md.append(f"- `{ctx}` -> `{sc.funcs[f].name}`: " + " -> ".join(p.split("::")[-1] for p in path))
    if not isr_block:
        md.append("none")
    md += ["", "## Contexts", ""]
    for ctx in sorted(sc.roots):
        md.append(f"- `{ctx}`: {', '.join(sorted(sc.roots[ctx]))}")
    (out / "candidates.md").write_text("\n".join(md) + "\n")
    (out / "candidates.json").write_text(json.dumps({
        "shared": [{"group": grp, "var": vk, "file": v.file, "line": v.line, "kind": v.kind,
                    "size": v.size, "volatile": v.volatile, "marked": v.marked,
                    "written_in": sorted(wc), "read_in": sorted(rc - wc),
                    "writers": sorted(writers[vk])} for grp, _, _, _, vk, v, wc, rc in shared],
        "mmio": {f: sorted(c) for f, c in mmio_files.items()},
        "isr_blocking": isr_block,
        "chan_calls": [{"file": f, "line": ln, "function": nm, "waits": w, "in_loop": lp,
                        "loop_reads_clock": ck, "contexts": cx}
                       for f, ln, nm, w, lp, ck, cx in chan_sites],
    }, indent=1))
    ga = sum(1 for t in unmarked if t[0] == "A")
    gs = sum(1 for t in unmarked if t[0] == "S")
    print(f"{len(sc.funcs)} functions, {sum(1 for v in sc.vars.values() if not v.readonly)} static variables, {len(sc.roots) + 1} contexts; "
          f"{len(unmarked)} shared unmarked (A {ga}, S {gs}, B {len(unmarked) - ga - gs}), {len(marked)} marked; "
          f"{sum(1 for f in mmio_files if len(mmio_files[f]) >= 2)} MMIO files in 2+ contexts; "
          f"{len(chan_sites)} chan_call sites ({sum(1 for c in chan_sites if c[3])} waiting, "
          f"{sum(1 for c in chan_sites if not c[3] and c[4])} hand-written retries); {len(isr_block)} handler paths to a blocking call "
          f"-> {out / 'candidates.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
