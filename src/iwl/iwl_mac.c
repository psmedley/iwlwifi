/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_mac.c - what a station join needs from the firmware (MAC context, binding, station entry, session
 * protection, rate table, smart FIFO, power, quotas), keys, and the data path (transmit commands, received
 * MPDUs, transmit responses).
 *
 * Port of OpenBSD if_iwm.c (ISC): iwm_auth, iwm_run, iwm_run_stop, iwm_deauth, iwm_mac_ctxt_cmd*, iwm_ack_rates,
 * iwm_binding_cmd, iwm_add_sta_cmd, iwm_rm_sta_cmd, iwm_protect_session, iwm_set_rate_table, iwm_setrates,
 * iwm_power_build_cmd, iwm_allow_mcast, iwm_tx, iwm_tx_fill_cmd, iwm_rx_mpdu, iwm_get_signal_strength,
 * iwm_rx_tx_cmd. Group keys are installed in the firmware as Linux mvm does (iwl_mvm_send_sta_key) because this
 * driver has no software CCMP; received frames are decrypted by the firmware (RX_MPDU status MIC_OK), and the
 * transmit side builds the CCMP header and hands the key in the transmit command, as iwm and Linux do.
 *
 * Phase 1 limits (see docs/ARCHITECTURE.md): 20 MHz only, no A-MPDU (block-ack requests are refused by the
 * station), no rate scaling beyond the firmware's retry chain through the rate table, no power save.
 */
#include "iwl.h"

USHORT g_tx_ok, g_tx_fail, g_rx_mpdu, g_rx_crc, g_rx_decfail;
USHORT g_tx_last_st, g_tx_last_fail, g_tx_st_hist[4];
ULONG  g_tx_last_rate;

static int two_streams(void);

static USHORT g_mac_on, g_bind_on, g_sta_on, g_te_on, g_assoc_on;
static ULONG  g_te_uid;
static UCHAR  g_ptk[16];
static ULONG  g_pn_lo;                          /* CCMP packet number (low 32 bits; the high 16 stay 0)    */
static ULONG  g_mgmt_rate;                      /* rate_n_flags of management frames (lowest basic rate)   */
static UCHAR  g_basic_cck, g_basic_ofdm;        /* basic-rate bitmaps: CCK 1,2,5.5,11; OFDM 6..54           */
static UCHAR  g_ap_rates_cck, g_ap_rates_ofdm;  /* every rate the AP supports                               */
static UCHAR  g_ap_ht_sgi20, g_ap_ht_sgi40;
static rx_phy_info_t g_phy;
static USHORT g_phy_ok;

/* legacy rates: 802.11 units (500 kb/s), firmware PLCP code. Index 0-3 CCK, 4-11 OFDM (iwm_rates order) */
static const UCHAR g_rate_val[12]  = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
static const UCHAR g_rate_plcp[12] = { 10, 20, 55, 110, 13, 15, 5, 7, 9, 11, 1, 3 };

static int rate_idx(UCHAR v)
{
   int i;

   v &= 0x7F;
   for (i = 0; i < 12; i++)
      if (g_rate_val[i] == v)
         return i;
   return -1;
}

static ULONG rate_code(int i)
{
   return (ULONG)g_rate_plcp[i] | RATE_MCS_ANT_A_MSK | (i < 4 ? RATE_MCS_CCK_MSK : 0UL);
}

void iwl_mac_reset(void)
{
   g_mac_on = g_bind_on = g_sta_on = g_te_on = g_assoc_on = 0;
}

/* The AP's rates from a block of elements (beacon or association response) */
static void scan_rates(UCHAR FAR *ie, USHORT len, UCHAR hi)
{
   USHORT pos = 0, i;
   int    k;

   g_basic_cck = g_basic_ofdm = g_ap_rates_cck = g_ap_rates_ofdm = 0;
   while ((USHORT)(pos + 2) <= len) {
      UCHAR id = ie[pos], l = ie[pos + 1];

      if ((USHORT)(pos + 2 + l) > len)
         break;
      if (id == 1 || id == 50)
         for (i = 0; i < l; i++) {
            k = rate_idx(ie[pos + 2 + i]);
            if (k < 0 || (hi && k < 4))
               continue;
            if (k < 4) {
               g_ap_rates_cck |= (UCHAR)(1 << k);
               if (ie[pos + 2 + i] & 0x80) g_basic_cck |= (UCHAR)(1 << k);
            } else {
               g_ap_rates_ofdm |= (UCHAR)(1 << (k - 4));
               if (ie[pos + 2 + i] & 0x80) g_basic_ofdm |= (UCHAR)(1 << (k - 4));
            }
         }
      if (id == 45 && l >= 26)
      {
         g_ap_ht_sgi20 = (UCHAR)((ie[pos + 2] & 0x20) != 0);
         g_ap_ht_sgi40 = (UCHAR)((ie[pos + 2] & 0x40) != 0);
      }
      pos = (USHORT)(pos + 2 + l);
   }
   /* the lowest basic rate sends management frames */
   if (!hi && g_basic_cck) {
      for (k = 0; k < 4 && !(g_basic_cck & (1 << k)); k++) ;
      g_mgmt_rate = rate_code(k);
   } else {
      for (k = 0; k < 8 && !(g_basic_ofdm & (1 << k)); k++) ;
      g_mgmt_rate = rate_code(k < 8 ? 4 + k : 4);
   }
}

