#!/bin/bash
# ============================================================================
# 02_run_s10_t4_d60.sh —— scale=10 / -t 4 / -d 60 跑测（学生开箱即用）
#
# 中等规模 + 60 秒，约 80 秒出结果，适合每次代码改动后回归。
#
# 它会做：
#   0. 检查二进制和 golden_s10 是否就绪；缺则自动调用 01_setup.sh 补
#   1. 关掉残留 server / driver
#   2. 用 golden_s10 秒级回滚 tpcc_db
#   3. 起 server
#   4. 跑 driver（前台，60 秒可接受）
#   5. 优雅关 server，输出结果摘要
#
# 用法：
#   bash scripts/02_run_s10_t4_d60.sh                # 默认 t=4 / d=60
#   T=8 D=120 bash scripts/02_run_s10_t4_d60.sh      # 临时改参数
# ============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/bin
DBROOT=$ROOT/build/dbroot
DB=tpcc_db
GOLDEN_NAME="${DB}.golden_s10"
GOLDEN="$DBROOT/$GOLDEN_NAME"
PORT=8765

T=${T:-4}
D=${D:-60}
S=10
LOG="driver_s10_t${T}_d${D}.log"
SRVLOG="server_s10_t${T}_d${D}.log"

log()  { echo "[run_s10 $(date '+%H:%M:%S')] $*"; }
fail() { echo; echo "[run_s10 ❌ FAIL] $*"; exit 1; }

# ---------- 0. 自检 + 自愈 ----------
if [[ ! -x "$BIN/rmdb" || ! -x "$BIN/tpcc_driver" ]]; then
    log "二进制缺失，自动调用 01_setup.sh ..."
    bash "$ROOT/scripts/01_setup.sh" --only=10
fi
if [[ ! -d "$GOLDEN" ]]; then
    log "$GOLDEN_NAME 不存在，自动调用 01_setup.sh --only=10 ..."
    bash "$ROOT/scripts/01_setup.sh" --only=10 --skip-build
fi
[[ -d "$GOLDEN" ]] || fail "自愈失败：仍找不到 $GOLDEN"

mkdir -p "$DBROOT"
cd "$DBROOT"

# ---------- 1. 清理残留 ----------
if pgrep -f "tpcc_driver -h 127.0.0.1 -p $PORT" >/dev/null; then
    log "kill 残留 driver ..."
    pkill -INT -f "tpcc_driver -h 127.0.0.1 -p $PORT" || true
    sleep 2
fi
if pgrep -f "rmdb $DB" >/dev/null; then
    log "stop 残留 rmdb (SIGINT) ..."
    kill -INT $(pgrep -f "rmdb $DB") || true
    for _ in $(seq 1 60); do
        pgrep -f "rmdb $DB" >/dev/null || break
        sleep 0.5
    done
    pgrep -f "rmdb $DB" >/dev/null && fail "rmdb 30 秒未退出，请手动排查（绝勿 kill -9）"
fi

# ---------- 2. 秒级回滚 ----------
log "用 golden 还原 $DB ..."
rm -rf "$DB"
cp -a "$GOLDEN" "$DB"

# ---------- 3. 起 server ----------
log "starting rmdb ..."
nohup setsid "$BIN/rmdb" "$DB" > "$SRVLOG" 2>&1 < /dev/null &
SRV_PID=$!
for _ in $(seq 1 40); do
    sleep 0.5
    (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break
done
if ! (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    tail -20 "$SRVLOG"
    fail "rmdb 未监听 $PORT，详见 $DBROOT/$SRVLOG"
fi
log "rmdb pid=$SRV_PID listening on $PORT"

# ---------- 4. 跑 driver ----------
log "running driver: -t $T -d $D -S $S ..."
"$BIN/tpcc_driver" -h 127.0.0.1 -p $PORT -w 1 -t $T -d $D -S $S | tee "$LOG"
log "driver finished."

# ---------- 5. 优雅关 server ----------
log "stopping rmdb ..."
kill -INT $SRV_PID 2>/dev/null || true
for _ in $(seq 1 60); do
    kill -0 $SRV_PID 2>/dev/null || break
    sleep 0.5
done

# ---------- 6. 结果摘要 ----------
log "========== RESULT (scale=$S, t=$T, d=$D) =========="
if grep -q "TPC-C Result" "$LOG"; then
    grep -E "duration|committed|aborted|TPS|tpmC|latency" "$LOG" | sed 's/^/    /'
else
    echo "    ⚠️ no final result, last 30 lines of driver log:"
    tail -30 "$LOG" | sed 's/^/    /'
fi
log "driver log: $DBROOT/$LOG"
log "server log: $DBROOT/$SRVLOG"
