/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dbg.c - COM1 trace, small helpers, the device's read/write channel: firmware upload, status text, debug
 * commands. From qc_dbg.c of the QCA6174 driver (github.com/psmedley/qca6174).
 *
 * Writing to \DEV\WRND32$:
 *   - a stream whose first bytes the back end recognises (hw_fw_is_mine) is the firmware file; it is collected
 *     until the writer closes the device, then the adapter is started;
 *   - anything else is a debug command line: up, scan, join <ssid>, leave, r <hex> (results on COM1).
 * Reading \DEV\WRND32$ returns the driver state as KEY=VALUE lines, then end of file.
 */
#include "drv.h"

ULONG g_last_err;
char *g_last_err_txt = "";

/* ---- helpers ------------------------------------------------------------------------------------------ */
/* n / d by shift and subtract (no C runtime in a 16-bit driver: no 32-bit division helper) */
ULONG u32_div(ULONG n, ULONG d)
{
   ULONG q = 0, r = 0;
   USHORT i;

   if (!d)
      return 0;
   for (i = 0; i < 32; i++) {
      r = (r << 1) | ((n & 0x80000000UL) ? 1UL : 0UL);
      n <<= 1;
      q <<= 1;
      if (r >= d) {
         r -= d;
         q |= 1UL;
      }
   }
   return q;
}

void put16(UCHAR FAR *p, USHORT v) { p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); }
void put32(UCHAR FAR *p, ULONG v)  { U32 u; u.l = v; p[0] = u.b[0]; p[1] = u.b[1]; p[2] = u.b[2]; p[3] = u.b[3]; }
USHORT get16(UCHAR FAR *p)         { return (USHORT)(p[0] | (p[1] << 8)); }
ULONG get32(UCHAR FAR *p)          { U32 u; u.b[0] = p[0]; u.b[1] = p[1]; u.b[2] = p[2]; u.b[3] = p[3]; return u.l; }

void fmemcpy(UCHAR FAR *d, UCHAR FAR *s, USHORT n)
{
   if (n)
      rt_movmem((PVOID)d, (PVOID)s, n);
}

void fmemset(UCHAR FAR *d, UCHAR c, USHORT n)
{
   if (n)
      rt_setmem((PVOID)d, c, n);
}

int fmemeq(UCHAR FAR *a, UCHAR FAR *b, USHORT n)
{
   while (n--)
      if (*a++ != *b++)
         return 0;
   return 1;
}

/* ---- trace ------------------------------------------------------------------------------------------ */
void tstr(const char *s)
{
   while (*s)
      tcom(*s++);
}

void th8(UCHAR b)
{
   static char hx[] = "0123456789ABCDEF";

   tcom(hx[(b >> 4) & 0x0F]);
   tcom(hx[b & 0x0F]);
}

void th16(USHORT w) { th8((UCHAR)(w >> 8)); th8((UCHAR)w); }

void th32(ULONG v)
{
   U32 u;

   u.l = v;
   th16(u.w[1]);
   th16(u.w[0]);
}

void tnl(void) { tcom(13); tcom(10); }

void dd_err(const char *what, ULONG v)
{
   g_last_err_txt = (char *)what;
   g_last_err = v;
   tstr("[" DRV_NAME ": ");
   tstr(what);
   tcom(' ');
   th32(v);
   tcom(']');
   tnl();
}

/* ---- upload / command stream ------------------------------------------------------------------------ */
#define K_NONE  0
#define K_FW    1
#define K_TEXT  3

static USHORT g_wr_sfn = 0xFFFF, g_wr_kind, g_wr_err;
static char   g_cmd[82];
static USHORT g_cmdn;
static USHORT g_rd_sfn = 0xFFFF, g_rd_done;
static ULONG  g_dbg_val, g_dbg_addr;
static USHORT g_dbg_n;