/* iwm_ack_rates: the basic rates plus the mandatory rates below the lowest basic one */
static void ack_rates(UCHAR hi, ULONG *cck, ULONG *ofdm)
{
   int lc = -1, lo = -1, i;
   UCHAR c = 0, o = 0;

   if (!hi)
      for (i = 0; i < 4; i++)
         if (g_basic_cck & (1 << i)) { c |= (UCHAR)(1 << i); if (lc < 0) lc = i; }
   for (i = 0; i < 8; i++)
      if (g_basic_ofdm & (1 << i)) { o |= (UCHAR)(1 << i); if (lo < 0) lo = i + 4; }
   if (8 < lo) o |= 0x10;                          /* 24M (index 8)                                         */
   if (6 < lo) o |= 0x04;                          /* 12M (index 6)                                         */
   o |= 0x01;                                      /* 6M                                                    */
   if (3 < lc) c |= 0x08;
   if (2 < lc) c |= 0x04;
   if (1 < lc) c |= 0x02;
   c |= 0x01;
   *cck = c;
   *ofdm = o;
}

/* ---- MAC context ------------------------------------------------------------------------------------------ */
static int mac_ctxt(ULONG action, USHORT assoc)
{
   static mac_ctx_cmd_t c;
   static const UCHAR ecwmin[4] = { 4, 4, 3, 2 }, ecwmax[4] = { 10, 10, 4, 3 }, aifsn[4] = { 3, 7, 2, 2 };
   static const USHORT txop[4] = { 0, 0, 94, 47 };   /* 802.11 default EDCA (BE, BK, VI, VO), 32 us units */
   static const UCHAR fifo[4] = { TX_FIFO_BE, TX_FIFO_BK, TX_FIFO_VI, TX_FIFO_VO };
   net_t *n = g_jnet;
   UCHAR hi = (UCHAR)(g_jch > 14);
   USHORT i, cap;

   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.id_and_color = ID_AND_COLOR(0, 0);
   c.action = action;
   c.mac_type = FW_MAC_TYPE_BSS_STA;
   c.tsf_id = TSF_ID_A;
   fmemcpy((UCHAR FAR *)c.node_addr, (UCHAR FAR *)g_mymac, 6);
   fmemcpy((UCHAR FAR *)c.bssid_addr, (UCHAR FAR *)g_jbssid, 6);
   ack_rates(hi, &c.cck_rates, &c.ofdm_rates);
   cap = n ? get16((UCHAR FAR *)n->ie + 10) : 0;
   c.cck_short_preamble = (cap & 0x0020) ? MAC_FLG_SHORT_PREAMBLE : 0UL;
   c.short_slot = (hi || (cap & 0x0400)) ? MAC_FLG_SHORT_SLOT : 0UL;
   for (i = 0; i < 4; i++) {
      ac_qos_t *a = &c.ac[fifo[i]];

      a->cw_min = (USHORT)((1 << ecwmin[i]) - 1);
      a->cw_max = (USHORT)((1 << ecwmax[i]) - 1);
      a->aifsn = aifsn[i];
      a->fifos_mask = (UCHAR)(1 << fifo[i]);
      a->edca_txop = (USHORT)(txop[i] << 5);
   }
   if (n && n->wmm)
      c.qos_flags |= MAC_QOS_FLG_UPDATE_EDCA;
   if (g_jht)
      c.qos_flags |= MAC_QOS_FLG_TGN;
   c.filter_flags = MAC_FILTER_ACCEPT_GRP;
   if (!assoc || !g_jaid || !n) {
      c.filter_flags |= MAC_FILTER_IN_BEACON;      /* beacons pass while not associated                    */
   } else {
      USHORT pos, dcount = 0;
      ULONG  off, di;

      for (pos = 12; (USHORT)(pos + 2) <= n->ielen; pos = (USHORT)(pos + 2 + n->ie[pos + 1])) {
         if ((USHORT)(pos + 2 + n->ie[pos + 1]) > n->ielen)
            break;
         if (n->ie[pos] == 5 && n->ie[pos + 1] >= 2)
            dcount = n->ie[pos + 2];               /* TIM: DTIM count of that beacon                        */
      }
      off = (ULONG)rt_imul16((int)dcount, (int)g_jbcn) << 10;   /* TUs -> microseconds (x 1024)             */
      di = (ULONG)rt_imul16((int)g_jbcn, (int)g_jdtim);
      c.u.sta.is_assoc = 1;
      c.u.sta.dtim_time = n->rx_time + off;
      c.u.sta.dtim_tsf_lo = n->tsf_lo + off;
      c.u.sta.dtim_tsf_hi = n->tsf_hi + ((c.u.sta.dtim_tsf_lo < off) ? 1UL : 0UL);
      c.u.sta.bi = g_jbcn;
      c.u.sta.bi_reciprocal = u32_div(0xFFFFFFFFUL, (ULONG)g_jbcn);
      c.u.sta.dtim_interval = di;
      c.u.sta.dtim_reciprocal = u32_div(0xFFFFFFFFUL, di);
      c.u.sta.listen_interval = 10;
      c.u.sta.assoc_id = g_jaid;
      c.u.sta.assoc_beacon_arrive_time = n->rx_time;
   }
   return iwl_cmd1(CMD_MAC_CONTEXT, (UCHAR FAR *)&c, sizeof(c));
}

