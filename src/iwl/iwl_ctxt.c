/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_ctxt.c - multi-queue receive of the 9000 family (Intel Wireless-AC 9260 / 9560).
 *
 * Written from Linux 5.4 (pcie/rx.c: iwl_pcie_rx_mq_hw_init, iwl_pcie_rxmq_restock, iwl_pcie_rx_handle; iwl-fh.h RFH
 * registers). The firmware is loaded exactly as for the 8000 family (iwl_trans.c: service channel, secure CPU1/CPU2
 * sections); what differs is the receive side: two rings, "free" (64-bit entries: buffer address | 12-bit buffer id)
 * and "used" (32-bit entries, the buffer id), the count of closed used entries in the status word, and the RFH_*
 * registers instead of FH_RSCSR_*. (The context-info boot of Linux's ctxt-info.c belongs to the 22000 family, not to
 * this one: the first 9560 test showed that; see docs/TESTING.md.) Transmit queues and the scheduler are as for the
 * 8000 family.
 */
#include "iwl.h"

#define MQ_N        512                         /* entries of the free and used rings                      */

#define RFH_Q_FRBDCB_BA_LSB     0xA08000UL
#define RFH_Q_FRBDCB_WIDX       0xA08080UL
#define RFH_Q_FRBDCB_RIDX       0xA080C0UL
#define RFH_Q_URBDCB_BA_LSB     0xA08100UL
#define RFH_Q_URBDCB_WIDX       0xA08180UL
#define RFH_Q_URBD_STTS_WPTR    0xA08200UL
#define RFH_GEN_CFG             0xA09800UL
#define RFH_RXF_RXQ_ACTIVE      0xA0980CUL
#define RFH_RXF_DMA_CFG         0xA09820UL
#define RFH_Q_FRBDCB_WIDX_TRG   0x1C80          /* a CSR (not periphery) register                          */

static DMA    g_fbd, g_ubd;
static USHORT g_mq_rd, g_mq_wr, g_mq_wra;       /* used ring read index; free ring write index (and what the device was told) */
USHORT g_mq_bad, g_mq_bufs;

int iwl9_alloc(void)
{
   if (dma_get_al(MQ_N * 8, 4096, &g_fbd) || dma_get_al(MQ_N * 4, 4096, &g_ubd))
      return -1;
   return 0;
}

void iwl9_reset(void)
{
   if (g_fbd.phys == 0UL)
      return;
   fmemset(DPTR(g_fbd), 0, MQ_N * 8);
   fmemset(DPTR(g_ubd), 0, MQ_N * 4);
   g_mq_rd = g_mq_wr = g_mq_wra = 0;
}

static void prph64(ULONG reg, ULONG v)
{
   iwl_prph_wr(reg, v);
   iwl_prph_wr(reg + 4UL, 0UL);
}

/* iwl_pcie_rx_mq_hw_init + the first restock: RX_NBUF buffers of 4 KB, buffer id = index + 1 */
void iwl9_rx_init(void)
{
   UCHAR FAR *d;
   USHORT i;

   iwl9_reset();
   fmemset(iwl_rbstat_ptr(), 0, 16);
   d = DPTR(g_fbd);
   for (i = 0; i < RX_NBUF; i++) {
      put32(d + i * 8, iwl_rxbuf_phys(i) | (ULONG)(i + 1));
      put32(d + i * 8 + 4, 0UL);
   }
   for (i = 0; i < 4; i++) {                               /* the first buffers zeroed: any DMA write into them shows in the status */
      USHORT sel = scr_map(SCR_C, iwl_rxbuf_phys(i), 4096);

      if (sel)
         fmemset((UCHAR FAR *)MAKEP(sel, 0), 0, 4096);
   }
   g_mq_wr = RX_NBUF;
   g_mq_wra = RX_NBUF;
   g_mq_bufs = RX_NBUF;
   iwl_prph_wr(RFH_RXF_DMA_CFG, 0UL);                      /* stop receive DMA                                      */
   iwl_prph_wr(RFH_RXF_RXQ_ACTIVE, 0UL);
   prph64(RFH_Q_FRBDCB_BA_LSB, g_fbd.phys);
   prph64(RFH_Q_URBDCB_BA_LSB, g_ubd.phys);
   prph64(RFH_Q_URBD_STTS_WPTR, iwl_rbstat_phys());
   iwl_prph_wr(RFH_Q_FRBDCB_WIDX, 0UL);
   iwl_prph_wr(RFH_Q_FRBDCB_RIDX, 0UL);
   iwl_prph_wr(RFH_Q_URBDCB_WIDX, 0UL);
   /* DMA enable | 4 KB buffers (4 << 16) | minimum buffer size 4/8 (3 << 24) | drop frames that exceed the buffer |
    * 512 descriptors (9 << 20) */
   iwl_prph_wr(RFH_RXF_DMA_CFG, 0x80000000UL | 0x00040000UL | 0x03000000UL | 0x04000000UL | 0x00900000UL);
   /* DMA snooping (bits 0, 1), default queue 0, 128-byte chunks (bit 4) */
   iwl_prph_wr(RFH_GEN_CFG, 0x00000003UL | 0x00000010UL);
   iwl_prph_wr(RFH_RXF_RXQ_ACTIVE, 0x00010001UL);          /* queue 0 (bits 0 and 16)                               */
   mmio_wr8(CSR_INT_COALESCING, HOST_INT_TIMEOUT_DEF);
   iwl_wr(RFH_Q_FRBDCB_WIDX_TRG, (ULONG)(g_mq_wr & ~7));
}

