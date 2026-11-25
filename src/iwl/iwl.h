/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl.h - the Intel iwlwifi back end: registers, firmware command interface, device table, internal API.
 *
 * Register addresses, command numbers and structure layouts are taken from OpenBSD's if_iwmreg.h (ISC licence,
 * itself derived from Intel's iwlwifi headers, dual BSD/GPL) and checked against Linux iwlwifi
 * (drivers/net/wireless/intel/iwlwifi: iwl-csr.h, iwl-fh.h, iwl-prph.h, the headers in fw/api). Every structure here is a
 * firmware ABI: compile with -zp1 (packed) and do not reorder fields. Sizes are asserted in iwl_cfg.c.
 */
#ifndef IWL_H
#define IWL_H

#include "drv.h"

/* =================================================================================================== */
/* Device families and the device table (iwl_cfg.c)                                                   */
/* =================================================================================================== */
#define IWL_FAM_7000    1                       /* 7260, 3160, 7265, 3165, 3168                         */
#define IWL_FAM_8000    2                       /* 8260, 8265, 4165                                     */
#define IWL_FAM_9000    3                       /* 9260, 9461, 9462, 9560                               */

#define IWL_NVM         0                       /* NVM layouts (iwm: nvm_type)                           */
#define IWL_NVM_EXT     1
#define IWL_NVM_SDP     2

typedef struct {
   UCHAR  family;
   UCHAR  nvm_type;
   UCHAR  host_int_oper;                        /* 7260/3160 interrupt coalescing work-around            */
   UCHAR  supported;                            /* this build drives the family (see ADDING-A-DEVICE.md)  */
   USHORT nvm_max_section;                      /* bytes                                                  */
   char  *fw;                                   /* firmware file (linux-firmware name)                    */
} iwl_cfg_t;

typedef struct {
   USHORT dev;                                  /* PCI device id                                          */
   USHORT subdev;                               /* PCI subsystem device id, 0xFFFF = any                  */
   const iwl_cfg_t *cfg;
   char  *name;
} iwl_dev_t;

extern const iwl_cfg_t *g_cfg;                  /* the configuration of the device found                  */
extern char *g_devname;

/* =================================================================================================== */
/* CSR (BAR0) registers                                                                               */
/* =================================================================================================== */
#define CSR_HW_IF_CONFIG_REG        0x000
#define CSR_INT_COALESCING          0x004
#define CSR_INT_PERIODIC_REG        0x005
#define CSR_INT                     0x008
#define CSR_INT_MASK                0x00C
#define CSR_FH_INT_STATUS           0x010
#define CSR_RESET                   0x020
#define CSR_GP_CNTRL                0x024
#define CSR_HW_REV                  0x028
#define CSR_GIO_REG                 0x03C
#define CSR_UCODE_DRV_GP1           0x054
#define CSR_UCODE_DRV_GP1_SET       0x058
#define CSR_UCODE_DRV_GP1_CLR       0x05C
#define CSR_MBOX_SET_REG            0x088
#define CSR_DRAM_INT_TBL_REG        0x0A0
#define CSR_MAC_SHADOW_REG_CTRL     0x0A8
#define CSR_GIO_CHICKEN_BITS        0x100
#define CSR_DBG_HPET_MEM_REG        0x240
#define CSR_DBG_LINK_PWR_MGMT_REG   0x250

#define CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH   0x00000003UL
#define CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP   0x0000000CUL
#define CSR_HW_IF_CONFIG_REG_BIT_MAC_SI     0x00000100UL
#define CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI   0x00000200UL
#define CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE   0x00000C00UL
#define CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH   0x00003000UL
#define CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP   0x0000C000UL
#define CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A 0x00080000UL
#define CSR_HW_IF_CONFIG_REG_BIT_NIC_READY  0x00400000UL
#define CSR_HW_IF_CONFIG_REG_PREPARE        0x08000000UL
#define CSR_MBOX_SET_REG_OS_ALIVE           0x20UL

#define CSR_INT_BIT_FH_RX       0x80000000UL
#define CSR_INT_BIT_HW_ERR      0x20000000UL
#define CSR_INT_BIT_RX_PERIODIC 0x10000000UL
#define CSR_INT_BIT_FH_TX       0x08000000UL
#define CSR_INT_BIT_SCD         0x04000000UL
#define CSR_INT_BIT_SW_ERR      0x02000000UL
#define CSR_INT_BIT_RF_KILL     0x00000080UL
#define CSR_INT_BIT_CT_KILL     0x00000040UL
#define CSR_INT_BIT_SW_RX       0x00000008UL
#define CSR_INT_BIT_WAKEUP      0x00000002UL
#define CSR_INT_BIT_ALIVE       0x00000001UL
#define CSR_INI_SET_MASK        (CSR_INT_BIT_FH_RX | CSR_INT_BIT_HW_ERR | CSR_INT_BIT_FH_TX | CSR_INT_BIT_SW_ERR | \
                                 CSR_INT_BIT_RF_KILL | CSR_INT_BIT_SW_RX | CSR_INT_BIT_WAKEUP | CSR_INT_BIT_ALIVE | \
                                 CSR_INT_BIT_RX_PERIODIC)
#define CSR_FH_INT_RX_MASK      0x40030000UL    /* HI_PRIOR | RX_CHNL1 | RX_CHNL0                         */
#define CSR_FH_INT_TX_MASK      0x00000003UL    /* TX_CHNL1 | TX_CHNL0                                    */
#define CSR_INT_PERIODIC_DIS    0x00
#define CSR_INT_PERIODIC_ENA    0xFF

#define CSR_RESET_REG_FLAG_SW_RESET          0x00000080UL
#define CSR_RESET_REG_FLAG_STOP_MASTER       0x00000200UL
#define CSR_RESET_REG_FLAG_MASTER_DISABLED   0x00000100UL
#define CSR_RESET_LINK_PWR_MGMT_DISABLED     0x80000000UL

#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY 0x00000001UL
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE      0x00000004UL
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ 0x00000008UL
#define CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP 0x00000010UL
#define CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN   0x00000001UL
#define CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW  0x08000000UL

#define CSR_GIO_REG_VAL_L0S_DISABLED         0x00000002UL   /* Linux: CSR_GIO_REG_VAL_L0S_DISABLED     */
#define CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX 0x00800000UL
#define CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER 0x20000000UL
#define CSR_DBG_HPET_MEM_REG_VAL             0xFFFF0000UL
#define CSR_UCODE_SW_BIT_RFKILL              0x00000002UL
#define CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED    0x00000004UL
#define HOST_INT_TIMEOUT_DEF                 0x40

