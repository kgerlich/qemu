# Alchemist (DG2) bring-up log

This is a running log for the Intel Arc "Alchemist" (DG2, Xe-HPG, Gen12.5)
QEMU device-model simulation. Goal for this phase of work: get the real,
unmodified upstream Linux `xe` kernel driver to successfully complete PCI
probe against our simulated device. See the project plan for full context
and phase breakdown; this file records the evidence captured at each phase.

## Phase 0 — Guest kernel: confirm a stock kernel has `xe.ko`

Checked the dev box's own installed Ubuntu 26.04 kernel
(`7.0.0-30-generic`) rather than needing to source or build one separately.

**`xe.ko` is present and built-in to the stock kernel:**
```
$ find /lib/modules/$(uname -r) -iname 'xe.ko*'
/lib/modules/7.0.0-30-generic/kernel/drivers/gpu/drm/xe/xe.ko.zst

$ modinfo xe | head -5
filename:       /lib/modules/7.0.0-30-generic/kernel/drivers/gpu/drm/xe/xe.ko.zst
import_ns:      DMA_BUF
license:        GPL and additional rights
description:    Intel Xe2 Graphics
author:         Intel Corporation
```

**All three module params the plan depends on are present:**
```
$ modinfo xe | grep -iE 'force_probe|guc_firmware_path|huc_firmware_path|gsc_firmware_path'
parm:  guc_firmware_path:GuC firmware path to use instead of the default one (charp)
parm:  huc_firmware_path:HuC firmware path to use instead of the default one - empty string disables (charp)
parm:  gsc_firmware_path:GSC firmware path to use instead of the default one - empty string disables (charp)
parm:  force_probe:Force probe options for specified devices. See CONFIG_DRM_XE_FORCE_PROBE for details [default=]) (charp)
```

**Our planned device ID (`0x56a5`, DG2-G11/Arc A380) is in the driver's PCI
alias table**, confirming the choice from the plan is a real, recognized ID:
```
$ modinfo xe | grep alias | grep 56A5
alias:  pci:v00008086d000056A5sv*sd*bc03sc*i*
```

**Bonus finding, better than the plan anticipated**: real DG2 GuC/HuC
firmware blobs are already present via `linux-firmware`, no download needed:
```
$ find /lib/firmware -iname 'dg2_guc*' -o -iname 'dg2_huc*'
/lib/firmware/i915/dg2_guc_70.1.2.bin.zst
/lib/firmware/i915/dg2_huc_gsc.bin.zst
/lib/firmware/i915/dg2_guc_70.bin.zst
/lib/firmware/i915/dg2_guc_70.4.1.bin.zst
```
This changes the Phase 4/5 approach for the better: rather than synthesizing
a byte-valid CSS-header firmware blob from scratch (the original plan, given
we expected not to have real signed firmware available), we can decompress
and use the **real** `dg2_guc_70.bin` directly. It trivially satisfies the
driver's own CSS-header parse/consistency check (it's genuine Intel
firmware), which both removes the "did we get the struct layout right"
risk entirely and gives us ground truth for the CSS header layout by
inspection, if needed. Our simulated device still doesn't need to actually
execute the microcode or perform real RSA verification — it only needs to
report `GUC_STATUS.GS_AUTH_STATUS_GOOD` after a believable delay, exactly as
planned; using the real blob just removes one whole category of "did we
build the fake firmware correctly" risk from Phase 4.

The kernel image itself is also present and usable directly:
```
$ ls -la /boot/vmlinuz-$(uname -r)
-rw------- 1 root root 17295752 Jul 31 18:28 /boot/vmlinuz-7.0.0-30-generic
```

**Conclusion**: no separate kernel build or download is needed for the
guest. Phase 0's goal (confirm a usable prebuilt kernel+driver+firmware
before building anything ourselves) is satisfied by the box's own installed
kernel. The guest boot setup (rootfs/initramfs matching this exact kernel's
module ABI) is addressed as part of Phase 1's verification step.
