#!/system/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: $0 read|write TAG" >&2
	exit 2
fi

case "$1" in
	read|write) bg_op="$1" ;;
	*) echo "invalid background op: $1" >&2; exit 2 ;;
esac

tag="$2"
bin=/data/local/tmp/smart_io_profile
data=/data/local/tmp/smart_io_profile.bin
bg_pid=

cleanup()
{
	if [ -n "$bg_pid" ]; then
		kill "$bg_pid" 2>/dev/null || true
		wait "$bg_pid" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

"$bin" probe --path "$data" \
	--csv "/data/local/tmp/${tag}-background-${bg_op}.csv" \
	--op "$bg_op" --samples 4000 --workers 4 --block-kb 4 --file-mb 64 \
	--ioprio idle --seed 5001 &
bg_pid=$!
sleep 0.05
"$bin" probe --path "$data" \
	--csv "/data/local/tmp/${tag}-foreground-read.csv" \
	--op read --samples 2000 --workers 1 --block-kb 4 --file-mb 64 \
	--ioprio rt --seed 6001
wait "$bg_pid"
bg_pid=
