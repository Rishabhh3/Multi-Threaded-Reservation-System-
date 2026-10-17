#!/usr/bin/env bash
#
# Automated benchmark harness.
#
# Sweeps the system-wide concurrency cap (MAX) across three regimes at fixed
# defaults (e=100 c=500 s=20), repeats each config, and records throughput,
# end-to-end latency percentiles and retry rate to results.csv.
#
#   normal          MAX = 3, 5    -- cap below what the threads can drive
#   saturated       MAX = 10      -- cap near the knee
#   over-subscribed MAX = 20, 40  -- cap at or above the thread count
#
# Usage: ./bench.sh            (or: make bench)

set -uo pipefail

BIN=./reservation_system
OUT=results.csv

EVENTS=100
CAPACITY=500
THREADS=20
DURATION=15
REPS=5
MAXES=(3 5 10 20 40)

regime() {
    case "$1" in
        3|5) echo "normal" ;;
        10)  echo "saturated" ;;
        *)   echo "over-subscribed" ;;
    esac
}

# Pull one key=value out of the program's RESULT line.
field() {
    printf '%s\n' "$1" | tr ',' '\n' | awk -F= -v k="$2" '$1==k {print $2; exit}'
}

if [ ! -x "$BIN" ]; then
    echo "==> building"
    make || exit 1
fi

total_runs=$(( ${#MAXES[@]} * REPS ))
echo "==> $total_runs runs x ${DURATION}s (~$(( total_runs * DURATION / 60 )) min)"
echo "==> e=$EVENTS c=$CAPACITY s=$THREADS, MAX in ${MAXES[*]}, $REPS reps each"
echo

echo "max,run,succeeded,attempted,retry_rate,qps,lat_mean_ms,lat_p50_ms,lat_p95_ms,lat_p99_ms,cap_wait_s" > "$OUT"

run_no=0
failures=0
for m in "${MAXES[@]}"; do
    for r in $(seq 1 "$REPS"); do
        run_no=$(( run_no + 1 ))
        printf "[%2d/%2d] MAX=%-3d rep %d ... " "$run_no" "$total_runs" "$m" "$r"

        out=$("$BIN" -e "$EVENTS" -c "$CAPACITY" -m "$m" -s "$THREADS" -t "$DURATION" -q 2>&1)
        line=$(printf '%s\n' "$out" | grep '^RESULT,' | head -1)

        if [ -z "$line" ]; then
            echo "NO RESULT LINE (run failed)"
            failures=$(( failures + 1 ))
            continue
        fi

        inv=$(field "$line" invariant)
        if [ "$inv" != "PASSED" ]; then
            echo "INVARIANT $inv"
            failures=$(( failures + 1 ))
        fi

        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$m" "$r" \
            "$(field "$line" succeeded)" \
            "$(field "$line" attempted)" \
            "$(field "$line" retry_rate)" \
            "$(field "$line" qps)" \
            "$(field "$line" lat_mean_ms)" \
            "$(field "$line" lat_p50_ms)" \
            "$(field "$line" lat_p95_ms)" \
            "$(field "$line" lat_p99_ms)" \
            "$(field "$line" cap_wait_s)" >> "$OUT"

        if [ "$inv" = "PASSED" ]; then
            echo "qps=$(field "$line" qps) retry=$(field "$line" retry_rate)% p95=$(field "$line" lat_p95_ms)ms"
        fi
    done
done

echo
echo "==> wrote $OUT"
echo
echo "Summary: mean +/- sample stddev over $REPS reps of ${DURATION}s"
echo
printf "%-16s %4s  %15s  %17s  %17s  %17s  %17s\n" \
    "regime" "MAX" "retry %" "throughput q/s" "lat mean ms" "lat p95 ms" "cap wait s"
printf '%s\n' "----------------------------------------------------------------------------------------------------------------------"

for m in "${MAXES[@]}"; do
    awk -F, -v m="$m" -v reg="$(regime "$m")" '
        function mean(s, n)        { return n > 0 ? s / n : 0 }
        function sd(s, sq, n,   v) { if (n < 2) return 0; v = (sq - s*s/n) / (n-1); if (v < 0) v = 0; return sqrt(v) }
        NR > 1 && $1 == m {
            n++
            s5  += $5;  q5  += $5*$5
            s6  += $6;  q6  += $6*$6
            s7  += $7;  q7  += $7*$7
            s9  += $9;  q9  += $9*$9
            s11 += $11; q11 += $11*$11
        }
        END {
            if (n == 0) exit
            printf "%-16s %4d  %7.2f ±%-6.2f  %8.1f ±%-7.1f  %8.1f ±%-7.1f  %8.1f ±%-7.1f  %8.1f ±%-7.1f\n", \
                reg, m, \
                mean(s5,n),  sd(s5,q5,n), \
                mean(s6,n),  sd(s6,q6,n), \
                mean(s7,n),  sd(s7,q7,n), \
                mean(s9,n),  sd(s9,q9,n), \
                mean(s11,n), sd(s11,q11,n)
        }' "$OUT"
done

echo
if [ "$failures" -gt 0 ]; then
    echo "!! $failures run(s) failed or violated the invariant"
    exit 1
fi
echo "All runs passed the invariant check."