/* HBUS: indirect access to the device's peripheral (PRPH) registers and SRAM */
#define HBUS_TARG_MEM_RADDR     0x40C
#define HBUS_TARG_MEM_WADDR     0x410
#define HBUS_TARG_MEM_WDAT      0x418
#define HBUS_TARG_MEM_RDAT      0x41C
#define HBUS_TARG_PRPH_WADDR    0x444
#define HBUS_TARG_PRPH_RADDR    0x448
#define HBUS_TARG_PRPH_WDAT     0x44C
#define HBUS_TARG_PRPH_RDAT     0x450
#define HBUS_TARG_WRPTR         0x460

/* FH: the DMA engine */
#define FH_KW_MEM_ADDR_REG              0x197C
#define FH_MEM_RCSR_CHNL0_CONFIG_REG    0x1C00
#define FH_MEM_RCSR_CHNL0_RBDCB_WPTR    0x1C08
#define FH_MEM_RCSR_CHNL0_FLUSH_RB_REQ  0x1C10
#define FH_RSCSR_CHNL0_STTS_WPTR_REG    0x1BC0
#define FH_RSCSR_CHNL0_RBDCB_BASE_REG   0x1BC4
#define FH_RSCSR_CHNL0_WPTR             0x1BC8
#define FH_RSCSR_CHNL0_RDPTR            0x1BCC
#define FH_MEM_RSSR_RX_STATUS_REG       0x1C44
#define FH_TFDIB_CTRL0_REG(c)           (0x1900 + 8 * (c))
#define FH_TFDIB_CTRL1_REG(c)           (0x1904 + 8 * (c))
#define FH_TCSR_CHNL_TX_CONFIG_REG(c)   (0x1D00 + 0x20 * (c))
#define FH_TCSR_CHNL_TX_BUF_STS_REG(c)  (0x1D08 + 0x20 * (c))
#define FH_TCSR_CHNL_NUM                8
#define FH_SRVC_CHNL                    9
#define FH_SRVC_CHNL_SRAM_ADDR_REG      0x19C8          /* FH_SRVC_LOWER_BOUND + (9 - 9) * 4            */
#define FH_TX_CHICKEN_BITS_REG          0x1E98
#define FH_UCODE_LOAD_STATUS            0x1AF0
#define FH_MEM_CBBC_0_15                0x19D0          /* TFD ring base of queue q (q < 16): + 4 * q      */

#define FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL    0x80000000UL
#define FH_RCSR_CHNL0_RX_IGNORE_RXF_EMPTY       0x00000004UL
#define FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL 0x00001000UL
#define FH_RCSR_RX_CONFIG_REG_IRQ_RBTH_POS      4
#define FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K    0x00000000UL
#define FH_RCSR_RX_CONFIG_RBDCB_SIZE_POS        20
#define RX_RB_TIMEOUT                           0x11
#define RX_QUEUE_SIZE_LOG                       8
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE  0x00000000UL
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE 0x80000000UL
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE 0x00000000UL
#define FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE  0x00000008UL
#define FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD 0x00100000UL
#define FH_TCSR_CHNL_TX_BUF_STS_REG_VAL_TFDB_VALID 0x00000003UL
#define FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_NUM  20
#define FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_IDX  12
#define FH_TX_CHICKEN_BITS_SCD_AUTO_RETRY_EN    0x00000002UL
#define FH_RSCSR_FRAME_SIZE_MSK                 0x00003FFFUL
#define FH_RSCSR_FRAME_INVALID                  0x55550000UL
#define FH_RSCSR_FRAME_ALIGN                    0x40

/* PRPH (peripheral) registers, reached through HBUS_TARG_PRPH_* */
#define PRPH_RELEASE_CPU_RESET      0x300CUL
#define PRPH_RELEASE_CPU_RESET_BIT  0x01000000UL
#define PRPH_LMPM_CHICK             0xA01FF8UL
#define PRPH_LMPM_CHICK_EXTENDED_ADDR_SPACE 0x01UL
#define PRPH_WFMP_MAC_ADDR_0        0xA03080UL
#define PRPH_WFMP_MAC_ADDR_1        0xA03084UL
#define PRPH_WFPM_CTRL_REG          0xA03030UL
#define PRPH_ENABLE_WFPM            0x80000000UL
#define PRPH_AUX_MISC_REG           0xA200B0UL
#define PRPH_HW_STEP_LOCATION_BITS  24
#define PRPH_WFPM_GP2               0xA030B4UL
#define PRPH_APMG_CLK_EN_REG        0x3004UL           /* 7000 family only                               */
#define PRPH_APMG_PCIDEV_STT_REG    0x3010UL
#define PRPH_APMG_PS_CTRL_REG       0x300CUL
#define FW_MEM_EXTENDED_START       0x40000UL
#define FW_MEM_EXTENDED_END         0x57FFFUL

/* The TX scheduler (SCD) */
#define SCD_BASE                    0xA02C00UL
#define SCD_SRAM_BASE_ADDR          (SCD_BASE + 0x0)
#define SCD_DRAM_BASE_ADDR          (SCD_BASE + 0x8)
#define SCD_TXFACT                  (SCD_BASE + 0x10)
#define SCD_CHAINEXT_EN             (SCD_BASE + 0x244)
#define SCD_AGGR_SEL                (SCD_BASE + 0x248)
#define SCD_GP_CTRL                 (SCD_BASE + 0x1A8)
#define SCD_EN_CTRL                 (SCD_BASE + 0x254)
#define SCD_QUEUE_RDPTR(q)          (SCD_BASE + 0x68 + (ULONG)(q) * 4)   /* queues 0..19            */
#define SCD_QUEUE_STATUS_BITS(q)    (SCD_BASE + 0x10C + (ULONG)(q) * 4)  /* queues 0..19            */
#define SCD_CONTEXT_MEM_LOWER_BOUND 0x600
#define SCD_TRANS_TBL_MEM_UPPER_BOUND 0x808
#define SCD_CONTEXT_QUEUE_OFFSET(q) (SCD_CONTEXT_MEM_LOWER_BOUND + (q) * 8)
#define SCD_GP_CTRL_ENABLE_31_QUEUES 0x00000001UL
#define SCD_GP_CTRL_AUTO_ACTIVE_MODE 0x00040000UL
#define SCD_QUEUE_STTS_REG_POS_TXF   0
#define SCD_QUEUE_STTS_REG_POS_ACTIVE 3
#define SCD_QUEUE_STTS_REG_POS_WSL   4
#define SCD_QUEUE_STTS_REG_POS_SCD_ACT_EN 19
#define SCD_QUEUE_STTS_REG_MSK       0x017F0000UL
#define SCD_QUEUE_CTX_REG2_WIN_SIZE_MSK    0x0000007FUL
#define SCD_QUEUE_CTX_REG2_FRAME_LIMIT_MSK 0x007F0000UL

