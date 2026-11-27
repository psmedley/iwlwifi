/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_agg.c - transmit aggregation (A-MPDU) for TID 0: QoS sequence numbers, the aggregation queue, its firmware
 * setup, and the completion handling.
 *
 * The station (wlan/sta.c) negotiates the block-ack agreement with the AP (ADDBA request / response). When the AP
 * accepts, hw_txagg_offer() starts the sequence below; the design follows OpenBSD iwm (iwm_sta_tx_agg,
 * iwm_enable_txq, iwm_ampdu_tx_done, iwm_rx_compressed_ba) and Linux mvm (iwl_mvm_sta_tx_agg_oper):
 *
 *   OFFER   the AP said yes. Data frames wait until the ordinary (BE) queue is empty, so that no older frame of this
 *           TID can overtake the aggregated ones (the receiver's replay counter and block-ack window would drop it).
 *   (capture) with the next QoS data frame: the aggregation queue starts at ring index = sequence number & 255 (the
 *           hardware ties the two together on an aggregation queue), unless the ring pointer would move by 64, 128 or
 *           192 entries, which the scheduler cannot do (iwm "scd_bug"): then that one frame goes on the ordinary queue
 *           and the next one tries again. SCD_QUEUE_CFG (enable, aggregate, ssn, window) and a modifying ADD_STA
 *           (the queue in the station's queue mask, aggregation allowed for the TID) are queued without waiting.
 *   CFG     the frames queue up on the aggregation ring without telling the device.
 *   ACTIVE  the answer to the ADD_STA arrived: the device is told, from then on every frame rings the bell.
 *
 * The firmware aggregates what is queued, retransmits what the block ack reports lost, and moves its window; the host
 * only frees ring slots: up to the "ssn" of a single-frame transmit response or of a BA notification.
 */
#include "iwl.h"

#define TXA_IDLE    0
#define TXA_OFFER   1
#define TXA_CFG     2
#define TXA_ACTIVE  3
#define TXA_FAILED  4                                   /* this link: the setup failed, no more attempts        */

USHORT g_hwtxlk;                                        /* the data queues (defined here, used by iwl_mac.c)     */

static USHORT g_txa_state, g_txa_win, g_txa_ssn;
static USHORT g_qsn;                                    /* next QoS sequence number of TID 0                      */
static UCHAR  g_txa_cfg_idx;
static ULONG  g_txa_t_offer;
USHORT g_txa_started, g_txa_stopped, g_txa_cfg_ok, g_txa_cfg_fail, g_txa_bug, g_txa_drain, g_txa_ba, g_txa_resp1;
USHORT g_txa_ampdu_resp, g_txa_ampdu_frames, g_txa_misalign, g_txa_bad_notif, g_txa_giveup, g_txa_cmd_fail;

/* TXA_PURE_BEGIN */
/* The scheduler's write pointer cannot move by 64, 128 or 192 entries (iwm_enable_txq: "scd_bug"). */
static int agg_move_bug(USHORT cur, USHORT idx)
{
   USHORT d = (USHORT)((idx - cur) & 0xFF);

   return d == 64 || d == 128 || d == 192;
}
/* TXA_PURE_END */

/* ---- sequence numbers ---------------------------------------------------------------------------------------- */
USHORT txa_alloc_sn(void)                               /* with the data-queue lock held                           */
{
   USHORT sn = g_qsn;

   g_qsn = (USHORT)((g_qsn + 1) & 0x0FFF);
   return sn;
}

USHORT hw_qos_sn(void)
{
   return g_qsn;
}

/* ---- the firmware side of a session -------------------------------------------------------------------------- */
static int cmd_queue_cfg(USHORT sn, USHORT win)
{
   static scd_txq_cfg_cmd_t c;

   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.tid = 0;
   c.scd_queue = (UCHAR)iwl_txq_qid(QI_AGG);
   c.enable = 1;
   c.sta_id = STATION_ID;
   c.tx_fifo = TX_FIFO_BE;
   c.aggregate = 1;
   c.ssn = sn;
   c.window = (UCHAR)win;
   return iwl_cmd_async(CMD_SCD_QUEUE_CFG, (UCHAR FAR *)&c, sizeof(c));
}

/* The station's queue mask (the four access-category queues, plus the aggregation queue when it is in use) and the
 * TIDs that may not aggregate (a set bit = not allowed). */
static int cmd_sta_queues(USHORT agg_on)
{
   static add_sta_cmd_t c;
   ULONG m = 0;
   USHORT i;

   for (i = 0; i < 4; i++)
      m |= (ULONG)(1U << (DQA_MIN_MGMT_QUEUE + i));
   m |= (ULONG)(1U << iwl_txq_qid(QI_AGG));                 /* the queue stays in the mask once enabled           */
   fmemset((UCHAR FAR *)&c, 0, sizeof(c));
   c.sta_id = STATION_ID;
   c.mac_id_n_color = ID_AND_COLOR(0, 0);
   c.add_modify = 1;
   c.modify_mask = STA_MODIFY_QUEUES | STA_MODIFY_TID_DISABLE_TX;
   c.tfd_queue_msk = m;
   c.tid_disable_tx = (USHORT)(agg_on ? 0xFFFE : 0xFFFF);   /* TID 0 may aggregate / may not                      */
   return iwl_cmd_async(CMD_ADD_STA, (UCHAR FAR *)&c,
                        (USHORT)(FW_API(API_STA_TYPE) ? sizeof(c) : ADD_STA_CMD_V7_SIZE));
}

/* The next QoS data frame starts the aggregation queue (data-queue lock held). 0 = started. */
static int capture(USHORT sn)
{
   int rc;

   iwl_txq_reset(QI_AGG, (USHORT)(sn & 0xFF));             /* the ring starts at index sn & 255                    */
   rc = cmd_queue_cfg(sn, g_txa_win);
   if (rc) {
      g_txa_cmd_fail++;
      return rc;
   }
   rc = cmd_sta_queues(1);
   if (rc) {
      g_txa_cmd_fail++;
      return rc;
   }
   g_txa_cfg_idx = (UCHAR)g_cmd_last_idx;
   g_txa_ssn = sn;
   g_txa_state = TXA_CFG;
   g_txa_started++;
   return 0;
}

int txa_pick(USHORT *qi)                                /* data-queue lock held                                   */
{
   USHORT sn;

   *qi = QI_BE;
   switch (g_txa_state) {
   case TXA_OFFER:
      if ((long)(dd_ms() - g_txa_t_offer) > 2000L) {      /* the ordinary queue never emptied: give up            */
         g_txa_state = TXA_IDLE;
         g_txa_giveup++;
         return 0;
      }
      if (iwl_txq_queued(QI_BE)) {                      /* let the frames in flight go first                    */
         g_txa_drain++;
         return -2;
      }
      sn = g_qsn;
      if (agg_move_bug(iwl_txq_cur(QI_AGG), (USHORT)(sn & 0xFF))) {
         g_txa_bug++;                                   /* this frame takes the ordinary queue (and one number)  */
         return 0;
      }
      if (capture(sn)) {
         g_txa_state = TXA_FAILED;
         return 0;
      }
      *qi = QI_AGG;
      return 0;
   case TXA_CFG:
   case TXA_ACTIVE:
      *qi = QI_AGG;
      return 0;
   }
   return 0;
}

int txa_bell_ok(USHORT qi)
{
   return qi != QI_AGG || g_txa_state == TXA_ACTIVE;
}

/* ---- locked entry points of the service path ------------------------------------------------------------------ */
static int enter(USHORT FAR *fl)
{
   *fl = rt_irq_save();
   if (rt_lock_acquire(&g_hwtxlk, (USHORT)(rt_cpu_id() + 1))) {
      rt_irq_restore(*fl);
      return -1;
   }
   return 0;
}

static void leave(USHORT fl)
{
   rt_lock_release(&g_hwtxlk);
   rt_irq_restore(fl);
}

int hw_txagg_active(void)
{
   return g_txa_state == TXA_OFFER || g_txa_state == TXA_CFG || g_txa_state == TXA_ACTIVE;
}

int hw_txagg_ok(void)
{
   return iwl_txq_present(QI_AGG) && g_hw_ready && FW_CAPA(CAPA_DQA_SUPPORT) && g_txa_state != TXA_FAILED;
}

void hw_txagg_offer(UCHAR tid, USHORT win)
{
   USHORT fl;

   if (tid != 0 || enter(&fl))
      return;
   if (g_txa_state == TXA_IDLE && hw_txagg_ok()) {
      g_txa_win = (win == 0 || win > 64) ? 64 : win;
      g_txa_t_offer = dd_ms();
      g_txa_state = TXA_OFFER;
   }
   leave(fl);
}

void hw_txagg_stop(UCHAR tid)
{
   USHORT fl;

   if (tid != 0 || enter(&fl))
      return;
   if (g_txa_state == TXA_CFG || g_txa_state == TXA_ACTIVE)
      cmd_sta_queues(0);                                /* aggregation not allowed for the TID any more         */
   if (g_txa_state != TXA_IDLE && g_txa_state != TXA_FAILED) {
      g_txa_state = TXA_IDLE;
      g_txa_stopped++;
   }
   leave(fl);
}

void txa_reset(void)                                    /* link down: the station is gone with its queues        */
{
   USHORT fl;

   if (enter(&fl))
      return;
   if (g_txa_state != TXA_IDLE)
      g_txa_stopped++;
   g_txa_state = TXA_IDLE;
   leave(fl);
}

/* The answer to the modifying ADD_STA that completes the setup. */
void txa_cfg_answer(UCHAR idx, int ok)
{
   USHORT fl;

   if (g_txa_state != TXA_CFG || idx != g_txa_cfg_idx || enter(&fl))
      return;
   if (g_txa_state == TXA_CFG) {
      if (ok) {
         g_txa_state = TXA_ACTIVE;
         g_txa_cfg_ok++;
      } else {
         g_txa_state = TXA_FAILED;                      /* the queued frames go out as they are                  */
         g_txa_cfg_fail++;
      }
      iwl_txq_bell(QI_AGG);                             /* frames queued while the firmware was being set up     */
   }
   leave(fl);
}

/* ---- completion ------------------------------------------------------------------------------------------------ */
/* BA_NOTIF: sta_addr[6] rsvd[2] sta_id tid seq_ctl[2] bitmap[8] scd_flow[2] scd_ssn[2] txed txed_2_done ... (28 bytes).
 * scd_ssn is the first frame still inside the firmware's window: everything before it is done. */
void txa_ba_notif(UCHAR FAR *p, USHORT len)
{
   USHORT fl, flow, ssn;

   if (len < 28)
      return;
   flow = get16(p + 20);
   ssn = get16(p + 22);
   if (p[8] != STATION_ID || p[9] != 0 || !iwl_txq_present(QI_AGG) || flow != iwl_txq_qid(QI_AGG)) {
      g_txa_bad_notif++;
      return;
   }
   rs_ba(p[24], p[25]);
   if (enter(&fl))
      return;
   if (g_txa_state == TXA_ACTIVE) {
      iwl_txq_reclaim_to(QI_AGG, (USHORT)(ssn & 0xFF));
      g_txa_ba++;
   }
   leave(fl);
}

/* TX_CMD answer of the aggregation queue (iwm_rx_tx_cmd / iwm_ampdu_tx_done): one frame = a single transmission with
 * the ssn after the status entries; several = an A-MPDU, whose slots are freed by the block ack. */
void txa_tx_resp(UCHAR qid, UCHAR FAR *p, USHORT len)
{
   USHORT fl, n;

   (void)qid;
   if (len < 36 + 4 + 4)
      return;
   n = p[0];                                            /* frame_count                                           */
   if (n > 1) {
      g_txa_ampdu_resp++;
      g_txa_ampdu_frames = (USHORT)(g_txa_ampdu_frames + n);
      return;
   }
   if (n != 1 || (USHORT)(36 + 4 + 4) > len)
      return;
   if (enter(&fl))
      return;
   if (g_txa_state == TXA_ACTIVE) {
      iwl_txq_reclaim_to(QI_AGG, (USHORT)(get32(p + 36 + 4) & 0xFF));
      g_txa_resp1++;
   }
   leave(fl);
}

void txa_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void))
{
   static const char *name[5] = { "idle", "offered", "configuring", "ACTIVE", "FAILED" };

   ps("TXAGG state="); ps(name[g_txa_state]); ps(" win="); ph(g_txa_win, 2); ps(" ssn="); ph(g_txa_ssn, 4);
   ps(" nextsn="); ph(g_qsn, 4); ps(" q="); ph(iwl_txq_qid(QI_AGG), 2);
   ps(" cur="); ph(iwl_txq_present(QI_AGG) ? iwl_txq_cur(QI_AGG) : 0, 2);
   ps(" queued="); ph(iwl_txq_present(QI_AGG) ? iwl_txq_queued(QI_AGG) : 0, 2); nl();
   ps("TXAGG started="); ph(g_txa_started, 4); ps(" stopped="); ph(g_txa_stopped, 4); ps(" cfgok="); ph(g_txa_cfg_ok, 4);
   ps(" cfgfail="); ph(g_txa_cfg_fail, 4); ps(" cmdfail="); ph(g_txa_cmd_fail, 4); ps(" scdbug="); ph(g_txa_bug, 4);
   ps(" drain="); ph(g_txa_drain, 4); ps(" giveup="); ph(g_txa_giveup, 4); ps(" misalign="); ph(g_txa_misalign, 4); nl();
   ps("TXAGG banotif="); ph(g_txa_ba, 4); ps(" badnotif="); ph(g_txa_bad_notif, 4); ps(" single="); ph(g_txa_resp1, 4);
   ps(" ampdus="); ph(g_txa_ampdu_resp, 4); ps(" frames="); ph(g_txa_ampdu_frames, 4); nl();
}
