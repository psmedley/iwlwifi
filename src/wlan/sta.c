/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sta.c - the chip-independent station: scan table, join (authentication, association), leave, keys, the EAPOL
 * queue for wpa_supplicant and the conversion between Ethernet frames (NDIS) and 802.11 data frames.
 *
 * Derived from ath_sta.c of the QCA6174 driver (github.com/psmedley/qca6174). On Linux this is mac80211's job; the chip
 * back end (hw.h) only provides the firmware side: being on the AP's channel with a station entry for it
 * (hw_join_prepare), sending frames, and the post-association setup (hw_join_complete).
 *
 * All of it runs in the GenMAC request path (task time, under nic_lock) and may block in hw_wait(), except
 * sta_rx_frame()/sta_beacon(), which run in the service path (task time, never blocking).
 */
#include "drv.h"

net_t  g_net[NET_MAX];
USHORT g_nnet;
USHORT g_joined, g_authorized, g_ptk_on, g_open_net;
UCHAR  g_jbssid[6], g_jssid[32], g_jslen, g_jch;
USHORT g_jfail;
UCHAR  g_jreq[200], g_jres[160];
USHORT g_jreqlen, g_jreslen, g_jcaps, g_jaid, g_jbcn, g_jdtim;
UCHAR  g_jht, g_jht_mcs[2], g_jrates[12];
UCHAR  g_j40, g_jsco, g_jnss = 1;
USHORT g_jnrates;
UCHAR  g_forcech, g_maxw = 2, g_maxnss = 2;      /* MAXWIDTH: 0 = 20, 1 or more = 40 MHz (80 is not implemented) */
USHORT g_link_lost;
net_t *g_jnet;
USHORT g_rx_mgmt[16];
USHORT g_rx_eapol, g_rx_ndis, g_rx_nobuf, g_rx_data, g_rx_drop[8], g_tx_data, g_tx_busy;

static USHORT g_auth_rx, g_auth_status, g_assoc_rx, g_assoc_status, g_assoc_aid, g_assoc_caps, g_assoc_reslen;
static UCHAR  g_assoc_res[192];
static UCHAR  g_want_ssid[33];
static UCHAR  g_want_len;
static USHORT g_want_open;
static UCHAR  g_target[6];
static USHORT g_joining, g_prepared, g_seq;
static UCHAR  g_frame[300];
static UCHAR  g_hdr[24];


static int mac_eq(UCHAR FAR *a, UCHAR FAR *b)
{
   return fmemeq(a, b, 6);
}

/* The element with this id in a block of information elements, or NULL. */
static UCHAR FAR *find_ie(UCHAR FAR *b, USHORT len, UCHAR id)
{
   USHORT pos = 0;

   while ((USHORT)(pos + 2) <= len) {
      if ((USHORT)(pos + 2 + b[pos + 1]) > len)
         break;
      if (b[pos] == id)
         return b + pos;
      pos = (USHORT)(pos + 2 + b[pos + 1]);
   }
   return 0;
}

/* The WMM element (vendor specific, OUI 00:50:F2, type 2) */
static UCHAR FAR *find_wmm(UCHAR FAR *b, USHORT len)
{
   USHORT pos = 0;

   while ((USHORT)(pos + 2) <= len) {
      UCHAR l = b[pos + 1];

      if ((USHORT)(pos + 2 + l) > len)
         break;
      if (b[pos] == 221 && l >= 7 && b[pos + 2] == 0x00 && b[pos + 3] == 0x50 && b[pos + 4] == 0xF2 &&
          b[pos + 5] == 2)
         return b + pos;
      pos = (USHORT)(pos + 2 + l);
   }
   return 0;
}

/* ---- scan table ------------------------------------------------------------------------------------- */
void sta_scan_clear(void)
{
   g_nnet = 0;
}