/* =================================================================================================== */
/* Queues, rings                                                                                       */
/* =================================================================================================== */
#define TFD_QUEUE_SIZE_MAX      256             /* hardware ring of transmit frame descriptors           */
#define TFD_QUEUE_SIZE_BC_DUP   64
#define TFD_QUEUE_BC_SIZE       (TFD_QUEUE_SIZE_MAX + TFD_QUEUE_SIZE_BC_DUP)
#define TFD_SIZE                128             /* struct iwl_tfd: 4 + 20 * 6 + 4                         */
#define NUM_OF_TBS              20
#define IWL_MAX_QUEUES          31
#define TX_CRC_SIZE             4
#define TX_DELIMITER_SIZE       4

#define DQA_CMD_QUEUE           0
#define DQA_AUX_QUEUE           1
#define DQA_MIN_MGMT_QUEUE      5               /* + EDCA access category (BE 0, BK 1, VI 2, VO 3)        */
#define DQA_MIN_DATA_QUEUE      10              /* + TID: the transmit aggregation queue of that TID      */
#define LEGACY_CMD_QUEUE        9
#define LEGACY_AUX_QUEUE        15
#define TX_FIFO_BK              0
#define TX_FIFO_BE              1
#define TX_FIFO_VI              2
#define TX_FIFO_VO              3
#define TX_FIFO_MCAST           5
#define TX_FIFO_CMD             7
#define STATION_ID              0
#define AUX_STA_ID              1
#define MAC_INDEX_AUX           4
#define MAX_TID_COUNT           8
#define TID_NON_QOS             0
#define FRAME_LIMIT             64

#define TX_SLOTS                32              /* data frames in flight (a divisor of the 256-entry ring)   */
#define TX_SLOT_SZ              1792U           /* command (<= 256 bytes) + CCMP header + 1514-byte frame      */
#define QI_BE                   2               /* index in the driver's queue table: the BE data queue    */
#define QI_AGG                  6               /* ... and the aggregation queue of TID 0                  */
#define RX_RING_COUNT           256             /* hardware ring of receive buffer descriptors            */
#define RX_NBUF                 64              /* buffers actually posted                                */
#define RBUF_SIZE               4096

/* =================================================================================================== */
/* Firmware file (.ucode, TLV format)                                                                  */
/* =================================================================================================== */
#define TLV_UCODE_MAGIC         0x0A4C5749UL    /* "IWL\n"                                                */
#define UCODE_TLV_PROBE_MAX_LEN 6
#define UCODE_TLV_PAN           7
#define UCODE_TLV_FLAGS         18
#define UCODE_TLV_SEC_RT        19
#define UCODE_TLV_SEC_INIT      20
#define UCODE_TLV_SEC_WOWLAN    21
#define UCODE_TLV_DEF_CALIB     22
#define UCODE_TLV_PHY_SKU       23
#define UCODE_TLV_NUM_OF_CPU    27
#define UCODE_TLV_CSCHEME       28
#define UCODE_TLV_API_CHANGES_SET 29
#define UCODE_TLV_ENABLED_CAPABILITIES 30
#define UCODE_TLV_N_SCAN_CHANNELS 31
#define UCODE_TLV_PAGING        32
#define UCODE_TLV_SEC_RT_USNIFFER 34
#define UCODE_TLV_FW_VERSION    36
#define UCODE_TLV_CMD_VERSIONS  48

#define UCODE_REGULAR           0
#define UCODE_INIT              1
#define UCODE_SECT_MAX          16
#define CPU1_CPU2_SEPARATOR_SECTION 0xFFFFCCCCUL
#define PAGING_SEPARATOR_SECTION    0xAAAABBBBUL

/* API and capability bits (iwm / iwlwifi numbering) */
#define API_FRAGMENTED_SCAN     8
#define API_WIFI_MCC_UPDATE     9
#define API_TKIP_MIC_KEYS       29
#define API_STA_TYPE            30
#define API_ADAPTIVE_DWELL      32
#define API_QUOTA_LOW_LATENCY   38
#define API_ADAPTIVE_DWELL_V2   42
#define API_SCAN_EXT_CHAN_VER   58
#define CAPA_LAR_SUPPORT        1
#define CAPA_UMAC_SCAN          2
#define CAPA_DS_PARAM_SET_IE_SUPPORT 9
#define CAPA_WFA_TPC_REP_IE_SUPPORT  10
#define CAPA_DQA_SUPPORT        12
#define CAPA_LAR_MULTI_MCC      29
#define CAPA_SOC_LATENCY_SUPPORT 37
#define CAPA_BINDING_CDB_SUPPORT 39
#define CAPA_CDB_SUPPORT        40
#define CAPA_DYNAMIC_QUOTA      44
#define CAPA_ULTRA_HB_CHANNELS  48
#define CAPA_LAR_SUPPORT_V3     73
#define CAPA_CT_KILL_BY_FW      74
#define UCODE_TLV_FLAGS_DW_BC_TABLE 0x10UL

#define FW_PHY_CFG_RADIO_TYPE_POS 0
#define FW_PHY_CFG_TX_CHAIN_POS   16
#define FW_PHY_CFG_RX_CHAIN_POS   20

/* Paging (CPU2 image kept in host memory) */
#define FW_PAGING_SIZE          4096
#define PAGING_BLOCK_SIZE       32768UL
#define NUM_OF_PAGE_PER_GROUP   8
#define BLOCK_2_EXP_SIZE        15
#define PAGE_2_EXP_SIZE         12
#define NUM_OF_FW_PAGING_BLOCKS 33
#define MAX_PAGING_IMAGE_SIZE   (32UL * PAGING_BLOCK_SIZE)
#define PAGING_CMD_IS_SECURED   0x200UL
#define PAGING_CMD_IS_ENABLED   0x100UL

typedef struct {
   ULONG  devoff;                               /* load address in the device                           */
   ULONG  off;                                  /* data: offset in the firmware blob                    */
   ULONG  len;
} fwsect_t;

typedef struct {
   USHORT nsec;
   fwsect_t sec[UCODE_SECT_MAX];
   ULONG  paging_size;
   ULONG  flow_trigger, event_trigger;          /* default calibration (TLV DEF_CALIB)                  */
} fwimg_t;

typedef struct {
   fwimg_t img[2];                              /* UCODE_REGULAR, UCODE_INIT                            */
   ULONG   capaflags;
   ULONG   phy_config;                          /* TLV PHY_SKU                                          */
   UCHAR   api[16];                             /* 128 API bits                                         */
   UCHAR   capa[16];                            /* 128 capability bits                                  */
   USHORT  n_scan_channels;
   USHORT  max_probe_len;
   char    version[32];
   ULONG   major;
} fwinfo_t;
extern fwinfo_t g_fw;
#define FW_API(b)   ((g_fw.api[(b) >> 3] >> ((b) & 7)) & 1)
#define FW_CAPA(b)  ((g_fw.capa[(b) >> 3] >> ((b) & 7)) & 1)

