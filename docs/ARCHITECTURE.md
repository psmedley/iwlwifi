# Architecture - iwlwifi for OS/2

## 1. Big picture

```
 Wireless LAN Monitor / wpa_supplicant              TCP/IP (NDIS 2.0.1 protocol)
        | DosDevIOCtl cat 99h (GenMAC OIDs)                 | TransmitChain / ReceiveChain
        v                                                    v
   os2/genmac.c (OID layer)                          os2/ndis.c (MAC tables, indications)
        \                                                   /
         +------------- wlan/sta.c: scan table, join, keys, EAPOL, Ethernet <-> 802.11 ---------+
                               |  wlan/hw.h (the back-end contract)
         +---------------------+------------------------------------------------------------+
         | iwl/iwl_mac.c  MAC context, binding, station, time event, rate table, keys, TX/RX  |
         | iwl/iwl_scan.c UMAC scan            iwl/iwl_mvm.c  bring-up, notifications, status |
         | iwl/iwl_nvm.c  NVM                  iwl/iwl_phydb.c calibration results            |
         | iwl/iwl_fw.c   .ucode TLV file      iwl/iwl_cfg.c  device table                    |
         | iwl/iwl_trans.c PCIe transport: APM, rings, scheduler, firmware DMA, IRQ, commands  |
         +---------------------+------------------------------------------------------------+
                               |
                os2/pci.c (PCI config, BAR0 MMIO, IRQ entry, context hook)   os2/mem.c (DMA memory, blob)
                               |
                     Intel Wireless-AC 8260 (PCIe)
```

One 16-bit PDD, character device `WRND32$`, the container and segment layout of the RTL88EU / QCA6174 drivers
(NE "dll", IOPL code segment, header first, resident-end markers last).

The split is the one Linux has: `os2/` + `wlan/` play the parts of the OS and of mac80211, `iwl/iwl_trans.c` is
iwlwifi's `pcie/` transport, the other `iwl/` files are the `mvm` operation mode. A different chip family would be a
second back end implementing `wlan/hw.h`; another iwlwifi device is a line in `iwl/iwl_cfg.c` plus whatever its
family needs (docs/ADDING-A-DEVICE.md).

## 2. Source map

| File | Role | Origin |
|---|---|---|
| `os2/hdr.asm`, `os2/end.asm`, `os2/ndisx.asm`, `os2/lib.asm` | device header, segment order, strategy entry; NDIS stubs; DevHelp wrappers, SMP locks, 32-bit MMIO/port access, SetIRQ/EOI, interrupt entry; **new:** 8-bit MMIO write | QCA6174 driver (from RTL88EU) |
| `os2/strat.c`, `os2/ndis.c/.h`, `os2/genmac.c` | strategy routine, INIT; NDIS 2.0.1 MAC; GenMAC OIDs | QCA6174 driver, names made generic |
| `os2/pci.c` | PCI configuration, BAR0 windows, interrupt entry and context hook, all chip work delegated to `hw_*` | QCA6174 `qc_pci.c`, generalised |
| `os2/mem.c` | GDT selectors, DMA chunks (**new:** physically aligned allocations), scratch mappings, firmware blob (4 MB) | QCA6174 `qc_mem.c` |
| `os2/dbg.c` | COM1 trace, helpers (`u32_div`, `get32`...), firmware upload (the back end recognises its file), status text, debug commands | QCA6174 `qc_dbg.c` |
| `wlan/sta.c` | the station: scan table, authentication/association frames, ADDBA refusal, keys, EAPOL queue, Ethernet <-> 802.11 | QCA6174 `ath_sta.c`, made chip-independent |
| `iwl/iwl_cfg.c` | device table, family configurations, identity, structure-size checks | Linux `pcie/drv.c`, `cfg/*.c`; iwm_attach |
| `iwl/iwl_fw.c` | the `.ucode` TLV file | iwm_read_firmware, Linux `iwl-drv.c` |
| `iwl/iwl_trans.c` | PCIe gen1 transport | iwm (see the file head), Linux `pcie/trans.c`, `rx.c`, `tx.c` |
| `iwl/iwl_mvm.c` | bring-up, paging, global commands, notification dispatch, status | iwm_init_hw & co, Linux `mvm/fw.c`, `mvm/ops.c`, `fw/paging.c` |
| `iwl/iwl_nvm.c` | NVM read and 8000-family parse | Linux `mvm/nvm.c`, iwm_parse_nvm_data |
| `iwl/iwl_phydb.c` | PHY database | iwm_phy_db_*, Linux `iwl-phy-db.c` |
| `iwl/iwl_scan.c` | UMAC scan | iwm_config_umac_scan, iwm_umac_scan |
| `iwl/iwl_mac.c` | join objects, keys, TX command, RX MPDU | iwm_auth/run/deauth/tx/rx_mpdu, Linux `mvm/sta.c` for keys |

