#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "usys.h"

#include <string.h>

/* ---- machine.I2C ---- */

typedef struct {
    mp_obj_base_t base;
    uint8_t bus;
} lugal_i2c_obj_t;

static mp_obj_t i2c_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 2, true);
    lugal_i2c_obj_t *self = mp_obj_malloc(lugal_i2c_obj_t, type);
    self->bus = n_args >= 1 ? (uint8_t)mp_obj_get_int(args[0]) : 0u;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t i2c_scan(mp_obj_t self_in) {
    lugal_i2c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint8_t req[2] = { 'S', self->bus };
    uint8_t resp[128];
    long n = uchan_call("hal", req, sizeof(req), resp, sizeof(resp));
    if (n < 1 || resp[0] == 0) {
        mp_raise_OSError(MP_EIO);
    }
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (long i = 1; i < n; i++) {
        mp_obj_list_append(list, mp_obj_new_int(resp[i]));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(i2c_scan_obj, i2c_scan);

static void i2c_xfer_checked(uint8_t bus, uint8_t addr, const uint8_t *w, size_t wlen,
                             uint8_t *r, size_t rlen) {
    if (wlen > 40u || rlen > 64u) {
        mp_raise_ValueError(MP_ERROR_TEXT("I2C transfer too large"));
    }
    uint8_t req[48];
    req[0] = 'I';
    req[1] = bus;
    req[2] = addr;
    req[3] = (uint8_t)wlen;
    req[4] = (uint8_t)rlen;
    if (wlen) memcpy(req + 5, w, wlen);
    uint8_t resp[72];
    long n = uchan_call("hal", req, (long)(5u + wlen), resp, sizeof(resp));
    if (n < 1 || resp[0] == 0 || (size_t)(n - 1) < rlen) {
        mp_raise_OSError(MP_EIO);
    }
    if (rlen && r) memcpy(r, resp + 1, rlen);
}

static mp_obj_t i2c_writeto(mp_obj_t self_in, mp_obj_t addr_obj, mp_obj_t buf_obj) {
    lugal_i2c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_obj, &buf, MP_BUFFER_READ);
    i2c_xfer_checked(self->bus, (uint8_t)mp_obj_get_int(addr_obj), buf.buf, buf.len, NULL, 0);
    return mp_obj_new_int_from_uint(buf.len);
}
static MP_DEFINE_CONST_FUN_OBJ_3(i2c_writeto_obj, i2c_writeto);

static mp_obj_t i2c_readfrom(mp_obj_t self_in, mp_obj_t addr_obj, mp_obj_t len_obj) {
    lugal_i2c_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t rlen = mp_obj_get_int(len_obj);
    if (rlen < 0) mp_raise_ValueError(MP_ERROR_TEXT("length < 0"));
    vstr_t vstr;
    vstr_init_len(&vstr, (size_t)rlen);
    i2c_xfer_checked(self->bus, (uint8_t)mp_obj_get_int(addr_obj), NULL, 0,
                     (uint8_t *)vstr.buf, (size_t)rlen);
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_3(i2c_readfrom_obj, i2c_readfrom);

static mp_obj_t i2c_readfrom_mem(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    lugal_i2c_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint8_t addr = (uint8_t)mp_obj_get_int(args[1]);
    uint8_t reg = (uint8_t)mp_obj_get_int(args[2]);
    mp_int_t rlen = mp_obj_get_int(args[3]);
    if (rlen < 0) mp_raise_ValueError(MP_ERROR_TEXT("length < 0"));
    vstr_t vstr;
    vstr_init_len(&vstr, (size_t)rlen);
    i2c_xfer_checked(self->bus, addr, &reg, 1, (uint8_t *)vstr.buf, (size_t)rlen);
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i2c_readfrom_mem_obj, 4, 4, i2c_readfrom_mem);

static mp_obj_t i2c_writeto_mem(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    lugal_i2c_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint8_t addr = (uint8_t)mp_obj_get_int(args[1]);
    uint8_t reg = (uint8_t)mp_obj_get_int(args[2]);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[3], &buf, MP_BUFFER_READ);
    if (buf.len > 39u) {
        mp_raise_ValueError(MP_ERROR_TEXT("I2C write too large"));
    }
    uint8_t wbuf[40];
    wbuf[0] = reg;
    if (buf.len) memcpy(wbuf + 1, buf.buf, buf.len);
    i2c_xfer_checked(self->bus, addr, wbuf, 1u + buf.len, NULL, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i2c_writeto_mem_obj, 4, 4, i2c_writeto_mem);

static const mp_rom_map_elem_t i2c_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_scan),          MP_ROM_PTR(&i2c_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeto),       MP_ROM_PTR(&i2c_writeto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readfrom),      MP_ROM_PTR(&i2c_readfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_readfrom_mem),  MP_ROM_PTR(&i2c_readfrom_mem_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeto_mem),   MP_ROM_PTR(&i2c_writeto_mem_obj) },
};
static MP_DEFINE_CONST_DICT(i2c_locals_dict, i2c_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    machine_i2c_type,
    MP_QSTR_I2C,
    MP_TYPE_FLAG_NONE,
    make_new, i2c_make_new,
    locals_dict, &i2c_locals_dict
);

