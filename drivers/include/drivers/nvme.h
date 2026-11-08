#ifndef LUGALOS_DRIVERS_NVME_H
#define LUGALOS_DRIVERS_NVME_H

#include <stdbool.h>
#include <stdint.h>
#include "drivers/block.h"

/* NVMe Controller Register Offsets (from BAR0) */
#define NVME_REG_CAP_LO     0x00u   /* Controller Capabilities (lower 32) */
#define NVME_REG_CAP_HI     0x04u   /* Controller Capabilities (upper 32) */
#define NVME_REG_VS         0x08u   /* Version */
#define NVME_REG_INTMS      0x0Cu   /* Interrupt Mask Set */
#define NVME_REG_INTMC      0x10u   /* Interrupt Mask Clear */
#define NVME_REG_CC         0x14u   /* Controller Configuration */
#define NVME_REG_CSTS       0x1Cu   /* Controller Status */
#define NVME_REG_NSSR       0x20u   /* NVM Subsystem Reset */
#define NVME_REG_AQA        0x24u   /* Admin Queue Attributes */
#define NVME_REG_ASQ_LO     0x28u   /* Admin Submission Queue Base (lower 32) */
#define NVME_REG_ASQ_HI     0x2Cu   /* Admin Submission Queue Base (upper 32) */
#define NVME_REG_ACQ_LO     0x30u   /* Admin Completion Queue Base (lower 32) */
#define NVME_REG_ACQ_HI     0x34u   /* Admin Completion Queue Base (upper 32) */
#define NVME_REG_DBS        0x1000u /* Doorbell array base */

/* CC register bitfields */
#define NVME_CC_EN          (1u << 0)
#define NVME_CC_CSS_NVM     (0u << 4)
#define NVME_CC_MPS_4K      (0u << 7)
#define NVME_CC_IOSQES_64   (6u << 16)
#define NVME_CC_IOCQES_16   (4u << 20)

/* CSTS register bitfields */
#define NVME_CSTS_RDY       (1u << 0)
#define NVME_CSTS_CFS       (1u << 1)

/* Admin Command Opcodes */
#define NVME_ADMIN_DELETE_SQ    0x00u
#define NVME_ADMIN_CREATE_SQ    0x01u
#define NVME_ADMIN_GET_LOG_PAGE 0x02u
#define NVME_ADMIN_DELETE_CQ    0x04u
#define NVME_ADMIN_CREATE_CQ    0x05u
#define NVME_ADMIN_IDENTIFY     0x06u
#define NVME_ADMIN_ABORT        0x08u
#define NVME_ADMIN_SET_FEATURES 0x09u
#define NVME_ADMIN_GET_FEATURES 0x0Au

/* NVM Command Opcodes */
#define NVME_CMD_FLUSH          0x00u
#define NVME_CMD_WRITE          0x01u
#define NVME_CMD_READ           0x02u

/* Queue Sizes */
#define NVME_ADMIN_Q_SIZE       16u
#define NVME_IO_Q_SIZE          64u

/* 64-byte Submission Queue Entry (SQE) */
typedef struct __attribute__((packed)) {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd1;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;

/* 16-byte Completion Queue Entry (CQE) */
typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} nvme_cqe_t;

/* 4096-byte Identify Controller Structure */
typedef struct __attribute__((packed)) {
    uint16_t vid;
    uint16_t ssvid;
    char     sn[20];
    char     mn[40];
    char     fr[8];
    uint8_t  rab;
    uint8_t  ieee[3];
    uint8_t  cmic;
    uint8_t  mdts;
    uint16_t cntlid;
    uint32_t ver;
    uint8_t  rsvd84[172];
    uint16_t oacs;
    uint8_t  acl;
    uint8_t  aerl;
    uint8_t  frmw;
    uint8_t  lpa;
    uint8_t  elpe;
    uint8_t  npss;
    uint8_t  avscc;
    uint8_t  apsta;
    uint16_t wctemp;
    uint16_t cctemp;
    uint8_t  rsvd270[242];
    uint8_t  sqes;
    uint8_t  cqes;
    uint8_t  rsvd514[2];
    uint32_t nn;
    uint8_t  rsvd520[3576];
} nvme_id_ctrl_t;

/* LBA Format Data Structure (4 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t ms;    /* Metadata Size */
    uint8_t  ds;    /* LBA Data Size (power of two, e.g. 9 = 512, 12 = 4096) */
    uint8_t  rp;    /* Relative Performance (bits 0..1) + reserved */
} nvme_lbaf_t;

/* 4096-byte Identify Namespace Structure */
typedef struct __attribute__((packed)) {
    uint64_t nsze;
    uint64_t ncap;
    uint64_t nuse;
    uint8_t  nsfeat;
    uint8_t  nlbaf;
    uint8_t  flbas;
    uint8_t  mc;
    uint8_t  dpc;
    uint8_t  dps;
    uint8_t  nmic;
    uint8_t  rescap;
    uint8_t  fpi;
    uint8_t  dlfeat;
    uint8_t  rsvd34[94];
    nvme_lbaf_t lbaf[16];
    uint8_t  rsvd192[3904];
} nvme_id_ns_t;

/* Driver public API */
int nvme_init(void);
bool nvme_is_present(void);
block_dev_t *nvme_get_device(void);

/* Diagnostic & benchmark reporters */
void nvme_cmd_info(void);
void nvme_cmd_bench(uint32_t size_mb);

#endif /* LUGALOS_DRIVERS_NVME_H */
