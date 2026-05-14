#!/bin/bash
# ============================================================================
# 03_run_s1_t4_d300.sh —— scale=1 / -t 4 / -d 300 跑测（学生开箱即用）
#
# 全量 TPC-C + 5 分钟长压，是写入文档的"官方基线"配置。
#
# 它会做：
#   0. 检查二进制和 golden_s1 是否就绪；缺则自动调用 01_setup.sh 补
#   1. 关掉残留 server / driver
#   2. 用 golden_s1 秒级回滚 tpcc_db（vs 重灌 2~3 分钟）
#   3. 起 server
#   4. 后台启 driver + 心跳轮询（绕开终端 30 秒静默检测）
#   5. 优雅关 server，输出结果摘要
#
# 用法：
#   bash scripts/03_run_s1_t4_d300.sh
#   T=8 D=600 bash scripts/03_run_s1_t4_d300.sh
#
# 学生 FAQ：
#   * "为什么后台跑而不是前台？"——很多终端和 ssh 心跳会在 30 秒静默后中断，
#     5 分钟全程沉默必断；改后台 + 心跳每 25 秒报活就稳了。
#   * "看见 abort 是不是 bug？"——不是。当前已是 S/X 表锁 + no-wait 死锁预防，
#     scale=1 + t=4 下少量 abort 属正常死锁规避，不影响数据正确性。
# ============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/bin
DBROOT=$ROOT/build/dbroot
DB=tpcc_db
GOLDEN_NAME="${DB}.golden_s1"
GOLDEN="$DBROOT/$GOLDEN_NAME"
PORT=8765

T=${T:-4}
D=${D:-300}
S=1
LOG="driver_s1_t${T}_d${D}.log"
SRVLOG="server_s1_t${T}_d${D}.log"

log()  { echo "[run_s1 $(date '+%H:%M:%S')] $*"; }
fail() { echo; echo "[run_s1 ❌ FAIL] $*"; exit 1; }

# ---------- 0. 自检 + 自愈 ----------
if [[ ! -x "$BIN/rmdb" || ! -x "$BIN/tpcc_driver" ]]; then
    log "二进制缺失，自动调用 01_setup.sh ..."
    bash "$ROOT/scripts/01_setup.sh" --only=1
fi
if [[ ! -d "$GOLDEN" ]]; then
    log "$GOLDEN_NAME 不存在，将自动调用 01_setup.sh --only=1（约 2~3 分钟） ..."
    bash "$ROOT/scripts/01_setup.sh" --only=1 --skip-build
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
log "用 golden 还原 $DB ($(du -sh "$GOLDEN" | cut -f1)) ..."
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

# ---------- 4. 后台启 driver ----------
log "launching driver in background: -t $T -d $D -S $S (预计 ${D} 秒 + 数据扫描)"
: > "$LOG"
nohup setsid "$BIN/tpcc_driver" \
    -h 127.0.0.1 -p $PORT -w 1 -t $T -d $D -S $S \
    > "$LOG" 2>&1 < /dev/null & disown
sleep 1
DRIVER_PID=$(pgrep -f "tpcc_driver -h 127.0.0.1 -p $PORT" | head -1 || echo "")
log "driver pid=$DRIVER_PID"

# ---------- 5. 心跳轮询（每 25 秒报活，绕开 30 秒静默检测） ----------
deadline=$(( $(date +%s) + D + 180 ))   # 留 3 分钟 buffer
while true; do
    if grep -q "TPC-C Result" "$LOG" 2>/dev/null; then
        log "✓ result ready"
        break
    fi
    if ! pgrep -f "tpcc_driver -h 127.0.0.1 -p $PORT" >/dev/null; then
        log "driver 已退出（看 $LOG 末尾确认是否正常）"
        break
    fi
    if [[ $(date +%s) -gt $deadline ]]; then
        log "⚠️ 超时（driver 在 d+180s 后仍存活），SIGINT kill ..."
        pkill -INT -f "tpcc_driver -h 127.0.0.1 -p $PORT" 2>/dev/null || true
        sleep 5
        pkill -KILL -f "tpcc_driver -h 127.0.0.1 -p $PORT" 2>/dev/null || true
        break
    fi
    last_line=$(tail -1 "$LOG" 2>/dev/null | cut -c1-80)
    log "alive, waiting ... (driver.log tail: ${last_line:-<empty>})"
    sleep 25
done

# ---------- 6. 优雅关 server ----------
log "stopping rmdb ..."
kill -INT $SRV_PID 2>/dev/null || true
for _ in $(seq 1 60); do
    kill -0 $SRV_PID 2>/dev/null || break
    sleep 0.5
done

# ---------- 7. 结果摘要 ----------
log "========== RESULT (scale=$S, t=$T, d=$D) =========="
if grep -q "TPC-C Result" "$LOG"; then
    grep -E "duration|committed|aborted|TPS|tpmC|latency" "$LOG" | sed 's/^/    /'
else
    echo "    ⚠️ no final result, last 30 lines of driver log:"
    tail -30 "$LOG" | sed 's/^/    /'
fi
log "driver log: $DBROOT/$LOG"
log "server log: $DBROOT/$SRVLOG"
