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

## Phase 3 — Forcewake domains

The temporary MMIO tracing from Phase 2 turned out to be exactly the right
tool for scoping this phase too: probe's next register access after PCODE
is a write to `FORCEWAKE_GT` (`0xa188`) with value `0x10001`, followed by a
tight poll of `0x130044` that never resolves. Cross-referencing
`xe_force_wake.c`/`regs/xe_gt_regs.h` confirms this exactly: `0x130044` is
`FORCEWAKE_ACK_GT`, and `0x10001` is precisely
`FORCEWAKE_MT_MASK(FORCEWAKE_KERNEL) | FORCEWAKE_MT(FORCEWAKE_KERNEL)` -
the masked-write convention used by every forcewake control register
(bits `[31:16]` select which of bits `[15:0]` to update).

Added `hw/display/alchemist/alchemist_forcewake.c`: a data-driven table of
every control/ack register pair DG2-class `xe` defines - `GT`, `RENDER`,
`GSC`, and the per-instance media decode (`VDBOX0-7`) and encode
(`VEBOX0-3`) domains - transcribed directly from
`xe_force_wake_init_gt()`/`xe_force_wake_init_engines()`. (Note: unlike
older i915-generation hardware, `xe` has no separate "blitter" forcewake
domain - the plan's original phrasing was based on i915-era terminology;
DG2's copy engine doesn't have its own domain in `xe`.) Any write to a
known control register applies the masked update and immediately mirrors
the result into the paired ack register - there's no real power-gated
hardware behind this device to model a wake delay for.

### Evidence

```
[    2.917667] xe 0000:00:04.0: [drm] Unknown revid 0x08
[    2.918871] xe 0000:00:04.0: [drm] Unknown revision 0x08
[    2.920084] xe 0000:00:04.0: [drm] Found dg2/g11 (device ID 56a5) discrete display version 13.00 stepping **
[    2.922394] xe 0000:00:04.0: [drm] VISIBLE VRAM: 0x00000e0040000000, 0x0000000010000000
[    2.924206] xe 0000:00:04.0: [drm] *ERROR* Tile without any CPU visible VRAM. Aborting.
```

The forcewake stall is gone - probe now runs all the way through platform
identification (correctly reading back "dg2/g11", exactly the subplatform
we chose) and into VRAM probing, where it correctly reads our real BAR2
address and 256MB size ("VISIBLE VRAM: ..., 0x10000000"). The "Unknown
revid/revision 0x08" lines are cosmetic - our chosen PCI revision isn't in
the driver's stepping lookup table - and not fatal.

**New, previously unplanned finding**: `xe_vram_probe()` treats the tile as
having no CPU-visible VRAM despite the BAR being correctly sized and
mapped, meaning it derives "visible VRAM" from a dedicated LMEM-size
register rather than (or in addition to) the BAR itself, and we haven't
implemented that register yet. This wasn't in the original phase list -
the plan's phases 4-7 covered GuC/IRQ/milestone, not VRAM register support.
Next step is a short research pass on `xe_vram_probe()` to identify the
exact register, added as an inserted phase before GuC.

## Phase 3.5 — VRAM/tile sizing registers (inserted, not in original plan)

Read `xe_vram.c` directly to find the exact cause: `tile_vram_size()`
computes usable VRAM from two plain registers, not from the LMEM BAR
itself:

- `SG_TILE_ADDR_RANGE(0)` (`0x1083a0`): tile size and offset, in 1GB units
  (`GENMASK(17,8)` / `GENMASK(7,1)`).
- `XEHP_FLAT_CCS_BASE_ADDR` (`0x4910`, an MCR/steered register): offset (in
  64K units) marking where flat-CCS compression metadata begins;
  `usable_size = ccs_offset - tile_offset`.

Both read `0` on our unmodified buffer, so `usable_size` computed to `0`,
tripping `vram_region_init()`'s `!vram->io_size` check.

Before implementing the MCR register, checked whether MCR steering
requires modeling: read `rw_with_mcr_steering()` in `xe_gt_mcr.c`
directly - it always writes a steering-selector register first (harmless,
falls into our generic buffer) but the actual data read/write always
happens at the register's own raw offset regardless of steering target.
So no MCR-specific device logic is needed at all, just a normal register
at `0x4910`.

Added `hw/display/alchemist/alchemist_vram.c`, called once from
`realize()` (not a write-triggered handler, like PCODE/forcewake - these
are plain fixed-value registers, no request/response protocol). Reports a
tile size of exactly 1GB (the smallest value the 1GB-granularity
`SG_TILE_ADDR_RANGE` field can represent) with the flat-CCS base set to
that same offset - i.e. no space reserved for CCS metadata, all of the
(fictional) 1GB tile nominally usable.

Since our real BAR2 is 256MB, smaller than that 1GB, `vram_region_init()`
clamps usable size down to what's actually mapped and logs "Small BAR
device." This is not a shortcut - it's DG2's real, documented behavior on
hardware without Resizable BAR enabled (see the kernel's
`Documentation/gpu/rfc/i915_small_bar.rst`), so exercising that exact
codepath is arguably more faithful than inventing a fake 1GB+ BAR2 just to
dodge it.

