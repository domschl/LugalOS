/* The two operating-system tables the ESP32-C6 Wi-Fi blob is built against,
 * filled with the radio domain's implementations (45.3b,
 * plan/phase45_esp32c6.md §4.5).
 *
 * **This is the only file in the radio shim that includes Espressif's headers**,
 * and that is deliberate: every initialiser below is assigned to a field of
 * IDF's own struct, so the compiler checks each entry's type against the
 * header the blob was built with, and IDF's wifi_init.c checks the layout's md5
 * against the blob's at run time (esp_wifi_internal_osi_funcs_md5_check).
 * tools/c6_blob_spike/osi_table_check.sh compiles this file against an IDF tree
 * and verifies that every field of both structs is assigned or is knowingly
 * absent.
 *
 * It mirrors IDF's esp32c6/esp_adapter.c and esp32c6/esp_coex_adapter.c field
 * for field. Where IDF's open code does something this does not -- the PHY and
 * clock entries -- the radio_plat_* seam stands in (drivers/radio/plat.h).
 *
 * The coexistence entries are thin calls into the blob's own libcoexist; they
 * are the same wrappers IDF has, with CONFIG_SW_COEXIST_ENABLE taken as set
 * (the default on this chip). Built only for the C6: it needs the IDF include
 * path and the blob to link against. */

#include "radio_redirect.h"
#include "osi_impl.h"
#include "plat.h"

#include "esp_private/wifi_os_adapter.h"
#include "private/esp_coexist_adapter.h"
#include "private/esp_coexist_internal.h"

/* ---- coexistence, into libcoexist ---------------------------------------- */

static int coex_init_w(void)                       { return coex_init(); }
static void coex_deinit_w(void)                    { coex_deinit(); }
static int coex_enable_w(void)                     { return coex_enable(); }
static void coex_disable_w(void)                   { coex_disable(); }
static uint32_t coex_status_get_w(void)            { return coex_status_get(COEX_STATUS_GET_WIFI_BITMAP); }
static int coex_wifi_request_w(uint32_t event, uint32_t latency, uint32_t duration) { return coex_wifi_request(event, latency, duration); }
static int coex_wifi_release_w(uint32_t event)     { return coex_wifi_release(event); }
static int coex_wifi_channel_set_w(uint8_t primary, uint8_t secondary) { return coex_wifi_channel_set(primary, secondary); }
static int coex_event_duration_get_w(uint32_t event, uint32_t *duration) { return coex_event_duration_get(event, duration); }
static int coex_pti_get_w(uint32_t event, uint8_t *pti) { return coex_pti_get(event, pti); }
static void coex_schm_status_bit_clear_w(uint32_t type, uint32_t status) { coex_schm_status_bit_clear(type, status); }
static void coex_schm_status_bit_set_w(uint32_t type, uint32_t status)   { coex_schm_status_bit_set(type, status); }
static int coex_schm_interval_set_w(uint32_t interval) { return coex_schm_interval_set(interval); }
static uint32_t coex_schm_interval_get_w(void)     { return coex_schm_interval_get(); }
static uint8_t coex_schm_curr_period_get_w(void)   { return coex_schm_curr_period_get(); }
static void *coex_schm_curr_phase_get_w(void)      { return coex_schm_curr_phase_get(); }
static int coex_register_start_cb_w(int (*cb)(void)) { return coex_register_start_cb(cb); }
static int coex_schm_process_restart_w(void)       { return coex_schm_process_restart(); }
static int coex_schm_register_cb_w(int type, int (*cb)(int)) { return coex_schm_register_callback(type, (void *)(uintptr_t)cb); }
/* Power management (CONFIG_ESP_COEX_POWER_MANAGEMENT) is off for this radio, and IDF's
 * own wrappers answer exactly this when it is. */
static int coex_schm_flexible_period_set_w(uint8_t period) { (void)period; return 0; }
static uint8_t coex_schm_flexible_period_get_w(void) { return 1; }
static void *coex_schm_get_phase_by_idx_w(int phase_idx) { return coex_schm_get_phase_by_idx(phase_idx); }

/* ---- the Wi-Fi table ------------------------------------------------------ */

