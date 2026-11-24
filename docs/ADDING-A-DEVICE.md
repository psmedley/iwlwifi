# Adding iwlwifi devices

The driver is organised like Linux iwlwifi so that the rest of the family can follow the 8260:

| Layer | Linux | Here | Per device / family? |
|---|---|---|---|
| device table, configuration | `pcie/drv.c`, `cfg/7000.c`, `cfg/8000.c`, `cfg/9000.c`, ... | `iwl/iwl_cfg.c` | one line per PCI id; one `iwl_cfg_t` per family/firmware |
| firmware file | `iwl-drv.c` | `iwl/iwl_fw.c` | common to every `.ucode` (TLV) file |
| PCIe transport | `pcie/trans.c`, `rx.c`, `tx.c` (gen1) / `*-gen2.c` | `iwl/iwl_trans.c` | family tests (`g_cfg->family`) |
| operation mode | `mvm/` (7000-9000, AX2xx) / `mld/` (newest) | `iwl/iwl_mvm.c`, `iwl_nvm.c`, `iwl_phydb.c`, `iwl_scan.c`, `iwl_mac.c` | capability/API bits from the firmware file (`FW_CAPA`, `FW_API`) |

## Same family (8000): a new PCI id

Add a line to `g_devs[]` in `iwl/iwl_cfg.c`:

```c
{ 0x24F3, 0x0050, &cfg_8260, "Intel(R) Dual Band Wireless-N 8260" },
```

`subdev` `0xFFFF` matches any subsystem; more specific lines go first. The 8265 (`24FD`) is listed already and uses
`iwlwifi-8265-36.ucode`; it is the same family (iwm treats it as 8000) and only needs testing.

## 7000 family (7260, 3160, 7265, 3165, 3168)

Listed in `g_devs[]` with `supported = 0`. What is missing (all in iwm, `sc_device_family == IWM_DEVICE_FAMILY_7000`):

1. **Firmware download** without the secure CPU1/CPU2 protocol: `iwm_load_firmware_7000` (each section with
   `iwm_firmware_load_sect`, then `CSR_RESET = 0` to start the CPU). `iwl_load_ucode()` returns -2 for it now.
2. **NVM layout** (`IWL_NVM` / `IWL_NVM_SDP`): MAC in the HW section (byte-swapped words), SKU/radio in the SW
   section, channels at `IWM_NVM_CHANNELS` of SW (or the SDP regulatory section for the 3168). `iwl_nvm_init()`
   returns -1 for it now.
3. **APM**: the 7000 parts of `apm_init`, `stop_device` and `nic_config` are already written (APMG clocks, L1-active,
   early power-off work-around), as is `host_int_oper` (interrupt coalescing work-around of the 7260/3160) in `rx_init`.
4. Bluetooth coexistence is sent to the INIT firmware too (done: `send_bt_init` for family < 8000).
5. Firmware without DQA (`-17` images): the legacy queue numbers are already handled (`g_cmdq` 9, aux 15, data 0-3).
6. Older firmware may lack the UMAC scan: the LMAC scan (`iwm_lmac_scan`) would have to be ported.

## 9000 family (9260, 9461, 9462, 9560)

The 9000 family uses **multi-queue receive** (`IWL_UCODE_TLV_CAPA_MULTI_QUEUE_RX_SUPPORT`, iwm `sc_mqrx_supported`):
a different receive ring (64-bit free descriptors, a used-descriptor ring, `RFH_*` registers) and a different RX
packet (`iwm_rx_mpdu_mq`, `struct iwm_rx_mpdu_desc`). Also: `iwm_clear_persistence_bit` in `start_hw`, antenna B for
single-stream rates, the SOC configuration command for integrated parts, and the 9000 firmware's scan/command
versions (`IWL_UCODE_TLV_CMD_VERSIONS`). The transmit side (TFD format) is unchanged.

## 22000 / AX200 and later

These use the gen2 transport (context info, a new TFD format, the TX command v2) and newer firmware APIs; they are a
separate transport file next to `iwl_trans.c` and a larger porting job (Linux `pcie/*-gen2.c`, `ctxt-info*.c`).

## Rules

* Firmware structures go into `iwl/iwl.h`, packed, with a size check in `iwl_cfg.c` (compute the expected size by
  compiling the iwm or Linux definition with gcc, as was done for the existing ones).
* Choose structure versions from the firmware's API/capability bits, never from the device id alone.
* Mark every family-specific path with `g_cfg->family`, so that a search for it shows what a new family touches.
