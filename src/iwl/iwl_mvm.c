/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_mvm.c - the firmware conversation of the iwlwifi "mvm" operation mode (7000/8000/9000 firmware):
 * bring-up (INIT image for NVM and calibration, then the runtime image), firmware paging, the global commands
 * (antennas, PHY configuration, Bluetooth coexistence, DQA, the auxiliary station, PHY context, power, regulatory,
 * scan configuration), the dispatch of everything the firmware sends, and the status lines.
 *
 * Port of OpenBSD if_iwm.c (ISC): iwm_preinit/iwm_run_init_mvm_ucode/iwm_init_hw/iwm_load_ucode_wait_alive/
 * iwm_save_fw_paging/iwm_send_paging_cmd/iwm_rx_pkt, cross-checked with Linux iwlwifi mvm/fw.c and mvm/ops.c.
 */
#include "iwl.h"

USHORT g_hw_ready, g_hw_failed, g_hw_state;
UCHAR  g_hw_mac[6], g_hw_nss = 1;
USHORT g_init_complete, g_calib_complete;
static USHORT g_unhandled, g_last_unhandled, g_fw_errors_seen;
static USHORT g_unh_qid, g_unh_idx, g_unh_len, g_unh_by[256];
static UCHAR  g_unh_d[8];
static ULONG  g_last_fw_err, g_last_fw_err_cmd;

/* ---- antennas --------------------------------------------------------------------------------------- */
UCHAR iwl_valid_tx_ant(void)
{
   UCHAR a = (UCHAR)((g_fw.phy_config >> FW_PHY_CFG_TX_CHAIN_POS) & 0xF);

   if (g_nvm_tx_ant)
      a &= g_nvm_tx_ant;
   return a ? a : (UCHAR)ANT_A;
}

UCHAR iwl_valid_rx_ant(void)
{
   UCHAR a = (UCHAR)((g_fw.phy_config >> FW_PHY_CFG_RX_CHAIN_POS) & 0xF);

   if (g_nvm_rx_ant)
      a &= g_nvm_rx_ant;
   return a ? a : (UCHAR)ANT_A;
}

static int send_tx_ant(void)
{
   static UCHAR b[4];

   put32((UCHAR FAR *)b, (ULONG)iwl_valid_tx_ant());
   return iwl_cmd1(CMD_TX_ANT_CONFIGURATION, (UCHAR FAR *)b, 4);
}

static int send_phy_cfg(USHORT type)
{
   static phy_cfg_cmd_t c;

   c.phy_cfg = g_fw.phy_config;
   c.flow_trigger = g_fw.img[type].flow_trigger;
   c.event_trigger = g_fw.img[type].event_trigger;
   return iwl_cmd1(CMD_PHY_CONFIGURATION, (UCHAR FAR *)&c, sizeof(c));
}

/* ---- smart FIFO ------------------------------------------------------------------------------------- */
int iwl_send_sf(ULONG state, USHORT assoc)
{
   static sf_cfg_cmd_t c;
   static const ULONG def[5][2]  = { { 400, 160 }, { 400, 160 }, { 400, 160 }, { 400, 160 }, { 400, 160 } };
   static const ULONG full[5][2] = { { 2016, 320 }, { 2016, 320 }, { 10016, 2016 }, { 2016, 320 }, { 2016, 320 } };
   USHORT i;

   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.state = state;
   c.watermark[0] = 4096;                          /* SF_LONG_DELAY_ON: IWM_SF_W_MARK_SCAN                  */
   c.watermark[1] = assoc ? (g_jht && g_jht_mcs[1] && g_hw_nss >= 2 ? 8192UL : 4096UL) : 8192UL;   /* FULL_ON */
   for (i = 0; i < 5; i++) {
      c.long_delay_timeouts[i][0] = 1000000UL;      /* IWM_SF_LONG_DELAY_AGING_TIMER                          */
      c.long_delay_timeouts[i][1] = 1000000UL;
      c.full_on_timeouts[i][0] = assoc ? full[i][0] : def[i][0];
      c.full_on_timeouts[i][1] = assoc ? full[i][1] : def[i][1];
   }
   return iwl_cmd1(CMD_REPLY_SF_CFG, (UCHAR FAR *)&c, sizeof(c));
}

