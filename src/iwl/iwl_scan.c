/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_scan.c - scanning with the firmware's UMAC scan (SCAN_CFG_CMD once, SCAN_REQ_UMAC per scan).
 *
 * Port of OpenBSD if_iwm.c iwm_config_umac_scan / iwm_umac_scan / iwm_fill_probe_req (ISC), offsets checked
 * against gcc's layout of the iwm structures (iwm_scan_req_umac v1/v7/v8, tails v1/v2). The scan is an active
 * scan with the wildcard SSID on every channel the NVM allows (the firmware itself keeps radar / no-IR channels
 * passive). Beacons and probe responses arrive as ordinary received frames (REPLY_RX_MPDU) and go to the
 * station's scan table; the scan ends with SCAN_COMPLETE_UMAC.
 */
#include "iwl.h"

USHORT g_scan_done, g_scan_n;
static DMA    g_sbuf;
static USHORT g_sbuf_ok;

static const UCHAR g_rates24[12] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C };
static const UCHAR g_rates5[8]   = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };

static int buf(void)
{
   if (!g_sbuf_ok) {
      if (dma_get(2048, &g_sbuf))
         return -1;
      g_sbuf_ok = 1;
   }
   fmemset(DPTR(g_sbuf), 0, 2048);
   return 0;
}

int iwl_scan_config(void)
{
   UCHAR FAR *c;
   USHORT n, i;

   if (buf())
      return -1;
   c = DPTR(g_sbuf);
   n = (g_nvm_nchan < g_fw.n_scan_channels) ? g_nvm_nchan : g_fw.n_scan_channels;
   put32(c + 4, (ULONG)iwl_valid_tx_ant());        /* tx_chains                                             */
   put32(c + 8, (ULONG)iwl_valid_rx_ant());        /* rx_chains                                             */
   put32(c + 12, SCAN_CONFIG_RATES_ALL | (SCAN_CONFIG_RATES_ALL << 16));   /* legacy and supported rates    */
   c[24] = 10;                                     /* dwell: active, passive, fragmented, extended (UNASSOC) */
   c[25] = 110;
   c[26] = 44;
   c[27] = 90;
   fmemcpy(c + 28, (UCHAR FAR *)g_hw_mac, 6);
   c[34] = AUX_STA_ID;                             /* bcast_sta_id                                          */
   c[35] = 0;                                      /* channel_flags                                         */
   for (i = 0; i < n; i++)
      c[36 + i] = g_nvm_chan[i];
   put32(c, SCAN_CONFIG_FLAG_ACTIVATE | SCAN_CONFIG_FLAG_ALLOW_CHUB_REQS | SCAN_CONFIG_FLAG_SET_TX_CHAINS |
            SCAN_CONFIG_FLAG_SET_RX_CHAINS | SCAN_CONFIG_FLAG_SET_AUX_STA_ID | SCAN_CONFIG_FLAG_SET_ALL_TIMES |
            SCAN_CONFIG_FLAG_SET_LEGACY_RATES | SCAN_CONFIG_FLAG_SET_MAC_ADDR | SCAN_CONFIG_FLAG_SET_CHANNEL_FLAGS |
            ((ULONG)n << 26) | SCAN_CONFIG_FLAG_CLEAR_FRAGMENTED);
   return iwl_cmd1(WIDE_ID(LONG_GROUP, CMD_SCAN_CFG), c, (USHORT)(36 + g_fw.n_scan_channels));
}

