/*
 * LugalOS SSH shell commands (`ssh`, `ssh-keygen`) and VFS keystore
 * (`/.ssh/id_ed25519`, `/.ssh/known_hosts`).
 */

#include "net/ssh.h"

#if !defined(LUGALOS_HOST_TEST) && defined(CONFIG_SSH_CLIENT)

#include "kernel/crypto.h"
#include "kernel/random.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/keyseq.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "fs/vfs.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *ssh_base_dir(void) {
    if (vfs_volume_mounted("nvme0")) return "/nvme0/.ssh";
    return "/sd0/.ssh";
}

static void ensure_ssh_dir(void) {
    const char *dir = ssh_base_dir();
    vfs_mkdir(dir);
}

static int parse_args(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            argv[argc++] = p;
            while (*p && *p != q) p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

static bool read_line_prompt(const char *prompt, char *out, size_t cap, bool echo) {
    cprintf("%s", prompt);
    size_t pos = 0;
    for (;;) {
        int ch = console_getc();
        if (ch < 0 || ch == 3) {
            cprintf("\n");
            return false;
        }
        if (ch == '\r' || ch == '\n') {
            cprintf("\n");
            out[pos] = '\0';
            return true;
        }
        if ((ch == '\b' || ch == 0x7f) && pos > 0) {
            pos--;
            if (echo) cprintf("\b \b");
            continue;
        }
        if (ch >= 0x20 && ch < 0x7f && pos + 1u < cap) {
            out[pos++] = (char)ch;
            if (echo) cprintf("%c", ch);
        }
    }
}

struct kh_ctx {
    bool auto_accept;
};

static bool known_hosts_cb(void *ctx, const char *host, uint16_t port,
                           const char *key_type, const uint8_t *blob, size_t blob_len) {
    struct kh_ctx *kh = (struct kh_ctx *)ctx;
    char fp[64];
    if (ssh_key_fingerprint_sha256(blob, blob_len, fp, sizeof(fp)) < 0) return false;

    char host_id[96];
    if (port == SSH_DEFAULT_PORT) ksnprintf(host_id, sizeof(host_id), "%s", host);
    else ksnprintf(host_id, sizeof(host_id), "[%s]:%u", host, (unsigned)port);

    char kh_path[96];
    ksnprintf(kh_path, sizeof(kh_path), "%s/known_hosts", ssh_base_dir());

    uint8_t buf[4096];
    int n = vfs_read(kh_path, buf, sizeof(buf) - 1u);
    if (n > 0) {
        buf[n] = '\0';
        char *p = (char *)buf;
        while (*p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            char *sp1 = strchr(p, ' ');
            if (sp1) {
                *sp1 = '\0';
                char *sp2 = strchr(sp1 + 1, ' ');
                if (sp2 && strcmp(p, host_id) == 0) {
                    *sp2 = '\0';
                    const char *stored_type = sp1 + 1;
                    const char *stored_fp = sp2 + 1;
                    if (strcmp(stored_type, key_type) == 0 && strcmp(stored_fp, fp) == 0) {
                        return true;
                    }
                    cprintf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
                    cprintf("@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n");
                    cprintf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
                    cprintf("Host %s (%s) expected %s, got %s\n", host_id, key_type, stored_fp, fp);
                    return false;
                }
            }
            if (!nl) break;
            p = nl + 1;
        }
    }

    cprintf("The authenticity of host '%s' can't be established.\n", host_id);
    cprintf("%s key fingerprint is %s.\n", key_type, fp);
    if (!kh->auto_accept) {
        char ans[16];
        if (!read_line_prompt("Are you sure you want to continue connecting (yes/no)? ",
                              ans, sizeof(ans), true)) {
            return false;
        }
        if (strcmp(ans, "yes") != 0 && strcmp(ans, "y") != 0) return false;
    }

    ensure_ssh_dir();
    char line[192];
    int llen = ksnprintf(line, sizeof(line), "%s %s %s\n", host_id, key_type, fp);
    if (llen > 0) (void)vfs_append(kh_path, line, (uint32_t)llen);
    return true;
}

static bool ssh_pw_prompt(void *ctx, const char *user, const char *host,
                          char *pw_out, size_t pw_cap) {
    (void)ctx;
    char prompt[128];
    ksnprintf(prompt, sizeof(prompt), "%s@%s's password: ", user, host);
    return read_line_prompt(prompt, pw_out, pw_cap, false);
}

void cmd_ssh_keygen(const char *args) {
    char argbuf[256];
    ksnprintf(argbuf, sizeof(argbuf), "%s", args ? args : "");
    char *argv[16];
    int argc = parse_args(argbuf, argv, 16);

    bool show_pub = false;
    const char *comment = "lugalos";
    char key_path[96];
    ksnprintf(key_path, sizeof(key_path), "%s/id_ed25519", ssh_base_dir());

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-y") == 0) show_pub = true;
        else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) comment = argv[++i];
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            ksnprintf(key_path, sizeof(key_path), "%s", argv[++i]);
        }
    }

    uint8_t seed[32], secret[64], pub[32], blob[64];
    if (show_pub) {
        int n = vfs_read(key_path, seed, sizeof(seed));
        if (n != (int)sizeof(seed)) {
            cprintf("ssh-keygen: cannot read %s\n", key_path);
            return;
        }
        ed25519_keypair_from_seed(secret, pub, seed);
        crypto_wipe_bytes(seed, sizeof(seed));
        crypto_wipe_bytes(secret, sizeof(secret));
        char pub_str[192], fp[64];
        ssh_encode_ed25519_blob(pub, blob, sizeof(blob));
        ssh_format_ed25519_pub(pub, comment, pub_str, sizeof(pub_str));
        ssh_key_fingerprint_sha256(blob, 51u, fp, sizeof(fp));
        cprintf("%s%s\n", pub_str, fp);
        return;
    }

    if (!random_is_hardware()) {
        cprintf("ssh-keygen: hardware entropy unavailable\n");
        return;
    }
    ensure_ssh_dir();
    random_bytes(seed, sizeof(seed));
    ed25519_keypair_from_seed(secret, pub, seed);
    crypto_wipe_bytes(secret, sizeof(secret));

    if (vfs_write(key_path, seed, sizeof(seed)) < 0) {
        crypto_wipe_bytes(seed, sizeof(seed));
        cprintf("ssh-keygen: failed to write %s\n", key_path);
        return;
    }
    crypto_wipe_bytes(seed, sizeof(seed));

    char pub_path[112];
    ksnprintf(pub_path, sizeof(pub_path), "%s.pub", key_path);
    char pub_str[192], fp[64];
    int plen = ssh_format_ed25519_pub(pub, comment, pub_str, sizeof(pub_str));
    if (plen > 0) (void)vfs_write(pub_path, pub_str, (uint32_t)plen);
    ssh_encode_ed25519_blob(pub, blob, sizeof(blob));
    ssh_key_fingerprint_sha256(blob, 51u, fp, sizeof(fp));
    cprintf("Generated Ed25519 key in %s\nPublic key (%s):\n%s%s\n",
            key_path, pub_path, pub_str, fp);
}