/* Drop entries not heard for 30 s (the table is not cleared at a scan: the joined AP must stay in it). */
void sta_age(void)
{
   USHORT k = 0, i;
   ULONG  now = dd_ms();

   for (i = 0; i < g_nnet; i++) {
      if ((long)(now - g_net[i].seen) > 30000L && !(g_joined && mac_eq(g_net[i].bssid, g_jbssid)))
         continue;
      if (k != i)
         g_net[k] = g_net[i];
      k++;
   }
   g_nnet = k;
   if (g_jnet && !g_joined && !g_joining)
      g_jnet = 0;
}

/* f: beacon or probe response (24-byte header); chan: the channel the frame was received on */
void sta_beacon(UCHAR FAR *f, USHORT len, USHORT chan, short rssi, ULONG rxtime)
{
   UCHAR FAR *bssid = f + 16;
   UCHAR FAR *body = f + 24;
   UCHAR FAR *e;
   USHORT blen, k, i, slot;
   net_t *n;

   if (len < 24 + 12)
      return;
   blen = (USHORT)(len - 24);
   e = find_ie(body + 12, (USHORT)(blen - 12), 3);       /* DS parameter set: more reliable than the rx channel */
   if (e && e[1] == 1)
      chan = e[2];
   slot = 0xFFFF;
   for (k = 0; k < g_nnet; k++)
      if (mac_eq(bssid, g_net[k].bssid)) { slot = k; break; }
   if (slot == 0xFFFF) {
      if (g_nnet < NET_MAX)
         slot = g_nnet++;
      else {
         slot = 0;
         for (k = 1; k < NET_MAX; k++)
            if (g_net[k].rssi < g_net[slot].rssi)
               slot = k;
         if (g_net[slot].rssi > rssi)
            return;
      }
   }
   n = &g_net[slot];
   if (n == g_jnet && (g_joining || g_joined) && !mac_eq(n->bssid, bssid))
      return;
   fmemcpy(n->bssid, bssid, 6);
   n->ch = (UCHAR)chan;
   n->rssi = (signed char)(rssi < -127 ? -127 : (rssi > 0 ? 0 : rssi));
   n->sec = (UCHAR)((body[10] & 0x10) ? 1 : 0);
   n->tsf_lo = get32(body);
   n->tsf_hi = get32(body + 4);
   n->rx_time = rxtime;
   e = find_ie(body + 12, (USHORT)(blen - 12), 45);       /* HT capabilities                                 */
   n->htc = (UCHAR)(e && e[1] >= 26);
   n->ht_mcs[0] = n->htc ? e[2 + 3] : 0;
   n->ht_mcs[1] = n->htc ? e[2 + 4] : 0;
   n->ht40 = (UCHAR)(n->htc ? ((e[2] >> 1) & 1) : 0);
   e = find_ie(body + 12, (USHORT)(blen - 12), 61);       /* HT operation                                    */
   n->htw = n->sco = 0;
   if (e && e[1] >= 22) {
      n->sco = (UCHAR)(e[3] & 3);
      n->htw = (UCHAR)((e[3] >> 2) & 1);
   }
   n->wmm = (UCHAR)(find_wmm(body + 12, (USHORT)(blen - 12)) != 0);
   n->len = 0;
   e = find_ie(body + 12, (USHORT)(blen - 12), 0);
   if (e && e[1] <= 32) {
      n->len = e[1];
      for (i = 0; i < n->len; i++)
         n->ssid[i] = (char)e[2 + i];
   }
   n->ielen = (blen > NET_IE_MAX) ? NET_IE_MAX : blen;
   fmemcpy((UCHAR FAR *)n->ie, body, n->ielen);
   n->seen = dd_ms();
}

short sta_rssi(void)
{
   USHORT k;

   for (k = 0; k < g_nnet; k++)
      if (mac_eq(g_net[k].bssid, g_jbssid))
         return g_net[k].rssi;
   return -70;
}