/* ---- PHY context (one: id 0, color 0) ----------------------------------------------------------------- */
int iwl_send_phy_ctxt(USHORT action, UCHAR ch, UCHAR cs, UCHAR cd, UCHAR width, UCHAR ctrl)
{
   static phy_ctx_cmd_uhb_t u;
   static phy_ctx_cmd_t c;
   ULONG rx = ((ULONG)iwl_valid_rx_ant() << PHY_RX_CHAIN_VALID_POS) | ((ULONG)cs << PHY_RX_CHAIN_CNT_POS) |
              ((ULONG)cd << PHY_RX_CHAIN_MIMO_CNT_POS);
   UCHAR band = (UCHAR)(ch > 14 ? PHY_BAND_5 : PHY_BAND_24);

   if (FW_CAPA(CAPA_ULTRA_HB_CHANNELS)) {         /* the wider channel-info layout                          */
      fmemset((UCHAR FAR *)&u, 0, sizeof(u));
      u.id_and_color = ID_AND_COLOR(0, 0);
      u.action = (ULONG)action;
      u.ci.channel = ch;
      u.ci.band = band;
      u.ci.width = width;
      u.ci.ctrl_pos = ctrl;
      u.txchain_info = iwl_valid_tx_ant();
      u.rxchain_info = rx;
      return iwl_cmd1(CMD_PHY_CONTEXT, (UCHAR FAR *)&u, sizeof(u));
   }
   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.id_and_color = ID_AND_COLOR(0, 0);
   c.action = (ULONG)action;
   c.ci.band = band;
   c.ci.channel = ch;
   c.ci.width = width;
   c.ci.ctrl_pos = ctrl;
   c.txchain_info = iwl_valid_tx_ant();
   c.rxchain_info = rx;
   return iwl_cmd1(CMD_PHY_CONTEXT, (UCHAR FAR *)&c, sizeof(c));
}

/* ---- power: constantly awake (CAM); no power save at all in this version ------------------------------ */
int iwl_send_power(void)
{
   static device_power_cmd_t d;

   d.flags = g_powersave ? DEVICE_POWER_FLAGS_POWER_SAVE_ENA_MSK : 0;   /* 0: continuously aware mode (CAM)    */
   d.reserved = 0;
   return iwl_cmd1(CMD_POWER_TABLE, (UCHAR FAR *)&d, sizeof(d));
}

/* ---- time quotas: the whole schedule for our binding (firmware without dynamic quota) ----------------- */
int iwl_send_quota(USHORT running)
{
   static quota_t q[MAX_BINDINGS];
   USHORT i, v1 = (USHORT)!FW_API(API_QUOTA_LOW_LATENCY);

   if (FW_CAPA(CAPA_DYNAMIC_QUOTA))
      return 0;
   fmemset((UCHAR FAR *)q, 0, sizeof(q));
   for (i = 0; i < MAX_BINDINGS; i++)
      q[i].id_and_color = FW_CTXT_INVALID;
   q[0].id_and_color = ID_AND_COLOR(0, 0);         /* binding 0 = PHY context 0                              */
   q[0].quota = running ? (ULONG)MAX_QUOTA : 0UL;
   if (v1) {                                       /* v1: 12-byte entries                                    */
      static UCHAR b[MAX_BINDINGS * 12];

      for (i = 0; i < MAX_BINDINGS; i++) {
         put32((UCHAR FAR *)b + i * 12, q[i].id_and_color);
         put32((UCHAR FAR *)b + i * 12 + 4, q[i].quota);
         put32((UCHAR FAR *)b + i * 12 + 8, q[i].max_duration);
      }
      return iwl_cmd1(CMD_TIME_QUOTA, (UCHAR FAR *)b, sizeof(b));
   }
   return iwl_cmd1(CMD_TIME_QUOTA, (UCHAR FAR *)q, sizeof(q));
}

