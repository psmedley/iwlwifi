/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_trans.c - the iwlwifi PCIe transport, generation 1 (7000 / 8000 / 9000 families with the legacy receive
 * queue): register access, power-up (APM), receive and transmit rings, the TX scheduler, firmware download
 * through the FH service channel, the interrupt, the service loop that drains the receive ring, host commands.
 *
 * Port of OpenBSD if_iwm.c (ISC licence) - iwm_nic_lock, iwm_apm_init, iwm_start_hw, iwm_stop_device,
 * iwm_nic_rx_legacy_init, iwm_nic_tx_init, iwm_post_alive, iwm_enable_ac_txq, iwm_enable_txq,
 * iwm_firmware_load_chunk, iwm_load_cpu_sections_8000, iwm_send_cmd, iwm_update_sched, iwm_intr,
 * iwm_notif_intr, iwm_rx_pkt - cross-checked with Linux iwlwifi pcie/trans.c, rx.c, tx.c.
 *
 * Differences from iwm, because this is a 16-bit OS/2 driver:
 *   - no ICT table: the interrupt causes are read from CSR_INT / CSR_FH_INT_STATUS (the mode the hardware
 *     starts in, which iwlwifi uses until ALIVE);
 *   - 64 receive buffers of 4 KB are posted in the 256-entry descriptor ring (Linux posts as many as it has);
 *   - host commands are synchronous; one 4 KB buffer serves the commands longer than 320 bytes;
 *   - firmware sections are copied in 32 KB pieces (iwlwifi falls back to page-sized pieces itself).
 */
#include "iwl.h"

/* ---- registers -------------------------------------------------------------------------------------- */
ULONG iwl_rd(USHORT reg)            { return mmio_rd((ULONG)reg); }
void  iwl_wr(USHORT reg, ULONG v)   { mmio_wr((ULONG)reg, v); }
void  iwl_set(USHORT reg, ULONG b)  { mmio_wr((ULONG)reg, mmio_rd((ULONG)reg) | b); }
void  iwl_clr(USHORT reg, ULONG b)  { mmio_wr((ULONG)reg, mmio_rd((ULONG)reg) & ~b); }

/* 1 when (reg & mask) == (bits & mask) within about us microseconds (polled every 10 us). */
int iwl_poll(USHORT reg, ULONG bits, ULONG mask, USHORT us)
{
   for (;;) {
      if ((iwl_rd(reg) & mask) == (bits & mask))
         return 1;
      if (us < 10)
         return 0;
      us = (USHORT)(us - 10);
      dd_iodelay(10);
   }
}

static int poll_long(USHORT reg, ULONG bits, ULONG mask, USHORT ms)
{
   USHORT i;

   for (i = 0; i < ms; i++)
      if (iwl_poll(reg, bits, mask, 1000))
         return 1;
   return 0;
}

/* NIC access: the device may only be touched beyond the CSR block while MAC_ACCESS_REQ holds it awake. */
static USHORT g_nic_locks;

