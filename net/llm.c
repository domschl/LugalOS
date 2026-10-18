#include "net/llm.h"
#include "net/http.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/palloc.h"
#include "libc/include/string.h"

/* accepted: the LLM client's configuration, read once and set by `llm`. */
static llm_config_t g_llm_cfg;
static bool         g_llm_inited = false;

static void json_append_escaped(char *dst, size_t *pos, size_t max, const char *src) {
    if (!dst || !pos || !src) return;
    while (*src && *pos + 4 < max) {
        char c = *src++;
        if (c == '"') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = '"';
        } else if (c == '\\') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = '\\';
        } else if (c == '\n') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'n';
        } else if (c == '\r') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'r';
        } else if (c == '\t') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 't';
        } else if ((unsigned char)c >= 32 || (unsigned char)c > 127) {
            dst[(*pos)++] = c;
        }
    }
    dst[*pos] = '\0';
}

static void parse_config_file(const char *path) {
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return;

    char line[128];
    uint32_t line_pos = 0;
    uint8_t buf[256];
    uint64_t offset = 0;

    for (;;) {
        int rd = vfs_pread(fd, buf, sizeof(buf), offset);
        if (rd <= 0) break;
        offset += (uint64_t)rd;

        for (int i = 0; i < rd; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_pos] = '\0';
                line_pos = 0;

                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\0') continue;

                char *eq = strchr(p, '=');
                if (!eq) continue;
                *eq = '\0';

                char *key = p;
                char *val = eq + 1;
                /* Trim key */
                char *k_end = eq - 1;
                while (k_end >= key && (*k_end == ' ' || *k_end == '\t')) *k_end-- = '\0';
                /* Trim val */
                while (*val == ' ' || *val == '\t') val++;
                char *v_end = val + strlen(val) - 1;
                while (v_end >= val && (*v_end == ' ' || *v_end == '\t')) *v_end-- = '\0';

                if (strcmp(key, "url") == 0) {
                    strncpy(g_llm_cfg.url, val, sizeof(g_llm_cfg.url) - 1);
                    g_llm_cfg.url[sizeof(g_llm_cfg.url) - 1] = '\0';
                } else if (strcmp(key, "model") == 0) {
                    strncpy(g_llm_cfg.model, val, sizeof(g_llm_cfg.model) - 1);
                    g_llm_cfg.model[sizeof(g_llm_cfg.model) - 1] = '\0';
                } else if (strcmp(key, "api_key") == 0 || strcmp(key, "key") == 0) {
                    strncpy(g_llm_cfg.api_key, val, sizeof(g_llm_cfg.api_key) - 1);
                    g_llm_cfg.api_key[sizeof(g_llm_cfg.api_key) - 1] = '\0';
                }
            } else if (line_pos < sizeof(line) - 1) {
                line[line_pos++] = c;
            }
        }
    }
    vfs_close(fd);
}

void llm_init(void) {
    if (g_llm_inited) return;

    memset(&g_llm_cfg, 0, sizeof(g_llm_cfg));
    strncpy(g_llm_cfg.url, LLM_DEFAULT_URL, sizeof(g_llm_cfg.url) - 1);
    strncpy(g_llm_cfg.model, LLM_DEFAULT_MODEL, sizeof(g_llm_cfg.model) - 1);
    g_llm_cfg.timeout_ms = LLM_DEFAULT_TIMEOUT_MS;

    /* Attempt loading from /sd0/etc/llm.conf then /flash0/etc/llm.conf */
    parse_config_file("/sd0/etc/llm.conf");
    parse_config_file("/flash0/etc/llm.conf");

    g_llm_inited = true;
}

bool llm_get_config(llm_config_t *cfg) {
    if (!cfg) return false;
    llm_init();
    *cfg = g_llm_cfg;
    return true;
}