/* ---- auxiliary station (scanning), Bluetooth, regulatory ------------------------------------------------- */
static int add_aux_sta(void)
{
   static add_sta_cmd_t c;
   ULONG st = ADD_STA_SUCCESS;
   USHORT qid = (USHORT)(FW_CAPA(CAPA_DQA_SUPPORT) ? DQA_AUX_QUEUE : LEGACY_AUX_QUEUE);
   static const ULONG qbit[16] = { 1UL, 2UL, 4UL, 8UL, 0x10UL, 0x20UL, 0x40UL, 0x80UL, 0x100UL, 0x200UL, 0x400UL,
                                   0x800UL, 0x1000UL, 0x2000UL, 0x4000UL, 0x8000UL };
   int rc;

   if (iwl_enable_txq(AUX_STA_ID, qid, TX_FIFO_MCAST, MAX_TID_COUNT))
      return -1;
   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.sta_id = AUX_STA_ID;
   if (FW_API(API_STA_TYPE))
      c.station_type = STA_AUX_ACTIVITY;
   c.mac_id_n_color = ID_AND_COLOR(MAC_INDEX_AUX, 0);
   c.tfd_queue_msk = qbit[qid];
   c.tid_disable_tx = 0xFFFF;
   rc = iwl_cmd_status(CMD_ADD_STA, (UCHAR FAR *)&c, (USHORT)(FW_API(API_STA_TYPE) ? sizeof(c) : ADD_STA_CMD_V7_SIZE), &st);
   if (rc == 0 && (st & 0xFF) != ADD_STA_SUCCESS)
      rc = -2;
   return rc;
}

static int send_bt_init(void)
{
   static bt_coex_cmd_t c;

   c.mode = BT_COEX_WIFI;
   c.enabled_modules = BT_COEX_HIGH_BAND_RET;
   return iwl_cmd1(CMD_BT_CONFIG, (UCHAR FAR *)&c, sizeof(c));
}

/* "ZZ": the world regulatory domain; with LAR the firmware then follows the country it hears in beacons. */
static int send_mcc(void)
{
   static mcc_update_cmd_t c;

   if (g_cfg->family == IWL_FAM_8000 && !g_nvm_lar)
      return 0;
   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.mcc = (USHORT)(('Z' << 8) | 'Z');
   c.source_id = (UCHAR)((FW_API(API_WIFI_MCC_UPDATE) || FW_CAPA(CAPA_LAR_MULTI_MCC)) ? MCC_SOURCE_GET_CURRENT
                                                                                    : MCC_SOURCE_OLD_FW);
   return iwl_cmd(CMD_MCC_UPDATE, (UCHAR FAR *)&c,
                  (USHORT)(FW_CAPA(CAPA_LAR_SUPPORT_V3) ? sizeof(c) : MCC_UPDATE_CMD_V1_SIZE),
                  (UCHAR FAR *)0, 0, CMDF_WANT_RESP);
}

/* ---- firmware paging (the CPU2 image lives in host memory; 8000 family) ------------------------------------ */
static ULONG  g_pg_phys[NUM_OF_FW_PAGING_BLOCKS];
static USHORT g_pg_nblk, g_pg_last;

static int copy_to_phys(ULONG dst, ULONG src_off, USHORT n)
{
   USHORT s = scr_map(SCR_D, dst, n);

   if (!s)
      return -1;
   return blob_read(&g_fwblob, src_off, (UCHAR FAR *)MAKEP(s, 0), n);
}

static int setup_paging(void)
{
   static UCHAR cmd[12 + NUM_OF_FW_PAGING_BLOCKS * 4];
   fwimg_t *im = &g_fw.img[UCODE_REGULAR];
   USHORT  sec, i, pages;
   ULONG   off = 0;

   if (!im->paging_size)
      return 0;
   for (sec = 0; sec < im->nsec; sec++)
      if (im->sec[sec].devoff == PAGING_SEPARATOR_SECTION)
         break;
   sec++;
   if (sec + 1 >= im->nsec) {
      dd_err("paging: CSS or paging section missing", (ULONG)sec);
      return -1;
   }
   pages = (USHORT)(im->paging_size >> PAGE_2_EXP_SIZE);
   g_pg_nblk = (USHORT)(((pages - 1) >> 3) + 1);
   g_pg_last = (USHORT)(pages - ((g_pg_nblk - 1) << 3));
   if (g_pg_nblk + 1 > NUM_OF_FW_PAGING_BLOCKS)
      return -2;
   if (!g_pg_phys[0]) {                            /* first bring-up: CSS block + paging blocks              */
      if ((g_pg_phys[0] = dma_big_al((ULONG)FW_PAGING_SIZE, 4096)) == 0UL)
         return -3;
      for (i = 1; i <= g_pg_nblk; i++)
         if ((g_pg_phys[i] = dma_big_al(PAGING_BLOCK_SIZE, 4096)) == 0UL)
            return -3;
   }
   /* CSS: the section right after the separator */
   if (copy_to_phys(g_pg_phys[0], im->sec[sec].off,
                    (USHORT)(im->sec[sec].len < FW_PAGING_SIZE ? im->sec[sec].len : FW_PAGING_SIZE)))
      return -4;
   sec++;
   for (i = 1; i <= g_pg_nblk; i++) {
      USHORT n = (i < g_pg_nblk) ? (USHORT)PAGING_BLOCK_SIZE : (USHORT)(g_pg_last << PAGE_2_EXP_SIZE);

      if (off + n > im->sec[sec].len)
         n = (USHORT)(im->sec[sec].len - off);
      if (copy_to_phys(g_pg_phys[i], im->sec[sec].off + off, n))
         return -4;
      off += n;
   }
   fmemset((UCHAR FAR *)cmd, 0, sizeof(cmd));
   put32((UCHAR FAR *)cmd, PAGING_CMD_IS_SECURED | PAGING_CMD_IS_ENABLED | (ULONG)g_pg_last);
   put32((UCHAR FAR *)cmd + 4, (ULONG)BLOCK_2_EXP_SIZE);
   put32((UCHAR FAR *)cmd + 8, (ULONG)g_pg_nblk);
   for (i = 0; i <= g_pg_nblk; i++)
      put32((UCHAR FAR *)cmd + 12 + i * 4, g_pg_phys[i] >> PAGE_2_EXP_SIZE);
   return iwl_cmd1(WIDE_ID(LONG_GROUP, CMD_FW_PAGING_BLOCK), (UCHAR FAR *)cmd, sizeof(cmd));
}

