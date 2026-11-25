/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_cfg.c - the devices this back end knows, their configuration, and the back end's identity functions.
 *
 * PCI ids and family assignments follow Linux iwlwifi (pcie/drv.c, cfg/7000.c, cfg/8000.c, cfg/9000.c) and
 * OpenBSD if_iwm.c (iwm_attach). To add a device of a family that is already supported, add a line to
 * g_devs[]. To add a family, see docs/ADDING-A-DEVICE.md: the family-specific code paths are marked with
 * g_cfg->family tests (firmware download, NVM layout, interrupt work-arounds, receive queue type).
 */
#include "iwl.h"

/* ---- compile-time checks of the firmware structures (sizes from OpenBSD if_iwmreg.h, gcc) ------------ */
#define SIZE_IS(t, n)  typedef char chk_##t[(sizeof(t) == (n)) ? 1 : -1]
SIZE_IS(alive_v1_t, 44);
SIZE_IS(alive_v2_t, 64);
SIZE_IS(alive_v3_t, 68);
SIZE_IS(nvm_access_cmd_t, 8);
SIZE_IS(phy_cfg_cmd_t, 12);
SIZE_IS(sf_cfg_cmd_t, 92);
SIZE_IS(scd_txq_cfg_cmd_t, 12);
SIZE_IS(add_sta_cmd_t, 48);
SIZE_IS(add_sta_key_cmd_t, 76);
SIZE_IS(phy_ctx_cmd_t, 36);
SIZE_IS(phy_ctx_cmd_uhb_t, 40);
SIZE_IS(binding_cmd_t, 28);
SIZE_IS(mac_ctx_cmd_t, 148);
SIZE_IS(mac_data_sta_t, 44);
SIZE_IS(time_event_cmd_t, 36);
SIZE_IS(time_event_resp_t, 16);
SIZE_IS(device_power_cmd_t, 4);
SIZE_IS(mac_power_cmd_t, 40);
SIZE_IS(beacon_filter_cmd_t, 44);
SIZE_IS(mcast_filter_cmd_t, 12);
SIZE_IS(mcc_update_cmd_t, 28);
SIZE_IS(lq_cmd_t, 88);
SIZE_IS(tx_cmd_t, 56);
SIZE_IS(tx_resp_t, 40);
SIZE_IS(rx_phy_info_t, 68);
SIZE_IS(scan_config_t, 36);
SIZE_IS(scan_chan_umac_t, 8);
SIZE_IS(missed_beacons_notif_t, 20);
SIZE_IS(error_resp_t, 20);
SIZE_IS(temp_report_ths_cmd_t, 20);
SIZE_IS(bt_coex_cmd_t, 8);

/* ---- family configurations --------------------------------------------------------------------------- */
/* Only the 8000 family is driven by this build. The others are listed so that the driver can name what it
 * found, and as the starting point for adding them (docs/ADDING-A-DEVICE.md). */
static const iwl_cfg_t cfg_8260 = { IWL_FAM_8000, IWL_NVM_EXT, 0, 1, 32768U, "iwlwifi-8000C-36.ucode" };
static const iwl_cfg_t cfg_8265 = { IWL_FAM_8000, IWL_NVM_EXT, 0, 1, 32768U, "iwlwifi-8265-36.ucode" };
static const iwl_cfg_t cfg_7260 = { IWL_FAM_7000, IWL_NVM,     1, 0, 16384U, "iwlwifi-7260-17.ucode" };
static const iwl_cfg_t cfg_3160 = { IWL_FAM_7000, IWL_NVM,     1, 0, 16384U, "iwlwifi-3160-17.ucode" };
static const iwl_cfg_t cfg_7265 = { IWL_FAM_7000, IWL_NVM,     0, 0, 16384U, "iwlwifi-7265D-29.ucode" };
static const iwl_cfg_t cfg_3168 = { IWL_FAM_7000, IWL_NVM_SDP, 0, 0, 16384U, "iwlwifi-3168-29.ucode" };
static const iwl_cfg_t cfg_9560 = { IWL_FAM_9000, IWL_NVM_EXT, 0, 0, 32768U, "iwlwifi-9000-pu-b0-jf-b0-46.ucode" };   /* CNVi, "Jefferson Peak" radio */
static const iwl_cfg_t cfg_9260 = { IWL_FAM_9000, IWL_NVM_EXT, 0, 0, 32768U, "iwlwifi-9260-th-b0-jf-b0-46.ucode" };

