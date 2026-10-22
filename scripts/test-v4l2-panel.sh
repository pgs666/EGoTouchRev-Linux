#!/usr/bin/env bash
# Opt-in KDE/Wayland hardware test. Briefly blanks the sole DSI panel,
# reloads the installed transport and starts touchd before waking the panel.
set -Eeuo pipefail
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
program=/usr/local/bin/himax-touchd
stamp=$(date +%Y%m%d-%H%M%S)
state_dir="$repo_dir/deploy-logs/panel-$stamp"
guard="himax-panel-restore-$stamp"
[[ -x "$program" && -n "${WAYLAND_DISPLAY:-}" && -n "${XDG_RUNTIME_DIR:-}" ]]
command -v kscreen-doctor >/dev/null
[[ "$(kscreen-doctor --dpms show)" == "dpms mode for screen DSI-1: on" ]] || {
	echo "Requires the sole DSI-1 display to be on in a KDE Wayland session" >&2
	exit 2
}
systemctl is-active --quiet himax-touchd.service
cmp "$repo_dir/touchscreen-hx83121a-dkms/himax-spi.ko" "$(modinfo -n himax_spi)"
initial_info=$(sudo "$program" --info)
expected_project=$(sed -n '/^project_id=W/p' <<< "$initial_info")
[[ -n "$expected_project" ]]
mkdir -p "$state_dir"
exec 9>"$repo_dir/deploy-logs/.v4l2-deploy.lock"
flock -n 9 || { echo "Another V4L2 hardware operation is running" >&2; exit 1; }
exec > >(tee -a "$state_dir/test.log") 2>&1
since=$(date --iso-8601=seconds)
echo "checkpoint=$state_dir"
printf '%s\n' "$initial_info"

# Independent of this shell: even a failed test or lost agent connection
# must not leave the panel deliberately off.
systemd-run --user --quiet --collect --unit="$guard" --on-active=20 \
	--setenv="WAYLAND_DISPLAY=$WAYLAND_DISPLAY" \
	--setenv="XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR" --setenv=QT_QPA_PLATFORM=wayland \
	/usr/bin/kscreen-doctor --dpms on
finish() {
	local status=$?
	trap - EXIT
	if kscreen-doctor --dpms on; then
		systemctl --user stop "$guard.timer" 2>/dev/null || true
	else
		status=1
	fi
	sudo modprobe himax_spi || status=1
	sudo systemctl start himax-touchd.service || status=1
	sudo systemctl is-active --quiet himax-touchd.service || status=1
	sudo journalctl -u himax-touchd.service --since "$since" --no-pager \
		> "$state_dir/service.log" || true
	sudo journalctl -k --since "$since" --no-pager > "$state_dir/kernel.log" || true
	if (( status == 0 )); then echo "Panel cold-start test passed; display and service are on"
	else echo "Panel cold-start test failed; attempted display/service restoration" >&2
	fi
	exit "$status"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

sudo systemctl stop himax-touchd.service
kscreen-doctor --dpms off
sleep 1
[[ "$(kscreen-doctor --dpms show)" == "dpms mode for screen DSI-1: off" ]]
sudo modprobe -r himax_spi
sudo modprobe himax_spi
sudo udevadm settle --timeout=5
sudo systemctl start himax-touchd.service
solver_pid=$(systemctl show --value -p MainPID himax-touchd.service)
restarts=$(systemctl show --value -p NRestarts himax-touchd.service)
[[ "$solver_pid" != 0 ]]
off_info=$(sudo "$program" --info)
printf '%s\n' "$off_info" | tee "$state_dir/off-info.txt"
rg -q '^project_id=$' <<< "$off_info"
sleep 2
sudo systemctl is-active --quiet himax-touchd.service
off_journal=$(sudo journalctl "_PID=$solver_pid" --since "$since" -o cat --no-pager)
printf '%s\n' "$off_journal" > "$state_dir/off-service.log"
rg -q 'project=pending-OTP' <<< "$off_journal"
if rg -q 'AFE epoch=' <<< "$off_journal"; then
	echo "AFE unexpectedly started while the panel was off" >&2
	exit 1
fi

kscreen-doctor --dpms on
ready=0
for ((attempt=0; attempt<10; attempt++)); do
	sleep 0.5
	if [[ "$(sudo "$program" --info | sed -n '/^project_id=/p')" == "$expected_project" ]]; then
		ready=1
		break
	fi
done
(( ready ))
sleep 1
[[ "$(kscreen-doctor --dpms show)" == "dpms mode for screen DSI-1: on" ]]
[[ "$(systemctl show --value -p MainPID himax-touchd.service)" == "$solver_pid" ]]
[[ "$(systemctl show --value -p NRestarts himax-touchd.service)" == "$restarts" ]]
sudo systemctl kill --kill-whom=main --signal=USR1 himax-touchd.service
sudo "$program" --info | tee "$state_dir/on-info.txt"
stats_ready=0
for ((attempt=0; attempt<10; attempt++)); do
	on_journal=$(sudo journalctl "_PID=$solver_pid" --since "$since" -o cat --no-pager)
	if rg -q 'solver_us mean=' <<< "$on_journal"; then
		stats_ready=1
		break
	fi
	sleep 0.1
done
(( stats_ready ))
rg -q 'AFE epoch=.*project=W' <<< "$on_journal"
awk '/^N: Name="Himax Capacitive TouchScreen"$/{n++} END{exit n != 1}' /proc/bus/input/devices