static int binding(ULONG action)
{
   static binding_cmd_t c;
   ULONG st = 0;
   int   rc;

   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.id_and_color = ID_AND_COLOR(0, 0);
   c.action = action;
   c.macs[0] = ID_AND_COLOR(0, 0);
   c.macs[1] = c.macs[2] = FW_CTXT_INVALID;
   c.phy = ID_AND_COLOR(0, 0);
   c.lmac_id = (g_jch > 14 && FW_CAPA(CAPA_CDB_SUPPORT)) ? 1UL : 0UL;
   rc = iwl_cmd_status(CMD_BINDING_CONTEXT, (UCHAR FAR *)&c,
                       (USHORT)(FW_CAPA(CAPA_BINDING_CDB_SUPPORT) ? sizeof(c) : 24), &st);
   return (rc == 0 && st != 0UL) ? -10 : rc;
}

static int add_sta(USHORT update)
{
   static add_sta_cmd_t c;
   ULONG st = ADD_STA_SUCCESS, m = 0;
   USHORT q0 = (USHORT)(FW_CAPA(CAPA_DQA_SUPPORT) ? DQA_MIN_MGMT_QUEUE : 0), i;
   int   rc;

   for (i = 0; i < 4; i++)
      m |= (ULONG)(1U << (q0 + i));               /* the four access-category queues (q0 + i < 16)          */
   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.sta_id = STATION_ID;
   if (FW_API(API_STA_TYPE))
      c.station_type = STA_LINK;
   c.mac_id_n_color = ID_AND_COLOR(0, 0);
   c.tfd_queue_msk = m;
   if (!update)
      fmemcpy((UCHAR FAR *)c.addr, (UCHAR FAR *)g_jbssid, 6);
   c.add_modify = (UCHAR)(update ? 1 : 0);
   c.station_flags_msk = STA_FLG_FAT_EN_MSK | STA_FLG_MIMO_EN_MSK;
   if (update)
      c.modify_mask = STA_MODIFY_QUEUES | STA_MODIFY_TID_DISABLE_TX;
   c.tid_disable_tx = 0xFFFF;                      /* no transmit aggregation                                 */
   if (update && g_jht) {
      c.station_flags_msk |= STA_FLG_MAX_AGG_SIZE_MSK | STA_FLG_AGG_MPDU_DENS_MSK;
      if (two_streams())
         c.station_flags |= STA_FLG_MIMO_EN_MIMO2;
      if (g_j80)
         c.station_flags |= STA_FLG_FAT_EN_80MHZ;
      else if (g_j40)
         c.station_flags |= STA_FLG_FAT_EN_40MHZ;
      c.station_flags |= (ULONG)(g_j80 ? g_jvexp : (g_ampdu_par & 3)) << STA_FLG_MAX_AGG_SIZE_SHIFT;
      c.station_flags |= (ULONG)((g_ampdu_par >> 2) & 7) << STA_FLG_AGG_MPDU_DENS_SHIFT;
   }
   rc = iwl_cmd_status(CMD_ADD_STA, (UCHAR FAR *)&c, (USHORT)(FW_API(API_STA_TYPE) ? sizeof(c) : ADD_STA_CMD_V7_SIZE), &st);
   if (rc == 0 && (st & 0xFFUL) != ADD_STA_SUCCESS)
      rc = -11;
   return rc;
}

static int rm_sta(void)
{
   static rm_sta_cmd_t c;

   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.sta_id = STATION_ID;
   return iwl_cmd1(CMD_REMOVE_STA, (UCHAR FAR *)&c, sizeof(c));
}