wifi_osi_funcs_t g_wifi_osi_funcs = {
    ._version = ESP_WIFI_OS_ADAPTER_VERSION,
    ._env_is_chip = radio_osi_env_is_chip,
    ._set_intr = radio_osi_set_intr,
    ._clear_intr = radio_osi_clear_intr,
    ._set_isr = radio_osi_set_isr,
    ._ints_on = radio_osi_ints_on,
    ._ints_off = radio_osi_ints_off,
    ._is_from_isr = radio_osi_is_from_isr,
    ._spin_lock_create = radio_osi_spin_lock_create,
    ._spin_lock_delete = radio_osi_spin_lock_delete,
    ._wifi_int_disable = radio_osi_wifi_int_disable,
    ._wifi_int_restore = radio_osi_wifi_int_restore,
    ._task_yield_from_isr = radio_osi_task_yield_from_isr,
    ._semphr_create = radio_osi_semphr_create,
    ._semphr_delete = radio_osi_semphr_delete,
    ._semphr_take = radio_osi_semphr_take,
    ._semphr_give = radio_osi_semphr_give,
    ._wifi_thread_semphr_get = radio_osi_wifi_thread_semphr_get,
    ._mutex_create = radio_osi_mutex_create,
    ._recursive_mutex_create = radio_osi_recursive_mutex_create,
    ._mutex_delete = radio_osi_mutex_delete,
    ._mutex_lock = radio_osi_mutex_lock,
    ._mutex_unlock = radio_osi_mutex_unlock,
    ._queue_create = radio_osi_queue_create,
    ._queue_delete = radio_osi_queue_delete,
    ._queue_send = radio_osi_queue_send,
    ._queue_send_from_isr = radio_osi_queue_send_from_isr,
    ._queue_send_to_back = radio_osi_queue_send_to_back,
    ._queue_send_to_front = radio_osi_queue_send_to_front,
    ._queue_recv = radio_osi_queue_recv,
    ._queue_msg_waiting = radio_osi_queue_msg_waiting,
    ._event_group_create = radio_osi_event_group_create,
    ._event_group_delete = radio_osi_event_group_delete,
    ._event_group_set_bits = radio_osi_event_group_set_bits,
    ._event_group_clear_bits = radio_osi_event_group_clear_bits,
    ._event_group_wait_bits = radio_osi_event_group_wait_bits,
    ._task_create_pinned_to_core = radio_osi_task_create_pinned_to_core,
    ._task_create = radio_osi_task_create,
    ._task_delete = radio_osi_task_delete,
    ._task_delay = radio_osi_task_delay,
    ._task_ms_to_tick = radio_osi_task_ms_to_tick,
    ._task_get_current_task = radio_osi_task_get_current_task,
    ._task_get_max_priority = radio_osi_task_get_max_priority,
    ._malloc = radio_osi_malloc,
    ._free = radio_osi_free,
    ._event_post = radio_osi_event_post,
    ._get_free_heap_size = radio_osi_get_free_heap_size,
    ._rand = radio_osi_rand,
    ._dport_access_stall_other_cpu_start_wrap = radio_osi_empty,
    ._dport_access_stall_other_cpu_end_wrap = radio_osi_empty,
    ._wifi_pm_sleep_lock_acquire = radio_osi_empty,
    ._wifi_pm_sleep_lock_release = radio_osi_empty,
    ._phy_disable = radio_plat_phy_disable,
    ._phy_enable = radio_plat_phy_enable,
    ._phy_update_country_info = radio_plat_phy_update_country,
    ._read_mac = radio_osi_read_mac,
    ._timer_arm = radio_osi_timer_arm,
    ._timer_disarm = radio_osi_timer_disarm,
    ._timer_done = radio_osi_timer_done,
    ._timer_setfn = radio_osi_timer_setfn,
    ._timer_arm_us = radio_osi_timer_arm_us,
    ._wifi_reset_mac = radio_plat_wifi_reset_mac,
    ._wifi_clock_enable = radio_plat_wifi_clock_enable,
    ._wifi_clock_disable = radio_plat_wifi_clock_disable,
    ._wifi_rtc_enable_iso = radio_osi_empty,
    ._wifi_rtc_disable_iso = radio_osi_empty,
    ._esp_timer_get_time = radio_osi_esp_timer_get_time,
    ._nvs_set_i8 = radio_osi_nvs_set_i8,
    ._nvs_get_i8 = radio_osi_nvs_get_i8,
    ._nvs_set_u8 = radio_osi_nvs_set_u8,
    ._nvs_get_u8 = radio_osi_nvs_get_u8,
    ._nvs_set_u16 = radio_osi_nvs_set_u16,
    ._nvs_get_u16 = radio_osi_nvs_get_u16,
    ._nvs_open = radio_osi_nvs_open,
    ._nvs_close = radio_osi_nvs_close,
    ._nvs_commit = radio_osi_nvs_commit,
    ._nvs_set_blob = radio_osi_nvs_set_blob,
    ._nvs_get_blob = radio_osi_nvs_get_blob,
    ._nvs_erase_key = radio_osi_nvs_erase_key,
    ._get_random = radio_osi_get_random,
    ._get_time = radio_osi_get_time,
    ._random = radio_osi_random,
    ._slowclk_cal_get = radio_osi_slowclk_cal_get,
    ._log_write = radio_osi_log_write,
    ._log_writev = radio_osi_log_writev,
    ._log_timestamp = radio_osi_log_timestamp,
    ._malloc_internal = radio_osi_malloc,
    ._realloc_internal = radio_osi_realloc,
    ._calloc_internal = radio_osi_calloc,
    ._zalloc_internal = radio_osi_zalloc,
    ._wifi_malloc = radio_osi_malloc,
    ._wifi_realloc = radio_osi_realloc,
    ._wifi_calloc = radio_osi_calloc,
    ._wifi_zalloc = radio_osi_zalloc,
    ._wifi_create_queue = radio_osi_wifi_create_queue,
    ._wifi_delete_queue = radio_osi_wifi_delete_queue,
    ._coex_init = coex_init_w,
    ._coex_deinit = coex_deinit_w,
    ._coex_enable = coex_enable_w,
    ._coex_disable = coex_disable_w,
    ._coex_status_get = coex_status_get_w,
    /* ._coex_condition_set: IDF leaves it NULL, and the blob does not call it. */
    ._coex_wifi_request = coex_wifi_request_w,
    ._coex_wifi_release = coex_wifi_release_w,
    ._coex_wifi_channel_set = coex_wifi_channel_set_w,
    ._coex_event_duration_get = coex_event_duration_get_w,
    ._coex_pti_get = coex_pti_get_w,
    ._coex_schm_status_bit_clear = coex_schm_status_bit_clear_w,
    ._coex_schm_status_bit_set = coex_schm_status_bit_set_w,
    ._coex_schm_interval_set = coex_schm_interval_set_w,
    ._coex_schm_interval_get = coex_schm_interval_get_w,
    ._coex_schm_curr_period_get = coex_schm_curr_period_get_w,
    ._coex_schm_curr_phase_get = coex_schm_curr_phase_get_w,
    ._coex_register_start_cb = coex_register_start_cb_w,
    ._regdma_link_set_write_wait_content = radio_osi_regdma_noop,
    ._sleep_retention_find_link_by_id = radio_osi_find_link_null,
    ._coex_schm_process_restart = coex_schm_process_restart_w,
    ._coex_schm_register_cb = coex_schm_register_cb_w,
    ._coex_schm_flexible_period_set = coex_schm_flexible_period_set_w,
    ._coex_schm_flexible_period_get = coex_schm_flexible_period_get_w,
    ._coex_schm_get_phase_by_idx = coex_schm_get_phase_by_idx_w,
    ._wifi_disable_ac_ax = radio_osi_false,
    ._wifi_bb_sleep_retention_attach = radio_osi_one_i32,
    ._wifi_bb_sleep_retention_detach = radio_osi_one_i32,
    ._wifi_mac_sleep_retention_attach = radio_osi_one_i32,
    ._wifi_mac_sleep_retention_detach = radio_osi_one_i32,
    ._magic = ESP_WIFI_OS_ADAPTER_MAGIC,
};

