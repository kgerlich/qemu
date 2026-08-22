# Testing `invertram` from a Linux guest

`hw/misc/invertram.c` is a minimal educational PCI device (vendor:device
`1234:11e9`): it exposes a 4KB MMIO BAR0. Any write to an offset is
bitwise-inverted and stored; a read of that offset returns the stored
(already-inverted) value.

`tests/qtest/invertram-test.c` already exercises the device via QEMU's
qtest harness, with no guest OS involved. This document covers the
alternative: driving the device from real Linux userspace running inside
QEMU, using `mmap()` on the BAR through sysfs — the same technique real
PCI userspace drivers use.

Build QEMU first (see the top-level `README.rst` / `docs/devel/build-system.rst`
for the general build instructions); this doc assumes an `x86_64-softmmu`
build already exists under `build/`.

## 1. Get a kernel and a minimal root filesystem

Alpine Linux publishes a small prebuilt x86_64 kernel and a minimal
rootfs tarball that are convenient for exactly this kind of device
smoke-testing:

```
mkdir -p /tmp/invertram-guest && cd /tmp/invertram-guest
curl -sL -o alpine-minirootfs.tar.gz \
  https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64/alpine-minirootfs-3.24.0-x86_64.tar.gz
curl -sL -o vmlinuz-virt \
  https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64/netboot/vmlinuz-virt
mkdir rootfs
sudo tar -xzf alpine-minirootfs.tar.gz -C rootfs
```

(Check `https://dl-cdn.alpinelinux.org/alpine/` for the current release
branch if `v3.24` is no longer current.)

## 2. A static test helper

Alpine's BusyBox build doesn't include the `devmem` applet, so use a tiny
static C program instead. It finds the device by scanning
`/sys/bus/pci/devices/*/vendor` and `.../device`, then `mmap()`s
`resource0` (the BAR0 sysfs file) directly — no `/dev/mem` or physical
address math required.

Save as `test_invert.c`:

```c
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>

int main(void)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *e;
    char path[256], vpath[256], dpath[256];
    char vbuf[16], dbuf[16];
    int found = 0;

    if (!d) {
        perror("opendir");
        return 1;
    }

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(vpath, sizeof(vpath), "/sys/bus/pci/devices/%s/vendor", e->d_name);
        snprintf(dpath, sizeof(dpath), "/sys/bus/pci/devices/%s/device", e->d_name);
        FILE *fv = fopen(vpath, "r");
        FILE *fdv = fopen(dpath, "r");
        if (!fv || !fdv) {
            if (fv) fclose(fv);
            if (fdv) fclose(fdv);
            continue;
        }
        fgets(vbuf, sizeof(vbuf), fv);
        fgets(dbuf, sizeof(dbuf), fdv);
        fclose(fv);
        fclose(fdv);
        if (strncmp(vbuf, "0x1234", 6) == 0 && strncmp(dbuf, "0x11e9", 6) == 0) {
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);

    if (!found) {
        printf("INVERTRAM: device 1234:11e9 not found on PCI bus\n");
        return 1;
    }

    printf("INVERTRAM: found device, BAR0 at %s\n", path);

    int fd_bar = open(path, O_RDWR | O_SYNC);
    if (fd_bar < 0) {
        perror("open resource0");
        return 1;
    }

    volatile uint32_t *bar = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd_bar, 0);
    if (bar == MAP_FAILED) {
        perror("mmap");
        close(fd_bar);
        return 1;
    }

    uint32_t testvals[] = { 0x12345678, 0x00000000, 0xFFFFFFFF, 0xDEADBEEF };
    int fail = 0;

    for (int i = 0; i < 4; i++) {
        bar[i] = testvals[i];
        uint32_t got = bar[i];
        uint32_t want = ~testvals[i];
        printf("  wrote 0x%08x -> read 0x%08x (expected 0x%08x) %s\n",
               testvals[i], got, want, got == want ? "OK" : "FAIL");
        if (got != want) {
            fail = 1;
        }
    }

    munmap((void *)bar, 4096);
    close(fd_bar);

    printf(fail ? "INVERTRAM TEST: FAIL\n" : "INVERTRAM TEST: PASS\n");
    return fail;
}
```

Compile it **statically** on the host, so it runs unmodified inside the
musl-based Alpine guest without needing a matching libc:

