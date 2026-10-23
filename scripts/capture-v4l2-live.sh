#!/usr/bin/env bash
# Request a window in an already-configured daemon. Never restart the service.
set -Eeuo pipefail
(( $# == 0 )) || { echo "Usage: bash scripts/capture-v4l2-live.sh" >&2; exit 2; }
service=himax-touchd.service
systemctl is-active --quiet "$service"
pid=$(systemctl show --value -p MainPID "$service")
[[ "$pid" =~ ^[1-9][0-9]*$ ]]
arguments=()
mapfile -d '' -t arguments < <(sudo -n cat "/proc/$pid/cmdline")
configured=0
for argument in "${arguments[@]}"; do
    [[ "$argument" != --live-capture-dir ]] || configured=1
done
(( configured )) || {
    echo "Running daemon is not configured for live capture; no signal sent." >&2
    echo "Install the new daemon and optional live-capture drop-in first; see userspace/README.md." >&2
    exit 1
}
[[ $(systemctl show --value -p MainPID "$service") == "$pid" ]] || {
    echo "Service changed during inspection; no signal sent." >&2
    exit 1
}
sudo -n systemctl kill --kill-whom=main --signal=USR2 "$service"
echo "Capture toggle requested without restart: first request starts, second ends early."
echo "This does not confirm a saved file; check journalctl -u himax-touchd.service -b."