/* =================================================================================================== */
/* Firmware commands                                                                                   */
/* =================================================================================================== */
#define WIDE_ID(g, c)           ((USHORT)(((g) << 8) | (c)))
#define LONG_GROUP              0x1
#define SYSTEM_GROUP            0x2
#define PHY_OPS_GROUP           0x4
#define DATA_PATH_GROUP         0x5

#define CMD_ALIVE               0x01
#define CMD_REPLY_ERROR         0x02
#define CMD_INIT_COMPLETE_NOTIF 0x04
#define CMD_PHY_CONTEXT         0x08
#define CMD_SCAN_CFG            0x0C            /* LONG_GROUP                                             */
#define CMD_SCAN_REQ_UMAC       0x0D            /* LONG_GROUP                                             */
#define CMD_SCAN_ABORT_UMAC     0x0E            /* LONG_GROUP                                             */
#define CMD_SCAN_COMPLETE_UMAC  0x0F            /* notification                                           */
#define CMD_ADD_STA_KEY         0x17
#define CMD_ADD_STA             0x18
#define CMD_REMOVE_STA          0x19
#define CMD_TX                  0x1C
#define CMD_SCD_QUEUE_CFG       0x1D
#define CMD_TXPATH_FLUSH        0x1E
#define CMD_MAC_CONTEXT         0x28
#define CMD_TIME_EVENT          0x29
#define CMD_TIME_EVENT_NOTIF    0x2A
#define CMD_BINDING_CONTEXT     0x2B
#define CMD_TIME_QUOTA          0x2C
#define CMD_LQ                  0x4E
#define CMD_FW_PAGING_BLOCK     0x4F            /* LONG_GROUP                                             */
#define CMD_PHY_CONFIGURATION   0x6A
#define CMD_CALIB_RES_NOTIF_PHY_DB 0x6B
#define CMD_PHY_DB              0x6C
#define CMD_POWER_TABLE         0x77
#define CMD_NVM_ACCESS          0x88
#define CMD_TX_ANT_CONFIGURATION 0x98
#define CMD_BT_CONFIG           0x9B
#define CMD_STATISTICS_NOTIF    0x9D
#define CMD_MISSED_BEACONS_NOTIF 0xA2
#define CMD_MAC_PM_POWER_TABLE  0xA9
#define CMD_MFUART_LOAD_NOTIF   0xB1
#define CMD_SCAN_ITERATION_COMPLETE_UMAC 0xB5
#define CMD_REPLY_RX_PHY        0xC0
#define CMD_REPLY_RX_MPDU       0xC1
#define CMD_BA_NOTIF            0xC5
#define CMD_MCC_UPDATE          0xC8
#define CMD_MCC_CHUB_UPDATE     0xC9
#define CMD_MCAST_FILTER        0xD0
#define CMD_REPLY_SF_CFG        0xD1
#define CMD_REPLY_BEACON_FILTERING 0xD2
#define CMD_DEBUG_LOG_MSG       0xF7
#define CMD_SOC_CONFIGURATION   0x01            /* SYSTEM_GROUP                                           */
#define CMD_DQA_ENABLE          0x00            /* DATA_PATH_GROUP                                        */
#define CMD_TEMP_REPORTING_THRESHOLDS 0x04      /* PHY_OPS_GROUP                                          */
#define CMD_CT_KILL_NOTIF       0xFE            /* PHY_OPS_GROUP                                          */

#define CMD_FAILED_MSK          0x40
#define DEF_CMD_PAYLOAD_SIZE    320
#define MAX_CMD_PAYLOAD_SIZE    4000            /* header + payload must fit the 12-bit TB length            */

#define FW_CTXT_ACTION_ADD      1UL
#define FW_CTXT_ACTION_MODIFY   2UL
#define FW_CTXT_ACTION_REMOVE   3UL
#define FW_CTXT_INVALID         0xFFFFFFFFUL
#define ID_AND_COLOR(id, c)     ((ULONG)(id) | ((ULONG)(c) << 8))

#pragma pack(push, 1)

typedef struct { UCHAR code, flags, idx, qid; } cmd_hdr_t;
typedef struct { UCHAR opcode, group_id, idx, qid; USHORT length; UCHAR reserved, version; } cmd_hdr_wide_t;

typedef struct {                                /* ALIVE, version 3 (the 8000 firmware; v1/v2 by length)  */
   USHORT status, flags;
   ULONG  ucode_minor, ucode_major;
   UCHAR  ver_subtype, ver_type, mac, opt;
   ULONG  timestamp, error_event_table_ptr, log_event_table_ptr, cpu_register_ptr, dbgm_config_ptr;
   ULONG  alive_counter_ptr, scd_base_ptr, st_fwrd_addr, st_fwrd_size, umac_minor, umac_major;
   ULONG  error_info_addr, dbg_print_buff_addr;
} alive_v3_t;
typedef struct {
   USHORT status, flags;
   UCHAR  ucode_minor, ucode_major;
   USHORT id;
   UCHAR  api_minor, api_major, ver_subtype, ver_type, mac, opt;
   USHORT reserved2;
   ULONG  timestamp, error_event_table_ptr, log_event_table_ptr, cpu_register_ptr, dbgm_config_ptr;
   ULONG  alive_counter_ptr, scd_base_ptr;
} alive_v1_t;
typedef struct {
   alive_v1_t v1;
   ULONG  st_fwrd_addr, st_fwrd_size;
   UCHAR  umac_minor, umac_major;
   USHORT umac_id;
   ULONG  error_info_addr, dbg_print_buff_addr;
} alive_v2_t;
#define ALIVE_STATUS_OK 0xCAFE

typedef struct { UCHAR op_code, target; USHORT type, offset, length; } nvm_access_cmd_t;
typedef struct { USHORT offset, length, type, status; } nvm_access_resp_t;   /* + data             */

typedef struct { ULONG phy_cfg; ULONG flow_trigger, event_trigger; } phy_cfg_cmd_t;
typedef struct { USHORT type, length; } phy_db_hdr_t;                         /* + data             */
#define PHY_DB_CFG              1
#define PHY_DB_CALIB_NCH        2
#define PHY_DB_CALIB_CHG_PAPD   4
#define PHY_DB_CALIB_CHG_TXP    5
#define NUM_PAPD_CH_GROUPS      9
#define NUM_TXP_CH_GROUPS       9

typedef struct {
   ULONG  state;
   ULONG  watermark[2];
   ULONG  long_delay_timeouts[5][2];
   ULONG  full_on_timeouts[5][2];
} sf_cfg_cmd_t;
#define SF_FULL_ON              1UL
#define SF_INIT_OFF             3UL