bool llm_set_config(const llm_config_t *cfg) {
    if (!cfg) return false;
    llm_init();
    if (cfg->url[0]) {
        strncpy(g_llm_cfg.url, cfg->url, sizeof(g_llm_cfg.url) - 1);
        g_llm_cfg.url[sizeof(g_llm_cfg.url) - 1] = '\0';
    }
    if (cfg->model[0]) {
        strncpy(g_llm_cfg.model, cfg->model, sizeof(g_llm_cfg.model) - 1);
        g_llm_cfg.model[sizeof(g_llm_cfg.model) - 1] = '\0';
    }
    if (cfg->api_key[0]) {
        strncpy(g_llm_cfg.api_key, cfg->api_key, sizeof(g_llm_cfg.api_key) - 1);
        g_llm_cfg.api_key[sizeof(g_llm_cfg.api_key) - 1] = '\0';
    }
    if (cfg->timeout_ms > 0) {
        g_llm_cfg.timeout_ms = cfg->timeout_ms;
    }
    return true;
}

typedef struct {
    llm_token_cb on_token;
    void        *user_ctx;
    char         line_buf[1024];
    uint32_t     line_pos;
    bool         done;
} sse_ctx_t;

static int parse_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void unescape_json_string(const char *src, size_t src_len, char *dst, size_t dst_max) {
    size_t d = 0;
    for (size_t s = 0; s < src_len && d + 4 < dst_max; ) {
        if (src[s] == '\\' && s + 1 < src_len) {
            char next = src[s + 1];
            if (next == 'n') { dst[d++] = '\n'; s += 2; }
            else if (next == 't') { dst[d++] = '\t'; s += 2; }
            else if (next == 'r') { dst[d++] = '\r'; s += 2; }
            else if (next == '"') { dst[d++] = '"'; s += 2; }
            else if (next == '\\') { dst[d++] = '\\'; s += 2; }
            else if (next == '/') { dst[d++] = '/'; s += 2; }
            else if (next == 'u' && s + 5 < src_len) {
                /* Basic Unicode \uXXXX decoding */
                int h1 = parse_hex_digit(src[s + 2]);
                int h2 = parse_hex_digit(src[s + 3]);
                int h3 = parse_hex_digit(src[s + 4]);
                int h4 = parse_hex_digit(src[s + 5]);
                if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
                    uint32_t codepoint = (uint32_t)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
                    if (codepoint < 0x80) {
                        dst[d++] = (char)codepoint;
                    } else if (codepoint < 0x800) {
                        dst[d++] = (char)(0xC0 | (codepoint >> 6));
                        dst[d++] = (char)(0x80 | (codepoint & 0x3F));
                    } else {
                        dst[d++] = (char)(0xE0 | (codepoint >> 12));
                        dst[d++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        dst[d++] = (char)(0x80 | (codepoint & 0x3F));
                    }
                    s += 6;
                } else {
                    dst[d++] = src[s++];
                }
            } else {
                dst[d++] = src[s++];
            }
        } else {
            dst[d++] = src[s++];
        }
    }
    dst[d] = '\0';
}

static void process_sse_line(sse_ctx_t *ctx) {
    char *line = ctx->line_buf;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return;

    if (strncmp(line, "data:", 5) != 0) return;
    char *data = line + 5;
    while (*data == ' ') data++;

    if (strcmp(data, "[DONE]") == 0) {
        ctx->done = true;
        return;
    }

    /* Check for content in delta: "content":"..." or "reasoning_content":"..." */
    bool is_reasoning = false;
    const char *key = "\"content\":\"";
    char *match = strstr(data, key);
    if (!match) {
        key = "\"reasoning_content\":\"";
        match = strstr(data, key);
        if (match) is_reasoning = true;
    }

    if (match) {
        char *start = match + strlen(key);
        /* Find matching closing quote (unescaped) */
        char *end = start;
        while (*end) {
            if (*end == '"') {
                size_t bs_count = 0;
                char *p = end - 1;
                while (p >= start && *p == '\\') {
                    bs_count++;
                    p--;
                }
                if ((bs_count % 2) == 0) break;
            }
            end++;
        }
        if (*end == '"') {
            size_t token_len = (size_t)(end - start);
            char decoded[256];
            unescape_json_string(start, token_len, decoded, sizeof(decoded));
            if (decoded[0] && ctx->on_token) {
                ctx->on_token(decoded, is_reasoning, ctx->user_ctx);
            }
        }
    }
}

