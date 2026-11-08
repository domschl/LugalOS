#include "drivers/nvme.h"
#include "drivers/pcie_jh7110.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "kernel/lock.h"
#include <string.h>

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG64(addr) (*(volatile uint64_t *)(uintptr_t)(addr))

/* SiFive L2 Cache Controller Flush64 register (JH7110 non-coherent DMA) */
#define SIFIVE_L2_BASE      0x02010000UL
#define SIFIVE_L2_FLUSH64   0x200UL

static inline void nvme_mb(void) {
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

static inline void nvme_flush_l2_range(uintptr_t pa_start, uintptr_t pa_end) {
    volatile uint64_t *flush64 = (volatile uint64_t *)(SIFIVE_L2_BASE + SIFIVE_L2_FLUSH64);
    nvme_mb();
    uintptr_t start = pa_start & ~(64UL - 1UL);
    for (uintptr_t line = start; line < pa_end; line += 64UL) {
        *flush64 = line;
        nvme_mb();
    }
}

/* Static Queue and Scratch Buffers (4096-byte aligned in .bss) */
/* guarded by: g_nvme_lock for hardware queue operations and scratch buffers */
static nvme_sqe_t g_admin_sq[NVME_ADMIN_Q_SIZE] __attribute__((aligned(4096)));
static nvme_cqe_t g_admin_cq[NVME_ADMIN_Q_SIZE] __attribute__((aligned(4096)));
static nvme_sqe_t g_io_sq[NVME_IO_Q_SIZE]       __attribute__((aligned(4096)));
static nvme_cqe_t g_io_cq[NVME_IO_Q_SIZE]       __attribute__((aligned(4096)));
static uint64_t   g_prp_list[512]               __attribute__((aligned(4096)));
static uint8_t    g_admin_buf[4096]             __attribute__((aligned(4096)));

/* Controller and Namespace State */
/* guarded by: g_nvme_lock for all hardware queue operations and state updates */
static ylock_t   g_nvme_lock;
static uintptr_t g_nvme_bar0 = 0;
static uint32_t  g_dstrd = 0;
static uint32_t  g_admin_sq_tail = 0;
static uint32_t  g_admin_cq_head = 0;
static uint8_t   g_admin_cq_phase = 1;
static uint32_t  g_io_sq_tail = 0;
static uint32_t  g_io_cq_head = 0;
static uint8_t   g_io_cq_phase = 1;
static uint16_t  g_cmd_id = 0;
static bool      g_nvme_present = false;
static uint32_t  g_sector_size = 512;
static uint64_t  g_total_sectors = 0;
static char      g_model[41];
static char      g_serial[21];
static char      g_firmware[9];

static inline uint32_t nvme_sq_doorbell_offset(uint32_t qid) {
    return NVME_REG_DBS + (2u * qid) * (4u << g_dstrd);
}

static inline uint32_t nvme_cq_doorbell_offset(uint32_t qid) {
    return NVME_REG_DBS + (2u * qid + 1u) * (4u << g_dstrd);
}

static int nvme_admin_submit_sync(nvme_sqe_t *sqe, nvme_cqe_t *cqe_out) {
    uint32_t sq_idx = g_admin_sq_tail;
    sqe->cid = ++g_cmd_id;

    /* Copy SQE into submission queue */
    memcpy(&g_admin_sq[sq_idx], sqe, sizeof(nvme_sqe_t));
    nvme_flush_l2_range((uintptr_t)&g_admin_sq[sq_idx],
                        (uintptr_t)(&g_admin_sq[sq_idx] + 1));

    /* Advance submission tail and ring SQ doorbell */
    g_admin_sq_tail = (sq_idx + 1u) % NVME_ADMIN_Q_SIZE;
    REG32(g_nvme_bar0 + nvme_sq_doorbell_offset(0)) = g_admin_sq_tail;

    /* Poll completion queue for Phase bit match */
    uint64_t timeout_us = time_get_us() + 2000000ULL;
    volatile nvme_cqe_t *cqe = &g_admin_cq[g_admin_cq_head];

    while (true) {
        nvme_flush_l2_range((uintptr_t)cqe, (uintptr_t)(cqe + 1));
        if ((cqe->status & 1u) == g_admin_cq_phase) {
            break;
        }
        if (time_get_us() > timeout_us) {
            printk("[NVMe] Admin command 0x%02x timed out!\n", sqe->opcode);
            return -1;
        }
        time_delay_us(1);
    }

    if (cqe_out) {
        memcpy(cqe_out, (const void *)cqe, sizeof(nvme_cqe_t));
    }

    uint16_t status = (cqe->status >> 1) & 0x7FFFu;

    /* Advance completion head */
    g_admin_cq_head++;
    if (g_admin_cq_head == NVME_ADMIN_Q_SIZE) {
        g_admin_cq_head = 0;
        g_admin_cq_phase ^= 1u;
    }
    REG32(g_nvme_bar0 + nvme_cq_doorbell_offset(0)) = g_admin_cq_head;

    if (status != 0) {
        printk("[NVMe] Admin command 0x%02x failed (status 0x%04x)\n", sqe->opcode, status);
        return -(int)status;
    }

    return 0;
}

static int nvme_io_submit_sync(nvme_sqe_t *sqe, nvme_cqe_t *cqe_out) {
    uint32_t sq_idx = g_io_sq_tail;
    sqe->cid = ++g_cmd_id;

    memcpy(&g_io_sq[sq_idx], sqe, sizeof(nvme_sqe_t));
    nvme_flush_l2_range((uintptr_t)&g_io_sq[sq_idx],
                        (uintptr_t)(&g_io_sq[sq_idx] + 1));

    g_io_sq_tail = (sq_idx + 1u) % NVME_IO_Q_SIZE;
    uint32_t dblr = nvme_sq_doorbell_offset(1);
    nvme_mb();
    REG32(g_nvme_bar0 + dblr) = g_io_sq_tail;
    nvme_mb();

    uint64_t timeout_us = time_get_us() + 5000000ULL;
    volatile nvme_cqe_t *cqe = &g_io_cq[g_io_cq_head];

    while (true) {
        nvme_flush_l2_range((uintptr_t)cqe, (uintptr_t)(cqe + 1));
        if ((cqe->status & 1u) == g_io_cq_phase) {
            break;
        }
        if (time_get_us() > timeout_us) {
            printk("[NVMe] I/O command 0x%02x timed out!\n", sqe->opcode);
            return -1;
        }
        time_delay_us(1);
    }

    if (cqe_out) {
        memcpy(cqe_out, (const void *)cqe, sizeof(nvme_cqe_t));
    }

    uint16_t status = (cqe->status >> 1) & 0x7FFFu;

    g_io_cq_head++;
    if (g_io_cq_head == NVME_IO_Q_SIZE) {
        g_io_cq_head = 0;
        g_io_cq_phase ^= 1u;
    }
    REG32(g_nvme_bar0 + nvme_cq_doorbell_offset(1)) = g_io_cq_head;

    if (status != 0) {
        printk("[NVMe] I/O command 0x%02x failed (status 0x%04x)\n", sqe->opcode, status);
        return -(int)status;
    }

    return 0;
}

static void trim_trailing_spaces(char *str, int len) {
    while (len > 0 && str[len - 1] == ' ') {
        str[len - 1] = '\0';
        len--;
    }
}

int nvme_init(void) {
    if (g_nvme_present) return 0;

    ylock_init(&g_nvme_lock);
    ylock_acquire(&g_nvme_lock);

    /* 1. Locate NVMe controller on PCIe buses (Class 0x01, Subclass 0x08) */
    const pci_device_t *pdev = jh7110_pcie_find_class(0x01, 0x08);
    if (!pdev) {
        ylock_release(&g_nvme_lock);
        return -1;
    }

    if (pdev->bars[0].size == 0) {
        printk("[NVMe] Error: Controller BAR0 unallocated\n");
        ylock_release(&g_nvme_lock);
        return -1;
    }

    g_nvme_bar0 = (uintptr_t)pdev->bars[0].base;

    /* 2. Read Controller Capabilities (CAP) */
    uint32_t cap_lo = REG32(g_nvme_bar0 + NVME_REG_CAP_LO);
    uint32_t cap_hi = REG32(g_nvme_bar0 + NVME_REG_CAP_HI);
    uint32_t vs     = REG32(g_nvme_bar0 + NVME_REG_VS);
    printk("[NVMe] Probing BAR0=0x%lx: CAP_LO=0x%08x CAP_HI=0x%08x VS=0x%08x\n",
           (unsigned long)g_nvme_bar0, cap_lo, cap_hi, vs);
    uint32_t to_ms  = ((cap_lo >> 24) & 0xFFu) * 500u;
    if (to_ms < 5000u) to_ms = 5000u;
    g_dstrd = cap_hi & 0x0Fu;

    /* 3. Disable controller if already enabled */
    uint32_t csts = REG32(g_nvme_bar0 + NVME_REG_CSTS);
    if (csts & NVME_CSTS_RDY) {
        REG32(g_nvme_bar0 + NVME_REG_CC) &= ~NVME_CC_EN;
        uint64_t t0 = time_get_us();
        while (REG32(g_nvme_bar0 + NVME_REG_CSTS) & NVME_CSTS_RDY) {
            if (time_get_us() - t0 > (uint64_t)to_ms * 1000u) {
                printk("[NVMe] Timeout waiting for controller reset\n");
                ylock_release(&g_nvme_lock);
                return -1;
            }
            time_delay_us(100);
        }
    }

    /* 4. Configure Admin Queue Attributes (AQA) and Queue Bases */
    uint32_t aqa = ((NVME_ADMIN_Q_SIZE - 1u) << 16) | (NVME_ADMIN_Q_SIZE - 1u);
    REG32(g_nvme_bar0 + NVME_REG_AQA) = aqa;

    uintptr_t asq_phys = (uintptr_t)&g_admin_sq[0];
    uintptr_t acq_phys = (uintptr_t)&g_admin_cq[0];

    memset(g_admin_sq, 0, sizeof(g_admin_sq));
    memset(g_admin_cq, 0, sizeof(g_admin_cq));
    nvme_flush_l2_range(asq_phys, asq_phys + sizeof(g_admin_sq));
    nvme_flush_l2_range(acq_phys, acq_phys + sizeof(g_admin_cq));

    REG32(g_nvme_bar0 + NVME_REG_ASQ_LO) = (uint32_t)asq_phys;
    REG32(g_nvme_bar0 + NVME_REG_ASQ_HI) = (uint32_t)(asq_phys >> 32);
    REG32(g_nvme_bar0 + NVME_REG_ACQ_LO) = (uint32_t)acq_phys;
    REG32(g_nvme_bar0 + NVME_REG_ACQ_HI) = (uint32_t)(acq_phys >> 32);

    g_admin_sq_tail = 0;
    g_admin_cq_head = 0;
    g_admin_cq_phase = 1;

    /* 5. Enable Controller in CC (64-byte SQE, 16-byte CQE, 4 KB page size, NVM CSS) */
    uint32_t cc = NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16 | NVME_CC_MPS_4K | NVME_CC_CSS_NVM | NVME_CC_EN;
    REG32(g_nvme_bar0 + NVME_REG_CC) = cc;

    /* 6. Wait for CSTS.RDY == 1 */
    uint64_t t_rdy = time_get_us();
    while ((REG32(g_nvme_bar0 + NVME_REG_CSTS) & NVME_CSTS_RDY) == 0) {
        if (REG32(g_nvme_bar0 + NVME_REG_CSTS) & NVME_CSTS_CFS) {
            printk("[NVMe] Fatal controller status detected during enable!\n");
            ylock_release(&g_nvme_lock);
            return -1;
        }
        if (time_get_us() - t_rdy > (uint64_t)to_ms * 1000u) {
            uint32_t cur_csts = REG32(g_nvme_bar0 + NVME_REG_CSTS);
            uint32_t cur_cc = REG32(g_nvme_bar0 + NVME_REG_CC);
            printk("[NVMe] Timeout waiting for CSTS.RDY (CC=0x%08x, CSTS=0x%08x, BAR0=0x%lx)\n",
                   cur_cc, cur_csts, (unsigned long)g_nvme_bar0);
            ylock_release(&g_nvme_lock);
            return -1;
        }
        time_delay_us(100);
    }

    /* 7. Identify Controller */
    memset(g_admin_buf, 0, 4096);
    nvme_flush_l2_range((uintptr_t)g_admin_buf, (uintptr_t)g_admin_buf + 4096);

    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_IDENTIFY;
    sqe.nsid = 0;
    sqe.prp1 = (uintptr_t)g_admin_buf;
    sqe.cdw10 = 1u; /* CNS = 1: Identify Controller */

    if (nvme_admin_submit_sync(&sqe, NULL) != 0) {
        printk("[NVMe] Failed to Identify Controller\n");
        ylock_release(&g_nvme_lock);
        return -1;
    }

    nvme_flush_l2_range((uintptr_t)g_admin_buf, (uintptr_t)g_admin_buf + 4096);
    nvme_id_ctrl_t *id_ctrl = (nvme_id_ctrl_t *)g_admin_buf;

    memcpy(g_serial, id_ctrl->sn, 20);
    g_serial[20] = '\0';
    trim_trailing_spaces(g_serial, 20);

    memcpy(g_model, id_ctrl->mn, 40);
    g_model[40] = '\0';
    trim_trailing_spaces(g_model, 40);

    memcpy(g_firmware, id_ctrl->fr, 8);
    g_firmware[8] = '\0';
    trim_trailing_spaces(g_firmware, 8);

    /* 8. Identify Namespace 1 */
    memset(g_admin_buf, 0, 4096);
    nvme_flush_l2_range((uintptr_t)g_admin_buf, (uintptr_t)g_admin_buf + 4096);

    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_IDENTIFY;
    sqe.nsid = 1u;  /* Namespace 1 */
    sqe.prp1 = (uintptr_t)g_admin_buf;
    sqe.cdw10 = 0u; /* CNS = 0: Identify Namespace */

    if (nvme_admin_submit_sync(&sqe, NULL) != 0) {
        printk("[NVMe] Failed to Identify Namespace 1\n");
        ylock_release(&g_nvme_lock);
        return -1;
    }

    nvme_flush_l2_range((uintptr_t)g_admin_buf, (uintptr_t)g_admin_buf + 4096);
    nvme_id_ns_t *id_ns = (nvme_id_ns_t *)g_admin_buf;

    g_total_sectors = id_ns->nsze;
    uint8_t flbas_idx = id_ns->flbas & 0x0Fu;
    uint8_t lbads = id_ns->lbaf[flbas_idx].ds;
    g_sector_size = 1u << lbads;
    if (g_sector_size == 0) g_sector_size = 512;

    /* 9. Create I/O Completion Queue (QID = 1) */
    uintptr_t io_cq_phys = (uintptr_t)&g_io_cq[0];
    memset(g_io_cq, 0, sizeof(g_io_cq));
    nvme_flush_l2_range(io_cq_phys, io_cq_phys + sizeof(g_io_cq));

    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_CREATE_CQ;
    sqe.prp1 = io_cq_phys;
    sqe.cdw10 = ((NVME_IO_Q_SIZE - 1u) << 16) | 1u; /* Size = 63 (64 entries), QID = 1 */
    sqe.cdw11 = 1u;                                 /* PC = 1 (Contiguous), IEN = 0 */

    if (nvme_admin_submit_sync(&sqe, NULL) != 0) {
        printk("[NVMe] Failed to create I/O Completion Queue 1\n");
        ylock_release(&g_nvme_lock);
        return -1;
    }

    /* 10. Create I/O Submission Queue (QID = 1, associated with CQ 1) */
    uintptr_t io_sq_phys = (uintptr_t)&g_io_sq[0];
    memset(g_io_sq, 0, sizeof(g_io_sq));
    nvme_flush_l2_range(io_sq_phys, io_sq_phys + sizeof(g_io_sq));

    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_CREATE_SQ;
    sqe.prp1 = io_sq_phys;
    sqe.cdw10 = ((NVME_IO_Q_SIZE - 1u) << 16) | 1u; /* Size = 63 (64 entries), QID = 1 */
    sqe.cdw11 = (1u << 16) | 1u;                    /* CQID = 1, PC = 1 (Contiguous) */

    if (nvme_admin_submit_sync(&sqe, NULL) != 0) {
        printk("[NVMe] Failed to create I/O Submission Queue 1\n");
        ylock_release(&g_nvme_lock);
        return -1;
    }

    g_io_sq_tail = 0;
    g_io_cq_head = 0;
    g_io_cq_phase = 1;
    g_nvme_present = true;

    uint32_t size_mb = (uint32_t)((g_total_sectors * g_sector_size) / (1024ULL * 1024ULL));
    printk("[NVMe] Ready: %s (SN: %s, FW: %s), %u MB (%llu sectors of %u B)\n",
           g_model, g_serial, g_firmware, size_mb,
           (unsigned long long)g_total_sectors, g_sector_size);

    ylock_release(&g_nvme_lock);
    return 0;
}

bool nvme_is_present(void) {
    return g_nvme_present;
}

static int nvme_read_blocks_impl(uint32_t lba, uint32_t count, void *buf) {
    if (!g_nvme_present || count == 0) return -1;

    uintptr_t p = (uintptr_t)buf;
    if ((p & 4095UL) != 0) {
        /* Bounce unaligned / stack buffers through 4KB-aligned g_admin_buf */
        uint8_t *dst = (uint8_t *)buf;
        uint32_t sectors_per_bounce = 4096u / g_sector_size;
        while (count > 0) {
            uint32_t batch = (count < sectors_per_bounce) ? count : sectors_per_bounce;
            if (nvme_read_blocks_impl(lba, batch, g_admin_buf) != 0) {
                return -1;
            }
            memcpy(dst, g_admin_buf, batch * g_sector_size);
            dst += batch * g_sector_size;
            lba += batch;
            count -= batch;
        }
        return 0;
    }

    uint32_t bytes = count * g_sector_size;
    uintptr_t off = p & 4095UL;
    uint32_t first_page_bytes = 4096u - (uint32_t)off;

    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_CMD_READ;
    sqe.nsid = 1u;
    sqe.cdw10 = lba;
    sqe.cdw11 = 0;
    sqe.cdw12 = (count - 1u) & 0xFFFFu;

    if (bytes <= first_page_bytes) {
        sqe.prp1 = p;
        sqe.prp2 = 0;
    } else if (bytes <= first_page_bytes + 4096u) {
        sqe.prp1 = p;
        sqe.prp2 = (p + first_page_bytes) & ~4095UL;
    } else {
        sqe.prp1 = p;
        sqe.prp2 = (uintptr_t)&g_prp_list[0];

        uintptr_t cur = (p + first_page_bytes) & ~4095UL;
        uint32_t remaining = bytes - first_page_bytes;
        int prp_idx = 0;
        while (remaining > 0 && prp_idx < 512) {
            g_prp_list[prp_idx++] = cur;
            cur += 4096UL;
            if (remaining > 4096u) remaining -= 4096u; else remaining = 0;
        }
        nvme_flush_l2_range((uintptr_t)&g_prp_list[0],
                            (uintptr_t)&g_prp_list[prp_idx]);
    }

    nvme_flush_l2_range(p, p + bytes);

    if (nvme_io_submit_sync(&sqe, NULL) != 0) {
        return -1;
    }

    nvme_flush_l2_range(p, p + bytes);
    return 0;
}

static int nvme_write_blocks_impl(uint32_t lba, uint32_t count, const void *buf) {
    if (!g_nvme_present || count == 0) return -1;

    uintptr_t p = (uintptr_t)buf;
    if ((p & 4095UL) != 0) {
        /* Bounce unaligned / stack buffers through 4KB-aligned g_admin_buf */
        const uint8_t *src = (const uint8_t *)buf;
        uint32_t sectors_per_bounce = 4096u / g_sector_size;
        while (count > 0) {
            uint32_t batch = (count < sectors_per_bounce) ? count : sectors_per_bounce;
            memcpy(g_admin_buf, src, batch * g_sector_size);
            if (nvme_write_blocks_impl(lba, batch, g_admin_buf) != 0) {
                return -1;
            }
            src += batch * g_sector_size;
            lba += batch;
            count -= batch;
        }
        return 0;
    }

    uint32_t bytes = count * g_sector_size;
    uintptr_t off = p & 4095UL;
    uint32_t first_page_bytes = 4096u - (uint32_t)off;

    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_CMD_WRITE;
    sqe.nsid = 1u;
    sqe.cdw10 = lba;
    sqe.cdw11 = 0;
    sqe.cdw12 = (count - 1u) & 0xFFFFu;

    if (bytes <= first_page_bytes) {
        sqe.prp1 = p;
        sqe.prp2 = 0;
    } else if (bytes <= first_page_bytes + 4096u) {
        sqe.prp1 = p;
        sqe.prp2 = (p + first_page_bytes) & ~4095UL;
    } else {
        sqe.prp1 = p;
        sqe.prp2 = (uintptr_t)&g_prp_list[0];

        uintptr_t cur = (p + first_page_bytes) & ~4095UL;
        uint32_t remaining = bytes - first_page_bytes;
        int prp_idx = 0;
        while (remaining > 0 && prp_idx < 512) {
            g_prp_list[prp_idx++] = cur;
            cur += 4096UL;
            if (remaining > 4096u) remaining -= 4096u; else remaining = 0;
        }
        nvme_flush_l2_range((uintptr_t)&g_prp_list[0],
                            (uintptr_t)&g_prp_list[prp_idx]);
    }

    nvme_flush_l2_range(p, p + bytes);

    if (nvme_io_submit_sync(&sqe, NULL) != 0) {
        return -1;
    }

    return 0;
}

static int nvme_read_blocks_wrapper(struct block_dev *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    ylock_acquire(&g_nvme_lock);
    int rc = nvme_read_blocks_impl(lba, count, buf);
    ylock_release(&g_nvme_lock);
    return rc;
}

static int nvme_write_blocks_wrapper(struct block_dev *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    ylock_acquire(&g_nvme_lock);
    int rc = nvme_write_blocks_impl(lba, count, buf);
    ylock_release(&g_nvme_lock);
    return rc;
}

static int nvme_probe_wrapper(struct block_dev *dev) {
    (void)dev;
    return g_nvme_present ? 0 : -1;
}

/* guarded by: g_nvme_lock for block_size and num_blocks; other fields are constant. */
static block_dev_t g_nvme_dev = {
    .name = "nvme0",
    .block_size = 512,
    .num_blocks = 0,
    .read_blocks = nvme_read_blocks_wrapper,
    .write_blocks = nvme_write_blocks_wrapper,
    .probe = nvme_probe_wrapper,
    .lost = false,
};

block_dev_t *nvme_get_device(void) {
    if (!g_nvme_present) {
        if (nvme_init() != 0) return NULL;
    }
    g_nvme_dev.block_size = g_sector_size;
    g_nvme_dev.num_blocks = (g_total_sectors > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)g_total_sectors;
    return &g_nvme_dev;
}

void nvme_cmd_info(void) {
    if (!g_nvme_present) {
        if (nvme_init() != 0) {
            cprintf("NVMe: No NVMe controller found or initialization failed\n");
            return;
        }
    }

    const jh7110_pcie_port_t *p1 = jh7110_pcie_get_port(1);
    uint32_t size_mb = (uint32_t)((g_total_sectors * g_sector_size) / (1024ULL * 1024ULL));

    cprintf("M.2 NVMe SSD (PCIe Port 1, Bus 01:00.0):\n");
    cprintf("  Model:              %s\n", g_model);
    cprintf("  Serial Number:      %s\n", g_serial);
    cprintf("  Firmware Revision:  %s\n", g_firmware);
    cprintf("  Total Capacity:     %u MB (%llu sectors of %u B)\n",
            (unsigned)size_mb, (unsigned long long)g_total_sectors, (unsigned)g_sector_size);
    cprintf("  BAR0 MMIO Address:  0x%lx\n", (unsigned long)g_nvme_bar0);
    cprintf("  Admin Queue Depth:  %u entries (ASQ: 0x%lx, ACQ: 0x%lx)\n",
            (unsigned)NVME_ADMIN_Q_SIZE,
            (unsigned long)(uintptr_t)&g_admin_sq[0],
            (unsigned long)(uintptr_t)&g_admin_cq[0]);
    cprintf("  I/O Queue Depth:    %u entries (SQ1: 0x%lx, CQ1: 0x%lx)\n",
            (unsigned)NVME_IO_Q_SIZE,
            (unsigned long)(uintptr_t)&g_io_sq[0],
            (unsigned long)(uintptr_t)&g_io_cq[0]);
    if (p1 && p1->link_up) {
        cprintf("  PCIe Link:          Gen%d x%d (5.0 GT/s, theoretical max ~400 MB/s)\n",
                p1->gen, p1->lanes);
    }

    /* Verify Sector 0 LBA read */
    /* guarded by: g_nvme_lock */
    static uint8_t mbr_buf[512] __attribute__((aligned(512)));
    ylock_acquire(&g_nvme_lock);
    int rc = nvme_read_blocks_impl(0, 1, mbr_buf);
    ylock_release(&g_nvme_lock);
    if (rc == 0) {
        uint16_t sig = (uint16_t)mbr_buf[510] | ((uint16_t)mbr_buf[511] << 8);
        cprintf("  Sector 0 Read:      OK (Signature: 0x%04x%s)\n",
                (unsigned)sig, (sig == 0xAA55) ? " - Valid MBR/GPT" : "");
    } else {
        cprintf("  Sector 0 Read:      FAILED\n");
    }
}

void nvme_cmd_bench(uint32_t size_mb) {
    if (!g_nvme_present) {
        if (nvme_init() != 0) {
            cprintf("nvme bench: NVMe not available\n");
            return;
        }
    }

    if (size_mb == 0 || size_mb > 128u) size_mb = 16u;

    uint32_t total_bytes = size_mb * 1024u * 1024u;
    uint32_t chunk_sectors = 128u; /* 64 KiB chunks (128 * 512) */
    uint32_t chunk_bytes = chunk_sectors * g_sector_size;
    uint32_t iterations = total_bytes / chunk_bytes;

    /* Read benchmark buffer in .bss */
    /* guarded by: g_nvme_lock */
    static uint8_t bench_buf[65536] __attribute__((aligned(4096)));

    cprintf("nvme bench: reading %u MB using 64 KB chunks (%u iterations)...\n",
            (unsigned)size_mb, (unsigned)iterations);

    ylock_acquire(&g_nvme_lock);
    uint64_t t0 = time_get_us();

    for (uint32_t i = 0; i < iterations; i++) {
        uint32_t lba = i * chunk_sectors;
        if (nvme_read_blocks_impl(lba, chunk_sectors, bench_buf) != 0) {
            cprintf("nvme bench: read error at LBA %u\n", (unsigned)lba);
            ylock_release(&g_nvme_lock);
            return;
        }
    }

    uint64_t elapsed_us = time_get_us() - t0;
    ylock_release(&g_nvme_lock);

    if (elapsed_us == 0) elapsed_us = 1;
    uint32_t throughput_kb_s = (uint32_t)(((uint64_t)total_bytes * 1000ULL) / elapsed_us);
    uint32_t throughput_mb_s = throughput_kb_s / 1024u;
    uint32_t iops = (uint32_t)(((uint64_t)iterations * 1000000ULL) / elapsed_us);

    cprintf("nvme bench: %u MB in %u ms = %u.%u MB/s (%u IOPS)\n",
            (unsigned)size_mb, (unsigned)(elapsed_us / 1000u),
            (unsigned)throughput_mb_s, (unsigned)((throughput_kb_s % 1024u) * 10u / 1024u),
            (unsigned)iops);
}