/* The probe request template: v2 has three band segments, v1 two. Returns its size. */
static void fill_preq(UCHAR FAR *pr, USHORT nbands)
{
   UCHAR FAR *b = pr + 4 + nbands * 4 + 4;         /* buf[]: after mac_header, band_data[], common_data     */
   USHORT k = 0, s, i;
   UCHAR FAR *seg = pr + 4;                        /* band_data[0]                                          */

   b[k++] = 0x40; b[k++] = 0;                      /* probe request, no flags                               */
   k += 2;                                         /* duration: filled by the hardware                       */
   for (i = 0; i < 6; i++) b[k++] = 0xFF;          /* DA                                                    */
   fmemcpy(b + k, (UCHAR FAR *)g_hw_mac, 6); k += 6;
   for (i = 0; i < 6; i++) b[k++] = 0xFF;          /* BSSID                                                 */
   k += 2;                                         /* sequence: filled by the hardware                       */
   b[k++] = 0; b[k++] = 0;                         /* SSID element (the hardware inserts the SSID)          */
   put16(pr, 0);                                   /* mac_header.offset                                     */
   put16(pr + 2, k);                               /* mac_header.len                                        */

   s = k;                                          /* 2.4 GHz: rates, extended rates, [DS parameter set]    */
   b[k++] = 1; b[k++] = 8;
   for (i = 0; i < 8; i++) b[k++] = g_rates24[i];
   b[k++] = 50; b[k++] = 4;
   for (i = 8; i < 12; i++) b[k++] = g_rates24[i];
   if (FW_CAPA(CAPA_DS_PARAM_SET_IE_SUPPORT)) {
      b[k++] = 3; b[k++] = 1; b[k++] = 0;
   }
   put16(seg, s);
   put16(seg + 2, (USHORT)(k - s));

   if (g_nvm_5ghz) {                               /* 5 GHz: rates                                          */
      s = k;
      b[k++] = 1; b[k++] = 8;
      for (i = 0; i < 8; i++) b[k++] = g_rates5[i];
      put16(seg + 4, s);
      put16(seg + 6, (USHORT)(k - s));
   }

   s = k;                                          /* both bands: HT capabilities (20 MHz, short GI 20)     */
   if (g_nvm_11n) {
      b[k++] = 45; b[k++] = 26;
      b[k++] = 0x2C; b[k++] = 0x00;                /* SM power save disabled, short GI 20                   */
      b[k++] = 0x03;                               /* A-MPDU parameters                                     */
      b[k++] = 0xFF;                               /* rx MCS 0-7                                            */
      b[k++] = (UCHAR)(((iwl_valid_rx_ant() & 3) == 3) ? 0xFF : 0x00);   /* rx MCS 8-15                       */
      for (i = 0; i < 21; i++) b[k++] = 0;
   }
   put16(pr + 4 + nbands * 4, s);                  /* common_data                                           */
   put16(pr + 4 + nbands * 4 + 2, (USHORT)(k - s));
}

/* LMAC scan (iwm_lmac_scan): SCAN_OFFLOAD_REQUEST_CMD, one active scan with the wildcard SSID on every channel the NVM allows.
 * Layout checked with gcc (struct iwm_scan_req_lmac, 764 bytes + channel configurations of 12 bytes + a v1 probe request). */