## 3. Memory

* **BAR0** of the 8260 is 8 KB; one 64 KB GDT window maps it. Every register access is one 32-bit bus cycle
  (`dd_rd32`/`dd_wr32`); `CSR_INT_COALESCING` and `CSR_INT_PERIODIC_REG` take 8-bit writes (`dd_wr8`), as in iwm.
* **NIC access**: registers outside the CSR block (PRPH, SRAM) are reached with `MAC_ACCESS_REQ` held
  (`iwl_nic_lock`, counted as in iwm).
* **DMA memory** (24 chunks of 64 KB with permanent selectors, `dma_get_al` aligns the *physical* address):
  | What | Size | Alignment |
  |---|---|---|
  | TFD rings: command queue, aux queue, four access-category queues | 6 x 32 KB (256 TFDs of 128 bytes) | 256 |
  | scheduler byte-count tables (31 queues) | 19840 bytes | 1024 |
  | keep-warm page | 4 KB | 4096 |
  | receive descriptor ring (256 x 4 bytes), receive status | 1 KB, 16 bytes | 256, 16 |
  | command slots (32 x 324) + one 4 KB buffer for long commands + response copy | 10 KB + 8 KB | 16 |
  | data transmit slots (BE queue) | 16 x 2 KB | 64 |
  | firmware download bounce buffer | 32 KB | 16 |
  | NVM section buffer, PHY database, scan command | 4 + 16 + 2 KB | 8 |
* **Receive buffers**: 64 buffers of 4 KB in four separate 64 KB AllocPhys blocks, read through the scratch
  selector `SCR_RX`. They are posted in the 256-entry descriptor ring; descriptor *i* always holds buffer *i mod 64*
  and a used buffer goes back 64 descriptors ahead (Linux posts what it has the same way).
* **Firmware paging**: the runtime firmware keeps part of its CPU2 image in host memory: one 4 KB CSS block and
  5 blocks of 32 KB for `iwlwifi-8000C-36` (155,648 bytes), 4 KB aligned, separate AllocPhys blocks.
* **The firmware file** (2.4 MB) is kept as a list of 64 KB physical blocks (`BLOB`), read through scratch selectors.
* DGROUP is about 31 KB; the code segment about 39 KB.

## 4. Execution contexts

* **Strategy routine (task time)**: GenMAC requests and device reads/writes hold `nic_lock` (ProcBlock-based).
  Bring-up, commands, scans and joins run here and block in `hw_wait()` (ProcBlock 2 ms between polls).
* **Interrupt** (`dd_irq_c` -> `hw_irq`): reads `CSR_INT`; when a cause we enabled is set, masks the device
  (`CSR_INT_MASK = 0`), issues the EOI, arms the context hook. A clear cause = "not mine" (shared line).
* **Context hook** (task time, never blocks): `hw_service()` (ack causes, drain the receive ring), unmask, deliver
  received frames to NDIS.
* `hw_service()` is guarded by an owner-tagged lock; a caller that finds it busy leaves a note and the owner runs
  once more. Every wait also services, so the driver works without an interrupt (polled).
