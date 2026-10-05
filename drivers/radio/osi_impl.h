#ifndef LUGALOS_RADIO_OSI_IMPL_H
#define LUGALOS_RADIO_OSI_IMPL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The radio domain's side of the Wi-Fi blob's operating-system interface
 * (45.3b, plan/phase45_esp32c6.md §4.5).
 *
 * Espressif's blob reaches its OS through `wifi_osi_funcs_t`: ~127 function
 * pointers whose types are fixed by IDF's header. This is the *implementation*
 * of those entries, written in plain C types and **without** that header, so
 * that the same functions build for the QEMU test (where IDF does not exist) and
 * for the C6. The table that wires them to the blob's struct is
 * drivers/radio/esp32c6_osi_table.c, which does include the header -- so the
 * compiler checks every entry's type against Espressif's, and tools/
 * c6_blob_spike/osi_table_check.sh checks that none is missing.
 *
 * Everything here runs in U-mode, in the radio's domain. Nothing calls the
 * kernel except through `ecall` (kernel/include/kernel/kobj_abi.h), nothing
 * touches memory outside the domain, and nothing calls libc: the text is
 * placed where the domain grants execute, and a call to a function outside it
 * would fault.
 *
 * ## Where each kind of entry is served
 *
 *   in the kernel (an ecall)   semaphores, mutexes, queues, event groups, timers,
 *                              threads, time, random, the MAC, one log line,
 *                              interrupts, events -- the things the blob must not
 *                              be able to corrupt, or that need privilege
 *   in the domain (no ecall)   malloc/free (the radio's own heap), the key/value
 *                              store, formatting, is_from_isr, ticks, and every
 *                              entry that is a constant or a no-op on this chip
 *   ported from IDF            phy enable/disable, clock and reset of the modem,
 *                              country info -- the open code around the blob,
 *                              behind drivers/radio/plat.h (45.6)
 *
 * One tick is one millisecond (`task_ms_to_tick` is the identity), and
 * 0xffffffff is "block forever", matching IDF's portMAX_DELAY, so the blob's
 * timeouts pass straight through. */

/* Text and data placement. On the QEMU test these are the domain's own pages; on
 * the C6 the radio's link places its sections. Empty on the host. */
#ifndef RADIO_DATA
#define RADIO_DATA
#endif

/* Must be called once, from the radio's first thread, before anything else.
 * `arena` is the radio heap (8-byte aligned); `isr_stack_lo/hi` bound the stack
 * of the interrupt thread, which is how `is_from_isr` knows which thread it is
 * in without a system call (pass 0, 0 until that thread exists). */
bool radio_osi_init(void *arena, uint32_t arena_size);
void radio_osi_set_isr_stack(uintptr_t lo, uintptr_t hi);
/* Creates the interrupt thread (needs the kernel's ISR_WAIT/ISR_DONE: C6 with the radio). */
bool radio_osi_start_isr_thread(void);

/* The two threads the shim itself runs. Both are created by the caller
 * (typically with radio_thread_create below) and never return. */
void radio_timer_thread(void *unused);

/* ---- the entries -------------------------------------------------------- */

bool     radio_osi_env_is_chip(void);
void     radio_osi_set_intr(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio);
void     radio_osi_clear_intr(uint32_t intr_source, uint32_t intr_num);
void     radio_osi_set_isr(int32_t n, void *f, void *arg);
void     radio_osi_ints_on(uint32_t mask);
void     radio_osi_ints_off(uint32_t mask);
bool     radio_osi_is_from_isr(void);
void    *radio_osi_spin_lock_create(void);
void     radio_osi_spin_lock_delete(void *lock);
uint32_t radio_osi_wifi_int_disable(void *wifi_int_mux);
void     radio_osi_wifi_int_restore(void *wifi_int_mux, uint32_t tmp);
void     radio_osi_task_yield_from_isr(void);
void    *radio_osi_semphr_create(uint32_t max, uint32_t init);
void     radio_osi_semphr_delete(void *semphr);
int32_t  radio_osi_semphr_take(void *semphr, uint32_t block_time_tick);
int32_t  radio_osi_semphr_give(void *semphr);
void    *radio_osi_wifi_thread_semphr_get(void);
void    *radio_osi_mutex_create(void);
void    *radio_osi_recursive_mutex_create(void);
void     radio_osi_mutex_delete(void *mutex);
int32_t  radio_osi_mutex_lock(void *mutex);
int32_t  radio_osi_mutex_unlock(void *mutex);
void    *radio_osi_queue_create(uint32_t queue_len, uint32_t item_size);
void     radio_osi_queue_delete(void *queue);
int32_t  radio_osi_queue_send(void *queue, void *item, uint32_t block_time_tick);
int32_t  radio_osi_queue_send_from_isr(void *queue, void *item, void *hptw);
int32_t  radio_osi_queue_send_to_back(void *queue, void *item, uint32_t block_time_tick);
int32_t  radio_osi_queue_send_to_front(void *queue, void *item, uint32_t block_time_tick);
int32_t  radio_osi_queue_recv(void *queue, void *item, uint32_t block_time_tick);
uint32_t radio_osi_queue_msg_waiting(void *queue);
void    *radio_osi_event_group_create(void);
void     radio_osi_event_group_delete(void *event);
uint32_t radio_osi_event_group_set_bits(void *event, uint32_t bits);
uint32_t radio_osi_event_group_clear_bits(void *event, uint32_t bits);
uint32_t radio_osi_event_group_wait_bits(void *event, uint32_t bits_to_wait_for, int clear_on_exit,
                                         int wait_for_all_bits, uint32_t block_time_tick);
int32_t  radio_osi_task_create_pinned_to_core(void *task_func, const char *name, uint32_t stack_depth,
                                              void *param, uint32_t prio, void *task_handle, uint32_t core_id);
int32_t  radio_osi_task_create(void *task_func, const char *name, uint32_t stack_depth,
                               void *param, uint32_t prio, void *task_handle);
void     radio_osi_task_delete(void *task_handle);
void     radio_osi_task_delay(uint32_t tick);
int32_t  radio_osi_task_ms_to_tick(uint32_t ms);
void    *radio_osi_task_get_current_task(void);
int32_t  radio_osi_task_get_max_priority(void);
void    *radio_osi_malloc(size_t size);
void     radio_osi_free(void *p);
int32_t  radio_osi_event_post(const char *event_base, int32_t event_id, void *event_data,
                              size_t event_data_size, uint32_t ticks_to_wait);
uint32_t radio_osi_get_free_heap_size(void);
uint32_t radio_osi_rand(void);
void     radio_osi_empty(void);           /* dport stall, pm sleep lock, rtc iso, ...: nothing to do on this chip */
int      radio_osi_read_mac(uint8_t *mac, unsigned int type);
void     radio_osi_timer_arm(void *timer, uint32_t tmout, bool repeat);
void     radio_osi_timer_disarm(void *timer);
void     radio_osi_timer_done(void *ptimer);
void     radio_osi_timer_setfn(void *ptimer, void *pfunction, void *parg);
void     radio_osi_timer_arm_us(void *ptimer, uint32_t us, bool repeat);
int64_t  radio_osi_esp_timer_get_time(void);
int      radio_osi_nvs_set_i8(uint32_t handle, const char *key, int8_t value);
int      radio_osi_nvs_get_i8(uint32_t handle, const char *key, int8_t *out_value);
int      radio_osi_nvs_set_u8(uint32_t handle, const char *key, uint8_t value);
int      radio_osi_nvs_get_u8(uint32_t handle, const char *key, uint8_t *out_value);
int      radio_osi_nvs_set_u16(uint32_t handle, const char *key, uint16_t value);
int      radio_osi_nvs_get_u16(uint32_t handle, const char *key, uint16_t *out_value);
int      radio_osi_nvs_open(const char *name, unsigned int open_mode, uint32_t *out_handle);
void     radio_osi_nvs_close(uint32_t handle);
int      radio_osi_nvs_commit(uint32_t handle);
int      radio_osi_nvs_set_blob(uint32_t handle, const char *key, const void *value, size_t length);
int      radio_osi_nvs_get_blob(uint32_t handle, const char *key, void *out_value, size_t *length);
int      radio_osi_nvs_erase_key(uint32_t handle, const char *key);
int      radio_osi_get_random(uint8_t *buf, size_t len);
int      radio_osi_get_time(void *t);
unsigned long radio_osi_random(void);
uint32_t radio_osi_slowclk_cal_get(void);
void     radio_osi_log_write(unsigned int level, const char *tag, const char *format, ...);
void     radio_osi_log_writev(unsigned int level, const char *tag, const char *format, va_list args);
uint32_t radio_osi_log_timestamp(void);
void    *radio_osi_realloc(void *ptr, size_t size);
void    *radio_osi_calloc(size_t n, size_t size);
void    *radio_osi_zalloc(size_t size);
void    *radio_osi_wifi_create_queue(int queue_len, int item_size);
void     radio_osi_wifi_delete_queue(void *queue);
int32_t  radio_osi_one_i32(void);         /* sleep-retention attach/detach: "nothing to do", success */
bool     radio_osi_false(void);
void     radio_osi_regdma_noop(void *addr, uint32_t value, uint32_t mask);
void    *radio_osi_find_link_null(int id);
/* The coexistence library's second table. */
int32_t  radio_osi_semphr_take_from_isr(void *semphr, void *hptw);
int32_t  radio_osi_semphr_give_from_isr(void *semphr, void *hptw);
int      radio_osi_is_in_isr(void);
int      radio_osi_xtal_freq_get(void);
int      radio_osi_debug_matrix_init(int event, int signal, bool rev);

/* Spawns a thread in this domain (KOBJ_OP_THREAD_CREATE) with a stack from the
 * radio heap; returns the pid or a negative KO_* code. */
int32_t radio_thread_create(void (*entry)(void *), void *param, uint32_t stack_bytes, uint32_t prio);

#endif /* LUGALOS_RADIO_OSI_IMPL_H */