/* ---- what to join ---------------------------------------------------------------------------------- */
void sta_set_want(UCHAR FAR *ssid, UCHAR len, USHORT open)
{
   if (len > 32)
      len = 32;
   fmemcpy((UCHAR FAR *)g_want_ssid, ssid, len);
   g_want_ssid[len] = 0;
   g_want_len = len;
   g_want_open = open;
}

static int ssid_match(net_t *n)
{
   if (!g_want_len || n->len != g_want_len)
      return 0;
   return fmemeq((UCHAR FAR *)n->ssid, (UCHAR FAR *)g_want_ssid, g_want_len);
}

/* ---- management frames ------------------------------------------------------------------------------ */
static const UCHAR g_rates24[12] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C };
static const UCHAR g_rates5[8]   = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
/* RSN element: version 1, group CCMP, 1 pairwise CCMP, 1 AKM PSK, no capabilities (WPA2-PSK / CCMP only) */
static const UCHAR g_rsn[22] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F,
                                 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };
/* WMM information element: version 1, QoS info 0 (no U-APSD) */
static const UCHAR g_wmm_ie[9] = { 221, 7, 0x00, 0x50, 0xF2, 0x02, 0x00, 0x01, 0x00 };
/* HT capabilities: 20 MHz only, short GI 20, SM power save disabled; A-MPDU 64 KB, no density limit;
 * rx MCS 0-7 (byte 1 = MCS 8-15 is set for a two-stream radio) */
static const UCHAR g_htcap[28] = { 45, 26, 0x2C, 0x00, 0x03,
                                   0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0 };

static void seq_put(UCHAR FAR *h)
{
   USHORT fl = rt_irq_save(), q;

   g_seq = (USHORT)((g_seq + 1) & 0x0FFF);
   q = g_seq;
   rt_irq_restore(fl);
   h[22] = (UCHAR)(q << 4);                            /* sequence control: number << 4                      */
   h[23] = (UCHAR)(q >> 4);
}

static void mgmt_hdr(UCHAR fc0)
{
   rt_setmem((PVOID)g_frame, 0, 24);
   g_frame[0] = fc0;
   fmemcpy((UCHAR FAR *)g_frame + 4, (UCHAR FAR *)g_target, 6);   /* DA = AP                                    */
   fmemcpy((UCHAR FAR *)g_frame + 10, (UCHAR FAR *)g_mymac, 6);   /* SA = us                                    */
   fmemcpy((UCHAR FAR *)g_frame + 16, (UCHAR FAR *)g_target, 6);  /* BSSID                                      */
   seq_put((UCHAR FAR *)g_frame);
}

static int tx_auth(void)
{
   mgmt_hdr(0xB0);
   g_frame[24] = g_frame[25] = 0;                      /* algorithm: open system                            */
   g_frame[26] = 1; g_frame[27] = 0;                   /* transaction sequence 1                            */
   g_frame[28] = g_frame[29] = 0;                      /* status                                            */
   return hw_mgmt_tx((UCHAR FAR *)g_frame, 30);
}

static int tx_deauth(void)
{
   mgmt_hdr(0xC0);
   g_frame[24] = 3;                                    /* reason 3: leaving                                 */
   g_frame[25] = 0;
   return hw_mgmt_tx((UCHAR FAR *)g_frame, 26);
}

/* Refuse a block-ack agreement (ADDBA request from the AP): status 37, request declined. Without an answer
 * some APs keep asking; with a refusal they send plain MPDUs. */