* **Transmit**: NDIS TransmitChain can arrive at interrupt time on any CPU. The station's frame building
  (`wlan/sta.c`) and the data queue (slot allocation in `iwl_mac.c`, reclaim on the TX response) are each guarded by
  interrupts-off + an owner-tagged spin lock; re-entry on the same CPU answers "busy".
* **Commands** are synchronous: one at a time from the request path; the answer is matched by queue index.
* There is **no ICT table**: the interrupt causes are read from `CSR_INT` / `CSR_FH_INT_STATUS`, the mode the
  hardware starts in. (iwm and Linux switch to ICT after ALIVE; that is an optimisation of the interrupt handler.)

## 5. Bring-up (`hw_up`, `g_hw_state` = step in the status text, hex)

| STATE | Step |
|---|---|
| 1 | firmware file present |
| 2 | TLV parse (`FWVER=`, `INITSEC=`, `RTSEC=`, `PAGING=`, `API=`, `CAPA=`) |
| 3 | PCI: memory + bus master, INTx on, ASPM off, retry timeout (config 41h) 0 |
| 4 | BAR0 mapped |
| 5 | DMA memory for rings and buffers |
| 6 | hardware revision (8000: new layout, C step from AUX_MISC_REG), interrupt hooked (or polled) |
| 7 | `start_hw`: card ready (NIC_READY handshake), software reset, APM init (MAC clock) |
| 8 | INIT firmware: NIC init (receive ring, TX rings, shadow registers), secured download CPU1 then CPU2 over the FH service channel (32 KB pieces, `FH_UCODE_LOAD_STATUS` bits), ALIVE |
| 10 | post-ALIVE: scheduler SRAM cleared, scheduler DRAM base, command queue on FIFO 7, scheduler on, DMA channels on |
| 11 | NVM read: HW_8000, MAC_OVERRIDE, SW, PHY_SKU, REGULATORY |
| 12 | smart FIFO off, TX antennas, PHY configuration: the firmware calibrates and sends PHY DB sections, then INIT_COMPLETE |
| 20 | device stopped and restarted |
| 21 | runtime firmware download, ALIVE |
| 22 | post-ALIVE |
| 23 | paging blocks filled, FW_PAGING_BLOCK_CMD |
| 24 | TX antennas, PHY DB (CFG, CALIB_NCH, PAPD groups, TXP groups), PHY configuration, BT coexistence |
| 25 | DQA enable (command queue 0), auxiliary station on queue 1 |
| 26 | PHY context 0 added, empty temperature thresholds (firmware does CT-kill), power table (constantly awake) |
| 27 | MCC update "ZZ" (LAR), UMAC scan configuration (channels from the NVM) |
| 64 | data queues 5-8 enabled (DQA), beacon filter off: **READY** |

## 6. GenMAC interface

As in the RTL and QCA6174 drivers: IOCtl category 99h, functions 40h/41h, parameter packet of ULONGs (OID, length,
NDIS status, bytes, NDIS status again); private OIDs `FFFFFF00` (lanN), `01` (wireless), `02` (link), `08` (NIF
name), `09` (GenMAC version >= 2.00), `0B`..`0E` (EAPOL). The supplicant's re-join trick (leave, then join from a
request 1.5 s later) is kept.

## 7. Join, keys, data

