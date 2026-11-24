# SPDX-License-Identifier: GPL-2.0-only
# Cross build on Linux with Open Watcom 2.0 (binl64 on PATH, WATCOM set) and NASM:  make
# On OS/2 use build.cmd (same flags). Objects go to obj/, the driver to IWLWIFI.OS2.
#
#   os2/   OS/2 plumbing: device header, DevHelp glue, strategy routine, NDIS MAC, GenMAC OIDs, DMA memory, PCI
#   wlan/  the chip-independent station (scan table, join, keys, EAPOL, Ethernet <-> 802.11) and hw.h
#   iwl/   the Intel iwlwifi back end (device table, firmware file, PCIe transport, mvm firmware conversation)
CFLAGS = -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=iwl
ASM    = hdr lib ndisx end
OS2C   = strat pci mem ndis genmac dbg
WLANC  = sta
IWLC   = iwl_cfg iwl_fw iwl_trans iwl_mvm iwl_nvm iwl_phydb iwl_scan iwl_mac
HDRS   = os2/drv.h os2/os2dd.h os2/ndis.h wlan/hw.h wlan/wlan.h iwl/iwl.h

OBJS   = $(addprefix obj/,$(addsuffix .obj,$(ASM) $(OS2C) $(WLANC) $(IWLC)))

IWLWIFI.OS2: obj $(OBJS) iwlwifi.lnk
	wlink @iwlwifi.lnk

obj:
	mkdir -p obj

obj/%.obj: os2/%.asm
	nasm -f obj $< -o $@

obj/%.obj: os2/%.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

obj/%.obj: wlan/%.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

obj/%.obj: iwl/%.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

clean:
	rm -rf obj IWLWIFI.OS2 IWLWIFI.map

.PHONY: clean
