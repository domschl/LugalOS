/* Auto-generated pre-sorted builtins table for binary search. */
/* Milestone 42.2 & 42.6: Flash-resident .rodata table with zero SRAM overhead. */

#ifndef LISP_BUILTINS_TABLE_H
#define LISP_BUILTINS_TABLE_H

#define BUILTIN_PRIM(name_str, func) \
    { (name_str), &(const lisp_val_t){ .type = LISP_PRIMITIVE, .u.prim = (func) } }
#define BUILTIN_VAL(name_str, val_ptr) \
    { (name_str), (val_ptr) }

static const builtin_t builtins[] = {
    BUILTIN_VAL("#f", &false_val),
    BUILTIN_VAL("#t", &true_val),
    BUILTIN_PRIM("*", prim_mul),
    BUILTIN_PRIM("+", prim_add),
    BUILTIN_PRIM("-", prim_sub),
    BUILTIN_PRIM("/", prim_div),
    BUILTIN_PRIM("/=", prim_ne),
    BUILTIN_PRIM("<", prim_lt),
    BUILTIN_PRIM("<=", prim_le),
    BUILTIN_PRIM("=", prim_eq),
    BUILTIN_PRIM(">", prim_gt),
    BUILTIN_PRIM(">=", prim_ge),
    BUILTIN_PRIM("abs", prim_abs),
    BUILTIN_PRIM("append", prim_append),
    BUILTIN_PRIM("apply", prim_apply),
    BUILTIN_PRIM("arch", prim_arch),
    BUILTIN_PRIM("assoc", prim_assoc),
    BUILTIN_PRIM("assq", prim_assq),
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    BUILTIN_PRIM("beep", prim_beep),
#endif
    BUILTIN_PRIM("bind", prim_bind),
    BUILTIN_PRIM("board", prim_board),
    BUILTIN_PRIM("boolean?", prim_boolean_p),
    BUILTIN_PRIM("boot-program", prim_boot_program),
    BUILTIN_PRIM("caaar", prim_caaar),
    BUILTIN_PRIM("caadr", prim_caadr),
    BUILTIN_PRIM("caar", prim_caar),
    BUILTIN_PRIM("cadar", prim_cadar),
    BUILTIN_PRIM("cadddr", prim_cadddr),
    BUILTIN_PRIM("caddr", prim_caddr),
    BUILTIN_PRIM("cadr", prim_cadr),
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-circle", prim_canvas_circle),
#endif
    BUILTIN_PRIM("canvas-fill", prim_canvas_fill),
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-frame", prim_canvas_frame),
#endif
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-get", prim_canvas_get),
#endif
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-invert", prim_canvas_invert),
#endif
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-line", prim_canvas_line),
#endif
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-on-redraw", prim_canvas_on_redraw),
#endif
    BUILTIN_PRIM("canvas-pixel", prim_canvas_pixel),
    BUILTIN_PRIM("canvas-rect", prim_canvas_rect),
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-row", prim_canvas_row),
#endif
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-size", prim_canvas_size),
#endif
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-swap", prim_canvas_swap),
#endif
    BUILTIN_PRIM("canvas-text", prim_canvas_text),
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-title", prim_canvas_title),
#endif
#if !(defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735)
    BUILTIN_PRIM("canvas-window", prim_canvas_window),
#endif
    BUILTIN_PRIM("car", prim_car),
    BUILTIN_PRIM("cat", prim_cat),
#if CONFIG_ENABLE_CC
    BUILTIN_PRIM("cc", prim_cc),
#endif
    BUILTIN_PRIM("cdaar", prim_cdaar),
    BUILTIN_PRIM("cdadr", prim_cdadr),
    BUILTIN_PRIM("cdar", prim_cdar),
    BUILTIN_PRIM("cddar", prim_cddar),
    BUILTIN_PRIM("cdddr", prim_cdddr),
    BUILTIN_PRIM("cddr", prim_cddr),
    BUILTIN_PRIM("cdr", prim_cdr),
#if CONFIG_ENABLE_CHESS
    BUILTIN_PRIM("chess", prim_chess),
#endif
#if CONFIG_ENABLE_CHESS
    BUILTIN_PRIM("chess-board-selftest", prim_chess_board_selftest),
#endif
#if CONFIG_ENABLE_CHESS
    BUILTIN_PRIM("chess-console", prim_chess_console),
#endif
#if CONFIG_ENABLE_CHESS
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735 && CONFIG_ENABLE_TM1638
    BUILTIN_PRIM("chess-run", prim_chess_run),
#endif
#endif
#if CONFIG_ENABLE_CHESS
    BUILTIN_PRIM("chess-san-selftest", prim_chess_san_selftest),
#endif
#if CONFIG_ENABLE_CHESS
    BUILTIN_PRIM("chess-selftest", prim_chess_selftest),
#endif
    BUILTIN_PRIM("clipboard", prim_clipboard),
    BUILTIN_PRIM("clipboard-set", prim_clipboard_set),
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    BUILTIN_PRIM("clock", prim_clock),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    BUILTIN_PRIM("clock-keys", prim_clock_keys),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    BUILTIN_PRIM("clock-leds", prim_clock_leds),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    BUILTIN_PRIM("clock-light", prim_clock_light),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    BUILTIN_PRIM("clock-text", prim_clock_text),
#endif
    BUILTIN_PRIM("cons", prim_cons),
    BUILTIN_PRIM("console-bind", prim_console_bind),
    BUILTIN_PRIM("console-device", prim_console_device),
    BUILTIN_PRIM("cp", prim_cp),
    BUILTIN_PRIM("date", prim_date),
    BUILTIN_PRIM("date-utc", prim_date_utc),
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-drivetest", prim_dcf_drivetest),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-hunt", prim_dcf_hunt),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-listen", prim_dcf_listen),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-mirror", prim_dcf_mirror),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
#if CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-monitor", prim_dcf_monitor),
#endif
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-pins", prim_dcf_pins),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-pinscan", prim_dcf_pinscan),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-poweron", prim_dcf_poweron),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-raw", prim_dcf_raw),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
#if CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-status", prim_dcf_status),
#endif
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    BUILTIN_PRIM("dcf-sync", prim_dcf_sync),
#endif
    BUILTIN_PRIM("dev-present?", prim_dev_present),
    BUILTIN_PRIM("devices", prim_devices),
    BUILTIN_PRIM("df", prim_df),
    BUILTIN_PRIM("display", prim_display),
    BUILTIN_PRIM("eeprom-read", prim_eeprom_read),
    BUILTIN_PRIM("eeprom-write", prim_eeprom_write),
    BUILTIN_PRIM("eq?", prim_eq_p),
    BUILTIN_PRIM("equal?", prim_equal_p),
    BUILTIN_PRIM("eval", prim_eval),
    BUILTIN_PRIM("exec", prim_exec),
    BUILTIN_PRIM("filter", prim_filter),
    BUILTIN_PRIM("for-each", prim_for_each),
    BUILTIN_PRIM("format", prim_format),
    BUILTIN_PRIM("gc-stats", prim_gc_stats),
    BUILTIN_PRIM("help", prim_help),
    BUILTIN_PRIM("i2c-scan", prim_i2c_scan),
    BUILTIN_PRIM("identity", prim_identity),
    BUILTIN_PRIM("identity-key", prim_identity_key),
    BUILTIN_PRIM("identity-name", prim_identity_name),
    BUILTIN_PRIM("identity-provision", prim_identity_provision),
    BUILTIN_PRIM("integer?", prim_integer_p),
    BUILTIN_PRIM("klog-attach", prim_klog_attach),
    BUILTIN_PRIM("klog-detach", prim_klog_detach),
    BUILTIN_PRIM("klog-sinks", prim_klog_sinks),
    BUILTIN_PRIM("length", prim_length),
    BUILTIN_PRIM("list", prim_list),
    BUILTIN_PRIM("list-ref", prim_list_ref),
    BUILTIN_PRIM("load", prim_load),
    BUILTIN_PRIM("ls", prim_ls),
    BUILTIN_PRIM("lsh", prim_lsh),
    BUILTIN_PRIM("map", prim_map),
    BUILTIN_PRIM("max", prim_max),
    BUILTIN_PRIM("member", prim_member),
    BUILTIN_PRIM("meminfo", prim_meminfo),
    BUILTIN_PRIM("memq", prim_memq),
    BUILTIN_PRIM("min", prim_min),
    BUILTIN_PRIM("mkdir", prim_mkdir),
    BUILTIN_PRIM("modulo", prim_modulo),
    BUILTIN_PRIM("mount-local", prim_mount_local),
    BUILTIN_PRIM("mount-ramdisk", prim_mount_ramdisk),
    BUILTIN_PRIM("mount-remote", prim_mount_remote),
    BUILTIN_PRIM("mounted?", prim_mounted),
    BUILTIN_PRIM("net-config", prim_net_config),
    BUILTIN_PRIM("net-identity", prim_net_identity),
    BUILTIN_PRIM("net-mount", prim_net_mount),
    BUILTIN_PRIM("net-status", prim_net_status),
    BUILTIN_PRIM("newline", prim_newline),
    BUILTIN_PRIM("nth", prim_list_ref),
    BUILTIN_PRIM("ntp-sync", prim_ntp_sync),
    BUILTIN_PRIM("null?", prim_null_p),
    BUILTIN_PRIM("number->string", prim_number_to_string),
    BUILTIN_PRIM("p9-cat", prim_p9_cat),
    BUILTIN_PRIM("p9-loopback", prim_p9_loopback),
    BUILTIN_PRIM("p9-remote-cat", prim_p9_remote_cat),
    BUILTIN_PRIM("p9-serve", prim_p9_serve),
    BUILTIN_PRIM("p9-uart-send", prim_p9_uart_send),
    BUILTIN_PRIM("p9-unserve", prim_p9_unserve),
    BUILTIN_PRIM("pair?", prim_pair_p),
    BUILTIN_PRIM("path-set", prim_path_set),
    BUILTIN_PRIM("peek", prim_peek),
    BUILTIN_PRIM("peers", prim_peers),
    BUILTIN_PRIM("peers-add", prim_peers_add),
    BUILTIN_PRIM("peers-remove", prim_peers_remove),
