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

## Phase 1 — Branch + device skeleton: PCI identity and BAR0/BAR2

Added `hw/display/alchemist/alchemist.c`: a minimal PCI device using DG2-G11's
real device ID (`8086:56a5`, Arc A380), class `0x030000`, BAR0 (GTTMMADR)
exactly 16MB (the floor `xe_mmio_probe_early()` checks before reading any
register) backed by a plain read/write buffer, and BAR2 (GMADR/LMEM) as a
256MB 64-bit prefetchable RAM region. No register has special behavior yet
— everything on BAR0 is a generic buffer, so any offset just echoes back
whatever was last written.

### Guest boot setup

Since the guest needs to load the box's *exact* `xe.ko` (kernel modules are
build/ABI-specific, unlike the Alpine-based generic-kernel setup used for
the earlier `invertram` lab), the guest rootfs is a minimal Ubuntu
`resolute` chroot built with `debootstrap --variant=minbase --include=kmod`,
augmented with:
- The box's own `/lib/modules/7.0.0-30-generic/` tree (for `xe.ko` and its
  dependency chain, resolved automatically by `modprobe` via the copied
  `modules.dep`)
- The box's own `/lib/firmware/i915/dg2_guc_70*.bin.zst` /
  `dg2_huc_gsc.bin.zst` (real firmware, per the Phase 0 finding above)
- `pciutils` (for `lspci -k` evidence), installed via `chroot` + `apt`
- A small custom `/init` that loads `xe` with `force_probe=0x56a5`, then
  dumps `dmesg`, `lspci -k`, and `/dev/dri`, then powers off via
  `echo o > /proc/sysrq-trigger`

Packaged as an initramfs and booted directly against the box's own kernel
image (copied out from the root-only-readable `/boot/vmlinuz-7.0.0-30-generic`
to a user-readable copy) with `-kernel`/`-initrd`, `-accel kvm -cpu host`,
and our `alchemist` device attached.

(One early snag, fixed in place: a first `cp -a` invocation flattened the
copied module tree instead of preserving the `7.0.0-30-generic/`
subdirectory, since the destination didn't exist yet — `modprobe` couldn't
find `xe` at all until that was corrected.)

### Evidence

PCI identity and BAR layout, exactly as intended:
```
[    0.586742] pci 0000:00:04.0: [8086:56a5] type 00 class 0x030000 conventional PCI endpoint
[    0.588840] pci 0000:00:04.0: BAR 0 [mem 0xe0000000000-0xe0000ffffff 64bit]
[    0.589835] pci 0000:00:04.0: BAR 2 [mem 0xe0040000000-0xe004fffffff 64bit pref]
[    1.297780] pci 0000:00:04.0: vgaarb: bridge control possible
```

`lspci -k` from inside the guest confirms both the real device identity
(from the PCI ID database) and that both `i915` and `xe` recognize it:
```
00:04.0 VGA compatible controller: Intel Corporation DG2 [Arc A380] (rev 08)
	Subsystem: Red Hat, Inc. Device 1100
	Kernel modules: i915, xe
```

And the expected, predicted stall — `xe.force_probe=0x56a5` load runs the
real driver's `xe_pcode_probe_early()` against our BAR0, which (since it's
still just a plain buffer with no PCODE mailbox behavior yet) never clears
the ready bit the driver polls for. After the real driver's own 3-minute
timeout:
```
[  332.646533] xe 0000:00:04.0: [drm] *ERROR* PCODE initialization timedout after: 3 min
[  332.648607] xe 0000:00:04.0: probe with driver xe failed with error -110
```

This confirms two things from the plan's research at once: the "3 minute"
PCODE timeout constant, and that probe fails with `-110`/`ETIMEDOUT` exactly
as expected — not a crash, not a different, unanticipated failure mode.
This is the correct, well-scoped exit point for Phase 1; Phase 2 implements
the PCODE mailbox handshake to get past it.

## Phase 2 — PCODE mailbox handshake

Added `hw/display/alchemist/alchemist_pcode.c`, implementing the exact
protocol read directly from `xe_pcode.c`/`xe_pcode_api.h` upstream (not
guessed): the guest writes `PCODE_DATA0`/`PCODE_DATA1` (offsets `0x138128`/
`0x13812c`), then `PCODE_MAILBOX` (`0x138124`) with `PCODE_READY` (bit 31)
set and the mailbox command in the low byte. We only answer the exact
request `xe_pcode_ready()` sends during probe - `DGFX_GET_INIT_STATUS`
(`0x0`) on the `DGFX_PCODE_STATUS` (`0x7E`) mailbox - by writing
`DGFX_INIT_STATUS_COMPLETE` (`0x1`) into `PCODE_DATA0` and clearing
`PCODE_MAILBOX` to `PCODE_SUCCESS` (`0x0`). Any other mailbox command is
deliberately left unanswered (`PCODE_READY` stays set) rather than
blanket-acknowledged, so an unexpected command stalling out stays useful
signal for a future phase instead of being silently papered over.

### Evidence

Re-ran the exact same guest boot as Phase 1. The 3-minute PCODE timeout is
gone entirely, and probe advances past it - confirmed both by the absence
of the Phase 1 error and, precisely, by adding temporary host-side MMIO
read/write tracing to the device (removed before this commit) to watch the
actual register exchange:
```
write addr=0x138128 val=0x0        # PCODE_DATA0 = DGFX_GET_INIT_STATUS
write addr=0x13812c val=0x0        # PCODE_DATA1 = 0
write addr=0x138124 val=0x8000007e # PCODE_MAILBOX = PCODE_READY | DGFX_PCODE_STATUS
read  addr=0x138124 val=0x0        # PCODE_READY now clear
read  addr=0x138128 val=0x1        # PCODE_DATA0 == DGFX_INIT_STATUS_COMPLETE
read  addr=0x138124 val=0x0
```
This is the real mailbox exchange completing correctly at the register
level, not just an absence of the old error. Probe then advances to the
next stage - forcewake - which Phase 3 covers.