static int hw_scan_lmac(void)
{
   UCHAR FAR *r, FAR *d;
   USHORT n, i, rxc;
   ULONG  t0, sf, ant;
   USHORT len;

   if (buf())
      return -2;
   r = DPTR(g_sbuf);
   n = (g_nvm_nchan < g_fw.n_scan_channels) ? g_nvm_nchan : g_fw.n_scan_channels;
   len = (USHORT)(SCAN_REQ_LMAC_SIZE + 12 * g_fw.n_scan_channels + 528);
   if (len > 2048)
      return -3;
   r[4] = (UCHAR)n;                                /* n_channels                                            */
   r[5] = 10;                                      /* active dwell                                          */
   r[6] = 110;                                     /* passive                                               */
   r[7] = 44;                                      /* fragmented                                            */
   r[8] = 90;                                      /* extended                                              */
   rxc = (USHORT)iwl_valid_rx_ant();
   put16(r + 10, (USHORT)((rxc << PHY_RX_CHAIN_VALID_POS) | (rxc << PHY_RX_CHAIN_FORCE_MIMO_SEL_POS) |
                          (rxc << PHY_RX_CHAIN_FORCE_SEL_POS) | (1U << PHY_RX_CHAIN_DRIVER_FORCE_POS)));
   sf = LMAC_SCAN_FLAG_PASS_ALL | LMAC_SCAN_FLAG_ITER_COMPLETE | LMAC_SCAN_FLAG_EXTENDED_DWELL;
   if (FW_CAPA(CAPA_DS_PARAM_SET_IE_SUPPORT) && FW_CAPA(CAPA_WFA_TPC_REP_IE_SUPPORT))
      sf |= LMAC_SCAN_FLAGS_RRM_ENABLED;
   put32(r + 12, sf);
   put32(r + 24, 1UL);                             /* RX on flags: band 2.4 GHz (5 GHz is value 0 too)       */
   put32(r + 28, MAC_FILTER_ACCEPT_GRP | MAC_FILTER_IN_BEACON);
   ant = (ULONG)iwl_valid_tx_ant();
   ant = (ant & 1UL) ? 0x4000UL : ((ant & 2UL) ? 0x8000UL : 0x4000UL);   /* RATE_MCS_ANT_A / B               */
   for (i = 0; i < 2; i++) {                       /* tx_cmd[0] 2.4 GHz, [1] 5 GHz: 6 Mbit/s OFDM            */
      put32(r + 32 + i * 12, TX_CMD_FLG_SEQ_CTL | TX_CMD_FLG_BT_DIS);
      put32(r + 36 + i * 12, 13UL | ant);
      r[40 + i * 12] = AUX_STA_ID;
   }
   r[56] = 0;                                      /* direct_scan[0]: the wildcard SSID (element 0, length 0) */
   r[57] = 0;
   put32(r + 736, SCAN_PRIORITY_HIGH);
   put32(r + 740, 1UL);                            /* iter_num                                              */
   r[748 + 2] = 1;                                 /* schedule[0]: one iteration                             */
   r[748 + 3] = 1;
   r[756 + 2] = 1;                                 /* channel_opt: EBS disabled                             */
   r[760 + 2] = 1;
   d = r + SCAN_REQ_LMAC_SIZE;
   for (i = 0; i < n; i++) {
      put32(d + i * 12, UNIFIED_SCAN_CHANNEL_PARTIAL | 2UL);   /* partial, select SSID 0                       */
      put16(d + i * 12 + 4, g_nvm_chan[i]);
      put16(d + i * 12 + 6, 1);                    /* iter_count                                            */
   }
   fill_preq(d + 12 * g_fw.n_scan_channels, 2);    /* the v1 probe request: two band segments                */
   g_scan_done = 0;
   sta_age();
   if (iwl_cmd1(CMD_SCAN_OFFLOAD_REQUEST, r, len))
      return -4;
   g_scan_n++;
   t0 = dd_ms();
   while (!g_scan_done) {
      hw_wait(&g_scan_done, 200);
      if (g_fw_error)
         return -5;
      if ((long)(dd_ms() - t0) > 12000L) {
         iwl_cmd1(CMD_SCAN_OFFLOAD_ABORT, (UCHAR FAR *)0, 0);
         return -6;
      }
   }
   return 0;
}

