#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "liblugal.h"

#include <stdio.h>
#include <string.h>

#define LUGAL_AF_INET     2
#define LUGAL_SOCK_STREAM 1
#define LUGAL_IPPROTO_TCP 6

typedef struct {
    mp_obj_base_t base;
    int handle;
    uint32_t timeout_ms;
} lugal_socket_obj_t;

static const mp_obj_type_t lugal_socket_type;

static bool net_resolve_host(const char *host, uint8_t ip_out[4]) {
    uint8_t req[129];
    req[0] = 'D';
    size_t hl = strlen(host);
    if (hl > sizeof(req) - 2) hl = sizeof(req) - 2;
    memcpy(req + 1, host, hl);
    req[1 + hl] = '\0';
    uint8_t resp[5];
    long n = uchan_call("net", req, (long)(2 + hl), resp, sizeof(resp));
    if (n < 5 || resp[0] == 0) {
        return false;
    }
    memcpy(ip_out, resp + 1, 4);
    return true;
}

static mp_obj_t sock_getaddrinfo(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    const char *host = mp_obj_str_get_str(args[0]);
    mp_int_t port = mp_obj_get_int(args[1]);
    uint8_t ip[4];
    if (!net_resolve_host(host, ip)) {
        mp_raise_OSError(MP_EHOSTUNREACH);
    }
    char ip_str[20];
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    mp_obj_t addr[2] = {
        mp_obj_new_str(ip_str, strlen(ip_str)),
        mp_obj_new_int(port),
    };
    mp_obj_t tuple[5] = {
        mp_obj_new_int(LUGAL_AF_INET),
        mp_obj_new_int(LUGAL_SOCK_STREAM),
        mp_obj_new_int(0),
        MP_OBJ_NEW_QSTR(MP_QSTR_),
        mp_obj_new_tuple(2, addr),
    };
    mp_obj_t item = mp_obj_new_tuple(5, tuple);
    return mp_obj_new_list(1, &item);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_getaddrinfo_obj, 2, 6, sock_getaddrinfo);

static mp_obj_t sock_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 3, false);
    (void)args;
    lugal_socket_obj_t *s = mp_obj_malloc_with_finaliser(lugal_socket_obj_t, type);
    s->handle = -1;
    s->timeout_ms = 10000u;
    return MP_OBJ_FROM_PTR(s);
}

