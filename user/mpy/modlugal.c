#include "py/obj.h"
#include "py/runtime.h"
#include "liblugal.h"

#include <string.h>

/* ---- lugal.canvas ---- */

static mp_obj_t canvas_open(void) {
    return mp_obj_new_bool(lugal_canvas_open());
}
static MP_DEFINE_CONST_FUN_OBJ_0(canvas_open_obj, canvas_open);

static mp_obj_t canvas_close(void) {
    return mp_obj_new_bool(lugal_canvas_close());
}
static MP_DEFINE_CONST_FUN_OBJ_0(canvas_close_obj, canvas_close);

static mp_obj_t canvas_size(void) {
    uint16_t w = 0, h = 0;
    if (!lugal_canvas_size(&w, &h)) {
        mp_raise_OSError(5); /* EIO */
    }
    mp_obj_t items[2] = { mp_obj_new_int(w), mp_obj_new_int(h) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(canvas_size_obj, canvas_size);

static mp_obj_t canvas_fill(mp_obj_t colour_obj) {
    uint16_t colour = (uint16_t)mp_obj_get_int(colour_obj);
    return mp_obj_new_bool(lugal_canvas_fill(colour));
}
static MP_DEFINE_CONST_FUN_OBJ_1(canvas_fill_obj, canvas_fill);

static mp_obj_t canvas_pixel(mp_obj_t x_obj, mp_obj_t y_obj, mp_obj_t col_obj) {
    return mp_obj_new_bool(lugal_canvas_pixel(
        (uint16_t)mp_obj_get_int(x_obj),
        (uint16_t)mp_obj_get_int(y_obj),
        (uint16_t)mp_obj_get_int(col_obj)));
}
static MP_DEFINE_CONST_FUN_OBJ_3(canvas_pixel_obj, canvas_pixel);

static mp_obj_t canvas_rect(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return mp_obj_new_bool(lugal_canvas_rect(
        (uint16_t)mp_obj_get_int(args[0]),
        (uint16_t)mp_obj_get_int(args[1]),
        (uint16_t)mp_obj_get_int(args[2]),
        (uint16_t)mp_obj_get_int(args[3]),
        (uint16_t)mp_obj_get_int(args[4])));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_rect_obj, 5, 5, canvas_rect);

static mp_obj_t canvas_frame(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return mp_obj_new_bool(lugal_canvas_frame(
        (uint16_t)mp_obj_get_int(args[0]),
        (uint16_t)mp_obj_get_int(args[1]),
        (uint16_t)mp_obj_get_int(args[2]),
        (uint16_t)mp_obj_get_int(args[3]),
        (uint16_t)mp_obj_get_int(args[4])));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_frame_obj, 5, 5, canvas_frame);

static mp_obj_t canvas_line(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return mp_obj_new_bool(lugal_canvas_line(
        (uint16_t)mp_obj_get_int(args[0]),
        (uint16_t)mp_obj_get_int(args[1]),
        (uint16_t)mp_obj_get_int(args[2]),
        (uint16_t)mp_obj_get_int(args[3]),
        (uint16_t)mp_obj_get_int(args[4])));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_line_obj, 5, 5, canvas_line);

static mp_obj_t canvas_circle(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return mp_obj_new_bool(lugal_canvas_circle(
        (uint16_t)mp_obj_get_int(args[0]),
        (uint16_t)mp_obj_get_int(args[1]),
        (uint16_t)mp_obj_get_int(args[2]),
        (uint16_t)mp_obj_get_int(args[3])));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_circle_obj, 4, 4, canvas_circle);

static mp_obj_t canvas_text(size_t n_args, const mp_obj_t *args) {
    uint16_t x = (uint16_t)mp_obj_get_int(args[0]);
    uint16_t y = (uint16_t)mp_obj_get_int(args[1]);
    const char *s = mp_obj_str_get_str(args[2]);
    uint16_t col = n_args >= 4 ? (uint16_t)mp_obj_get_int(args[3]) : 0xffffu;
    uint16_t scale = n_args >= 5 ? (uint16_t)mp_obj_get_int(args[4]) : 1u;
    return mp_obj_new_bool(lugal_canvas_text(x, y, s, col, scale));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(canvas_text_obj, 3, 5, canvas_text);

static mp_obj_t canvas_swap(void) {
    return mp_obj_new_bool(lugal_canvas_swap());
}
static MP_DEFINE_CONST_FUN_OBJ_0(canvas_swap_obj, canvas_swap);

static mp_obj_t canvas_rgb(mp_obj_t r_obj, mp_obj_t g_obj, mp_obj_t b_obj) {
    return mp_obj_new_int(lugal_rgb565(
        (uint8_t)mp_obj_get_int(r_obj),
        (uint8_t)mp_obj_get_int(g_obj),
        (uint8_t)mp_obj_get_int(b_obj)));
}
static MP_DEFINE_CONST_FUN_OBJ_3(canvas_rgb_obj, canvas_rgb);

static const mp_rom_map_elem_t canvas_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_canvas) },
    { MP_ROM_QSTR(MP_QSTR_open),     MP_ROM_PTR(&canvas_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),    MP_ROM_PTR(&canvas_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_size),     MP_ROM_PTR(&canvas_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill),     MP_ROM_PTR(&canvas_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel),    MP_ROM_PTR(&canvas_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect),     MP_ROM_PTR(&canvas_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_frame),    MP_ROM_PTR(&canvas_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_line),     MP_ROM_PTR(&canvas_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_circle),   MP_ROM_PTR(&canvas_circle_obj) },
    { MP_ROM_QSTR(MP_QSTR_text),     MP_ROM_PTR(&canvas_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_swap),     MP_ROM_PTR(&canvas_swap_obj) },
    { MP_ROM_QSTR(MP_QSTR_rgb),      MP_ROM_PTR(&canvas_rgb_obj) },
};
static MP_DEFINE_CONST_DICT(canvas_module_globals, canvas_module_globals_table);

