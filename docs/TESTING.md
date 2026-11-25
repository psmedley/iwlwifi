# Testing

The driver has not run on hardware yet. This is the plan for the first runs; it relies on the COM1 trace (the
driver only writes the transmit register of COM1, at whatever speed the BIOS or kernel debugger left) and on the
status text that a read of `\DEV\WRND32$` returns.

Use a machine where the WiFi card is not needed to boot, keep a working CONFIG.SYS to fall back to, and remove
GenMAC / other `WRND32$` drivers first. `/B` (beeps) and `/S1`..`/S9` (stop INIT after a step) locate a boot hang
without a serial port, as in the QCA6174 driver.

## 1. Load

`DEVICE=C:\IBMCOM\MACS\IWLWIFI.OS2 /V`. Expected:

```
IWLWIFI: Intel wireless (iwlwifi family) WLAN driver for OS/2.
Intel(R) Dual Band Wireless-AC 8260
IWLWIFI: NDIS MAC registered with the Protocol Manager.
IWLWIFI: GenMAC device WRND32$. Copy the firmware file to it at boot:
iwlwifi-8000C-36.ucode
```

`copy \DEV\WRND32$ con` shows `ADAPTER=`, `PCI=`, `ID=8086:24F3`, the subsystem ids, the IRQ and BAR0. A device of a
family this build does not drive yet makes the driver refuse to load and name it.

## 2. Firmware upload and bring-up

```
copy /b C:\IBMCOM\MACS\IWLWIFI\iwlwifi-8000C-36.ucode \DEV\WRND32$
copy \DEV\WRND32$ con
```

COM1: `[fw 002500FC loaded]`, then the bring-up steps `[i01][i02]...[i64]` and
`[iwl: ready, firmware 36.c8e8e144.0]`, or `[IWLWIFI: <what failed> <value>]`. The status text then shows
`STATE=0064 READY`, `FWVER=36.c8e8e144.0`, `RTSEC=0C INITSEC=0D PAGING=00026000`, `HWMAC=` (the adapter's address),
`NVM ... chans=` (about 30-40), `txant=3 rxant=3`, `PHYDB=` (a dozen sections).

What to look at when it stops (`STATE=`, hex; ARCHITECTURE.md section 5):

| STATE | Meaning | Look at |
|---|---|---|
| 7 | the card is not ready / no MAC clock | `GP_CNTRL=`; is the card owned by the management engine (AMT)? |
| 8 | INIT firmware: a chunk not taken, or no ALIVE | `LASTERR`; `CSR_INT=`; `RXPKT=` (0: nothing came back on the receive ring) |
| 10 | NVM | `CMDTO=` (commands timed out), `REPLYERR=` |
| 11 | calibration | `PHYDB=`, `FWERR=` |
| 21 | runtime firmware | as 8 |
| 22 | paging | `PAGING=`, `LASTERR` |
| 23-27 | a configuration command | `LASTERR` names it; `REPLYERR type= cmd=` is the firmware's complaint |

`echo up > \DEV\WRND32$` retries. `echo r 24 > \DEV\WRND32$` reads a CSR register (here GP_CNTRL) onto COM1.

## 3. Scan

`echo scan > \DEV\WRND32$`, then `copy \DEV\WRND32$ con`: `SCAN n=` counts scans, `MPDU=` received frames,
`NETS=` and one `NET=channel rssi HT ssid` line per network. Then start the Wireless LAN Monitor: it should list the
same networks.

## 4. Open network

`echo join MyOpenNet > \DEV\WRND32$`: COM1 `[joined aid xxxx]` or `[join rc FFFF fail nn]`:

| JFAIL | Meaning |
|---|---|
| 1 | SSID not in the scan table |
| 2 | no authentication reply (frames not sent? `TXOK`/`TXFAIL`; wrong channel?) |
| 3 | authentication refused |
| 4 | no association reply |
| 5 | association refused |
| 9 | the firmware objects for the join failed (`LASTERR`) |
| 11 | the post-association commands failed |

Then bind TCP/IP (DHCP) and ping. `MGMTRX=` counts the management frames the AP sent back (subtype 11 authentication, 1 association response, 12
deauthentication, 10 disassociation); `TXST` shows the firmware's status for the last frame sent (`0001` ok) and the
rate it used. `DATA rx= ndis= tx=` and `TXOK=`/`TXFAIL=` show the data path; `drop=` counts
received frames dropped (0 not from our AP, 1 null data, 2 A-MSDU, 3 protected but not decrypted, 4 too short,
5 before the keys, 6 not LLC/SNAP).

## 5. WPA2-PSK

With the Wireless LAN Monitor or wpa_supplicant: `EAPOL=`/`EAPOLQ=` show the handshake frames, `LINK= ... PTK
AUTHORIZED` the installed keys. `DECFAIL=` counts frames the firmware could not decrypt.

## 5b. 5 GHz and 40 MHz

The driver uses a 40 MHz channel when the access point runs one and the regulatory data allows it; `LINK=` shows
`HT40 NSS=2 SCO=1` (secondary channel above) or `SCO=3` (below), `HT20` otherwise; the NDIS link speed follows
(300 Mbit/s for two streams of 40 MHz). The last transmit's rate is in `TXST rate=` (flags: `00000800` 40 MHz,
`00002000` short GI, `0000C000` two antennas, `00000100` HT, low byte the MCS: 0F is MCS 15).

