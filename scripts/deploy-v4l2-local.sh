#!/usr/bin/env bash
# --trial runs a five-second raw capture and a short solver service test, then
# restores the previous module. Default installs the tested V4L2 stack.
set -Eeuo pipefail
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
trial=0
trial_seconds=${HIMAX_TRIAL_SECONDS:-5}
trial_afe=${HIMAX_TRIAL_AFE:-none}
afe_diagnostics=${HIMAX_AFE_DIAGNOSTICS:-0}
[[ "$trial_seconds" =~ ^[0-9]+$ ]] && (( trial_seconds >= 5 && trial_seconds <= 120 )) || {
	echo "HIMAX_TRIAL_SECONDS must be between 5 and 120" >&2
	exit 2
}
case "${1:-}" in
	--trial) trial=1 ;;
	"") ;;
	*) echo "Usage: bash scripts/deploy-v4l2-local.sh [--trial]" >&2; exit 2 ;;
esac
case "$trial_afe" in
	none) ;;
	basic|full|frequency)
		(( trial )) || { echo "HIMAX_TRIAL_AFE requires --trial" >&2; exit 2; }
		afe_diagnostics=1
		;;
	*) echo "HIMAX_TRIAL_AFE must be none, basic, full or frequency" >&2; exit 2 ;;
esac
[[ "$afe_diagnostics" == 0 || "$afe_diagnostics" == 1 ]] || {
	echo "HIMAX_AFE_DIAGNOSTICS must be 0 or 1" >&2
	exit 2
}
kernel_version=$(uname -r)
dkms_tree=/var/lib/dkms/himax-spi
dkms_module="$dkms_tree/0.1/$kernel_version/$(uname -m)/module/himax-spi.ko"
stamp=$(date +%Y%m%d-%H%M%S)
state_dir="$repo_dir/deploy-logs/v4l2-$stamp"
unit="himax-touchd-trial-$stamp"
old_module=$(modinfo -n himax_spi 2>/dev/null || true)
changed=0
persistent=0
module_unloaded=0
runtime_mask_added=0
old_service_active=0
old_service_enabled=0
systemctl is-active --quiet himax-touchd.service && old_service_active=1
systemctl is-enabled --quiet himax-touchd.service && old_service_enabled=1
installed_paths=(
	/usr/local/bin/himax-touchd
	/usr/local/bin/himax-replay
	/usr/local/lib/systemd/system/himax-touchd.service
	/usr/local/bin/himax-screen-bridge
	/usr/local/lib/systemd/user/himax-screen-bridge.service
	/usr/local/lib/udev/rules.d/70-himax-raw.rules
	/usr/local/lib/modules-load.d/himax-touchd.conf
	/etc/modprobe.d/himax-spi-dkms.conf
	"/lib/modules/$kernel_version/extra/uinput.ko"
)
mkdir -p "$state_dir"
exec 9>"$repo_dir/deploy-logs/.v4l2-deploy.lock"
flock -n 9 || { echo "Another V4L2 deployment is running" >&2; exit 1; }
exec > >(tee -a "$state_dir/deploy.log") 2>&1
echo "checkpoint=$state_dir"
echo "afe_diagnostics=$afe_diagnostics trial_afe=$trial_afe"
[[ -f "$old_module" && "$old_module" == *.ko ]] || {
	echo "Need an existing uncompressed known-good himax-spi.ko for rollback" >&2
	exit 1
}
cp "$old_module" "$state_dir/previous-himax-spi.ko"
printf '%s\n' "$old_module" > "$state_dir/previous-module-path"

module_paths() {
	find "/lib/modules/$kernel_version" -type f \
		\( -name himax-spi.ko -o -name himax-spi.ko.xz \
		-o -name himax-spi.ko.zst -o -name himax-spi.ko.gz \) -print0
}