/* ---- session protection (stay on the AP's channel during authentication and association) ------------------ */
static int time_event(USHORT add, USHORT duration, USHORT max_delay)
{
   static time_event_cmd_t c;
   int rc;

   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.id_and_color = ID_AND_COLOR(0, 0);
   if (add) {
      c.action = FW_CTXT_ACTION_ADD;
      c.id = TE_BSS_STA_AGGRESSIVE_ASSOC;
      c.max_frags = TE_V2_FRAG_NONE;
      c.max_delay = max_delay;
      c.interval = 1;
      c.duration = duration;
      c.repeat = 1;
      c.policy = TE_V2_NOTIF_HOST_EVENT_START | TE_V2_NOTIF_HOST_EVENT_END | T2_V2_START_IMMEDIATELY;
   } else {
      c.action = FW_CTXT_ACTION_REMOVE;
      c.id = g_te_uid;
   }
   rc = iwl_cmd(CMD_TIME_EVENT, (UCHAR FAR *)&c, sizeof(c), (UCHAR FAR *)0, 0, CMDF_WANT_RESP);
   if (rc)
      return rc;
   if (g_resp_len < sizeof(time_event_resp_t) || get32(g_resp) != 0UL)
      return -12;
   if (add)
      g_te_uid = get32(g_resp + 8);                /* unique_id                                             */
   return 0;
}

void iwl_te_notif(UCHAR FAR *p, USHORT len)
{
   if (len < sizeof(time_event_notif_t))
      return;
   if (get32(p + 8) == g_te_uid && (get32(p + 16) & TE_V2_NOTIF_HOST_EVENT_END))
      g_te_on = 0;
}

/* ---- rate table: the firmware walks it on retries (iwm_set_rate_table, without iwm's rate adaptation) ----- */
/* The AP receives two streams, the radio has two chains and PROTOCOL.INI (STREAMS) allows it */
static int vht_max_mcs(USHORT nss);

static int two_streams(void)
{
   if (g_j80)
      return g_hw_nss >= 2 && g_maxnss >= 2 && vht_max_mcs(2) >= 0;
   return g_jht && g_hw_nss >= 2 && g_maxnss >= 2 && g_jht_mcs[1] != 0;
}

/* The first two attempts of an HT rate use the 40 MHz channel (with RTS protection) and the short guard interval
 * the AP allows; later attempts fall back to 20 MHz, as iwm does. */
static ULONG ht_wide(USHORT j)
{
   ULONG f = 0;

   if (j >= 2)
      return 0UL;
   if (g_j40) {
      f |= RATE_MCS_CHAN_WIDTH_40 | RATE_MCS_RTS_REQUIRED_MSK;
      if (g_ap_ht_sgi40)
         f |= RATE_MCS_SGI_MSK;
   } else if (g_ap_ht_sgi20)
      f |= RATE_MCS_SGI_MSK;
   return f;
}

/* VHT: the highest MCS the AP receives with n streams (0-7, 0-8 or 0-9), -1 when none (VHT MCS map, 2 bits per stream). */
static int vht_max_mcs(USHORT nss)
{
   UCHAR v = (UCHAR)((g_jvrx[0] >> ((nss - 1) * 2)) & 3);

   return v == 3 ? -1 : 7 + v;
}

/* As ht_wide: the first two attempts use 80 MHz (with RTS protection) and the short guard interval. */
static ULONG vht_wide(USHORT j)
{
   ULONG f = 0;

   if (j >= 2)
      return 0UL;
   f = RATE_MCS_CHAN_WIDTH_80 | RATE_MCS_RTS_REQUIRED_MSK;
   if (g_jvsgi80)
      f |= RATE_MCS_SGI_MSK;
   return f;
}

static int send_lq(void)
{
   static lq_cmd_t c;
   USHORT j = 0, m;
   int    i, lowest = -1;
   UCHAR  hi = (UCHAR)(g_jch > 14);

   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.sta_id = STATION_ID;
   c.single_stream_ant_msk = ANT_A;
   c.dual_stream_ant_msk = ANT_AB;
   c.agg_time_limit = 4000;
   c.agg_disable_start_th = 3;
   c.agg_frame_cnt_limit = 0x3F;
   if (g_jht && g_j80) {                           /* VHT: up to 8 two-stream rates, then single-stream ones     */
      int top, k;

      top = (g_hw_nss >= 2 && g_maxnss >= 2) ? vht_max_mcs(2) : -1;
      for (k = 0; top >= 0 && k < 8 && top - k >= 0; k++)
      {
         ULONG f = (ULONG)(top - k) | ((ULONG)1 << RATE_VHT_MCS_NSS_POS) | RATE_MCS_VHT_MSK | RATE_MCS_ANT_AB_MSK | vht_wide(j);

         c.rs_table[j++] = f;
      }
      c.mimo_delim = (UCHAR)j;
      top = vht_max_mcs(1);
      for (k = 0; top >= 0 && k < 8 && top - k >= 0 && j < 16; k++)
      {
         ULONG f = (ULONG)(top - k) | RATE_MCS_VHT_MSK | RATE_MCS_ANT_A_MSK | vht_wide(j);

         c.rs_table[j++] = f;
      }
   } else if (g_jht) {
      if (two_streams()) {                         /* two streams first: MCS 15 .. 8, then the single-stream ones */
         for (m = 8; m-- > 0 && j < 16; )
            if (g_jht_mcs[1] & (1 << m)) {
               ULONG f = (ULONG)(8 + m) | RATE_MCS_HT_MSK | RATE_MCS_ANT_AB_MSK | ht_wide(j);

               c.rs_table[j++] = f;
            }
         c.mimo_delim = (UCHAR)j;                  /* the first single-stream index                           */
      }
      for (m = 8; m-- > 0 && j < 16; )
         if (g_jht_mcs[0] & (1 << m)) {
            ULONG f = (ULONG)m | RATE_MCS_HT_MSK | RATE_MCS_ANT_A_MSK | ht_wide(j);

            c.rs_table[j++] = f;
         }
   } else {                                        /* the AP's legacy rates, fastest first                     */
      for (i = 11; i >= 0 && j < 16; i--) {
         if (i < 4 && (hi || !(g_ap_rates_cck & (1 << i))))
            continue;
         if (i >= 4 && !(g_ap_rates_ofdm & (1 << (i - 4))))
            continue;
         c.rs_table[j++] = rate_code(i);
      }
   }
   for (i = 0; i < 12; i++)                        /* the rest: the lowest basic rate                           */
      if ((i < 4 && !hi && (g_basic_cck & (1 << i))) || (i >= 4 && (g_basic_ofdm & (1 << (i - 4))))) {
         lowest = i;
         break;
      }
   while (j < 16)
      c.rs_table[j++] = lowest >= 0 ? rate_code(lowest) : g_mgmt_rate;
   return iwl_cmd1(CMD_LQ, (UCHAR FAR *)&c, sizeof(c));
}

