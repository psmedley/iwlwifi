/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_ctxt.c - the 9000 family (Intel Wireless-AC 9260 / 9560): firmware boot through the "context info" structure and
 * multi-queue receive.
 *
 * Written from Linux 5.4 (pcie/ctxt-info.c, iwl-context-info.h, pcie/rx.c: iwl_pcie_rx_mq_hw_init, iwl_pcie_rxmq_restock,
 * iwl_pcie_rx_handle; iwl-fh.h RFH registers). Differences from the 8000 family (iwl_trans.c):
 *   - the firmware is not pushed through the FH service channel: the sections are copied into DRAM, the addresses go
 *     into a context-info structure, the structure's address into CSR 0x40, and CPU_INIT_RUN is set; the device's ROM
 *     does the rest and sends the ALIVE notification;
 *   - receive uses two rings, "free" (64-bit entries: buffer address | 12-bit buffer id) and "used" (32-bit entries,
 *     the buffer id); the device writes the used ring and the count of closed entries to the status word;
 *   - the receive registers are the RFH_* ones, not FH_RSCSR_*.
 * Transmit queues and the scheduler are as for the 8000 family.
 */
#include "iwl.h"

#define MQ_N        512                         /* entries of the free and used rings                      */
#define CTX_SIZE    1792                        /* sizeof(struct iwl_context_info)                          */

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
#define CSR_CTXT_INFO_BA        0x040
#define UREG_CPU_INIT_RUN       0xA05C44UL

static DMA    g_fbd, g_ubd, g_ctx;
static USHORT g_mq_rd, g_mq_wr, g_mq_wra;       /* used ring read index; free ring write index (and what the device was told) */
static ULONG  g_img_ph[UCODE_SECT_MAX];         /* firmware sections in DRAM: freed after ALIVE            */
static USHORT g_img_n;
static ULONG  g_pg_ph[UCODE_SECT_MAX];          /* paging sections: until the device is stopped            */
static USHORT g_pg_n;
USHORT g_ctx_lm, g_ctx_um, g_ctx_pg, g_ctx_fail;
ULONG  g_ctx_bytes;
USHORT g_mq_bad, g_mq_bufs;

/* pure logic: the number of sections from start up to a separator (iwl_pcie_get_num_sections) */
/* CTX_PURE_BEGIN */
static USHORT sec_count(ULONG FAR *devoff, USHORT nsec, USHORT start)
{
   USHORT n = 0;

   while (start < nsec && devoff[start] != 0xFFFFCCCCUL && devoff[start] != 0xAAAABBBBUL) {
      start++;
      n++;
   }
   return n;
}
/* CTX_PURE_END */

static void put64(UCHAR FAR *p, ULONG v)
{
   put32(p, v);
   put32(p + 4, 0UL);                           /* all DMA memory is below 4 GB                             */
}