#if CONFIG_ENABLE_CHESS
    BUILTIN_PRIM("perft", prim_perft),
#endif
    BUILTIN_PRIM("poke", prim_poke),
    BUILTIN_PRIM("ports", prim_ports),
    BUILTIN_PRIM("procedure?", prim_procedure_p),
    BUILTIN_PRIM("ps", prim_ps),
    BUILTIN_PRIM("psram", prim_psram),
    BUILTIN_PRIM("quotient", prim_quotient),
    BUILTIN_PRIM("read-file", prim_read_file),
    BUILTIN_PRIM("release", prim_release),
    BUILTIN_PRIM("remainder", prim_remainder),
    BUILTIN_PRIM("reverse", prim_reverse),
    BUILTIN_PRIM("rm", prim_rm),
    BUILTIN_PRIM("rmdir", prim_rmdir),
    BUILTIN_PRIM("screenshot", prim_screenshot),
    BUILTIN_PRIM("set-date", prim_set_date),
    BUILTIN_PRIM("set-time", prim_set_date),
    BUILTIN_PRIM("spawn", prim_spawn),
    BUILTIN_PRIM("spawn-pump", prim_spawn_pump),
    BUILTIN_PRIM("string->number", prim_string_to_number),
    BUILTIN_PRIM("string-append", prim_string_append),
    BUILTIN_PRIM("string-bytes", prim_string_bytes),
    BUILTIN_PRIM("string-length", prim_string_length),
    BUILTIN_PRIM("string=?", prim_string_eq),
    BUILTIN_PRIM("string?", prim_string_p),
    BUILTIN_PRIM("substring", prim_substring),
    BUILTIN_PRIM("symbol?", prim_symbol_p),
    BUILTIN_PRIM("time", prim_time),
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    BUILTIN_PRIM("tm-display", prim_tm_display),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    BUILTIN_PRIM("tm-get-key", prim_tm_get_key),
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    BUILTIN_PRIM("tm-set-leds", prim_tm_set_leds),
#endif
    BUILTIN_PRIM("top", prim_top),
    BUILTIN_PRIM("touch", prim_touch),
    BUILTIN_PRIM("tz", prim_tz),
    BUILTIN_PRIM("unmount", prim_unmount),
    BUILTIN_PRIM("usb-status", prim_usb_status),
    BUILTIN_PRIM("version", prim_version),
    BUILTIN_PRIM("which", prim_which),
    BUILTIN_PRIM("wlan", prim_wlan),
    BUILTIN_PRIM("wlan-set", prim_wlan_set),
    BUILTIN_PRIM("write", prim_write),
    BUILTIN_PRIM("write-file", prim_write_file),
    BUILTIN_PRIM("zero?", prim_zero_p),
};

#undef BUILTIN_PRIM
#undef BUILTIN_VAL

static lisp_val_t *builtin_get(const char *sym) {
    int low = 0;
    int high = (int)(sizeof(builtins) / sizeof(builtins[0])) - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        int cmp = strcmp(sym, builtins[mid].name);
        if (cmp == 0) {
            return (lisp_val_t *)builtins[mid].val;
        } else if (cmp < 0) {
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    return NULL;
}

#endif /* LISP_BUILTINS_TABLE_H */