static int send_mac_power(void)
{
   static mac_power_cmd_t c;
   ULONG ka = (ULONG)rt_imul16((int)g_jdtim, (int)g_jbcn);   /* DTIM period in TUs (~ ms)                     */
   USHORT secs = 25;                               /* IWM_POWER_KEEP_ALIVE_PERIOD_SEC                          */

   if (ka > 8333UL)                                /* at least 3 DTIM periods                                  */
      secs = (USHORT)(u32_div(ka + ka + ka + 999UL, 1000UL));
   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.id_and_color = ID_AND_COLOR(0, 0);
   c.keep_alive_seconds = secs;
   c.flags = 0;                                    /* no power save (constantly awake)                         */
   return iwl_cmd1(CMD_MAC_PM_POWER_TABLE, (UCHAR FAR *)&c, sizeof(c));
}

static int allow_mcast(void)
{
   static mcast_filter_cmd_t c;

   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.filter_own = 1;
   c.pass_all = 1;
   fmemcpy((UCHAR FAR *)c.bssid, (UCHAR FAR *)g_jbssid, 6);
   return iwl_cmd1(CMD_MCAST_FILTER, (UCHAR FAR *)&c, sizeof(c));
}

/* ---- join ------------------------------------------------------------------------------------------------- */
int hw_join_prepare(net_t *n)
{
   UCHAR hi = (UCHAR)(n->ch > 14);

   if (!g_hw_ready)
      return -1;
   scan_rates((UCHAR FAR *)n->ie + 12, (USHORT)(n->ielen > 12 ? n->ielen - 12 : 0), hi);
   if (iwl_send_phy_ctxt((USHORT)FW_CTXT_ACTION_MODIFY, n->ch, 1, 1, 0, 0))
      return -2;
   if (mac_ctxt(FW_CTXT_ACTION_ADD, 0))
      return -3;
   g_mac_on = 1;
   if (binding(FW_CTXT_ACTION_ADD))
      return -4;
   g_bind_on = 1;
   if (add_sta(0))
      return -5;
   g_sta_on = 1;
   g_pn_lo = 0;
   /* stay on the channel for association: up to about one second (Linux uses up to 1000 TU here) */
   if (time_event(1, 1000, (USHORT)(g_jbcn / 2)) == 0)
      g_te_on = 1;
   return 0;
}

/* The channel the link runs on: width and the position of the primary channel in it (Linux iwl_mvm_get_ctrl_pos). */
static UCHAR join_width(void)
{
   return (UCHAR)(g_j80 ? PHY_VHT_CHANNEL_MODE80 : (g_j40 ? PHY_VHT_CHANNEL_MODE40 : PHY_VHT_CHANNEL_MODE20));
}

static UCHAR join_ctrl(void)
{
   if (g_j80) {
      int d = (int)g_jch - (int)g_jcf0;              /* channel numbers are 5 MHz apart: -6 = 30 MHz below the centre */

      return (UCHAR)(d <= -6 ? PHY_VHT_CTRL_POS_2_BELOW : (d < 0 ? PHY_VHT_CTRL_POS_1_BELOW :
                     (d < 6 ? PHY_VHT_CTRL_POS_1_ABOVE : PHY_VHT_CTRL_POS_2_ABOVE)));
   }
   if (g_j40)
      return (UCHAR)(g_jsco == 3 ? PHY_VHT_CTRL_POS_1_ABOVE : PHY_VHT_CTRL_POS_1_BELOW);
   return PHY_VHT_CTRL_POS_1_BELOW;
}