/* ---- notifications and answers --------------------------------------------------------------------------- */
void iwl_rx_pkt(USHORT code, UCHAR qid, UCHAR idx, UCHAR FAR *p, USHORT len)
{
   (void)idx;
   switch (code) {
   case CMD_ALIVE:
      if (len == sizeof(alive_v3_t)) {
         alive_v3_t FAR *a = (alive_v3_t FAR *)p;

         g_err_table = a->error_event_table_ptr;
         g_umac_err_table = a->error_info_addr;
         g_sched_base = a->scd_base_ptr;
         g_alive_ok = (USHORT)(a->status == ALIVE_STATUS_OK);
      } else if (len == sizeof(alive_v2_t)) {
         alive_v2_t FAR *a = (alive_v2_t FAR *)p;

         g_err_table = a->v1.error_event_table_ptr;
         g_umac_err_table = a->error_info_addr;
         g_sched_base = a->v1.scd_base_ptr;
         g_alive_ok = (USHORT)(a->v1.status == ALIVE_STATUS_OK);
      } else if (len == sizeof(alive_v1_t)) {
         alive_v1_t FAR *a = (alive_v1_t FAR *)p;

         g_err_table = a->error_event_table_ptr;
         g_sched_base = a->scd_base_ptr;
         g_alive_ok = (USHORT)(a->status == ALIVE_STATUS_OK);
      } else
         dd_err("ALIVE of unknown length", (ULONG)len);
      g_alive = 1;
      break;
   case CMD_CALIB_RES_NOTIF_PHY_DB:
      iwl_phydb_store(p, len);
      g_calib_complete = 1;
      break;
   case CMD_INIT_COMPLETE_NOTIF:
      g_init_complete = 1;
      break;
   case CMD_STATISTICS_NOTIF:                     /* sent every few hundred ms; not used                    */
   case CMD_MCC_CHUB_UPDATE:                      /* the regulatory domain the firmware settled on          */
   case CMD_MFUART_LOAD_NOTIF:
      break;
   case CMD_REPLY_RX_PHY:
      iwl_rx_phy(p, len);
      break;
   case CMD_REPLY_RX_MPDU:
      iwl_rx_mpdu(p, len);
      break;
   case CMD_BA_NOTIF:
      txa_ba_notif(p, len);
      break;
   case CMD_TX:
      iwl_tx_done(qid, idx, p, len);
      break;
   case CMD_MISSED_BEACONS_NOTIF:
      if (len >= sizeof(missed_beacons_notif_t) &&
          ((missed_beacons_notif_t FAR *)p)->consec_missed_beacons_since_last_rx > 24UL)
         sta_lost();                               /* about 2.5 s without a beacon from the AP               */
      break;
   case CMD_SCAN_OFFLOAD_COMPLETE:
   case CMD_SCAN_ITERATION_COMPLETE:
      iwl_scan_done();                            /* LMAC scan                                              */
      break;
   case WIDE_ID(0, CMD_SCAN_COMPLETE_UMAC):
   case WIDE_ID(0, CMD_SCAN_ITERATION_COMPLETE_UMAC):
   case WIDE_ID(LONG_GROUP, CMD_SCAN_COMPLETE_UMAC):
   case WIDE_ID(LONG_GROUP, CMD_SCAN_ITERATION_COMPLETE_UMAC):
      iwl_scan_done();
      break;
   case CMD_TIME_EVENT_NOTIF:
      iwl_te_notif(p, len);
      break;
   case CMD_REPLY_ERROR:
      if (len >= sizeof(error_resp_t)) {
         g_fw_errors_seen++;
         g_last_fw_err = ((error_resp_t FAR *)p)->error_type;
         g_last_fw_err_cmd = ((error_resp_t FAR *)p)->cmd_id;
         dd_err("firmware reports an error for command", g_last_fw_err_cmd);
      }
      break;
   case WIDE_ID(PHY_OPS_GROUP, CMD_CT_KILL_NOTIF):
      dd_err("the adapter is at its critical temperature", (ULONG)get16(p));
      g_fw_error++;
      break;
   default:
      if (qid & 0x80) {                            /* an unsolicited notification we do not use              */
         g_unhandled++;
         g_last_unhandled = code;
         g_unh_qid = qid;
         g_unh_idx = idx;
         g_unh_len = len;
         fmemcpy((UCHAR FAR *)g_unh_d, p, (USHORT)(len < 8 ? len : 8));
         if (code < 256 && g_unh_by[code] < 0xFFFF)
            g_unh_by[code]++;
      }
      break;
   }
}

