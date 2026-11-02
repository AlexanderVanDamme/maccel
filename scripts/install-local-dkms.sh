#!/bin/sh
# Install the driver from THIS checkout through DKMS (instead of the upstream
# install.sh, which re-clones upstream into /opt/maccel).
#
# Run from the repository root:  sudo sh scripts/install-local-dkms.sh
#
# What it does:
#   1. removes older maccel DKMS versions so only one maccel.ko is installed
#   2. registers driver/ as maccel/<version> and builds it for the running kernel
#   3. installs the udev rules that fix parameter permissions on boot
#   4. loads the module and installs the CLI built in target/release (if present)

set -e

VERSION="${VERSION:-0.6.0.lut1}"
DEBUG_CFLAGS=""
if [ "${DEBUG:-0}" -eq 1 ]; then
  DEBUG_CFLAGS="-g -DDEBUG"
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root: sudo sh $0" >&2
  exit 1
fi

cd "$(dirname "$0")/.."

echo "==> unloading the current module (fine if it is not loaded)"
rmmod maccel 2>/dev/null || true

echo "==> removing other DKMS versions of maccel"
for v in $(dkms status maccel 2>/dev/null | sed -n 's|^maccel/\([^,:]*\).*|\1|p' | sort -u); do
  if [ "$v" != "$VERSION" ]; then
    echo "    removing maccel/$v"
    dkms remove "maccel/$v" --all || true
  fi
done
if dkms status "maccel/$VERSION" 2>/dev/null | grep -q .; then
  dkms remove "maccel/$VERSION" --all || true
fi

echo "==> registering maccel/$VERSION from $(pwd)/driver"
rm -rf "/usr/src/maccel-$VERSION"
mkdir -p "/usr/src/maccel-$VERSION"
cp -r driver/. "/usr/src/maccel-$VERSION/"
sed -e "s/@_PKGNAME@/maccel/" \
    -e "s/@PKGVER@/$VERSION/" \
    -e "s/@DRIVER_CFLAGS@/'$DEBUG_CFLAGS'/" \
    dkms.conf > "/usr/src/maccel-$VERSION/dkms.conf"

echo "==> building and installing through DKMS"
dkms install --force "maccel/$VERSION"

echo "==> installing udev rules"
install -m 644 -D udev_rules/99-maccel.rules /usr/lib/udev/rules.d/99-maccel.rules
install -m 755 -D udev_rules/maccel_param_ownership_and_resets /usr/lib/udev/maccel_param_ownership_and_resets
udevadm control --reload-rules

echo "==> loading the module"
groupadd -f maccel
modprobe maccel
sleep 1
ls /sys/module/maccel/parameters/LUT_X /sys/module/maccel/parameters/LUT_Y

if [ -x target/release/maccel ]; then
  echo "==> installing the CLI"
  install -m 755 target/release/maccel /usr/local/bin/maccel
fi

echo "done. If parameters are not group-writable yet, run:"
echo "  sudo /usr/lib/udev/maccel_param_ownership_and_resets"
