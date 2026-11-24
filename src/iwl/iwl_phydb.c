/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_phydb.c - the PHY database: calibration results the INIT firmware sends (CALIB_RES_NOTIF_PHY_DB) are kept
 * and handed to the runtime firmware (PHY_DB_CMD), as iwlwifi's iwl-phy-db.c and OpenBSD iwm_phy_db_* do.
 * Sections: CFG, CALIB_NCH, CALIB_CHG_PAPD (per channel group), CALIB_CHG_TXP (per channel group).
 */
#include "iwl.h"

#define PHYDB_SIZE  16384U
#define PHYDB_MAX   24

typedef struct { USHORT type, grp, off, len; } phyent_t;

static DMA      g_db;
static USHORT   g_db_ok, g_db_used;
static phyent_t g_ent[PHYDB_MAX];
USHORT          g_phydb_n;
static USHORT   g_phydb_drop;

void iwl_phydb_reset(void)
{
   g_phydb_n = 0;
   g_db_used = 0;
}

/* p: { USHORT type; USHORT length; data } */
void iwl_phydb_store(UCHAR FAR *p, USHORT len)
{
   USHORT type, dl, grp = 0, i;
   phyent_t *e = 0;

   if (len < 4)
      return;
   if (!g_db_ok) {
      if (dma_get(PHYDB_SIZE, &g_db)) {
         g_phydb_drop++;
         return;
      }
      g_db_ok = 1;
   }
   type = get16(p);
   dl = get16(p + 2);
   if ((USHORT)(dl + 4) > len) {
      g_phydb_drop++;
      return;
   }
   if (type == PHY_DB_CALIB_CHG_PAPD || type == PHY_DB_CALIB_CHG_TXP)
      grp = (USHORT)(dl >= 2 ? get16(p + 4) : 0);
   for (i = 0; i < g_phydb_n; i++)                /* a new version of a section replaces the old one         */
      if (g_ent[i].type == type && g_ent[i].grp == grp)
         e = &g_ent[i];
   if (e && e->len >= dl) {
      fmemcpy(DPTR(g_db) + e->off, p + 4, dl);
      e->len = dl;
      return;
   }
   if (!e) {
      if (g_phydb_n >= PHYDB_MAX) {
         g_phydb_drop++;
         return;
      }
      e = &g_ent[g_phydb_n++];
   }
   if ((ULONG)g_db_used + dl > PHYDB_SIZE) {
      g_phydb_drop++;
      return;
   }
   e->type = type;
   e->grp = grp;
   e->off = g_db_used;
   e->len = dl;
   fmemcpy(DPTR(g_db) + e->off, p + 4, dl);
   g_db_used = (USHORT)(g_db_used + ((dl + 3) & ~3));
}

static int send_type(USHORT type)
{
   static UCHAR hdr[4];
   USHORT i;
   int    sent = 0;

   for (i = 0; i < g_phydb_n; i++) {
      phyent_t *e = &g_ent[i];

      if (e->type != type || !e->len)
         continue;
      put16((UCHAR FAR *)hdr, type);
      put16((UCHAR FAR *)hdr + 2, e->len);
      if (iwl_cmd(CMD_PHY_DB, (UCHAR FAR *)hdr, 4, DPTR(g_db) + e->off, e->len, 0))
         return -1;
      sent++;
   }
   return sent;
}

/* Order as iwlwifi: CFG, CALIB_NCH, the PAPD groups, the TXP groups. CFG and NCH must exist. */
int iwl_phydb_send(void)
{
   if (send_type(PHY_DB_CFG) <= 0) {
      dd_err("PHY DB: no CFG section from the INIT firmware", (ULONG)g_phydb_n);
      return -1;
   }
   if (send_type(PHY_DB_CALIB_NCH) <= 0) {
      dd_err("PHY DB: no CALIB_NCH section from the INIT firmware", (ULONG)g_phydb_n);
      return -2;
   }
   if (send_type(PHY_DB_CALIB_CHG_PAPD) < 0 || send_type(PHY_DB_CALIB_CHG_TXP) < 0)
      return -3;
   return 0;
}
