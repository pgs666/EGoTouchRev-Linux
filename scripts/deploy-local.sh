#!/usr/bin/env bash
# Build/install/reload himax-spi while guaranteeing that a known-good module
# is restored if DKMS, probe, binding, or input registration fails.
set -Eeuo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
src_dir="$repo_dir/touchscreen-hx83121a-dkms"
module_name=himax_spi
dkms_name=himax-spi
dkms_version=0.1
spi_device=spi0.0
kernel_version=$(uname -r)
machine_arch=$(uname -m)
dkms_src="/usr/src/${dkms_name}-${dkms_version}"
dkms_module="/var/lib/dkms/${dkms_name}/${dkms_version}/${kernel_version}/${machine_arch}/module/himax-spi.ko"
log_dir="$repo_dir/deploy-logs"
stamp=$(date +%Y%m%d-%H%M%S)
log_file="$log_dir/$stamp.log"
state_dir=$(mktemp -d "/tmp/himax-spi-deploy.XXXXXX")
old_module=$(modinfo -n "$module_name" 2>/dev/null || true)
backup_module="$state_dir/himax-spi.ko.backup"
sysfs_state="$state_dir/sysfs.tsv"
algo_dir="/sys/bus/spi/devices/$spi_device/algo"
driver_link="/sys/bus/spi/devices/$spi_device/driver"
old_input_count=$(awk '/^N: Name="Himax Capacitive TouchScreen"$/{n++} END{print n+0}' \
	/proc/bus/input/devices)
rollback_needed=1
himax_diagnostics="${HIMAX_DIAGNOSTICS:-0}"

case "$himax_diagnostics" in
	0|1) ;;
	*) echo "HIMAX_DIAGNOSTICS must be 0 or 1" >&2; exit 2 ;;
esac

mkdir -p "$log_dir"
exec > >(tee -a "$log_file") 2>&1

cleanup() { rm -rf "$state_dir"; }
rollback() {
	local status=$?
	if (( rollback_needed )); then
		echo "deployment failed (status $status); restoring previous module"
		sudo modprobe -r "$module_name" 2>/dev/null || true
		if [[ -s "$backup_module" && -n "$old_module" ]]; then
			sudo install -Dm644 "$backup_module" "$old_module"
			sudo depmod -a "$kernel_version"
		fi
		sudo modprobe "$module_name" || true
	fi
	cleanup
	exit "$status"
}
trap rollback ERR INT TERM

if [[ -n "$old_module" && -f "$old_module" ]]; then
	cp --preserve=mode,timestamps "$old_module" "$backup_module"
fi

: > "$sysfs_state"
while IFS= read -r path; do
	[[ -r "$path" ]] || continue
	case "$(basename "$path")" in
		diagnostics|event_stack_hex) continue ;;
	esac
	printf '%s\t%s\n' "$(basename "$path")" "$(<"$path")" >> "$sysfs_state"
done < <(find "$algo_dir" -maxdepth 1 -type f 2>/dev/null | sort)

echo "checkpoint=$stamp kernel=$kernel_version old_module=${old_module:-none}"
make -C "$repo_dir/tests" clean test
make -C "$src_dir" clean all HIMAX_DIAGNOSTICS="$himax_diagnostics"
repo_hash=$(sha256sum "$src_dir/himax-spi.ko" | awk '{print $1}')

[[ "$dkms_src" == "/usr/src/himax-spi-0.1" ]]
sudo install -d "$dkms_src"
sudo find "$dkms_src" -mindepth 1 -delete
for file in Makefile dkms.conf himax-spi-core.c himax-spi-hw.c \
		himax-spi-sysfs.c himax-spi.h hx-algo.c hx-algo-preprocess.c \
		hx-algo-detect.c hx-algo-resolve.c hx-algo-track.c hx-algo.h \
		hx-algo-internal.h hx-frame-status.h; do
	sudo install -m644 "$src_dir/$file" "$dkms_src/$file"
done
sudo install -Dm644 "$src_dir/himax-spi-dkms.conf" /etc/modprobe.d/himax-spi-dkms.conf
sudo dkms install --force -m "$dkms_name" -v "$dkms_version" -k "$kernel_version"
installed_module=$(modinfo -n "$module_name")
# DKMS rebuilds in a different path, which changes otherwise harmless debug
# metadata.  Install the already-tested repository artifact so all three
# copies have the same bytes/build-id.
sudo install -m644 "$src_dir/himax-spi.ko" "$dkms_module"
sudo install -m644 "$src_dir/himax-spi.ko" "$installed_module"
sudo depmod -a "$kernel_version"

log_since=$(date --iso-8601=seconds)
sudo modprobe -r "$module_name"
sudo modprobe "$module_name"

while IFS=$'\t' read -r name value; do
	[[ -e "$algo_dir/$name" ]] || continue
	printf '%s\n' "$value" | sudo tee "$algo_dir/$name" >/dev/null ||
		echo "skipped stale/invalid sysfs value: $name=$value"
done < "$sysfs_state"

bound_driver=$(basename "$(readlink -f "$driver_link" 2>/dev/null || true)")
[[ "$bound_driver" == "himax-spi" ]]
new_input_count=$(awk '/^N: Name="Himax Capacitive TouchScreen"$/{n++} END{print n+0}' \
	/proc/bus/input/devices)
if (( old_input_count > 0 )); then
	(( new_input_count > 0 ))
fi
disk_hash=$(sha256sum "$installed_module" | awk '{print $1}')
[[ "$repo_hash" == "$disk_hash" ]]
installed_build_id=$(readelf -n "$installed_module" | awk '/Build ID:/{print $3; exit}')
running_build_id=$(od -An -tx1 -v "/sys/module/$module_name/notes/.note.gnu.build-id" |
	tr -d ' \n' | sed 's/^040000001400000003000000474e5500//')
[[ -n "$installed_build_id" && "$running_build_id" == "$installed_build_id" ]]

echo "repo_sha256=$repo_hash"
echo "installed_module=$installed_module disk_sha256=$disk_hash"
echo "build_id=$running_build_id spi_driver=$bound_driver input_count=$new_input_count"
sudo journalctl -k --since "$log_since" --no-pager | grep -Ei 'himax|spi0\.0|touch' || true

rollback_needed=0
trap - ERR INT TERM
cleanup
echo "deployment verified; log=$log_file"
