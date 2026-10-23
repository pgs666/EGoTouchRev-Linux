#!/usr/bin/env bash
# Source-only helpers. Health checks never load/unload modules or write sysfs.
# Use a journal cursor rather than wall time (this machine changes RTC epoch
# during boot), and retain all queried records in the caller's checkpoint.

himax_kernel_boot_id() { cat /proc/sys/kernel/random/boot_id; }
himax_kernel_taint() { cat /proc/sys/kernel/tainted; }
himax_kernel_journal_start() {
	sudo -n journalctl -k -n 1 --show-cursor --no-pager -o cat
}
himax_kernel_journal_after() {
	sudo -n journalctl --after-cursor="$1" _TRANSPORT=kernel --no-pager -o short-monotonic
}

himax_kernel_health_start() {
	local directory=$1 taint cursor

	himax_kernel_boot_id > "$directory/kernel-boot-id" || return 1
	taint=$(himax_kernel_taint) || return 1
	[[ "$taint" =~ ^[0-9]+$ ]] || return 1
	printf '%s\n' "$taint" > "$directory/kernel-taint-before" || return 1
	# TAINT_BAD_PAGE=5, TAINT_DIE=7: a loaded OOT module alone is allowed.
	if (( taint & ((1 << 5) | (1 << 7)) )); then
		echo "Kernel already has bad-page/Oops taint; refusing hardware changes" >&2
		return 1
	fi
	himax_kernel_journal_start > "$directory/kernel-start.log" \
		2> "$directory/kernel-start.err" || return 1
	cursor=$(sed -n 's/^-- cursor: //p' "$directory/kernel-start.log")
	[[ -n "$cursor" && "$cursor" != *$'\n'* ]] || {
		echo "Cannot establish kernel journal cursor; refusing hardware changes" >&2
		return 1
	}
	printf '%s\n' "$cursor" > "$directory/kernel-cursor" || return 1
}

himax_kernel_health_check() {
	local directory=$1 phase=$2 before after boot cursor scan_status

	[[ "$phase" =~ ^[a-z0-9-]+$ ]] || return 1
	boot=$(himax_kernel_boot_id) || return 1
	[[ -r "$directory/kernel-boot-id" &&
	   "$boot" == "$(<"$directory/kernel-boot-id")" ]] || {
		echo "Boot changed during deployment; health evidence is invalid" >&2
		return 1
	}
	before=$(<"$directory/kernel-taint-before") || return 1
	after=$(himax_kernel_taint) || return 1
	[[ "$before" =~ ^[0-9]+$ && "$after" =~ ^[0-9]+$ ]] || return 1
	printf '%s\n' "$after" > "$directory/kernel-taint-$phase" || return 1
	cursor=$(<"$directory/kernel-cursor") || return 1
	[[ -n "$cursor" ]] || return 1
	himax_kernel_journal_after "$cursor" > "$directory/kernel-$phase.log" \
		2> "$directory/kernel-$phase.err" || {
		echo "Cannot read kernel evidence at $phase; refusing to claim success" >&2
		return 1
	}
	# New WARN taint also fails even if a rate-limited printk was lost.
	if (( (after & ~before) & ((1 << 5) | (1 << 7) | (1 << 9)) )); then
		echo "New kernel bad-page/Oops/WARN taint at $phase; checkpoint=$directory" >&2
		return 1
	fi
	scan_status=0
	LC_ALL=C grep -Ein \
		'BUG:|WARNING:|Unable to handle kernel|Internal error:|Kernel panic|Oops:|KASAN:|KFENCE:|UBSAN:|Bad page state|slab.*corrupt|soft lockup|hard LOCKUP|rcu:.*stall|SError Interrupt' \
		"$directory/kernel-$phase.log" > "$directory/kernel-$phase.faults" || scan_status=$?
	if (( scan_status != 1 )); then
		echo "Kernel fault/evidence scan error at $phase; checkpoint=$directory" >&2
		return 1
	fi
}