/* ---- bring-up ------------------------------------------------------------------------------------------- */
static void step(USHORT n)
{
   g_hw_state = n;
   tcom('['); tcom('i'); th8((UCHAR)n); tcom(']');
}

static int fail(const char *what, ULONG v)
{
   dd_err(what, v);
   if (g_fw_error)
      iwl_dump_fw_error();
   g_hw_failed = 1;
   return -1;
}

/* 8000 family: the revision register has a new layout, and the C step is read from the AUX bus (iwm_attach) */
static void read_hw_rev(void)
{
   g_hw_rev = iwl_rd(CSR_HW_REV);
   if (g_cfg->family < IWL_FAM_8000)
      return;
   g_hw_rev = (g_hw_rev & 0xFFF0UL) | ((((g_hw_rev << 2) >> 2) & 3UL) << 2);
   if (iwl_prepare_card_hw())
      return;
   iwl_set(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);
   dd_iodelay(2);
   if (!iwl_poll(CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY, 25000))
      return;
   if (iwl_nic_lock()) {
      ULONG st;

      iwl_prph_wr(PRPH_WFPM_CTRL_REG, iwl_prph_rd(PRPH_WFPM_CTRL_REG) | PRPH_ENABLE_WFPM);
      st = (iwl_prph_rd(PRPH_AUX_MISC_REG) >> PRPH_HW_STEP_LOCATION_BITS) & 0xFUL;
      if (st == 3UL)
         g_hw_rev = (g_hw_rev & 0xFFFFFFF3UL) | (2UL << 2);   /* SILICON_C_STEP                             */
      iwl_nic_unlock();
   }
}

static int run_init_image(void)
{
   ULONG t0;

   g_init_complete = g_calib_complete = 0;
   iwl_phydb_reset();
   if (iwl_load_ucode(UCODE_INIT))
      return fail("the INIT firmware did not start", (ULONG)g_hw_state);
   if (iwl_post_alive())
      return fail("post-ALIVE setup failed (INIT)", 0UL);
   step(0x10);
   if (g_cfg->family < IWL_FAM_8000 && send_bt_init())
      return fail("Bluetooth coexistence command failed", 0UL);
   if (iwl_nvm_init())
      return fail("the NVM could not be read", 0UL);
   step(0x11);
   if (iwl_send_sf(SF_INIT_OFF, 0) || send_tx_ant() || send_phy_cfg(UCODE_INIT))
      return fail("INIT configuration commands failed", 0UL);
   step(0x12);
   t0 = dd_ms();
   while (!(g_init_complete && g_calib_complete)) {   /* the firmware calibrates, sends PHY DB sections      */
      USHORT one = 1;

      hw_wait(&g_init_complete, 100);
      if (g_fw_error)
         return fail("firmware error during calibration", g_err_table);
      if ((long)(dd_ms() - t0) > 2000L)
         return fail("no INIT_COMPLETE from the firmware", (ULONG)((g_init_complete << 1) | g_calib_complete));
      (void)one;
   }
   return 0;
}

