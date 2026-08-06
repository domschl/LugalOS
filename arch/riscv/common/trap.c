#include "arch/trap.h"
#include "arch/csr.h"
#include "kernel/printk.h"
#include "kernel/ipc.h"
#include "fs/vfs.h"
#include "drivers/uart.h"

#if defined(CONFIG_BOARD_RP2350)
#include "drivers/usb_cdc.h"
#define REG(addr) (*(volatile uint32_t *)(addr))
#endif

void trap_init(void) {
    /* Enable M-mode External Interrupts (MEIE bit 11 in mie) */
    uintptr_t mie_val;
    __asm__ __volatile__("csrr %0, mie" : "=r"(mie_val));
    mie_val |= (1u << 11);
    __asm__ __volatile__("csrw mie, %0" :: "r"(mie_val));

    /* Enable M-mode Global Interrupts (MIE bit 3 in mstatus) */
    uintptr_t mstatus_val;
    __asm__ __volatile__("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val |= (1u << 3);
    __asm__ __volatile__("csrw mstatus, %0" :: "r"(mstatus_val));
}

void trap_handler(trap_frame_t *frame) {
    uintptr_t cause = frame->cause;
    uintptr_t is_interrupt = cause & ((uintptr_t)1 << (__riscv_xlen - 1));
    uintptr_t code = cause & ~((uintptr_t)1 << (__riscv_xlen - 1));

    if (is_interrupt) {
        if (code == 11) { // Machine External Interrupt
#if defined(CONFIG_BOARD_RP2350)
            REG(0xE000E280) = (1u << 14); // Clear pending IRQ 14 (USBCTRL_IRQ) in NVIC ICPR
            usb_cdc_task();
#endif
        } else {
            printk("\n[Trap] Interrupt received: code 0x%lx\n", (unsigned long)code);
        }
    } else {
        /* If ecall (Environment Call from U-mode, S-mode, or M-mode) */
        if (code == 8 || code == 9 || code == 11) {
            uintptr_t sys_nr = frame->a0;
            int target_pid = (int)frame->a1;
            ipc_msg_t *msg_in = (ipc_msg_t *)frame->a2;
            ipc_msg_t *msg_out = (ipc_msg_t *)frame->a3;

            long ret = 0;
            switch (sys_nr) {
                case 0: /* SYS_EXIT / yield */
                    ret = 0;
                    break;
                case SYS_IPC_CALL:
                    ret = sys_ipc_call(target_pid, msg_in, msg_out);
                    break;
                case SYS_IPC_REPLY:
                    ret = sys_ipc_reply(target_pid, msg_in);
                    break;
                case SYS_IPC_SEND:
                    ret = sys_ipc_send(target_pid, msg_in);
                    break;
                case SYS_IPC_RECV:
                    ret = sys_ipc_recv(target_pid, msg_in);
                    break;
                case 10: /* SYS_PRINT */
                    if (frame->a1) printk("%s", (const char *)frame->a1);
                    ret = 0;
                    break;
                case 11: /* SYS_PUTNUM */
                    printk("%ld", (long)frame->a1);
                    ret = 0;
                    break;
                case 12: /* SYS_PUTCHAR */
                    uart_putc((char)frame->a1);
                    ret = 0;
                    break;
                case 13: /* SYS_READ_FILE: vfs_read(path, buf, max_len) */
                    ret = vfs_read((const char *)frame->a1, (void *)frame->a2, (uint32_t)frame->a3);
                    break;
                case 14: /* SYS_WRITE_FILE: vfs_write(path, buf, len) */
                    ret = vfs_write((const char *)frame->a1, (const void *)frame->a2, (uint32_t)frame->a3);
                    break;
                default:
                    printk("[Syscall] Unknown syscall nr %ld requested\n", (long)sys_nr);
                    ret = -1;
                    break;
            }

            frame->a0 = (uintptr_t)ret;
            /* Advance EPC past 4-byte ecall instruction */
            frame->epc += 4;
            return;
        }

        /* Fatal exception hang */
        uint32_t inst_val = 0;
        if (frame->epc >= 0x10000000 && frame->epc < 0x20082000) {
            const uint8_t *p = (const uint8_t *)frame->epc;
            inst_val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        printk("\n[Trap Exception] Cause: 0x%lx, epc=0x%lx, tval=0x%lx, inst=0x%08x\n",
               (unsigned long)code, (unsigned long)frame->epc, (unsigned long)frame->tval, (unsigned int)inst_val);
        printk("[Trap Register Dump] a0=0x%lx, a1=0x%lx, sp=0x%lx, ra=0x%lx\n",
               (unsigned long)frame->a0, (unsigned long)frame->a1, (unsigned long)frame->sp, (unsigned long)frame->ra);
        printk("[Fatal] System halted due to unhandled exception.\n");
        while (1) {
            __asm__ __volatile__("wfi");
        }
    }
}