static int sse_chunk_cb(const uint8_t *data, uint32_t len, void *user_ctx) {
    sse_ctx_t *ctx = (sse_ctx_t *)user_ctx;
    if (!ctx || ctx->done) return 1;

    for (uint32_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            ctx->line_buf[ctx->line_pos] = '\0';
            process_sse_line(ctx);
            ctx->line_pos = 0;
            if (ctx->done) return 1;
        } else if (ctx->line_pos < sizeof(ctx->line_buf) - 1) {
            ctx->line_buf[ctx->line_pos++] = c;
        }
    }
    return 0;
}

int llm_parse_sse_response(const char *sse_text, llm_token_cb on_token, void *user_ctx) {
    if (!sse_text || !on_token) return -1;
    sse_ctx_t state;
    memset(&state, 0, sizeof(state));
    state.on_token = on_token;
    state.user_ctx = user_ctx;
    int rc = sse_chunk_cb((const uint8_t *)sse_text, (uint32_t)strlen(sse_text), &state);
    return (rc == 0 || state.done) ? 0 : rc;
}

int llm_stream_chat(const char *prompt, const char *system_prompt, const char *model_override,
                    llm_token_cb on_token, void *ctx,
                    char *err_out, size_t err_sz) {
    if (!prompt) {
        if (err_out && err_sz > 0) strncpy(err_out, "Empty prompt", err_sz - 1);
        return -1;
    }

    llm_init();

    const char *model = model_override ? model_override : g_llm_cfg.model;
    if (!model || !*model) model = LLM_DEFAULT_MODEL;

    /* Build endpoint URL */
    char full_url[160];
    size_t url_len = strlen(g_llm_cfg.url);
    if (url_len > 0 && g_llm_cfg.url[url_len - 1] == '/') {
        ksnprintf(full_url, sizeof(full_url), "%schat/completions", g_llm_cfg.url);
    } else {
        ksnprintf(full_url, sizeof(full_url), "%s/chat/completions", g_llm_cfg.url);
    }

    /* Build JSON request payload in PSRAM (bulk zone) with SRAM fallback */
    uint32_t body_pages = 16; /* 64 KB */
    size_t body_max = body_pages * 4096;
    char *body = (char *)palloc_pages_bulk(body_pages);
    if (!body) {
        body_pages = 4; /* 16 KB SRAM fallback */
        body_max = body_pages * 4096;
        body = (char *)palloc_pages(body_pages);
    }
    if (!body) {
        if (err_out && err_sz > 0) strncpy(err_out, "Out of memory for request body", err_sz - 1);
        return -1;
    }
    size_t bpos = 0;

    int n = ksnprintf(body, body_max, "{\"model\":\"%s\",\"stream\":true,\"temperature\":0.2,\"messages\":[", model);
    if (n > 0) bpos = (size_t)n;

    if (system_prompt && *system_prompt) {
        n = ksnprintf(body + bpos, body_max - bpos, "{\"role\":\"system\",\"content\":\"");
        if (n > 0) bpos += (size_t)n;
        json_append_escaped(body, &bpos, body_max - 64, system_prompt);
        n = ksnprintf(body + bpos, body_max - bpos, "\"},");
        if (n > 0) bpos += (size_t)n;
    }

    if (strstr(prompt, "[USER]") || strstr(prompt, "[ASSISTANT]")) {
        const char *cur = prompt;
        while (*cur) {
            bool is_user = true;
            if (strncmp(cur, "[USER]", 6) == 0) {
                is_user = true;
                cur += 6;
            } else if (strncmp(cur, "[ASSISTANT]", 11) == 0) {
                is_user = false;
                cur += 11;
            } else {
                cur++;
                continue;
            }
            while (*cur == ' ' || *cur == '\n' || *cur == '\r') cur++;
            const char *next_u = strstr(cur, "[USER]");
            const char *next_a = strstr(cur, "[ASSISTANT]");
            const char *msg_end = NULL;
            if (next_u && next_a) msg_end = next_u < next_a ? next_u : next_a;
            else if (next_u) msg_end = next_u;
            else if (next_a) msg_end = next_a;
            else msg_end = cur + strlen(cur);

            size_t mlen = (size_t)(msg_end - cur);
            while (mlen > 0 && (cur[mlen - 1] == ' ' || cur[mlen - 1] == '\n' || cur[mlen - 1] == '\r')) mlen--;

            if (mlen > 0 && bpos + 64 < body_max) {
                n = ksnprintf(body + bpos, body_max - bpos, "{\"role\":\"%s\",\"content\":\"",
                              is_user ? "user" : "assistant");
                if (n > 0) bpos += (size_t)n;
                char tmp[2048];
                size_t clen = mlen < sizeof(tmp) - 1 ? mlen : sizeof(tmp) - 1;
                memcpy(tmp, cur, clen);
                tmp[clen] = '\0';
                json_append_escaped(body, &bpos, body_max - 64, tmp);
                n = ksnprintf(body + bpos, body_max - bpos, "\"},");
                if (n > 0) bpos += (size_t)n;
            }
            cur = msg_end;
        }
        if (bpos > 0 && body[bpos - 1] == ',') bpos--;
        n = ksnprintf(body + bpos, body_max - bpos, "]}");
        if (n > 0) bpos += (size_t)n;
    } else {
        n = ksnprintf(body + bpos, body_max - bpos, "{\"role\":\"user\",\"content\":\"");
        if (n > 0) bpos += (size_t)n;
        json_append_escaped(body, &bpos, body_max - 16, prompt);
        n = ksnprintf(body + bpos, body_max - bpos, "\"}]}");
        if (n > 0) bpos += (size_t)n;
    }

    /* Auth header if configured */
    char auth_buf[96] = {0};
    const char *auth_hdr = NULL;
    if (g_llm_cfg.api_key[0] && strcmp(g_llm_cfg.api_key, "none") != 0) {
        ksnprintf(auth_buf, sizeof(auth_buf), "Bearer %s", g_llm_cfg.api_key);
        auth_hdr = auth_buf;
    }

    sse_ctx_t sse_state;
    memset(&sse_state, 0, sizeof(sse_state));
    sse_state.on_token = on_token;
    sse_state.user_ctx = ctx;

    http_req_t req;
    memset(&req, 0, sizeof(req));
    req.url = full_url;
    req.method = "POST";
    req.content_type = "application/json";
    req.auth_header = auth_hdr;
    req.body = body;
    req.body_len = (uint32_t)bpos;
    req.timeout_ms = g_llm_cfg.timeout_ms;

    int status = http_request(&req, sse_chunk_cb, &sse_state, err_out, err_sz);
    palloc_free(body, body_pages);
    if (status != 200) {
        if (err_out && err_sz > 0 && status > 0) {
            ksnprintf(err_out, err_sz, "LLM server returned HTTP %d", status);
        }
        return status;
    }

    return 0;
}

