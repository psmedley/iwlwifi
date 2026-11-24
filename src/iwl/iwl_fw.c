/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * iwl_fw.c - the iwlwifi firmware file (.ucode, TLV format).
 *
 * Layout (Linux iwl-drv.c iwl_parse_tlv_firmware, OpenBSD iwm_read_firmware): a header
 *   ULONG zero; ULONG magic "IWL\n"; char human_readable[64]; ULONG ver; ULONG build; 8 bytes ignored
 * (88 bytes), then TLVs: ULONG type, ULONG length, data padded to 4. The sections of the INIT and regular
 * images are kept as offsets into the firmware blob (the file stays where "copy /b" put it). Each section starts
 * with the ULONG device load address, followed by the data.
 */
#include "iwl.h"

fwinfo_t g_fw;

#define HDR_LEN 88

static void setbits(UCHAR *map, USHORT idx, ULONG bits, USHORT maxbit)
{
   USHORT i, b;

   for (i = 0; i < 32; i++) {
      if (!(bits & 1UL)) {
         bits >>= 1;
         continue;
      }
      bits >>= 1;
      b = (USHORT)(idx * 32 + i);
      if (b < maxbit)
         map[b >> 3] |= (UCHAR)(1 << (b & 7));
   }
}

static int store_section(USHORT type, ULONG off, ULONG len)
{
   fwimg_t *im = &g_fw.img[type];

   if (len < 4 || im->nsec >= UCODE_SECT_MAX)
      return -1;
   im->sec[im->nsec].devoff = blob_rd32(&g_fwblob, off);
   im->sec[im->nsec].off = off + 4;
   im->sec[im->nsec].len = len - 4;
   im->nsec++;
   return 0;
}

/* "major.minor.api" as Linux prints it for firmware 35 and later: decimal, 8 hex digits, decimal */
static USHORT put_dec(char *s, USHORT v)
{
   char   t[6];
   USHORT n = 0, k = 0;

   do {
      t[n++] = (char)('0' + v % 10);
      v = (USHORT)(v / 10);
   } while (v && n < 5);
   while (n)
      s[k++] = t[--n];
   return k;
}

static void ver_str(ULONG major, ULONG minor, ULONG api)
{
   static char d[] = "0123456789abcdef";
   USHORT n = 0, i;
   char  *s = g_fw.version;
   U32    u;

   n = (USHORT)(n + put_dec(s + n, (USHORT)major));
   s[n++] = '.';
   u.l = minor;
   for (i = 0; i < 8; i++) {
      USHORT sh = (USHORT)(28 - i * 4);
      s[n++] = d[(USHORT)((sh >= 16 ? (u.w[1] >> (sh - 16)) : (u.w[0] >> sh)) & 15)];
   }
   s[n++] = '.';
   n = (USHORT)(n + put_dec(s + n, (USHORT)api));
   s[n] = 0;
}

