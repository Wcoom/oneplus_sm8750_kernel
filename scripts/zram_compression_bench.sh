#!/usr/bin/env bash
# Compare zram compression algorithms through adb.

set -Eeuo pipefail

SCRIPT_NAME=${0##*/}
SERIAL=${ADB_SERIAL:-}
ZRAM_DEV=${ZRAM_DEV:-zram0}
TEST_MB=${TEST_MB:-256}
ROUNDS=${ROUNDS:-1}
OUTPUT_DIR=${OUTPUT_DIR:-zram-bench-$(date +%Y%m%d-%H%M%S)}
INPUT_FILE=${INPUT_FILE:-}
REMOTE_INPUT=${REMOTE_INPUT:-/data/local/tmp/zram-compression-bench.bin}
FORCE=0
RESTORE=1
USE_SU=0
ALGORITHMS=(lz4 zstd lz4kd lzo-rle)

usage() {
	cat <<EOF
Usage: $SCRIPT_NAME [options]

This benchmark resets the selected zram device once per algorithm.
The same input payload is reused for every algorithm and round.
Original zram contents are not recoverable; only its configuration and active
swap state are restored after the benchmark.

  --serial SERIAL       adb device serial (default: ADB_SERIAL)
  --zram NAME           zram device name (default: zram0)
  --size-mb N           generated input size, multiple of 4 (default: 256)
  --input FILE          use an existing input file instead of generating one
  --rounds N            repetitions per algorithm (default: 1)
  --output DIR          result directory
  --force               allow the destructive swapoff/reset benchmark
  --su                  run privileged device commands through su -c
  --no-restore          do not restore the original config/swap state
  --help                show this help

Examples:
  $SCRIPT_NAME --force --size-mb 512
  $SCRIPT_NAME --force --input ./workload.bin --rounds 3 --output results
EOF
}

die() { echo "$SCRIPT_NAME: $*" >&2; exit 1; }
log() { echo "[$SCRIPT_NAME] $*"; }

while (($#)); do
	case "$1" in
	--serial) SERIAL=${2:?missing value for --serial}; shift 2 ;;
	--zram) ZRAM_DEV=${2:?missing value for --zram}; shift 2 ;;
	--size-mb) TEST_MB=${2:?missing value for --size-mb}; shift 2 ;;
	--input) INPUT_FILE=${2:?missing value for --input}; shift 2 ;;
	--rounds) ROUNDS=${2:?missing value for --rounds}; shift 2 ;;
	--output) OUTPUT_DIR=${2:?missing value for --output}; shift 2 ;;
	--force) FORCE=1; shift ;;
	--su) USE_SU=1; shift ;;
	--no-restore) RESTORE=0; shift ;;
	--help|-h) usage; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
done

[[ $ZRAM_DEV =~ ^[a-zA-Z0-9._-]+$ ]] || die "invalid zram device name"
[[ $REMOTE_INPUT =~ ^/[a-zA-Z0-9._/-]+$ ]] || die "invalid REMOTE_INPUT path"
[[ $TEST_MB =~ ^[0-9]+$ ]] || die "--size-mb must be an integer"
[[ $ROUNDS =~ ^[1-9][0-9]*$ ]] || die "--rounds must be positive"
(( TEST_MB >= 4 && TEST_MB % 4 == 0 )) ||
	die "--size-mb must be at least 4 and divisible by 4"
(( FORCE )) || die "benchmark resets zram and destroys its contents; pass --force"

ADB=(adb)
[[ -z $SERIAL ]] || ADB+=(-s "$SERIAL")
adb_shell() { "${ADB[@]}" shell "$@"; }
adb_shell_clean() { adb_shell "$@" 2>/dev/null | tr -d '\r'; }
device_exec() {
	if (( USE_SU )); then
		local command=$*
		adb_shell "su -c '$command'"
	else
		adb_shell "$@"
	fi
}
device_write() {
	device_exec "echo $2 > $1"
}

