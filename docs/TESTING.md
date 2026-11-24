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

Then bind TCP/IP (DHCP) and ping. `DATA rx= ndis= tx=` and `TXOK=`/`TXFAIL=` show the data path; `drop=` counts
received frames dropped (0 not from our AP, 1 null data, 2 A-MSDU, 3 protected but not decrypted, 4 too short,
5 before the keys, 6 not LLC/SNAP).

## 5. WPA2-PSK

With the Wireless LAN Monitor or wpa_supplicant: `EAPOL=`/`EAPOLQ=` show the handshake frames, `LINK= ... PTK
AUTHORIZED` the installed keys. `DECFAIL=` counts frames the firmware could not decrypt.

## 6. Interrupts

`IRQS=` grows while the adapter is busy; `SPUR=` counts interrupts on a shared line that were not ours. `IRQS=0` with
everything working means the driver runs polled (ARCHITECTURE.md section 8, question 1).

Please send the full `copy \DEV\WRND32$ con` output and the COM1 log with any report.
