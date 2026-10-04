/* zfront.h — LugalOS kernel front for the Z-machine interpreter
 *
 * Session model (mirrors the chess engine's memory discipline,
 * plan/phase38_psram.md): nothing is allocated at boot, nothing lives in
 * .bss/.data.  Everything heavy — the 128 KB address space, the story
 * image kept for RESTART, and the VM struct itself (~52 KB) — is claimed
 * from palloc_pages_bulk() (PSRAM on boards that have it; the fallback
 * counts in /proc/meminfo) when a game starts and released on every exit
 * path.  While `zmachine` is not running, it costs zero bytes of RAM.
 */
#ifndef ZMACHINE_ZFRONT_H
#define ZMACHINE_ZFRONT_H

/* Run one interactive Z-machine session.
 *
 * `name` is a game name (resolved against the default story directory
 * /sd0/games/ as "<name>.z3") or an explicit path (anything containing
 * '/' or ending in .z3/.zip).  NULL or "" defaults to "zork1".
 *
 * Returns 0 on a clean game exit (the story's QUIT, Ctrl-C), non-zero
 * on a load/runtime failure (already reported via cprintf).
 */
int zmachine_run(const char *name);

#endif /* ZMACHINE_ZFRONT_H */