static ULONG hexval(char *p)
{
   ULONG v = 0;

   while (*p == ' ') p++;
   for (;; p++) {
      char c = *p;
      USHORT d;

      if (c >= '0' && c <= '9') d = (USHORT)(c - '0');
      else if (c >= 'a' && c <= 'f') d = (USHORT)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = (USHORT)(c - 'A' + 10);
      else break;
      v = (v << 4) | d;
   }
   return v;
}

static void run_cmd(void)
{
   char *c = g_cmd;

   while (*c == ' ') c++;
   if (c[0] == 'u' && c[1] == 'p') {
      if (!g_hw_ready) { g_hw_failed = 0; hw_up(); }
   } else if (c[0] == 's' && c[1] == 'c') {
      if (g_hw_ready) hw_scan();
   } else if (c[0] == 'j' && c[1] == 'o') {                  /* "join <ssid>": an UNENCRYPTED network          */
      if (g_hw_ready) {
         char *q = c + 2;
         UCHAR n = 0;

         while (*q == ' ') q++;
         while (q[n] && n < 32) n++;
         sta_set_want((UCHAR FAR *)q, n, 1);
         tstr("[join rc "); th16((USHORT)(sta_join(0, 1) & 0xFFFF)); tstr(" fail "); th16(g_jfail); tcom(']'); tnl();
      }
   } else if (c[0] == 'l' && c[1] == 'e') {
      if (g_hw_ready) sta_leave();
   } else if (c[0] == 'r' && c[1] == ' ') {
      if (bar_map() == 0) {
         ULONG a = hexval(c + 2);

         g_dbg_addr = a;
         g_dbg_val = mmio_rd(a);
         g_dbg_n++;
         tstr("[r "); th32(a); tcom('='); th32(g_dbg_val); tcom(']'); tnl();
      }
   }
}

void dd_dev_open(USHORT sfn)
{
   (void)sfn;
}

void dd_dev_close(USHORT sfn)
{
   if (sfn == g_rd_sfn)
      g_rd_sfn = 0xFFFF;
   if (sfn != g_wr_sfn)
      return;
   nic_lock();
   if (g_wr_kind == K_FW) {
      g_fwblob.done = (USHORT)(g_wr_err ? 0 : 1);
      tstr("[fw ");
      th32(g_fwblob.len);
      tstr(g_wr_err ? " FAILED]" : " loaded]");
      tnl();
      /* The file is in: start the chip now, so the real MAC is known before the protocol binds. */
      if (!g_wr_err && !g_hw_ready && !g_hw_failed)
         hw_up();
   } else if (g_wr_kind == K_TEXT && g_cmdn) {
      g_cmd[g_cmdn] = 0;
      run_cmd();
   }
   g_wr_sfn = 0xFFFF;
   g_wr_kind = K_NONE;
   g_cmdn = 0;
   nic_unlock();
}

void dd_dev_write(USHORT sfn, ULONG phys, USHORT n)
{
   static UCHAR head[32];
   USHORT s, i;

   if (!n)
      return;
   nic_lock();
   if (sfn != g_wr_sfn) {                        /* first write of this handle: what is it?                */
      USHORT k = (n < sizeof(head)) ? n : (USHORT)sizeof(head);

      g_wr_sfn = sfn;
      g_wr_err = 0;
      g_cmdn = 0;
      rt_setmem((PVOID)head, 0, sizeof(head));
      s = scr_map(SCR_B, phys, k);
      if (s)
         rt_movmem((PVOID)head, MAKEP(s, 0), k);
      g_wr_kind = hw_fw_is_mine((UCHAR FAR *)head, k) ? K_FW : K_TEXT;
      if (g_wr_kind == K_FW) {
         if (g_hw_ready) {
            tstr("[upload ignored: the adapter is running]");
            g_wr_err = 1;
         } else
            blob_reset(&g_fwblob);
      }
   }
   if (g_wr_kind == K_FW) {
      if (!g_wr_err && blob_write(&g_fwblob, phys, n))
         g_wr_err = 1;
   } else {
      s = scr_map(SCR_B, phys, n);
      for (i = 0; s && i < n; i++) {
         char c = ((char FAR *)MAKEP(s, 0))[i];

         if (c == '\r' || c == '\n') {
            if (g_cmdn) {
               g_cmd[g_cmdn] = 0;
               run_cmd();
               g_cmdn = 0;
               s = scr_map(SCR_B, phys, n);      /* run_cmd may have used the scratch selector             */
            }
         } else if (g_cmdn < sizeof(g_cmd) - 1)
            g_cmd[g_cmdn++] = c;
      }
   }
   nic_unlock();
}