int hw_join_complete(void)
{
   UCHAR ch = (UCHAR)(two_streams() ? 2 : 1);

   scan_rates((UCHAR FAR *)g_jres, g_jreslen, (UCHAR)(g_jch > 14));
   g_jnss = (UCHAR)(two_streams() ? 2 : 1);
   if (g_jht && iwl_send_phy_ctxt((USHORT)FW_CTXT_ACTION_MODIFY, g_jch, ch, ch, join_width(), join_ctrl()))
      return -1;
   if (add_sta(1))
      return -2;
   if (mac_ctxt(FW_CTXT_ACTION_MODIFY, 1))
      return -3;
   g_assoc_on = 1;
   if (iwl_send_sf(SF_FULL_ON, 1) || allow_mcast() || iwl_send_power() || send_mac_power() || iwl_send_quota(1))
      return -4;
   if (send_lq())
      return -5;
   if (g_te_on && time_event(0, 0, 0) == 0)        /* associated: the session protection can go               */
      g_te_on = 0;
   return 0;
}

void hw_leave(void)
{
   txa_reset();
   if (g_assoc_on) {
      iwl_send_sf(SF_INIT_OFF, 0);
      iwl_send_quota(0);
      mac_ctxt(FW_CTXT_ACTION_MODIFY, 0);
      g_assoc_on = 0;
   }
   if (g_te_on) {
      time_event(0, 0, 0);
      g_te_on = 0;
   }
   if (g_sta_on) {
      rm_sta();
      g_sta_on = 0;
   }
   if (g_bind_on) {
      binding(FW_CTXT_ACTION_REMOVE);
      g_bind_on = 0;
   }
   if (g_mac_on) {
      mac_ctxt(FW_CTXT_ACTION_REMOVE, 0);
      g_mac_on = 0;
   }
   iwl_send_phy_ctxt((USHORT)FW_CTXT_ACTION_MODIFY, g_nvm_nchan ? g_nvm_chan[0] : 1, 1, 1, 0, 0);
}

/* ---- receive aggregation -------------------------------------------------------------------------------------
 * iwm_sta_rx_agg / Linux iwl_mvm_sta_rx_agg for firmware without multi-queue receive: a modifying ADD_STA that adds
 * (or removes) an "immediate BA" for a TID. The firmware then answers the AP's aggregates with block acks; the
 * reordering of the frames is the host's job (wlan/sta.c). */
static int ba_cmd(UCHAR tid, USHORT ssn, USHORT win, USHORT start)
{
   static add_sta_cmd_t c;                         /* static: the receive path calls this, one at a time    */

   if (!g_sta_on || !g_assoc_on)
      return -1;
   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.sta_id = STATION_ID;
   c.mac_id_n_color = ID_AND_COLOR(0, 0);
   c.add_modify = 1;
   if (start) {
      c.add_immediate_ba_tid = tid;
      c.add_immediate_ba_ssn = ssn;
      c.rx_ba_window = win;
      c.modify_mask = STA_MODIFY_ADD_BA_TID;
   } else {
      c.remove_immediate_ba_tid = tid;
      c.modify_mask = STA_MODIFY_REMOVE_BA_TID;
   }
   return iwl_cmd_async(CMD_ADD_STA, (UCHAR FAR *)&c, (USHORT)(FW_API(API_STA_TYPE) ? sizeof(c) : ADD_STA_CMD_V7_SIZE));
}

int hw_ba_start(UCHAR tid, USHORT ssn, USHORT win)
{
   return ba_cmd(tid, ssn, win, 1);
}

int hw_ba_stop(UCHAR tid)
{
   return ba_cmd(tid, 0, 0, 0);
}

/* ---- keys -------------------------------------------------------------------------------------------------- */
static int sta_key(UCHAR FAR *key, USHORT pairwise, UCHAR idx, USHORT remove)
{
   static add_sta_key_cmd_t c;
   ULONG st = ADD_STA_SUCCESS;
   int   rc;

   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.sta_id = STATION_ID;
   c.key_offset = (UCHAR)(pairwise ? 0 : 1 + (idx & 3));
   c.key_flags = (USHORT)(STA_KEY_FLG_WEP_KEY_MAP | ((USHORT)(pairwise ? 0 : (idx & 3)) << STA_KEY_FLG_KEYID_POS));
   if (remove)
      c.key_flags |= STA_KEY_NOT_VALID | STA_KEY_FLG_NO_ENC;
   else {
      c.key_flags |= STA_KEY_FLG_CCM;
      fmemcpy((UCHAR FAR *)c.key, key, 16);
   }
   if (!pairwise)
      c.key_flags |= STA_KEY_MULTICAST;
   rc = iwl_cmd_status(CMD_ADD_STA_KEY, (UCHAR FAR *)&c, (USHORT)(FW_API(API_TKIP_MIC_KEYS) ? 76 : 64), &st);
   if (rc == 0 && (st & 0xFFUL) != ADD_STA_SUCCESS)
      rc = -13;
   return rc;
}