static void tx_addba_refuse(UCHAR FAR *req, USHORT rlen)
{
   static UCHAR f[33];                                 /* service path: not g_frame (the request path's)     */

   if (rlen < 24 + 9)
      return;
   rt_setmem((PVOID)f, 0, sizeof(f));
   f[0] = 0xD0;
   fmemcpy((UCHAR FAR *)f + 4, (UCHAR FAR *)g_target, 6);
   fmemcpy((UCHAR FAR *)f + 10, (UCHAR FAR *)g_mymac, 6);
   fmemcpy((UCHAR FAR *)f + 16, (UCHAR FAR *)g_target, 6);
   seq_put((UCHAR FAR *)f);
   f[24] = 3;                                          /* category: block ack                               */
   f[25] = 1;                                          /* ADDBA response                                    */
   f[26] = req[24 + 2];                                /* dialog token                                      */
   f[27] = 37; f[28] = 0;                              /* status: request declined                          */
   f[29] = req[24 + 3]; f[30] = req[24 + 4];           /* block ack parameter set                           */
   f[31] = req[24 + 5]; f[32] = req[24 + 6];           /* timeout                                           */
   hw_mgmt_tx((UCHAR FAR *)f, 33);
}

static int tx_assoc(net_t *n)
{
   USHORT i, k = 24, cap;
   UCHAR  hi = (UCHAR)(n->ch > 14);

   mgmt_hdr(0x00);
   cap = (USHORT)(g_want_open ? 0x0021 : 0x0031);      /* ESS, [privacy], short preamble                    */
   if (!hi)
      cap |= 0x0400;                                   /* short slot time                                   */
   g_frame[k++] = (UCHAR)cap;
   g_frame[k++] = (UCHAR)(cap >> 8);
   g_frame[k++] = 10; g_frame[k++] = 0;                /* listen interval                                   */
   g_frame[k++] = 0;  g_frame[k++] = n->len;           /* SSID                                              */
   for (i = 0; i < n->len; i++)
      g_frame[k++] = (UCHAR)n->ssid[i];
   g_frame[k++] = 1;  g_frame[k++] = 8;                /* supported rates                                   */
   for (i = 0; i < 8; i++)
      g_frame[k++] = hi ? g_rates5[i] : g_rates24[i];
   if (!hi) {
      g_frame[k++] = 50; g_frame[k++] = 4;             /* extended supported rates                          */
      for (i = 8; i < 12; i++)
         g_frame[k++] = g_rates24[i];
   }
   if (!g_want_open)
      for (i = 0; i < 22; i++)
         g_frame[k++] = g_rsn[i];
   if (g_jht) {                                        /* after the RSN element, as mac80211 orders them     */
      for (i = 0; i < 28; i++)
         g_frame[k++] = g_htcap[i];
      if (g_hw_nss >= 2 && g_maxnss >= 2)
         g_frame[k - 28 + 6] = 0xFF;                   /* rx MCS 8-15                                        */
      if (g_j40)
         g_frame[k - 28 + 2] |= 0x42;                  /* supports 40 MHz, short GI 40                       */
   }
   if (n->wmm)
      for (i = 0; i < 9; i++)
         g_frame[k++] = g_wmm_ie[i];
   g_jreqlen = (USHORT)(k - 28);                       /* what the supplicant reads back as request IEs     */
   if (g_jreqlen > sizeof(g_jreq))
      g_jreqlen = sizeof(g_jreq);
   fmemcpy((UCHAR FAR *)g_jreq, (UCHAR FAR *)g_frame + 28, g_jreqlen);
   return hw_mgmt_tx((UCHAR FAR *)g_frame, k);
}

/* A management frame that is not a beacon or probe response. Service path. */
static void sta_mgmt_rx(UCHAR FAR *f, USHORT len)
{
   UCHAR fc0 = f[0];
   USHORT n;

   if (!g_joining && !g_joined)
      return;
   if (!mac_eq(f + 10, (UCHAR FAR *)g_target))          /* sender must be the AP we talk to                  */
      return;
   g_rx_mgmt[(fc0 >> 4) & 15]++;
   if (fc0 == 0xB0 && len >= 30) {                       /* authentication                                    */
      if (f[26] == 2 && f[27] == 0) {                    /* transaction sequence 2                            */
         g_auth_status = get16(f + 28);
         g_auth_rx = 1;
      }
   } else if ((fc0 == 0x10 || fc0 == 0x30) && len >= 30) {   /* association / reassociation response          */
      g_assoc_caps   = get16(f + 24);
      g_assoc_status = get16(f + 26);
      g_assoc_aid    = (USHORT)(get16(f + 28) & 0x3FFF);
      n = (USHORT)(len - 30);
      if (n > sizeof(g_assoc_res))
         n = sizeof(g_assoc_res);
      fmemcpy((UCHAR FAR *)g_assoc_res, f + 30, n);
      g_assoc_reslen = n;
      g_assoc_rx = 1;
   } else if (fc0 == 0xC0 || fc0 == 0xA0) {              /* deauthentication / disassociation                 */
      g_link_lost = 1;
   } else if (fc0 == 0xD0 && len >= 26 && g_joined) {    /* action                                            */
      if (f[24] == 3 && f[25] == 0)                      /* block ack: ADDBA request                          */
         tx_addba_refuse(f, len);
   }
}

