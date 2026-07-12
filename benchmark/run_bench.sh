#!/usr/bin/env bash
# benchmark/run_bench.sh — Run redis-benchmark against redis_cpp server.
#
# Requirements:
#   - redis_cpp running: ./build/redis_cpp --port 6380
#   - redis-benchmark installed: apt install redis-tools
#
# Usage:
#   ./benchmark/run_bench.sh [PORT]

set -euo pipefail

PORT="${1:-6380}"
HOST="127.0.0.1"
CLIENTS=50
REQUESTS=100000
DATA_SIZE=64   # value size in bytes
PIPELINE=16    # commands per pipeline

echo "════════════════════════════════════════════════════════"
echo "  redis_cpp benchmark — redis-benchmark"
echo "  Host: $HOST:$PORT  Clients: $CLIENTS  Requests: $REQUESTS"
echo "  Data size: ${DATA_SIZE}B  Pipeline: $PIPELINE"
echo "════════════════════════════════════════════════════════"
echo ""

# Check server is reachable
if ! redis-cli -h "$HOST" -p "$PORT" PING > /dev/null 2>&1; then
    echo "ERROR: redis_cpp server not running on $HOST:$PORT"
    echo "Start it with: ./build/redis_cpp --port $PORT"
    exit 1
fi

echo "✓ Server is up"
echo ""

# ── 1. PING (baseline) ───────────────────────────────────────────────────────
echo "── PING (baseline) ──────────────────────────────────────"
redis-benchmark -h "$HOST" -p "$PORT" -c "$CLIENTS" -n "$REQUESTS" \
    -t ping_inline,ping_mbulk --csv

echo ""

# ── 2. SET ───────────────────────────────────────────────────────────────────
echo "── SET ($DATA_SIZE byte values) ─────────────────────────"
redis-benchmark -h "$HOST" -p "$PORT" -c "$CLIENTS" -n "$REQUESTS" \
    -d "$DATA_SIZE" -t set --csv

echo ""

# ── 3. GET ───────────────────────────────────────────────────────────────────
echo "── GET ──────────────────────────────────────────────────"
redis-benchmark -h "$HOST" -p "$PORT" -c "$CLIENTS" -n "$REQUESTS" \
    -d "$DATA_SIZE" -t get --csv

echo ""

# ── 4. SET + GET mix with pipelining ────────────────────────────────────────
echo "── SET+GET with pipeline=$PIPELINE ─────────────────────"
redis-benchmark -h "$HOST" -p "$PORT" -c "$CLIENTS" -n "$REQUESTS" \
    -d "$DATA_SIZE" -t set,get -P "$PIPELINE" --csv

echo ""

# ── 5. SET with EX (TTL test) ────────────────────────────────────────────────
echo "── SET with EX 10 (TTL) ─────────────────────────────────"
redis-benchmark -h "$HOST" -p "$PORT" -c "$CLIENTS" -n "$REQUESTS" \
    --csv \
    -e \
    -r 1000000 \
    -d "$DATA_SIZE" \
    -t set

echo ""

# ── 6. DEL ───────────────────────────────────────────────────────────────────
echo "── DEL ──────────────────────────────────────────────────"
redis-benchmark -h "$HOST" -p "$PORT" -c "$CLIENTS" -n "$REQUESTS" \
    -t del --csv 2>/dev/null || true  # DEL test may not exist in older versions

echo ""

# ── 7. Throughput summary ────────────────────────────────────────────────────
echo "════════════════════════════════════════════════════════"
echo "  Full suite (all commands, no pipeline)"
echo "════════════════════════════════════════════════════════"
redis-benchmark -h "$HOST" -p "$PORT" -c "$CLIENTS" -n "$REQUESTS" \
    -d "$DATA_SIZE" -t ping_inline,ping_mbulk,set,get,incr,lpush,rpush,lpop,rpop,sadd \
    --csv 2>/dev/null || true

echo ""
echo "Done. Compare with real Redis:"
echo "  redis-benchmark -h $HOST -p 6379 -c $CLIENTS -n $REQUESTS -d $DATA_SIZE -t set,get --csv"