int iwl9_alloc(void)
{
   if (dma_get_al(MQ_N * 8, 4096, &g_fbd) || dma_get_al(MQ_N * 4, 4096, &g_ubd) || dma_get_al(CTX_SIZE, 4096, &g_ctx))
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

/* One firmware section into DRAM (a contiguous block of its own); 0 = ok */
static int sec_alloc(fwsect_t *s, ULONG FAR *ph)
{
   ULONG size = s->len ? s->len : 4UL, done = 0UL, p;
   USHORT n, sel;

   p = dma_big(size);
   if (!p)
      return -1;
   while (done < s->len) {
      n = (s->len - done > 32768UL) ? 32768U : (USHORT)(s->len - done);
      sel = scr_map(SCR_D, p + done, n);
      if (!sel || blob_read(&g_fwblob, s->off + done, (UCHAR FAR *)MAKEP(sel, 0), n)) {
         dd_dh_free_phys(p);
         return -2;
      }
      done += n;
   }
   *ph = p;
   g_ctx_bytes += size;
   return 0;
}

void iwl9_free_images(void)                    /* after ALIVE: the device has taken its copy                */
{
   while (g_img_n)
      dd_dh_free_phys(g_img_ph[--g_img_n]);
}

void iwl9_free_all(void)                       /* the device is stopped                                      */
{
   iwl9_free_images();
   while (g_pg_n)
      dd_dh_free_phys(g_pg_ph[--g_pg_n]);
}

void iwl9_rx_stop(void)
{
   if (g_fbd.phys == 0UL)
      return;
   iwl_prph_wr(RFH_RXF_DMA_CFG, 0UL);
   iwl_prph_wr(RFH_RXF_RXQ_ACTIVE, 0UL);
}

/* iwl_pcie_ctxt_info_init: build the structure, tell the device, set the CPU running */
int iwl9_boot(USHORT type)
{
   fwimg_t *im = &g_fw.img[type];
   static ULONG offs[UCODE_SECT_MAX];
   UCHAR FAR *c;
   USHORT lm, um, pg, i, k;
   ULONG  ph;

   iwl9_free_all();
   g_ctx_bytes = 0UL;
   g_ctx_fail = 0;
   for (i = 0; i < im->nsec; i++)
      offs[i] = im->sec[i].devoff;
   lm = sec_count((ULONG FAR *)offs, im->nsec, 0);
   um = sec_count((ULONG FAR *)offs, im->nsec, (USHORT)(lm + 1));
   pg = sec_count((ULONG FAR *)offs, im->nsec, (USHORT)(lm + um + 2));
   g_ctx_lm = lm;
   g_ctx_um = um;
   g_ctx_pg = pg;
   if (lm > 64 || um > 64 || pg > 64 || lm == 0) {
      g_ctx_fail = 1;
      return -1;
   }
   c = DPTR(g_ctx);
   fmemset(c, 0, CTX_SIZE);
   put16(c + 0, (USHORT)iwl_rd(CSR_HW_REV));                /* version.mac_id                                      */
   put16(c + 2, 0);
   put16(c + 4, CTX_SIZE / 4);                             /* size in dwords                                       */
   /* control: TFD format long (bit 8), receive ring size log2(512) = 9 at bit 4, buffer size 4 KB (4) at bit 9 */
   put32(c + 8, 0x00000100UL | (9UL << 4) | (4UL << 9));
   put64(c + 24, g_fbd.phys);                               /* rbd_cfg: free, used, status                          */
   put64(c + 32, g_ubd.phys);
   put64(c + 40, iwl_rbstat_phys());
   put64(c + 48, iwl_cmdq_ring_phys());                     /* hcmd_cfg: the command queue                          */
   c[56] = 5;                                              /* TFD_QUEUE_CB_SIZE(256) = log2(256) - 3                */
   for (i = 0; i < lm; i++) {
      if (sec_alloc(&im->sec[i], &ph)) { g_ctx_fail = (USHORT)(0x100 + i); iwl9_free_all(); return -2; }
      g_img_ph[g_img_n++] = ph;
      put64(c + 704 + i * 8, ph);                           /* dram.lmac_img[i]                                     */
   }
   for (i = 0; i < um; i++) {
      k = (USHORT)(lm + 1 + i);
      if (sec_alloc(&im->sec[k], &ph)) { g_ctx_fail = (USHORT)(0x200 + i); iwl9_free_all(); return -3; }
      g_img_ph[g_img_n++] = ph;
      put64(c + 192 + i * 8, ph);                           /* dram.umac_img[i]                                     */
   }
   for (i = 0; i < pg; i++) {
      k = (USHORT)(lm + um + 2 + i);
      if (sec_alloc(&im->sec[k], &ph)) { g_ctx_fail = (USHORT)(0x300 + i); iwl9_free_all(); return -4; }
      g_pg_ph[g_pg_n++] = ph;
      put64(c + 1216 + i * 8, ph);                          /* dram.virtual_img[i]                                  */
   }
   iwl_set_mask(CSR_INT_BIT_ALIVE | CSR_INT_BIT_FH_RX);     /* only the ALIVE interrupt and receive                 */
   iwl_wr(CSR_INT, 0xFFFFFFFFUL);
   iwl_wr(CSR_CTXT_INFO_BA, g_ctx.phys);
   iwl_wr(CSR_CTXT_INFO_BA + 4, 0UL);
   iwl_prph_wr(UREG_CPU_INIT_RUN, 1UL);                     /* the ROM now loads the images and starts the firmware */
   return 0;
}

void iwl9_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void))
{
   ps("CTXINFO lmac="); ph(g_ctx_lm, 2); ps(" umac="); ph(g_ctx_um, 2); ps(" paging="); ph(g_ctx_pg, 2);
   ps(" dram="); ph(g_ctx_bytes, 8); ps(" fail="); ph(g_ctx_fail, 4); ps(" MQRX rd="); ph(g_mq_rd, 3);
   ps(" wr="); ph(g_mq_wr, 3); ps(" told="); ph(g_mq_wra, 3); ps(" bufs="); ph(g_mq_bufs, 2); ps(" bad="); ph(g_mq_bad, 4);
   nl();
}