`wlan/sta.c` runs the station side of the 802.11 state machine (mac80211's job on Linux); the firmware objects come
from `iwl/iwl_mac.c` in the order iwm uses:

1. **hw_join_prepare**: PHY context 0 to the AP's channel (always 20 MHz while authenticating and associating), MAC context (station type, BSSID, ACK rates from
   the beacon's basic rates, default EDCA, beacons passed), binding (MAC 0 to PHY 0), station 0 (the AP) on queues
   5-8, a time event (up to 1000 TU) so that the firmware stays on the channel.
2. **authentication** (open system) and **association request** (rates, RSN WPA2-PSK/CCMP, HT capabilities when the
   AP has them: short GI, MCS 0-15 on a 2x2 radio, 40 MHz when used (below); WMM information when the AP has WMM) are sent as
   management frames on queue 5 at the lowest basic rate; the answers arrive as received frames.
3. **hw_join_complete**: PHY context with two chains (HT, 2x2) and, when 40 MHz is used, the wide channel;
   40 MHz is used when MAXWIDTH allows it, the AP's beacon has the HT capabilities "40 MHz supported" bit and an HT
   operation element with "STA channel width" set and a secondary channel above or below, the HT capabilities of the
   association response confirm HT, and the NVM's regulatory data allows 40 MHz on the primary channel
   (`hw_chan_ht40`). The PHY context then carries width 40 and the control channel position (secondary above: control
   below, secondary below: control above), the station gets `FAT_EN_40MHZ`, and the first two rate-table entries carry
   the 40 MHz flag, RTS protection and the AP's 40 MHz short-GI setting (later entries fall back to 20 MHz, as iwm
   does), station update (MIMO2 when the AP has two streams),
   MAC context with the association data (AID, beacon interval, DTIM, TSF of the last beacon), smart FIFO on, multicast
   filter "pass all", power (constantly awake, keep-alive), time quota (all of it to binding 0), rate table, the time
   event removed.
4. **keys**: ADD_STA_KEY (v1, 64 bytes, for this firmware) with CCM; pairwise at key offset 0, group keys at
   1 + key index with the multicast flag. The firmware decrypts received frames (status MIC_OK); the transmit side puts
   the CCMP header (packet number) in front of the payload and the pairwise key into the TX command.
5. **data**: non-QoS data frames, To-DS, LLC/SNAP, sequence numbers from the driver (`TX_CMD_FLG_SEQ_CTL`), the rate
   from the rate table (`STA_RATE`). Received QoS and non-QoS data frames are converted to Ethernet; the CCMP header and
   the 8-byte MIC the firmware leaves in place are removed. EAPOL goes to the supplicant's queue.
6. **ADDBA requests** from the AP are refused (status 37), so it sends single MPDUs.
7. **leave**: deauthentication, smart FIFO off, quota 0, MAC context unassociated, time event removed, station removed,
   binding removed, MAC context removed, PHY context back to the first channel.

## 8. Open questions - to check on hardware

These are the places where the code rests on reading iwm/iwlwifi with nothing to test it against yet.

1. **Interrupt handler contract and IRQ > 15** - as in the QCA6174 driver (its ARCHITECTURE.md, section 7). The driver
   falls back to polling (`IRQHOOKED=0`, `[iwl: no interrupt: polled]`).
2. **Receive ring with 64 of 256 descriptors posted.** If `RXPKT=` stays 0 after a firmware download (no ALIVE), look at
   `FH_RSCSR_CHNL0_WPTR` handling in `rx_init`/`rx_service` first.
3. **No ICT.** The hardware starts in this mode and iwm/iwlwifi use it until ALIVE; whether every later cause also
   lands in `CSR_INT` without ICT is to be confirmed (watch `IRQS=` and `SVC=`).
4. **Commands in one transfer buffer**, as iwm does; Linux copies the first 20 bytes into a separate buffer.
5. **Group keys installed in the firmware.** iwm does group CCMP in software; Linux installs them with the multicast
   flag as done here. If unicast works and broadcast (ARP, DHCP) does not, look here (`DECFAIL=`).
6. **The MIC on hardware-decrypted frames** is assumed present (Linux's legacy RX path; iwm notes that some firmware
   strips it). If received IP packets are 8 bytes too long, this is it.
7. **ASPM**: the driver turns ASPM off in configuration space and sets the "L0s disabled" bit in `CSR_GIO_REG`
   (Linux's choice). iwm sets/clears the same bit from the L1 state.
8. **No rate adaptation**: the firmware starts every data frame at the top of the rate table (MCS 7 or the AP's fastest
   legacy rate) and walks down on retries. Throughput at a distance will suffer.