int hw_up(void)
{
   USHORT ac;
   static const UCHAR fifo[4] = { TX_FIFO_BE, TX_FIFO_BK, TX_FIFO_VI, TX_FIFO_VO };

   g_hw_failed = 0;
   g_hw_ready = 0;
   step(1);
   if (!g_cfg)
      return fail("no supported adapter", 0UL);
   if (!g_fwblob.done)
      return fail("the firmware file has not been written to the device", 0UL);
   step(2);
   if (iwl_fw_parse())
      return fail("the firmware file is not usable", g_fwblob.len);
   step(3);
   pci_enable();
   pci_cfg_wr32(0x40, pci_cfg_rd32(0x40) & ~0x0000FF00UL);   /* RETRY_TIMEOUT 0 (all iwlwifi drivers do this) */
   step(4);
   if (bar_map())
      return fail("BAR0 could not be mapped", g_bar0);
   step(5);
   if (iwl_trans_alloc())
      return fail("not enough DMA memory for the rings", (ULONG)dma_free_bytes());
   step(6);
   read_hw_rev();
   if (hook_init() || irq_hook())
      tstr("[iwl: no interrupt: polled]");
   step(7);
   if (iwl_start_hw())
      return fail("the adapter could not be started", iwl_rd(CSR_GP_CNTRL));
   if (g_rfkill)
      return fail("the radio is switched off (hardware RF kill)", iwl_rd(CSR_GP_CNTRL));
   step(8);
   if (run_init_image())
      return -1;
   step(0x20);
   iwl_stop_device();
   if (iwl_start_hw())
      return fail("the adapter could not be restarted after INIT", iwl_rd(CSR_GP_CNTRL));
   step(0x21);
   if (iwl_load_ucode(UCODE_REGULAR))
      return fail("the runtime firmware did not start", (ULONG)g_hw_state);
   if (iwl_post_alive())
      return fail("post-ALIVE setup failed", 0UL);
   step(0x22);
   if (setup_paging())
      return fail("firmware paging setup failed", g_fw.img[UCODE_REGULAR].paging_size);
   step(0x23);
   if (send_tx_ant() || iwl_phydb_send() || send_phy_cfg(UCODE_REGULAR) || send_bt_init())
      return fail("runtime configuration (antennas, PHY DB, PHY, BT) failed", 0UL);
   step(0x24);
   if (FW_CAPA(CAPA_DQA_SUPPORT)) {
      static UCHAR dqa[4];

      put32((UCHAR FAR *)dqa, (ULONG)DQA_CMD_QUEUE);
      if (iwl_cmd1(WIDE_ID(DATA_PATH_GROUP, CMD_DQA_ENABLE), (UCHAR FAR *)dqa, 4))
         return fail("DQA_ENABLE failed", 0UL);
   }
   if (add_aux_sta())
      return fail("the auxiliary station could not be added", 0UL);
   step(0x25);
   if (iwl_send_phy_ctxt((USHORT)FW_CTXT_ACTION_ADD, g_nvm_nchan ? g_nvm_chan[0] : 1, 1, 1, 0, 0))
      return fail("PHY context add failed", 0UL);
   if (FW_CAPA(CAPA_CT_KILL_BY_FW)) {
      static temp_report_ths_cmd_t ths;            /* no thresholds (num_temps 0): the firmware handles CT-kill  */

      if (iwl_cmd1(WIDE_ID(PHY_OPS_GROUP, CMD_TEMP_REPORTING_THRESHOLDS), (UCHAR FAR *)&ths, sizeof(ths)))
         tstr("[iwl: temperature thresholds refused]");
   }
   if (iwl_send_power())
      return fail("POWER_TABLE failed", 0UL);
   step(0x26);
   if (FW_CAPA(CAPA_LAR_SUPPORT) && send_mcc())
      tstr("[iwl: MCC update refused]");
   if (FW_CAPA(CAPA_UMAC_SCAN) && iwl_scan_config())   /* the LMAC scan (7000 family) needs no configuration */
      return fail("scan configuration failed", 0UL);
   step(0x27);
   for (ac = 0; ac < 4; ac++)
      if (iwl_enable_txq(STATION_ID, (USHORT)((FW_CAPA(CAPA_DQA_SUPPORT) ? DQA_MIN_MGMT_QUEUE : 0) + ac),
                         fifo[ac], TID_NON_QOS))
         return fail("a data queue could not be enabled", (ULONG)ac);
   {
      static beacon_filter_cmd_t bf;               /* all zero: beacon filtering off                         */

      iwl_cmd1(CMD_REPLY_BEACON_FILTERING, (UCHAR FAR *)&bf, sizeof(bf));
   }
   iwl_mac_reset();
   if (!g_cfgmac) {
      fmemcpy((UCHAR FAR *)g_mymac, (UCHAR FAR *)g_hw_mac, 6);
      ndis_set_mac(g_mymac);
   }
   g_hw_nss = (UCHAR)(((iwl_valid_rx_ant() & 3) == 3) ? 2 : 1);
   step(0x64);
   g_hw_ready = 1;
   tstr("[iwl: ready, firmware "); tstr(g_fw.version); tstr("]"); tnl();
   return 0;
}