void sta_lost(void)
{
   if (g_joined)
      g_link_lost = 1;
}

/* ---- receive ----------------------------------------------------------------------------------------- */
#define EQ_N   4
#define EQ_SZ  512
static UCHAR  g_eq[EQ_N][EQ_SZ];
static USHORT g_eql[EQ_N], g_eqh, g_eqt, g_eqn, g_eq_drop;
static UCHAR  g_eth[1520];

USHORT sta_eapol_count(void)
{
   return g_eqn;
}

USHORT sta_eapol_get(UCHAR FAR *dst, USHORT max)
{
   USHORT n;

   if (!g_eqn)
      return 0;
   n = g_eql[g_eqt];
   if (n > max)
      n = max;
   fmemcpy(dst, (UCHAR FAR *)g_eq[g_eqt], n);
   g_eqt = (USHORT)((g_eqt + 1) % EQ_N);
   g_eqn--;
   return n;
}

static void rx_eth(UCHAR FAR *eth, USHORT len)
{
   static USHORT h;                                    /* address passed on: static (SS != DS)               */
   UCHAR FAR *d;

   if (eth[12] == 0x88 && eth[13] == 0x8E) {           /* EAPOL: to the supplicant, never to the stack      */
      g_rx_eapol++;
      if (g_eqn >= EQ_N || len > EQ_SZ) {
         g_eq_drop++;
         return;
      }
      fmemcpy((UCHAR FAR *)g_eq[g_eqh], eth, len);
      g_eql[g_eqh] = len;
      g_eqh = (USHORT)((g_eqh + 1) % EQ_N);
      g_eqn++;
      return;
   }
   if (!g_open_net && !g_ptk_on) {                     /* secure network, no keys yet: nothing else passes  */
      g_rx_drop[5]++;
      return;
   }
   d = ndis_rx_get(&h);
   if (!d) {
      g_rx_nobuf++;
      return;
   }
   fmemcpy(d, eth, len);
   g_rx_ndis++;
   ndis_rx_put(h, len);
}