static const iwl_dev_t g_devs[] = {
   /* 8000 family (Linux cfg/8000.c: iwl8260_cfg, iwl8265_cfg) */
   { 0x24F3, 0xFFFF, &cfg_8260, "Intel(R) Dual Band Wireless-AC 8260" },
   { 0x24F4, 0xFFFF, &cfg_8260, "Intel(R) Dual Band Wireless-AC 8260" },
   { 0x24F5, 0xFFFF, &cfg_8260, "Intel(R) Dual Band Wireless-AC 4165" },
   { 0x24F6, 0xFFFF, &cfg_8260, "Intel(R) Dual Band Wireless-AC 8260" },
   { 0x24FD, 0xFFFF, &cfg_8265, "Intel(R) Dual Band Wireless-AC 8265 (experimental)" },
   /* 7000 family: not driven yet (firmware download without CPU2, NVM layout, 7260 coalescing work-around) */
   { 0x08B1, 0xFFFF, &cfg_7260, "Intel(R) Wireless-N/AC 7260 (not supported yet)" },
   { 0x08B2, 0xFFFF, &cfg_7260, "Intel(R) Wireless-N/AC 7260 (not supported yet)" },
   { 0x08B3, 0xFFFF, &cfg_3160, "Intel(R) Wireless 3160 (not supported yet)" },
   { 0x08B4, 0xFFFF, &cfg_3160, "Intel(R) Wireless 3160 (not supported yet)" },
   { 0x095A, 0xFFFF, &cfg_7265, "Intel(R) Wireless 7265 (not supported yet)" },
   { 0x095B, 0xFFFF, &cfg_7265, "Intel(R) Wireless 7265 (not supported yet)" },
   { 0x3165, 0xFFFF, &cfg_7265, "Intel(R) Wireless 3165 (not supported yet)" },
   { 0x3166, 0xFFFF, &cfg_7265, "Intel(R) Wireless 3165 (not supported yet)" },
   { 0x24FB, 0xFFFF, &cfg_3168, "Intel(R) Wireless 3168 (not supported yet)" },
   /* 9000 family: not driven yet (multi-queue receive) */
   { 0x2526, 0xFFFF, &cfg_9260, "Intel(R) Wireless-AC 9260 (not supported yet)" },
   { 0xA370, 0x4030, &cfg_9560, "Intel(R) Wireless-AC 9560 160MHz (not supported yet)" },
   { 0xA370, 0xFFFF, &cfg_9560, "Intel(R) Wireless-AC 9560 / 9560 CNVi (not supported yet)" },   /* Cannon Lake PCH CNVi */
   { 0x9DF0, 0xFFFF, &cfg_9560, "Intel(R) Wireless-AC 9560 CNVi (not supported yet)" },
   { 0x31DC, 0xFFFF, &cfg_9560, "Intel(R) Wireless-AC 9560 CNVi (not supported yet)" },
   { 0x02F0, 0xFFFF, &cfg_9560, "Intel(R) Wireless-AC 9560 CNVi (not supported yet)" },
};
#define NDEVS (sizeof(g_devs) / sizeof(g_devs[0]))

const iwl_cfg_t *g_cfg;
char *g_devname = "no supported Intel wireless adapter";
static char g_unsup[] = "an Intel wireless adapter this build does not drive yet";
static USHORT g_seen_unsup;

/* INIT: called for every PCI function. */
int hw_pci_match(USHORT ven, USHORT dev, USHORT subven, USHORT subdev)
{
   USHORT i;

   (void)subven;
   if (ven != 0x8086)
      return 0;
   for (i = 0; i < NDEVS; i++) {
      if (g_devs[i].dev != dev)
         continue;
      if (g_devs[i].subdev != 0xFFFF && g_devs[i].subdev != subdev)
         continue;
      if (!g_devs[i].cfg->supported) {
         if (!g_seen_unsup) {
            g_seen_unsup = 1;
            g_devname = g_devs[i].name;           /* reported if nothing better turns up                   */
         }
         return 0;
      }
      g_cfg = g_devs[i].cfg;
      g_devname = g_devs[i].name;
      g_bar_len = 0x00010000UL;                   /* BAR0 is 8 KB on these devices; one 64 KB window maps it */
      return 1;
   }
   return 0;
}

char *hw_name(void)
{
   if (!g_cfg && g_seen_unsup)
      return g_devname[0] ? g_devname : g_unsup;
   return g_devname;
}

char *hw_fw_name(void)
{
   return g_cfg ? g_cfg->fw : "iwlwifi-8000C-36.ucode";
}

/* An iwlwifi .ucode file in the TLV format starts with a zero ULONG and the magic "IWL\n". */
int hw_fw_is_mine(UCHAR FAR *h, USHORT n)
{
   if (n < 8)
      return 0;
   return h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 0 && h[4] == 'I' && h[5] == 'W' && h[6] == 'L' && h[7] == '\n';
}

UCHAR hw_vendor_oui(USHORT i)
{
   static const UCHAR intel[3] = { 0x00, 0x1B, 0x21 };   /* an Intel OUI; the adapter's own when known     */

   if (g_hw_mac[0] | g_hw_mac[1] | g_hw_mac[2])
      return g_hw_mac[i < 3 ? i : 0];
   return intel[i < 3 ? i : 0];
}