/* ---- status text ----------------------------------------------------------------------------------- */
static char   g_txt[4096];
static USHORT g_tn;

static void ps(const char *s) { while (*s && g_tn < sizeof(g_txt) - 1) g_txt[g_tn++] = *s++; }
static void pfs(char FAR *s, USHORT max) { while (max-- && *s && g_tn < sizeof(g_txt) - 1) g_txt[g_tn++] = *s++; }
static void ph(ULONG v, USHORT digits)
{
   static char hx[] = "0123456789ABCDEF";
   U32 u;
   USHORT i;

   u.l = v;
   for (i = digits; i > 0; i--) {
      USHORT sh = (USHORT)((i - 1) * 4);
      USHORT d  = (USHORT)((sh >= 16 ? (u.w[1] >> (sh - 16)) : (u.w[0] >> sh)) & 15);

      if (g_tn < sizeof(g_txt) - 1) g_txt[g_tn++] = hx[d];
   }
}
static void nl(void) { ps("\r\n"); }

extern USHORT g_hook_runs, g_lat_max, g_lat_n;
extern ULONG  g_lat_sum;
extern ULONG  g_oidlog[], g_oidlog_st[];
extern UCHAR  g_oidlog_set[];
extern USHORT g_oidlog_n, g_ioctl_n, g_bad_n, g_bad_cf;
USHORT ndis_state(USHORT what);

