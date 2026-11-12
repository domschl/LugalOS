#ifndef NET_SSH_H
#define NET_SSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(LUGALOS_HOST_TEST)
#include "lugalos_config.h"
#endif

/*
 * Freestanding SSH-2 client (RFC 4251-4254, curve25519-sha256,
 * chacha20-poly1305@openssh.com, ssh-ed25519 / ecdsa-sha2-nistp256).
 *
 * Transport-independent: caller provides non-blocking recv/send callbacks,
 * or uses the built-in TCP wrapper ssh_connect_tcp(). Session state is
 * dynamically allocated from PSRAM/bulk pages when driven on-device so
 * non-PSRAM microcontrollers incur zero static SRAM overhead.
 */

#define SSH_DEFAULT_PORT 22u

#define SSH_OK               0
#define SSH_EOF             -1
#define SSH_ERR_INVAL       -2
#define SSH_ERR_NOMEM       -3
#define SSH_ERR_NET         -4
#define SSH_ERR_PROTO       -5
#define SSH_ERR_CRYPTO      -6
#define SSH_ERR_HOSTKEY     -7
#define SSH_ERR_AUTH        -8
#define SSH_ERR_CHANNEL     -9
#define SSH_ERR_TIMEOUT     -10

typedef int (*ssh_recv_fn)(void *ctx, uint8_t *buf, uint32_t len);
typedef int (*ssh_send_fn)(void *ctx, const uint8_t *buf, uint32_t len);

/* Optional host-key verification callback.
 * `key_type` is e.g. "ssh-ed25519", `blob` is the raw SSH host key blob.
 * Return true to accept the host key, false to abort connection. */
typedef bool (*ssh_hostkey_cb)(void *ctx, const char *host, uint16_t port,
                               const char *key_type,
                               const uint8_t *blob, size_t blob_len);

/* Optional password prompt callback (used if publickey auth is unavailable/rejected). */
typedef bool (*ssh_password_cb)(void *ctx, const char *user, const char *host,
                                char *pw_out, size_t pw_cap);

typedef struct {
    const char      *host;
    uint16_t         port;         /* 0 defaults to 22 */
    const char      *user;
    const char      *password;     /* optional static password (NULL -> prompt_cb) */
    const uint8_t   *ed25519_seed; /* optional 32-byte Ed25519 private seed */
    uint32_t         timeout_ms;   /* 0 defaults to 15000 ms */
    ssh_hostkey_cb   hostkey_cb;   /* NULL accepts and checks known_hosts if wired */
    void            *hostkey_ctx;
    ssh_password_cb  password_cb;
    void            *password_ctx;
} ssh_cfg_t;

typedef struct ssh_session ssh_session_t;

/* Opens a transport-agnostic SSH-2 session over custom I/O callbacks.
 * Allocates session state dynamically; caller must free via ssh_close(). */
int ssh_open_io(ssh_session_t **out, const ssh_cfg_t *cfg,
                ssh_recv_fn recv_fn, ssh_send_fn send_fn, void *io_ctx);

/* Connects over LugalOS TCP + DNS and performs SSH-2 handshake + userauth. */
int ssh_connect_tcp(ssh_session_t **out, const ssh_cfg_t *cfg,
                    char *err_out, size_t err_sz);

/* Opens an interactive shell channel with PTY ("xterm-256color"). */
int ssh_open_shell(ssh_session_t *s, uint32_t cols, uint32_t rows);

/* Opens an exec channel running `command` (e.g. "git-upload-pack '/repo'"). */
int ssh_open_exec(ssh_session_t *s, const char *command);

/* Sends stdin bytes on the open channel. Returns bytes queued/sent, 0 if window
 * or TX busy, or negative SSH_ERR_*. */
int ssh_channel_write(ssh_session_t *s, const uint8_t *buf, uint32_t len);

/* Polls/reads stdout (or stderr into `is_stderr`) from the open channel.
 * Returns >0 byte count, 0 if no data ready yet, SSH_EOF on channel EOF/close,
 * or negative SSH_ERR_*. */
int ssh_channel_read(ssh_session_t *s, uint8_t *buf, uint32_t cap, bool *is_stderr);

/* Sends a window-change request for an active PTY channel. */
int ssh_channel_window_change(ssh_session_t *s, uint32_t cols, uint32_t rows);

/* Sends SSH_MSG_CHANNEL_EOF on the active channel. */
int ssh_channel_send_eof(ssh_session_t *s);

/* Returns exit status if received (-1 if not yet received). */
int ssh_channel_exit_status(const ssh_session_t *s);

/* Closes channel/transport, wipes secrets, and frees session memory. */
void ssh_close(ssh_session_t *s);

/* --- Keygen & known_hosts helpers ------------------------------------------- */

/* Formats an Ed25519 public key as "ssh-ed25519 <base64> <comment>\n". */
int ssh_format_ed25519_pub(const uint8_t pub[32], const char *comment,
                           char *out, size_t cap);

/* Formats a SHA-256 fingerprint "SHA256:<base64>" of an SSH key blob. */
int ssh_key_fingerprint_sha256(const uint8_t *blob, size_t blob_len,
                               char *out, size_t cap);

/* Encodes raw Ed25519 pubkey into SSH wire blob ("ssh-ed25519" + 32-byte key). */
size_t ssh_encode_ed25519_blob(const uint8_t pub[32], uint8_t *out, size_t cap);

#if defined(LUGALOS_HOST_TEST) || defined(CONFIG_SSH_CLIENT)
/* Interactive shell command entrypoints: `ssh` and `ssh-keygen`. */
void cmd_ssh(const char *args);
void cmd_ssh_keygen(const char *args);
#endif

#endif /* NET_SSH_H */
