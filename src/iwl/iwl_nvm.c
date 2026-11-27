/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_nvm.c - the adapter's non-volatile memory (OTP) as the INIT firmware reports it with NVM_ACCESS_CMD:
 * MAC address, antennas, bands, the channel list, LAR (location-aware regulatory).
 *
 * Read like Linux mvm/nvm.c (iwl_nvm_read_chunk/iwl_nvm_read_section: 2 KB chunks until a short one), parsed
 * like OpenBSD if_iwm.c iwm_parse_nvm_data for the 8000 family ("extended" NVM: sections HW_8000, SW,
 * REGULATORY, MAC_OVERRIDE, PHY_SKU; word offsets from if_iwmreg.h). Only what this driver uses is kept.
 */
#include "iwl.h"

#define NVM_SECTION_SW           1
#define NVM_SECTION_REGULATORY   3
#define NVM_SECTION_HW_8000      10
#define NVM_SECTION_MAC_OVERRIDE 11
#define NVM_SECTION_PHY_SKU      12
#define NVM_CHUNK                2048
#define NVM_KEEP                 4096            /* bytes of a section kept (the regulatory LAR word is at 0xA0E) */

UCHAR  g_nvm_chan[64];
USHORT g_nvm_flags[64];                         /* NVM channel flags: 1 valid, 8 active, 200h 40 MHz, 400h 80 MHz */
USHORT g_nvm_nchan;
UCHAR  g_nvm_5ghz, g_nvm_11n, g_nvm_11ac, g_nvm_tx_ant, g_nvm_rx_ant, g_nvm_lar;
USHORT g_nvm_version;

static const UCHAR g_chan8000[51] = {
   1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
   36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92,
   96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
   149, 153, 157, 161, 165, 169, 173, 177, 181
};

static const UCHAR g_chan7000[39] = {
   1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
   36, 40, 44, 48, 52, 56, 60, 64,
   100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
   149, 153, 157, 161, 165
};

static DMA    g_buf;
static USHORT g_buf_ok;

/* Read one section into g_buf (at most NVM_KEEP bytes kept); returns the section length, -1 = absent. */
static long read_section(USHORT type)
{
   static nvm_access_cmd_t c;
   USHORT off = 0, got, n;
   UCHAR FAR *b = DPTR(g_buf);

   for (;;) {
      fmemset((UCHAR FAR *)&c, 0, sizeof(c));
      c.op_code = 0;                              /* read                                                   */
      c.type = type;
      c.offset = off;
      c.length = NVM_CHUNK;
      if (iwl_cmd(CMD_NVM_ACCESS, (UCHAR FAR *)&c, sizeof(c), (UCHAR FAR *)0, 0, CMDF_WANT_RESP) ||
          g_resp_len < sizeof(nvm_access_resp_t))
         return off ? (long)off : -1L;
      if (get16(g_resp + 6) != 0)                 /* status: at offset 0 = no such section; later = the end */
         return off ? (long)off : -1L;
      if (get16(g_resp) != off)
         return -1L;
      got = get16(g_resp + 2);
      if (got > NVM_CHUNK || (USHORT)(got + 8) > g_resp_len)
         return -1L;
      if (off < NVM_KEEP) {
         n = (USHORT)((off + got > NVM_KEEP) ? (NVM_KEEP - off) : got);
         fmemcpy(b + off, g_resp + 8, n);
      }
      off = (USHORT)(off + got);
      if (got < NVM_CHUNK || off >= 0x8000U)
         return (long)off;
   }
}

static int valid_mac(UCHAR FAR *m)
{
   static const UCHAR reserved[6] = { 0x02, 0xCC, 0xAA, 0xFF, 0xEE, 0x00 };
   USHORT i, z = 1, f = 1;

   for (i = 0; i < 6; i++) {
      if (m[i] != 0) z = 0;
      if (m[i] != 0xFF) f = 0;
   }
   return !z && !f && !(m[0] & 1) && !fmemeq(m, (UCHAR FAR *)reserved, 6);
}

int hw_chan_vht80(UCHAR ch)
{
   USHORT i;

   if (!g_nvm_11n || !g_nvm_11ac)
      return 0;
   for (i = 0; i < g_nvm_nchan; i++)
      if (g_nvm_chan[i] == ch)
         return (g_nvm_flags[i] & 0x0400) != 0;
   return 0;
}

int hw_chan_ht40(UCHAR ch)
{
   USHORT i;

   if (!g_nvm_11n)
      return 0;
   for (i = 0; i < g_nvm_nchan; i++)
      if (g_nvm_chan[i] == ch)
         return (g_nvm_flags[i] & 0x0200) != 0;
   return 0;
}

