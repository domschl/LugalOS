#define _GNU_SOURCE
#include "net/git.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool random_is_hardware(void) { return true; }

static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(data, 1u, strlen(data), f) == strlen(data));
    fclose(f);
}

static void read_file_str(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    size_t n = fread(out, 1u, cap - 1u, f);
    out[n] = '\0';
    fclose(f);
}

int main(void) {
    /* 1. Standard Git blob SHA-1 KAT: printf "hello world\n" | git hash-object --stdin */
    uint8_t bsha1[GIT_SHA1_LEN];
    char bhex[GIT_HEX_LEN + 1u];
    git_hash_object(GIT_OBJ_BLOB, "hello world\n", 12u, bsha1);
    git_sha1_to_hex(bsha1, bhex);
    assert(strcmp(bhex, "3b18e512dba79e4c8300dd08aeb37f8e728b8dad") == 0);

    /* 2. Delta applier test: base="hello world!" -> target="hello LugalOS world!" */
    static const uint8_t base[] = "hello world!";
    /* src_len=12, dst_len=20, copy(off=0,len=6), insert(8,"LugalOS "), copy(off=6,len=6) */
    static const uint8_t delta[] = {
        12, 20,
        0x90, 0x06,
        8, 'L', 'u', 'g', 'a', 'l', 'O', 'S', ' ',
        0x91, 6, 6
    };
    uint8_t *dout = NULL;
    size_t dlen = 0;
    assert(git_apply_delta(base, 12u, delta, sizeof(delta), &dout, &dlen) == GIT_OK);
    assert(dlen == 20u);
    assert(memcmp(dout, "hello LugalOS world!", 20u) == 0);
    git_free(dout, dlen + 1u);

    /* 3. Init, add, commit, pack, unpack into repo2, checkout */
    char tmp1[] = "/tmp/lugalos_git1_XXXXXX";
    char tmp2[] = "/tmp/lugalos_git2_XXXXXX";
    assert(mkdtemp(tmp1) != NULL);
    assert(mkdtemp(tmp2) != NULL);

    assert(git_init(tmp1) == GIT_OK);

    char p[512];
    snprintf(p, sizeof(p), "%s/README.md", tmp1);
    write_file(p, "LugalOS Git Test\n");
    snprintf(p, sizeof(p), "%s/src", tmp1);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/src/main.c", tmp1);
    write_file(p, "int main(void) { return 0; }\n");

    assert(git_add_file(tmp1, "README.md") == GIT_OK);
    assert(git_add_file(tmp1, "src/main.c") == GIT_OK);

    git_index_entry_t *ents = NULL;
    size_t nents = 0;
    assert(git_index_read(tmp1, &ents, &nents) == GIT_OK);
    assert(nents == 2u);
    git_free(ents, nents * sizeof(git_index_entry_t));

    uint8_t csha1[GIT_SHA1_LEN];
    assert(git_commit(tmp1, "Test <test@lugalos>", "Initial commit", csha1) == GIT_OK);

    /* Read commit -> tree -> subtree -> blobs and pack all objects */
    int ctype = -1, ttype = -1, stype = -1;
    uint8_t *cdata = NULL, *tdata = NULL, *sdata = NULL;
    size_t clen = 0, tlen = 0, slen = 0;
    assert(git_read_loose_obj(tmp1, csha1, &ctype, &cdata, &clen) == GIT_OK);
    assert(ctype == GIT_OBJ_COMMIT);
    uint8_t tsha1[GIT_SHA1_LEN];
    assert(git_hex_to_sha1((const char *)(cdata + 5u), tsha1));
    assert(git_read_loose_obj(tmp1, tsha1, &ttype, &tdata, &tlen) == GIT_OK);
    assert(ttype == GIT_OBJ_TREE);

    /* Collect tree + blobs + subtree SHA-1s for packfile */
    uint8_t objs[5][GIT_SHA1_LEN];
    memcpy(objs[0], csha1, GIT_SHA1_LEN);
    memcpy(objs[1], tsha1, GIT_SHA1_LEN);
    git_hash_object(GIT_OBJ_BLOB, "LugalOS Git Test\n", 17u, objs[2]);
    git_hash_object(GIT_OBJ_BLOB, "int main(void) { return 0; }\n", 29u, objs[3]);
    /* Extract subtree SHA1 from root tree (second entry: "40000 src\0<20 bytes>") */
    const uint8_t *src_ent = (const uint8_t *)memmem(tdata, tlen, "src\0", 4u);
    assert(src_ent != NULL);
    memcpy(objs[4], src_ent + 4u, GIT_SHA1_LEN);
    assert(git_read_loose_obj(tmp1, objs[4], &stype, &sdata, &slen) == GIT_OK);
    assert(stype == GIT_OBJ_TREE);

    git_free(cdata, clen + 1u);
    git_free(tdata, tlen + 1u);
    git_free(sdata, slen + 1u);

    uint8_t *pack = NULL;
    size_t pack_len = 0;
    assert(git_build_packfile(tmp1, objs, 5u, &pack, &pack_len) == GIT_OK);

    assert(git_init(tmp2) == GIT_OK);
    assert(git_unpack_packfile(tmp2, pack, pack_len) == GIT_OK);
    git_free(pack, pack_len);

    char chex[GIT_HEX_LEN + 1u];
    git_sha1_to_hex(csha1, chex);
    assert(git_checkout_ref(tmp2, chex) == GIT_OK);

    char fbuf[128];
    snprintf(p, sizeof(p), "%s/README.md", tmp2);
    read_file_str(p, fbuf, sizeof(fbuf));
    assert(strcmp(fbuf, "LugalOS Git Test\n") == 0);

    snprintf(p, sizeof(p), "%s/src/main.c", tmp2);
    read_file_str(p, fbuf, sizeof(fbuf));
    assert(strcmp(fbuf, "int main(void) { return 0; }\n") == 0);

    printf("git_host: all tests passed\n");
    return 0;
}