SYS=/sys/block/$ZRAM_DEV
DISKSIZE=$SYS/disksize
COMP_ALGO=$SYS/comp_algorithm
MM_STAT=$SYS/mm_stat
SDDC_STAT=$SYS/sddc_stat
ZMS_STAT=$SYS/zms_stat

"${ADB[@]}" get-state >/dev/null 2>&1 || die "adb device is not available"
adb_shell test -e "$DISKSIZE" || die "$SYS does not exist"
ORIGINAL_ALGO=$(adb_shell_clean cat "$COMP_ALGO" |
	sed -n 's/.*\[\([^]]*\)\].*/\1/p')
[[ -n $ORIGINAL_ALGO ]] ||
	ORIGINAL_ALGO=$(adb_shell_clean cat "$COMP_ALGO" | awk '{print $1}')
ORIGINAL_SIZE=$(adb_shell_clean cat "$DISKSIZE")
[[ $ORIGINAL_SIZE =~ ^[0-9]+$ ]] || die "could not read disksize"
SUPPORTED=$(adb_shell_clean cat "$COMP_ALGO")
for algorithm in "${ALGORITHMS[@]}"; do
	grep -Eq "(^|[[:space:]])\[?$algorithm\]?([[:space:]]|$)" <<< "$SUPPORTED" ||
		die "$algorithm is not listed in comp_algorithm: $SUPPORTED"
done

ACTIVE_SWAP=$(adb_shell_clean cat /proc/swaps |
	awk -v name="$ZRAM_DEV" '$1 ~ ("/" name "$|" name "$") {print $1; exit}')
ORIGINAL_SWAP_PRIORITY=$(adb_shell_clean cat /proc/swaps |
	awk -v name="$ZRAM_DEV" '$1 ~ ("/" name "$|" name "$") {print $5; exit}')
if [[ -n $ACTIVE_SWAP ]]; then
	BLOCK_DEV=$ACTIVE_SWAP
elif adb_shell test -b "/dev/block/$ZRAM_DEV"; then
	BLOCK_DEV=/dev/block/$ZRAM_DEV
elif adb_shell test -b "/dev/$ZRAM_DEV"; then
	BLOCK_DEV=/dev/$ZRAM_DEV
else
	die "could not find the block device for $ZRAM_DEV"
fi

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/zram-bench.XXXXXX")
GENERATED=0
DEVICE_MUTATED=0
RESTORED=0
cleanup() {
	local exit_status=$?
	trap - EXIT
	set +e
	if (( DEVICE_MUTATED && RESTORE && !RESTORED )); then
		restore_device
		(( $? == 0 || exit_status != 0 )) || exit_status=1
	fi
	adb_shell "rm -f '$REMOTE_INPUT'" >/dev/null 2>&1 || true
	if (( GENERATED )); then
		rm -f "$INPUT_FILE"
	fi
	rm -rf "$TMP_DIR"
	exit "$exit_status"
}
trap cleanup EXIT

if [[ -z $INPUT_FILE ]]; then
	INPUT_FILE=$TMP_DIR/input.bin
	GENERATED=1
	QUARTER=$((TEST_MB / 4))
	PATTERN=$TMP_DIR/pattern.bin
	log "generating ${TEST_MB} MiB mixed input"
	dd if=/dev/zero of="$INPUT_FILE" bs=1M count="$QUARTER" status=none
	set +o pipefail
	yes 'Crystal-HybridSWAP-LZ4KD-SDDC-0123456789' |
		tr -d '\n' | head -c "$((QUARTER * 1024 * 1024))" > "$PATTERN"
	set -o pipefail
	dd if="$PATTERN" of="$INPUT_FILE" bs=1M seek="$QUARTER" conv=notrunc status=none
	dd if=/dev/urandom of="$INPUT_FILE" bs=1M seek="$((QUARTER * 2))" \
		count="$QUARTER" conv=notrunc status=none
	dd if="$PATTERN" of="$INPUT_FILE" bs=1M seek="$((QUARTER * 3))" conv=notrunc status=none
fi