int hw_key_set(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx)
{
   if (klen != 16 || !g_sta_on)
      return -1;
   if (sta_key(key, pairwise, idx, 0))
      return -2;
   if (pairwise) {
      fmemcpy((UCHAR FAR *)g_ptk, key, 16);
      g_pn_lo = 0;
   }
   return 0;
}

int hw_key_clear(USHORT pairwise, UCHAR idx)
{
   if (!g_sta_on)
      return 0;
   return sta_key((UCHAR FAR *)g_ptk, pairwise, idx, 1);
}

/* ---- transmit ---------------------------------------------------------------------------------------------- */
/* hdr: the 802.11 header (24 bytes), body: what follows it. Layout of a slot: cmd header (4), tx command (56),
 * the 802.11 header; the payload (CCMP header + body) at offset 256. */

static int tx_frame_locked(UCHAR FAR *hdr, USHORT hlen, UCHAR FAR *body, USHORT blen, USHORT protect, USHORT mgmt);

static int tx_frame(UCHAR FAR *hdr, USHORT hlen, UCHAR FAR *body, USHORT blen, USHORT protect, USHORT mgmt)
{
   USHORT fl = rt_irq_save();
   int    rc;

   if (rt_lock_acquire(&g_hwtxlk, (USHORT)(rt_cpu_id() + 1))) {
      rt_irq_restore(fl);
      return -2;
   }
   rc = tx_frame_locked(hdr, hlen, body, blen, protect, mgmt);
   rt_lock_release(&g_hwtxlk);
   rt_irq_restore(fl);
   return rc;
}

static int tx_frame_locked(UCHAR FAR *hdr, USHORT hlen, UCHAR FAR *body, USHORT blen, USHORT protect, USHORT mgmt)
{
   static USHORT idx, qi;
   static ULONG  ph;
   UCHAR FAR *s;
   tx_cmd_t FAR *t;
   USHORT pad, cmdlen, plen = 0, len, qos = 0, sn = 0;
   ULONG  fl;
   UCHAR  sub = (UCHAR)(hdr[0] & 0xF0);
   int    rc;

   if ((USHORT)(blen + 8) > TX_SLOT_SZ - 256)
      return -3;
   qi = QI_BE;
   if (!mgmt && hlen == 26 && (hdr[0] & 0x80)) {   /* QoS data (TID 0): a sequence number of our own          */
      qos = 1;
      rc = txa_pick(&qi);
      if (rc)
         return rc;
   }
   s = iwl_txq_slot(qi, &idx, &ph);
   if (!s)
      return -2;
   if (qos) {
      sn = txa_alloc_sn();
      hdr[22] = (UCHAR)((hdr[22] & 0x0F) | (UCHAR)(sn << 4));
      hdr[23] = (UCHAR)(sn >> 4);
   }
   pad = (USHORT)((4 - (hlen & 3)) & 3);
   cmdlen = (USHORT)(4 + sizeof(tx_cmd_t) + hlen + pad);
   fmemset(s, 0, cmdlen);
   s[0] = CMD_TX;
   s[2] = (UCHAR)idx;
   s[3] = (UCHAR)iwl_txq_qid(qi);
   t = (tx_cmd_t FAR *)(s + 4);
   fmemcpy(s + 4 + sizeof(tx_cmd_t), hdr, hlen);
   if (protect) {                                  /* CCMP header: PN 0..1, rsvd, key id 0 | ExtIV, PN 2..5  */
      UCHAR FAR *iv = s + 256;
      U32 pn;

      g_pn_lo++;
      pn.l = g_pn_lo;
      iv[0] = pn.b[0]; iv[1] = pn.b[1]; iv[2] = 0; iv[3] = 0x20;
      iv[4] = pn.b[2]; iv[5] = pn.b[3]; iv[6] = 0; iv[7] = 0;
      plen = 8;
      t->sec_ctl = TX_CMD_SEC_CCM;
      fmemcpy((UCHAR FAR *)t->key, (UCHAR FAR *)g_ptk, 16);
   }
   fmemcpy(s + 256 + plen, body, blen);
   plen = (USHORT)(plen + blen);
   len = (USHORT)(hlen + plen);
   t->len = len;
   fl = TX_CMD_FLG_ACK | TX_CMD_FLG_BT_DIS;
   if (!qos)
      fl |= TX_CMD_FLG_SEQ_CTL;                    /* the firmware numbers the frame                         */
   if (pad) {
      fl |= TX_CMD_FLG_MH_PAD;
      t->offload_assist = TX_CMD_OFFLD_PAD;
   }
   t->rts_retry_limit = RTS_DFAULT_RETRY_LIMIT;
   t->sta_id = STATION_ID;
   t->life_time = TX_CMD_LIFE_TIME_INFINITE;
   t->dram_lsb_ptr = ph + 4 + 8;                   /* the scratch area inside this tx command               */
   if (mgmt) {
      t->rate_n_flags = g_mgmt_rate;
      t->data_retry_limit = MGMT_DFAULT_RETRY_LIMIT;
      t->tid_tspec = MAX_TID_COUNT;
      t->pm_frame_timeout = (USHORT)((sub == 0x00 || sub == 0x20) ? 3 : 2);   /* (re)association request: 3 */
   } else {
      fl |= TX_CMD_FLG_STA_RATE;                   /* the rate table (LQ_CMD)                                */
      t->data_retry_limit = LOW_RETRY_LIMIT;
      t->tid_tspec = qos ? 0 : TID_NON_QOS;
   }
   t->tx_flags = fl;
   return iwl_txq_kick(qi, idx, cmdlen, plen, (USHORT)(len + (protect ? 8 : 0)), txa_bell_ok(qi));
}