static mp_obj_t sock_connect(mp_obj_t self_in, mp_obj_t addr_in) {
    lugal_socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t *items;
    mp_obj_get_array_fixed_n(addr_in, 2, &items);
    const char *host = mp_obj_str_get_str(items[0]);
    uint16_t port = (uint16_t)mp_obj_get_int(items[1]);

    uint8_t ip[4];
    if (!net_resolve_host(host, ip)) {
        mp_raise_OSError(MP_EHOSTUNREACH);
    }

    uint8_t req[7];
    req[0] = 'O';
    memcpy(req + 1, ip, 4);
    lugal_put16(req + 5, port);
    uint8_t resp[2];
    long n = uchan_call("net", req, sizeof(req), resp, sizeof(resp));
    if (n < 2 || resp[0] == 0) {
        mp_raise_OSError(MP_ECONNREFUSED);
    }
    self->handle = (int)resp[1];

    long start = utime_ms();
    for (;;) {
        uint8_t rreq[2] = { 'R', (uint8_t)self->handle };
        uint8_t rresp[1];
        long rn = uchan_call("net", rreq, sizeof(rreq), rresp, sizeof(rresp));
        if (rn >= 1 && rresp[0] == 1) {
            break;
        }
        if (rn < 1 || rresp[0] == 2) {
            uint8_t creq[2] = { 'C', (uint8_t)self->handle };
            (void)uchan_call("net", creq, sizeof(creq), rresp, sizeof(rresp));
            self->handle = -1;
            mp_raise_OSError(MP_ECONNREFUSED);
        }
        if ((unsigned long)(utime_ms() - start) >= self->timeout_ms) {
            uint8_t creq[2] = { 'C', (uint8_t)self->handle };
            (void)uchan_call("net", creq, sizeof(creq), rresp, sizeof(rresp));
            self->handle = -1;
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        uyield();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_connect_obj, sock_connect);

static mp_obj_t sock_settimeout(mp_obj_t self_in, mp_obj_t to_obj) {
    lugal_socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (to_obj == mp_const_none) {
        self->timeout_ms = 60000u;
    } else {
        mp_int_t s = mp_obj_get_int(to_obj);
        self->timeout_ms = s > 0 ? (uint32_t)s * 1000u : 1u;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_settimeout_obj, sock_settimeout);

static mp_uint_t sock_stream_read(mp_obj_t o_in, void *buf, mp_uint_t size, int *errcode) {
    lugal_socket_obj_t *self = MP_OBJ_TO_PTR(o_in);
    if (self->handle < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    if (size == 0) return 0;

    long start = utime_ms();
    for (;;) {
        uint16_t chunk = size > 256u ? 256u : (uint16_t)size;
        uint8_t req[4] = { 'G', (uint8_t)self->handle, 0, 0 };
        lugal_put16(req + 2, chunk);
        uint8_t resp[257];
        long n = uchan_call("net", req, sizeof(req), resp, sizeof(resp));
        if (n < 1 || resp[0] == 2) {
            *errcode = MP_EIO;
            return MP_STREAM_ERROR;
        }
        if (resp[0] == 1) {
            return 0; /* EOF */
        }
        if (n > 1) {
            memcpy(buf, resp + 1, (size_t)(n - 1));
            return (mp_uint_t)(n - 1);
        }
        if ((unsigned long)(utime_ms() - start) >= self->timeout_ms) {
            *errcode = MP_ETIMEDOUT;
            return MP_STREAM_ERROR;
        }
        uyield();
    }
}

static mp_uint_t sock_stream_write(mp_obj_t o_in, const void *buf, mp_uint_t size, int *errcode) {
    lugal_socket_obj_t *self = MP_OBJ_TO_PTR(o_in);
    if (self->handle < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    const uint8_t *p = (const uint8_t *)buf;
    mp_uint_t total = 0;
    long start = utime_ms();
    while (total < size) {
        mp_uint_t chunk = size - total;
        if (chunk > 256u) chunk = 256u;
        uint8_t req[258];
        req[0] = 'W';
        req[1] = (uint8_t)self->handle;
        memcpy(req + 2, p + total, chunk);
        uint8_t resp[3];
        long n = uchan_call("net", req, (long)(2u + chunk), resp, sizeof(resp));
        if (n < 3 || resp[0] == 0) {
            *errcode = MP_EIO;
            return MP_STREAM_ERROR;
        }
        uint16_t w = lugal_get16(resp + 1);
        total += w;
        if (w == 0) {
            if ((unsigned long)(utime_ms() - start) >= self->timeout_ms) {
                *errcode = MP_ETIMEDOUT;
                return MP_STREAM_ERROR;
            }
            uyield();
        }
    }
    return total;
}

static mp_uint_t sock_stream_ioctl(mp_obj_t o_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)arg;
    lugal_socket_obj_t *self = MP_OBJ_TO_PTR(o_in);
    if (request == MP_STREAM_CLOSE) {
        if (self->handle >= 0) {
            uint8_t req[2] = { 'C', (uint8_t)self->handle };
            uint8_t resp[1];
            (void)uchan_call("net", req, sizeof(req), resp, sizeof(resp));
            self->handle = -1;
        }
        return 0;
    }
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static mp_obj_t sock_send(mp_obj_t self_in, mp_obj_t data_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_in, &buf, MP_BUFFER_READ);
    int err = 0;
    mp_uint_t n = sock_stream_write(self_in, buf.buf, buf.len, &err);
    if (n == MP_STREAM_ERROR) mp_raise_OSError(err);
    return mp_obj_new_int_from_uint(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_send_obj, sock_send);

static mp_obj_t sock_sendall(mp_obj_t self_in, mp_obj_t data_in) {
    (void)sock_send(self_in, data_in);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_sendall_obj, sock_sendall);

static mp_obj_t sock_recv(mp_obj_t self_in, mp_obj_t size_in) {
    mp_int_t sz = mp_obj_get_int(size_in);
    if (sz <= 0) return mp_obj_new_bytes(NULL, 0);
    vstr_t vstr;
    vstr_init_len(&vstr, (size_t)sz);
    int err = 0;
    mp_uint_t n = sock_stream_read(self_in, vstr.buf, (mp_uint_t)sz, &err);
    if (n == MP_STREAM_ERROR) {
        vstr_clear(&vstr);
        mp_raise_OSError(err);
    }
    vstr.len = n;
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_recv_obj, sock_recv);

static const mp_rom_map_elem_t sock_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect),    MP_ROM_PTR(&sock_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_send),       MP_ROM_PTR(&sock_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendall),    MP_ROM_PTR(&sock_sendall_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv),       MP_ROM_PTR(&sock_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),       MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),   MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline),   MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),      MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),      MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),    MP_ROM_PTR(&mp_stream_close_obj) },
};
static MP_DEFINE_CONST_DICT(sock_locals_dict, sock_locals_dict_table);

static const mp_stream_p_t sock_stream_p = {
    .read = sock_stream_read,
    .write = sock_stream_write,
    .ioctl = sock_stream_ioctl,
    .is_text = false,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    lugal_socket_type,
    MP_QSTR_socket,
    MP_TYPE_FLAG_NONE,
    make_new, sock_make_new,
    protocol, &sock_stream_p,
    locals_dict, &sock_locals_dict
);

static const mp_rom_map_elem_t socket_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_socket) },
    { MP_ROM_QSTR(MP_QSTR_socket),      MP_ROM_PTR(&lugal_socket_type) },
    { MP_ROM_QSTR(MP_QSTR_getaddrinfo), MP_ROM_PTR(&sock_getaddrinfo_obj) },
    { MP_ROM_QSTR(MP_QSTR_AF_INET),     MP_ROM_INT(LUGAL_AF_INET) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_STREAM), MP_ROM_INT(LUGAL_SOCK_STREAM) },
    { MP_ROM_QSTR(MP_QSTR_IPPROTO_TCP), MP_ROM_INT(LUGAL_IPPROTO_TCP) },
};
static MP_DEFINE_CONST_DICT(socket_module_globals, socket_module_globals_table);

const mp_obj_module_t mp_module_socket = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&socket_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_socket, mp_module_socket);