typedef struct {
    char        *out_buf;
    size_t       out_max;
    size_t       out_pos;
    llm_token_cb print_cb;
    void        *print_ctx;
} collect_ctx_t;

static void collect_token_cb(const char *token, bool is_reasoning, void *ctx) {
    collect_ctx_t *c = (collect_ctx_t *)ctx;
    if (!c) return;

    if (c->print_cb) {
        c->print_cb(token, is_reasoning, c->print_ctx);
    }

    if (!is_reasoning && c->out_buf && c->out_pos + 1 < c->out_max) {
        size_t len = strlen(token);
        size_t room = c->out_max - 1 - c->out_pos;
        size_t copy_len = len < room ? len : room;
        memcpy(c->out_buf + c->out_pos, token, copy_len);
        c->out_pos += copy_len;
        c->out_buf[c->out_pos] = '\0';
    }
}

int llm_chat(const char *prompt, const char *system_prompt, const char *model_override,
             char *out_buf, size_t out_max,
             llm_token_cb stream_print_cb, void *ctx,
             char *err_out, size_t err_sz) {
    if (out_buf && out_max > 0) out_buf[0] = '\0';

    collect_ctx_t collector;
    memset(&collector, 0, sizeof(collector));
    collector.out_buf = out_buf;
    collector.out_max = out_max;
    collector.print_cb = stream_print_cb;
    collector.print_ctx = ctx;

    return llm_stream_chat(prompt, system_prompt, model_override,
                           collect_token_cb, &collector,
                           err_out, err_sz);
}