### Evidence

```
[    2.779620] xe 0000:00:04.0: [drm] Small BAR device
[    2.780558] xe 0000:00:04.0: [drm] VRAM[0]: Actual physical size 0x0000000040000000, usable size exclude stolen 0x0000000040000000, CPU accessible size 0x0000000010000000
[    2.783275] xe 0000:00:04.0: [drm] VRAM[0]: DPA range: [0x0000000000000000-40000000], io range: [0x00000e0040000000-e0050000000]
[    2.785397] xe 0000:00:04.0: [drm] VRAM: 0x0000000040000000 is larger than resource 0x0000000010000000
[    2.793049] xe 0000:00:04.0: [drm] Display not present, disabling
```

Physical size (1GB), usable size (1GB), and CPU-accessible size (256MB)
all read back exactly as computed. "Display not present, disabling" is
expected and non-fatal - we haven't implemented any display/output
registers, and none are required for probe success. Probe now advances to
IRQ allocation:
```
[    2.800356] xe 0000:00:04.0: [drm] *ERROR* Failed to allocate IRQ vectors: -22
[    2.801727] xe 0000:00:04.0: probe with driver xe failed with error -22
```
This is exactly Phase 6 (IRQ plumbing) from the original plan - our device
has no MSI capability configured at all yet, so `pci_alloc_irq_vectors()`
has nothing to allocate.

## Phase 6 — MSI interrupt support

Checked `xe_irq_install()` first rather than assuming MSI-X was needed:
`xe_irq_msix_init()` calls `pci_msix_vec_count(pdev)`, and when that
returns `-EINVAL` (no MSI-X capability present at all) it returns `0`
without error - `xe_device_has_msix()` then reports false, and
`xe_irq_install()` falls back to a single plain MSI vector
(`pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI)`). So no MSI-X modeling
is needed at all, just one ordinary MSI vector.

Added directly in `alchemist.c`'s `realize()`/`exit()` (small enough not
to need its own file, mirroring how `edu.c` does it inline):
`pci_config_set_interrupt_pin()` + `msi_init(pdev, 0, 1, true, false, errp)`,
matched with `msi_uninit()` on exit.

### Evidence

```
[    2.931056] xe 0000:00:04.0: [drm] Display not present, disabling
[    2.941102] xe 0000:00:04.0: [drm] Interrupt register 0x444f8 is not zero: 0xffffffff
[    2.942765] WARNING: drivers/gpu/drm/xe/xe_irq.c:49 at xe_irq_postinstall+0xdf/0x280 [xe], CPU#0: modprobe/73
...
[    3.042120] xe 0000:00:04.0: [drm] Tile0: GT0: Using GuC firmware from i915/dg2_guc_70.bin version 70.53.0
[    3.077547] xe 0000:00:04.0: [drm] Failed to init uC WOPCM registers!
[    3.078958] xe 0000:00:04.0: [drm] DMA_GUC_WOPCM_OFFSET(0xc340)=0x0
[    3.080355] xe 0000:00:04.0: [drm] GUC_WOPCM_SIZE(0xc050)=0x3f3000
[    3.081681] xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: Failed to initialize uC (-EINVAL)
[    3.083413] xe 0000:00:04.0: probe with driver xe failed with error -22
[    3.089969] xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: GuC reset timed out, GDRST=0x8
[    3.091861] xe 0000:00:04.0: [drm] *ERROR* Failed to reset GuC, ret = -110
```

`pci_alloc_irq_vectors()` now succeeds - no more "Failed to allocate IRQ
vectors" error - and probe advances dramatically further: past VRAM,
display, and into real GuC firmware loading. Notably, it picked up the
**real** `i915/dg2_guc_70.bin version 70.53.0` from the box's own
`linux-firmware`, exactly the Phase 0 finding paying off - no synthetic
firmware blob was ever needed.

There's a benign `xe_irq_postinstall` `WARN_ON` about interrupt register
`0x444f8` not reading as zero at postinstall time (a soft warning, not a
probe-ending failure by itself) worth investigating alongside the GuC
work, since it's in the same interrupt-handling neighborhood.

The real next blocker is GuC's WOPCM (Write-Once Protected Code Memory)
register setup failing, which cascades into a GuC reset timeout. This is
exactly the territory the original plan flagged as needing a dedicated
research phase before writing code - GUC_WOPCM_SIZE/DMA_GUC_WOPCM_OFFSET
semantics, the boot-status handshake, and (per the plan) potentially GuC
CTB traffic during probe itself. Next step is that research spike.

## Phase 4/5 — GuC research spike + firmware load handshake

Read `xe_wopcm.c`, `xe_uc_fw.c`, and `xe_guc.c` directly to resolve the
plan's three open GuC unknowns:

1. **`struct uc_css_header` layout** - moot. We use the box's real
   `i915/dg2_guc_70.bin` (per the Phase 0 finding), so the driver's
   own CSS-header parse just works; no synthetic blob was ever needed.
