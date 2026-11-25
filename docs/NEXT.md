# Next steps

In order of usefulness once the driver associates on hardware:

0. **Done after the first successful run:** two-stream (MCS 8-15) rates in the rate table when the AP supports them
   (`STREAMS=` in PROTOCOL.INI), 32 transmit slots instead of 16. Measure whether `STREAMS=1` or `2` is faster.

1. **Rate adaptation.** Today the firmware starts every frame at the top of the rate table and walks down on retries.
   Track `TXOK`/`TXFAIL` per rate from the TX responses (`tx_resp_t.initial_rate`, `failure_frame`) and rebuild the
   table (LQ_CMD) with a lower starting rate, as iwm's AMRR/RA or Linux `mvm/rs.c` do.
2. **A-MPDU receive: done** (host reorder buffer, `AMPDU=` in PROTOCOL.INI). **A-MPDU transmit: written, awaiting
   hardware results** (`AGGTX=`; TID 0 only, `src/iwl/iwl_agg.c`; see docs/TESTING.md 5d).
3. **80 MHz: written, awaiting hardware results** (`MAXWIDTH=40` turns it off): VHT
   capabilities in the association request, `STA_FLG_FAT_EN_*`, the rate flags.
4. **QoS data frames** and the AP's WMM parameters in the MAC context (EDCA per access category).
5. **ICT interrupt table** (fewer register reads per interrupt) and MSI, once the plain interrupt works.
6. **Power save** (POWER_TABLE / MAC_PM_POWER_TABLE with power save enabled) for laptops.
7. **7000 and 9000 families** (docs/ADDING-A-DEVICE.md).
8. **Firmware error dump**: read the error event table (`ERRTBL=`) through `HBUS_TARG_MEM_*` into the status text
   after a `SW_ERR`, as iwm_nic_error does.

## 9000 family (Intel Wireless-AC 9560, `8086:A370`, CNVi) - plan

The 9560 on the author's second machine is `8086:A370` (Cannon Lake PCH CNVi). Likely firmware:
`iwlwifi-9000-pu-b0-jf-b0-46.ucode` (version 46, 1,519,568 bytes; SHA-256 9a0c5d93...c3e0), to be confirmed from
`dmesg | grep iwlwifi` on Linux (which file it loads, the hardware revision). What differs from the 8000 family, using
Linux 5.4 `pcie/ctxt-info.c`, `pcie/rx.c`, `pcie/trans.c` as the reference:
1. Stage 1, boot: firmware loaded through the context-info structure (firmware sections placed in DRAM, an image-loader
   TLV, the card started through CSR/RFH registers), multi-queue receive (RFH registers, "used" and "free" rings), CNVi reset
   sequence. Goal: `STATE=READY`, firmware version in the status.
2. Stage 2, scan: command formats of the 9000 firmware (PHY context, scan, TX flags).
3. Stage 3, join (WPA2): station layer unchanged.
4. Stage 4, speed: aggregation, VHT, rate adaptation, with the multi-queue receive path.
Until stage 1 exists the device table lists the IDs as "not supported yet" and the driver ignores them.
