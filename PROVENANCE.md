# Provenance

| Part | Source | Licence |
|---|---|---|
| `src/os2/*` (device header, DevHelp glue, strategy routine, NDIS MAC, GenMAC OIDs, DMA memory, PCI, status text) | the [QCA6174 driver](https://github.com/psmedley/qca6174), which took it from OS2World [DRV-NET-RTLUSBWifi](https://github.com/OS2World/DRV-NET-RTLUSBWifi) (RTL88EU / RTL8821AU drivers), written from the IBM PDD Reference and the NDIS 2.0.1 specification | GPL-2.0-only |
| `src/wlan/sta.c` (scan table, join, frames, EAPOL queue) | the QCA6174 driver's `src/ath_sta.c` (itself following the RTL8821AU driver for the frames and the supplicant behaviour) | GPL-2.0-only |
| Register addresses, command numbers, firmware structure layouts (`src/iwl/iwl.h`), the bring-up, transport, scan, join, key and data sequences (`src/iwl/*.c`) | OpenBSD `sys/dev/pci/if_iwm.c`, `if_iwmreg.h`, `if_iwmvar.h` (Copyright Stefan Sperling, Damien Bergamini, Intel and others) | ISC (if_iwmreg.h also carries Intel's dual BSD/GPL notice) |
| Cross-checks: group-key installation, NVM chunk reading with offsets, paging scope, L0s bit, legacy RX crypto (MIC left in place) | Linux `drivers/net/wireless/intel/iwlwifi` (`pcie/trans.c`, `mvm/sta.c`, `mvm/nvm.c`, `mvm/rx.c`, `mvm/fw.c`, `fw/paging.c`, `iwl-drv.c`, `cfg/8000.c`, `pcie/drv.c`) | GPL-2.0 / BSD-3-Clause |
| PCI ids and family assignments | Linux `pcie/drv.c`, OpenBSD `iwm_attach` | - |
| Structure sizes asserted in `iwl_cfg.c` | computed by compiling OpenBSD's definitions with gcc | - |
| `bin/firmware/iwlwifi-9000-pu-b0-jf-b0-46.ucode` | linux-firmware `intel/iwlwifi/iwlwifi-9000-pu-b0-jf-b0-46.ucode` (SHA-256 `9a0c5d938646942f9798e6212579c25771e4bb44301cd409b82abe361ca2c3e0`) | Intel, `bin/firmware/LICENCE.iwlwifi_firmware` |
| `bin/firmware/iwlwifi-7265D-29.ucode`, `iwlwifi-7265-17.ucode` | linux-firmware `intel/iwlwifi/` (SHA-256 `c52bcaa0...617e` and `91f69930...e870`) | Intel, `bin/firmware/LICENCE.iwlwifi_firmware` |
| `bin/firmware/iwlwifi-cc-a0-50.ucode` | linux-firmware `intel/iwlwifi/iwlwifi-cc-a0-50.ucode` (SHA-256 `c0e3e205...0177`) | Intel, `bin/firmware/LICENCE.iwlwifi_firmware` |
| `bin/firmware/iwlwifi-cc-a0-77.ucode` | linux-firmware `intel/iwlwifi/iwlwifi-cc-a0-77.ucode` (SHA-256 `19db22bb...d335`) | Intel, `bin/firmware/LICENCE.iwlwifi_firmware` |
| `bin/firmware/iwlwifi-8000C-36.ucode` | linux-firmware `intel/iwlwifi/iwlwifi-8000C-36.ucode` (SHA-256 `479931721f5e168d69d67c297c11738acd75da390adb94f96030a1055f4cf57a`) | Intel, `bin/firmware/LICENCE.iwlwifi_firmware` |

The combined work is distributed under GPL-2.0-only (`COPYING`); the ISC-licensed iwm parts are compatible with it.
The firmware is not part of the GPL work: it is Intel's binary, redistributed unmodified under Intel's licence.