static const mp_obj_module_t mp_module_lugal_canvas = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&canvas_module_globals,
};

/* ---- lugal.sensor ---- */

static mp_obj_t sensor_list(void) {
    char buf[128];
    long n = lugal_sensor_list(buf, sizeof(buf));
    if (n < 0) {
        mp_raise_OSError(5);
    }
    mp_obj_t list = mp_obj_new_list(0, NULL);
    long start = 0;
    for (long i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            if (i > start) {
                mp_obj_list_append(list, mp_obj_new_str(buf + start, (size_t)(i - start)));
            }
            start = i + 1;
        }
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(sensor_list_obj, sensor_list);

static mp_obj_t sensor_read(size_t n_args, const mp_obj_t *args) {
    const char *name = mp_obj_str_get_str(args[0]);
    int32_t val = 0;
    if (n_args >= 2 && mp_obj_is_str(args[1])) {
        const char *chan = mp_obj_str_get_str(args[1]);
        if (!lugal_sensor_read_dev(name, chan, &val, NULL)) {
            return mp_const_none;
        }
        return mp_obj_new_int(val);
    }
    bool filtered = n_args >= 2 && mp_obj_is_true(args[1]);
    bool ok = filtered
        ? lugal_sensor_read_filtered(name, &val, NULL)
        : lugal_sensor_read(name, &val, NULL);
    if (ok) {
        return mp_obj_new_int(val);
    }
    /* Fall back to reading all supported channels if `name` is a device. */
    static const char *const chans[] = {
        "temp", "pressure", "humidity", "lux", "eco2", "tvoc", "gas_res",
        "co", "no2", "nh3", "co2", "pressure_msl", "ah", "dew_point", "iaq",
        "voltage", "current", "power", "battery",
        "accel_x", "accel_y", "accel_z", "gyro_x", "gyro_y", "gyro_z"
    };
    mp_obj_t dict = mp_obj_new_dict(0);
    size_t found = 0;
    for (size_t i = 0; i < sizeof(chans) / sizeof(chans[0]); i++) {
        if (lugal_sensor_read_dev(name, chans[i], &val, NULL)) {
            mp_obj_dict_store(dict,
                mp_obj_new_str(chans[i], strlen(chans[i])),
                mp_obj_new_int(val));
            found++;
        }
    }
    return found > 0 ? dict : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sensor_read_obj, 1, 2, sensor_read);

static mp_obj_t sensor_read_dev(mp_obj_t dev_obj, mp_obj_t chan_obj) {
    const char *dev = mp_obj_str_get_str(dev_obj);
    const char *chan = mp_obj_str_get_str(chan_obj);
    int32_t val = 0;
    if (!lugal_sensor_read_dev(dev, chan, &val, NULL)) {
        return mp_const_none;
    }
    return mp_obj_new_int(val);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sensor_read_dev_obj, sensor_read_dev);

static mp_obj_t sensor_age(mp_obj_t chan_obj) {
    const char *chan = mp_obj_str_get_str(chan_obj);
    int32_t val = 0;
    uint32_t age = 0;
    if (!lugal_sensor_read(chan, &val, &age)) {
        return mp_const_none;
    }
    return mp_obj_new_int_from_uint(age);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sensor_age_obj, sensor_age);

static const mp_rom_map_elem_t sensor_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sensor) },
    { MP_ROM_QSTR(MP_QSTR_list),     MP_ROM_PTR(&sensor_list_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),     MP_ROM_PTR(&sensor_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_dev), MP_ROM_PTR(&sensor_read_dev_obj) },
    { MP_ROM_QSTR(MP_QSTR_age),      MP_ROM_PTR(&sensor_age_obj) },
};
static MP_DEFINE_CONST_DICT(sensor_module_globals, sensor_module_globals_table);