/* iwl_pcie_rx_handle: the used ring up to the device's closed index; each buffer goes back into the free ring */
void iwl9_rx_service(void)
{
   USHORT hw, vid;
   UCHAR FAR *u, FAR *f;

   hw = (USHORT)(get16(iwl_rbstat_ptr()) & 0x0FFF);
   hw &= (MQ_N - 1);
   if (hw == g_mq_rd)
      return;
   while (g_mq_rd != hw) {
      u = DPTR(g_ubd);
      vid = (USHORT)(get32(u + g_mq_rd * 4) & 0x0FFFUL);
      g_mq_rd = (USHORT)((g_mq_rd + 1) & (MQ_N - 1));
      if (vid == 0 || vid > RX_NBUF) {
         g_mq_bad++;                                        /* never expected: that buffer stays lost                */
         g_mq_bufs--;
         continue;
      }
      iwl_rx_buffer((USHORT)(vid - 1));
      f = DPTR(g_fbd);
      put32(f + g_mq_wr * 8, iwl_rxbuf_phys((USHORT)(vid - 1)) | (ULONG)vid);
      put32(f + g_mq_wr * 8 + 4, 0UL);
      g_mq_wr = (USHORT)((g_mq_wr + 1) & (MQ_N - 1));
   }
   if ((USHORT)(g_mq_wr & ~7) != g_mq_wra) {                /* the device is told in multiples of 8                  */
      g_mq_wra = (USHORT)(g_mq_wr & ~7);
      iwl_wr(RFH_Q_FRBDCB_WIDX_TRG, (ULONG)g_mq_wra);
   }
}

void iwl9_rx_stop(void)
{
   if (g_fbd.phys == 0UL)
      return;
   iwl_prph_wr(RFH_RXF_DMA_CFG, 0UL);
   iwl_prph_wr(RFH_RXF_RXQ_ACTIVE, 0UL);
}

void iwl9_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void))
{
   USHORT i;

   ps("PERSIST hpm="); ph(g_hpm, 8); ps(" wprot="); ph(g_wprot, 8);
   if (g_fw_error || g_hw_failed) {                          /* secure-boot status of both CPUs, after a failed start  */
      ps(" SBOOT cpu1="); ph(iwl_prph_rd(0xA01E30UL), 8); ps(" cpu2="); ph(iwl_prph_rd(0xA01E34UL), 8);
      ps(" umag1="); ph(iwl_prph_rd(0xA038C0UL), 8); ps(" umag2="); ph(iwl_prph_rd(0xA038C4UL), 8);
   }
   nl();
   if (g_fw_error || g_hw_failed) {                          /* did the device consume receive buffers or write anything? */
      ps("RFH dmacfg="); ph(iwl_prph_rd(RFH_RXF_DMA_CFG), 8); ps(" gen="); ph(iwl_prph_rd(RFH_GEN_CFG), 8);
      ps(" active="); ph(iwl_prph_rd(RFH_RXF_RXQ_ACTIVE), 8); ps(" frb_w="); ph(iwl_prph_rd(RFH_Q_FRBDCB_WIDX), 4);
      ps(" frb_r="); ph(iwl_prph_rd(0xA080C0UL), 4); ps(" urb_w="); ph(iwl_prph_rd(RFH_Q_URBDCB_WIDX), 4);
      ps(" stts="); ph(get32(iwl_rbstat_ptr()), 8); ps(" used0="); ph(get32(DPTR(g_ubd)), 8); nl();
      ps("CHICK="); ph(iwl_prph_rd(0xA05C00UL), 8); ps(" DRV_GP1="); ph(iwl_rd(0x054), 8); ps(" DRV_GP2="); ph(iwl_rd(0x060), 8);
      ps(" MBOX="); ph(iwl_rd(0x088), 8); ps(" FHINT="); ph(iwl_rd(0x010), 8); nl();
      ps("RFHREGS fbd="); ph(iwl_prph_rd(RFH_Q_FRBDCB_BA_LSB), 8); ps(" ubd="); ph(iwl_prph_rd(RFH_Q_URBDCB_BA_LSB), 8);
      ps(" stts="); ph(iwl_prph_rd(RFH_Q_URBD_STTS_WPTR), 8); ps(" want fbd="); ph(g_fbd.phys, 8); ps(" ubd="); ph(g_ubd.phys, 8);
      ps(" stts="); ph(iwl_rbstat_phys(), 8); nl();
      for (i = 0; i < 2; i++) {                             /* the first bytes of two receive buffers: did the device write anything? */
         USHORT sel = scr_map(SCR_C, iwl_rxbuf_phys(i), 64);
         UCHAR FAR *p = (UCHAR FAR *)MAKEP(sel, 0);
         USHORT k;

         ps("RXBUF"); ph(i, 1); ps(" ");
         if (!sel) { ps("unmapped"); nl(); continue; }
         for (k = 0; k < 24; k++)
            ph(p[k], 2);
         nl();
      }
   }
   ps("INTCAUSES seen="); ph(g_int_seen, 8); ps(" alive="); ph(g_int_alive, 4); ps(" rx="); ph(g_int_rx, 4);
   ps(" fhtx="); ph(g_int_fhtx, 4); ps(" other="); ph(g_int_other, 4); nl();
   ps("MQRX rd="); ph(g_mq_rd, 3);
   ps(" wr="); ph(g_mq_wr, 3); ps(" told="); ph(g_mq_wra, 3); ps(" bufs="); ph(g_mq_bufs, 2); ps(" bad="); ph(g_mq_bad, 4);
   nl();
}
