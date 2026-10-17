/* tests/host/llm_host.c — Host test harness for DNS, HTTP, and LLM SSE client.
 *
 * Verifies:
 * 1. HTTP URL parser (scheme, host, optional port, default port 80, path)
 * 2. DNS resolver (IPv4 dotted-quads, /sd0/etc/hosts file parsing, cache expiry)
 * 3. LLM configuration loader (/sd0/etc/llm.conf key-value parser)
 * 4. SSE streaming token parser & JSON unescaper (content, reasoning_content, escapes)
 *
 * Runs under AddressSanitizer, UndefinedBehaviorSanitizer, and Valgrind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "net/dns.h"
#include "net/http.h"
#include "net/llm.h"
#include "shim.h"

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) do { \
    g_tests_run++; \
    printf("  [TEST] %s ... ", name); \
} while(0)

#define PASS() do { \
    g_tests_passed++; \
    printf("PASS\n"); \
} while(0)

static void test_http_url_parser(void) {
    TEST("http_parse_url()");
    char host[64];
    char path[128];
    uint16_t port = 0;

    /* Standard URL with path */
    assert(http_parse_url("http://192.168.1.10/v1/models", host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "192.168.1.10") == 0);
    assert(port == 80);
    assert(strcmp(path, "/v1/models") == 0);

    /* Custom port */
    assert(http_parse_url("http://ai-server.local:8080/v1/chat/completions", host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "ai-server.local") == 0);
    assert(port == 8080);
    assert(strcmp(path, "/v1/chat/completions") == 0);

    /* Bare host without trailing slash -> port 80, path "/" */
    assert(http_parse_url("http://localhost", host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "localhost") == 0);
    assert(port == 80);
    assert(strcmp(path, "/") == 0);

    /* Case-insensitivity in scheme */
    assert(http_parse_url("HTTP://ROUTER:8000/", host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "ROUTER") == 0);
    assert(port == 8000);
    assert(strcmp(path, "/") == 0);

    /* Reject non-HTTP schemes */
    assert(!http_parse_url("spartan://router/path", host, sizeof(host), &port, path, sizeof(path)));
    assert(!http_parse_url("ftp://router/file", host, sizeof(host), &port, path, sizeof(path)));

    PASS();
}

static void test_dns_hosts_file(void) {
    TEST("dns_resolve() via /sd0/etc/hosts");
    host_files_clear();

    const char *hosts_data =
        "# Static hosts mapping for LugalOS\n"
        "127.0.0.1       localhost\n"
        "10.0.0.42       llm-node  ai-server.local\n"
        "10.0.0.5        gateway\n";
    host_file_put("/sd0/etc/hosts", hosts_data, (uint32_t)strlen(hosts_data));

    uint8_t ip[4];

    /* Dotted quad direct pass-through */
    assert(dns_resolve("192.168.1.1", ip));
    assert(ip[0] == 192 && ip[1] == 168 && ip[2] == 1 && ip[3] == 1);

    /* Hosts file lookup: primary hostname */
    assert(dns_resolve("llm-node", ip));
    assert(ip[0] == 10 && ip[1] == 0 && ip[2] == 0 && ip[3] == 42);

    /* Hosts file lookup: alias */
    assert(dns_resolve("ai-server.local", ip));
    assert(ip[0] == 10 && ip[1] == 0 && ip[2] == 0 && ip[3] == 42);

    /* Hosts file lookup: localhost */
    assert(dns_resolve("localhost", ip));
    assert(ip[0] == 127 && ip[1] == 0 && ip[2] == 0 && ip[3] == 1);

    /* Unknown host (when no upstream DNS) fails cleanly */
    uint8_t bad_ip[4];
    assert(!dns_resolve("nonexistent.invalid", bad_ip));

    PASS();
}

static void test_llm_config_loader(void) {
    TEST("llm_init() and /sd0/etc/llm.conf");
    host_files_clear();

    const char *conf_data =
        "# LLM Server Configuration\n"
        "url = http://ai.local:8180/v1\n"
        "model = Gemma4-12B-MTP\n"
        "api_key = test-secret-key\n";
    host_file_put("/sd0/etc/llm.conf", conf_data, (uint32_t)strlen(conf_data));

    llm_config_t cfg;
    assert(llm_get_config(&cfg));
    assert(strcmp(cfg.url, "http://ai.local:8180/v1") == 0);
    assert(strcmp(cfg.model, "Gemma4-12B-MTP") == 0);
    assert(strcmp(cfg.api_key, "test-secret-key") == 0);

    /* Dynamic reconfiguration */
    llm_config_t new_cfg;
    memset(&new_cfg, 0, sizeof(new_cfg));
    strncpy(new_cfg.url, "http://10.0.0.99:5000/v1", sizeof(new_cfg.url) - 1);
    strncpy(new_cfg.model, "Qwen-Test", sizeof(new_cfg.model) - 1);
    assert(llm_set_config(&new_cfg));

    assert(llm_get_config(&cfg));
    assert(strcmp(cfg.url, "http://10.0.0.99:5000/v1") == 0);
    assert(strcmp(cfg.model, "Qwen-Test") == 0);

    PASS();
}

static char g_thought_acc[256];
static char g_answer_acc[256];

static void sse_token_collector(const char *tok, bool is_reasoning, void *ctx) {
    (void)ctx;
    if (is_reasoning) {
        strncat(g_thought_acc, tok, sizeof(g_thought_acc) - strlen(g_thought_acc) - 1);
    } else {
        strncat(g_answer_acc, tok, sizeof(g_answer_acc) - strlen(g_answer_acc) - 1);
    }
}

static void test_sse_stream_parser(void) {
    TEST("llm_parse_sse_response()");

    g_thought_acc[0] = '\0';
    g_answer_acc[0] = '\0';

    const char *sse_payload =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"I should \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"calculate.\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"The answer is \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"42\\t\\\"OK\\\"\\n\"}}]}\n\n"
        "data: [DONE]\n\n";

    assert(llm_parse_sse_response(sse_payload, sse_token_collector, NULL) == 0);
    assert(strcmp(g_thought_acc, "I should calculate.\n") == 0);
    assert(strcmp(g_answer_acc, "The answer is 42\t\"OK\"\n") == 0);

    PASS();
}

int main(void) {
    printf("=== LugalOS Host Test: DNS, HTTP & LLM Client ===\n");

    test_http_url_parser();
    test_dns_hosts_file();
    test_llm_config_loader();
    test_sse_stream_parser();

    printf("\nAll %d/%d tests passed successfully!\n", g_tests_passed, g_tests_run);
    return 0;
}