To compare, put `MAXWIDTH = 20` and `STREAMS = 1` in the `[IWLWIFI_NIF]` section of PROTOCOL.INI. To test 5 GHz
(the driver picks the strongest access point of the SSID, which is usually the 2.4 GHz one), pin the primary
channel with `CHANNEL = 149` (or 36, 40, 44, 48: channels 52-144 are DFS channels that the firmware may refuse to
transmit on until it has checked for radar, so they are a poorer first test).

## 5c. Receive aggregation

`BA(rx aggregation) sessions=` counts the block-ack sessions the AP started (`refused=` the ones we could not take:
no free session, no HT, or no memory), `direct=` frames handed on at once, `reordered=` frames that were held and
released, `old=`/`dup=` frames dropped as duplicates, `timeouts=` holes closed after 100 ms, `maxheld=` the most frames
held at once. `ASYNC ok= fail=` counts the firmware's answers to the ADD_STA commands that start and stop sessions
(`fail` should stay 0; a failed one also appears in `CMDS=` as `8018/FFF3/<status>`). While downloading, `sessions=`
should become non-zero and `reordered=` should grow; compare the speed with `AMPDU = 0` in PROTOCOL.INI. If a download
stalls or TCP slows to a crawl, send me the BA line, `timeouts=` high means the AP's frames are not arriving.


## 5d. Transmit aggregation

After 24 QoS data frames the driver sends an ADDBA request (TID 0); if the AP accepts, TID 0 moves to its own firmware
queue (queue 10) and the firmware builds A-MPDUs. `AGGTX = 0` in PROTOCOL.INI turns it off, to compare.

* `TXBA(tx aggregation) qosframes= addba= accepted= refused= timeouts= delba=`: requests sent, AP answers.
* `TXAGG state=` idle / offered / configuring / ACTIVE / FAILED, with the next sequence number, queue and ring position.
* `TXAGG started= cfgok= cfgfail= cmdfail= scdbug= drain= giveup=`: `cfgok` should reach `started`; `scdbug` counts frames
  that used the normal queue to avoid a scheduler restriction; `drain` counts waits for the normal queue to empty.
* `TXAGG banotif= single= ampdus= frames=`: block-ack notifications, single-frame and aggregate completions.
  `queued=` on the first TXAGG line must return to 0 when the upload ends (otherwise slots leak).

Test: upload a large file (compare with `AGGTX = 0`). If the upload stalls, `state=` and `queued=` tell where.

## 6. Interrupts

`IRQS=` grows while the adapter is busy; `SPUR=` counts interrupts on a shared line that were not ours. `IRQS=0` with
everything working means the driver runs polled (ARCHITECTURE.md section 8, question 1).

## 7. Hardware results

**Run 1** (8260, subsystem 8086:0010, rev 3A, IRQ 12, BAR0 EC000000; build 3a8d465): the driver loaded; the status
text before the firmware was as expected. After the copy: `STATE=0025 FAILED`, `LASTERR=POWER_TABLE failed`.
Everything before that worked: `FWVER=36.c8e8e144.0`, ALIVE twice (`ALIVE=11`), `HWMAC=40:A3:CC:4F:31:02`,
`NVM ver=0E56 chans=26 5G=1 11n=1 11ac=1 txant=3 rxant=3 LAR=1`, `PHYDB=0F`, 115 interrupts delivered (`IRQHOOKED=1`),
46 firmware packets received. Then `FWERR=1` (the firmware crashed), `CMDTO=1`, `REPLYERR=1`, last packet `0002`
(REPLY_ERROR). Cause suspected: the temperature-thresholds command was sent with 44 bytes instead of 20 (fixed;
a REPLY_ERROR answer is now treated as a failed command).