/* The 7000 family (iwm_parse_nvm_data, device family 7000): the MAC address in the HW section (word 0x15, bytes swapped
 * in pairs); version, radio configuration and SKU in the SW section (words 0, 1, 2); the channel flags in the SW
 * section from word 0x20 (the 3168: the regulatory section 8). The antennas come from the firmware's PHY SKU. */
static int nvm_init_7000(void)
{
   static UCHAR mac[6];
   UCHAR FAR *b = DPTR(g_buf);
   long  len;
   ULONG sku;
   USHORT i, w0;

   len = read_section(0);                          /* NVM_SECTION_TYPE_HW                                     */
   if (len < 0x2A + 6)
      return -2;
   for (i = 0; i < 6; i += 2) {                    /* "214365": little-endian 16-bit words                    */
      mac[i] = b[0x2A + i + 1];
      mac[i + 1] = b[0x2A + i];
   }
   len = read_section(NVM_SECTION_SW);
   if (len < 8)
      return -3;
   g_nvm_version = get16(b);
   sku = get16(b + 4);
   g_nvm_tx_ant = g_nvm_rx_ant = 0;                /* unknown: the firmware's PHY SKU decides                 */
   g_nvm_lar = 0;
   g_nvm_5ghz = (UCHAR)((sku & 0x02UL) != 0);
   g_nvm_11n  = (UCHAR)((sku & 0x04UL) != 0);
   g_nvm_11ac = (UCHAR)((sku & 0x08UL) != 0);
   w0 = 0x20;
   if (g_cfg->nvm_type == IWL_NVM_SDP) {           /* the 3168: channel flags in the regulatory section       */
      len = read_section(8);
      if (len < 78)
         return -4;
      w0 = 0;
   } else if (len < (long)((0x20 + 39) * 2))
      return -4;
   g_nvm_nchan = 0;
   for (i = 0; i < 39; i++) {
      USHORT f = get16(b + (w0 + i) * 2);

      if (!(f & 0x0001))
         continue;
      if (i >= 14 && !g_nvm_5ghz)
         continue;
      g_nvm_flags[g_nvm_nchan] = f;
      g_nvm_chan[g_nvm_nchan++] = g_chan7000[i];
   }
   fmemcpy((UCHAR FAR *)g_hw_mac, (UCHAR FAR *)mac, 6);
   if (!valid_mac((UCHAR FAR *)g_hw_mac))
      dd_err("the NVM holds no valid MAC address; set NETADDRESS in PROTOCOL.INI", 0UL);
   return 0;
}

/* The 22000 family (AX200): NVM_GET_INFO (REGULATORY_AND_NVM_GROUP 0x0C, command 2) returns everything in one answer: general (flags,
 * version @4, board type, number of addresses), SKU flags @8, TX / RX chains @12 / @16, LAR @20, channel count @24 and the 32-bit channel
 * flags from @28 (Linux iwl_get_nvm, response version 4; the older version 3 has 16-bit flags). The MAC address is the strap in the CSR
 * (0x08 / 0x0C, bytes reversed). */
int iwl_nvm_init_22000(void)
{
   static UCHAR req[4];
   UCHAR FAR *r;
   ULONG sku, a0, a1;
   USHORT i, v4;
   U32    u;

   if (iwl_cmd(WIDE_ID(0x0C, 2), (UCHAR FAR *)req, 4, (UCHAR FAR *)0, 0, CMDF_WANT_RESP) || g_resp_len < 28 + 4)
      return -1;
   r = g_resp;
   v4 = (USHORT)(g_resp_len >= 28 + 4 * 110);
   g_nvm_version = get16(r + 4);
   sku = get32(r + 8);
   g_nvm_tx_ant = (UCHAR)get32(r + 12);
   g_nvm_rx_ant = (UCHAR)get32(r + 16);
   g_nvm_lar = (UCHAR)(get32(r + 20) != 0UL);
   g_nvm_5ghz = (UCHAR)((sku & 0x02UL) != 0);
   g_nvm_11n  = (UCHAR)((sku & 0x04UL) != 0);
   g_nvm_11ac = (UCHAR)((sku & 0x08UL) != 0);
   g_nvm_nchan = 0;
   for (i = 0; i < 51; i++) {
      USHORT f;

      if ((USHORT)(28 + (v4 ? 4 : 2) * (i + 1)) > g_resp_len)
         break;
      f = v4 ? get16(r + 28 + 4 * i) : get16(r + 28 + 2 * i);
      if (!(f & 0x0001))
         continue;
      if (i >= 14 && !g_nvm_5ghz)
         continue;
      if (g_nvm_nchan < sizeof(g_nvm_chan)) {
         g_nvm_flags[g_nvm_nchan] = f;
         g_nvm_chan[g_nvm_nchan++] = g_chan8000[i];
      }
   }
   a0 = iwl_rd(0x008);
   a1 = iwl_rd(0x00C);
   u.l = a0;
   g_hw_mac[0] = u.b[3]; g_hw_mac[1] = u.b[2]; g_hw_mac[2] = u.b[1]; g_hw_mac[3] = u.b[0];
   u.l = a1;
   g_hw_mac[4] = u.b[1]; g_hw_mac[5] = u.b[0];
   if (!valid_mac((UCHAR FAR *)g_hw_mac))
      dd_err("the NVM holds no valid MAC address; set NETADDRESS in PROTOCOL.INI", 0UL);
   return 0;
}