typedef struct {
   UCHAR  token, sta_id, tid, scd_queue, enable, aggregate, tx_fifo, window;
   USHORT ssn, reserved;
} scd_txq_cfg_cmd_t;

typedef struct {
   UCHAR  add_modify, awake_acs;
   USHORT tid_disable_tx;
   ULONG  mac_id_n_color;
   UCHAR  addr[6];
   USHORT reserved2;
   UCHAR  sta_id, modify_mask;
   USHORT reserved3;
   ULONG  station_flags, station_flags_msk;
   UCHAR  add_immediate_ba_tid, remove_immediate_ba_tid;
   USHORT add_immediate_ba_ssn;
   USHORT sleep_tx_count;
   UCHAR  sleep_state_flags, station_type;     /* v7: one USHORT sleep_state_flags                       */
   USHORT assoc_id, beamform_flags;
   ULONG  tfd_queue_msk;
   USHORT rx_ba_window;                         /* not in v7 (v7 ends after tfd_queue_msk)                */
   UCHAR  sp_length, uapsd_acs;
} add_sta_cmd_t;
#define ADD_STA_CMD_V7_SIZE     44
#define STA_FLG_MAX_AGG_SIZE_MSK 0x00380000UL
#define STA_FLG_AGG_MPDU_DENS_MSK 0x01C00000UL
#define STA_FLG_FAT_EN_MSK      0x0C000000UL
#define STA_FLG_MIMO_EN_MSK     0x30000000UL
#define STA_FLG_MIMO_EN_MIMO2   0x10000000UL
#define STA_FLG_FAT_EN_40MHZ    0x04000000UL
#define STA_FLG_FAT_EN_80MHZ    0x08000000UL
#define STA_FLG_MAX_AGG_SIZE_SHIFT 19
#define STA_FLG_AGG_MPDU_DENS_SHIFT 23
#define STA_FLG_MAX_AGG_SIZE_64K 0x00180000UL   /* 3 << 19                                                */
#define STA_MODIFY_ADD_BA_TID   0x08
#define STA_MODIFY_REMOVE_BA_TID 0x10
#define STA_MODIFY_TID_DISABLE_TX 0x02
#define STA_MODIFY_QUEUES       0x80
#define STA_LINK                0
#define STA_AUX_ACTIVITY        4
#define ADD_STA_SUCCESS         0x1

typedef struct {
   UCHAR  sta_id, key_offset;
   USHORT key_flags;
   UCHAR  key[32];
   UCHAR  rx_secur_seq_cnt[16];
   UCHAR  tail[24];                             /* v2: rx_mic_key, tx_mic_key, transmit_seq_cnt (8 each) */
} add_sta_key_cmd_t;                            /* v1 size 64 (12-byte tail), v2 size 76                  */
#define STA_KEY_FLG_CCM         0x0002
#define STA_KEY_FLG_NO_ENC      0x0000
#define STA_KEY_FLG_WEP_KEY_MAP 0x0008
#define STA_KEY_FLG_KEYID_POS   8
#define STA_KEY_NOT_VALID       0x0800
#define STA_KEY_MULTICAST       0x4000

typedef struct { UCHAR sta_id, reserved[3]; } rm_sta_cmd_t;

typedef struct { UCHAR band, channel, width, ctrl_pos; } chan_info_v1_t;
typedef struct { ULONG channel; UCHAR band, width, ctrl_pos, reserved; } chan_info_t;
typedef struct {
   ULONG  id_and_color, action, apply_time, tx_param_color;
   chan_info_v1_t ci;
   ULONG  txchain_info, rxchain_info, acquisition_data, dsp_cfg_flags;
} phy_ctx_cmd_t;
typedef struct {
   ULONG  id_and_color, action, apply_time, tx_param_color;
   chan_info_t ci;
   ULONG  txchain_info, rxchain_info, acquisition_data, dsp_cfg_flags;
} phy_ctx_cmd_uhb_t;
#define PHY_BAND_5              0
#define PHY_BAND_24             1
#define PHY_VHT_CHANNEL_MODE20  0
#define PHY_VHT_CHANNEL_MODE40  1
#define PHY_VHT_CHANNEL_MODE80  2
#define PHY_VHT_CTRL_POS_1_ABOVE 4
#define PHY_VHT_CTRL_POS_1_BELOW 0
#define PHY_VHT_CTRL_POS_2_BELOW 1
#define PHY_VHT_CTRL_POS_2_ABOVE 5
#define PHY_RX_CHAIN_DRIVER_FORCE_POS 0
#define PHY_RX_CHAIN_VALID_POS  1
#define PHY_RX_CHAIN_FORCE_SEL_POS 4
#define PHY_RX_CHAIN_FORCE_MIMO_SEL_POS 7
#define PHY_RX_CHAIN_CNT_POS    10
#define PHY_RX_CHAIN_MIMO_CNT_POS 12

typedef struct {
   ULONG  id_and_color, action;
   ULONG  macs[3];
   ULONG  phy;
   ULONG  lmac_id;                              /* not in v1 (size 24)                                    */
} binding_cmd_t;

typedef struct { USHORT cw_min, cw_max; UCHAR aifsn, fifos_mask; USHORT edca_txop; } ac_qos_t;
typedef struct {
   ULONG  is_assoc, dtim_time;
   ULONG  dtim_tsf_lo, dtim_tsf_hi;
   ULONG  bi, bi_reciprocal, dtim_interval, dtim_reciprocal, listen_interval, assoc_id, assoc_beacon_arrive_time;
} mac_data_sta_t;
typedef struct {
   ULONG  id_and_color, action, mac_type, tsf_id;
   UCHAR  node_addr[6];
   USHORT reserved_for_node_addr;
   UCHAR  bssid_addr[6];
   USHORT reserved_for_bssid_addr;
   ULONG  cck_rates, ofdm_rates, protection_flags, cck_short_preamble, short_slot, filter_flags, qos_flags;
   ac_qos_t ac[5];
   union {
      mac_data_sta_t sta;
      UCHAR raw[48];                            /* the largest member of the firmware union (P2P client)  */
   } u;
} mac_ctx_cmd_t;
#define FW_MAC_TYPE_BSS_STA     5UL
#define TSF_ID_A                0UL
#define MAC_FILTER_ACCEPT_GRP   0x04UL
#define MAC_FILTER_IN_BEACON    0x40UL
#define MAC_QOS_FLG_UPDATE_EDCA 0x01UL
#define MAC_QOS_FLG_TGN         0x02UL
#define MAC_FLG_SHORT_SLOT      0x10UL
#define MAC_FLG_SHORT_PREAMBLE  0x20UL

