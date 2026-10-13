#!/usr/bin/env bash

set -u

BASE_PORT=1234
HTTP_PORT=8080

RUNTIME="runtime"

mkdir -p "$RUNTIME"

SERVER_LOG="$RUNTIME/m4_server.log"


if ss -ltn | grep -q ":${BASE_PORT} "; then
    echo "[FAIL] Port ${BASE_PORT} is already in use."
    echo "Stop the existing server first."
    exit 1
fi


rm -f "$SERVER_LOG"
rm -f gateway.log
rm -f "$RUNTIME"/m4_sensor_*.log


stdbuf -oL -eL \
    ./build/bin/server \
    "$BASE_PORT" \
    "$HTTP_PORT" \
    > "$SERVER_LOG" 2>&1 &


SERVER_PID=$!


cleanup()
{
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -INT "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}


trap cleanup EXIT INT TERM


sleep 1


if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[FAIL] Server did not start."
    cat "$SERVER_LOG"
    exit 1
fi


echo "[OK] Server PID=$SERVER_PID"


PIDS=()


launch_sensor()
{
    local type="$1"
    local id="$2"
    local period="$3"
    shift 3

    timeout -s INT 5 \
        ./build/bin/sensor_node \
        "$type" \
        "$id" \
        127.0.0.1 \
        "$BASE_PORT" \
        "$period" \
        "$@" \
        > "$RUNTIME/m4_sensor_${id}.log" \
        2>&1 &

    PIDS+=("$!")
}


wait_sensors()
{
    for pid in "${PIDS[@]}"; do
        wait "$pid" || true
    done

    PIDS=()
}


echo
echo "=== Baseline: 3 sensors per type ==="


for id in 10 11 12; do
    launch_sensor temperature "$id" 250
done

for id in 20 21 22; do
    launch_sensor humidity "$id" 250
done

for id in 30 31 32; do
    launch_sensor motion "$id" 250
done

for id in 40 41 42; do
    launch_sensor door "$id" 250
done

for id in 50 51 52; do
    launch_sensor smoke_gas "$id" 250
done


wait_sensors


echo
echo "=== --fragment ==="


launch_sensor temperature 110 300 --fragment
launch_sensor humidity    120 300 --fragment
launch_sensor motion      130 300 --fragment
launch_sensor door        140 300 --fragment
launch_sensor smoke_gas   150 300 --fragment

wait_sensors


echo
echo "=== --glue ==="


launch_sensor temperature 111 300 --glue
launch_sensor humidity    121 300 --glue
launch_sensor motion      131 300 --glue
launch_sensor door        141 300 --glue
launch_sensor smoke_gas   151 300 --glue

wait_sensors


echo
echo "=== --corrupt ==="


launch_sensor temperature 112 250 --corrupt
launch_sensor humidity    122 250 --corrupt
launch_sensor motion      132 250 --corrupt
launch_sensor door        142 250 --corrupt
launch_sensor smoke_gas   152 250 --corrupt

wait_sensors


cleanup

trap - EXIT INT TERM


echo
echo "=== Verify measurements ==="


FAIL=0


ALL_IDS=(
    10 11 12
    20 21 22
    30 31 32
    40 41 42
    50 51 52

    110 120 130 140 150
    111 121 131 141 151
    112 122 132 142 152
)


for id in "${ALL_IDS[@]}"; do

    CONN=$(
        grep -c \
        "\[CONN\].* frame: id=${id} " \
        "$SERVER_LOG" \
        || true
    )

    DATA=$(
        grep -c \
        "\[DATA\] id=${id} " \
        "$SERVER_LOG" \
        || true
    )

    STOR=$(
        grep -c \
        "\[STOR\] id=${id} " \
        "$SERVER_LOG" \
        || true
    )


    printf \
        "id=%-3s CONN=%-3s DATA=%-3s STOR=%-3s " \
        "$id" \
        "$CONN" \
        "$DATA" \
        "$STOR"


    if (
        [ "$CONN" -gt 0 ] &&
        [ "$CONN" -eq "$DATA" ] &&
        [ "$DATA" -eq "$STOR" ]
    ); then

        echo "PASS"

    else

        echo "FAIL"

        FAIL=1
    fi
done


echo
echo "=== Verify malformed-frame logging ==="


for protocol in TBP HLP EVP SGP; do

    if grep -q \
        "Received malformed frame on ${protocol} port" \
        gateway.log; then

        echo "${protocol}: PASS"

    else

        echo "${protocol}: FAIL"

        FAIL=1
    fi
done


echo
echo "=== Shutdown ==="


if grep -q \
    "Gateway stopped cleanly" \
    "$SERVER_LOG"; then

    echo "Clean shutdown: PASS"

else

    echo "Clean shutdown: FAIL"

    FAIL=1
fi


echo


if [ "$FAIL" -eq 0 ]; then

    echo "================================"
    echo "M4 FUNCTIONAL TEST: PASS"
    echo "================================"

    exit 0
fi


echo "================================"
echo "M4 FUNCTIONAL TEST: FAIL"
echo "================================"

exit 1