static const mp_obj_module_t mp_module_lugal_sensor = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&sensor_module_globals,
};

/* ---- lugal.chan ---- */

static mp_obj_t chan_call(size_t n_args, const mp_obj_t *args) {
    const char *name = mp_obj_str_get_str(args[0]);
    mp_buffer_info_t req;
    mp_get_buffer_raise(args[1], &req, MP_BUFFER_READ);
    size_t max_resp = n_args >= 3 ? (size_t)mp_obj_get_int(args[2]) : 256u;
    if (max_resp > 4096u) {
        max_resp = 4096u;
    }
    uint8_t *resp = max_resp ? m_new(uint8_t, max_resp) : NULL;
    long n = uchan_call(name, req.buf, (long)req.len, resp, (long)max_resp);
    if (n < 0) {
        if (resp) m_del(uint8_t, resp, max_resp);
        mp_raise_OSError(5);
    }
    mp_obj_t out = mp_obj_new_bytes(resp, (size_t)n);
    if (resp) m_del(uint8_t, resp, max_resp);
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(chan_call_obj, 2, 3, chan_call);

static const mp_rom_map_elem_t chan_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_chan) },
    { MP_ROM_QSTR(MP_QSTR_call),     MP_ROM_PTR(&chan_call_obj) },
};
static MP_DEFINE_CONST_DICT(chan_module_globals, chan_module_globals_table);

static const mp_obj_module_t mp_module_lugal_chan = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&chan_module_globals,
};

/* ---- lugal.ui ---- */

enum {
    LUGAL_UI_OP_FILL = 1,
    LUGAL_UI_OP_LABEL = 2,
    LUGAL_UI_OP_BUTTON = 3,
    LUGAL_UI_OP_CHECKBOX = 5,
    LUGAL_UI_OP_TOGGLE = 6,
    LUGAL_UI_OP_SLIDER = 7,
    LUGAL_UI_OP_PROGRESS = 8,
    LUGAL_UI_OP_TEXTFIELD = 13,
    LUGAL_UI_OP_SECTION = 14,
    LUGAL_UI_OP_DISABLE = 15,
};

static bool ui_call_bool(const uint8_t *req, size_t req_len) {
    uint8_t resp[4];
    long n = uchan_call("ui", req, (long)req_len, resp, sizeof(resp));
    return n >= 1 && resp[0] != 0;
}

