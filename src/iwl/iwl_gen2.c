/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_gen2.c - the 22000 family (Intel Wi-Fi 6 AX200): the "gen2" transport's firmware boot through the context-info structure
 * (version 1: Linux 5.4 pcie/ctxt-info.c, pcie/trans-gen2.c iwl_trans_pcie_gen2_start_fw). Stage 1 only: boot to ALIVE.
 *
 * The firmware sections are copied into DRAM (one contiguous block each), the context information holds their addresses, the
 * addresses of the receive rings and of the command queue (a ring of 32 TFH descriptors of 256 bytes), its address goes to CSR
 * 0x40 and CPU_INIT_RUN is set; the boot ROM loads the image and the firmware sends the ALIVE notification.
 */
#include "iwl.h"

#define CTX_SIZE    1792                        /* sizeof(struct iwl_context_info)                          */
#define CSR_CTXT_INFO_BA        0x040
#define UREG_CPU_INIT_RUN       0xA05C44UL
#define CQ_N        32                          /* command queue entries (IWL_CMD_QUEUE_SIZE)               */
#define TFH_SIZE    256

static DMA    g_ctx, g_cring;
static ULONG  g_img_ph[UCODE_SECT_MAX];         /* firmware sections in DRAM: freed after ALIVE             */
static USHORT g_img_n;
static ULONG  g_pg_ph[UCODE_SECT_MAX];          /* paging sections: until the device is stopped             */
static USHORT g_pg_n;
USHORT g_ctx_lm, g_ctx_um, g_ctx_pg, g_ctx_fail;
ULONG  g_ctx_bytes;

/* CTX_PURE_BEGIN */
/* the number of sections from start up to a separator (iwl_pcie_get_num_sections) */
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