/* An 802.11 frame from the air (FCS removed). Service path: must not block. */
void sta_rx_frame(UCHAR FAR *f, USHORT len, UCHAR chan, short rssi, USHORT rxflags, ULONG rxtime)
{
   UCHAR  fc0, fc1, type;
   USHORT hl;

   if (len < 24)
      return;
   fc0 = f[0];
   fc1 = f[1];
   type = (UCHAR)((fc0 >> 2) & 3);
   if (type == 0) {
      if (fc0 == 0x80 || fc0 == 0x50) {                /* beacon, probe response                            */
         sta_beacon(f, len, chan, rssi, rxtime);
         if (g_joined && mac_eq(f + 16, (UCHAR FAR *)g_jbssid))
            g_link_lost = 0;
      } else
         sta_mgmt_rx(f, len);
      return;
   }
   if (type != 2)
      return;
   g_rx_data++;
   if (!g_joined || (fc1 & 0x03) != 0x02 || !mac_eq(f + 10, (UCHAR FAR *)g_jbssid)) {
      g_rx_drop[0]++;                                  /* not From-DS from our AP                           */
      return;
   }
   if (fc0 & 0x40) {                                   /* null data / QoS null                              */
      g_rx_drop[1]++;
      return;
   }
   hl = 24;
   if (fc0 & 0x80) {                                   /* QoS data                                          */
      if (f[24] & 0x80) {                              /* A-MSDU: not negotiated, not handled               */
         g_rx_drop[2]++;
         return;
      }
      hl += 2;
      if (fc1 & 0x80)
         hl += 4;                                      /* HT control                                        */
   }
   if (fc1 & 0x40) {                                   /* protected                                         */
      if (!(rxflags & RXF_DECRYPTED)) {
         g_rx_drop[3]++;
         return;
      }
      hl += 8;                                         /* CCMP header; the 8-byte MIC is at the end         */
      if (len < hl + 8) { g_rx_drop[4]++; return; }
      len = (USHORT)(len - 8);
   }
   if (len < hl + 8 || (USHORT)(len - hl - 8 + 14) > sizeof(g_eth)) {
      g_rx_drop[4]++;
      return;
   }
   if (f[hl] != 0xAA || f[hl + 1] != 0xAA || f[hl + 2] != 0x03) {
      g_rx_drop[6]++;                                  /* not LLC/SNAP                                      */
      return;
   }
   fmemcpy((UCHAR FAR *)g_eth, f + 4, 6);              /* destination = address 1                           */
   fmemcpy((UCHAR FAR *)g_eth + 6, f + 16, 6);         /* source = address 3 (From-DS)                      */
   fmemcpy((UCHAR FAR *)g_eth + 12, f + hl + 6, (USHORT)(len - hl - 6));   /* ethertype + payload               */
   rx_eth((UCHAR FAR *)g_eth, (USHORT)(len - hl - 6 + 12));
   (void)rssi;
}

/* ---- transmit ---------------------------------------------------------------------------------------- */
/* eth: an Ethernet frame the caller lets us modify (bytes 6..11 become the LLC/SNAP header). */
static USHORT g_txlk;                                  /* NDIS TransmitChain may run at interrupt time, on any CPU */

static int tx_eth(UCHAR FAR *eth, USHORT len, USHORT flags)
{
   int    rc;
   USHORT fl;

   if (len < 14)
      return -1;
   fl = rt_irq_save();
   if (rt_lock_acquire(&g_txlk, (USHORT)(rt_cpu_id() + 1))) {
      rt_irq_restore(fl);                              /* re-entered on this CPU: busy                       */
      g_tx_busy++;
      return -2;
   }
   g_hdr[0] = 0x08;                                    /* data                                              */
   g_hdr[1] = (UCHAR)(0x01 | ((flags & HWTX_PROTECT) ? 0x40 : 0));   /* To-DS, [protected]                    */
   g_hdr[2] = g_hdr[3] = 0;
   fmemcpy((UCHAR FAR *)g_hdr + 4, (UCHAR FAR *)g_jbssid, 6);      /* receiver = AP                        */
   fmemcpy((UCHAR FAR *)g_hdr + 10, eth + 6, 6);                    /* transmitter = source                 */
   fmemcpy((UCHAR FAR *)g_hdr + 16, eth, 6);                        /* destination                          */
   seq_put((UCHAR FAR *)g_hdr);
   eth[6] = 0xAA; eth[7] = 0xAA; eth[8] = 0x03; eth[9] = 0; eth[10] = 0; eth[11] = 0;
   rc = hw_data_tx((UCHAR FAR *)g_hdr, 24, eth + 6, (USHORT)(len - 6), flags);
   rt_lock_release(&g_txlk);
   rt_irq_restore(fl);
   if (rc == 0)
      g_tx_data++;
   else
      g_tx_busy++;
   return rc;
}

