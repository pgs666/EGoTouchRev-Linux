#!/usr/bin/env bash
# Opt-in real-device command/epoch/reset test. Restores the original daemon state.
set -Eeuo pipefail
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$repo_dir/deploy-logs"
state_dir=$(mktemp -d "$repo_dir/deploy-logs/transport-XXXXXXXX")
was_active=0
systemctl is-active --quiet himax-touchd.service && was_active=1
exec 9>"$repo_dir/deploy-logs/.v4l2-deploy.lock"
flock -n 9 || { echo "Another V4L2 hardware operation is running" >&2; exit 1; }
exec > >(tee -a "$state_dir/test.log") 2>&1
echo "checkpoint=$state_dir"
since=$(date --iso-8601=seconds)
cmake -S "$repo_dir/userspace" -B "$repo_dir/userspace/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$repo_dir/userspace/build" --target himax-transport-test -j4
cmp "$repo_dir/touchscreen-hx83121a-dkms/himax-spi.ko" "$(modinfo -n himax_spi)"
finish() {
	local status=$?
	trap - EXIT
	if (( was_active )); then
		sudo systemctl start himax-touchd.service || status=1
		sudo systemctl is-active --quiet himax-touchd.service || status=1
	fi
	sudo journalctl -k --since "$since" --no-pager > "$state_dir/kernel.log" || true
	sudo journalctl -u himax-touchd.service --since "$since" --no-pager \
		> "$state_dir/service.log" || true
	exit "$status"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sudo systemctl stop himax-touchd.service
sudo timeout --signal=TERM --kill-after=3 15 \
	"$repo_dir/userspace/build/himax-transport-test" /dev/himax-raw
echo "Real-device transport test passed"
