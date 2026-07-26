#!/bin/sh
# run-nested.sh: test embervisor on a machine with NO /dev/kvm.
#
# The trick: boot our kernel as an "L1" guest under pure-TCG QEMU
# (no KVM anywhere), with an emulated AMD CPU that *exposes SVM*. L1's
# kernel sees SVM, initializes kvm_amd against QEMU's emulation of it,
# and offers a real /dev/kvm. embervisor runs inside L1 exactly as it
# would on bare metal: same ioctls, same exits, only slower, since
# every L2 instruction is ultimately interpreted.
#
#   L0: this machine, QEMU TCG (pure emulation, no virt hardware needed)
#   L1: our kernel + busybox, /dev/kvm courtesy of emulated SVM
#   L2: whatever embervisor boots (hello payload, or Linux again)
#
# usage: run-nested.sh [hello|linux|kindling|shell]
#   hello      embervisor runs the bare-metal serial payload in L2
#   linux      embervisor boots Linux-in-Linux to an automated probe
#   kindling   no nesting: load + test the kindling modules in L1
#   shell      interactive L1 shell to poke around (default)
set -eu

MODE=${1:-shell}
HERE=$(cd "$(dirname "$0")/.." && pwd)          # embervisor/
KC=$(cd "$HERE/.." && pwd)                      # kernelcraft/
KERNEL=$KC/linux-6.12.43/arch/x86/boot/bzImage
BB=$(command -v busybox) || { echo "need busybox-static"; exit 1; }

[ -e "$KERNEL" ] || { echo "build the kernel first (build-kernel.sh)"; exit 1; }
[ -e "$HERE/embervisor-static" ] || { echo "make embervisor-static first"; exit 1; }

STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT

# --- inner initramfs: L2's userspace, with an automated probe aboard ---
mkdir -p "$STAGE/extra"
cat > "$STAGE/extra/probe.sh" << 'EOF'
#!/bin/sh
echo "== L2 probe: Linux is alive under embervisor =="
uname -a
grep -m1 'model name' /proc/cpuinfo || true
grep MemTotal /proc/meminfo
echo "irq lines visible: $(wc -l < /proc/interrupts)"
dd if=/dev/zero of=/tmp/x bs=1M count=8 2>&1 | tail -1
echo "== L2 probe complete, rebooting (reboot=t: triple-fault exit) =="
reboot -f
EOF
chmod 755 "$STAGE/extra/probe.sh"
"$HERE/scripts/mkinitramfs.sh" "$BB" "$STAGE/inner.cpio.gz" "$STAGE/extra"

# --- L1 initramfs: busybox + embervisor + payloads + kindling modules ---
R="$STAGE/root"
mkdir -p "$R/bin" "$R/ember" "$R/kindling"
cp "$BB" "$R/bin/busybox"; chmod 755 "$R/bin/busybox"
cp "$HERE/embervisor-static" "$R/ember/embervisor"
cp "$HERE/guests/hello.bin" "$R/ember/hello.bin"
cp "$KERNEL" "$R/ember/bzImage"
cp "$STAGE/inner.cpio.gz" "$R/ember/inner.cpio.gz"
if ls "$KC"/kindling/*.ko > /dev/null 2>&1; then
    cp "$KC"/kindling/*.ko "$R/kindling/"
    cp "$KC/kindling/test/run-tests.sh" "$R/kindling/run-tests.sh"
    [ -x "$KC/kindling/test/vault_test" ] && \
        cp "$KC/kindling/test/vault_test" "$R/kindling/vault_test"
fi

cat > "$R/init" << 'EOF'
#!/bin/busybox sh
/bin/busybox mkdir -p /proc /sys /dev /tmp
/bin/busybox mount -t proc proc /proc
/bin/busybox --install -s /bin
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

MODE=shell
for w in $(cat /proc/cmdline); do
    case "$w" in ember.mode=*) MODE=${w#ember.mode=} ;; esac
done

echo
echo "== L1 up: $(uname -sr), mode=$MODE =="
if [ -c /dev/kvm ]; then
    echo "== /dev/kvm present (kvm_amd on QEMU's emulated SVM) =="
else
    echo "!! no /dev/kvm in L1: kvm_amd failed to init"
    dmesg | grep -iE "kvm|svm" | tail -5
    [ "$MODE" = shell ] || poweroff -f
fi

case "$MODE" in
hello)
    /ember/embervisor --flat /ember/hello.bin
    echo "== rig: hello mode complete (exit $?) =="
    poweroff -f ;;
linux)
    echo "== embervisor booting L2 Linux (every instruction interpreted, minutes not seconds) =="
    /ember/embervisor --kernel /ember/bzImage --initrd /ember/inner.cpio.gz \
        --cmdline "console=ttyS0 reboot=t panic=-1 i8042.nokbd i8042.noaux rdinit=/init ember.probe=1"
    echo "== rig: linux mode complete =="
    poweroff -f ;;
kindling)
    cd /kindling && sh run-tests.sh
    poweroff -f ;;
*)
    exec setsid cttyhack sh ;;
esac
EOF
chmod 755 "$R/init"

(cd "$R" && find . | cpio -o -H newc --quiet | gzip -1) > "$STAGE/l1.cpio.gz"

# Per-mode machine config:
#  - kindling needs no /dev/kvm, so it gets 2 CPUs under MTTCG. The
#    kthread_race demo needs real SMP for the lost updates to show.
#  - the embervisor modes need QEMU's emulated SVM, which is happiest
#    single-threaded, and with NPT *disabled*: QEMU's NPT emulation
#    sent kvm_amd into an endless nested-page-fault loop (diagnosed
#    via /proc/<pid>/stack: xfer_to_guest_mode_handle_work forever);
#    without the npt bit, kvm_amd falls back to shadow paging, which
#    works. War story in docs/notes.md.
case "$MODE" in
kindling) CPUFLAGS="-smp 2 -accel tcg,thread=multi -cpu qemu64" ;;
*)        CPUFLAGS="-smp 1 -accel tcg,thread=single -cpu qemu64,vendor=AuthenticAMD,+svm" ;;
esac

echo "rig: L1 initramfs $(du -h "$STAGE/l1.cpio.gz" | cut -f1), starting QEMU (pure TCG)..."
exec qemu-system-x86_64 \
    -M pc -m 2048 $CPUFLAGS \
    -kernel "$KERNEL" \
    -initrd "$STAGE/l1.cpio.gz" \
    -append "console=ttyS0 ember.mode=$MODE" \
    -nographic -no-reboot