load_dependencies() {
	local dependencies
	local -a modules

	dependencies=$(modinfo -F depends "$1") || return
	IFS=, read -r -a modules <<< "$dependencies"
	if (( ${#modules[@]} )); then
		sudo modprobe -a -- "${modules[@]}" || return
	fi
}

unmask_trial_service() {
	if (( runtime_mask_added )); then
		sudo systemctl unmask --runtime himax-touchd.service || return
		runtime_mask_added=0
	fi
}

restore() {
	sudo systemctl stop "$unit.service" 2>/dev/null || true
	sudo systemctl stop "$unit-watchdog.service" "$unit-unbind.service" 2>/dev/null || true
	sudo systemctl stop himax-touchd.service 2>/dev/null || true
	if (( module_unloaded )); then
		sudo modprobe -r himax_spi || return
	fi
	if (( persistent )); then
		# Keep failed source/cache for diagnosis, and restore the complete
		# prior DKMS registration rather than leaving a new cached .ko.
		sudo mv /usr/src/himax-spi-0.1 "$state_dir/failed-dkms-source" || return
		if [[ -d "$state_dir/dkms-source" ]]; then
			sudo cp -a "$state_dir/dkms-source" /usr/src/himax-spi-0.1 || return
		fi
		sudo mv "$dkms_tree" "$state_dir/failed-dkms-state" || return
		if [[ -d "$state_dir/dkms-state" ]]; then
			sudo cp -a "$state_dir/dkms-state" "$dkms_tree" || return
		fi
		local path index=0
		while IFS= read -r -d '' path; do
			if [[ ! -f "$state_dir/modules$path" ]]; then
				sudo mv -- "$path" "$state_dir/failed-module-$index" || return
				index=$((index + 1))
			fi
		done < <(module_paths)
		while IFS= read -r -d '' path; do
			sudo install -Dpm644 "$path" "${path#"$state_dir/modules"}" || return
		done < <(find "$state_dir/modules" -type f -print0)
		if (( ! old_service_enabled )); then
			sudo systemctl disable himax-touchd.service 2>/dev/null || true
		fi
		for path in "${installed_paths[@]}"; do
			if [[ -f "$state_dir/installed$path" ]]; then
				sudo cp -p "$state_dir/installed$path" "$path" || return
			else
				sudo rm -f -- "$path" || return
			fi
		done
		sudo udevadm control --reload-rules || return
		sudo systemctl daemon-reload || return
		sudo depmod -a "$kernel_version" || return
		if (( old_service_enabled )); then
			sudo systemctl enable himax-touchd.service || return
		fi
	fi
	if (( module_unloaded )); then
		load_dependencies "$state_dir/previous-himax-spi.ko" || return
		sudo insmod "$state_dir/previous-himax-spi.ko" || return
	fi
	[[ -L /sys/bus/spi/devices/spi0.0/driver ]] || return
	unmask_trial_service || return
	if (( old_service_active )); then
		sudo systemctl reset-failed himax-touchd.service || return
		sudo systemctl start himax-touchd.service || return
		sudo systemctl is-active --quiet himax-touchd.service || return
	fi
	echo "Restored previous touch module"
}
finish() {
	local status=$?
	trap - EXIT
	if (( changed )); then
		restore || { echo "ROLLBACK FAILED; backup=$state_dir" >&2; exit 1; }
	fi
	exit "$status"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cmake -S "$repo_dir/userspace" -B "$repo_dir/userspace/build" \
	-DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release
make -C "$repo_dir" userspace test
make -C "$repo_dir/touchscreen-hx83121a-dkms" -j4 HIMAX_V4L2=1 \
	HIMAX_AFE_DIAGNOSTICS="$afe_diagnostics"
if [[ -e /dev/uinput ]]; then
	:
elif ! modinfo uinput >/dev/null 2>&1; then
	bash "$repo_dir/scripts/build-uinput.sh"
	sudo insmod "$repo_dir/artifacts/uinput.ko"
else
	sudo modprobe uinput
fi
changed=1
# A newly registered video node must not let udev start the installed daemon
# against a trial module with a different ABI or steal the trial capture queue.
case "$(systemctl is-enabled himax-touchd.service 2>/dev/null || true)" in
	masked*) sudo systemctl stop himax-touchd.service 2>/dev/null || true ;;
	*)
		[[ ! -e /run/systemd/system/himax-touchd.service &&
		   ! -L /run/systemd/system/himax-touchd.service ]] || {
			echo "Refusing to overwrite an existing runtime service unit" >&2
			exit 1
		}
		runtime_mask_added=1
		sudo systemctl mask --runtime --now himax-touchd.service
		;;
esac
sudo modprobe -r himax_spi
module_unloaded=1
# rmmod may also remove unused dependencies of the old V4L2 driver. Load the
# exact new module's dependencies after unloading, on forward and rollback paths.
load_dependencies "$repo_dir/touchscreen-hx83121a-dkms/himax-spi.ko"
module_options=()
afe_since=$(date --iso-8601=seconds)
if [[ "$trial_afe" != none ]]; then
	module_options+=(afe_mailbox_debug=1)
fi
sudo insmod "$repo_dir/touchscreen-hx83121a-dkms/himax-spi.ko" "${module_options[@]}"
udevadm settle --timeout=10
[[ "$(basename "$(readlink -f /sys/bus/spi/devices/spi0.0/driver)")" == himax-spi ]]
echo "Capturing five seconds of HXST metadata"
result=0
sudo timeout --signal=TERM --kill-after=3 5 \
	"$repo_dir/userspace/build/himax-touchd" --observe \
	--record "$state_dir/smoke.hxst" || result=$?
[[ "$result" == 0 || "$result" == 124 ]]
[[ -s "$state_dir/smoke.hxst" ]]
"$repo_dir/userspace/build/himax-replay" --allow-startup-invalid --hxst "$state_dir/smoke.hxst" \
	> "$state_dir/smoke.csv"
if [[ "$trial_afe" != none ]]; then
	# Exercise the temporary module, not the installed .ko. The normal test
	# wrapper deliberately requires equality with the installed artifact.
	# Any failure below goes through restore(), including disabling logging
	# by unloading this diagnostic module and reinstating the previous one.
	afe_options=()
	[[ "$trial_afe" != basic ]] || afe_options+=(--basic)
	[[ "$trial_afe" != frequency ]] || afe_options+=(--frequency-only)
	afe_result=0
	sudo timeout --signal=TERM --kill-after=3 60 \
		"$repo_dir/userspace/build/himax-afe-hardware-test" \
		/dev/himax-raw "$state_dir/afe.hxst" "${afe_options[@]}" \
		> "$state_dir/afe-test.log" 2>&1 || afe_result=$?
	sudo journalctl -k --since "$afe_since" --no-pager > "$state_dir/afe-kernel.log"
	cat "$state_dir/afe-test.log"
	printf '%s\n' 0 | sudo tee /sys/module/himax_spi/parameters/afe_mailbox_debug >/dev/null
	if (( afe_result )); then
		echo "Selected AFE handshake test failed: $afe_result; restoring previous module" >&2
		exit "$afe_result"
	fi
	"$repo_dir/userspace/build/himax-replay" --allow-startup-invalid --hxst "$state_dir/afe.hxst" \
		> "$state_dir/afe.csv"
fi
sudo systemd-run --quiet --unit="$unit" --property=Type=notify \
	--property=WatchdogSec=3 --property=TimeoutStopSec=3 \
	"$repo_dir/userspace/build/himax-touchd"
for ((i=0; i<trial_seconds; i++)); do
	sleep 1
	sudo systemctl is-active --quiet "$unit.service"
	if (( i == 2 )); then
		sudo systemctl kill --kill-whom=main --signal=USR1 "$unit.service"
	fi
done
awk '/^N: Name="Himax Capacitive TouchScreen"$/{n++} END{exit n != 1}' /proc/bus/input/devices
sudo systemctl stop "$unit.service"
sudo journalctl -u "$unit.service" --no-pager > "$state_dir/solver.log"
if (( trial )); then
	echo "Checking hung solver watchdog"
	sudo systemd-run --quiet --unit="$unit-watchdog" --property=Type=notify \
		--property=WatchdogSec=3 --property=WatchdogSignal=SIGKILL \
		--property=TimeoutStopSec=3 "$repo_dir/userspace/build/himax-touchd"
	sudo systemctl kill --kill-whom=main --signal=STOP "$unit-watchdog.service"
	for ((i=0; i<10; i++)); do
		sleep 1
		systemctl is-active --quiet "$unit-watchdog.service" || break
	done
	[[ "$(systemctl show -p Result --value "$unit-watchdog.service")" == watchdog ]]
	awk '/^N: Name="Himax Capacitive TouchScreen"$/{n++} END{exit n != 0}' /proc/bus/input/devices
	echo "Checking unbind with an open stream and MMAP buffers"
	sudo systemd-run --quiet --unit="$unit-unbind" --property=Type=notify \
		--property=WatchdogSec=3 --property=WatchdogSignal=SIGKILL \
		--property=TimeoutStopSec=3 "$repo_dir/userspace/build/himax-touchd" --observe
	printf '%s' spi0.0 | sudo timeout 10 tee /sys/bus/spi/drivers/himax-spi/unbind >/dev/null
	for ((i=0; i<5; i++)); do
		sleep 1
		systemctl is-active --quiet "$unit-unbind.service" || break
	done
	[[ "$(systemctl show -p Result --value "$unit-unbind.service")" == exit-code ]]
	printf '%s' spi0.0 | sudo timeout 10 tee /sys/bus/spi/drivers/himax-spi/bind >/dev/null
	[[ -L /sys/bus/spi/devices/spi0.0/driver ]]
	sudo journalctl -u "$unit-watchdog.service" -u "$unit-unbind.service" \
		--no-pager > "$state_dir/lifecycle.log"
fi
if (( trial )); then
	echo "Trial passed; restoring previous module"
	exit 0
fi

# Persist only after live metadata, solver and uinput registration succeeded.
while IFS= read -r -d '' path; do
	sudo install -Dpm644 "$path" "$state_dir/modules$path"
done < <(module_paths)
if [[ -d /usr/src/himax-spi-0.1 ]]; then
	sudo cp -a /usr/src/himax-spi-0.1 "$state_dir/dkms-source"
fi
if [[ -d "$dkms_tree" ]]; then
	sudo cp -a "$dkms_tree" "$state_dir/dkms-state"
fi
for path in "${installed_paths[@]}"; do
	if [[ -f "$path" ]]; then
		sudo install -d "$(dirname "$state_dir/installed$path")"
		sudo cp -p "$path" "$state_dir/installed$path"
	fi
done
persistent=1
sudo install -d /usr/src/himax-spi-0.1
while IFS= read -r file; do
	sudo install -m644 "$repo_dir/touchscreen-hx83121a-dkms/$file" "/usr/src/himax-spi-0.1/$file"
done < <(make -s --no-print-directory -C "$repo_dir/touchscreen-hx83121a-dkms" print-dkms-sources)
# install --force alone can reuse the legacy .ko already in the DKMS cache.
sudo dkms build --force -m himax-spi -v 0.1 -k "$kernel_version"
sudo dkms install --force -m himax-spi -v 0.1 -k "$kernel_version"
# Keep the tested repository artifact, installed module and DKMS cache equal.
installed_module=$(modinfo -n himax_spi)
[[ "$installed_module" == "/lib/modules/$kernel_version/"*"/himax-spi.ko" ]]
sudo install -m644 "$repo_dir/touchscreen-hx83121a-dkms/himax-spi.ko" "$installed_module"
sudo install -m644 "$repo_dir/touchscreen-hx83121a-dkms/himax-spi.ko" "$dkms_module"
if [[ -f "$repo_dir/artifacts/uinput.ko" ]] && ! modinfo uinput >/dev/null 2>&1; then
	sudo install -Dm644 "$repo_dir/artifacts/uinput.ko" "/lib/modules/$kernel_version/extra/uinput.ko"
fi
sudo install -Dm644 "$repo_dir/touchscreen-hx83121a-dkms/himax-spi-dkms.conf" \
	/etc/modprobe.d/himax-spi-dkms.conf
sudo depmod -a "$kernel_version"
sudo cmake --install "$repo_dir/userspace/build"
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=video4linux
sudo udevadm settle --timeout=10
sudo systemctl daemon-reload
unmask_trial_service
sudo systemctl enable --now himax-touchd.service
sleep 2
sudo systemctl is-active --quiet himax-touchd.service
cmp "$installed_module" "$repo_dir/touchscreen-hx83121a-dkms/himax-spi.ko"
cmp "$dkms_module" "$installed_module"
installed_build_id=$(readelf -n "$installed_module" | awk '/Build ID:/{print $3; exit}')
running_build_id=$(od -An -tx1 -v /sys/module/himax_spi/notes/.note.gnu.build-id |
	tr -d ' \n' | sed 's/^040000001400000003000000474e5500//')
[[ -n "$installed_build_id" && "$running_build_id" == "$installed_build_id" ]]
[[ -L /dev/himax-raw ]]
awk '/^N: Name="Himax Capacitive TouchScreen"$/{n++} END{exit n != 1}' /proc/bus/input/devices
echo "installed_module=$installed_module build_id=$running_build_id"
changed=0
echo "Installed V4L2 transport and himax-touchd; rollback backup=$state_dir"
