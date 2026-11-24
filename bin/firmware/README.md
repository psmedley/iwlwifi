# Firmware

The driver needs one file, copied to `\DEV\WRND32$` at every boot (`install/STARTUP.CMD.example`):

| Adapter | File | Source |
|---|---|---|
| Intel Wireless-AC 8260 (`8086:24F3`, `24F4`, `24F5`, `24F6`) | `iwlwifi-8000C-36.ucode` (2,425,084 bytes, firmware 36.c8e8e144.0) | linux-firmware `intel/iwlwifi/` |
| Intel Wireless-AC 8265 (`8086:24FD`), experimental | `iwlwifi-8265-36.ucode` (not included) | linux-firmware `intel/iwlwifi/` |

The file is Intel's, redistributed unmodified under `LICENCE.iwlwifi_firmware` (from linux-firmware); read that
licence before redistributing it further. Older 8000C files (`-34`, `-27`, ...) should work too but have not been
looked at; the driver chooses command versions from the file's API/capability bits.
