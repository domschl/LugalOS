#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "usys.h"

#include <string.h>

static char g_cwd[256] = "/";

void lugal_resolve_path(const char *in, char *out, size_t out_max) {
    if (!in || !in[0] || strcmp(in, ".") == 0) {
        strncpy(out, g_cwd, out_max - 1);
        out[out_max - 1] = '\0';
        return;
    }
    if (in[0] == '/') {
        strncpy(out, in, out_max - 1);
        out[out_max - 1] = '\0';
        return;
    }
    size_t cwd_len = strlen(g_cwd);
    size_t in_len = strlen(in);
    bool need_slash = (cwd_len == 0 || g_cwd[cwd_len - 1] != '/');
    size_t pos = 0;
    if (cwd_len < out_max - 1) {
        memcpy(out, g_cwd, cwd_len);
        pos = cwd_len;
    }
    if (need_slash && pos < out_max - 1) {
        out[pos++] = '/';
    }
    size_t rem = (out_max - 1) - pos;
    size_t cp = in_len < rem ? in_len : rem;
    memcpy(out + pos, in, cp);
    out[pos + cp] = '\0';
}

/* ---- File stream object ---- */

typedef struct {
    mp_obj_base_t base;
    int fd;
    bool is_text;
} lugal_file_obj_t;

static const mp_obj_type_t lugal_file_type;

static void check_open(lugal_file_obj_t *self) {
    if (self->fd < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("I/O operation on closed file"));
    }
}

static mp_uint_t file_read(mp_obj_t o_in, void *buf, mp_uint_t size, int *errcode) {
    lugal_file_obj_t *self = MP_OBJ_TO_PTR(o_in);
    if (self->fd < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    long n = uread(self->fd, buf, (unsigned long)size);
    if (n < 0) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)n;
}

static mp_uint_t file_write(mp_obj_t o_in, const void *buf, mp_uint_t size, int *errcode) {
    lugal_file_obj_t *self = MP_OBJ_TO_PTR(o_in);
    if (self->fd < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    long n = uwrite(self->fd, buf, (unsigned long)size);
    if (n < 0) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)n;
}

static mp_uint_t file_ioctl(mp_obj_t o_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    lugal_file_obj_t *self = MP_OBJ_TO_PTR(o_in);
    if (request == MP_STREAM_CLOSE) {
        if (self->fd >= 0) {
            uclose(self->fd);
            self->fd = -1;
        }
        return 0;
    }
    if (request == MP_STREAM_FLUSH) {
        return 0;
    }
    if (request == MP_STREAM_SEEK) {
        if (self->fd < 0) {
            *errcode = MP_EBADF;
            return MP_STREAM_ERROR;
        }
        struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
        long pos = ulseek(self->fd, (long)s->offset, s->whence);
        if (pos < 0) {
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
        }
        s->offset = (mp_off_t)pos;
        return 0;
    }
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static mp_obj_t file_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    lugal_file_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (self->fd >= 0) {
        uclose(self->fd);
        self->fd = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(file_exit_obj, 4, 4, file_exit);

static mp_obj_t file_fileno(mp_obj_t self_in) {
    lugal_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_open(self);
    return mp_obj_new_int(self->fd);
}
static MP_DEFINE_CONST_FUN_OBJ_1(file_fileno_obj, file_fileno);

static const mp_rom_map_elem_t file_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read),      MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),  MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline),  MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),     MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek),      MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell),      MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush),     MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),     MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno),    MP_ROM_PTR(&file_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),   MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),  MP_ROM_PTR(&file_exit_obj) },
};
static MP_DEFINE_CONST_DICT(file_locals_dict, file_locals_dict_table);

static const mp_stream_p_t file_stream_p = {
    .read = file_read,
    .write = file_write,
    .ioctl = file_ioctl,
    .is_text = true,
};

static const mp_stream_p_t file_bin_stream_p = {
    .read = file_read,
    .write = file_write,
    .ioctl = file_ioctl,
    .is_text = false,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    lugal_file_type,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &file_stream_p,
    locals_dict, &file_locals_dict
);

static MP_DEFINE_CONST_OBJ_TYPE(
    lugal_file_bin_type,
    MP_QSTR_FileIO,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &file_bin_stream_p,
    locals_dict, &file_locals_dict
);

static mp_obj_t lugal_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    (void)kwargs;
    const char *raw_path = mp_obj_str_get_str(args[0]);
    const char *mode = n_args >= 2 ? mp_obj_str_get_str(args[1]) : "r";

    int flags = 0;
    bool is_text = true;
    bool append = false;
    for (const char *p = mode; *p; p++) {
        switch (*p) {
            case 'r':
                flags |= LUGAL_O_READ;
                break;
            case 'w':
                flags |= LUGAL_O_WRITE | LUGAL_O_CREATE | LUGAL_O_TRUNC;
                break;
            case 'a':
                flags |= LUGAL_O_WRITE | LUGAL_O_CREATE;
                append = true;
                break;
            case '+':
                flags |= LUGAL_O_READ | LUGAL_O_WRITE;
                break;
            case 'b':
                is_text = false;
                break;
            case 't':
                is_text = true;
                break;
            default:
                mp_raise_ValueError(MP_ERROR_TEXT("invalid file mode"));
        }
    }
    if ((flags & (LUGAL_O_READ | LUGAL_O_WRITE)) == 0) {
        flags |= LUGAL_O_READ;
    }

    char path[256];
    lugal_resolve_path(raw_path, path, sizeof(path));
    int fd = uopen(path, flags);
    if (fd < 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    if (append) {
        (void)ulseek(fd, 0, LUGAL_SEEK_END);
    }

    lugal_file_obj_t *o = mp_obj_malloc_with_finaliser(
        lugal_file_obj_t, is_text ? &lugal_file_type : &lugal_file_bin_type);
    o->fd = fd;
    o->is_text = is_text;
    return MP_OBJ_FROM_PTR(o);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, lugal_builtin_open);