int sta_tx(UCHAR FAR *eth, USHORT len)
{
   if (!g_joined)
      return -1;
   if (!g_open_net && !g_ptk_on)
      return -1;                                       /* never send data in the clear on a secure network */
   return tx_eth(eth, len, (USHORT)(g_open_net ? 0 : HWTX_PROTECT));
}

int sta_eapol_tx(UCHAR FAR *eth, USHORT len)
{
   static UCHAR buf[EQ_SZ];

   if (!g_joined || len > sizeof(buf))
      return -1;
   fmemcpy((UCHAR FAR *)buf, eth, len);
   /* Unprotected while the pairwise key is not installed (4-way handshake messages 2 and 4); afterwards the
    * group-key handshake and re-keying must be encrypted, or the access point ignores them. */
   return tx_eth((UCHAR FAR *)buf, len, (USHORT)(HWTX_EAPOL | (g_ptk_on ? HWTX_PROTECT : 0)));
}

/* ---- join --------------------------------------------------------------------------------------------- */
static void abort_join(void)
{
   if (g_prepared) {
      hw_leave();
      g_prepared = 0;
   }
   g_joining = 0;
}

/* The legacy rates of the AP's association response (supported + extended rates), at most 12. */
static void ap_rates(void)
{
   USHORT pos = 0, i;

   g_jnrates = 0;
   while (pos + 2 <= g_assoc_reslen) {
      UCHAR id = g_assoc_res[pos], l = g_assoc_res[pos + 1];

      if ((USHORT)(pos + 2 + l) > g_assoc_reslen)
         break;
      if (id == 1 || id == 50)
         for (i = 0; i < l && g_jnrates < 12; i++)
            g_jrates[g_jnrates++] = g_assoc_res[pos + 2 + i];
      pos = (USHORT)(pos + 2 + l);
   }
}