typedef struct {
   ULONG  id_and_color, action, id, apply_time, max_delay, depends_on, interval, duration;
   UCHAR  repeat, max_frags;
   USHORT policy;
} time_event_cmd_t;
typedef struct { ULONG status, id, unique_id, id_and_color; } time_event_resp_t;
typedef struct { ULONG timestamp, session_id, unique_id, id_and_color, action, status; } time_event_notif_t;
#define TE_BSS_STA_AGGRESSIVE_ASSOC 0UL
#define TE_V2_FRAG_NONE         0
#define TE_V2_NOTIF_HOST_EVENT_START 0x0001
#define TE_V2_NOTIF_HOST_EVENT_END   0x0002
#define T2_V2_START_IMMEDIATELY 0x0800

typedef struct { ULONG id_and_color, quota, max_duration, low_latency; } quota_t;   /* v1: no low_latency */
#define MAX_BINDINGS            4
#define MAX_QUOTA               128

#define DEVICE_POWER_FLAGS_POWER_SAVE_ENA_MSK 0x0001
#define POWER_FLAGS_POWER_SAVE_ENA_MSK        0x0001
#define POWER_FLAGS_POWER_MANAGEMENT_ENA_MSK  0x0002
#define PS_DATA_TIMEOUT_US      100000UL        /* stay awake this long after a frame in either direction       */
typedef struct { USHORT flags, reserved; } device_power_cmd_t;
typedef struct {
   ULONG  id_and_color;
   USHORT flags, keep_alive_seconds;
   ULONG  rx_data_timeout, tx_data_timeout, rx_data_timeout_uapsd, tx_data_timeout_uapsd;
   UCHAR  lprx_rssi_threshold, skip_dtim_periods;
   USHORT snooze_interval, snooze_window;
   UCHAR  snooze_step, qndp_tid, uapsd_ac_flags, uapsd_max_sp;
   UCHAR  heavy_tx_thld_packets, heavy_rx_thld_packets, heavy_tx_thld_percentage, heavy_rx_thld_percentage;
   UCHAR  limited_ps_threshold, reserved;
} mac_power_cmd_t;
typedef struct { ULONG v[11]; } beacon_filter_cmd_t;
typedef struct { UCHAR filter_own, port_id, count, pass_all; UCHAR bssid[6]; UCHAR reserved[2]; } mcast_filter_cmd_t;
typedef struct { ULONG num_temps; USHORT thresholds[8]; } temp_report_ths_cmd_t;   /* GRP_PHY_TEMP_REPORTING_THRESHOLDS_CMD */
typedef struct { ULONG mode, enabled_modules; } bt_coex_cmd_t;
#define BT_COEX_WIFI            0x3UL
#define BT_COEX_HIGH_BAND_RET   0x10UL
typedef struct { USHORT mcc; UCHAR source_id, reserved; ULONG key; ULONG reserved2[5]; } mcc_update_cmd_t;
#define MCC_UPDATE_CMD_V1_SIZE  4
#define MCC_SOURCE_OLD_FW       0
#define MCC_SOURCE_GET_CURRENT  0x10

typedef struct {
   UCHAR  sta_id, reserved1;
   USHORT control;
   UCHAR  flags, mimo_delim, single_stream_ant_msk, dual_stream_ant_msk;
   UCHAR  initial_rate_index[4];
   USHORT agg_time_limit;
   UCHAR  agg_disable_start_th, agg_frame_cnt_limit;
   ULONG  reserved2;
   ULONG  rs_table[16];
   ULONG  bf_params;
} lq_cmd_t;
#define ANT_A                   0x01
#define ANT_B                   0x02
#define ANT_AB                  0x03

typedef struct {
   USHORT len, offload_assist;
   ULONG  tx_flags;
   UCHAR  try_cnt, btkill_cnt;
   USHORT scratch_reserved;
   ULONG  rate_n_flags;
   UCHAR  sta_id, sec_ctl, initial_rate_index, reserved2;
   UCHAR  key[16];
   ULONG  reserved3, life_time, dram_lsb_ptr;
   UCHAR  dram_msb_ptr, rts_retry_limit, data_retry_limit, tid_tspec;
   USHORT pm_frame_timeout, reserved4;
} tx_cmd_t;                                     /* 56 bytes, then the 802.11 header                       */
#define TX_CMD_FLG_PROT_REQUIRE 0x00000001UL
#define TX_CMD_FLG_ACK          0x00000008UL
#define TX_CMD_FLG_STA_RATE     0x00000010UL
#define TX_CMD_FLG_BT_DIS       0x00001000UL
#define TX_CMD_FLG_SEQ_CTL      0x00002000UL
#define TX_CMD_FLG_MH_PAD       0x00100000UL
#define TX_CMD_OFFLD_PAD        0x2000
#define TX_CMD_SEC_CCM          0x02
#define TX_CMD_LIFE_TIME_INFINITE 0xFFFFFFFFUL
#define RTS_DFAULT_RETRY_LIMIT  3
#define MGMT_DFAULT_RETRY_LIMIT 3
#define LOW_RETRY_LIMIT         7
#define TX_STATUS_MSK           0x00FF
#define TX_STATUS_SUCCESS       0x01
#define TX_STATUS_DIRECT_DONE   0x02

typedef struct {
   UCHAR  frame_count, bt_kill_count, failure_rts, failure_frame;
   ULONG  initial_rate;
   USHORT wireless_media_time;
   UCHAR  pa_status, pa_integ_res_a[3], pa_integ_res_b[3], pa_integ_res_c[3];
   USHORT measurement_req_id, reserved;
   ULONG  tfd_info;
   USHORT seq_ctl, byte_cnt;
   UCHAR  tlc_info, ra_tid;
   USHORT frame_ctrl;
   USHORT status;                               /* agg_tx_status: status, idx, qid                        */
   UCHAR  st_idx, st_qid;
} tx_resp_t;

typedef struct {
   UCHAR  non_cfg_phy_cnt, cfg_phy_cnt, stat_id, reserved1;
   ULONG  system_timestamp;
   ULONG  timestamp_lo, timestamp_hi;
   ULONG  beacon_time_stamp;
   USHORT phy_flags, channel;
   ULONG  non_cfg_phy[8];
   ULONG  rate_n_flags, byte_count;
   USHORT mac_active_msk, frame_time;
} rx_phy_info_t;
#define RX_MPDU_RES_STATUS_CRC_OK       0x00000001UL
#define RX_MPDU_RES_STATUS_OVERRUN_OK   0x00000002UL
#define RX_MPDU_RES_STATUS_MIC_OK       0x00000040UL
#define RX_MPDU_RES_STATUS_SEC_ENC_MSK  0x00000700UL
#define RX_MPDU_RES_STATUS_SEC_NO_ENC   0x00000000UL
#define RX_MPDU_RES_STATUS_SEC_CCM_ENC  0x00000200UL
#define RX_MPDU_RES_STATUS_SEC_ENC_ERR  0x00000700UL
#define RX_MPDU_RES_STATUS_DEC_DONE     0x00000800UL