static bool ui_add_op(uint8_t op, const int32_t *nums, uint8_t nnums,
                      const char *const *strs, uint8_t nstrs) {
    uint8_t req[256];
    req[0] = 'A';
    req[1] = op;
    req[2] = nnums;
    req[3] = nstrs;
    size_t pos = 4;
    for (uint8_t i = 0; i < nnums; i++) {
        uint32_t u = (uint32_t)nums[i];
        if (pos + 4 > sizeof(req)) return false;
        req[pos++] = (uint8_t)(u & 0xffu);
        req[pos++] = (uint8_t)((u >> 8) & 0xffu);
        req[pos++] = (uint8_t)((u >> 16) & 0xffu);
        req[pos++] = (uint8_t)((u >> 24) & 0xffu);
    }
    for (uint8_t i = 0; i < nstrs; i++) {
        const char *s = strs[i] ? strs[i] : "";
        size_t sl = strlen(s);
        if (pos + sl + 1 > sizeof(req)) return false;
        memcpy(req + pos, s, sl + 1);
        pos += sl + 1;
    }
    return ui_call_bool(req, pos);
}

static mp_obj_t ui_open(mp_obj_t title_obj) {
    const char *title = mp_obj_str_get_str(title_obj);
    uint8_t req[65];
    req[0] = 'O';
    size_t tl = strlen(title);
    if (tl > sizeof(req) - 2) tl = sizeof(req) - 2;
    memcpy(req + 1, title, tl);
    req[1 + tl] = '\0';
    return mp_obj_new_bool(ui_call_bool(req, 2 + tl));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_open_obj, ui_open);

