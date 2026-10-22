#!/usr/bin/env bash
# Record from a fresh solver/AFE session, then restore the prior service.
# This does not attach to or preserve an already-faulty running baseline.
set -Eeuo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
seconds=${1:-10}
[[ "$seconds" =~ ^[0-9]+$ ]] && (( seconds >= 5 && seconds <= 120 )) || {
	echo "Usage: bash scripts/capture-v4l2-session.sh [5..120 seconds]" >&2
	exit 2
}
(( $# <= 1 )) || { echo "Expected only capture duration" >&2; exit 2; }
daemon="$repo_dir/userspace/build/himax-touchd"
replay="$repo_dir/userspace/build/himax-replay"
[[ -x "$daemon" && -x "$replay" ]] || {
	echo "Run make userspace first" >&2
	exit 1
}
mkdir -p "$repo_dir/deploy-logs"
state_dir=$(mktemp -d "$repo_dir/deploy-logs/session-XXXXXXXX")
echo "capture=$state_dir"
echo "This starts a fresh AFE/solver session; recording keeps touch input active."
was_active=0
systemctl is-active --quiet himax-touchd.service && was_active=1
restored=0
capture_pid=
restore_service() {
	if (( was_active && ! restored )); then
		sudo systemctl start himax-touchd.service || return
		systemctl is-active --quiet himax-touchd.service || return
	fi
	restored=1
}
finish() {
	local status=$?
	trap - EXIT
	if [[ -n "$capture_pid" ]]; then
		# Terminate only the exact sudo/timeout child started below; sudo
		# forwards the signal and timeout supplies the three-second kill bound.
		sudo kill -TERM "$capture_pid" 2>/dev/null || true
		wait "$capture_pid" 2>/dev/null || true
	fi
	if ! restore_service; then
		echo "FAILED to restore himax-touchd.service; capture=$state_dir" >&2
		exit 1
	fi
	exit "$status"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
git -C "$repo_dir" rev-parse HEAD > "$state_dir/source-commit.txt"
git -C "$repo_dir" status --short > "$state_dir/source-status.txt"
sha256sum "$daemon" "$replay" > "$state_dir/binaries.sha256"
sudo systemctl stop himax-touchd.service
set +e
sudo timeout --signal=TERM --kill-after=3 "$seconds" "$daemon" \
	--record "$state_dir/capture.hxst" > "$state_dir/daemon.log" 2>&1 &
capture_pid=$!
wait "$capture_pid"
result=$?
capture_pid=
set -e
restore_service
if (( result != 0 && result != 124 )); then
	tail -25 "$state_dir/daemon.log"
	echo "Capture process failed (status=$result); evidence kept in $state_dir" >&2
	exit 1
fi
sudo "$replay" --session "$state_dir/capture.hxst" \
	> "$state_dir/contacts.csv" 2> "$state_dir/replay.log" || {
	tail -25 "$state_dir/daemon.log"
	cat "$state_dir/replay.log"
	echo "Capture incomplete or replay mismatch; evidence kept in $state_dir" >&2
	exit 1
}
# Return root-created evidence to the invoking developer.
sudo chown "$(id -u):$(id -g)" \
	"$state_dir/capture.hxst" "$state_dir/capture.hxst.events"
tail -14 "$state_dir/daemon.log"
cat "$state_dir/replay.log"
echo "Complete session: $state_dir"
