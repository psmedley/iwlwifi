# iwlwifi for OS/2 and ArcaOS

A native driver for **Intel Wireless-AC 8260** PCIe WiFi adapters (`8086:24F3`, the "Wireless 8260" that lspci
reports) for OS/2 and ArcaOS, structured so that the other members of Linux's **iwlwifi** family can be added. It
has the same shape as the [QCA6174 driver](https://github.com/psmedley/qca6174) and the OS2World RTL drivers:

* a **GenMAC-compatible device** (`\DEV\WRND32$`, DosDevIOCtl category 99h OID GET/SET), so the ArcaOS
  **Wireless LAN Monitor** and `wpa_supplicant` can drive it, and
* an **NDIS 2.0.1 MAC** (`IWLWIFI_NIF`), so the OS/2 TCP/IP stack binds to it as `lanN`.

The chip side is a port of OpenBSD's **iwm(4)** (ISC licence), cross-checked line by line against Linux
**iwlwifi** (`pcie/`, `mvm/`, `fw/api/`). It is a 16-bit physical device driver built with Open Watcom 2.0 and
NASM; no IBM DDK is needed.

> **Status: first hardware results.** On an 8260 (`8086:24F3`, subsystem `8086:0010`, rev 3A) the driver loads and
> registers with NDIS, the firmware file is accepted, and bring-up gets through both firmware images (INIT and
> runtime, with paging), the NVM (MAC address, 26 channels, 2.4/5 GHz, 11n/11ac, LAR), calibration (15 PHY DB
> sections), DQA, the auxiliary station and the PHY context, with working interrupts. It then stopped on a firmware
> error at the power/temperature commands; see [docs/TESTING.md](docs/TESTING.md), "Hardware results". Scanning and
> joining have not been reached yet. A COM1 cable helps but is not required: the driver state can be read from
> `\DEV\WRND32$`.

## What it does

| Step | Code | Status |
|---|---|---|
| Find the card on the PCI bus (device table), map BAR0, DMA memory | `os2/pci.c`, `os2/mem.c`, `iwl/iwl_cfg.c` | written |
| Take `iwlwifi-8000C-36.ucode` written to `\DEV\WRND32$`, parse its TLVs | `os2/dbg.c`, `iwl/iwl_fw.c` | written, tested on the file |
| Power-up (APM), NIC init, receive ring, TX rings and scheduler, interrupt | `iwl/iwl_trans.c` | written |
| INIT firmware: secured CPU1/CPU2 download over the FH service channel, ALIVE | `iwl/iwl_trans.c`, `iwl/iwl_mvm.c` | written |
| NVM: MAC address, antennas, bands, channel list, LAR | `iwl/iwl_nvm.c` | written |
| Calibration: PHY configuration, PHY database kept for the runtime firmware | `iwl/iwl_mvm.c`, `iwl/iwl_phydb.c` | written |
| Runtime firmware, paging (CPU2 image in host memory), DQA queues, auxiliary station, PHY context, power, regulatory (MCC "ZZ"), UMAC scan configuration | `iwl/iwl_mvm.c`, `iwl/iwl_scan.c` | written |
| Scan (2.4 and 5 GHz, active, wildcard SSID), BSS list to the Wireless LAN Monitor | `iwl/iwl_scan.c`, `wlan/sta.c`, `os2/genmac.c` | written |
| Join: MAC context, binding, station, session protection, open authentication, association (HT capabilities, WMM), rate table, smart FIFO, quotas | `wlan/sta.c`, `iwl/iwl_mac.c` | written |
| WPA2-PSK: EAPOL queue to the supplicant, CCMP keys in the firmware (pairwise and group), CCMP header on transmit | `wlan/sta.c`, `iwl/iwl_mac.c` | written |
| Data: Ethernet <-> 802.11 (LLC/SNAP), TX commands on the BE queue, RX MPDUs decrypted by the firmware | `wlan/sta.c`, `iwl/iwl_mac.c` | written |

Not done (see [docs/NEXT.md](docs/NEXT.md)): 40/80 MHz channels, A-MPDU (block-ack requests are refused, so the
AP sends single MPDUs), host-side rate adaptation (the firmware only walks the rate table on retries), power save,
WPA3/TKIP/WEP, roaming. Other iwlwifi devices are listed in the device table but not driven yet: see
[docs/ADDING-A-DEVICE.md](docs/ADDING-A-DEVICE.md).

## Install (for testing)

1. Copy `bin\IWLWIFI.OS2` and `install\IWLWIFI.NIF` to `C:\IBMCOM\MACS`.
2. Copy `bin\firmware\iwlwifi-8000C-36.ucode` (from linux-firmware `intel/iwlwifi/`) to `C:\IBMCOM\MACS\IWLWIFI\`.
3. `CONFIG.SYS`: `DEVICE=C:\IBMCOM\MACS\IWLWIFI.OS2 /V` (only one `WRND32$` driver can be loaded: remove GenMAC,
   an RTL driver or the QCA6174 driver line).
4. `PROTOCOL.INI`: bind `IWLWIFI_NIF` as in `install\PROTOCOL.INI.example`.
5. `STARTUP.CMD`: the `copy /b` line from `install\STARTUP.CMD.example`.
6. Reboot. `copy \DEV\WRND32$ con` prints the driver state; `echo up > \DEV\WRND32$` retries a failed start and
   `echo scan > \DEV\WRND32$` scans, with progress on COM1. Once `STATE=0064 READY`, `IWLMAC.CMD` writes the
   adapter's MAC into PROTOCOL.INI.

## Building

Linux (cross): put Open Watcom 2.0 `binl64` and NASM on the `PATH`, set `WATCOM`, then `make -C src`.
OS/2: `build.cmd` in `src`. See [BUILD.md](BUILD.md).

## Layout

```
./
  src/os2/    OS/2 plumbing: device header, DevHelp glue, strategy routine, NDIS MAC, GenMAC OIDs, DMA memory, PCI
  src/wlan/   the chip-independent station (scan table, join, keys, EAPOL, Ethernet <-> 802.11) and hw.h, the
              contract a chip back end implements
  src/iwl/    the Intel iwlwifi back end: device table, firmware file, PCIe transport, the mvm firmware conversation
  install/    NIF, PROTOCOL.INI and STARTUP.CMD examples, IWLMAC.CMD
  bin/        the driver binary and the firmware file
  docs/       ARCHITECTURE, TESTING, ADDING-A-DEVICE, NEXT
```

It started inside the QCA6174 repository and shares its OS/2 plumbing.

## Licence

GPL-2.0-only (`COPYING`). The OS/2 plumbing derives from the OS2World RTL88EU driver (GPL-2.0-only) via the
QCA6174 driver; the chip code is ported from OpenBSD iwm (ISC) and checked against Linux iwlwifi (GPL-2.0 / BSD).
The firmware is Intel's, redistributed under `bin/firmware/LICENCE.iwlwifi_firmware`. See
[PROVENANCE.md](PROVENANCE.md).