typedef struct {                                /* UMAC scan configuration (+ channel_array[n])           */
   ULONG  flags, tx_chains, rx_chains, legacy_rates, out_of_channel_time, suspend_time;
   UCHAR  dwell_active, dwell_passive, dwell_fragmented, dwell_extended;
   UCHAR  mac_addr[6];
   UCHAR  bcast_sta_id, channel_flags;
} scan_config_t;
#define SCAN_CONFIG_FLAG_ACTIVATE           0x00000001UL
#define SCAN_CONFIG_FLAG_ALLOW_CHUB_REQS    0x00000008UL
#define SCAN_CONFIG_FLAG_SET_TX_CHAINS      0x00000100UL
#define SCAN_CONFIG_FLAG_SET_RX_CHAINS      0x00000200UL
#define SCAN_CONFIG_FLAG_SET_AUX_STA_ID     0x00000400UL
#define SCAN_CONFIG_FLAG_SET_ALL_TIMES      0x00000800UL
#define SCAN_CONFIG_FLAG_SET_CHANNEL_FLAGS  0x00002000UL
#define SCAN_CONFIG_FLAG_SET_LEGACY_RATES   0x00004000UL
#define SCAN_CONFIG_FLAG_SET_MAC_ADDR       0x00008000UL
#define SCAN_CONFIG_FLAG_CLEAR_FRAGMENTED   0x00020000UL
#define SCAN_CONFIG_RATES_ALL               0x00000FFFUL    /* 6..54 (bits 0-7), 1, 2, 5.5, 11 (bits 8-11) */

typedef struct { ULONG flags; UCHAR channel_num, iter_count; USHORT iter_interval; } scan_chan_umac_t;
typedef struct { USHORT offset, len; } probe_seg_t;
#define SCAN_OFFLOAD_PROBE_REQ_SIZE 512
#define PROBE_OPTION_MAX        20
#define MAX_SCHED_SCAN_PLANS    2
#define SCAN_REQ_UMAC_SIZE_V1   36
#define SCAN_REQ_UMAC_SIZE_V7   48
#define SCAN_REQ_UMAC_SIZE_V8   52
#define SCAN_TAIL_V1_SIZE       (8 + 4 + (4 + 8 + 4 + 512) + 20 * 34)    /* 1220                          */
#define SCAN_TAIL_V2_SIZE       (8 + 4 + (4 + 12 + 4 + 512) + 20 * 34)   /* 1224                          */
#define UMAC_SCAN_GEN_FLAGS_PASS_ALL        0x0004
#define UMAC_SCAN_GEN_FLAGS_PASSIVE         0x0008
#define UMAC_SCAN_GEN_FLAGS_PRE_CONNECT     0x0010
#define UMAC_SCAN_GEN_FLAGS_ITER_COMPLETE   0x0020
#define UMAC_SCAN_GEN_FLAGS_RRM_ENABLED     0x0100
#define UMAC_SCAN_GEN_FLAGS_EXTENDED_DWELL  0x0400
#define UMAC_SCAN_GEN_FLAGS_ADAPTIVE_DWELL  0x2000
#define SCAN_PRIORITY_HIGH      2UL

typedef struct { ULONG mac_id, consec_missed_beacons_since_last_rx, consec_missed_beacons, num_expected_beacons,
                 num_recvd_beacons; } missed_beacons_notif_t;
typedef struct { ULONG error_type; UCHAR cmd_id, reserved1; USHORT bad_cmd_seq_num; ULONG error_service;
                 ULONG ts_lo, ts_hi; } error_resp_t;

#pragma pack(pop)

/* Rate codes (rate_n_flags) */
#define RATE_MCS_HT_MSK         0x00000100UL
#define RATE_MCS_CCK_MSK        0x00000200UL
#define RATE_MCS_SGI_MSK        0x00002000UL
#define RATE_MCS_CHAN_WIDTH_40  0x00000800UL
#define RATE_MCS_CHAN_WIDTH_80  0x00001000UL
#define RATE_MCS_VHT_MSK        0x04000000UL
#define RATE_VHT_MCS_NSS_POS    4
#define RATE_MCS_RTS_REQUIRED_MSK 0x40000000UL
#define RATE_MCS_ANT_A_MSK      0x00004000UL
#define RATE_MCS_ANT_B_MSK      0x00008000UL
#define RATE_MCS_ANT_AB_MSK     0x0000C000UL

/* =================================================================================================== */
/* Internal API                                                                                        */
/* =================================================================================================== */
/* iwl_trans.c: PCIe transport (generation 1: 7000 / 8000 / 9000 without multi-queue RX) */
ULONG  iwl_rd(USHORT reg);
void   iwl_wr(USHORT reg, ULONG v);
void   iwl_set(USHORT reg, ULONG bits);
void   iwl_clr(USHORT reg, ULONG bits);
int    iwl_poll(USHORT reg, ULONG bits, ULONG mask, USHORT us);   /* 1 = matched within us microseconds */
ULONG  iwl_prph_rd(ULONG addr);
void   iwl_prph_wr(ULONG addr, ULONG v);
void   iwl_prph_set(ULONG addr, ULONG bits);
void   iwl_prph_clr(ULONG addr, ULONG bits);
int    iwl_mem_wr(ULONG addr, UCHAR FAR *src, USHORT dwords);   /* src NULL = zeroes                   */
int    iwl_mem_wr32(ULONG addr, ULONG v);
int    iwl_nic_lock(void);
void   iwl_nic_unlock(void);
int    iwl_trans_alloc(void);                   /* DMA memory for rings and buffers (first bring-up)     */
int    iwl_prepare_card_hw(void);
int    iwl_start_hw(void);
void   iwl_stop_device(void);
int    iwl_load_ucode(USHORT type);             /* NIC init, firmware download, wait for ALIVE           */
int    iwl_post_alive(void);
int    iwl_enable_txq(USHORT sta, USHORT qid, USHORT fifo, USHORT tid);
void   iwl_check_rfkill(void);
extern USHORT g_cmdq, g_alive, g_alive_ok, g_rfkill, g_fw_error, g_intmask_on;
extern ULONG  g_sched_base, g_hw_rev, g_err_table, g_umac_err_table;
extern USHORT g_fw_chunk_done;

