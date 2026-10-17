#include "kernel/ea.h"
#include "kernel/console.h"
#include "kernel/shell.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/palloc.h"
#include "net/llm.h"
#include "fs/vfs.h"
#include "libc/include/string.h"
#include "user/lisp/include/lisp.h"

static const char EA_SYSTEM_PROMPT[] =
    "You are ea, the intelligent on-device AI assistant for LugalOS.\n"
    "You have tools to inspect the system, evaluate Lisp expressions, run shell commands, compile C programs, and read system manuals.\n\n"
    "Available Tools:\n"
    "1. Action: lisp\n"
    "```lisp\n"
    "<expression>\n"
    "```\n"
    "Evaluates expression in LugalOS kernel Lisp machine.\n"
    "CAS (Computer Algebra System) math primitives (preloaded):\n"
    "  (diff expr var [n])          - Symbolic n-th derivative. Example: (diff '(expt x 3) 'x) or (diff '(* x (sin x)) 'x)\n"
    "  (integrate expr var)         - Indefinite integral. Example: (integrate '(* x x) 'x)\n"
    "  (integrate-def expr var a b) - Definite integral. Example: (integrate-def '(* x x) 'x 0 1)\n"
    "  (solve '(eq) 'var)           - Symbolic equation solver. Example: (solve '(= (+ (* x x) -4) 0) 'x)\n"
    "  (simplify expr)              - Algebraic simplification. Example: (simplify '(+ (* 2 x) (* 3 x)))\n"
    "  (taylor expr var order [x0]) - Taylor series expansion\n"
    "  (limit expr var point [dir]) - Symbolic limits / limes\n\n"
    "2. Action: sh\n"
    "```sh\n"
    "<command>\n"
    "```\n"
    "Runs shell command (ls, cat, mem, ps, net, sensor, etc.).\n\n"
    "3. Action: cc\n"
    "```c\n"
    "<c-source>\n"
    "```\n"
    "Compiles C code with chibicc and runs the resulting binary.\n\n"
    "4. Action: help\n"
    "```text\n"
    "<topic>\n"
    "```\n"
    "Reads system manual (cas, lisp, c_programming, shell, sensors, etc.).\n\n"
    "Format rules:\n"
    "Thought: <reasoning>\n"
    "Action: <tool>\n"
    "```<lang>\n"
    "<input>\n"
    "```\n"
    "Stop immediately after the closing ``` and wait for Observation. Do not generate thoughts or answers before Observation is given.\n\n"
    "When done, provide:\n"
    "Thought: <final reasoning>\n"
    "Answer: <clear explanation and solution>";