/* ---- the coexistence library's table -------------------------------------- */

coex_adapter_funcs_t g_coex_adapter_funcs = {
    ._version = COEX_ADAPTER_VERSION,
    ._task_yield_from_isr = radio_osi_task_yield_from_isr,
    ._semphr_create = radio_osi_semphr_create,
    ._semphr_delete = radio_osi_semphr_delete,
    ._semphr_take_from_isr = radio_osi_semphr_take_from_isr,
    ._semphr_give_from_isr = radio_osi_semphr_give_from_isr,
    ._semphr_take = radio_osi_semphr_take,
    ._semphr_give = radio_osi_semphr_give,
    ._is_in_isr = radio_osi_is_in_isr,
    ._malloc_internal = radio_osi_malloc,
    ._free = radio_osi_free,
    ._esp_timer_get_time = radio_osi_esp_timer_get_time,
    ._env_is_chip = radio_osi_env_is_chip,
    ._timer_disarm = radio_osi_timer_disarm,
    ._timer_done = radio_osi_timer_done,
    ._timer_setfn = radio_osi_timer_setfn,
    ._timer_arm_us = radio_osi_timer_arm_us,
    ._debug_matrix_init = radio_osi_debug_matrix_init,
    ._xtal_freq_get = radio_osi_xtal_freq_get,
    ._magic = COEX_ADAPTER_MAGIC,
};