int iwl_nvm_init(void)
{
   static UCHAR mac_ovr[6];
   UCHAR FAR *b;
   long  len;
   ULONG radio, sku, a0, a1;
   USHORT have_hw, have_mao = 0, i, lar_off;
   U32   u;

   if (g_cfg->family < IWL_FAM_7000)
      return -1;
   if (!g_buf_ok) {
      if (dma_get(NVM_KEEP, &g_buf))
         return -2;
      g_buf_ok = 1;
   }
   b = DPTR(g_buf);
   if (g_cfg->family == IWL_FAM_7000)
      return nvm_init_7000();

   have_hw = (USHORT)(read_section(NVM_SECTION_HW_8000) > 0);
   if (read_section(NVM_SECTION_MAC_OVERRIDE) >= 8) {
      fmemcpy((UCHAR FAR *)mac_ovr, b + 2, 6);   /* word 1: no byte swapping in this section               */
      have_mao = 1;
   }
   if (read_section(NVM_SECTION_SW) < 8)
      return -3;
   g_nvm_version = get16(b);                       /* SW word 0                                              */
   if (read_section(NVM_SECTION_PHY_SKU) < 8)
      return -4;
   radio = get32(b);                               /* PHY_SKU word 0: radio configuration                    */
   sku = get32(b + 4);                             /* PHY_SKU word 2: SKU capabilities                       */
   u.l = radio;
   g_nvm_tx_ant = (UCHAR)(u.b[3] & 0x0F);
   g_nvm_rx_ant = (UCHAR)(u.b[3] >> 4);
   g_nvm_5ghz = (UCHAR)((sku & 0x02UL) != 0);
   g_nvm_11n  = (UCHAR)((sku & 0x04UL) != 0);
   g_nvm_11ac = (UCHAR)((sku & 0x08UL) != 0);

   len = read_section(NVM_SECTION_REGULATORY);
   if (len < 102)
      return -5;
   lar_off = (USHORT)(g_nvm_version < 0x0E39 ? 0x4C7 : 0x507);   /* words                                  */
   g_nvm_lar = (UCHAR)((len >= (long)(lar_off * 2 + 2) && (lar_off * 2 + 2) <= NVM_KEEP)
                       ? ((get16(b + lar_off * 2) & 0x7) != 0) : 0);
   g_nvm_nchan = 0;
   for (i = 0; i < 51 && (long)(i * 2 + 2) <= len; i++) {
      USHORT f = get16(b + i * 2);

      if (!(f & 0x0001))                           /* NVM_CHANNEL_VALID                                      */
         continue;
      if (i >= 14 && !g_nvm_5ghz)
         continue;
      if (g_nvm_nchan < sizeof(g_nvm_chan))
      {
         g_nvm_flags[g_nvm_nchan] = f;
         g_nvm_chan[g_nvm_nchan++] = g_chan8000[i];
      }
   }

   /* MAC address: the override section when it holds a real address, else the OTP (WFMP registers) */
   if (have_mao && valid_mac((UCHAR FAR *)mac_ovr)) {
      fmemcpy((UCHAR FAR *)g_hw_mac, (UCHAR FAR *)mac_ovr, 6);
   } else if (have_hw) {
      a0 = iwl_prph_rd(PRPH_WFMP_MAC_ADDR_0);
      a1 = iwl_prph_rd(PRPH_WFMP_MAC_ADDR_1);
      u.l = a0;
      g_hw_mac[0] = u.b[3]; g_hw_mac[1] = u.b[2]; g_hw_mac[2] = u.b[1]; g_hw_mac[3] = u.b[0];
      u.l = a1;
      g_hw_mac[4] = u.b[1]; g_hw_mac[5] = u.b[0];
   } else
      return -6;
   if (!valid_mac((UCHAR FAR *)g_hw_mac))
      dd_err("the NVM holds no valid MAC address; set NETADDRESS in PROTOCOL.INI", 0UL);
   return 0;
}