int sta_join(UCHAR FAR *want_bssid, USHORT any)
{
   USHORT k, best = 0xFFFF, att, pos;
   int    bestr = -1000, rv;
   net_t *n;
   UCHAR FAR *h;

   g_jfail = 0;
   if (!g_hw_ready) { g_jfail = 8; return -1; }
   if (g_joined)
      sta_leave();
   g_link_lost = 0;
   for (k = 0; k < g_nnet; k++) {
      if (!ssid_match(&g_net[k]))
         continue;
      if (!any && !mac_eq(g_net[k].bssid, want_bssid))
         continue;
      if (g_forcech && g_net[k].ch != g_forcech)
         continue;
      rv = g_net[k].rssi ? (int)g_net[k].rssi : -120;
      if (best == 0xFFFF || rv > bestr) {
         best = k;
         bestr = rv;
      }
   }
   if (best == 0xFFFF) {
      g_jfail = 1;                                      /* no network with that SSID in the scan table       */
      return -1;
   }
   n = &g_net[best];
   g_jnet = n;
   fmemcpy((UCHAR FAR *)g_target, (UCHAR FAR *)n->bssid, 6);
   fmemcpy((UCHAR FAR *)g_jbssid, (UCHAR FAR *)n->bssid, 6);
   g_jch = n->ch;
   g_joining = 1;
   g_auth_status = g_assoc_status = 0xFFFF;

   g_jbcn = get16((UCHAR FAR *)n->ie + 8);
   if (g_jbcn < 10 || g_jbcn > 2000)
      g_jbcn = 100;
   g_jdtim = 1;
   h = (n->ielen > 12) ? find_ie((UCHAR FAR *)n->ie + 12, (USHORT)(n->ielen - 12), 5) : 0;
   if (h && h[1] >= 2 && h[3] >= 1)
      g_jdtim = h[3];                                   /* TIM element: DTIM period                          */
   g_jht = n->htc;
   g_j40 = g_jsco = 0;
   if (g_jht && g_maxw >= 1 && n->ht40 && n->htw && (n->sco == 1 || n->sco == 3) && hw_chan_ht40(n->ch)) {
      g_j40 = 1;                                        /* the AP runs a 40 MHz channel and may use it here: so do we */
      g_jsco = n->sco;
   }
   g_jaid = 0;

   if (hw_join_prepare(n)) { g_jfail = 9; abort_join(); return -1; }
   g_prepared = 1;

   for (att = 1; att <= 3; att++) {                       /* open-system authentication                        */
      g_auth_rx = 0;
      if (tx_auth()) { g_jfail = 10; abort_join(); return -1; }
      if (!hw_wait(&g_auth_rx, 400))
         break;
   }
   if (!g_auth_rx)               { g_jfail = 2; abort_join(); return -1; }   /* no reply                            */
   if (g_auth_status != 0)       { g_jfail = 3; abort_join(); return -1; }   /* refused                             */

   for (att = 1; att <= 3; att++) {                       /* association                                       */
      g_assoc_rx = 0;
      if (tx_assoc(n)) { g_jfail = 10; abort_join(); return -1; }
      if (!hw_wait(&g_assoc_rx, 600))
         break;
   }
   if (!g_assoc_rx)              { g_jfail = 4; abort_join(); return -1; }   /* no reply                            */
   if (g_assoc_status != 0)      { g_jfail = 5; abort_join(); return -1; }   /* refused                             */

   ap_rates();
   h = g_jht ? find_ie((UCHAR FAR *)g_assoc_res, g_assoc_reslen, 45) : 0;
   if (h && h[1] < 26)
      h = 0;
   g_jht = (UCHAR)(h != 0);                              /* the AP confirmed HT in its response               */
   if (!g_jht)
      g_j40 = 0;
   g_jht_mcs[0] = h ? h[2 + 3] : 0;
   g_jht_mcs[1] = h ? h[2 + 4] : 0;
   g_jaid = g_assoc_aid;
   g_jcaps = g_assoc_caps;
   for (pos = 0; pos < 32; pos++)
      g_jssid[pos] = (UCHAR)n->ssid[pos];
   g_jslen = n->len;
   g_jreslen = (g_assoc_reslen > sizeof(g_jres)) ? (USHORT)sizeof(g_jres) : g_assoc_reslen;
   fmemcpy((UCHAR FAR *)g_jres, (UCHAR FAR *)g_assoc_res, g_jreslen);

   if (hw_join_complete()) {
      g_jfail = 11;
      tx_deauth();
      abort_join();
      return -1;
   }
   g_open_net = g_want_open;
   g_ptk_on = 0;
   g_authorized = 0;
   g_joined = 1;
   g_joining = 0;
   if (g_want_open)
      g_authorized = 1;                                   /* nothing to wait for on an open network            */
   tstr("[joined aid "); th16(g_jaid); tstr("]"); tnl();
   return 0;
}

void sta_leave(void)
{
   if (!g_joined && !g_joining && !g_prepared)
      return;
   if (g_joined)
      tx_deauth();
   g_joined = g_authorized = g_ptk_on = 0;
   g_jht = 0;
   g_j40 = g_jsco = 0;
   g_jnss = 1;
   abort_join();
   g_link_lost = 0;
}

/* Request path: notice that the AP is gone (deauthentication, disassociation, missed beacons). */
void sta_poll(void)
{
   if (g_link_lost && g_joined) {
      tstr("[link lost]"); tnl();
      sta_leave();
   }
   g_link_lost = 0;
}

/* ---- keys ------------------------------------------------------------------------------------------- */
int sta_key_add(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx)
{
   if (!g_joined)
      return -1;
   if (klen != 16)
      return -2;                                          /* CCMP only                                         */
   if (hw_key_set(key, 16, pairwise, idx))
      return -3;
   if (pairwise) {
      g_ptk_on = 1;
      g_authorized = 1;
   }
   return 0;
}

int sta_key_remove(USHORT pairwise, UCHAR idx)
{
   if (!g_joined || !pairwise)
      return 0;                                           /* group keys are replaced, not removed              */
   g_ptk_on = 0;
   return hw_key_clear(1, idx);
}