static void strip_code_fences(char *dst, size_t dst_max, const char *src) {
    if (!dst || dst_max == 0 || !src) return;
    dst[0] = '\0';

    const char *p = src;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

    /* Skip leading ```lang */
    if (strncmp(p, "```", 3) == 0) {
        p += 3;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    size_t d = 0;
    while (*p && d + 1 < dst_max) {
        if (strncmp(p, "```", 3) == 0) break;
        dst[d++] = *p++;
    }
    dst[d] = '\0';

    /* Trim trailing whitespace */
    while (d > 0 && (dst[d - 1] == ' ' || dst[d - 1] == '\t' || dst[d - 1] == '\r' || dst[d - 1] == '\n')) {
        dst[--d] = '\0';
    }
}

int ea_eval_tool(const char *tool, const char *input, char *out_buf, size_t out_max) {
    if (!tool || !input || !out_buf || out_max == 0) return -1;
    out_buf[0] = '\0';

    char clean_code[1024];
    strip_code_fences(clean_code, sizeof(clean_code), input);

    if (strcmp(tool, "lisp") == 0) {
        static bool cas_loaded = false;
        if (!cas_loaded && (strstr(clean_code, "diff") || strstr(clean_code, "integrate") ||
                            strstr(clean_code, "solve") || strstr(clean_code, "simplify") ||
                            strstr(clean_code, "taylor") || strstr(clean_code, "limit"))) {
            vfs_stat_t st;
            lisp_lock();
            if (vfs_stat("/sd0/cas/cas.lisp", &st) == 0) {
                lisp_eval_string("(load \"/sd0/cas/cas.lisp\")");
            } else if (vfs_stat("/flash0/cas/cas.lisp", &st) == 0) {
                lisp_eval_string("(load \"/flash0/cas/cas.lisp\")");
            }
            lisp_unlock();
            cas_loaded = true;
        }

        console_capture(out_buf, (uint32_t)(out_max - 1));
        lisp_lock();
        lisp_val_t *res = lisp_eval_string(clean_code);
        if (res) {
            lisp_print(res);
        }
        lisp_unlock();
        uint32_t len = console_capture_end();
        if (len == 0 && out_buf[0] == '\0') {
            strncpy(out_buf, "nil", out_max - 1);
        }
        return 0;
    }

    if (strcmp(tool, "sh") == 0 || strcmp(tool, "shell") == 0) {
        console_capture(out_buf, (uint32_t)(out_max - 1));
        shell_exec(clean_code);
        uint32_t len = console_capture_end();
        if (len == 0 && out_buf[0] == '\0') {
            strncpy(out_buf, "(ok)", out_max - 1);
        }
        return 0;
    }

    if (strcmp(tool, "help") == 0 || strcmp(tool, "man") == 0) {
        const char *topic = clean_code;
        if (strcmp(topic, "calculus") == 0 || strcmp(topic, "math") == 0 ||
            strcmp(topic, "algebra") == 0 || strcmp(topic, "solve") == 0 ||
            strcmp(topic, "diff") == 0 || strcmp(topic, "integrate") == 0) {
            topic = "cas";
        } else if (strcmp(topic, "c") == 0 || strcmp(topic, "chibicc") == 0) {
            topic = "c_programming";
        } else if (strcmp(topic, "wifi") == 0 || strcmp(topic, "ip") == 0 ||
                   strcmp(topic, "dhcp") == 0 || strcmp(topic, "network") == 0) {
            topic = "networking";
        }

        char path[128];
        ksnprintf(path, sizeof(path), "/sd0/help/%s.gmi", topic);
        int fd = vfs_open(path, VFS_O_READ);
        if (fd < 0) {
            ksnprintf(path, sizeof(path), "/flash0/help/%s.gmi", topic);
            fd = vfs_open(path, VFS_O_READ);
        }
        if (fd >= 0) {
            int rd = vfs_pread(fd, (uint8_t *)out_buf, (uint32_t)(out_max - 1), 0);
            vfs_close(fd);
            if (rd > 0) out_buf[rd] = '\0';
            else strncpy(out_buf, "Empty help file", out_max - 1);
        } else {
            ksnprintf(out_buf, out_max, "Help topic '%s' not found", topic);
        }
        return 0;
    }

    if (strcmp(tool, "cc") == 0) {
        int fd = vfs_open("/sd0/tmp/ea_prog.c", VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
        if (fd >= 0) {
            vfs_pwrite(fd, (const uint8_t *)clean_code, (uint32_t)strlen(clean_code), 0);
            vfs_close(fd);

            console_capture(out_buf, (uint32_t)(out_max - 1));
            shell_exec("cc /sd0/tmp/ea_prog.c /sd0/bin/ea_prog");
            uint32_t clen = console_capture_end();

            /* Check if binary was generated */
            int bfd = vfs_open("/sd0/bin/ea_prog", VFS_O_READ);
            if (bfd >= 0) {
                vfs_close(bfd);
                /* Execute binary and capture stdout */
                console_capture(out_buf, (uint32_t)(out_max - 1));
                shell_exec("/sd0/bin/ea_prog");
                console_capture_end();
            } else if (clen == 0) {
                strncpy(out_buf, "Compilation failed", out_max - 1);
            }
        } else {
            strncpy(out_buf, "Cannot write /sd0/tmp/ea_prog.c", out_max - 1);
        }
        return 0;
    }

    ksnprintf(out_buf, out_max, "Unknown tool '%s'", tool);
    return -1;
}

static bool g_in_thought = false;

static void live_stream_token_cb(const char *token, bool is_reasoning, void *ctx) {
    (void)ctx;
    if (is_reasoning) {
        if (!g_in_thought) {
            cprintf("\033[2m[Thought: ");
            g_in_thought = true;
        }
        cprintf("%s", token);
    } else {
        if (g_in_thought) {
            cprintf("]\033[0m\n");
            g_in_thought = false;
        }
        cprintf("%s", token);
    }
}

static bool parse_react_action(char *resp, char *tool_out, size_t tool_sz,
                               char *code_out, size_t code_sz) {
    if (!resp || !tool_out || !code_out) return false;

    char *p = strstr(resp, "Action:");
    if (!p) return false;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;

    char *t_end = p;
    while (*t_end && *t_end != '\r' && *t_end != '\n' && *t_end != ' ') t_end++;

    size_t t_len = (size_t)(t_end - p);
    if (t_len == 0 || t_len >= tool_sz) return false;
    memcpy(tool_out, p, t_len);
    tool_out[t_len] = '\0';

    /* Find start of code block ``` */
    char *fence = strstr(t_end, "```");
    if (!fence) return false;
    fence += 3;
    /* Skip lang tag until newline */
    while (*fence && *fence != '\n') fence++;
    if (*fence == '\n') fence++;

    /* Find closing ``` */
    char *fence_end = strstr(fence, "```");
    if (!fence_end) {
        fence_end = fence + strlen(fence);
    }

    size_t c_len = (size_t)(fence_end - fence);
    if (c_len >= code_sz) c_len = code_sz - 1;
    memcpy(code_out, fence, c_len);
    code_out[c_len] = '\0';

    /* If closing fence exists, truncate resp right after it to keep turn history clean */
    if (strncmp(fence_end, "```", 3) == 0) {
        char *trunc = fence_end + 3;
        if (*trunc == '\r') trunc++;
        if (*trunc == '\n') trunc++;
        *trunc = '\0';
    }

    return true;
}

int ea_run(const char *prompt, bool interactive, bool auto_confirm, const char *model_override) {
    (void)interactive; (void)auto_confirm;
    if (!prompt || !*prompt) return -1;

    llm_init();

    char *history = (char *)palloc_pages(4); /* 16 KB conversation buffer */
    char *resp = (char *)palloc_pages(4);    /* 16 KB response buffer */
    char *tool_obs = (char *)palloc_pages(2);/* 8 KB tool observation buffer */

    if (!history || !resp || !tool_obs) {
        if (history) palloc_free(history, 4);
        if (resp) palloc_free(resp, 4);
        if (tool_obs) palloc_free(tool_obs, 2);
        cprintf("ea: out of memory for conversation buffers\n");
        return -2;
    }

    ksnprintf(history, 16384, "[USER]\n%s", prompt);

    int turns = 0;
    int status = 0;

    cprintf("\n\033[1m[ea: Thinking...]\033[0m\n");

    while (turns < 8) {
        turns++;
        g_in_thought = false;

        char err_buf[128] = {0};
        int rc = llm_chat(history, EA_SYSTEM_PROMPT, model_override,
                          resp, 16384, live_stream_token_cb, NULL,
                          err_buf, sizeof(err_buf));

        if (g_in_thought) {
            cprintf("]\033[0m\n");
            g_in_thought = false;
        }

        if (rc != 0) {
            cprintf("\nea: generation failed: %s\n", err_buf[0] ? err_buf : "unknown error");
            status = rc;
            break;
        }

        /* Check for Action: */
        char tool[32];
        char code[1024];
        if (parse_react_action(resp, tool, sizeof(tool), code, sizeof(code))) {
            cprintf("\n\033[1;34m--> [Executing Tool: %s]\033[0m\n", tool);
            ea_eval_tool(tool, code, tool_obs, 8192);
            cprintf("\033[36m<-- [Observation: %s]\033[0m\n\n", tool_obs);

            /* Append assistant turn and tool observation to history */
            size_t hlen = strlen(history);
            int n = ksnprintf(history + hlen, 16384 - hlen,
                              "\n\n[ASSISTANT]\n%s\n\n[USER]\nObservation:\n%s", resp, tool_obs);
            if (n < 0 || hlen + (size_t)n >= 16384 - 256) {
                cprintf("ea: conversation buffer full\n");
                break;
            }
        } else {
            /* No further action requested; model provided final answer */
            break;
        }
    }

    palloc_free(history, 4);
    palloc_free(resp, 4);
    palloc_free(tool_obs, 2);
    return status;
}

void cmd_ea(const char *args) {
    if (!args || !*args) {
        cprintf("Usage: ea [-m model] [-s url] [-y] \"<prompt>\"\n");
        cprintf("       ea -c / ea config       Display current LLM configuration\n");
        cprintf("       ea -l / ea models       List available models from server\n");
        return;
    }

    while (*args == ' ') args++;

    if (strcmp(args, "-c") == 0 || strcmp(args, "config") == 0) {
        llm_config_t cfg;
        llm_get_config(&cfg);
        cprintf("LLM Endpoint Configuration:\n");
        cprintf("  URL:     %s\n", cfg.url);
        cprintf("  Model:   %s\n", cfg.model);
        cprintf("  Timeout: %u ms\n", (unsigned)cfg.timeout_ms);
        return;
    }

    if (strcmp(args, "-l") == 0 || strcmp(args, "models") == 0) {
        char models[512];
        char err[128] = {0};
        cprintf("Querying available models from server...\n");
        if (llm_models(models, sizeof(models), err, sizeof(err)) == 0) {
            cprintf("Available models:\n  %s\n", models);
        } else {
            cprintf("Error fetching models: %s\n", err);
        }
        return;
    }

    const char *model = NULL;
    const char *url = NULL;
    bool auto_confirm = false;
    const char *p = args;

    while (*p == '-') {
        if (strncmp(p, "-m", 2) == 0 && (p[2] == ' ' || p[2] == '=' || p[2] == '"' || p[2] == '\'')) {
            p += 2;
            while (*p == ' ' || *p == '=') p++;
            char quote = 0;
            if (*p == '"' || *p == '\'') quote = *p++;
            const char *m_start = p;
            if (quote) {
                while (*p && *p != quote) p++;
            } else {
                while (*p && *p != ' ') p++;
            }
            size_t m_len = (size_t)(p - m_start);
            static char m_buf[64];
            if (m_len < sizeof(m_buf)) {
                memcpy(m_buf, m_start, m_len);
                m_buf[m_len] = '\0';
                model = m_buf;
            }
            if (quote && *p == quote) p++;
            while (*p == ' ') p++;
        } else if (strncmp(p, "-s", 2) == 0 && (p[2] == ' ' || p[2] == '=' || p[2] == '"' || p[2] == '\'')) {
            p += 2;
            while (*p == ' ' || *p == '=') p++;
            char quote = 0;
            if (*p == '"' || *p == '\'') quote = *p++;
            const char *s_start = p;
            if (quote) {
                while (*p && *p != quote) p++;
            } else {
                while (*p && *p != ' ') p++;
            }
            size_t s_len = (size_t)(p - s_start);
            static char s_buf[128];
            if (s_len < sizeof(s_buf)) {
                memcpy(s_buf, s_start, s_len);
                s_buf[s_len] = '\0';
                url = s_buf;
            }
            if (quote && *p == quote) p++;
            while (*p == ' ') p++;
        } else if (strncmp(p, "-y", 2) == 0) {
            auto_confirm = true;
            p += 2;
            while (*p == ' ') p++;
        } else {
            break;
        }
    }

    if (url || model) {
        llm_config_t cfg;
        llm_get_config(&cfg);
        if (url) {
            strncpy(cfg.url, url, sizeof(cfg.url) - 1);
            cfg.url[sizeof(cfg.url) - 1] = '\0';
        }
        if (model) {
            strncpy(cfg.model, model, sizeof(cfg.model) - 1);
            cfg.model[sizeof(cfg.model) - 1] = '\0';
        }
        llm_set_config(&cfg);
    }

    /* Strip leading quotes if present */
    while (*p == ' ' || *p == '"' || *p == '\'') p++;
    char clean_prompt[512];
    strncpy(clean_prompt, p, sizeof(clean_prompt) - 1);
    clean_prompt[sizeof(clean_prompt) - 1] = '\0';
    size_t len = strlen(clean_prompt);
    while (len > 0 && (clean_prompt[len - 1] == '"' || clean_prompt[len - 1] == '\'' || clean_prompt[len - 1] == ' ')) {
        clean_prompt[--len] = '\0';
    }

    if (clean_prompt[0] == '\0') {
        cprintf("ea: prompt cannot be empty\n");
        return;
    }

    ea_run(clean_prompt, false, auto_confirm, model);
}