/* ---- machine.Pin ---- */

enum {
    LUGAL_PIN_IN = 0,
    LUGAL_PIN_OUT = 1,
};

typedef struct {
    mp_obj_base_t base;
    uint8_t pin;
    uint8_t mode;
    uint8_t val;
} lugal_pin_obj_t;

static mp_obj_t pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 3, false);
    lugal_pin_obj_t *self = mp_obj_malloc(lugal_pin_obj_t, type);
    self->pin = (uint8_t)mp_obj_get_int(args[0]);
    self->mode = n_args >= 2 ? (uint8_t)mp_obj_get_int(args[1]) : LUGAL_PIN_IN;
    self->val = n_args >= 3 ? (mp_obj_is_true(args[2]) ? 1u : 0u) : 0u;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t pin_value(size_t n_args, const mp_obj_t *args) {
    lugal_pin_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint8_t req[4] = { 'P', n_args >= 2 ? 3u : 2u, self->pin,
                       n_args >= 2 ? (mp_obj_is_true(args[1]) ? 1u : 0u) : 0u };
    uint8_t resp[2];
    long n = uchan_call("hal", req, sizeof(req), resp, sizeof(resp));
    if (n < 1 || resp[0] == 0) {
        mp_raise_OSError(MP_ENODEV);
    }
    if (n_args >= 2) {
        self->val = req[3];
        return mp_const_none;
    }
    return mp_obj_new_int(n >= 2 ? resp[1] : self->val);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pin_value_obj, 1, 2, pin_value);

static const mp_rom_map_elem_t pin_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_IN),    MP_ROM_INT(LUGAL_PIN_IN) },
    { MP_ROM_QSTR(MP_QSTR_OUT),   MP_ROM_INT(LUGAL_PIN_OUT) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&pin_value_obj) },
};
static MP_DEFINE_CONST_DICT(pin_locals_dict, pin_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_type,
    MP_QSTR_Pin,
    MP_TYPE_FLAG_NONE,
    make_new, pin_make_new,
    locals_dict, &pin_locals_dict
);

static const mp_rom_map_elem_t machine_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_machine) },
    { MP_ROM_QSTR(MP_QSTR_I2C),      MP_ROM_PTR(&machine_i2c_type) },
    { MP_ROM_QSTR(MP_QSTR_Pin),      MP_ROM_PTR(&machine_pin_type) },
};
static MP_DEFINE_CONST_DICT(machine_module_globals, machine_module_globals_table);

const mp_obj_module_t mp_module_machine = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&machine_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_machine, mp_module_machine);
