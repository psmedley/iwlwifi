# Next steps

In order of usefulness once the driver associates on hardware:

0. **Done after the first successful run:** two-stream (MCS 8-15) rates in the rate table when the AP supports them
   (`STREAMS=` in PROTOCOL.INI), 32 transmit slots instead of 16. Measure whether `STREAMS=1` or `2` is faster.

1. **Rate adaptation.** Today the firmware starts every frame at the top of the rate table and walks down on retries.
   Track `TXOK`/`TXFAIL` per rate from the TX responses (`tx_resp_t.initial_rate`, `failure_frame`) and rebuild the
   table (LQ_CMD) with a lower starting rate, as iwm's AMRR/RA or Linux `mvm/rs.c` do.
2. **A-MPDU receive: done** (host reorder buffer, `AMPDU=` in PROTOCOL.INI); **A-MPDU transmit** is next (ADDBA request
   to the AP, aggregation queues, block-ack handling): uploads are still one MPDU at a time.
3. **80 MHz** (40 MHz is done, `MAXWIDTH`): VHT
   capabilities in the association request, `STA_FLG_FAT_EN_*`, the rate flags.
4. **QoS data frames** and the AP's WMM parameters in the MAC context (EDCA per access category).
5. **ICT interrupt table** (fewer register reads per interrupt) and MSI, once the plain interrupt works.
6. **Power save** (POWER_TABLE / MAC_PM_POWER_TABLE with power save enabled) for laptops.
7. **7000 and 9000 families** (docs/ADDING-A-DEVICE.md).
8. **Firmware error dump**: read the error event table (`ERRTBL=`) through `HBUS_TARG_MEM_*` into the status text
   after a `SW_ERR`, as iwm_nic_error does.
