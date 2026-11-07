#include "arch/sbi.h"

sbi_ret_t sbi_ecall(unsigned long ext, unsigned long fid,
                    unsigned long arg0, unsigned long arg1,
                    unsigned long arg2, unsigned long arg3,
                    unsigned long arg4, unsigned long arg5) {
    register unsigned long a0 __asm__("a0") = arg0;
    register unsigned long a1 __asm__("a1") = arg1;
    register unsigned long a2 __asm__("a2") = arg2;
    register unsigned long a3 __asm__("a3") = arg3;
    register unsigned long a4 __asm__("a4") = arg4;
    register unsigned long a5 __asm__("a5") = arg5;
    register unsigned long a6 __asm__("a6") = fid;
    register unsigned long a7 __asm__("a7") = ext;

    __asm__ __volatile__("ecall"
                         : "+r"(a0), "+r"(a1)
                         : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                         : "memory");

    sbi_ret_t ret = { .error = (long)a0, .value = (long)a1 };
    return ret;
}

void sbi_console_putchar(int ch) {
    (void)sbi_ecall(SBI_EXT_0_1_CONSOLE_PUTCHAR, 0, (unsigned long)(unsigned char)ch,
                    0, 0, 0, 0, 0);
}

int sbi_console_getchar(void) {
    sbi_ret_t r = sbi_ecall(SBI_EXT_0_1_CONSOLE_GETCHAR, 0, 0, 0, 0, 0, 0, 0);
    return (int)r.error;
}

void sbi_set_timer(uint64_t stime_value) {
    (void)sbi_ecall(SBI_EXT_TIME, 0, (unsigned long)stime_value, 0, 0, 0, 0, 0);
}

long sbi_send_ipi(unsigned long hart_mask, unsigned long hart_mask_base) {
    sbi_ret_t r = sbi_ecall(SBI_EXT_IPI, 0, hart_mask, hart_mask_base, 0, 0, 0, 0);
    return r.error;
}

long sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque) {
    sbi_ret_t r = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START,
                            hartid, start_addr, opaque, 0, 0, 0);
    return r.error;
}

long sbi_hart_stop(void) {
    sbi_ret_t r = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_STOP, 0, 0, 0, 0, 0, 0);
    return r.error;
}

long sbi_hart_get_status(unsigned long hartid) {
    sbi_ret_t r = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_GET_STATUS,
                            hartid, 0, 0, 0, 0, 0);
    return (r.error == SBI_SUCCESS) ? r.value : r.error;
}

void sbi_system_reset(uint32_t reset_type, uint32_t reset_reason) {
    (void)sbi_ecall(SBI_EXT_SRST, SBI_EXT_SRST_RESET,
                    (unsigned long)reset_type, (unsigned long)reset_reason,
                    0, 0, 0, 0);
}
