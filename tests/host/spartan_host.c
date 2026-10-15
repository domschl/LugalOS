/* tests/host/spartan_host.c — Host test harness for Spartan protocol.
 *
 * Verifies:
 * 1. Spartan URL parser (scheme, host, optional port, default port 300, path)
 * 2. Path sandboxing & chroot discriminator (traversal rejection, safe jail to /sd0/pub)
 * 3. Spartan request processing & response generation (status 2, status 40 error headers)
 *
 * Runs under AddressSanitizer, UndefinedBehaviorSanitizer, and Valgrind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "net/spartan.h"
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

static void test_url_parser(void) {
    TEST("spartan_parse_url()");
    char host[64];
    char path[128];
    uint16_t port = 0;

    /* Standard URL with path */
    assert(spartan_parse_url("spartan://192.168.1.10/doc/guide.gmi", host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "192.168.1.10") == 0);
    assert(port == 300);
    assert(strcmp(path, "/doc/guide.gmi") == 0);

    /* Custom port */
    assert(spartan_parse_url("spartan://192.168.1.50:3000/capsule/index.gmi", host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "192.168.1.50") == 0);
    assert(port == 3000);
    assert(strcmp(path, "/capsule/index.gmi") == 0);

    /* Bare host without trailing slash -> port 300, path "/" */
    assert(spartan_parse_url("spartan://10.0.0.1", host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "10.0.0.1") == 0);
    assert(port == 300);
    assert(strcmp(path, "/") == 0);

    /* Host with custom port and no path */
    assert(spartan_parse_url("spartan://10.0.0.1:8080", host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "10.0.0.1") == 0);
    assert(port == 8080);
    assert(strcmp(path, "/") == 0);

    /* Case-insensitive scheme */
    assert(spartan_parse_url("SPARTAN://192.168.1.100/test.gmi", host, sizeof(host), &port, path, sizeof(path)));
    assert(strcmp(host, "192.168.1.100") == 0);
    assert(port == 300);
    assert(strcmp(path, "/test.gmi") == 0);

    /* Negative checks */
    assert(!spartan_parse_url("http://192.168.1.1/", host, sizeof(host), &port, path, sizeof(path)));
    assert(!spartan_parse_url("spartan://", host, sizeof(host), &port, path, sizeof(path)));
    assert(!spartan_parse_url("spartan://:300/", host, sizeof(host), &port, path, sizeof(path)));
    assert(!spartan_parse_url("spartan://10.0.0.1:99999/", host, sizeof(host), &port, path, sizeof(path)));
    assert(!spartan_parse_url("spartan://10.0.0.1:abc/", host, sizeof(host), &port, path, sizeof(path)));

    PASS();
}

static void test_sandbox_path_validation(void) {
    TEST("spartan_sandbox_validate_path()");
    char safe[160];

    /* Root request maps to /sd0/pub/index.gmi */
    assert(spartan_sandbox_validate_path("/", safe, sizeof(safe)));
    assert(strcmp(safe, "/sd0/pub/index.gmi") == 0);

    /* Subpaths jail to /sd0/pub */
    assert(spartan_sandbox_validate_path("/help/shell.gmi", safe, sizeof(safe)));
    assert(strcmp(safe, "/sd0/pub/help/shell.gmi") == 0);

    assert(spartan_sandbox_validate_path("/books/iliad.gmi", safe, sizeof(safe)));
    assert(strcmp(safe, "/sd0/pub/books/iliad.gmi") == 0);

    /* Traversal attempts MUST be rejected */
    assert(!spartan_sandbox_validate_path("/../system/identity.key", safe, sizeof(safe)));
    assert(!spartan_sandbox_validate_path("/..", safe, sizeof(safe)));
    assert(!spartan_sandbox_validate_path("/foo/../../etc/passwd", safe, sizeof(safe)));
    assert(!spartan_sandbox_validate_path("/pub/../private", safe, sizeof(safe)));
    assert(!spartan_sandbox_validate_path("/foo/..", safe, sizeof(safe)));

    /* Relative or malformed paths rejected */
    assert(!spartan_sandbox_validate_path("relative/path.gmi", safe, sizeof(safe)));
    assert(!spartan_sandbox_validate_path("\\windows\\traversal", safe, sizeof(safe)));

    PASS();
}

static void test_server_request_processing(void) {
    TEST("spartan_process_request()");
    host_files_clear();

    /* Put a mock file in the in-memory host VFS at /sd0/pub/test.gmi */
    const char *mock_content = "# Hello Spartan\n\nThis is a test capsule.\n";
    host_file_put("/sd0/pub/test.gmi", mock_content, strlen(mock_content));

    uint8_t rx[256];
    uint8_t tx[4096];
    uint32_t consumed = 0;
    uint32_t tx_len = 0;
    bool close_after = false;

    /* 1. Incomplete line (no \n yet) -> must wait (-1) */
    strcpy((char *)rx, "localhost /test.gmi 0");
    assert(spartan_process_request(rx, strlen((char *)rx), &consumed, tx, sizeof(tx), &tx_len, &close_after) == -1);

    /* 2. Valid request -> status 2 and body */
    strcpy((char *)rx, "localhost /test.gmi 0\r\n");
    assert(spartan_process_request(rx, strlen((char *)rx), &consumed, tx, sizeof(tx), &tx_len, &close_after) == 0);
    assert(consumed == strlen("localhost /test.gmi 0\r\n"));
    assert(close_after == true);
    assert(tx_len > 0);
    tx[tx_len] = '\0';
    assert(strncmp((char *)tx, "2 text/gemini", 13) == 0);
    assert(strstr((char *)tx, "# Hello Spartan") != NULL);

    /* 3. Nonexistent file -> status 40 Not Found */
    strcpy((char *)rx, "localhost /nonexistent.gmi 0\r\n");
    assert(spartan_process_request(rx, strlen((char *)rx), &consumed, tx, sizeof(tx), &tx_len, &close_after) == 0);
    tx[tx_len] = '\0';
    assert(strncmp((char *)tx, "40 File Not Found", 17) == 0);

    /* 4. Directory traversal attempt -> status 40 Access Denied */
    strcpy((char *)rx, "localhost /../system/identity.key 0\r\n");
    assert(spartan_process_request(rx, strlen((char *)rx), &consumed, tx, sizeof(tx), &tx_len, &close_after) == 0);
    tx[tx_len] = '\0';
    assert(strncmp((char *)tx, "40 Access Denied", 16) == 0);

    /* 5. Malformed request (missing path) -> status 40 Bad Request */
    strcpy((char *)rx, "localhost_only_no_path\r\n");
    assert(spartan_process_request(rx, strlen((char *)rx), &consumed, tx, sizeof(tx), &tx_len, &close_after) == 0);
    tx[tx_len] = '\0';
    assert(strncmp((char *)tx, "40 Bad Request", 14) == 0);

    PASS();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("=== LugalOS Host Test: Spartan Protocol & Sandbox ===\n");
    test_url_parser();
    test_sandbox_path_validation();
    test_server_request_processing();
    printf("\nAll %d/%d tests passed successfully!\n", g_tests_passed, g_tests_run);
    return 0;
}