2. **WOPCM register semantics** - `__wopcm_init_regs()`
   (`xe_wopcm.c`) uses `xe_mmio_write32_and_verify()`: write the raw
   value, then read back and require a status bit the guest never wrote
   to be set - `GUC_WOPCM_SIZE` (`0xc050`) bit 0 (`LOCKED`) and
   `DMA_GUC_WOPCM_OFFSET` (`0xc340`) bit 0 (`VALID`). Real hardware sets
   these itself on write to confirm the partition took effect; our
   device does the same.
3. **Does GuC mailbox traffic happen during probe?** Confirmed yes -
   `xe_guc_min_load_for_hwconfig()` calls `__xe_guc_upload()` (firmware
   DMA + boot-status wait) first, then `xe_guc_hwconfig_init()` and
   `xe_guc_enable_communication()` immediately after, all during normal
   probe. Firmware boot and mailbox communication are sequential,
   though, not interleaved - we can implement and verify them as two
   separate steps.

`uc_fw_xfer()` (`xe_uc_fw.c`) writes `DMA_ADDR_0/1`, `DMA_COPY_SIZE`
(plain writes, no side effects needed), then `DMA_CTRL` (`0xc314`) - a
masked-write register (same `[31:16]` mask / `[15:0]` data convention as
forcewake) that starts the transfer via `START_DMA` (bit 0). Real
hardware clears `START_DMA` once the transfer completes, which the guest
polls for. `guc_wait_ucode()` then polls `GUC_STATUS` (`0xc000`) for the
`GS_UKERNEL_MASK` field to read `XE_GUC_LOAD_STATUS_READY` (`0xF0`),
bounded to 3 seconds (non-debug builds).