typedef struct {
    char   *buf;
    size_t  max;
    size_t  pos;
} models_ctx_t;

static int models_chunk_cb(const uint8_t *data, uint32_t len, void *ctx) {
    models_ctx_t *m = (models_ctx_t *)ctx;
    if (!m || !m->buf) return 1;

    /* Scan for "id":"..." */
    const char *p = (const char *)data;
    const char *end = p + len;

    while (p < end) {
        const char *match = strstr(p, "\"id\":");
        if (!match) break;
        const char *q1 = strchr(match + 5, '"');
        if (!q1) break;
        q1++;
        const char *q2 = strchr(q1, '"');
        if (!q2) break;

        size_t id_len = (size_t)(q2 - q1);
        if (id_len > 0 && m->pos + id_len + 2 < m->max) {
            if (m->pos > 0) m->buf[m->pos++] = ' ';
            memcpy(m->buf + m->pos, q1, id_len);
            m->pos += id_len;
            m->buf[m->pos] = '\0';
        }
        p = q2 + 1;
    }
    return 0;
}

int llm_models(char *out_buf, size_t out_max, char *err_out, size_t err_sz) {
    if (!out_buf || out_max == 0) return -1;
    out_buf[0] = '\0';

    llm_init();

    char full_url[160];
    size_t url_len = strlen(g_llm_cfg.url);
    if (url_len > 0 && g_llm_cfg.url[url_len - 1] == '/') {
        ksnprintf(full_url, sizeof(full_url), "%smodels", g_llm_cfg.url);
    } else {
        ksnprintf(full_url, sizeof(full_url), "%s/models", g_llm_cfg.url);
    }

    char auth_buf[96] = {0};
    const char *auth_hdr = NULL;
    if (g_llm_cfg.api_key[0] && strcmp(g_llm_cfg.api_key, "none") != 0) {
        ksnprintf(auth_buf, sizeof(auth_buf), "Bearer %s", g_llm_cfg.api_key);
        auth_hdr = auth_buf;
    }

    models_ctx_t mctx;
    mctx.buf = out_buf;
    mctx.max = out_max;
    mctx.pos = 0;

    http_req_t req;
    memset(&req, 0, sizeof(req));
    req.url = full_url;
    req.method = "GET";
    req.auth_header = auth_hdr;
    req.timeout_ms = 10000;

    int status = http_request(&req, models_chunk_cb, &mctx, err_out, err_sz);
    if (status != 200) {
        if (err_out && err_sz > 0 && status > 0) {
            ksnprintf(err_out, err_sz, "Models endpoint returned HTTP %d", status);
        }
        return status;
    }
    return 0;
}