```
gcc -static -O2 -o test_invert test_invert.c
```

## 3. A minimal init script

The guest doesn't need a real init system — just enough to mount the
pseudo-filesystems, run the test, and power off. Save as `init`:

```sh
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || mdev -s

echo "=== lspci-equivalent: PCI devices seen by the kernel ==="
for d in /sys/bus/pci/devices/*; do
    printf '%s vendor=%s device=%s\n' "$d" "$(cat "$d/vendor")" "$(cat "$d/device")"
done

echo "=== running invertram guest-side test ==="
/test_invert
echo "=== test exit code: $? ==="

sleep 1
poweroff -f
```

## 4. Assemble the initramfs

```
cp test_invert rootfs/test_invert
cp init rootfs/init
sudo chown root:root rootfs/init rootfs/test_invert
chmod +x rootfs/init rootfs/test_invert
(cd rootfs && find . | cpio -o -H newc 2>/dev/null | gzip -1) > initramfs.cpio.gz
```

## 5. Boot it with `invertram` attached

From the QEMU build directory:

```
./qemu-system-x86_64 -M q35 -m 512 \
  -kernel /tmp/invertram-guest/vmlinuz-virt \
  -initrd /tmp/invertram-guest/initramfs.cpio.gz \
  -append "console=ttyS0 quiet" \
  -device invertram,addr=04.0 \
  -nographic -no-reboot
```

Boots to a pass/fail result in a couple of seconds:

```
=== running invertram guest-side test ===
INVERTRAM: found device, BAR0 at /sys/bus/pci/devices/0000:00:04.0/resource0
  wrote 0x12345678 -> read 0xedcba987 (expected 0xedcba987) OK
  wrote 0x00000000 -> read 0xffffffff (expected 0xffffffff) OK
  wrote 0xffffffff -> read 0x00000000 (expected 0x00000000) OK
  wrote 0xdeadbeef -> read 0x21524110 (expected 0x21524110) OK
INVERTRAM TEST: PASS
=== test exit code: 0 ===
```

Drop `quiet` from `-append` to see the full kernel boot log alongside the
test output.

## 6. Speeding it up with KVM

By default the run above uses TCG (software emulation of every guest
instruction). If the host has hardware virtualization, `-accel kvm -cpu
host` gets a real speedup — the boot above drops from a few seconds of
CPU-bound emulation to about 2 seconds wall-clock with well under a
second of actual CPU time:

```
./qemu-system-x86_64 -M q35 -m 512 -accel kvm -cpu host \
  -kernel /tmp/invertram-guest/vmlinuz-virt \
  -initrd /tmp/invertram-guest/initramfs.cpio.gz \
  -append "console=ttyS0 quiet" \
  -device invertram,addr=04.0 \
  -nographic -no-reboot
```

Two prerequisites, both diagnosable from inside the box:

```
grep -oE 'vmx|svm' /proc/cpuinfo   # empty output means no HW virt exposed at all
ls -la /dev/kvm                    # must exist; group is usually "kvm"
groups                             # your user must be in that group
```

If `vmx`/`svm` is missing from `/proc/cpuinfo` and this machine is itself
a VM (check with `systemd-detect-virt`), the fix is one level up, on
whatever hypervisor hosts it:

- Nested virtualization must be enabled on the *hypervisor host's* kernel
  module: `cat /sys/module/kvm_intel/parameters/nested` (Intel) or
  `cat /sys/module/kvm_amd/parameters/nested` (AMD) should read `1`/`Y`.
  If not, `echo "options kvm_intel nested=1" > /etc/modprobe.d/kvm-nested.conf`
  (swap in `kvm_amd` as appropriate) and reload the module or reboot the
  host.
- The VM's CPU model must actually expose the flag to the guest — a
  generic CPU type (e.g. Proxmox's `x86-64-v2-AES`, or QEMU's `qemu64`)
  strips virtualization extensions. Set it to `host`. On Proxmox:
  `qm set <vmid> --cpu host`, then `qm reboot <vmid>` (a running VM won't
  pick up a CPU-type change without a full stop/start — `qm reboot` does
  this for you).

If `/dev/kvm` exists but isn't accessible, add your user to its owning
group and open a new login session (group membership is read at login
time, so an already-open shell won't pick it up):

```
sudo usermod -aG kvm $USER
```
