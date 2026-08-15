#!/system/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 SCHEDULE_CSV OUTPUT_DIR" >&2
	exit 2
fi

schedule=$1
output_dir=$2
bin=/data/local/tmp/smart_io_guard_test
read_file=/data/local/tmp/smart_io_guard_read.bin
write_file=/data/local/tmp/smart_io_guard_write.bin
proc_root=/proc/smart_io_sched
battery_temp=/sys/class/power_supply/battery/temp
last_block=0
last_idle_stats=

record_thermal()
{
	thermal_prefix=$1
	{
		date '+timestamp=%Y-%m-%dT%H:%M:%S%z'
		if [ -r "$battery_temp" ]; then
			echo "battery_temp_tenths_c=$(cat "$battery_temp")"
		fi
		for zone in /sys/class/thermal/thermal_zone*; do
			[ -r "$zone/type" ] || continue
			[ -r "$zone/temp" ] || continue
			zone_type=$(cat "$zone/type" 2>/dev/null) || continue
			zone_temp=$(cat "$zone/temp" 2>/dev/null) || continue
			echo "$(basename "$zone") type=$zone_type temp=$zone_temp"
		done
	} > "$thermal_prefix-thermal.txt"
}

wait_for_thermal_gate()
{
	tries=0
	while [ -r "$battery_temp" ]; do
		temp=$(cat "$battery_temp")
		case "$temp" in
			*[!0-9]*) echo "invalid battery temperature: $temp" >&2; exit 1 ;;
		esac
		if [ "$temp" -ge 450 ]; then
			echo "hard thermal stop: battery temp is $temp tenths C" >&2
			exit 1
		fi
		[ "$temp" -lt 420 ] && return
		tries=$((tries + 1))
		if [ "$tries" -gt 12 ]; then
			echo "thermal gate did not recover below 42.0 C" >&2
			exit 1
		fi
		sleep 10
	done
}

require_value()
{
	path=$1
	expected=$2
	actual=$(cat "$path")
	if [ "$actual" != "$expected" ]; then
		echo "configuration mismatch: $path expected='$expected' actual='$actual'" >&2
		exit 1
	fi
}

wait_for_module_idle()
{
	snapshot_path=${1:-}
	tries=0
	while [ "$tries" -lt 5 ]; do
		stats=$(cat "$proc_root/stats")
		case "$stats" in
			*'current_depth:0 reserved_depth:0 issued_depth:0 '*'inuse=0'*)
				last_idle_stats=$stats
				if [ -n "$snapshot_path" ]; then
					printf '%s\n' "$stats" > "$snapshot_path"
				fi
				return
				;;
		esac
		tries=$((tries + 1))
		sleep 1
	done
	echo "module did not become idle within 5 seconds" >&2
	echo "$stats" >&2
	exit 1
}

counter_value()
{
	counter_name=$1
	counter_text=$2
	echo "$counter_text" |
		sed -n "s/.* $counter_name:\([0-9][0-9]*\).*/\1/p"
}

require_counters_stable()
{
	counter_now=$(cat "$proc_root/stats")
	counter_now_inference=$(counter_value inference "$counter_now")
	counter_now_timeout=$(counter_value timeout "$counter_now")
	counter_now_invalid=$(counter_value invalid "$counter_now")
	counter_now_anomalies=$(counter_value depth_anomalies "$counter_now")
	counter_now_allocations=$(counter_value allocation_failures "$counter_now")
	if [ "$counter_now_inference" != "$counter_base_inference" ] ||
	   [ "$counter_now_timeout" != "$counter_base_timeout" ] ||
	   [ "$counter_now_invalid" != "$counter_base_invalid" ] ||
	   [ "$counter_now_anomalies" != "$counter_base_anomalies" ] ||
	   [ "$counter_now_allocations" != "$counter_base_allocations" ]; then
		echo "module counter changed during conditional experiment" >&2
		echo "baseline inference=$counter_base_inference timeout=$counter_base_timeout invalid=$counter_base_invalid anomalies=$counter_base_anomalies allocations=$counter_base_allocations" >&2
		echo "current inference=$counter_now_inference timeout=$counter_now_timeout invalid=$counter_now_invalid anomalies=$counter_now_anomalies allocations=$counter_now_allocations" >&2
		exit 1
	fi
}

[ -x "$bin" ] || { echo "missing executable: $bin" >&2; exit 1; }
[ -r "$schedule" ] || { echo "missing schedule: $schedule" >&2; exit 1; }
[ -r "$read_file" ] || { echo "missing read file: $read_file" >&2; exit 1; }
[ -r "$write_file" ] || { echo "missing write file: $write_file" >&2; exit 1; }
[ ! -e "$output_dir" ] || { echo "output exists: $output_dir" >&2; exit 1; }
mkdir -p "$output_dir"

scheduler=$(cat /sys/block/sdc/queue/scheduler)
case "$scheduler" in
	*'[smart-deadline]'*) ;;
	*) echo "smart-deadline is not selected: $scheduler" >&2; exit 1 ;;
esac
require_value "$proc_root/throttle_enable" "1"
require_value "$proc_root/action_source" "fixed"
require_value "$proc_root/fixed_action" "NO BASELINE"
require_value "$proc_root/dev_lat" "10000000"
require_value "$proc_root/background_deadline_ms" "2000"
foreground_uid=$(cat "$proc_root/fg_uid")
case "$foreground_uid" in
	''|*[!0-9]*) echo "invalid framework fg_uid: $foreground_uid" >&2; exit 1 ;;