[[ -f $INPUT_FILE ]] || die "input file does not exist: $INPUT_FILE"
INPUT_BYTES=$(wc -c < "$INPUT_FILE")
[[ $INPUT_BYTES =~ ^[0-9]+$ ]] || die "could not determine input size"
(( INPUT_BYTES >= 4096 && INPUT_BYTES % 4096 == 0 )) ||
	die "input size must be a multiple of 4096 bytes"
INPUT_PAGES=$((INPUT_BYTES / 4096))
INPUT_MIB=$(awk -v bytes="$INPUT_BYTES" 'BEGIN {printf "%.2f", bytes / 1048576}')
TEST_SIZE=$ORIGINAL_SIZE
(( TEST_SIZE >= INPUT_BYTES )) || TEST_SIZE=$((INPUT_BYTES * 2))

mkdir -p "$OUTPUT_DIR"
SUMMARY_CSV=$OUTPUT_DIR/summary.csv
SUMMARY_TXT=$OUTPUT_DIR/summary.txt
AVERAGES_CSV=$OUTPUT_DIR/averages.csv
AVERAGES_TXT=$OUTPUT_DIR/averages.txt
printf 'algorithm,round,write_ms,write_mib_s,read_ms,read_mib_s,orig_data_size,compr_data_size,mem_used_total,payload_ratio,effective_ratio\n' > "$SUMMARY_CSV"
printf 'algorithm round write_ms write_MiB_s read_ms read_MiB_s orig_bytes compr_bytes mem_used_total payload_ratio effective_ratio\n' > "$SUMMARY_TXT"
log "pushing $(basename "$INPUT_FILE") (${INPUT_MIB} MiB)"
"${ADB[@]}" push "$INPUT_FILE" "$REMOTE_INPUT" >/dev/null

restore_device() {
	local restore_status=0
	(( RESTORE && !RESTORED )) || return 0
	RESTORED=1
	log "restoring empty zram: algorithm=$ORIGINAL_ALGO disksize=$ORIGINAL_SIZE"
	device_write "$SYS/reset" 1 || restore_status=1
	device_write "$COMP_ALGO" "$ORIGINAL_ALGO" || restore_status=1
	if (( ORIGINAL_SIZE > 0 )); then
		device_write "$DISKSIZE" "$ORIGINAL_SIZE" || restore_status=1
	fi
	if [[ -n $ACTIVE_SWAP ]]; then
		device_exec "swapon -p $ORIGINAL_SWAP_PRIORITY $ACTIVE_SWAP" || restore_status=1
	fi
	return "$restore_status"
}
reset_device() {
	local algorithm=$1
	log "resetting $ZRAM_DEV and selecting $algorithm"
	DEVICE_MUTATED=1
	device_write "$SYS/reset" 1
	device_write "$COMP_ALGO" "$algorithm"
	device_write "$DISKSIZE" "$TEST_SIZE"
}
wait_for_observation() {
	local i pending
	adb_shell test -e "$SDDC_STAT" || return 0
	for ((i = 0; i < 60; i++)); do
		pending=$(adb_shell_clean cat "$SDDC_STAT" |
			awk -F: '$1 == "pending" {gsub(/[[:space:]]/, "", $2); print $2}')
		[[ ${pending:-0} == 0 ]] && return 0
		sleep 1
	done
	log "observation did not drain within 60 seconds"
}
timed_dd() {
	local command=$1 elapsed
	elapsed=$(device_exec "start=\$(date +%s%N); $command; rc=\$?; end=\$(date +%s%N); echo \$(((end-start)/1000000)); exit \$rc")
	tr -d '\r' <<< "$elapsed" | tail -n 1
}
capture_stats() {
	local algorithm=$1 round=$2
	local prefix=$OUTPUT_DIR/${algorithm}.r${round}
	local line orig compr mem payload_ratio effective_ratio write_rate read_rate
	adb_shell_clean cat "$MM_STAT" > "$prefix.mm_stat"
	if adb_shell test -e "$SDDC_STAT"; then
		adb_shell_clean cat "$SDDC_STAT" > "$prefix.sddc_stat"
	fi
	if adb_shell test -e "$ZMS_STAT"; then
		adb_shell_clean cat "$ZMS_STAT" > "$prefix.zms_stat"
	fi
	line=$(tr '\n' ' ' < "$prefix.mm_stat")
	read -r orig compr mem _ _ _ _ _ _ <<< "$line"
	payload_ratio=$(awk -v o="$orig" -v c="$compr" 'BEGIN {if (c) printf "%.4f", o / c; else print "0"}')
	effective_ratio=$(awk -v o="$orig" -v m="$mem" 'BEGIN {if (m) printf "%.4f", o / m; else print "0"}')
	write_rate=$(awk -v bytes="$INPUT_BYTES" -v ms="$write_ms" 'BEGIN {if (ms) printf "%.2f", bytes * 1000 / 1048576 / ms; else print "0"}')
	read_rate=$(awk -v bytes="$INPUT_BYTES" -v ms="$read_ms" 'BEGIN {if (ms) printf "%.2f", bytes * 1000 / 1048576 / ms; else print "0"}')
	printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$algorithm" "$round" "$write_ms" "$write_rate" "$read_ms" "$read_rate" "$orig" "$compr" "$mem" "$payload_ratio" "$effective_ratio" >> "$SUMMARY_CSV"
	printf '%s %s %s %s %s %s %s %s %s %s %s\n' "$algorithm" "$round" "$write_ms" "$write_rate" "$read_ms" "$read_rate" "$orig" "$compr" "$mem" "$payload_ratio" "$effective_ratio" >> "$SUMMARY_TXT"
}