/* commands (iwl_trans.c) */
#define CMDF_WANT_RESP  0x0001
int    iwl_cmd(USHORT id, UCHAR FAR *d0, USHORT l0, UCHAR FAR *d1, USHORT l1, USHORT flags);
int    iwl_cmd1(USHORT id, UCHAR FAR *d, USHORT len);
int    iwl_cmd_async(USHORT id, UCHAR FAR *d, USHORT len);   /* never waits; for the receive path */
extern USHORT g_async_ok, g_async_fail;
int    iwl_cmd_status(USHORT id, UCHAR FAR *d, USHORT len, ULONG FAR *status);   /* status may be on the stack */
int    iwl_mem_rd(ULONG addr, ULONG FAR *dst, USHORT dwords);
void   iwl_dump_fw_error(void);                 /* read the firmware error tables into g_errlog / g_uerrlog */
#define ERRLOG_DW   36
#define UERRLOG_DW  15
extern ULONG  g_errlog[ERRLOG_DW], g_uerrlog[UERRLOG_DW];
extern USHORT g_errlog_ok, g_uerrlog_ok;
#define CLOG_N      12
typedef struct { USHORT id; short rc; ULONG info; } clog_t;   /* a host command and how it ended              */
extern clog_t g_clog[CLOG_N];
extern USHORT g_clog_n;
extern ULONG  g_cmd_err;                        /* error_type of the last REPLY_ERROR answer                */
extern UCHAR FAR *g_resp;                       /* payload of the last response (CMDF_WANT_RESP)          */
extern USHORT g_resp_len, g_resp_failed;

/* data transmit (iwl_trans.c) */

void   iwl_tx_reclaim(UCHAR qid, UCHAR idx);
/* data queues (QI_BE, QI_AGG): a free slot, put a frame in it (bell 1 = tell the device now), bookkeeping */
UCHAR FAR *iwl_txq_slot(USHORT qi, USHORT *idx, ULONG *phys);
int    iwl_txq_kick(USHORT qi, USHORT idx, USHORT cmdlen, USHORT paylen, USHORT totlen, USHORT bell);
void   iwl_txq_bell(USHORT qi);
void   iwl_txq_reset(USHORT qi, USHORT idx);     /* an aggregation queue starts at ring index idx            */
void   iwl_txq_reclaim_to(USHORT qi, USHORT idx);   /* free the slots before ring index idx                   */
USHORT iwl_txq_queued(USHORT qi);
USHORT iwl_txq_cur(USHORT qi);
USHORT iwl_txq_qid(USHORT qi);
int    iwl_txq_present(USHORT qi);
extern USHORT g_cmd_last_idx;                    /* ring index of the command queued last (asynchronous ones) */
void   iwl_trans_off(void);
USHORT iwl_rx_cur(void);
extern USHORT g_hw_err, g_svc_runs, g_rx_pkts, g_rx_bad, g_last_code, g_cmd_to;

/* iwl_fw.c */
int    iwl_fw_parse(void);

/* iwl_mvm.c: the operation mode (firmware conversation) */
void   iwl_rx_pkt(USHORT code, UCHAR qid, UCHAR idx, UCHAR FAR *p, USHORT len);
extern USHORT g_init_complete, g_calib_complete;
UCHAR  iwl_valid_tx_ant(void);
UCHAR  iwl_valid_rx_ant(void);
int    iwl_send_phy_ctxt(USHORT action, UCHAR ch, UCHAR chains_static, UCHAR chains_dynamic, UCHAR width, UCHAR ctrl);   /* width: PHY_VHT_CHANNEL_MODE*, ctrl: position of the primary channel in it */
int    iwl_send_sf(ULONG state, USHORT assoc);
int    iwl_send_power(void);
int    iwl_send_quota(USHORT running);

/* iwl_nvm.c */
int    iwl_nvm_init(void);
extern UCHAR  g_nvm_chan[64];
extern USHORT g_nvm_flags[64];                   /* usable channels (scan list)                            */
extern USHORT g_nvm_nchan;
extern UCHAR  g_nvm_5ghz, g_nvm_11n, g_nvm_11ac, g_nvm_tx_ant, g_nvm_rx_ant, g_nvm_lar;
extern USHORT g_nvm_version;

/* iwl_phydb.c */
void   iwl_phydb_reset(void);
void   iwl_phydb_store(UCHAR FAR *p, USHORT len);
int    iwl_phydb_send(void);
extern USHORT g_phydb_n;

/* iwl_scan.c */
int    iwl_scan_config(void);
void   iwl_scan_done(void);
extern USHORT g_scan_done, g_scan_n;

/* iwl_mac.c: join, keys, data */
/* iwl_agg.c: transmit aggregation (A-MPDU) */
int    txa_pick(USHORT *qi);                    /* which queue the next QoS data frame uses (0 = ok, -2 = busy) */
USHORT txa_alloc_sn(void);                      /* the next QoS sequence number (TID 0)                       */
int    txa_bell_ok(USHORT qi);                  /* may the device be told about a frame put on queue qi?      */
void   txa_cfg_answer(UCHAR idx, int ok);       /* the firmware answered the ADD_STA that completes the setup  */
void   txa_ba_notif(UCHAR FAR *p, USHORT len);  /* BA_NOTIF: the firmware's block-ack window moved             */
void   txa_tx_resp(UCHAR qid, UCHAR FAR *p, USHORT len);   /* TX_CMD answer of the aggregation queue          */
void   txa_reset(void);                         /* link down                                                  */
void   txa_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void));
void   rs_resp(UCHAR failed_attempts);           /* rate adaptation: transmit results and the periodic decision */
void   rs_ba(UCHAR txed, UCHAR done);
void   rs_tick(void);
void   rs_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void));
/* 9000 family (iwl_ctxt.c) and the hooks it needs in iwl_trans.c */
int    iwl9_alloc(void);
void   iwl9_reset(void);
void   iwl9_rx_init(void);
void   iwl9_rx_service(void);
void   iwl9_rx_stop(void);
int    iwl9_boot(USHORT type);
void   iwl9_free_images(void);
void   iwl9_free_all(void);
void   iwl9_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void));
ULONG  iwl_rxbuf_phys(USHORT i);
void   iwl_rx_buffer(USHORT i);
void   iwl_set_mask(ULONG m);
ULONG  iwl_rbstat_phys(void);
UCHAR FAR *iwl_rbstat_ptr(void);
ULONG  iwl_cmdq_ring_phys(void);
extern USHORT g_hwtxlk;                         /* the data queues: transmit (any context) and reclaim (service) */
extern UCHAR  g_ampdu_par;                      /* the AP's HT capabilities A-MPDU parameters (association response) */
void   iwl_rx_mpdu(UCHAR FAR *p, USHORT len);
void   iwl_rx_phy(UCHAR FAR *p, USHORT len);
void   iwl_tx_done(UCHAR qid, UCHAR idx, UCHAR FAR *p, USHORT len);
void   iwl_te_notif(UCHAR FAR *p, USHORT len);
void   iwl_mac_reset(void);
extern USHORT g_tx_ok, g_tx_fail, g_rx_mpdu, g_rx_crc, g_rx_decfail;
extern USHORT g_tx_last_st, g_tx_last_fail, g_tx_st_hist[4];
extern ULONG  g_tx_last_rate;

#endif
