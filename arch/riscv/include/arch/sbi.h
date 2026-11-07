#ifndef LUGALOS_ARCH_RISCV_SBI_H
#define LUGALOS_ARCH_RISCV_SBI_H

#include <stdint.h>

/* RISC-V Supervisor Binary Interface (SBI v0.2+ and legacy v0.1).
 *
 * Used on S-mode hardware targets (CONFIG_SBI) where M-mode is owned by
 * resident firmware (OpenSBI v1.0 on StarFive JH7110). Calling convention:
 *   a7 = extension ID (EID)
 *   a6 = function ID  (FID, 0 for legacy v0.1 extensions)
 *   a0..a5 = arguments
 *   returns: a0 = error code (0 = SBI_SUCCESS), a1 = return value. */

#define SBI_SUCCESS                 0L
#define SBI_ERR_FAILED             (-1L)
#define SBI_ERR_NOT_SUPPORTED      (-2L)
#define SBI_ERR_INVALID_PARAM      (-3L)
#define SBI_ERR_DENIED             (-4L)
#define SBI_ERR_INVALID_ADDRESS    (-5L)
#define SBI_ERR_ALREADY_AVAILABLE  (-6L)
#define SBI_ERR_ALREADY_STARTED    (-7L)
#define SBI_ERR_ALREADY_STOPPED    (-8L)

/* Legacy v0.1 extension IDs */
#define SBI_EXT_0_1_CONSOLE_PUTCHAR 0x01UL
#define SBI_EXT_0_1_CONSOLE_GETCHAR 0x02UL

/* Standard v0.2+ extension IDs */
#define SBI_EXT_BASE                0x10UL
#define SBI_EXT_TIME                0x54494D45UL /* "TIME" */
#define SBI_EXT_IPI                 0x735049UL   /* "sPI"  */
#define SBI_EXT_RFENCE              0x52464E43UL /* "RFNC" */
#define SBI_EXT_HSM                 0x48534DUL   /* "HSM"  */
#define SBI_EXT_SRST                0x53525354UL /* "SRST" */

/* HSM function IDs and hart states */
#define SBI_EXT_HSM_HART_START      0UL
#define SBI_EXT_HSM_HART_STOP       1UL
#define SBI_EXT_HSM_HART_GET_STATUS 2UL

#define SBI_HSM_STATE_STARTED       0L
#define SBI_HSM_STATE_STOPPED       1L
#define SBI_HSM_STATE_START_PENDING 2L
#define SBI_HSM_STATE_STOP_PENDING  3L

/* SRST function IDs, reset types, and reset reasons */
#define SBI_EXT_SRST_RESET          0UL

#define SBI_SRST_RESET_TYPE_SHUTDOWN    0U
#define SBI_SRST_RESET_TYPE_COLD_REBOOT 1U
#define SBI_SRST_RESET_TYPE_WARM_REBOOT 2U

#define SBI_SRST_RESET_REASON_NONE      0U
#define SBI_SRST_RESET_REASON_SYSFAIL   1U

typedef struct {
    long error;
    long value;
} sbi_ret_t;

sbi_ret_t sbi_ecall(unsigned long ext, unsigned long fid,
                    unsigned long arg0, unsigned long arg1,
                    unsigned long arg2, unsigned long arg3,
                    unsigned long arg4, unsigned long arg5);

void sbi_console_putchar(int ch);
int  sbi_console_getchar(void);

void sbi_set_timer(uint64_t stime_value);
long sbi_send_ipi(unsigned long hart_mask, unsigned long hart_mask_base);

long sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque);
long sbi_hart_stop(void);
long sbi_hart_get_status(unsigned long hartid);

void sbi_system_reset(uint32_t reset_type, uint32_t reset_reason);

#endif /* LUGALOS_ARCH_RISCV_SBI_H */