/* ---- os module ---- */

static mp_obj_t os_listdir(size_t n_args, const mp_obj_t *args) {
    char path[256];
    lugal_resolve_path(n_args >= 1 ? mp_obj_str_get_str(args[0]) : ".", path, sizeof(path));
    if (strcmp(path, "/") == 0) {
        static const char *const roots[] = {
            "flash0", "sd0", "nvme0", "c6", "proc", "dev"
        };
        mp_obj_t list = mp_obj_new_list(0, NULL);
        lugal_stat_t st;
        for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
            char probe[32];
            probe[0] = '/';
            strncpy(probe + 1, roots[i], sizeof(probe) - 2);
            probe[sizeof(probe) - 1] = '\0';
            if (ustat(probe, &st) == 0) {
                mp_obj_list_append(list, mp_obj_new_str(roots[i], strlen(roots[i])));
            }
        }
        return list;
    }
    int fd = uopen(path, LUGAL_O_READ);
    if (fd < 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    mp_obj_t list = mp_obj_new_list(0, NULL);
    char name[128];
    lugal_stat_t st;
    for (unsigned int idx = 0; ; idx++) {
        int rc = ureaddir(fd, idx, name, sizeof(name), &st);
        if (rc < 0) {
            break;
        }
        mp_obj_list_append(list, mp_obj_new_str(name, strlen(name)));
    }
    uclose(fd);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(os_listdir_obj, 0, 1, os_listdir);

static mp_obj_t os_stat(mp_obj_t path_obj) {
    char path[256];
    lugal_resolve_path(mp_obj_str_get_str(path_obj), path, sizeof(path));
    lugal_stat_t st;
    if (ustat(path, &st) != 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    mp_int_t mode = st.is_dir ? (0x4000 | 0755) : (0x8000 | 0644);
    mp_obj_t items[10] = {
        mp_obj_new_int(mode),
        mp_obj_new_int(0),
        mp_obj_new_int(0),
        mp_obj_new_int(1),
        mp_obj_new_int(0),
        mp_obj_new_int(0),
        mp_obj_new_int_from_uint(st.size),
        mp_obj_new_int(0),
        mp_obj_new_int(0),
        mp_obj_new_int(0),
    };
    return mp_obj_new_tuple(10, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_stat_obj, os_stat);

static mp_obj_t os_remove(mp_obj_t path_obj) {
    char path[256];
    lugal_resolve_path(mp_obj_str_get_str(path_obj), path, sizeof(path));
    if (uunlink(path) != 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_remove_obj, os_remove);

static mp_obj_t os_rmdir(mp_obj_t path_obj) {
    char path[256];
    lugal_resolve_path(mp_obj_str_get_str(path_obj), path, sizeof(path));
    if (urmdir(path) != 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_rmdir_obj, os_rmdir);

static mp_obj_t os_mkdir(mp_obj_t path_obj) {
    char path[256];
    lugal_resolve_path(mp_obj_str_get_str(path_obj), path, sizeof(path));
    if (umkdir(path) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_mkdir_obj, os_mkdir);

static mp_obj_t os_rename(mp_obj_t old_obj, mp_obj_t new_obj) {
    char oldp[256], newp[256];
    lugal_resolve_path(mp_obj_str_get_str(old_obj), oldp, sizeof(oldp));
    lugal_resolve_path(mp_obj_str_get_str(new_obj), newp, sizeof(newp));
    if (urename(oldp, newp) != 0) {
        mp_raise_OSError(MP_ENOENT);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(os_rename_obj, os_rename);

static mp_obj_t os_getcwd(void) {
    return mp_obj_new_str(g_cwd, strlen(g_cwd));
}
static MP_DEFINE_CONST_FUN_OBJ_0(os_getcwd_obj, os_getcwd);

static mp_obj_t os_chdir(mp_obj_t path_obj) {
    char path[256];
    lugal_resolve_path(mp_obj_str_get_str(path_obj), path, sizeof(path));
    lugal_stat_t st;
    if (strcmp(path, "/") != 0 && (ustat(path, &st) != 0 || !st.is_dir)) {
        mp_raise_OSError(MP_ENOENT);
    }
    strncpy(g_cwd, path, sizeof(g_cwd) - 1);
    g_cwd[sizeof(g_cwd) - 1] = '\0';
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(os_chdir_obj, os_chdir);

static const mp_rom_map_elem_t os_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },
    { MP_ROM_QSTR(MP_QSTR_sep),      MP_ROM_QSTR(MP_QSTR__slash_) },
    { MP_ROM_QSTR(MP_QSTR_listdir),  MP_ROM_PTR(&os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat),     MP_ROM_PTR(&os_stat_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove),   MP_ROM_PTR(&os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_unlink),   MP_ROM_PTR(&os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir),    MP_ROM_PTR(&os_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir),    MP_ROM_PTR(&os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename),   MP_ROM_PTR(&os_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd),   MP_ROM_PTR(&os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir),    MP_ROM_PTR(&os_chdir_obj) },
};
static MP_DEFINE_CONST_DICT(os_module_globals, os_module_globals_table);

const mp_obj_module_t mp_module_os = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&os_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_os, mp_module_os);
