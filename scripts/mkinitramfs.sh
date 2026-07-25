#!/bin/sh
# mkinitramfs.sh: build a minimal busybox initramfs for the guest.
#
# usage: mkinitramfs.sh <busybox-static-binary> <output.cpio.gz> [extra-dir]
#
# The initramfs is the kernel's first (and, here, only) userspace: a
# gzipped cpio archive the kernel unpacks into a tmpfs and whose /init
# it execs as PID 1. Anything in [extra-dir] is copied into the image
# root (used to ship kernel modules + tests into the guest).
set -eu

BUSYBOX=${1:?path to static busybox}
OUT=${2:?output .cpio.gz}
EXTRA=${3:-}

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

mkdir -p "$ROOT/bin" "$ROOT/sbin" "$ROOT/proc" "$ROOT/sys" \
         "$ROOT/dev" "$ROOT/tmp" "$ROOT/etc"
cp "$BUSYBOX" "$ROOT/bin/busybox"
chmod 755 "$ROOT/bin/busybox"

if [ -n "$EXTRA" ]; then
    cp -a "$EXTRA"/. "$ROOT"/
fi

cat > "$ROOT/init" << 'EOF'
#!/bin/busybox sh
# PID 1. If this script exits, the kernel panics, hence `exec` at the end.
/bin/busybox mkdir -p /proc /sys /dev /tmp
/bin/busybox mount -t proc proc /proc
/bin/busybox --install -s /bin      # symlink every applet (needs /proc)
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

echo
echo '  ┌───────────────────────────────────────────────┐'
echo '  │  Linux, booted by embervisor                  │'
echo '  │  (a from-scratch KVM VMM in ~1.5k lines of C) │'
echo '  └───────────────────────────────────────────────┘'
uname -a
echo
# Automation hook: booted with ember.probe=1 and shipped a /probe.sh?
# Run it instead of handing over a shell (used by the nested test rig).
case " $(cat /proc/cmdline) " in
  *" ember.probe=1 "*) [ -x /probe.sh ] && exec /probe.sh ;;
esac
# cttyhack gives the shell a controlling terminal => job control, ^C
exec setsid cttyhack sh
EOF
chmod 755 "$ROOT/init"

(cd "$ROOT" && find . | cpio -o -H newc --quiet | gzip -9) > "$OUT"
echo "initramfs: $(du -h "$OUT" | cut -f1) at $OUT"