int hw_mgmt_tx(UCHAR FAR *f, USHORT len)
{
   if (len < 24 || !g_hw_ready)
      return -1;
   return tx_frame(f, 24, f + 24, (USHORT)(len - 24), 0, 1);
}

int hw_data_tx(UCHAR FAR *hdr, USHORT hlen, UCHAR FAR *body, USHORT blen, USHORT flags)
{
   if (!g_sta_on || !g_assoc_on)
      return -1;
   return tx_frame(hdr, hlen, body, blen, (USHORT)((flags & HWTX_PROTECT) ? 1 : 0), 0);
}

void iwl_tx_done(UCHAR qid, UCHAR idx, UCHAR FAR *p, USHORT len)
{
   USHORT st;

   if (len < sizeof(tx_resp_t))
      return;
   if (p[0] > 1 && iwl_txq_present(QI_AGG) && qid == iwl_txq_qid(QI_AGG)) {
      txa_tx_resp(qid, p, len);                    /* an A-MPDU: the status field is not a frame's result    */
      return;
   }
   st = (USHORT)(get16(p + 36) & TX_STATUS_MSK);
   g_tx_last_st = get16(p + 36);                   /* full status word                                       */
   g_tx_last_fail = p[3];                          /* failure_frame: retries that failed                     */
   g_tx_last_rate = get32(p + 4);                  /* initial_rate                                           */
   g_tx_st_hist[(st == TX_STATUS_SUCCESS) ? 0 : (st == TX_STATUS_DIRECT_DONE) ? 1 : (st < 0x80) ? 2 : 3]++;
   if (st == TX_STATUS_SUCCESS || st == TX_STATUS_DIRECT_DONE)
      g_tx_ok++;
   else
      g_tx_fail++;
   if (iwl_txq_present(QI_AGG) && qid == iwl_txq_qid(QI_AGG)) {
      txa_tx_resp(qid, p, len);
      return;
   }
   {
      USHORT fl = rt_irq_save();

      if (!rt_lock_acquire(&g_hwtxlk, (USHORT)(rt_cpu_id() + 1))) {
         iwl_tx_reclaim(qid, idx);
         rt_lock_release(&g_hwtxlk);
      }
      rt_irq_restore(fl);
   }
}

/* ---- receive ------------------------------------------------------------------------------------------------ */
void iwl_rx_phy(UCHAR FAR *p, USHORT len)
{
   if (len < sizeof(rx_phy_info_t))
      return;
   fmemcpy((UCHAR FAR *)&g_phy, p, sizeof(g_phy));
   g_phy_ok = 1;
}

/* REPLY_RX_MPDU: { USHORT byte_count; USHORT reserved; frame[byte_count]; ULONG status } */
void iwl_rx_mpdu(UCHAR FAR *p, USHORT len)
{
   USHORT bc, fl = 0;
   ULONG  st, e;
   int    a, b, c, r;
   UCHAR FAR *f;

   if (len < 8)
      return;
   bc = get16(p);
   if (bc < 24 || (USHORT)(bc + 8) > len)
      return;
   g_rx_mpdu++;
   f = p + 4;
   st = get32(f + bc);
   if (!(st & RX_MPDU_RES_STATUS_CRC_OK) || !(st & RX_MPDU_RES_STATUS_OVERRUN_OK)) {
      g_rx_crc++;
      return;
   }
   if ((f[1] & 0x40) && (st & RX_MPDU_RES_STATUS_SEC_ENC_MSK) == RX_MPDU_RES_STATUS_SEC_CCM_ENC) {
      if (!(st & RX_MPDU_RES_STATUS_MIC_OK)) {
         g_rx_decfail++;
         return;
      }
      fl |= RXF_DECRYPTED;
   }
   if (!g_phy_ok || g_phy.cfg_phy_cnt > 20)
      return;
   e = g_phy.non_cfg_phy[1];                       /* energy per antenna (A, B, C), positive dBm below 0     */
   a = (int)(e & 0xFFUL);
   b = (int)((e >> 8) & 0xFFUL);
   c = (int)((e >> 16) & 0xFFUL);
   a = a ? -a : -256;
   b = b ? -b : -256;
   c = c ? -c : -256;
   r = a > b ? a : b;
   r = r > c ? r : c;
   sta_rx_frame(f, bc, (UCHAR)g_phy.channel, (short)r, fl, g_phy.system_timestamp);
}
