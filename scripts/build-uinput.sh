#!/usr/bin/env bash
# Optional dependency for development kernels built without CONFIG_INPUT_UINPUT.
# Does not modify the kernel source tree or install/load anything.
set -Eeuo pipefail
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
kernel_dir="/lib/modules/$(uname -r)/build"
kernel_src=$(readlink -f "$kernel_dir")
state_dir=$(mktemp -d /tmp/himax-uinput-build.XXXXXX)
trap 'rm -rf -- "$state_dir"' EXIT
[[ -f "$kernel_src/drivers/input/misc/uinput.c" ]] || {
	echo "Need the matching kernel source with drivers/input/misc/uinput.c" >&2
	exit 1
}
mkdir -p "$state_dir/input/misc" "$repo_dir/artifacts"
cp "$kernel_src/drivers/input/misc/uinput.c" "$state_dir/input/misc/"
cp "$kernel_src/drivers/input/input-compat.h" "$state_dir/input/"
cp "$repo_dir/scripts/uinput.Makefile" "$state_dir/input/misc/Makefile"
make -C "$kernel_dir" M="$state_dir/input/misc" -j4 modules
install -m644 "$state_dir/input/misc/uinput.ko" "$repo_dir/artifacts/uinput.ko"
echo "Built $repo_dir/artifacts/uinput.ko; enable CONFIG_INPUT_UINPUT=m in future kernels"