void hw_down(void)
{
   if (!g_hw_ready && !g_hw_state)
      return;
   if (g_joined)
      sta_leave();
   g_hw_ready = 0;
   iwl_stop_device();
   iwl_trans_off();
   g_hw_state = 0;
}

/* ---- status lines ---------------------------------------------------------------------------------------- */
void hw_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void))
{
   USHORT i;

   ps("FWVER="); ps(g_fw.version); ps(" CAPAFLAGS="); ph(g_fw.capaflags, 8); ps(" PHYSKU="); ph(g_fw.phy_config, 8);
   ps(" INITSEC="); ph(g_fw.img[UCODE_INIT].nsec, 2); ps(" RTSEC="); ph(g_fw.img[UCODE_REGULAR].nsec, 2);
   ps(" PAGING="); ph(g_fw.img[UCODE_REGULAR].paging_size, 8); nl();
   ps("API="); for (i = 0; i < 8; i++) ph(g_fw.api[7 - i], 2);
   ps(" CAPA="); for (i = 0; i < 16; i++) ph(g_fw.capa[15 - i], 2); ps(" SCANCH="); ph(g_fw.n_scan_channels, 2); nl();
   ps("HWREV="); ph(g_hw_rev, 8); ps(" GP_CNTRL="); ph(g_nbarsel ? iwl_rd(CSR_GP_CNTRL) : 0UL, 8);
   ps(" CSR_INT="); ph(g_nbarsel ? iwl_rd(CSR_INT) : 0UL, 8); ps(" RFKILL="); ph(g_rfkill, 1);
   ps(" ALIVE="); ph(g_alive, 1); ph(g_alive_ok, 1); ps(" ERRTBL="); ph(g_err_table, 8);
   ps(" SCHED="); ph(g_sched_base, 8); ps(" CMDQ="); ph(g_cmdq, 2); nl();
   ps("NVM ver="); ph(g_nvm_version, 4); ps(" chans="); ph(g_nvm_nchan, 2); ps(" 5G="); ph(g_nvm_5ghz, 1);
   ps(" 11n="); ph(g_nvm_11n, 1); ps(" 11ac="); ph(g_nvm_11ac, 1); ps(" txant="); ph(iwl_valid_tx_ant(), 1);
   ps(" rxant="); ph(iwl_valid_rx_ant(), 1); ps(" LAR="); ph(g_nvm_lar, 1); ps(" PHYDB="); ph(g_phydb_n, 2); nl();
   ps("SVC="); ph(g_svc_runs, 4); ps(" RXPKT="); ph(g_rx_pkts, 4); ps(" RXBAD="); ph(g_rx_bad, 4);
   ps(" RXCUR="); ph(iwl_rx_cur(), 2); ps(" LASTCODE="); ph(g_last_code, 4); ps(" UNHANDLED="); ph(g_unhandled, 4);
   ps("/"); ph(g_last_unhandled, 4); ps(" CMDTO="); ph(g_cmd_to, 4); nl();
   if (g_unhandled) {                              /* what the notifications we do not use are                */
      USHORT n = 0;

      ps("UNH last code="); ph(g_last_unhandled, 4); ps(" qid="); ph(g_unh_qid, 2); ps(" idx="); ph(g_unh_idx, 2);
      ps(" len="); ph(g_unh_len, 4); ps(" data="); for (i = 0; i < 8; i++) ph(g_unh_d[i], 2);
      ps(" by code:");
      for (i = 0; i < 256 && n < 6; i++)
         if (g_unh_by[i]) { ps(" "); ph(i, 2); ps("x"); ph(g_unh_by[i], 4); n++; }
      nl();
   }
   ps("FWERR="); ph(g_fw_error, 4); ps(" HWERR="); ph(g_hw_err, 4); ps(" REPLYERR="); ph(g_fw_errors_seen, 4);
   ps(" type="); ph(g_last_fw_err, 8); ps(" cmd="); ph(g_last_fw_err_cmd, 2); nl();
   ps("CMDS="); ph(g_clog_n, 4); ps(" (oldest first, id/result/info):");
   {
      USHORT n = g_clog_n, first = (n > CLOG_N) ? (USHORT)(n - CLOG_N) : 0, k;

      for (k = first; k < n; k++) {
         clog_t *c = &g_clog[k % CLOG_N];

         ps(" "); ph(c->id, 4); ps("/"); ph((ULONG)(USHORT)c->rc, 4);
         if (c->info) { ps("/"); ph(c->info, 8); }
      }
   }
   nl();
   if (g_errlog_ok) {
      ps("FWLOG valid="); ph(g_errlog[0], 8); ps(" id="); ph(g_errlog[1], 8); ps(" data="); ph(g_errlog[7], 8);
      ps(","); ph(g_errlog[8], 8); ps(","); ph(g_errlog[9], 8); ps(" blink2="); ph(g_errlog[4], 8);
      ps(" ilink="); ph(g_errlog[5], 8); ps(","); ph(g_errlog[6], 8); nl();
      ps("FWLOG fwrev="); ph(g_errlog[15], 8); ps("/"); ph(g_errlog[16], 8); ps("/"); ph(g_errlog[17], 8);
      ps(" hw="); ph(g_errlog[18], 8); ps(" board="); ph(g_errlog[19], 8); ps(" pc="); ph(g_errlog[20], 8);
      ps(" fp="); ph(g_errlog[21], 8); ps(" sp="); ph(g_errlog[22], 8); nl();
      ps("FWLOG hcmd="); ph(g_errlog[23], 8); ps(" isr="); ph(g_errlog[24], 8); ps(","); ph(g_errlog[25], 8);
      ps(","); ph(g_errlog[26], 8); ps(","); ph(g_errlog[27], 8); ps(","); ph(g_errlog[28], 8);
      ps(" lastcmdid="); ph(g_errlog[29], 8); ps(" waitevent="); ph(g_errlog[30], 8); nl();
   }
   if (g_uerrlog_ok) {
      ps("UMACLOG valid="); ph(g_uerrlog[0], 8); ps(" id="); ph(g_uerrlog[1], 8); ps(" data="); ph(g_uerrlog[6], 8);
      ps(","); ph(g_uerrlog[7], 8); ps(","); ph(g_uerrlog[8], 8); ps(" blink="); ph(g_uerrlog[2], 8);
      ps(","); ph(g_uerrlog[3], 8); ps(" ilink="); ph(g_uerrlog[4], 8); ps(","); ph(g_uerrlog[5], 8);
      ps(" cmd="); ph(g_uerrlog[13], 8); ps(" isr="); ph(g_uerrlog[14], 8); nl();
   }
   ps("SCAN n="); ph(g_scan_n, 4); ps(" done="); ph(g_scan_done, 1); ps(" MPDU="); ph(g_rx_mpdu, 4);
   ps(" CRC="); ph(g_rx_crc, 4); ps(" DECFAIL="); ph(g_rx_decfail, 4); ps(" TXOK="); ph(g_tx_ok, 4);
   ps(" TXFAIL="); ph(g_tx_fail, 4); ps(" TXQ="); ph(iwl_txq_queued(QI_BE), 2); nl();
   ps("POWER save="); ph(g_powersave, 1); nl();
   if (g_cfg && g_cfg->family == IWL_FAM_9000)
      iwl9_status(ps, ph, nl);
   rs_status(ps, ph, nl);
   txa_status(ps, ph, nl);
   ps("ASYNC ok="); ph(g_async_ok, 4); ps(" fail="); ph(g_async_fail, 4); nl();
   ps("TXST last="); ph(g_tx_last_st, 4); ps(" retries="); ph(g_tx_last_fail, 2); ps(" rate="); ph(g_tx_last_rate, 8);
   ps(" hist(ok,direct,fail,other)="); for (i = 0; i < 4; i++) { ph(g_tx_st_hist[i], 4); ps(i < 3 ? "," : ""); } nl();
}