int iwl_fw_parse(void)
{
   static UCHAR t[16];
   ULONG pos, len, type, tlen, v;
   USHORT idx;

   rt_setmem((PVOID)&g_fw, 0, sizeof(g_fw));
   g_fw.n_scan_channels = 40;                     /* IWM_DEFAULT_SCAN_CHANNELS                            */
   len = g_fwblob.len;
   if (len < HDR_LEN || blob_rd32(&g_fwblob, 0) != 0UL || blob_rd32(&g_fwblob, 4) != TLV_UCODE_MAGIC)
      return -1;
   v = blob_rd32(&g_fwblob, 72);                  /* ver: major 31..24, minor 23..16, api 15..8            */
   {
      U32 u;

      u.l = v;
      g_fw.major = u.b[3];
      ver_str((ULONG)u.b[3], (ULONG)u.b[2], (ULONG)u.b[1]);
   }
   pos = HDR_LEN;
   while (pos + 8 <= len) {
      type = blob_rd32(&g_fwblob, pos);
      tlen = blob_rd32(&g_fwblob, pos + 4);
      pos += 8;
      if (tlen > len - pos) {
         dd_err("firmware file truncated at TLV", type);
         return -2;
      }
      if (HI16(type)) {                           /* debug TLVs (0x1000000 and up): not used              */
         pos += (tlen + 3) & ~3UL;
         continue;
      }
      switch ((USHORT)type) {
      case UCODE_TLV_SEC_RT:
         if (store_section(UCODE_REGULAR, pos, tlen))
            return -3;
         break;
      case UCODE_TLV_SEC_INIT:
         if (store_section(UCODE_INIT, pos, tlen))
            return -3;
         break;
      case UCODE_TLV_FLAGS:
         if (tlen >= 4) g_fw.capaflags = blob_rd32(&g_fwblob, pos);
         break;
      case UCODE_TLV_PROBE_MAX_LEN:
         if (tlen >= 4) g_fw.max_probe_len = (USHORT)blob_rd32(&g_fwblob, pos);
         break;
      case UCODE_TLV_PHY_SKU:
         if (tlen == 4) g_fw.phy_config = blob_rd32(&g_fwblob, pos);
         break;
      case UCODE_TLV_DEF_CALIB:                   /* ULONG ucode_type, ULONG flow_trigger, ULONG event_trigger */
         if (tlen == 12) {
            ULONG ut = blob_rd32(&g_fwblob, pos);

            if (ut == 0UL || ut == 1UL) {
               g_fw.img[(USHORT)ut].flow_trigger = blob_rd32(&g_fwblob, pos + 4);
               g_fw.img[(USHORT)ut].event_trigger = blob_rd32(&g_fwblob, pos + 8);
            }
         }
         break;
      case UCODE_TLV_API_CHANGES_SET:             /* ULONG index, ULONG bits                                */
         if (tlen == 8) {
            idx = (USHORT)blob_rd32(&g_fwblob, pos);
            if (idx < 4)
               setbits(g_fw.api, idx, blob_rd32(&g_fwblob, pos + 4), 128);
         }
         break;
      case UCODE_TLV_ENABLED_CAPABILITIES:
         if (tlen == 8) {
            idx = (USHORT)blob_rd32(&g_fwblob, pos);
            if (idx < 4)
               setbits(g_fw.capa, idx, blob_rd32(&g_fwblob, pos + 4), 128);
         }
         break;
      case UCODE_TLV_N_SCAN_CHANNELS:
         if (tlen == 4) g_fw.n_scan_channels = (USHORT)blob_rd32(&g_fwblob, pos);
         break;
      case UCODE_TLV_PAGING:
         if (tlen == 4) {
            v = blob_rd32(&g_fwblob, pos);
            if (v > MAX_PAGING_IMAGE_SIZE || (v & (FW_PAGING_SIZE - 1))) {
               dd_err("firmware paging size not supported", v);
               return -4;
            }
            g_fw.img[UCODE_REGULAR].paging_size = v;
         }
         break;
      case UCODE_TLV_FW_VERSION:                  /* major, minor, local: newer than the header field       */
         if (tlen == 12) {
            g_fw.major = blob_rd32(&g_fwblob, pos);
            blob_read(&g_fwblob, pos, (UCHAR FAR *)t, 12);
            ver_str(get32((UCHAR FAR *)t), get32((UCHAR FAR *)t + 4), get32((UCHAR FAR *)t + 8));
         }
         break;
      default:                                    /* everything else (debug, WoWLAN, ...) is not needed      */
         break;
      }
      pos += (tlen + 3) & ~3UL;
   }
   if (!g_fw.img[UCODE_INIT].nsec || !g_fw.img[UCODE_REGULAR].nsec) {
      dd_err("firmware file has no INIT or runtime image", (ULONG)g_fw.img[UCODE_INIT].nsec);
      return -5;
   }
   if (g_fw.n_scan_channels > 52)
      g_fw.n_scan_channels = 52;
   return 0;
}