**Run 2** (build 7d8d850): `STATE=0064 READY`, no firmware errors. `CMDS=` shows the whole sequence accepted
(ADD_STA, PHY_CONTEXT, thresholds `0404`, POWER_TABLE `0077`, MCC `00C8`, SCAN_CFG `010C`, four `001D` queue
configurations, `00D2`, SCAN_REQ `010D`). The scan found 16 access points (RSSI -41 to -78 dBm): 2.4 GHz channels
1, 4, 6, 11 and 5 GHz channels 36, 56, 136, 149, hidden SSIDs included; 19 frames received, none with a CRC error.
`LAT irq2hook max=004E` (78 ms from interrupt to the task-time handler at worst). `JFAIL=08` is a join attempted
by the Wireless LAN Monitor while the adapter was not ready (before the fix). The Monitor's `GET` requests now get
answers and the MAC is set (`MAC=40:A3:CC:4F:31:02`).

**Run 3** (build a506edf plus the CRLF fix): joined `smedleywan` on 2.4 GHz channel 11 (AID 5, WPA2-PSK, HT, `PTK
AUTHORIZED`), 41,716 frames received and 10,121 sent in one session, no decryption failures, no CRC errors, no
firmware errors; the last transmit was MCS 7, short GI, no retries (-39 dBm). `MGMTRX=` shows one association
response, one authentication and two action frames (the ADDBA requests we refuse). Download speed about 3.3 MB/s.
Three observations: 408 sends were refused because all 16 transmit slots were busy (now 32); 1,869 firmware
statistics notifications were counted as unhandled (now ignored); 96 received frames were dropped as "not
LLC/SNAP" (`drop[6]`), not yet understood and not important at this rate.

**Run 4** (build 4d8657e, HT40 but not yet receive aggregation): joined `smedleywan` on 5 GHz channel 149 (`HT40
NSS=2 SCO=1`), last transmit `rate=4000E90F` = MCS 15, 40 MHz, short GI, two antennas (300 Mbit/s), no retries; no
firmware, CRC or decryption errors; `txbusy=0`; `UNHANDLED=0`. Download about 4.7 MB/s (was 3.3 MB/s on 2.4 GHz channel
11, 20 MHz, one stream). 75 received frames were dropped as "not LLC/SNAP"; these are probably IEEE 802.3 frames with a
plain LLC header (NetBEUI, NetBIOS over LLC, bridge BPDUs), which the driver now converts instead (`8023 rx= tx=`
in the `DATA` line counts them).

**Run 5** (build 085a99b: receive aggregation): on 2.4 GHz channel 11, HT20 with two streams (`rate=0000E10F`, MCS 15,
144 Mbit/s), download above 6.5 MB/s (3.3 MB/s on the same channel with one stream and no aggregation).
`BA sessions=2 refused=0`, `ASYNC ok=2 fail=0`, `direct=50096 reordered=1055 old=472 dup=0 timeouts=0 maxheld=17`: the
AP's two block-ack sessions were accepted, 2% of the frames needed reordering, the 0.9% `old` are retransmissions of
frames already delivered, no hole ever timed out. No firmware, CRC or decryption errors. Two things seen: `UNHANDLED`
grew to 2949 with last code 0000 (it was 0 before aggregation: leftover bytes in reused receive buffers, now cleared
after use; the `UNH` line records what they are), and `drop[6]=32` frames not LLC/SNAP (that build did not yet carry
IEEE 802.3 frames: see run 4).

New status lines for a failure of this kind:

* `CMDS=n (oldest first, id/result/info): ...` the last 12 host commands: `id` (group in the high byte), result
  `0000` ok, `FFFB` refused by the firmware (info = its error type), `FFFC` no answer, `FFF9` the firmware crashed.
* `FWLOG ...` and `UMACLOG ...`: the firmware's own error table, read after a crash (`id=` is the error code,
  `data=` three error-specific words, `lastcmdid=`/`hcmd=` the command it was handling). Please send these.

Please send the full `copy \DEV\WRND32$ con` output and the COM1 log with any report.

**Run 6** (build c5117e4): downloads and `ls` on an SMB share stalled, small exchanges worked. Cause: the receive check
`len - hl + 14 > sizeof(g_eth)` (added with the 802.3 conversion) counted the 8-byte LLC/SNAP header as if it stayed, so
every full-size frame (1500-byte IP packet = 1522 > 1520) was dropped (`drop[4]` counts these). `g_eth` is now 1600 bytes.
`AMPDU = 0` and `MAXWIDTH = 20` did not help, which pointed away from aggregation.