if [[ -n $ACTIVE_SWAP ]]; then
	log "swapoff $ACTIVE_SWAP (used zram data will be migrated by the kernel)"
	device_exec "swapoff $ACTIVE_SWAP" || die "swapoff failed"
	DEVICE_MUTATED=1
fi

for ((round = 1; round <= ROUNDS; round++)); do
	for algorithm in "${ALGORITHMS[@]}"; do
		reset_device "$algorithm"
		device_exec "dd if=$REMOTE_INPUT of=/dev/null bs=1M 2>/dev/null" >/dev/null
		log "[$algorithm round $round] writing $INPUT_MIB MiB"
		write_ms=$(timed_dd "dd if=$REMOTE_INPUT of=$BLOCK_DEV bs=4096 count=$INPUT_PAGES 2>/dev/null")
		wait_for_observation
		log "[$algorithm round $round] reading $INPUT_MIB MiB"
		read_ms=$(timed_dd "dd if=$BLOCK_DEV of=/dev/null bs=4096 count=$INPUT_PAGES 2>/dev/null")
		capture_stats "$algorithm" "$round"
		log "[$algorithm round $round] write=${write_ms}ms read=${read_ms}ms"
	done
done

restore_device
printf 'algorithm,rounds,avg_write_ms,avg_write_MiB_s,avg_read_ms,avg_read_MiB_s,avg_payload_ratio,avg_effective_ratio\n' > "$AVERAGES_CSV"
printf 'algorithm rounds avg_write_ms avg_write_MiB_s avg_read_ms avg_read_MiB_s avg_payload_ratio avg_effective_ratio\n' > "$AVERAGES_TXT"
for algorithm in "${ALGORITHMS[@]}"; do
	awk -F, -v algorithm="$algorithm" 'NR > 1 && $1 == algorithm {
		n++; write_ms += $3; write_rate += $4; read_ms += $5; read_rate += $6;
		payload += $10; effective += $11
	} END {if (n) printf "%s,%d,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f\n",
		algorithm, n, write_ms/n, write_rate/n, read_ms/n, read_rate/n,
		payload/n, effective/n}' "$SUMMARY_CSV" >> "$AVERAGES_CSV"
done
tr ',' ' ' < "$AVERAGES_CSV" > "$AVERAGES_TXT"
log "results written to $OUTPUT_DIR"
column -t -s ' ' "$AVERAGES_TXT" 2>/dev/null || cat "$AVERAGES_TXT"