void cmd_ssh(const char *args) {
    char argbuf[512];
    ksnprintf(argbuf, sizeof(argbuf), "%s", args ? args : "");
    char *argv[24];
    int argc = parse_args(argbuf, argv, 24);
    if (argc < 1) {
        cprintf("Usage: ssh [-p port] [-i keyfile] [-y] [user@]host [command...]\n");
        return;
    }

    uint16_t port = SSH_DEFAULT_PORT;
    bool auto_yes = false;
    char key_path[96];
    ksnprintf(key_path, sizeof(key_path), "%s/id_ed25519", ssh_base_dir());

    int idx = 0;
    while (idx < argc && argv[idx][0] == '-') {
        if (strcmp(argv[idx], "-p") == 0 && idx + 1 < argc) {
            uint32_t pval = 0;
            for (const char *s = argv[++idx]; *s >= '0' && *s <= '9'; s++) {
                pval = pval * 10u + (uint32_t)(*s - '0');
            }
            if (pval > 0 && pval <= 65535u) port = (uint16_t)pval;
            idx++;
        } else if (strcmp(argv[idx], "-i") == 0 && idx + 1 < argc) {
            ksnprintf(key_path, sizeof(key_path), "%s", argv[++idx]);
            idx++;
        } else if (strcmp(argv[idx], "-y") == 0) {
            auto_yes = true;
            idx++;
        } else {
            cprintf("ssh: unknown option %s\n", argv[idx]);
            return;
        }
    }
    if (idx >= argc) {
        cprintf("Usage: ssh [-p port] [-i keyfile] [-y] [user@]host [command...]\n");
        return;
    }

    const char *target = argv[idx++];
    char user[64] = "root";
    char host[128];
    const char *at = strchr(target, '@');
    if (at) {
        size_t ulen = (size_t)(at - target);
        if (ulen >= sizeof(user)) ulen = sizeof(user) - 1u;
        memcpy(user, target, ulen);
        user[ulen] = '\0';
        ksnprintf(host, sizeof(host), "%s", at + 1);
    } else {
        ksnprintf(host, sizeof(host), "%s", target);
    }

    uint8_t seed[32];
    bool have_key = (vfs_read(key_path, seed, sizeof(seed)) == (int)sizeof(seed));

    struct kh_ctx kh = { .auto_accept = auto_yes };
    ssh_cfg_t cfg = {
        .host = host,
        .port = port,
        .user = user,
        .ed25519_seed = have_key ? seed : NULL,
        .timeout_ms = 15000u,
        .hostkey_cb = known_hosts_cb,
        .hostkey_ctx = &kh,
        .password_cb = ssh_pw_prompt,
    };

    ssh_session_t *s = NULL;
    char err[96] = {0};
    int rc = ssh_connect_tcp(&s, &cfg, err, sizeof(err));
    crypto_wipe_bytes(seed, sizeof(seed));
    if (rc != SSH_OK) {
        cprintf("ssh: %s\n", err[0] ? err : "connection failed");
        return;
    }

    if (idx < argc) {
        char cmd[512];
        size_t pos = 0;
        for (int i = idx; i < argc && pos + 2u < sizeof(cmd); i++) {
            if (i > idx) cmd[pos++] = ' ';
            size_t al = strlen(argv[i]);
            if (pos + al >= sizeof(cmd)) al = sizeof(cmd) - pos - 1u;
            memcpy(cmd + pos, argv[i], al);
            pos += al;
        }
        cmd[pos] = '\0';
        rc = ssh_open_exec(s, cmd);
        if (rc != SSH_OK) {
            cprintf("ssh: exec failed (%d)\n", rc);
            ssh_close(s);
            return;
        }
        uint8_t buf[512];
        for (;;) {
            bool is_err = false;
            int n = ssh_channel_read(s, buf, sizeof(buf), &is_err);
            if (n == SSH_EOF || n < 0) break;
            if (n > 0) {
                for (int i = 0; i < n; i++) console_putc((char)buf[i]);
            } else {
                if (console_interrupt_requested()) break;
                sched_yield();
            }
        }
        ssh_close(s);
        return;
    }

    unsigned cols = 80u, rows = 24u;
    (void)console_size(&cols, &rows);
    rc = ssh_open_shell(s, cols, rows);
    if (rc != SSH_OK) {
        cprintf("ssh: shell failed (%d)\n", rc);
        ssh_close(s);
        return;
    }

    uint8_t buf[512];
    for (;;) {
        bool did_work = false;
        bool is_err = false;
        int n = ssh_channel_read(s, buf, sizeof(buf), &is_err);
        if (n == SSH_EOF || n < 0) break;
        if (n > 0) {
            for (int i = 0; i < n; i++) console_putc((char)buf[i]);
            did_work = true;
        }
        if (console_has_char()) {
            key_event_t ev = keyseq_read_console();
            uint8_t seq[24];
            size_t slen = keyseq_encode(ev, seq, sizeof(seq));
            if (slen > 0) (void)ssh_channel_write(s, seq, (uint32_t)slen);
            did_work = true;
        }
        if (!did_work) sched_yield();
    }
    ssh_close(s);
}

#else
typedef int ssh_cli_unused_t;
#endif