esac
[ "$foreground_uid" -gt 0 ] || {
	echo "framework fg_uid must be positive: $foreground_uid" >&2
	exit 1
}
wait_for_module_idle
counter_base_stats=$(cat "$proc_root/stats")
counter_base_inference=$(counter_value inference "$counter_base_stats")
counter_base_timeout=$(counter_value timeout "$counter_base_stats")
counter_base_invalid=$(counter_value invalid "$counter_base_stats")
counter_base_anomalies=$(counter_value depth_anomalies "$counter_base_stats")
counter_base_allocations=$(counter_value allocation_failures "$counter_base_stats")
for counter_initial in "$counter_base_inference" "$counter_base_timeout" \
	"$counter_base_invalid" "$counter_base_anomalies" \
	"$counter_base_allocations"; do
	[ -n "$counter_initial" ] || {
		echo "failed to parse module counter baseline" >&2
		exit 1
	}
done

{
	echo "scheduler=$scheduler"
	echo "throttle_enable=$(cat "$proc_root/throttle_enable")"
	echo "action_source=$(cat "$proc_root/action_source")"
	echo "fixed_action=$(cat "$proc_root/fixed_action")"
	echo "throttle_ratios=$(cat "$proc_root/throttle_ratios")"
	echo "dev_lat=$(cat "$proc_root/dev_lat")"
	echo "background_deadline_ms=$(cat "$proc_root/background_deadline_ms")"
	echo "fg_uid=$foreground_uid"
	echo "warmup_ms=500"
	echo "pre_ms=1000"
	echo "settle_ms=200"
	echo "treatment_ms=2000"
	echo "schedule_sha256=$(sha256sum "$schedule" | awk '{print $1}')"
	echo "binary_sha256=$(sha256sum "$bin" | awk '{print $1}')"
	echo "runner_sha256=$(sha256sum "$0" | awk '{print $1}')"
	echo "counter_baseline_inference=$counter_base_inference"
	echo "counter_baseline_timeout=$counter_base_timeout"
	echo "counter_baseline_invalid=$counter_base_invalid"
	echo "counter_baseline_depth_anomalies=$counter_base_anomalies"
	echo "counter_baseline_allocation_failures=$counter_base_allocations"
	printf '%s\n' "$last_idle_stats"
} > "$output_dir/config-before.txt"
record_thermal "$output_dir/before"

tail -n +2 "$schedule" |
while IFS=, read -r block position profile read_guard write_guard seed; do
	[ -n "$block" ] || continue
	case "$profile" in
		BASELINE|READ_GUARD|WRITE_GUARD|ALL_GUARD) ;;
		*) echo "invalid profile in schedule: $profile" >&2; exit 1 ;;
	esac
	case "$profile:$read_guard:$write_guard" in
		BASELINE:0:0|READ_GUARD:1:0|WRITE_GUARD:0:1|ALL_GUARD:1:1) ;;
		*) echo "guard flags do not match profile: $profile $read_guard $write_guard" >&2; exit 1 ;;
	esac
	if [ "$block" != "$last_block" ] && [ "$last_block" -ne 0 ]; then
		sleep 5
	fi
	last_block=$block
	wait_for_thermal_gate
	scheduler=$(cat /sys/block/sdc/queue/scheduler)
	case "$scheduler" in
		*'[smart-deadline]'*) ;;
		*) echo "smart-deadline is no longer selected: $scheduler" >&2; exit 1 ;;
	esac
	require_value "$proc_root/throttle_enable" "1"
	require_value "$proc_root/action_source" "fixed"
	require_value "$proc_root/fixed_action" "NO BASELINE"
	require_value "$proc_root/dev_lat" "10000000"
	require_value "$proc_root/background_deadline_ms" "2000"
	require_value "$proc_root/fg_uid" "$foreground_uid"
	wait_for_module_idle
	require_counters_stable

	trial=$(printf 'b%02d-p%d-%s' "$block" "$position" "$profile")
	prefix="$output_dir/$trial"
	[ ! -e "$prefix.csv" ] || { echo "trial already exists: $trial" >&2; exit 1; }
	record_thermal "$prefix-before"
	require_counters_stable
	wait_for_module_idle "$prefix-stats-before.txt"
	require_counters_stable
	date '+start=%Y-%m-%dT%H:%M:%S%z' > "$prefix-meta.txt"
	echo "block=$block position=$position profile=$profile read_guard=$read_guard write_guard=$write_guard seed=$seed" >> "$prefix-meta.txt"

	set +e
	timeout 20 "$bin" run-state --profile "$profile" \
		--read-path "$read_file" --write-path "$write_file" \
		--csv "$prefix.csv" --file-mb 128 --warmup-ms 500 \
		--pre-ms 1000 --settle-ms 200 --measure-ms 2000 \
		--foreground-uid "$foreground_uid" --seed "$seed" \
		> "$prefix-stdout.txt" 2> "$prefix-stderr.txt"
	trial_rc=$?
	set -e
	echo "exit_code=$trial_rc" >> "$prefix-meta.txt"
	date '+end=%Y-%m-%dT%H:%M:%S%z' >> "$prefix-meta.txt"
	[ "$trial_rc" -eq 0 ] || { echo "trial failed: $trial rc=$trial_rc" >&2; exit 1; }
	[ -s "$prefix.csv" ] || { echo "empty trial CSV: $trial" >&2; exit 1; }
	grep -q 'state_mode=1 .* ret=0$' "$prefix-stdout.txt" || {
		echo "trial did not report state_mode=1 ret=0: $trial" >&2
		exit 1
	}
	wait_for_module_idle "$prefix-stats-after.txt"
	require_counters_stable
	record_thermal "$prefix-after"
	echo "completed $trial"
	sleep 2
done

wait_for_module_idle "$output_dir/stats-after.txt"
require_counters_stable
record_thermal "$output_dir/after"
echo "complete" > "$output_dir/status.txt"