int hw_scan(void)
{
   UCHAR FAR *r, FAR *d, FAR *tail;
   USHORT base, tlen, n, i, ext = (USHORT)FW_API(API_SCAN_EXT_CHAN_VER), chanoff;
   USHORT gf;
   ULONG  t0;

   if (!g_hw_ready)
      return -1;
   if (!FW_CAPA(CAPA_UMAC_SCAN))
      return hw_scan_lmac();
   if (buf())
      return -2;
   r = DPTR(g_sbuf);
   if (FW_API(API_ADAPTIVE_DWELL_V2))      { base = SCAN_REQ_UMAC_SIZE_V8; chanoff = 48; }
   else if (FW_API(API_ADAPTIVE_DWELL))    { base = SCAN_REQ_UMAC_SIZE_V7; chanoff = 44; }
   else                                    { base = SCAN_REQ_UMAC_SIZE_V1; chanoff = 32; }
   tlen = (USHORT)(ext ? SCAN_TAIL_V2_SIZE : SCAN_TAIL_V1_SIZE);
   if ((USHORT)(base + 8 * g_fw.n_scan_channels + tlen) > 2048)
      return -3;

   put32(r + 8, SCAN_PRIORITY_HIGH);               /* ooc_priority                                          */
   if (FW_API(API_ADAPTIVE_DWELL)) {
      r[19] = 2;                                   /* adwell default APs (low band), social channels         */
      r[20] = 10;
      put16(r + 22, 300);                          /* adwell_max_budget for a full scan                     */
      put32(r + 40, SCAN_PRIORITY_HIGH);
      if (FW_API(API_ADAPTIVE_DWELL_V2)) {
         r[16] = 10;                               /* active_dwell[LB]                                       */
         r[21] = 0x02;                             /* general_flags2: ALLOW_CHNL_REORDER                    */
         r[44] = 110;                              /* passive_dwell[LB]                                     */
      } else {
         r[16] = 10; r[17] = 110; r[18] = 44;      /* active, passive, fragmented                           */
      }
   } else {
      r[16] = 90; r[17] = 10; r[18] = 110; r[19] = 44;   /* extended, active, passive, fragmented          */
      put32(r + 28, SCAN_PRIORITY_HIGH);
   }

   d = r + base;                                   /* channel configurations                                */
   n = (g_nvm_nchan < g_fw.n_scan_channels) ? g_nvm_nchan : g_fw.n_scan_channels;
   for (i = 0; i < n; i++) {
      d[i * 8 + 4] = g_nvm_chan[i];                /* channel_num                                           */
      d[i * 8 + 5] = 1;                            /* iter_count                                            */
   }
   r[chanoff + 1] = (UCHAR)n;                      /* channel.count                                         */

   gf = UMAC_SCAN_GEN_FLAGS_PASS_ALL | UMAC_SCAN_GEN_FLAGS_ITER_COMPLETE;
   if (FW_CAPA(CAPA_DS_PARAM_SET_IE_SUPPORT) && FW_CAPA(CAPA_WFA_TPC_REP_IE_SUPPORT))
      gf |= UMAC_SCAN_GEN_FLAGS_RRM_ENABLED;
   gf |= FW_API(API_ADAPTIVE_DWELL) ? UMAC_SCAN_GEN_FLAGS_ADAPTIVE_DWELL : UMAC_SCAN_GEN_FLAGS_EXTENDED_DWELL;
   put16(r + 12, gf);                              /* general_flags                                         */

   tail = d + 8 * g_fw.n_scan_channels;
   tail[2] = 1;                                    /* schedule[0]: interval 0, one iteration                 */
   fill_preq(tail + 12, (USHORT)(ext ? 3 : 2));

   g_scan_done = 0;
   sta_age();
   if (iwl_cmd1(WIDE_ID(LONG_GROUP, CMD_SCAN_REQ_UMAC), r, (USHORT)(base + 8 * g_fw.n_scan_channels + tlen)))
      return -4;
   g_scan_n++;
   t0 = dd_ms();
   while (!g_scan_done) {                          /* about 3 s for 2.4 + 5 GHz                              */
      hw_wait(&g_scan_done, 200);
      if (g_fw_error)
         return -5;
      if ((long)(dd_ms() - t0) > 12000L) {
         static UCHAR ab[8];                       /* uid 0, flags 0                                         */

         iwl_cmd1(WIDE_ID(LONG_GROUP, CMD_SCAN_ABORT_UMAC), (UCHAR FAR *)ab, 8);
         return -6;
      }
   }
   return 0;
}

void iwl_scan_done(void)
{
   g_scan_done = 1;
}