static mp_obj_t ui_close(void) {
    uint8_t req[1] = { 'C' };
    return mp_obj_new_bool(ui_call_bool(req, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_0(ui_close_obj, ui_close);

static mp_obj_t ui_size(void) {
    uint8_t req[1] = { 'S' };
    uint8_t resp[8];
    long n = uchan_call("ui", req, 1, resp, sizeof(resp));
    if (n < 5 || resp[0] == 0) {
        mp_raise_OSError(5);
    }
    mp_obj_t items[2] = {
        mp_obj_new_int(lugal_get16(resp + 1)),
        mp_obj_new_int(lugal_get16(resp + 3)),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ui_size_obj, ui_size);

static mp_obj_t ui_begin(void) {
    uint8_t req[1] = { 'B' };
    return mp_obj_new_bool(ui_call_bool(req, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_0(ui_begin_obj, ui_begin);

static mp_obj_t ui_end(void) {
    uint8_t req[1] = { 'E' };
    return mp_obj_new_bool(ui_call_bool(req, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_0(ui_end_obj, ui_end);

static mp_obj_t ui_fill(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int32_t nums[5] = {
        (int32_t)mp_obj_get_int(args[0]), (int32_t)mp_obj_get_int(args[1]),
        (int32_t)mp_obj_get_int(args[2]), (int32_t)mp_obj_get_int(args[3]),
        (int32_t)mp_obj_get_int(args[4]),
    };
    return mp_obj_new_bool(ui_add_op(LUGAL_UI_OP_FILL, nums, 5, NULL, 0));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_fill_obj, 5, 5, ui_fill);

static mp_obj_t ui_label(size_t n_args, const mp_obj_t *args) {
    int32_t nums[4] = {
        (int32_t)mp_obj_get_int(args[0]),
        (int32_t)mp_obj_get_int(args[1]),
        (n_args >= 4 && mp_obj_is_true(args[3])) ? 1 : 0,
        (n_args >= 5 && mp_obj_is_true(args[4])) ? 1 : 0,
    };
    const char *s = mp_obj_str_get_str(args[2]);
    return mp_obj_new_bool(ui_add_op(LUGAL_UI_OP_LABEL, nums, 4, &s, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_label_obj, 3, 5, ui_label);

static mp_obj_t ui_button(size_t n_args, const mp_obj_t *args) {
    int32_t nums[6] = {
        (int32_t)mp_obj_get_int(args[0]), (int32_t)mp_obj_get_int(args[1]),
        (int32_t)mp_obj_get_int(args[2]), (int32_t)mp_obj_get_int(args[3]),
        (int32_t)mp_obj_get_int(args[4]),
        (n_args >= 7 && mp_obj_is_true(args[6])) ? 1 : 0,
    };
    const char *s = mp_obj_str_get_str(args[5]);
    return mp_obj_new_bool(ui_add_op(LUGAL_UI_OP_BUTTON, nums, 6, &s, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_button_obj, 6, 7, ui_button);

static mp_obj_t ui_checkbox_like(uint8_t op, const mp_obj_t *args) {
    int32_t nums[6] = {
        (int32_t)mp_obj_get_int(args[0]), (int32_t)mp_obj_get_int(args[1]),
        (int32_t)mp_obj_get_int(args[2]), (int32_t)mp_obj_get_int(args[3]),
        (int32_t)mp_obj_get_int(args[4]), mp_obj_is_true(args[5]) ? 1 : 0,
    };
    const char *s = mp_obj_str_get_str(args[6]);
    return mp_obj_new_bool(ui_add_op(op, nums, 6, &s, 1));
}

static mp_obj_t ui_checkbox(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return ui_checkbox_like(LUGAL_UI_OP_CHECKBOX, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_checkbox_obj, 7, 7, ui_checkbox);

static mp_obj_t ui_toggle(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return ui_checkbox_like(LUGAL_UI_OP_TOGGLE, args);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_toggle_obj, 7, 7, ui_toggle);

static mp_obj_t ui_slider(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int32_t nums[8] = {
        (int32_t)mp_obj_get_int(args[0]), (int32_t)mp_obj_get_int(args[1]),
        (int32_t)mp_obj_get_int(args[2]), (int32_t)mp_obj_get_int(args[3]),
        (int32_t)mp_obj_get_int(args[4]), (int32_t)mp_obj_get_int(args[5]),
        (int32_t)mp_obj_get_int(args[6]), (int32_t)mp_obj_get_int(args[7]),
    };
    return mp_obj_new_bool(ui_add_op(LUGAL_UI_OP_SLIDER, nums, 8, NULL, 0));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_slider_obj, 8, 8, ui_slider);

static mp_obj_t ui_progress(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int32_t nums[5] = {
        (int32_t)mp_obj_get_int(args[0]), (int32_t)mp_obj_get_int(args[1]),
        (int32_t)mp_obj_get_int(args[2]), (int32_t)mp_obj_get_int(args[3]),
        (int32_t)mp_obj_get_int(args[4]),
    };
    return mp_obj_new_bool(ui_add_op(LUGAL_UI_OP_PROGRESS, nums, 5, NULL, 0));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_progress_obj, 5, 5, ui_progress);

static mp_obj_t ui_textfield(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int32_t nums[5] = {
        (int32_t)mp_obj_get_int(args[0]), (int32_t)mp_obj_get_int(args[1]),
        (int32_t)mp_obj_get_int(args[2]), (int32_t)mp_obj_get_int(args[3]),
        (int32_t)mp_obj_get_int(args[4]),
    };
    return mp_obj_new_bool(ui_add_op(LUGAL_UI_OP_TEXTFIELD, nums, 5, NULL, 0));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_textfield_obj, 5, 5, ui_textfield);

static mp_obj_t ui_section(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int32_t nums[3] = {
        (int32_t)mp_obj_get_int(args[0]),
        (int32_t)mp_obj_get_int(args[1]),
        (int32_t)mp_obj_get_int(args[2]),
    };
    const char *s = mp_obj_str_get_str(args[3]);
    return mp_obj_new_bool(ui_add_op(LUGAL_UI_OP_SECTION, nums, 3, &s, 1));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ui_section_obj, 4, 4, ui_section);

static mp_obj_t ui_disable(mp_obj_t parts_obj) {
    int32_t parts = (int32_t)mp_obj_get_int(parts_obj);
    return mp_obj_new_bool(ui_add_op(LUGAL_UI_OP_DISABLE, &parts, 1, NULL, 0));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_disable_obj, ui_disable);

static mp_obj_t ui_event(void) {
    uint8_t req[1] = { 'V' };
    uint8_t resp[16];
    long n = uchan_call("ui", req, 1, resp, sizeof(resp));
    if (n < 14 || resp[0] == 0) {
        return mp_const_none;
    }
    mp_obj_t items[6] = {
        mp_obj_new_int(resp[1]),
        mp_obj_new_int(lugal_get16(resp + 2)),
        mp_obj_new_int((int16_t)lugal_get16(resp + 4)),
        mp_obj_new_int((int16_t)lugal_get16(resp + 6)),
        mp_obj_new_int((int16_t)lugal_get16(resp + 8)),
        mp_obj_new_int(lugal_get_le32(resp + 10)),
    };
    return mp_obj_new_tuple(6, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ui_event_obj, ui_event);

static mp_obj_t ui_text(mp_obj_t id_obj) {
    uint16_t id = (uint16_t)mp_obj_get_int(id_obj);
    uint8_t req[3] = { 'G', 0, 0 };
    lugal_put16(req + 1, id);
    uint8_t resp[132];
    long n = uchan_call("ui", req, 3, resp, sizeof(resp));
    if (n < 1 || resp[0] == 0) {
        return mp_obj_new_str("", 0);
    }
    return mp_obj_new_str((const char *)(resp + 1), (size_t)(n - 1));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ui_text_obj, ui_text);

static mp_obj_t ui_set_text(mp_obj_t id_obj, mp_obj_t text_obj) {
    uint16_t id = (uint16_t)mp_obj_get_int(id_obj);
    const char *s = mp_obj_str_get_str(text_obj);
    uint8_t req[132];
    req[0] = 'T';
    lugal_put16(req + 1, id);
    size_t sl = strlen(s);
    if (sl > sizeof(req) - 4) sl = sizeof(req) - 4;
    memcpy(req + 3, s, sl);
    req[3 + sl] = '\0';
    return mp_obj_new_bool(ui_call_bool(req, 4 + sl));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ui_set_text_obj, ui_set_text);

static const mp_rom_map_elem_t ui_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_ui) },
    { MP_ROM_QSTR(MP_QSTR_open),      MP_ROM_PTR(&ui_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),     MP_ROM_PTR(&ui_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_size),      MP_ROM_PTR(&ui_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_begin),     MP_ROM_PTR(&ui_begin_obj) },
    { MP_ROM_QSTR(MP_QSTR_end),       MP_ROM_PTR(&ui_end_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill),      MP_ROM_PTR(&ui_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_label),     MP_ROM_PTR(&ui_label_obj) },
    { MP_ROM_QSTR(MP_QSTR_button),    MP_ROM_PTR(&ui_button_obj) },
    { MP_ROM_QSTR(MP_QSTR_checkbox),  MP_ROM_PTR(&ui_checkbox_obj) },
    { MP_ROM_QSTR(MP_QSTR_toggle),    MP_ROM_PTR(&ui_toggle_obj) },
    { MP_ROM_QSTR(MP_QSTR_slider),    MP_ROM_PTR(&ui_slider_obj) },
    { MP_ROM_QSTR(MP_QSTR_progress),  MP_ROM_PTR(&ui_progress_obj) },
    { MP_ROM_QSTR(MP_QSTR_textfield), MP_ROM_PTR(&ui_textfield_obj) },
    { MP_ROM_QSTR(MP_QSTR_section),   MP_ROM_PTR(&ui_section_obj) },
    { MP_ROM_QSTR(MP_QSTR_disable),   MP_ROM_PTR(&ui_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_event),     MP_ROM_PTR(&ui_event_obj) },
    { MP_ROM_QSTR(MP_QSTR_text),      MP_ROM_PTR(&ui_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_text),  MP_ROM_PTR(&ui_set_text_obj) },
};
static MP_DEFINE_CONST_DICT(ui_module_globals, ui_module_globals_table);

static const mp_obj_module_t mp_module_lugal_ui = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ui_module_globals,
};

/* ---- lugal package ---- */

static const mp_rom_map_elem_t lugal_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_lugal) },
    { MP_ROM_QSTR(MP_QSTR_canvas),   MP_ROM_PTR(&mp_module_lugal_canvas) },
    { MP_ROM_QSTR(MP_QSTR_sensor),   MP_ROM_PTR(&mp_module_lugal_sensor) },
    { MP_ROM_QSTR(MP_QSTR_chan),     MP_ROM_PTR(&mp_module_lugal_chan) },
    { MP_ROM_QSTR(MP_QSTR_ui),       MP_ROM_PTR(&mp_module_lugal_ui) },
};
static MP_DEFINE_CONST_DICT(lugal_module_globals, lugal_module_globals_table);

const mp_obj_module_t mp_module_lugal = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lugal_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lugal, mp_module_lugal);