static void build_status(void)
{
   USHORT i;

   g_tn = 0;
   ps("DRIVER=" DRV_NAME " " DRV_VERSION " " DRV_BUILD); nl();
   ps("ADAPTER="); ps(hw_name()); nl();
   ps("PCI="); ph(g_pci_bdf, 4); ps(" ID="); ph(g_pci_ven, 4); ps(":"); ph(g_pci_dev, 4);
   ps(" SUB="); ph(g_pci_subven, 4); ps(":"); ph(g_pci_subdev, 4);
   ps(" REV="); ph(g_pci_rev, 2); ps(" IRQ="); ph(g_pci_irq, 2); ps(" BAR0="); ph(g_bar0, 8); nl();
   ps("FWFILE="); ps(hw_fw_name()); ps(" LEN="); ph(g_fwblob.len, 8); ps(g_fwblob.done ? " loaded" : " missing"); nl();
   ps("STATE="); ph(g_hw_state, 4); ps(g_hw_ready ? " READY" : (g_hw_failed ? " FAILED" : " DOWN")); nl();
   ps("MAC=");
   for (i = 0; i < 6; i++) { ph(g_mymac[i], 2); if (i < 5) ps(":"); }
   ps(g_cfgmac ? " (PROTOCOL.INI NETADDRESS)" : " (adapter)"); ps(" HWMAC=");
   for (i = 0; i < 6; i++) { ph(g_hw_mac[i], 2); if (i < 5) ps(":"); }
   nl();
   ps("NETS="); ph(g_nnet, 4); ps(" IRQS="); ph(g_irq_count, 4); ps(" SPUR="); ph(g_irq_spurious, 4);
   ps(" HOOK="); ph(g_hook_runs, 4); ps(" IRQHOOKED="); ph(g_irq_hooked, 1); ps(" BARWIN="); ph(g_nbarsel, 2); nl();
   ps("IOCTLS="); ph(g_ioctl_n, 4); ps(" OTHER="); ph(g_bad_n, 4); ps(" LASTOTHER(cat/func)="); ph(g_bad_cf, 4);
   ps(" LANNUM="); ph(g_lannum, 2); nl();
   {
      USHORT n = g_oidlog_n, first = (n > 16) ? (USHORT)(n - 16) : 0, k;

      for (k = first; k < n; k++) {                       /* oldest first                                     */
         ps(g_oidlog_set[k % 16] ? "SET " : "GET "); ph(g_oidlog[k % 16], 8);
         ps(" -> "); ph(g_oidlog_st[k % 16], 8); nl();
      }
   }
   ps("LASTERR="); ps(g_last_err_txt); ps(" "); ph(g_last_err, 8); nl();
   ps("DBG=");  ph(g_dbg_n, 4); ps(" ADDR="); ph(g_dbg_addr, 8); ps(" VAL="); ph(g_dbg_val, 8); nl();
   ps("LINK="); ps(g_joined ? "JOINED " : "none "); ph(g_jch, 2); ps(" AID="); ph(g_jaid, 4);
   ps(" JFAIL="); ph(g_jfail, 2); ps(g_open_net ? " OPEN" : " RSN"); ps(g_ptk_on ? " PTK" : " nokey");
   ps(g_authorized ? " AUTHORIZED" : ""); ps(g_jht ? " HT" : " legacy"); nl();
   ps("DATA rx="); ph(g_rx_data, 4); ps(" ndis="); ph(g_rx_ndis, 4); ps(" eapol="); ph(g_rx_eapol, 4);
   ps(" nobuf="); ph(g_rx_nobuf, 4); ps(" drop="); for (i = 0; i < 8; i++) { ph(g_rx_drop[i], 4); ps(i < 7 ? "," : ""); }
   ps(" tx="); ph(g_tx_data, 4); ps(" txbusy="); ph(g_tx_busy, 4); ps(" EAPOLQ="); ph(sta_eapol_count(), 2); nl();
   ps("NDIS reg="); ph(ndis_state(8), 1); ps(" status="); ph(ndis_state(0), 4); ps(" filter="); ph(ndis_state(1), 4);
   ps(" rxbufs="); ph(ndis_state(6), 2); ps(" txbuf="); ph(ndis_state(7), 1); ps(" tx="); ph(ndis_state(2), 4);
   ps(" txhw="); ph(ndis_state(3), 4); ps(" rx="); ph(ndis_state(4), 4); ps(" nobuf="); ph(ndis_state(5), 4); nl();
   ps("MGMTRX(from the AP, subtype 0-15; 1 assoc resp, 11 auth, 12 deauth, 10 disassoc, 13 action)=");
   for (i = 0; i < 16; i++) { ph(g_rx_mgmt[i], 2); ps(i < 15 ? "," : ""); }
   nl();
   ps("LAT irq2hook max="); ph(g_lat_max, 4); ps("ms n="); ph(g_lat_n, 4); nl();
   hw_status(ps, ph, nl);                                  /* the back end's own lines                         */
   for (i = 0; i < g_nnet; i++) {
      ps("NET="); ph(g_net[i].ch, 2); ps(" ");
      ph((ULONG)(UCHAR)g_net[i].rssi, 2); ps(g_net[i].htc ? " HT " : " -- ");
      pfs((char FAR *)g_net[i].ssid, g_net[i].len); nl();
   }
}

USHORT dd_dev_read(USHORT sfn, ULONG phys, USHORT n)
{
   USHORT s, k;

   if (sfn == g_rd_sfn && g_rd_done)
      return 0;                                   /* second read of the same open: end of file             */
   g_rd_sfn = sfn;
   g_rd_done = 1;
   nic_lock();
   build_status();
   k = (g_tn < n) ? g_tn : n;
   s = scr_map(SCR_B, phys, k);
   if (s)
      rt_movmem(MAKEP(s, 0), (PVOID)g_txt, k);
   nic_unlock();
   return s ? k : 0;
}
