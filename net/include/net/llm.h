#ifndef NET_LLM_H
#define NET_LLM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define LLM_DEFAULT_URL "http://ai-server.local:8080/v1"
#define LLM_DEFAULT_MODEL "default"
#define LLM_DEFAULT_TIMEOUT_MS 120000

typedef struct llm_config {
    char     url[128];
    char     model[64];
    char     api_key[64];
    uint32_t timeout_ms;
} llm_config_t;

/* Callback for streamed tokens.
 * is_reasoning: true for thinking tokens (reasoning_content), false for answer (content) */
typedef void (*llm_token_cb)(const char *token, bool is_reasoning, void *ctx);

void llm_init(void);
bool llm_get_config(llm_config_t *cfg);
bool llm_set_config(const llm_config_t *cfg);

/* Streams tokens from /v1/chat/completions using Server-Sent Events (SSE). */
int llm_stream_chat(const char *prompt, const char *system_prompt, const char *model_override,
                    llm_token_cb on_token, void *ctx,
                    char *err_out, size_t err_sz);

/* Parses a raw Server-Sent Events stream chunk or buffer and dispatches decoded tokens. */
int llm_parse_sse_response(const char *sse_text, llm_token_cb on_token, void *user_ctx);

/* Synchronous convenience: collects text answer into out_buf.
 * If stream_print_cb is non-NULL, tokens are also printed/forwarded live during generation. */
int llm_chat(const char *prompt, const char *system_prompt, const char *model_override,
             char *out_buf, size_t out_max,
             llm_token_cb stream_print_cb, void *ctx,
             char *err_out, size_t err_sz);

/* Queries /v1/models and formats available models into out_buf. */
int llm_models(char *out_buf, size_t out_max, char *err_out, size_t err_sz);

#endif /* NET_LLM_H */
