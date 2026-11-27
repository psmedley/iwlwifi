# Firmware

The driver needs one file, copied to `\DEV\WRND32$` at every boot (`install/STARTUP.CMD.example`):

| Adapter | File | Source |
|---|---|---|
| Intel Wireless-AC 8260 (`8086:24F3`, `24F4`, `24F5`, `24F6`) | `iwlwifi-8000C-36.ucode` (2,425,084 bytes, firmware 36.c8e8e144.0) | linux-firmware `intel/iwlwifi/` |
| Intel Wireless-AC 9560 (`8086:A370`, subsystem `4030`, CNVi), not driven yet | `iwlwifi-9000-pu-b0-jf-b0-46.ucode` (1,519,568 bytes, firmware 46.7e3e4b69.0, the file Linux loads on this card) | linux-firmware `intel/iwlwifi/` |
| Intel Wireless 7265 / 3165 (`8086:095A`, `095B`, `3165`, `3166`), experimental | `iwlwifi-7265D-29.ucode` (1,036,312 bytes) for the D step (most cards); `iwlwifi-7265-17.ucode` (1,180,412 bytes) for the older B/C steps | linux-firmware `intel/iwlwifi/` |
| Intel Wi-Fi 6 AX200 / Killer AX1650w (`8086:2723`), in development | `iwlwifi-cc-a0-50.ucode` (1,101,228 bytes; the newest firmware API of Linux 5.4, which the code is based on; `iwlwifi-cc-a0-77.ucode` boots too but needs newer command versions) / `iwlwifi-cc-a0-77.ucode` (1,368,100 bytes, firmware 77.8dbafb52.0, the file Linux loads on the author's laptop; uncompressed: Linux ships it as `.ucode.zst`) | linux-firmware `intel/iwlwifi/` |
| Intel Wireless-AC 8265 (`8086:24FD`), experimental | `iwlwifi-8265-36.ucode` (2,432,528 bytes, firmware 36.c8e8e144.0, same release as the 8260's file) | linux-firmware `intel/iwlwifi/` |

The file is Intel's, redistributed unmodified under `LICENCE.iwlwifi_firmware` (from linux-firmware); read that
licence before redistributing it further. Older 8000C files (`-34`, `-27`, ...) should work too but have not been
looked at; the driver chooses command versions from the file's API/capability bits.