Added `hw/display/alchemist/alchemist_guc.c`: on `GUC_WOPCM_SIZE`/
`DMA_GUC_WOPCM_OFFSET` writes, OR in the confirmation bit before storing.
On a `DMA_CTRL` write with `START_DMA` requested, apply the masked
update, immediately clear `START_DMA` (there's nothing to actually
transfer - the guest's own GGTT-mapped source and WOPCM destination are
both within guest memory we don't touch), and set `GUC_STATUS` to
booted+authenticated (`GS_AUTH_STATUS_GOOD | GS_UKERNEL_READY |
GS_BOOTROM_JUMP_PASSED`) in the same step. As with PCODE, the driver
itself never cryptographically verifies the firmware - that's delegated
to hardware (us), so reporting success here isn't a shortcut around real
validation, it's what a real chip's boot ROM result collapses to from
the driver's point of view too.

All the other registers `__xe_guc_upload()` touches along the way
(`GUC_SHIM_CONTROL`, `GT_PM_CONFIG`, `PMINTRMSK`, `SOFT_SCRATCH(n)`
params, `UOS_RSA_SCRATCH(n)`) are plain writes with no readback
verification, so the existing generic buffer already handles them
correctly with no new code.

### Evidence

```
[    3.077277] xe 0000:00:04.0: [drm] Tile0: GT0: Using GuC firmware from i915/dg2_guc_70.bin version 70.53.0
[    3.173030] xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: GuC mmio request 0x4100: no reply 0x4100
[    3.175328] xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: Failed to initialize uC (-ETIMEDOUT)
```

The WOPCM-init failure and the DMA-timeout error from before are both
gone entirely - firmware DMA and the `GUC_STATUS` boot-ready handshake
now complete cleanly using the real GuC firmware blob. Probe advances
into GuC mailbox communication (`xe_guc_hwconfig_init()`'s "mmio
request"), confirming open question 3 above empirically as well as by
source reading. This mailbox protocol is the next thing to research and
implement.

(The `xe_irq_postinstall` "Interrupt register 0x444f8 is not zero"
`WARN_ON` from Phase 6 is still present and still benign - noted again
here since it's adjacent to this GuC work, not because it changed.)

## Phase 5b — GuC mmio mailbox (HXG protocol)

Once firmware boots, probe advances into GuC *mailbox* communication -
a distinct protocol from the DMA/boot-status registers above. Diagnosed
with the same host-side MMIO tracing technique used for Phase 3's
forcewake stall (added temporarily, removed before committing):

- The guest encodes a request in `xe_guc_mmio_send_recv()`
  (`abi/guc_messages_abi.h`'s "HXG" format) into `VF_SW_FLAG(0..3)`
  (`0x190240`+), then writes `xe_guc_notify()`'s `notify_reg`. First
  guess (`GUC_SEND_INTERRUPT`, `0xc4c8`) was wrong - tracing showed the
  guest never touched it. Reading `xe_guc_notify()` directly showed the
  real register: `GUC_HOST_INTERRUPT` (`0x1901f0`), for the main GT - and
  it's not a bit-flag register, *any* write at all rings the doorbell.
- The guest then polls `VF_SW_FLAG(0)` for a GuC-origin response.

Added the mailbox handler to `hw/display/alchemist/alchemist_guc.c`
(same file as the firmware/WOPCM handshake - same "GuC" domain): decodes
the HXG request header, and for two actions we know the driver sends
during probe:

- `XE_GUC_ACTION_GET_HWCONFIG` (`0x4100`): the driver first asks for the
  hwconfig table's *size* (a zero ggtt-address/zero-size request); we
  answer with a nonzero placeholder (`xe_guc_hwconfig_init()` hard-fails
  on a zero size). We do **not** deliver real table content on the
  follow-up copy request - that needs writing bytes into a GGTT address
  the guest provides, which needs GGTT translation (see Phase 7 below).
  The guest's hwconfig buffer is left as freshly-allocated zeroed guest
  memory, which `xe_guc_hwconfig_lookup_u32()`'s parser reads safely as
  "zero attributes" rather than crashing - a deliberate, documented
  deferral, not silently faked content.
- `GUC_ACTION_HOST2GUC_SELF_CFG` (`0x0508`): used to tell GuC the CTB
  descriptor/ring GGTT addresses. First attempt answered these (and
  everything else) with a generic `data0=0` success - which broke this
  action specifically: `guc_self_cfg()` (`xe_guc.c`) treats a response
  `data0` of exactly `0` as **failure** (`-ENOKEY`), not `1` (a count of
  configured KLV entries, per its own success path). Fixed by
  special-casing `data0=1` for this action.

### Evidence

Before the `GUC_HOST_INTERRUPT` fix, nothing happened at all - the
notify register the driver actually used was silently absorbed as a
plain buffer write, so the driver polled forever:
```
xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: GuC mmio request 0x4100: no reply 0x4100
xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: Failed to initialize uC (-ETIMEDOUT)
```
After the notify-register fix but before the SELF_CFG `data0` fix:
```
xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: Failed to enable GuC CT (-ENOKEY)
xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: Failed to initialize uC (-ENOKEY)
```
With both fixes, GuC mailbox communication completes cleanly - no more
mmio timeout or `-ENOKEY`, and probe advances into engine/topology
enumeration (all engines log "fused off", which is expected/correct: we
haven't implemented any of the fuse-mask registers, so every optional
engine reads as absent, same as a real minimal-fuse SKU would) and then
into the **real CTB (Command Transport Buffer) ring protocol** - a
different, larger communication mechanism the mmio mailbox above only
bootstraps. That surfaces as a new stall (`GuC reset timed out,
GDRST=0x8`, a downstream symptom of CTB communication never completing,
not a new independent register to fix) roughly 13 seconds later.

This prompted stepping back from per-register reactive fixes to map the
actual remaining scope: CTB requires real address translation to guest
memory (GGTT), which is a foundational capability, not another status
register - covered in Phase 7 below.

## Phase 7 — GGTT address translation

Before writing any CTB code, mapped the exact remaining call chain
(`xe_uc.c`, `xe_guc_ct.c`) rather than continuing to react to the next
error: past CT-enable, `xe_uc_load_hw()` does a **second full GuC
reload** (`xe_huc_upload` → `xe_guc_upload` → `xe_guc_enable_communication`
→ `xe_guc_post_load_init` → `xe_guc_pc_start` → `xe_guc_rc_enable` → HuC
auth → GSC load), and every step past CT-enable talks over the real CTB
ring buffer, not the mmio mailbox from Phase 5b. The `GDRST` timeout is
that second reload's own error-recovery fallback after CTB communication
never completes - not an independent thing to fix.

CTB buffers are guest-allocated BOs, referenced only by GGTT address
(registered via the `SELF_CFG` calls Phase 5b already handles correctly).
To read/write them we need real GGTT translation - confirmed directly
against source, not assumed:

- `xe_mmio.c` documents BAR0's own layout: registers `0-4MB`, reserved
  `4-8MB`, **GGTT `8-16MB`** - inside the 16MB BAR0 we already have.
- `xe_ggtt_init_early()` confirms exactly that: `ggtt->gsm = tile->mmio.regs + SZ_8M`.
- `xe_ggtt_set_pte()` writes PTEs as plain 8-byte MMIO stores
  (`writeq(pte, &ggtt->gsm[addr >> XE_PTE_SHIFT])`) - our generic buffer
  already stores these correctly for free; translation is a pure
  read-side decode, no write-side hook needed.
- PTE format (`regs/xe_gtt_defs.h`): bit 0 present, bit 1
  `XE_GGTT_PTE_DM` (1 = the address is a VRAM/BAR2 offset, 0 = a guest
  system-RAM physical address), bits `[51:12]` the address itself. The
  8MB GSM region sized for exactly 4GiB of GGTT space at 4K/PTE confirms
  the shift is 12 (standard 4K pages), not a 64K-page variant.

Added `hw/display/alchemist/alchemist_ggtt.c`: `alchemist_ggtt_read()`/
`alchemist_ggtt_write()` walk a transfer **one page at a time** (never
assuming GGTT-contiguous addresses map to physically-contiguous guest
pages - a real correctness requirement, not paranoia, since a multi-page
transfer that only translated the first page could silently corrupt data
on any guest allocation that isn't physically contiguous), dispatching
each page to either our own VRAM buffer (`memory_region_get_ram_ptr()`,
cached once at realize time as `s->vram_ptr`) or real guest system RAM
via `pci_dma_read()`/`pci_dma_write()` - the same PCI DMA API `edu.c`
uses for its own toy DMA engine. Both return `false` (nothing silently
substituted) if any page in the range is unmapped or the DMA itself
fails.

The `ALCHEMIST_MMIO_SIZE`/`ALCHEMIST_VRAM_SIZE` size constants and the
`AlchemistState` struct moved from `alchemist.c` into
`alchemist_internal.h` as part of this change, since GGTT translation
needs both from a separate file - no behavior change, just making them
genuinely shared rather than locally-defined-and-hoped-for-consistency.

### Evidence

This phase is pure infrastructure - nothing calls `alchemist_ggtt_read()`/
`write()` yet, so it changes no observable behavior. Confirmed via a full
rebuild and re-run of the existing boot test: identical stall point
(`GuC reset timed out, GDRST=0x8`) as before this phase, i.e. no
regression from the `AlchemistState`/size-constant refactor. The real,
meaningful test of this code is Phase 8 (CTB), which will exercise it
end-to-end against the actual driver - that is the point at which
"GGTT translation works" becomes a verified claim rather than a
carefully-reasoned-through one.

## Phase 8 — CTB (Command Transport Buffer) ring protocol

Mapped the real ring format directly from `abi/guc_communication_ctb_abi.h`
before writing anything: a 64-byte `struct guc_ct_buffer_desc`
(head/tail/status, offsets in DWORDS - H2G head is receiver-owned (us),
tail sender-owned (guest); reversed for G2H), and each ring message is a
CTB header dword (FENCE/FORMAT/NUM_DWORDS) followed by an embedded HXG
message - the *same* HXG format the mmio mailbox already uses, just
delivered through guest memory instead of `VF_SW_FLAG` registers.

The guest registers both rings' descriptor/ring GGTT addresses and sizes
via `SELF_CFG` KLV keys (`abi/guc_klvs_abi.h`, `0x0902`-`0x0907`) - the
mmio mailbox handler now decodes the actual key/value from each call
(previously it just acknowledged generically) and hands it to
`alchemist_ctb_register()`. `xe_guc_notify()` (`GUC_HOST_INTERRUPT`) is
reused as the doorbell for *both* the mmio mailbox and CTB sends, so its
handler now checks the H2G ring on every ring regardless of whether a
fresh mmio request was also present - safe because answering an mmio
request overwrites `VF_SW_FLAG(0)` with a RESPONSE-type message, so a
later doorbell ring for a real CTB reason naturally reads as "not a
fresh request" there.

We only answer `HXG_TYPE_REQUEST` messages over CTB - `HXG_TYPE_EVENT`
and `HXG_TYPE_FAST_REQUEST` are both explicitly documented
(`guc_messages_abi.h`) as not expecting a response, so sending one would
be protocol-incorrect.

### The interrupt cascade (a real subsystem, not a shortcut)

G2H delivery needs the guest to actually notice new data, and since our
device reports real MSI as present, `ct_needs_safe_mode()` means the
driver will *not* fall back to polling - we have to get real interrupt
delivery right, not fake it. Real Intel GPUs route interrupts through
several indirection levels rather than a flat status register - traced
directly from `xe_irq.c` (`dg1_irq_handler`/`gt_irq_handler`/
`gt_engine_identity`) and `regs/xe_irq_regs.h`:

```
DG1_MSTR_TILE_INTR (any tile pending?)
  -> GFX_MSTR_IRQ (which category - GT banks, display, ...)
    -> GT_INTR_DW(bank) (which specific source, e.g. GuC)
      -> IIR_REG_SELECTOR(bank) / INTR_IDENTITY_REG(bank): guest writes
         which bit it wants identified, polls for us to answer with an
         encoded class/instance/vector
```

Added `hw/display/alchemist/alchemist_irq.c` implementing this for real,
scoped to the one source we currently raise (GuC2Host, `XE_ENGINE_CLASS_OTHER`/
`OTHER_GUC_INSTANCE`/`GUC_INTR_GUC2HOST`) - not a shortcut, since we have
no engines/submission to raise anything else yet, but a source we don't
recognize is handled the same way real hardware would (no
`INTR_DATA_VALID` set, the driver's own ~100us poll timeout and a logged
error, not a hang).

`DG1_MSTR_TILE_INTR`/`GFX_MSTR_IRQ`/`GT_INTR_DW` are all write-1-to-clear
(standard Intel ISR/IIR convention) - confirmed directly against
`dg1_intr_disable()`'s write(0)-then-read-then-writeback pattern (a
write of 0 changes nothing under strict W1C, which is exactly "sample
the current level," not a special case). Because the generic
store-then-react dispatch every other register uses would store the
*written* value as if it were the new register value (wrong for W1C),
these three addresses are routed around it entirely via
`alchemist_irq_is_status_reg()`/`alchemist_irq_status_write()` in
`alchemist.c`'s dispatch, straight to real read-modify-write W1C logic.

### Debugging: two real bugs found via tracing, not guessed

Added `hw/display/alchemist/alchemist_ctb.c`, wired the SELF_CFG capture
and H2G-ring check into the doorbell handler, and booted - it made no
observable difference at all versus before CTB existed, same stall at
the same ~13s mark. Re-added the same host-side MMIO tracing technique
used twice before (temporary, removed before committing) and found:

1. **CT-enable itself was already succeeding even before this phase** -
   `guc_ct_control_toggle()`'s response check tolerates `data0=0`, which
   the old generic mmio-mailbox default already provided. The real
   stall was later, and CTB traffic doesn't explain it: the trace showed
   registration completing normally, then **total silence** for the
   rest of the run - nothing sent over either the mailbox or the ring.
2. Extending the trace to catch the *entire* address range in play
   (`0xc000-0xc400`, `0x190000-0x1a0000`, plus `GDRST` once its offset
   was looked up) all the way through to the actual failure showed the
   real cause: `GDRST` (`0x941c`) gets written with `0x8`
   (`xe_guc_reset()`'s `GRDOM_GUC`) and polled for hardware to clear it -
   exactly the same "write triggers an action, real hardware completes
   it and clears the trigger bit" pattern as `DMA_CTRL`, which we simply
   hadn't implemented for this register yet.

Fixed by clearing `GDRST` immediately on write (`hw/display/alchemist/alchemist_guc.c`).
That surfaced a second, more specific check one layer deeper:
`xe_guc_reset()` reads `GUC_STATUS` afterward and requires `GS_MIA_IN_RESET`
set - a real domain reset invalidates the whole prior boot state, so
rather than OR the bit in we reset `GUC_STATUS` to just that bit,
consistent with "everything else needs to be re-established via a fresh
DMA_CTRL/GUC_STATUS boot handshake," which the existing Phase 5 handler
already supports unchanged.

### Evidence

The WOPCM/DMA/mailbox/CTB/GDRST chain is now completely clean - no
errors anywhere in that machinery. Probe advances substantially further,
through interrupt-mask setup for every engine class and into real
command submission:
```
[   17.300272] xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: hwe rcs0: emit_wa_job failed (-ETIME) guc_id=1
[   17.302597] xe 0000:00:04.0: probe with driver xe failed with error -62
[   17.305242] xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: GuC RC enable mode=0 failed: -ENODEV
[   17.309233] xe 0000:00:04.0: [drm] Tile0: GT0: Kernel-submitted job timed out
```

This is a qualitatively different, new subsystem: the driver has
registered a real GuC-scheduled context (`guc_id=1`) for the render
engine (`rcs0`) and submitted an actual workaround-application batch job
to it, then waited for completion via a hardware fence - none of which
is CTB/mailbox/firmware-load territory. Getting `probe` to fully succeed
now needs at least minimal real engine/ring-buffer/context submission
support, not another register fix in the areas this phase covered.

## Phase 9a: satellite GuC coprocessor process (launch + QMP control)

The command-submission stall above is being addressed two ways: a
protocol-level fix (Phase 8, in progress separately) and, per direction,
actually running GuC's real signed firmware rather than simulating its
protocol behavior in C. Firmware execution needs a real CPU; embedding a
second CPU object in this same process would force the whole VM onto TCG
(QEMU's accelerator choice is per-process, not per-CPU -
`current_accel()` in `accel/accel-system.c` returns a single
`AccelState*` from `MachineState->accelerator`), losing KVM for the main
guest. This phase instead launches a second, independent
`qemu-system-x86_64` process - free to run its own `-accel tcg` - and
proves QMP control of it works. It does not yet load firmware, run the
satellite CPU, or relay register accesses.

`hw/display/alchemist/alchemist_guc_proc.c` is new. `fork()` +
`qemu_close_all_open_fd()` + `execv()` is the same convention
`net/tap.c`'s `net_bridge_run_helper()`/`launch_script()` use for a
long-lived helper process; the QMP client itself (connect, greeting,
`qmp_capabilities` handshake) reuses QEMU's own JSON object model
(`qobject_from_json`/`qobject_to_json`, `include/qobject/qjson.h`) the
same way `tests/qtest/libqtest.c` drives a spawned QEMU from C. Launched
from `realize()`, torn down from `exit()` - the process exists for the
device's whole lifetime, the same way the real GuC die is powered
whenever the card is, even though its boot ROM doesn't run until
`DMA_CTRL.START_DMA` (a later phase). Launch failure is non-fatal
(logged, not propagated) since nothing yet depends on the process being
up.

One real, non-obvious fix needed: `-machine none -cpu 486` alone fails
CPU realize with `apic-id property was not initialized properly` -
`none` doesn't run the generic x86 possible-CPU-list/APIC-ID machinery
real machine types do, so `-cpu` can't auto-create a default CPU the way
it does elsewhere. Confirmed directly against this build:
```
$ ./qemu-system-x86_64 -machine none -cpu 486 -nographic
QEMU 11.1.50 monitor - type 'help' for more information
qemu-system-x86_64: apic-id property was not initialized properly
```
Fixed by creating the CPU explicitly with its APIC ID set:
`-nodefaults -device 486-x86_64-cpu,apic-id=0`, confirmed working:
```
$ ./qemu-system-x86_64 -machine none -nodefaults -device 486-x86_64-cpu,apic-id=0 -nographic
(runs cleanly, no error)
```

### Evidence

Booting the guest with the alchemist device attached now also spawns a
second, correctly-parented `qemu-system-x86_64` process for the whole
life of the main VM:
```
$ ps -ef | grep qemu-system
kgerlich  693616       1  ...  qemu-system-x86_64 -M q35 ... -device alchemist,addr=04.0 ...
kgerlich  693621  693616 ...  qemu-system-x86_64 -machine none -accel tcg -nodefaults \
    -device 486-x86_64-cpu,apic-id=0 -m 16 \
    -qmp unix:/tmp/alchemist-guc-qmp-693616-0x62e867eaa3e0.sock,server=on,wait=off \
    -nographic -no-reboot -run-with exit-with-parent=on -S
```

A temporary trace (added then removed, per established practice) in
`guc_proc_qmp_handshake()` confirmed the actual JSON exchange over that
socket, captured during a real guest boot:
```
ALCHEMIST-TRACE: greeting: {"QMP": {"version": {"qemu": {"minor": 1, "micro": 50, "major": 11}, "package": ""}, "capabilities": ["oob"]}}
ALCHEMIST-TRACE: qmp_capabilities reply: {"return": {}}
ALCHEMIST-TRACE: query-status reply: {"return": {"status": "prelaunch", "running": false}}
```
(`query-status` was only sent for this trace, to independently confirm a
second command/response round-trip beyond capabilities negotiation - it
is not part of the committed code.) `running: false` confirms the
satellite genuinely started halted (`-S`), matching the "coprocessor
present but not yet told to boot" analogy.

Both teardown paths verified independently:
- Graceful: `kill -15` on the main VM (both a clean guest-triggered ACPI
  poweroff and a manual `SIGTERM`) leaves no satellite process behind -
  `pci_alchemist_exit()` -> `alchemist_guc_proc_stop()` sends
  `{"execute":"quit"}`, then `waitpid()`s.
- Hard failure: `kill -9` on the main VM (no chance for our own cleanup
  code to run at all) also leaves no satellite process behind - the
  `-run-with exit-with-parent=on` safety net catches this case
  independently.

In both cases confirmed via `ps -ef` showing zero matching processes
immediately after.

## Phase 8: protocol-level command submission (rcs0 workaround job)

Extends `alchemist_ctb_check_h2g()` to act on `HXG_TYPE_FAST_REQUEST`/
`HXG_TYPE_EVENT` message *content* (previously correctly received but
never acted on), specifically `GUC_ACTION_REGISTER_CONTEXT` (records a
`guc_id`'s LRC GGTT address) and `XE_GUC_ACTION_SCHED_CONTEXT[_MODE_SET]`
(the real "run this context's ring" trigger). New
`hw/display/alchemist/alchemist_submit.c`.

Rather than a general MI-instruction command-streamer, this recognizes
only the fixed 9-dword completion epilogue every render/compute-class job
ends with (`xe_ring_ops.c`'s `emit_job_gen12_render_compute()`: a
`PIPE_CONTROL` QW breadcrumb write, then `MI_USER_INTERRUPT`), writes the
real seqno to the real GGTT address it specifies, and raises the
interrupt cascade's `INTR_RCS0` identity (`alchemist_irq.c`, extended
from a single-source to a shared bank-0 raise helper). The batch content
before that epilogue (the actual workaround register writes) is
deliberately not interpreted - `xe_hw_fence_signaled()` only ever polls
the seqno memory location, so real hardware's own completion path
doesn't depend on that content being simulated either.

### A real bug found and fixed by testing, not guessed at

Initial implementation read `CTX_RING_TAIL` and walked backward exactly
9 dwords, assuming zero padding. Live testing showed the real epilogue
consistently starting one dword *later* than expected - `RING_TAIL` is
QWORD (8-byte) aligned, but the epilogue is 9 dwords (36 bytes, not a
multiple of 8), so `xe_lrc_write_ring()` inserts a single `MI_NOOP` (value
`0`) pad dword to reach that alignment. Fixed by searching backward from
tail for the epilogue's actual last instruction (`MI_ARB_CHECK`, a fixed,
recognizable value) across a small bounded tolerance, rather than
assuming a fixed offset. Real trace confirming this, captured before the
fix (temporary tracing, removed before commit):
```
run_context guc_id=1 ring_addr=0x670000 tail_off=0xb0 ring_size=0x4000
epilogue=[01104080 00674200 00000000 ffffff81 00000000 01000000 04000001 02800000 00000000]
want=[7a000004 01104080 .. 01000000]
```
(`epilogue[3]=0xffffff81` is `XE_FENCE_INITIAL_SEQNO` as a `u32` - real,
expected data, confirming the read itself was correct and only the
window's starting offset was off by one dword.)

Also found and fixed: `CTX_RING_START` is a 32-bit register, but the
first implementation read it directly into a `uint64_t` via a 4-byte
`alchemist_ggtt_read()` - the upper 32 bits were left as uninitialized
stack garbage, which would have made the ring address essentially random
whenever the compiler happened to leave nonzero bits there. Fixed by
reading into a `uint32_t` and assigning (zero-extending) into the 64-bit
address used for GGTT calls.

### Evidence

The originally-targeted stall is gone - `hwe rcs0`'s workaround job (the
exact one this project's earlier evidence showed timing out with `-ETIME`,
`guc_id=1`) now completes, and probe advances substantially further,
setting up further GuC-scheduled contexts:
```
REGISTER_CONTEXT guc_id=1 hwlrca=0x674019 lrc_ggtt_addr=0x674000
SCHED_CONTEXT_MODE_SET guc_id=1 enable=1   -> rcs0 job completes, no -ETIME
REGISTER_CONTEXT guc_id=2 ...
REGISTER_CONTEXT guc_id=3 hwlrca=0x6b4019 lrc_ggtt_addr=0x6b4000
SCHED_CONTEXT_MODE_SET guc_id=3 enable=1
[   16.568477] xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: hwe bcs0: emit_wa_job failed (-ETIME) guc_id=3
[   16.568595] xe 0000:00:04.0: probe with driver xe failed with error -62
```
Probe now fails on a **different, structurally distinct** engine class -
`bcs0` (the blitter/copy engine), `guc_id=3` - not another rcs0 problem.
Confirmed directly from source
(`drivers/gpu/drm/xe/xe_ring_ops.c`): `XE_ENGINE_CLASS_COPY` uses
`ring_ops_gen12_copy` -> `emit_job_gen12_copy()` ->
`__emit_job_gen12_simple()`, a genuinely different (and simpler)
completion sequence with **no `PIPE_CONTROL` at all** - a plain
`MI_STORE_DATA_IMM` breadcrumb instead. This is real, new, additional
scope (recognizing a second, different fixed epilogue and a second
interrupt identity/engine class), not a bug in the rcs0 fix - reproduced
identically across a clean rebuild with all temporary tracing removed.

Reproduced cleanly with the temporary tracing removed, confirming this
milestone doesn't depend on it:
```
[   16.568477] xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: hwe bcs0: emit_wa_job failed (-ETIME) guc_id=3
```
(same stall, same guc_id, across two independent boots - not a fluke.)

## Phase 8b: extend command submission to bcs0 (blitter/copy engine)

`alchemist_submit.c` tracks each context's `engine_class` (already present
in `GUC_ACTION_REGISTER_CONTEXT`'s payload, previously ignored) and
dispatches to one of two recognized ring-epilogue shapes, confirmed
directly from `xe_ring_ops.c`:

- `XE_ENGINE_CLASS_RENDER` -> `emit_job_gen12_render_compute()`'s 9-dword
  `PIPE_CONTROL`-based epilogue (already implemented in Phase 8).
- `XE_ENGINE_CLASS_COPY` -> `__emit_job_gen12_simple()`'s different,
  shorter 8-dword `MI_FLUSH_DW`-based epilogue - no `PIPE_CONTROL` at
  all, confirmed literal encoding from source
  (`emit_flush_imm_ggtt()`/`emit_store_imm_ggtt()`).

Both endings share the identical 3-dword `emit_user_interrupt()` suffix
(`MI_USER_INTERRUPT`, `MI_ARB_ON_OFF|ENABLE`, `MI_ARB_CHECK`) - the
tail-search logic added in Phase 8 (tolerating the QWORD-alignment pad
dword) is reused unchanged as a shared anchor-finder, then the
class-appropriate epilogue shape is checked ending at that anchor.
`alchemist_irq.c` gained a third raised source, `INTR_BCS0` (bit 15 of
the same bank-0 `GT_INTR_DW` register `INTR_GUC`/`INTR_RCS0` already
use - confirmed from `regs/xe_irq_regs.h` that Copy/Render/GuC don't
collide in that bank; Video classes use different, lower bits that would
collide with `INTR_RCS0` were they in the same bank, confirming they
must live in a different bank not yet modeled).

### Evidence

Both engine classes' workaround jobs now complete cleanly - zero
`emit_wa_job`/`-ETIME` failures anywhere in the log, reproduced across
two independent clean boots. Probe now advances into a completely
different, unrelated subsystem - GuC PC (power/frequency control), not
command submission at all:
```
[   15.418109] xe 0000:00:04.0: [drm] Tile0: GT0: GuC PC start taking longer than normal [freq = 0MHz (req = 0MHz), perf_limit_reasons = 0x00000000]
[   16.418126] xe 0000:00:04.0: [drm] *ERROR* Tile0: GT0: GuC PC Start failed: Dynamic GT frequency control and GT sleep states are now disabled.
[   16.418219] xe 0000:00:04.0: probe with driver xe failed with error -5
```
This is real, new, unresearched territory (GuC PC's own mmio mailbox
handshake/register interface for GT frequency requests) - not a command-
submission problem, and not attempted in this commit.