int iwl_nic_lock(void)
{
   if (g_nic_locks) {
      g_nic_locks++;
      return 1;
   }
   iwl_set(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
   if (g_cfg->family >= IWL_FAM_8000)
      dd_iodelay(2);
   if (poll_long(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN,
                 CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY | CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP, 150)) {
      g_nic_locks++;
      return 1;
   }
   dd_err("acquiring the device failed, GP_CNTRL", iwl_rd(CSR_GP_CNTRL));
   return 0;
}

void iwl_nic_unlock(void)
{
   if (g_nic_locks && --g_nic_locks == 0)
      iwl_clr(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

static ULONG prph_rd_unlocked(ULONG addr)
{
   iwl_wr(HBUS_TARG_PRPH_RADDR, (addr & 0x000FFFFFUL) | 0x03000000UL);
   return iwl_rd(HBUS_TARG_PRPH_RDAT);
}

static void prph_wr_unlocked(ULONG addr, ULONG v)
{
   iwl_wr(HBUS_TARG_PRPH_WADDR, (addr & 0x000FFFFFUL) | 0x03000000UL);
   iwl_wr(HBUS_TARG_PRPH_WDAT, v);
}

ULONG iwl_prph_rd(ULONG addr)
{
   ULONG v = 0xFFFFFFFFUL;

   if (iwl_nic_lock()) {
      v = prph_rd_unlocked(addr);
      iwl_nic_unlock();
   }
   return v;
}

void iwl_prph_wr(ULONG addr, ULONG v)
{
   if (iwl_nic_lock()) {
      prph_wr_unlocked(addr, v);
      iwl_nic_unlock();
   }
}

void iwl_prph_set(ULONG addr, ULONG bits)
{
   if (iwl_nic_lock()) {
      prph_wr_unlocked(addr, prph_rd_unlocked(addr) | bits);
      iwl_nic_unlock();
   }
}

void iwl_prph_clr(ULONG addr, ULONG bits)
{
   if (iwl_nic_lock()) {
      prph_wr_unlocked(addr, prph_rd_unlocked(addr) & ~bits);
      iwl_nic_unlock();
   }
}

int iwl_mem_wr(ULONG addr, UCHAR FAR *src, USHORT dwords)
{
   USHORT i;

   if (!iwl_nic_lock())
      return -1;
   iwl_wr(HBUS_TARG_MEM_WADDR, addr);           /* auto-increments                                       */
   for (i = 0; i < dwords; i++)
      iwl_wr(HBUS_TARG_MEM_WDAT, src ? get32(src + i * 4) : 0UL);
   iwl_nic_unlock();
   return 0;
}

int iwl_mem_rd(ULONG addr, ULONG FAR *dst, USHORT dwords)
{
   USHORT i;

   if (!iwl_nic_lock())
      return -1;
   iwl_wr(HBUS_TARG_MEM_RADDR, addr);           /* auto-increments                                       */
   for (i = 0; i < dwords; i++)
      dst[i] = iwl_rd(HBUS_TARG_MEM_RDAT);
   iwl_nic_unlock();
   return 0;
}

int iwl_mem_wr32(ULONG addr, ULONG v)
{
   static UCHAR b[4];

   put32((UCHAR FAR *)b, v);
   return iwl_mem_wr(addr, (UCHAR FAR *)b, 1);
}

/* ---- state ------------------------------------------------------------------------------------------- */
USHORT g_cmdq, g_alive, g_alive_ok, g_rfkill, g_fw_error, g_intmask_on, g_fw_chunk_done;
ULONG  g_sched_base, g_hw_rev, g_err_table, g_umac_err_table;
static ULONG  g_intmask;
static USHORT g_trans_on;                       /* registers mapped, device powered: interrupts may be ours */
static USHORT g_irq_masked;
static USHORT g_auxq, g_dataq;
void   iwl_cmd_answer(USHORT code, UCHAR idx, UCHAR flags, UCHAR FAR *p, USHORT len);                  /* DQA: aux 1, data 5..8; legacy: aux 15, data 0..3       */
USHORT g_hw_err, g_svc_runs, g_rx_pkts, g_rx_bad, g_last_code, g_cmd_to;
USHORT g_rxlog[32][2], g_rxlog_n;               /* the last 32 received packets: code, length (status text RXLOG) */
ULONG  g_rx_lnf;                                /* len_n_flags of the packet being handled                  */

/* ---- DMA memory ------------------------------------------------------------------------------------ */
#define NTXQ        7                           /* cmd, aux, 4 data (one per access category), TID 0 aggregation */
#define CMD_SLOTS   32
#define CMD_SLOT_SZ (4 + DEF_CMD_PAYLOAD_SIZE)  /* header + payload                                       */

typedef struct {
   USHORT qid;
   USHORT cur, tail, queued;
   DMA    tfd;                                  /* 256 TFDs of 128 bytes, 256-byte aligned                */
   DMA    buf;                                  /* slots (command queue, the BE data queue)              */
   USHORT nslot, slotsz;
   UCHAR  busy[TX_SLOTS];                       /* data queue: slot awaits its TX response               */
   UCHAR  ok;                                   /* the ring exists (the aggregation queue is optional)    */
} txq_t;

static txq_t  g_txq[NTXQ];
static DMA    g_kw, g_sched, g_rbd, g_rbstat, g_bigcmd, g_respbuf, g_fwdma;
USHORT g_mq_nbuf = RX_NBUF;                     /* receive buffers posted on the multi-queue families (what the memory allowed) */
static ULONG  g_rxblk[MQ_NBUF / 16];            /* blocks of 64 KB, 16 buffers each (4 used on the older families, 16 on the multi-queue ones) */
static USHORT g_rxcur;
static USHORT g_alloc_done;
UCHAR FAR    *g_resp;
USHORT        g_resp_len, g_resp_failed;

ULONG iwl_rxbuf_phys(USHORT i);
static ULONG rxbuf_phys(USHORT i)
{
   i &= (USHORT)((g_cfg->family >= IWL_FAM_9000 ? g_mq_nbuf : RX_NBUF) - 1);
   return g_rxblk[i >> 4] + (ULONG)((i & 15) << 12);
}

ULONG iwl_rxbuf_phys(USHORT i)    { return rxbuf_phys(i); }
ULONG iwl_rbstat_phys(void)        { return g_rbstat.phys; }
UCHAR FAR *iwl_rbstat_ptr(void)    { return DPTR(g_rbstat); }
ULONG iwl_cmdq_ring_phys(void)     { return g_txq[0].tfd.phys; }

static txq_t *txq_of(USHORT qid)
{
   USHORT i;

   for (i = 0; i < NTXQ; i++)
      if (g_txq[i].qid == qid)
         return &g_txq[i];
   return 0;
}

int iwl_trans_alloc(void)
{
   USHORT i;
   int    dqa = FW_CAPA(CAPA_DQA_SUPPORT);

   g_cmdq  = (USHORT)((dqa || g_cfg->family == IWL_FAM_22000) ? DQA_CMD_QUEUE : LEGACY_CMD_QUEUE);
   g_auxq  = (USHORT)(dqa ? DQA_AUX_QUEUE : LEGACY_AUX_QUEUE);
   g_dataq = (USHORT)(dqa ? DQA_MIN_MGMT_QUEUE : 0);
   g_txq[0].qid = g_cmdq;
   g_txq[1].qid = g_auxq;
   for (i = 0; i < 4; i++)
      g_txq[2 + i].qid = (USHORT)(g_dataq + i);
   g_txq[QI_AGG].qid = (USHORT)(dqa ? DQA_MIN_DATA_QUEUE : 0xFFFF);   /* transmit aggregation needs DQA firmware */
   if (g_cfg->family == IWL_FAM_22000)
      g_txq[QI_BE].qid = 0xFFFF;                   /* the firmware numbers the data queue when it creates it */
   if (g_alloc_done)
      return 0;
   if (dma_get_al(4096, 4096, &g_kw) ||
       dma_get_al((USHORT)(IWL_MAX_QUEUES * TFD_QUEUE_BC_SIZE * 2), 1024, &g_sched) ||
       dma_get_al(RX_RING_COUNT * 4, 256, &g_rbd) ||
       dma_get_al(16, 16, &g_rbstat) ||
       dma_get_al(4096, 16, &g_bigcmd) ||
       dma_get_al(4096, 16, &g_respbuf) ||
       dma_get_al(32768U, 16, &g_fwdma))
      return -1;
   for (i = 0; i < QI_AGG; i++) {
      if (g_cfg->family == IWL_FAM_22000) {          /* gen2: one data queue (the BE one), its ring comes from iwl_gen2.c */
         g_txq[i].ok = 1;
         continue;
      }
      if (dma_get_al(TFD_QUEUE_SIZE_MAX * TFD_SIZE, 256, &g_txq[i].tfd))
         return -2;
      g_txq[i].ok = 1;
   }
   g_txq[0].nslot = CMD_SLOTS;
   g_txq[0].slotsz = CMD_SLOT_SZ;
   if (dma_get_al(CMD_SLOTS * CMD_SLOT_SZ, 16, &g_txq[0].buf))
      return -3;
   g_txq[2].nslot = TX_SLOTS;
   g_txq[2].slotsz = TX_SLOT_SZ;
   if (dma_get_al(TX_SLOTS * TX_SLOT_SZ, 64, &g_txq[2].buf))
      return -4;
   if (dqa && g_cfg->family != IWL_FAM_22000 &&     /* the aggregation queue: a ring and its own slots; without memory the driver simply does not aggregate */
       dma_get_al(TFD_QUEUE_SIZE_MAX * TFD_SIZE, 256, &g_txq[QI_AGG].tfd) == 0) {
      if (dma_get_al(TX_SLOTS * TX_SLOT_SZ, 64, &g_txq[QI_AGG].buf) == 0) {
         g_txq[QI_AGG].nslot = TX_SLOTS;
         g_txq[QI_AGG].slotsz = TX_SLOT_SZ;
         g_txq[QI_AGG].ok = 1;
      }
   }
   {
      USHORT want = (USHORT)((g_cfg->family >= IWL_FAM_9000 ? MQ_NBUF : RX_NBUF) / 16), got;

      for (got = 0; got < want; got++)
         if ((g_rxblk[got] = dma_big_al(0x10000UL, 4096)) == 0UL)
            break;
      if (got < 4)
         return -5;
      while (got & (got - 1))                       /* a power of two (4, 8, 16 blocks): 64, 128 or 256 buffers */
         got--;
      g_mq_nbuf = (USHORT)(got * 16);
   }
   if ((g_cfg->family == IWL_FAM_9000 || g_cfg->family == IWL_FAM_22000) && iwl9_alloc())
      return -6;
   if (g_cfg->family == IWL_FAM_22000 && iwl22_alloc())
      return -7;
   g_resp = DPTR(g_respbuf);
   g_alloc_done = 1;
   return 0;
}

static void reset_rings(void)
{
   USHORT i;

   for (i = 0; i < NTXQ; i++) {
      if (!g_txq[i].ok)
         continue;
      if (g_cfg->family != IWL_FAM_22000)          /* gen2: the rings are in iwl_gen2.c */
         fmemset(DPTR(g_txq[i].tfd), 0, TFD_QUEUE_SIZE_MAX * TFD_SIZE);
      g_txq[i].cur = g_txq[i].tail = g_txq[i].queued = 0;
      fmemset((UCHAR FAR *)g_txq[i].busy, 0, TX_SLOTS);
   }
   fmemset(DPTR(g_sched), 0, (USHORT)(IWL_MAX_QUEUES * TFD_QUEUE_BC_SIZE * 2));
   fmemset(DPTR(g_rbstat), 0, 16);
   g_rxcur = 0;
   if (g_cfg->family == IWL_FAM_9000 || g_cfg->family == IWL_FAM_22000)
      iwl9_reset();
}

/* ---- interrupts --------------------------------------------------------------------------------------- */
static void set_mask(ULONG m)
{
   g_intmask = m;
   iwl_wr(CSR_INT_MASK, m);
}

void iwl_set_mask(ULONG m)
{
   set_mask(m);
}

static void disable_interrupts(void)
{
   iwl_wr(CSR_INT_MASK, 0);
   iwl_wr(CSR_INT, 0xFFFFFFFFUL);
   iwl_wr(CSR_FH_INT_STATUS, 0xFFFFFFFFUL);
}

void iwl_check_rfkill(void)
{
   /* GP_CNTRL bit 27 (HW_RF_KILL_SW) reads 0 while the radio switch is OFF (iwm_check_rfkill). */
   g_rfkill = (USHORT)((iwl_rd(CSR_GP_CNTRL) & CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW) == 0UL);
}

/* Interrupt time: is the (shared) line ours? Then mask the device until the context hook has serviced it. */
USHORT hw_irq(void)
{
   ULONG r;

   if (!g_trans_on)
      return 0;
   r = iwl_rd(CSR_INT);
   if (r == 0xFFFFFFFFUL || (r & 0xFFFFFFF0UL) == 0xA5A5A5A0UL)
      return 0;
   if (!(r & g_intmask))
      return 0;
   iwl_wr(CSR_INT_MASK, 0);
   g_irq_masked = 1;
   return 1;
}

/* ---- receive ------------------------------------------------------------------------------------------- */
static void rx_init(void)
{
   USHORT i, n;
   UCHAR FAR *d = DPTR(g_rbd);

   fmemset(d, 0, RX_RING_COUNT * 4);
   for (i = 0; i < RX_NBUF; i++)
      put32(d + i * 4, rxbuf_phys(i) >> 8);
   fmemset(DPTR(g_rbstat), 0, 16);
   g_rxcur = 0;

   if (iwl_nic_lock()) {                          /* stop the receive DMA, then program the ring           */
      iwl_wr(FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);
      for (n = 0; n < 1000; n++) {
         if (iwl_rd(FH_MEM_RSSR_RX_STATUS_REG) & 0x01000000UL)   /* FH_RSSR_CHNL0_RX_STATUS_CHNL_IDLE      */
            break;
         dd_iodelay(10);
      }
      iwl_nic_unlock();
   }
   if (!iwl_nic_lock())
      return;
   iwl_wr(FH_MEM_RCSR_CHNL0_RBDCB_WPTR, 0);
   iwl_wr(FH_MEM_RCSR_CHNL0_FLUSH_RB_REQ, 0);
   iwl_wr(FH_RSCSR_CHNL0_RDPTR, 0);
   iwl_wr(FH_RSCSR_CHNL0_WPTR, 0);
   iwl_wr(FH_RSCSR_CHNL0_RBDCB_BASE_REG, g_rbd.phys >> 8);
   iwl_wr(FH_RSCSR_CHNL0_STTS_WPTR_REG, g_rbstat.phys >> 4);
   iwl_wr(FH_MEM_RCSR_CHNL0_CONFIG_REG,
          FH_RCSR_RX_CONFIG_CHNL_EN_ENABLE_VAL | FH_RCSR_CHNL0_RX_IGNORE_RXF_EMPTY |
          FH_RCSR_CHNL0_RX_CONFIG_IRQ_DEST_INT_HOST_VAL |
          ((ULONG)RX_RB_TIMEOUT << FH_RCSR_RX_CONFIG_REG_IRQ_RBTH_POS) |
          FH_RCSR_RX_CONFIG_REG_VAL_RB_SIZE_4K |
          ((ULONG)RX_QUEUE_SIZE_LOG << FH_RCSR_RX_CONFIG_RBDCB_SIZE_POS));
   mmio_wr8(CSR_INT_COALESCING, HOST_INT_TIMEOUT_DEF);
   if (g_cfg->host_int_oper)
      iwl_set(CSR_INT_COALESCING, 0x80000000UL);   /* IWM_HOST_INT_OPER_MODE (7260/3160)                    */
   iwl_nic_unlock();
   iwl_wr(FH_RSCSR_CHNL0_WPTR, RX_NBUF);           /* descriptors 0 .. RX_NBUF-1 hold buffers               */
}

/* One receive buffer: a sequence of packets, each { ULONG len_n_flags; cmd_hdr_t; data }, 64-byte aligned. */
static void rx_buffer(USHORT i)
{
   USHORT sel, off = 0, plen, total;
   UCHAR FAR *b, FAR *p;
   ULONG  lnf;
   USHORT code;

   sel = scr_map(SCR_RX, rxbuf_phys(i), RBUF_SIZE);
   if (!sel) {
      g_rx_bad++;
      return;
   }
   b = (UCHAR FAR *)MAKEP(sel, 0);
   while (off + 8 <= RBUF_SIZE) {
      p = b + off;
      lnf = get32(p);
      if (lnf == FH_RSCSR_FRAME_INVALID)
         break;
      plen = (USHORT)(lnf & FH_RSCSR_FRAME_SIZE_MSK);   /* header + data                                    */
      code = WIDE_ID(p[5] & 0x1F, p[4]);                /* hdr.flags carries the group of a wide id (and the
                                                         * CMD_FAILED bit 40h, kept out of the id)           */
      if (p[7] == 0 && p[6] == 0 && code == 0)          /* qid, idx, code all zero: end of the packets       */
         break;
      total = (USHORT)(plen + 4);
      if (plen < 4 || total > RBUF_SIZE - off)
         break;
      g_rx_pkts++;
      g_last_code = code;
      g_rxlog[g_rxlog_n & 31][0] = code;
      g_rxlog[g_rxlog_n & 31][1] = plen;
      g_rxlog_n++;
      g_rx_lnf = lnf;
      iwl_rx_pkt(code, p[7], p[6], p + 8, (USHORT)(plen - 4));
      /* the firmware sets bit 7 of qid on notifications it originates; the rest answer a host command */
      if (!(p[7] & 0x80) && p[7] == g_cmdq) {
         txq_t *q = &g_txq[0];

         if (q->queued)
            q->queued--;
         q->tail = (USHORT)((p[6] + 1) & 0xFF);
         iwl_cmd_answer(code, p[6], p[5], p + 8, (USHORT)(plen - 4));
      }
      off = (USHORT)(off + ((total + FH_RSCSR_FRAME_ALIGN - 1) & ~(FH_RSCSR_FRAME_ALIGN - 1)));
   }
   /* The buffer goes back to the device: clear it, so that what the next packets do not overwrite (aggregates differ in
    * size, packet boundaries move) can never be read as a packet. An all-zero header ends the loop above. */
   rt_setmem((PVOID)b, 0, RBUF_SIZE);
}

void iwl_rx_buffer(USHORT i)
{
   rx_buffer(i);
}

static void rx_service(void)
{
   USHORT hw, wp;
   UCHAR FAR *d;

   if (!g_trans_on)
      return;
   if (g_cfg->family == IWL_FAM_9000 || g_cfg->family == IWL_FAM_22000) {   /* multi-queue receive              */
      iwl9_rx_service();
      return;
   }
   hw = (USHORT)(get16(DPTR(g_rbstat)) & 0x0FFF);   /* closed_rb_num                                       */
   hw &= (RX_RING_COUNT - 1);
   if (hw == g_rxcur)
      return;
   d = DPTR(g_rbd);
   while (g_rxcur != hw) {
      rx_buffer(g_rxcur);
      d = DPTR(g_rbd);
      /* the buffer goes back RX_NBUF descriptors ahead */
      put32(d + ((g_rxcur + RX_NBUF) & (RX_RING_COUNT - 1)) * 4, rxbuf_phys(g_rxcur) >> 8);
      g_rxcur = (USHORT)((g_rxcur + 1) & (RX_RING_COUNT - 1));
   }
   wp = (USHORT)((g_rxcur + RX_NBUF) & (RX_RING_COUNT - 1));
   iwl_wr(FH_RSCSR_CHNL0_WPTR, (ULONG)(wp & ~7));
}

/* ---- the service loop: interrupt causes and the receive ring --------------------------------------------- */
static USHORT g_svc_lock, g_svc_again;
ULONG  g_int_seen;                              /* OR of every CSR_INT cause seen                           */
USHORT g_int_alive, g_int_rx, g_int_fhtx, g_int_other;

static void service_once(void)
{
   ULONG r1, r2;

   r1 = iwl_rd(CSR_INT);
   r2 = iwl_rd(CSR_FH_INT_STATUS);
   if (r1 == 0xFFFFFFFFUL || (r1 & 0xFFFFFFF0UL) == 0xA5A5A5A0UL)
      return;                                     /* device gone, powered down or asleep (the 9000 family answers 0xA5A5A5Ax while its MAC sleeps; the interrupt stays pending) */
   if (r1 | r2) {
      iwl_wr(CSR_INT, r1);
      iwl_wr(CSR_FH_INT_STATUS, r2);
      g_int_seen |= r1;
      if (r1 & CSR_INT_BIT_ALIVE) g_int_alive++;
      if (r1 & CSR_INT_BIT_FH_RX) g_int_rx++;
      if (r1 & CSR_INT_BIT_FH_TX) g_int_fhtx++;
      if (!(r1 & (CSR_INT_BIT_ALIVE | CSR_INT_BIT_FH_RX | CSR_INT_BIT_FH_TX))) g_int_other++;
      if (r1 & CSR_INT_BIT_SW_ERR) {
         g_fw_error++;
         dd_err("firmware error (SW_ERR), error table at", g_err_table);
         iwl_dump_fw_error();                       /* keep the error tables for the status text (also for a crash between commands) */
      }
      if (r1 & CSR_INT_BIT_HW_ERR) {
         g_hw_err++;
         dd_err("hardware error (HW_ERR), CSR_INT", r1);
      }
      if (r1 & CSR_INT_BIT_RF_KILL)
         iwl_check_rfkill();
      if ((r1 & CSR_INT_BIT_FH_TX) || (r2 & CSR_FH_INT_TX_MASK))
         g_fw_chunk_done = 1;
      if (r1 & (CSR_INT_BIT_FH_RX | CSR_INT_BIT_SW_RX | CSR_INT_BIT_RX_PERIODIC)) {
         /* the periodic interrupt is a one-shot that catches a receive left behind by coalescing */
         mmio_wr8(CSR_INT_PERIODIC_REG, CSR_INT_PERIODIC_DIS);
         if (r1 & (CSR_INT_BIT_FH_RX | CSR_INT_BIT_SW_RX))
            mmio_wr8(CSR_INT_PERIODIC_REG, CSR_INT_PERIODIC_ENA);
      }
      if (r1 & CSR_INT_BIT_ALIVE) {
         if (g_cfg->family == IWL_FAM_22000)
            iwl22_alive_restock();                /* gen2: the firmware has programmed the RFH: announce the free buffers */
      }
   }
   rx_service();
   sta_rx_tick();                                 /* block-ack reorder buffers: frames that waited too long   */
}

void hw_service(void)
{
   USHORT id = (USHORT)(rt_cpu_id() + 1);

   if (!g_trans_on)
      return;
   if (rt_lock_try(&g_svc_lock, id)) {
      g_svc_again = 1;                            /* the owner runs once more                              */
      return;
   }
   do {
      g_svc_again = 0;
      g_svc_runs++;
      service_once();
   } while (g_svc_again);
   rt_lock_release(&g_svc_lock);
}

void hw_hook(void)
{
   hw_service();
   rs_tick();
   if (g_irq_masked && g_trans_on) {
      g_irq_masked = 0;
      iwl_wr(CSR_INT_MASK, g_intmask);
   }
}

int hw_wait(USHORT *flag, USHORT ms)
{
   ULONG t0 = dd_ms();

   for (;;) {
      hw_service();
      if (*flag)
         return 0;
      if (g_fw_error)
         return -2;
      if ((long)(dd_ms() - t0) > (long)ms)
         return -1;
      dd_msleep(2);
   }
}

/* ---- power-up ------------------------------------------------------------------------------------------- */
static int set_hw_ready(void)
{
   iwl_set(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
   if (iwl_poll(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_BIT_NIC_READY, CSR_HW_IF_CONFIG_REG_BIT_NIC_READY, 50)) {
      iwl_set(CSR_MBOX_SET_REG, CSR_MBOX_SET_REG_OS_ALIVE);
      return 1;
   }
   return 0;
}

int iwl_prepare_card_hw(void)
{
   USHORT n, t;

   if (set_hw_ready())
      return 0;
   iwl_set(CSR_DBG_LINK_PWR_MGMT_REG, CSR_RESET_LINK_PWR_MGMT_DISABLED);
   dd_msleep(1);
   for (n = 0; n < 10; n++) {
      iwl_set(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_PREPARE);
      for (t = 0; t < 750; t++) {                 /* 150 ms in 200 us steps                                */
         if (set_hw_ready())
            return 0;
         dd_iodelay(200);
      }
      dd_msleep(25);
   }
   return -1;
}

static int apm_init(void)
{
   if (g_cfg->family < IWL_FAM_8000)
      iwl_set(CSR_GIO_CHICKEN_BITS, CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER);
   iwl_set(CSR_GIO_CHICKEN_BITS, CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);
   iwl_set(CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);
   iwl_set(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);
   /* L0s is unstable on these devices; the driver turned ASPM off in PCI configuration space (pci_enable),
    * so the device is told L0s is disabled (Linux iwl_pcie_apm_config: CSR_GIO_REG_VAL_L0S_DISABLED). */
   iwl_set(CSR_GIO_REG, CSR_GIO_REG_VAL_L0S_DISABLED);
   if (g_cfg->family == IWL_FAM_22000)            /* bisr_workaround: the TOP FSM may still be in the previous reset */
      dd_msleep(2);
   iwl_set(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
   if (g_cfg->family == IWL_FAM_8000)
      dd_iodelay(2);
   if (!poll_long(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, 25)) {
      dd_err("timeout waiting for the MAC clock, GP_CNTRL", iwl_rd(CSR_GP_CNTRL));
      return -1;
   }
   if (g_cfg->family == IWL_FAM_22000)
      dd_iodelay(200);                            /* bisr_workaround: BISR shift to finish                  */
   if (g_cfg->family == IWL_FAM_7000) {          /* DMA clock, L1-Active off, RF-kill interrupt in APMG   */
      iwl_prph_wr(PRPH_APMG_CLK_EN_REG, 0x00000200UL);   /* APMG_CLK_VAL_DMA_CLK_RQT                       */
      dd_iodelay(20);
      iwl_prph_set(PRPH_APMG_PCIDEV_STT_REG, 0x00000800UL);   /* APMG_PCIDEV_STT_VAL_L1_ACT_DIS            */
      iwl_prph_wr(0x301CUL, 0x00000002UL);                 /* APMG_RTC_INT_STT_REG = RFKILL               */
   }
   return 0;
}

static void apm_stop(void)
{
   iwl_set(CSR_DBG_LINK_PWR_MGMT_REG, CSR_RESET_LINK_PWR_MGMT_DISABLED);
   iwl_set(CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_PREPARE | 0x10000000UL);   /* | ENABLE_PME            */
   dd_msleep(1);
   iwl_clr(CSR_DBG_LINK_PWR_MGMT_REG, CSR_RESET_LINK_PWR_MGMT_DISABLED);
   dd_msleep(5);
   iwl_set(CSR_RESET, CSR_RESET_REG_FLAG_STOP_MASTER);
   if (!iwl_poll(CSR_RESET, CSR_RESET_REG_FLAG_MASTER_DISABLED, CSR_RESET_REG_FLAG_MASTER_DISABLED, 100))
      tstr("[iwl: timeout waiting for the bus master to stop]");
   iwl_clr(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
}

static void enable_rfkill_int(void)
{
   set_mask(CSR_INT_BIT_RF_KILL);
}

/* 9000 family (Linux iwl_trans_pcie_clear_persistence_bit): firmware left running by an earlier driver (Linux on the same
 * machine without a power cycle) sets a "persistence" bit that makes the boot ROM refuse a new image. HPM_DEBUG bit 12. */
ULONG g_hpm, g_wprot;
static void clear_persistence_bit(void)
{
   g_hpm = prph_rd_unlocked(0xA03440UL);
   g_wprot = 0UL;
   if (g_hpm != 0xA5A5A5A0UL && (g_hpm & 0x1000UL)) {
      g_wprot = prph_rd_unlocked(g_cfg->family == IWL_FAM_22000 ? 0xA04D00UL : 0xA04CE0UL);   /* PREG_PRPH_WPROT_22000 / _9000 */
      if (g_wprot & 0x1000UL)
         dd_err("cannot clear the persistence bit (write protected)", g_wprot);
      else
         prph_wr_unlocked(0xA03440UL, g_hpm & ~0x1000UL);
   }
}

int iwl_start_hw(void)
{
   if (iwl_prepare_card_hw()) {
      dd_err("the adapter did not become ready (owned by the management engine?)", iwl_rd(CSR_HW_IF_CONFIG_REG));
      return -1;
   }
   if (g_cfg->family == IWL_FAM_9000 || g_cfg->family == IWL_FAM_22000)
      clear_persistence_bit();
   iwl_wr(CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);
   dd_msleep(5);
   if (apm_init())
      return -2;
   if (g_cfg->family == IWL_FAM_9000 || g_cfg->family == IWL_FAM_22000)   /* multi-queue receive hardware: interrupts as MSI / legacy (UREG_CHICK) */
      iwl_prph_wr(0xA05C00UL, 0x01000000UL);       /* UREG_CHICK_MSI_ENABLE                                  */
   g_trans_on = 1;
   enable_rfkill_int();
   iwl_check_rfkill();
   return 0;
}

void iwl_stop_device(void)
{
   USHORT ch, n;

   disable_interrupts();
   if (iwl_nic_lock()) {
      prph_wr_unlocked(SCD_TXFACT, 0);
      for (ch = 0; ch < FH_TCSR_CHNL_NUM; ch++) {
         iwl_wr(FH_TCSR_CHNL_TX_CONFIG_REG(ch), 0);
         for (n = 0; n < 200; n++) {
            if (iwl_rd(0x1EB0) & ((ULONG)(1 << ch) << 16))   /* FH_TSSR_TX_STATUS_REG: channel idle        */
               break;
            dd_iodelay(20);
         }
      }
      iwl_nic_unlock();
   }
   if (iwl_nic_lock()) {
      iwl_wr(FH_MEM_RCSR_CHNL0_CONFIG_REG, 0);
      for (n = 0; n < 1000; n++) {
         if (iwl_rd(FH_MEM_RSSR_RX_STATUS_REG) & 0x01000000UL)
            break;
         dd_iodelay(10);
      }
      iwl_nic_unlock();
   }
   if (g_cfg->family == IWL_FAM_9000 || g_cfg->family == IWL_FAM_22000)
      iwl9_rx_stop();
   if (g_cfg->family == IWL_FAM_22000)
      iwl22_free_all();
   reset_rings();
   if (g_cfg->family == IWL_FAM_7000) {
      iwl_prph_wr(0x3008UL, 0x00000200UL);       /* APMG_CLK_DIS_REG: DMA clock off                        */
      dd_iodelay(5);
   }
   iwl_clr(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
   g_nic_locks = 0;
   apm_stop();
   iwl_wr(CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);
   dd_msleep(5);
   disable_interrupts();
   enable_rfkill_int();
   iwl_check_rfkill();
   iwl_prepare_card_hw();
}

/* Radio type/step/dash from the firmware's PHY_SKU and the MAC step/dash from the hardware revision. */
static void nic_config(void)
{
   ULONG pc = g_fw.phy_config, v, mask, reg = 0;

   reg |= ((g_hw_rev >> 2) & 3UL) << 2;           /* MAC step                                              */
   reg |= (g_hw_rev & 3UL);                       /* MAC dash                                              */
   reg |= (pc & 3UL) << 10;                       /* radio type                                            */
   reg |= ((pc >> 2) & 3UL) << 14;                /* radio step                                            */
   reg |= ((pc >> 4) & 3UL) << 12;                /* radio dash                                            */
   mask = CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH | CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP | CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP |
          CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH | CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE |
          CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI | CSR_HW_IF_CONFIG_REG_BIT_MAC_SI;
   v = iwl_rd(CSR_HW_IF_CONFIG_REG);
   v = (v & ~mask) | reg;
   iwl_wr(CSR_HW_IF_CONFIG_REG, v);
   if (g_cfg->family == IWL_FAM_7000)             /* early PCIe power-off work-around                      */
      iwl_prph_set(PRPH_APMG_PS_CTRL_REG, 0x00000020UL);
}

static int tx_init(void)
{
   USHORT i;

   if (!iwl_nic_lock())
      return -1;
   prph_wr_unlocked(SCD_TXFACT, 0);               /* deactivate the TX scheduler                           */
   iwl_wr(FH_KW_MEM_ADDR_REG, g_kw.phys >> 4);
   for (i = 0; i < NTXQ; i++) {
      USHORT q = g_txq[i].qid;

      if (g_txq[i].ok && q < 16)
         iwl_wr((USHORT)(FH_MEM_CBBC_0_15 + q * 4), g_txq[i].tfd.phys >> 8);
   }
   prph_wr_unlocked(SCD_GP_CTRL, prph_rd_unlocked(SCD_GP_CTRL) |
                    SCD_GP_CTRL_AUTO_ACTIVE_MODE | SCD_GP_CTRL_ENABLE_31_QUEUES);
   iwl_nic_unlock();
   return 0;
}

static int nic_init(void)
{
   if (apm_init())
      return -1;
   if (g_cfg->family == IWL_FAM_7000)
      iwl_prph_wr(PRPH_APMG_PS_CTRL_REG, (iwl_prph_rd(PRPH_APMG_PS_CTRL_REG) & ~0x03000000UL));   /* VMAIN */
   nic_config();
   if (g_cfg->family == IWL_FAM_22000) {          /* gen2: no scheduler or ring registers; the command queue is described in the context information */
      iwl22_rx_init();
      iwl_set(CSR_MAC_SHADOW_REG_CTRL, 0x800FFFFFUL);
      return 0;
   }
   if (g_cfg->family == IWL_FAM_9000)
      iwl9_rx_init();
   else
      rx_init();
   if (tx_init())
      return -2;
   iwl_set(CSR_MAC_SHADOW_REG_CTRL, 0x800FFFFFUL);
   return 0;
}

/* ---- firmware download (FH service channel) ------------------------------------------------------------ */
static int load_chunk(ULONG dst, USHORT n)
{
   int ext = (dst >= FW_MEM_EXTENDED_START && dst <= FW_MEM_EXTENDED_END);

   if (ext)
      iwl_prph_set(PRPH_LMPM_CHICK, PRPH_LMPM_CHICK_EXTENDED_ADDR_SPACE);
   g_fw_chunk_done = 0;
   if (!iwl_nic_lock())
      return -1;
   iwl_wr(FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL), FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_PAUSE);
   iwl_wr(FH_SRVC_CHNL_SRAM_ADDR_REG, dst);
   iwl_wr(FH_TFDIB_CTRL0_REG(FH_SRVC_CHNL), g_fwdma.phys);
   iwl_wr(FH_TFDIB_CTRL1_REG(FH_SRVC_CHNL), (ULONG)n);   /* address bits 35:32 are zero: below 4 GB   */
   iwl_wr(FH_TCSR_CHNL_TX_BUF_STS_REG(FH_SRVC_CHNL),
          (1UL << FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_NUM) | (1UL << FH_TCSR_CHNL_TX_BUF_STS_REG_POS_TB_IDX) |
          FH_TCSR_CHNL_TX_BUF_STS_REG_VAL_TFDB_VALID);
   iwl_wr(FH_TCSR_CHNL_TX_CONFIG_REG(FH_SRVC_CHNL),
          FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE | FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_DISABLE |
          FH_TCSR_TX_CONFIG_REG_VAL_CIRQ_HOST_ENDTFD);
   iwl_nic_unlock();
   if (hw_wait(&g_fw_chunk_done, 1000)) {
      dd_err("firmware chunk not taken, device address", dst);
      return -2;
   }
   if (ext)
      iwl_prph_clr(PRPH_LMPM_CHICK, PRPH_LMPM_CHICK_EXTENDED_ADDR_SPACE);
   return 0;
}

static int load_section(fwsect_t *s)
{
   ULONG done = 0;
   USHORT n;

   while (done < s->len) {
      n = (s->len - done > 32768UL) ? 32768U : (USHORT)(s->len - done);
      if (blob_read(&g_fwblob, s->off + done, DPTR(g_fwdma), n))
         return -1;
      if (load_chunk(s->devoff + done, n))
         return -2;
      done += n;
   }
   return 0;
}

/* iwm_load_cpu_sections_8000: the secured sections of one CPU, the load status bits after each */
static int load_cpu_sections_8000(fwimg_t *im, USHORT cpu, USHORT *first)
{
   USHORT i, last = 0;
   ULONG  secn = 1UL, v;

   if (cpu == 1)
      *first = 0;
   else
      (*first)++;
   for (i = *first; i < im->nsec; i++) {
      last = i;
      if (im->sec[i].devoff == CPU1_CPU2_SEPARATOR_SECTION || im->sec[i].devoff == PAGING_SEPARATOR_SECTION)
         break;
      if (load_section(&im->sec[i])) {
         dd_err("firmware section not loaded", (ULONG)i);
         return -1;
      }
      if (!iwl_nic_lock())
         return -2;
      v = iwl_rd(FH_UCODE_LOAD_STATUS);
      iwl_wr(FH_UCODE_LOAD_STATUS, v | (cpu == 1 ? secn : (secn << 16)));
      secn = (secn << 1) | 1UL;
      iwl_nic_unlock();
   }
   *first = last;
   if (!iwl_nic_lock())
      return -3;
   iwl_wr(FH_UCODE_LOAD_STATUS, cpu == 1 ? 0x0000FFFFUL : 0xFFFFFFFFUL);
   iwl_nic_unlock();
   return 0;
}

static int load_firmware_8000(USHORT type)
{
   fwimg_t *im = &g_fw.img[type];
   static USHORT first;

   iwl_prph_wr(PRPH_RELEASE_CPU_RESET, PRPH_RELEASE_CPU_RESET_BIT);
   if (load_cpu_sections_8000(im, 1, &first))
      return -1;
   if (load_cpu_sections_8000(im, 2, &first))
      return -2;
   set_mask(CSR_INI_SET_MASK);
   g_intmask_on = 1;
   return 0;
}

/* iwm_load_firmware_7000: the sections of the first CPU in order (to the separator), then CSR_RESET = 0 starts it */
static int load_firmware_7000(USHORT type)
{
   fwimg_t *im = &g_fw.img[type];
   USHORT i;

   for (i = 0; i < im->nsec; i++) {
      if (im->sec[i].devoff == CPU1_CPU2_SEPARATOR_SECTION || im->sec[i].devoff == PAGING_SEPARATOR_SECTION)
         break;                                   /* (a second CPU, as in the 3160, would continue after it) */
      if (load_section(&im->sec[i])) {
         dd_err("firmware section not loaded", (ULONG)i);
         return -1;
      }
   }
   set_mask(CSR_INI_SET_MASK);
   g_intmask_on = 1;
   iwl_wr(CSR_RESET, 0UL);
   return 0;
}

int iwl_load_ucode(USHORT type)
{
   g_alive = g_alive_ok = 0;
   g_fw_error = 0;
   if (g_cfg->family == IWL_FAM_9000 || g_cfg->family == IWL_FAM_22000)   /* Linux iwl_trans_pcie_start_fw: prepare the card again (sets the OS_ALIVE mailbox bit, cleared by the reset) */
      iwl_prepare_card_hw();
   iwl_wr(CSR_INT, 0xFFFFFFFFUL);
   if (nic_init()) {
      dd_err("NIC init failed", 0UL);
      return -1;
   }
   iwl_wr(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
   iwl_wr(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
   iwl_wr(CSR_INT, 0xFFFFFFFFUL);
   set_mask(CSR_INT_BIT_FH_TX);                   /* only the chunk-done interrupt while loading           */
   iwl_wr(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
   iwl_wr(CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
   if (g_cfg->family == IWL_FAM_22000) {          /* the context-info boot (iwl_gen2.c) */
      if (iwl22_boot(type))
         return -5;
   } else if (g_cfg->family == IWL_FAM_7000) {
      if (load_firmware_7000(type))
         return -3;
   } else if (g_cfg->family != IWL_FAM_8000 && g_cfg->family != IWL_FAM_9000)
      return -2;
   else if (load_firmware_8000(type))             /* 8000 and 9000: the secured CPU1 / CPU2 sections          */
      return -3;
   if (hw_wait(&g_alive, (USHORT)(g_cfg->family >= IWL_FAM_9000 ? 3000 : 1000)) || !g_alive_ok) {
      dd_err(g_alive ? "firmware ALIVE with bad status" : "no ALIVE from the firmware", iwl_rd(CSR_INT));
      return -4;
   }
   if (g_cfg->family == IWL_FAM_22000) {
      iwl22_free_images();
      set_mask(CSR_INI_SET_MASK);                 /* iwl_trans_pcie_gen2_fw_alive: all interrupts on       */
      g_intmask_on = 1;
   }
   return 0;
}

/* ---- after ALIVE: the TX scheduler and the command queue (iwm_post_alive) -------------------------------- */
static ULONG bit32(USHORT n)
{
   ULONG v = 1UL;

   while (n--)
      v <<= 1;                                    /* no variable 32-bit shift in 16-bit code               */
   return v;
}

/* Called with the NIC lock held (it nests). */
static void enable_ac_txq(USHORT qid, USHORT fifo)
{
   iwl_wr(HBUS_TARG_WRPTR, (ULONG)(qid << 8));
   prph_wr_unlocked(SCD_QUEUE_STATUS_BITS(qid), 1UL << SCD_QUEUE_STTS_REG_POS_SCD_ACT_EN);
   prph_wr_unlocked(SCD_AGGR_SEL, prph_rd_unlocked(SCD_AGGR_SEL) & ~bit32(qid));
   prph_wr_unlocked(SCD_QUEUE_RDPTR(qid), 0);
   iwl_mem_wr32(g_sched_base + SCD_CONTEXT_QUEUE_OFFSET(qid), 0);
   iwl_mem_wr32(g_sched_base + SCD_CONTEXT_QUEUE_OFFSET(qid) + 4,
                ((ULONG)FRAME_LIMIT & SCD_QUEUE_CTX_REG2_WIN_SIZE_MSK) |
                (((ULONG)FRAME_LIMIT << 16) & SCD_QUEUE_CTX_REG2_FRAME_LIMIT_MSK));
   prph_wr_unlocked(SCD_QUEUE_STATUS_BITS(qid),
                    (1UL << SCD_QUEUE_STTS_REG_POS_ACTIVE) | ((ULONG)fifo << SCD_QUEUE_STTS_REG_POS_TXF) |
                    (1UL << SCD_QUEUE_STTS_REG_POS_WSL) | SCD_QUEUE_STTS_REG_MSK);
   if (qid == g_cmdq)
      prph_wr_unlocked(SCD_EN_CTRL, prph_rd_unlocked(SCD_EN_CTRL) | bit32(qid));
}

int iwl_post_alive(void)
{
   USHORT ch;
   ULONG  base;

   if (!iwl_nic_lock())
      return -1;
   base = prph_rd_unlocked(SCD_SRAM_BASE_ADDR);
   iwl_nic_unlock();
   if (g_sched_base == 0UL)
      g_sched_base = base;
   iwl_wr(CSR_DRAM_INT_TBL_REG, 0);               /* no ICT: causes stay in CSR_INT                         */
   iwl_wr(CSR_INT, 0xFFFFFFFFUL);
   set_mask(CSR_INI_SET_MASK);
   /* clear the scheduler's context and translation tables in device SRAM */
   if (iwl_mem_wr(g_sched_base + SCD_CONTEXT_MEM_LOWER_BOUND, (UCHAR FAR *)0,
                  (SCD_TRANS_TBL_MEM_UPPER_BOUND - SCD_CONTEXT_MEM_LOWER_BOUND) / 4))
      return -2;
   if (!iwl_nic_lock())
      return -3;
   prph_wr_unlocked(SCD_DRAM_BASE_ADDR, g_sched.phys >> 10);
   prph_wr_unlocked(SCD_CHAINEXT_EN, 0);
   enable_ac_txq(g_cmdq, TX_FIFO_CMD);
   prph_wr_unlocked(SCD_TXFACT, 0xFFUL);          /* activate the scheduler                                 */
   for (ch = 0; ch < FH_TCSR_CHNL_NUM; ch++)
      iwl_wr(FH_TCSR_CHNL_TX_CONFIG_REG(ch),
             FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE | FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE);
   iwl_set(FH_TX_CHICKEN_BITS_REG, FH_TX_CHICKEN_BITS_SCD_AUTO_RETRY_EN);
   iwl_nic_unlock();
   if (g_cfg->family < IWL_FAM_8000)
      iwl_prph_clr(PRPH_APMG_PCIDEV_STT_REG, 0x00000800UL);   /* L1-Active back on                         */
   return 0;
}

/* A data queue through the firmware (DQA: SCD_QUEUE_CFG) or directly (legacy mode). */
int iwl_enable_txq(USHORT sta, USHORT qid, USHORT fifo, USHORT tid)
{
   static scd_txq_cfg_cmd_t c;
   txq_t *q = txq_of(qid);

   if (!q)
      return -1;
   if (!FW_CAPA(CAPA_DQA_SUPPORT)) {
      if (!iwl_nic_lock())
         return -2;
      enable_ac_txq(qid, fifo);
      iwl_nic_unlock();
      return 0;
   }
   if (!iwl_nic_lock())
      return -2;
   iwl_wr(HBUS_TARG_WRPTR, (ULONG)(qid << 8) | q->cur);
   iwl_nic_unlock();
   q->tail = q->cur;
   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.tid = (UCHAR)tid;
   c.scd_queue = (UCHAR)qid;
   c.enable = 1;
   c.sta_id = (UCHAR)sta;
   c.tx_fifo = (UCHAR)fifo;
   c.aggregate = 0;
   c.ssn = q->cur;
   c.window = FRAME_LIMIT;
   return iwl_cmd1(CMD_SCD_QUEUE_CFG, (UCHAR FAR *)&c, sizeof(c));
}

/* AX200: the firmware creates the data queue for the station (SCD_QUEUE_CFG); its number is whatever the firmware answers. */
int iwl22_dataq_create(USHORT sta)
{
   txq_t *q = &g_txq[QI_BE];
   USHORT qid;

   if (iwl22_txq_alloc((UCHAR)sta, 0, 32, &qid))   /* TID 0: the best-effort FIFO (TID 15 maps to the small management FIFO on this API: a 1.5 KB frame crashed the firmware) */
      return -1;
   q->qid = qid;
   q->cur = q->tail = q->queued = 0;
   fmemset((UCHAR FAR *)q->busy, 0, TX_SLOTS);
   return 0;
}

void iwl22_dataq_destroy(USHORT sta)
{
   (void)sta;                                      /* the firmware drops the queue with the station */
   g_txq[QI_BE].qid = 0xFFFF;
}

/* ---- transmit descriptors -------------------------------------------------------------------------------- */
static void tfd_set(txq_t *q, USHORT idx, USHORT ntb, ULONG FAR *addr, USHORT FAR *len)
{
   UCHAR FAR *t = DPTR(q->tfd) + idx * TFD_SIZE;
   USHORT i;

   fmemset(t, 0, TFD_SIZE);
   t[3] = (UCHAR)ntb;
   for (i = 0; i < ntb; i++) {
      put32(t + 4 + i * 6, addr[i]);
      put16(t + 8 + i * 6, (USHORT)(len[i] << 4));   /* address bits 35:32 are zero                       */
   }
}

static void sched_update(USHORT qid, USHORT idx, UCHAR sta, USHORT len)
{
   UCHAR FAR *bc = DPTR(g_sched) + qid * (TFD_QUEUE_BC_SIZE * 2);
   USHORT v;

   len = (USHORT)(len + TX_CRC_SIZE + TX_DELIMITER_SIZE);
   if (g_fw.capaflags & UCODE_TLV_FLAGS_DW_BC_TABLE)
      len = (USHORT)((len + 3) >> 2);
   v = (USHORT)(((USHORT)sta << 12) | len);
   put16(bc + idx * 2, v);
   if (idx < TFD_QUEUE_SIZE_BC_DUP)
      put16(bc + (TFD_QUEUE_SIZE_MAX + idx) * 2, v);
}

static void bell(txq_t *q)                       /* tell the device where the ring ends now                */
{
   iwl_wr(HBUS_TARG_WRPTR, (ULONG)(q->qid << 8) | q->cur);
}

static void kick(txq_t *q)
{
   q->queued++;
   q->cur = (USHORT)((q->cur + 1) & (TFD_QUEUE_SIZE_MAX - 1));
   bell(q);
}

/* ---- host commands --------------------------------------------------------------------------------------- */
static USHORT g_cmd_done, g_cmd_wait_idx = 0xFFFF;
clog_t g_clog[CLOG_N];
USHORT g_clog_n;
ULONG  g_cmd_err;
ULONG  g_errlog[ERRLOG_DW], g_uerrlog[UERRLOG_DW];
USHORT g_errlog_ok, g_uerrlog_ok;

static void clog(USHORT id, int rc, ULONG info)
{
   clog_t *c = &g_clog[g_clog_n % CLOG_N];

   c->id = id;
   c->rc = (short)rc;
   c->info = info;
   g_clog_n++;
}

/* The firmware's error log (iwm_nic_error / iwm_nic_umac_error): valid, error id, ... in device SRAM. */
void iwl_dump_fw_error(void)
{
   if (g_errlog_ok || !g_trans_on)
      return;
   if (g_err_table >= 0x800000UL && iwl_mem_rd(g_err_table, (ULONG FAR *)g_errlog, ERRLOG_DW) == 0)
      g_errlog_ok = 1;
   if (g_umac_err_table >= 0x800000UL && iwl_mem_rd(g_umac_err_table, (ULONG FAR *)g_uerrlog, UERRLOG_DW) == 0)
      g_uerrlog_ok = 1;
   if (g_errlog_ok) {
      tstr("[iwl: firmware error id "); th32(g_errlog[1]); tstr(" data "); th32(g_errlog[7]); tcom(' ');
      th32(g_errlog[8]); tcom(' '); th32(g_errlog[9]); tstr(" last cmd "); th32(g_errlog[25]); tcom(']'); tnl();
   }
}
static USHORT g_cmd_want;

/* Called from iwl_rx_pkt for every packet that answers a host command (qid = command queue, bit 7 clear). */
void iwl_cmd_answer(USHORT code, UCHAR idx, UCHAR flags, UCHAR FAR *p, USHORT len)
{
   if (idx != g_cmd_wait_idx) {                  /* an asynchronous command's answer (or a late one)       */
      if (code == CMD_REPLY_ERROR) {
         g_async_fail++;
         clog((USHORT)(0x8000U | (len > 4 ? p[4] : 0)), -5, len >= 4 ? get32(p) : 0UL);
      } else if (code == CMD_ADD_STA && len >= 4) {
         if ((get32(p) & 0xFFUL) == ADD_STA_SUCCESS)
            g_async_ok++;
         else {
            g_async_fail++;
            clog(0x8018U, -13, get32(p));
         }
         txa_cfg_answer(idx, (get32(p) & 0xFFUL) == ADD_STA_SUCCESS);   /* the one that completes an aggregation setup */
      } else if (flags & CMD_FAILED_MSK) {
         g_async_fail++;
         clog((USHORT)(0x8000U | code), -5, 0UL);
      }
      return;
   }
   g_resp_failed = (USHORT)((flags & CMD_FAILED_MSK) != 0);
   if (code == CMD_REPLY_ERROR) {                /* the firmware refused the command: it answers with REPLY_ERROR */
      g_resp_failed = 1;
      g_cmd_err = (len >= 4) ? get32(p) : 0xFFFFFFFFUL;
   }
   if (g_cmd_want) {
      if (len > 4000)
         len = 4000;
      fmemcpy(g_resp, p, len);
      g_resp_len = len;
   }
   g_cmd_done = 1;
}

USHORT g_cmd_last_idx;
static USHORT g_cmdlk;                          /* the command ring: the request path and the receive path both queue commands */
USHORT g_async_ok, g_async_fail;

/* Build a command in the next slot of the command ring and kick it. A synchronous command (async 0) sets up the
 * wait state first; an asynchronous one must fit a slot and nobody waits for its answer. Returns 0 or < 0. */
static int cmd_queue(USHORT id, UCHAR FAR *d0, USHORT l0, UCHAR FAR *d1, USHORT l1, USHORT flags, USHORT async)
{
   txq_t *q = &g_txq[0];
   USHORT idx, pay = (USHORT)(l0 + l1), hl, group = (USHORT)(id >> 8), fl;
   UCHAR FAR *c;
   ULONG  ph;
   static ULONG  a[1];
   static USHORT n[1];

   if (!g_trans_on)
      return -1;
   if (pay > MAX_CMD_PAYLOAD_SIZE)
      return -2;
   if (g_cfg->family == IWL_FAM_22000 && group == 0)
      group = 1;                                  /* API >= 50: group 0 commands go out as LONG_GROUP (1)   */
   hl = (USHORT)((group || g_cfg->family == IWL_FAM_22000) ? 8 : 4);   /* gen2: the wide header for every command */
   if (async && pay + hl > CMD_SLOT_SZ)
      return -2;
   fl = rt_irq_save();
   if (rt_lock_acquire(&g_cmdlk, (USHORT)(rt_cpu_id() + 1))) {
      rt_irq_restore(fl);
      return -8;
   }
   if (q->queued >= CMD_SLOTS - 1) {
      rt_lock_release(&g_cmdlk);
      rt_irq_restore(fl);
      dd_err("command queue full", (ULONG)q->queued);
      return -3;
   }
   idx = q->cur;
   g_cmd_last_idx = idx;
   if (pay + hl <= CMD_SLOT_SZ) {
      USHORT s = (USHORT)(idx & (CMD_SLOTS - 1));

      c  = DPTR(q->buf) + s * CMD_SLOT_SZ;
      ph = q->buf.phys + (ULONG)(s * CMD_SLOT_SZ);
   } else {
      c  = DPTR(g_bigcmd);                        /* synchronous commands: one big buffer is enough         */
      ph = g_bigcmd.phys;
   }
   if (group || g_cfg->family == IWL_FAM_22000) {
      c[0] = (UCHAR)id;                           /* opcode                                                 */
      c[1] = (UCHAR)group;
      c[2] = (UCHAR)idx;
      c[3] = (UCHAR)q->qid;
      put16(c + 4, pay);
      c[6] = 0;
      c[7] = 0;                                   /* version                                                */
   } else {
      c[0] = (UCHAR)id;
      c[1] = 0;
      c[2] = (UCHAR)idx;
      c[3] = (UCHAR)q->qid;
   }
   if (l0) fmemcpy(c + hl, d0, l0);
   if (l1) fmemcpy(c + hl + l0, d1, l1);
   a[0] = ph;
   n[0] = (USHORT)(hl + pay);
   if (g_cfg->family == IWL_FAM_22000)
      iwl22_post(idx, ph, (USHORT)(hl + pay));
   else {
      tfd_set(q, idx, 1, (ULONG FAR *)a, (USHORT FAR *)n);
      sched_update(q->qid, idx, 0, 0);
   }
   if (!async) {
      g_cmd_done = 0;
      g_cmd_want = (USHORT)(flags & CMDF_WANT_RESP);
      g_resp_len = 0;
      g_resp_failed = 0;
      g_cmd_err = 0;
      g_cmd_wait_idx = idx;
   }
   if (g_cfg->family == IWL_FAM_22000) {          /* gen2: 32 entries; the write pointer and the queue id (bits 16 and up) */
      q->queued++;
      q->cur = (USHORT)((q->cur + 1) & (CMD_SLOTS - 1));
      iwl_wr(HBUS_TARG_WRPTR, (ULONG)q->cur | ((ULONG)q->qid << 16));
   } else
      kick(q);
   rt_lock_release(&g_cmdlk);
   rt_irq_restore(fl);
   return 0;
}

int iwl_cmd(USHORT id, UCHAR FAR *d0, USHORT l0, UCHAR FAR *d1, USHORT l1, USHORT flags)
{
   int rc = cmd_queue(id, d0, l0, d1, l1, flags, 0);

   if (rc) {
      clog(id, rc, 0UL);
      return rc;
   }
   if (hw_wait(&g_cmd_done, 2000)) {
      g_cmd_to++;
      g_cmd_wait_idx = 0xFFFF;
      if (g_fw_error) {
         dd_err("the firmware crashed during command", (ULONG)id);
         iwl_dump_fw_error();
         clog(id, -7, g_errlog_ok ? g_errlog[1] : 0UL);
         return -7;
      }
      dd_err("firmware command timed out, id", (ULONG)id);
      clog(id, -4, 0UL);
      return -4;
   }
   g_cmd_wait_idx = 0xFFFF;
   if (g_resp_failed) {
      dd_err("the firmware refused command", (ULONG)id);
      clog(id, -5, g_cmd_err);
      return -5;
   }
   clog(id, 0, 0UL);
   return 0;
}

/* A command nobody waits for, usable from the receive path (it never blocks). The answer is only checked and
 * counted (iwl_cmd_answer). */
int iwl_cmd_async(USHORT id, UCHAR FAR *d, USHORT len)
{
   return cmd_queue(id, d, len, (UCHAR FAR *)0, 0, 0, 1);
}

int iwl_cmd1(USHORT id, UCHAR FAR *d, USHORT len)
{
   return iwl_cmd(id, d, len, (UCHAR FAR *)0, 0, 0);
}

/* A command whose answer is a ULONG status (ADD_STA, BINDING_CONTEXT, ...). */
int iwl_cmd_status(USHORT id, UCHAR FAR *d, USHORT len, ULONG FAR *status)
{
   int rc = iwl_cmd(id, d, len, (UCHAR FAR *)0, 0, CMDF_WANT_RESP);

   if (rc)
      return rc;
   if (g_resp_len < 4)
      return -6;
   *status = get32(g_resp);
   return 0;
}

/* ---- data transmit -------------------------------------------------------------------------------------- */
/* A free slot on the data queue qi (QI_BE or QI_AGG): the caller builds { cmd header, tx command, 802.11 header,
 * pad } at the start and the payload at offset 256, then calls iwl_txq_kick. */
UCHAR FAR *iwl_txq_slot(USHORT qi, USHORT *idx, ULONG *phys)
{
   txq_t *q = &g_txq[qi];
   USHORT s;

   if (!g_trans_on || !q->ok || !q->nslot || q->qid == 0xFFFF || q->queued >= TX_SLOTS - 1)
      return 0;
   s = (USHORT)(q->cur & (TX_SLOTS - 1));
   if (q->busy[s])
      return 0;
   *idx = q->cur;
   *phys = q->buf.phys + (ULONG)(s * TX_SLOT_SZ);
   return DPTR(q->buf) + s * TX_SLOT_SZ;
}

/* cmdlen: header + tx command + 802.11 header + pad (TB0 16 bytes, TB1 the rest), paylen at offset 256 (TB2);
 * totlen: the frame length the scheduler is told (header + payload + MIC). bell 0: only queue it (an aggregation
 * queue whose firmware setup is not complete yet). */
int iwl_txq_kick(USHORT qi, USHORT idx, USHORT cmdlen, USHORT paylen, USHORT totlen, USHORT dobell)
{
   txq_t *q = &g_txq[qi];
   USHORT s = (USHORT)(idx & (TX_SLOTS - 1));
   ULONG  ph = q->buf.phys + (ULONG)(s * TX_SLOT_SZ);
   static ULONG  a[3];
   static USHORT n[3];

   if (g_cfg->family == IWL_FAM_22000) {          /* gen2: TFH ring of 32, doorbell with the queue id in bits 16 and up */
      iwl22_data_tfd(idx, ph, cmdlen, paylen, totlen);
      q->busy[s] = 1;
      q->queued++;
      q->cur = (USHORT)((q->cur + 1) & 31);
      iwl_wr(HBUS_TARG_WRPTR, (ULONG)q->cur | ((ULONG)q->qid << 16));
      return 0;
   }
   a[0] = ph;       n[0] = 16;
   a[1] = ph + 16;  n[1] = (USHORT)(cmdlen - 16);
   a[2] = ph + 256; n[2] = paylen;
   tfd_set(q, idx, (USHORT)(paylen ? 3 : 2), (ULONG FAR *)a, (USHORT FAR *)n);
   sched_update(q->qid, idx, STATION_ID, totlen);
   q->busy[s] = 1;
   if (dobell)
      kick(q);
   else {
      q->queued++;
      q->cur = (USHORT)((q->cur + 1) & (TFD_QUEUE_SIZE_MAX - 1));
   }
   return 0;
}

void iwl_txq_bell(USHORT qi)
{
   bell(&g_txq[qi]);
}

/* An aggregation queue starts at ring index idx: nothing is queued, the device is told where the ring starts. */
void iwl_txq_reset(USHORT qi, USHORT idx)
{
   txq_t *q = &g_txq[qi];

   fmemset((UCHAR FAR *)q->busy, 0, TX_SLOTS);
   q->cur = q->tail = (USHORT)(idx & (TFD_QUEUE_SIZE_MAX - 1));
   q->queued = 0;
   iwl_wr(HBUS_TARG_WRPTR, (ULONG)(q->qid << 8) | q->cur);
}

/* Free the slots from the tail up to (not including) ring index idx; an index beyond what was queued is ignored. */
void iwl_txq_reclaim_to(USHORT qi, USHORT idx)
{
   txq_t *q = &g_txq[qi];
   USHORT msk = (USHORT)(g_cfg->family == IWL_FAM_22000 ? 31 : (TFD_QUEUE_SIZE_MAX - 1));   /* the gen2 ring has 32 entries */
   USHORT n = (USHORT)((idx - q->tail) & msk);

   if (n > q->queued)
      return;
   while (n--) {
      USHORT s = (USHORT)(q->tail & (TX_SLOTS - 1));

      if (q->busy[s]) {
         q->busy[s] = 0;
         q->queued--;
      }
      q->tail = (USHORT)((q->tail + 1) & msk);
   }
}

USHORT iwl_txq_queued(USHORT qi)  { return g_txq[qi].queued; }
USHORT iwl_txq_cur(USHORT qi)     { return g_txq[qi].cur; }
USHORT iwl_txq_qid(USHORT qi)     { return g_txq[qi].qid; }
int    iwl_txq_present(USHORT qi) { return g_txq[qi].ok && g_txq[qi].nslot; }

/* The firmware's TX response for (qid, idx): free the slot. */
void iwl_tx_reclaim(UCHAR qid, UCHAR idx)
{
   txq_t *q = txq_of(qid);
   USHORT s;

   if (!q || !q->nslot || q == &g_txq[0] || q == &g_txq[QI_AGG])
      return;                                     /* the aggregation queue is reclaimed by block-ack / ssn  */
   s = (USHORT)(idx & (TX_SLOTS - 1));
   if (q->busy[s]) {
      q->busy[s] = 0;
      if (q->queued)
         q->queued--;
   }
   q->tail = (USHORT)((idx + 1) & (TFD_QUEUE_SIZE_MAX - 1));
}


void iwl_trans_off(void)
{
   g_trans_on = 0;
}

USHORT iwl_rx_cur(void)
{
   return g_rxcur;
}
