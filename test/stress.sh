#!/usr/bin/env bash

set -u


DURATION="${DURATION:-180}"

BASE_PORT=1234
HTTP_PORT=8080

SERVER_LOG="runtime/m7_server.log"


mkdir -p runtime

rm -f "$SERVER_LOG"
rm -f gateway.log
rm -f gateway.db


echo "================================"
echo "M7 INTEGRATION / STRESS TEST"
echo "Duration: ${DURATION}s"
echo "================================"


# ------------------------------------------------------------
# Check stale server
# ------------------------------------------------------------

if ss -ltn | grep -q ":${BASE_PORT} "; then

    echo "[FAIL] Port ${BASE_PORT} already in use"
    exit 1
fi


# ------------------------------------------------------------
# Start gateway
# ------------------------------------------------------------

stdbuf -oL -eL \
    ./build/bin/server \
    "$BASE_PORT" \
    "$HTTP_PORT" \
    > "$SERVER_LOG" 2>&1 &

SERVER_PID=$!


sleep 2


if ! kill -0 "$SERVER_PID" 2>/dev/null; then

    echo "[FAIL] Gateway failed to start"

    cat "$SERVER_LOG"

    exit 1
fi


echo "[OK] Gateway PID=$SERVER_PID"


# ------------------------------------------------------------
# Start five sensor types concurrently
# ------------------------------------------------------------

timeout -s INT "$DURATION" \
    ./build/bin/sensor_node \
    temperature \
    10 \
    127.0.0.1 \
    "$BASE_PORT" \
    200 \
    > runtime/m7_temp.log 2>&1 &

P1=$!


timeout -s INT "$DURATION" \
    ./build/bin/sensor_node \
    humidity \
    21 \
    127.0.0.1 \
    "$BASE_PORT" \
    250 \
    > runtime/m7_humidity.log 2>&1 &

P2=$!


timeout -s INT "$DURATION" \
    ./build/bin/sensor_node \
    motion \
    30 \
    127.0.0.1 \
    "$BASE_PORT" \
    300 \
    > runtime/m7_motion.log 2>&1 &

P3=$!


timeout -s INT "$DURATION" \
    ./build/bin/sensor_node \
    door \
    40 \
    127.0.0.1 \
    "$BASE_PORT" \
    350 \
    > runtime/m7_door.log 2>&1 &

P4=$!


timeout -s INT "$DURATION" \
    ./build/bin/sensor_node \
    smoke_gas \
    50 \
    127.0.0.1 \
    "$BASE_PORT" \
    400 \
    > runtime/m7_gas.log 2>&1 &

P5=$!


echo "[OK] Five sensor types started"


# ------------------------------------------------------------
# Wait for sensors
# ------------------------------------------------------------

wait "$P1" || true
wait "$P2" || true
wait "$P3" || true
wait "$P4" || true
wait "$P5" || true


echo "[OK] Sensor stress period finished"


# ------------------------------------------------------------
# Graceful gateway shutdown
# ------------------------------------------------------------

kill -INT "$SERVER_PID"

wait "$SERVER_PID"

echo "[OK] Gateway exited"


# ------------------------------------------------------------
# Verification
# ------------------------------------------------------------

FAIL=0


if grep -q \
    "Gateway stopped cleanly" \
    "$SERVER_LOG"; then

    echo "Clean shutdown: PASS"

else

    echo "Clean shutdown: FAIL"
    FAIL=1
fi


if pgrep -f \
    './build/bin/server' \
    >/dev/null; then

    echo "Server process cleanup: FAIL"
    FAIL=1

else

    echo "Server process cleanup: PASS"
fi


if ss -ltn | grep -Eq \
    ':1234|:1235|:1236|:1237'; then

    echo "Socket cleanup: FAIL"
    FAIL=1

else

    echo "Socket cleanup: PASS"
fi


if [ -e logFifo ]; then

    echo "FIFO cleanup: FAIL"
    FAIL=1

else

    echo "FIFO cleanup: PASS"
fi


ROWS=$(
    sqlite3 gateway.db \
    'SELECT COUNT(*) FROM SensorData;' \
    2>/dev/null ||
    echo 0
)


echo "SQLite rows stored: $ROWS"


if [ "$ROWS" -gt 0 ]; then

    echo "SQLite persistence: PASS"

else

    echo "SQLite persistence: FAIL"
    FAIL=1
fi


echo


if [ "$FAIL" -eq 0 ]; then

    echo "================================"
    echo "M7 INTEGRATION TEST: PASS"
    echo "================================"

    exit 0
fi


echo "================================"
echo "M7 INTEGRATION TEST: FAIL"
echo "================================"

exit 1
