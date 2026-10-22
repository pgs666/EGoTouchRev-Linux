#!/usr/bin/env bash
# Opt-in, hands-off firmware handshake test. Restores the original daemon state.
set -Eeuo pipefail
test_options=()
case "${1:-}" in
    --basic) test_options+=(--basic) ;;
    "") ;;
    *) echo "Usage: bash scripts/test-v4l2-afe.sh [--basic]" >&2; exit 2 ;;
esac
(( $# <= 1 )) || { echo "Too many arguments" >&2; exit 2; }
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$repo_dir/deploy-logs"
state_dir=$(mktemp -d "$repo_dir/deploy-logs/afe-XXXXXXXX")
was_active=0
systemctl is-active --quiet himax-touchd.service && was_active=1
exec 9>"$repo_dir/deploy-logs/.v4l2-deploy.lock"
flock -n 9 || { echo "Another V4L2 hardware operation is running" >&2; exit 1; }
exec > >(tee -a "$state_dir/test.log") 2>&1
echo "checkpoint=$state_dir"
since=$(date --iso-8601=seconds)
cmake -S "$repo_dir/userspace" -B "$repo_dir/userspace/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$repo_dir/userspace/build" --target himax-afe-hardware-test himax-replay -j4
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
echo "Keep hands off panel; testing firmware handshakes (options: ${test_options[*]:-full})."
sudo systemctl stop himax-touchd.service
sudo timeout --signal=TERM --kill-after=3 60 \
    "$repo_dir/userspace/build/himax-afe-hardware-test" /dev/himax-raw "$state_dir/firmware.hxst" "${test_options[@]}"
"$repo_dir/userspace/build/himax-replay" --allow-startup-invalid --hxst "$state_dir/firmware.hxst" \
    > "$state_dir/firmware.csv"
echo "Selected AFE handshake checks passed (not strict solver-session equivalence; --basic excludes frequency)"