int iwl22_alloc(void)
{
   if (dma_get_al(CTX_SIZE, 4096, &g_ctx) || dma_get_al(CQ_N * TFH_SIZE, 256, &g_cring))
      return -1;
   return 0;
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

void iwl22_free_images(void)                    /* after ALIVE: the device has taken its copy                */
{
   while (g_img_n)
      dd_dh_free_phys(g_img_ph[--g_img_n]);
}

void iwl22_free_all(void)                       /* the device is stopped                                      */
{
   iwl22_free_images();
   while (g_pg_n)
      dd_dh_free_phys(g_pg_ph[--g_pg_n]);
}

/* iwl_pcie_ctxt_info_init: build the structure, tell the device, set the CPU running */
int iwl22_boot(USHORT type)
{
   fwimg_t *im = &g_fw.img[type];
   static ULONG offs[UCODE_SECT_MAX];
   UCHAR FAR *c;
   USHORT lm, um, pg, i, k;
   static ULONG ph;

   iwl22_free_all();
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
   fmemset(DPTR(g_cring), 0, CQ_N * TFH_SIZE);
   c = DPTR(g_ctx);
   fmemset(c, 0, CTX_SIZE);
   put16(c + 0, (USHORT)iwl_rd(CSR_HW_REV));                /* version.mac_id                                      */
   put16(c + 2, 0);
   put16(c + 4, CTX_SIZE / 4);                             /* size in dwords                                       */
   /* control: TFD format long (bit 8), receive ring size log2(512) = 9 at bit 4, buffer size 4 KB (4) at bit 9 */
   put32(c + 8, 0x00000100UL | (9UL << 4) | (4UL << 9));
   put64(c + 24, iwl9_fbd_phys());                          /* rbd_cfg: free, used, status                          */
   put64(c + 32, iwl9_ubd_phys());
   put64(c + 40, iwl_rbstat_phys());
   put64(c + 48, g_cring.phys);                             /* hcmd_cfg: the command queue                          */
   c[56] = 2;                                              /* TFD_QUEUE_CB_SIZE(32) = log2(32) - 3                  */
   for (i = 0; i < lm; i++) {
      if (sec_alloc(&im->sec[i], (ULONG FAR *)&ph)) { g_ctx_fail = (USHORT)(0x100 + i); iwl22_free_all(); return -2; }
      g_img_ph[g_img_n++] = ph;
      put64(c + 704 + i * 8, ph);                           /* dram.lmac_img[i]                                     */
   }
   for (i = 0; i < um; i++) {
      k = (USHORT)(lm + 1 + i);
      if (sec_alloc(&im->sec[k], (ULONG FAR *)&ph)) { g_ctx_fail = (USHORT)(0x200 + i); iwl22_free_all(); return -3; }
      g_img_ph[g_img_n++] = ph;
      put64(c + 192 + i * 8, ph);                           /* dram.umac_img[i]                                     */
   }
   for (i = 0; i < pg; i++) {
      k = (USHORT)(lm + um + 2 + i);
      if (sec_alloc(&im->sec[k], (ULONG FAR *)&ph)) { g_ctx_fail = (USHORT)(0x300 + i); iwl22_free_all(); return -4; }
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

/* Command queue entry idx: a TFH descriptor of 256 bytes (num_tbs at 0, then up to 25 transfer buffers of 10 bytes: length at +0, 64-bit
 * address at +2). The first buffer holds the first 20 bytes of the command (the header), the second the rest (Linux
 * iwl_pcie_gen2_enqueue_hcmd). */
void iwl22_post(USHORT idx, ULONG phys, USHORT len)
{
   UCHAR FAR *t = DPTR(g_cring) + (idx & (CQ_N - 1)) * TFH_SIZE;
   USHORT l0 = len < 20 ? len : 20;

   fmemset(t, 0, TFH_SIZE);
   put16(t + 2, l0);
   put64(t + 4, phys);
   if (len > l0) {
      put16(t + 12, (USHORT)(len - l0));
      put64(t + 14, phys + (ULONG)l0);
      put16(t, 2);
   } else
      put16(t, 1);
}

/* A transmit queue, created by the firmware on request (Linux iwl_trans_pcie_dyn_txq_alloc, SCD_QUEUE_CFG in the data-path group): the host
 * supplies a ring of TFH descriptors and a byte-count table; the answer is the queue number and its first write pointer. */
USHORT g_q22_last, g_q22_wptr, g_q22_fail;
ULONG  g_q22_ring, g_q22_bc;
static DMA g_dring, g_dbc;                      /* the data queue's descriptor ring (32 TFDs) and byte-count table */
USHORT g_dq_n = 128;
static USHORT g_dq_ok, g_dq_qid;

int iwl22_txq_alloc(UCHAR sta_id, UCHAR tid, USHORT size, USHORT FAR *qid)
{
   static UCHAR cfg[24];
   int    rc;

   if (!g_dq_ok) {
      while (dma_get_al((USHORT)(size * TFH_SIZE), 256, &g_dring)) {      /* the ring: 128, else 64, else 32 entries */
         if (size <= 32) {
            g_q22_fail = 1;
            return -1;
         }
         size = (USHORT)(size >> 1);
      }
      if (dma_get_al(640, 64, &g_dbc)) {
         g_q22_fail = 1;
         return -1;
      }
      g_dq_n = size;
      g_dq_ok = 1;
   }
   size = g_dq_n;
   fmemset(DPTR(g_dring), 0, (USHORT)(size * TFH_SIZE));     /* physical memory is not cleared */
   fmemset(DPTR(g_dbc), 0, 640);
   g_q22_ring = g_dring.phys;
   g_q22_bc = g_dbc.phys;
   fmemset((UCHAR FAR *)cfg, 0, sizeof(cfg));
   cfg[0] = sta_id;
   cfg[1] = tid;
   put16((UCHAR FAR *)cfg + 2, 1);                          /* flags: TX_QUEUE_CFG_ENABLE_QUEUE                       */
   put32((UCHAR FAR *)cfg + 4, (ULONG)(size == 16 ? 1 : (size == 32 ? 2 : (size == 64 ? 3 : (size == 128 ? 4 : 5)))));   /* log2(size) - 3 */
   put64((UCHAR FAR *)cfg + 8, g_q22_bc);                   /* byte_cnt_addr                                          */
   put64((UCHAR FAR *)cfg + 16, g_q22_ring);                /* tfdq_addr                                              */
   rc = iwl_cmd(0x1D, (UCHAR FAR *)cfg, 24, (UCHAR FAR *)0, 0, CMDF_WANT_RESP);
   if (rc || g_resp_len < 8) {
      g_q22_fail = 3;
      return -3;
   }
   g_q22_last = get16(g_resp);                              /* queue_number                                           */
   g_q22_wptr = get16(g_resp + 4);                          /* write_pointer                                          */
   g_dq_qid = g_q22_last;
   *qid = g_q22_last;
   return 0;
}

/* Remove the data queue again (SCD_QUEUE_CFG with the enable flag clear), when the station goes away. */
void iwl22_txq_free(UCHAR sta_id, UCHAR tid)
{
   static UCHAR cfg[24];

   fmemset((UCHAR FAR *)cfg, 0, sizeof(cfg));
   cfg[0] = sta_id;
   cfg[1] = tid;
   iwl_cmd(0x1D, (UCHAR FAR *)cfg, 24, (UCHAR FAR *)0, 0, CMDF_WANT_RESP);
}

/* A data frame in the data queue's ring: three transfer buffers (the first 20 bytes of the command, the rest of the command
 * and the 802.11 header, the payload at ph + 256), and its entry in the byte-count table (frame length in dwords). */
void iwl22_data_tfd(USHORT idx, ULONG ph, USHORT cmdlen, USHORT paylen, USHORT totlen)
{
   UCHAR FAR *t = DPTR(g_dring) + (idx & (DQ_N - 1)) * TFH_SIZE;

   fmemset(t, 0, TFH_SIZE);
   put16(t + 2, 20);
   put64(t + 4, ph);
   put16(t + 12, (USHORT)(cmdlen - 20));
   put64(t + 14, ph + 20UL);
   if (paylen) {
      put16(t + 22, paylen);
      put64(t + 24, ph + 256UL);
      put16(t, 3);
   } else
      put16(t, 2);
   put16(DPTR(g_dbc) + (idx & (DQ_N - 1)) * 2, (USHORT)((totlen + 3) >> 2));
}

void iwl22_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void))
{
   ps("CTXINFO lmac="); ph(g_ctx_lm, 2); ps(" umac="); ph(g_ctx_um, 2); ps(" paging="); ph(g_ctx_pg, 2);
   ps(" dram="); ph(g_ctx_bytes, 8); ps(" fail="); ph(g_ctx_fail, 4); nl();
   ps("TXQ22 last="); ph(g_q22_last, 2); ps(" wptr="); ph(g_q22_wptr, 4); ps(" ring="); ph(g_q22_ring, 8); ps(" entries="); ph(g_dq_n, 3); ps(" fail="); ph(g_q22_fail, 2); nl();
}
