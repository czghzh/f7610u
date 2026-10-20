#!/bin/sh
# Configure ImmortalWrt to build a U-Boot FIT initramfs for the ZXSLC F7610U.
set -eu

TOPDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEFAULT_KERNEL_TREE="$TOPDIR/../linux-mainline-zte-zxslc-sr1010/linux-6.18.38"
KERNEL_TREE=${KERNEL_TREE:-$DEFAULT_KERNEL_TREE}

if [ ! -f "$KERNEL_TREE/Makefile" ] || [ ! -f "$KERNEL_TREE/arch/arm64/boot/dts/zte/zx279131-f7610u.dts" ]; then
	echo "invalid ZX279131 kernel tree: $KERNEL_TREE" >&2
	exit 1
fi

KERNEL_TREE=$(CDPATH= cd -- "$KERNEL_TREE" && pwd)
cd "$TOPDIR"

if [ -f .config ]; then
	mkdir -p tmp
	cp .config tmp/.config.before-f7610u
	echo "saved previous configuration as tmp/.config.before-f7610u"
fi

cp configs/zte_zx279131_f7610u_initramfs.config .config
printf 'CONFIG_EXTERNAL_KERNEL_TREE="%s"\n' "$KERNEL_TREE" >> .config

make defconfig

cat <<EOF

F7610U initramfs configuration is ready.
Build with:
  make -j\$(nproc)

Expected image:
  bin/targets/zte/zx279131/immortalwrt-zte-zx279131-zte_f7610u-initramfs.itb
EOF
