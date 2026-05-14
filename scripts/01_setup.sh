#!/bin/bash
# ============================================================================
# 01_setup.sh —— 一次性准备脚本（学生开箱即用）
#
# 这一个脚本做完之后，scale=1 / scale=10 的 golden 备份都会就位，
# 后续 02/03 只需秒级回滚即可反复跑测。
#
# 它会做：
#   0. 环境预检：cmake / g++ / make / 端口 8765 / nproc
#   1. 编译 rmdb / tpcc_loader / tpcc_driver（增量编译）
#   2. 灌 scale=10（小，~30秒）→ 制作 golden_s10  ← 先做小的验证链路
#   3. 灌 scale=1 （大，2~3分钟）→ 制作 golden_s1
#
# 用法：
#   bash scripts/01_setup.sh                # 智能：缺哪个 golden 就补哪个
#   bash scripts/01_setup.sh --rebuild      # 强制全部重做（覆盖已有 golden）
#   bash scripts/01_setup.sh --skip-build   # 跳过编译，只补 golden
#   bash scripts/01_setup.sh --only=10      # 只做 scale=10 的 golden（快速体验）
#   bash scripts/01_setup.sh --only=1       # 只做 scale=1 的 golden
#
# 关键铁律（违反任何一条都会损坏 golden）：
#   * 关 server 一律用 kill -INT，等进程优雅退出后再做下一步
#   * golden 目录只在 server 完全退出后才能 cp -a
#   * 学生私自 kill -9 会跳过 close_db()，整个库的脏页与元信息一起丢
# ============================================================================
set -euo pipefail

# ---------- 0. 路径常量 ----------
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=$ROOT/build/bin
DBROOT=$ROOT/build/dbroot
DB=tpcc_db
PORT=8765

REBUILD=0
SKIP_BUILD=0
ONLY=""
for arg in "$@"; do
    case "$arg" in
        --rebuild)     REBUILD=1 ;;
        --skip-build)  SKIP_BUILD=1 ;;
        --only=1)      ONLY=1 ;;
        --only=10)     ONLY=10 ;;
        -h|--help)
            sed -n '2,28p' "$0"; exit 0 ;;
        *)
            echo "[setup] unknown arg: $arg"
            echo "        run with -h to see usage"; exit 1 ;;
    esac
done

log()  { echo "[setup $(date '+%H:%M:%S')] $*"; }
fail() { echo; echo "[setup ❌ FAIL] $*"; exit 1; }

# ---------- 1. 环境预检 ----------
log "checking environment ..."
need_cmd() { command -v "$1" >/dev/null 2>&1 || fail "缺少命令 '$1'，请先安装（Ubuntu: sudo apt install $2）"; }
need_cmd cmake "cmake"
need_cmd g++   "g++"
need_cmd make  "make"
need_cmd cp    "coreutils"
need_cmd pgrep "procps"

# CMake 版本：项目要求 ≥ 3.16
cmake_ver=$(cmake --version | head -1 | awk '{print $3}')
log "  cmake=$cmake_ver  g++=$(g++ -dumpversion)  cores=$(nproc)"

# 端口占用检查（友好提示）
if (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
    echo
    echo "[setup ⚠️] 端口 $PORT 已被占用，可能是上次跑测残留的 rmdb。"
    echo "          下面会尝试自动用 SIGINT 关掉它（绝不 kill -9，以免丢库）。"
    echo
fi

# ---------- 2. 编译 ----------
if [[ $SKIP_BUILD -eq 0 ]]; then
    log "compiling rmdb / tpcc_loader / tpcc_driver ..."
    if ! cmake -B "$ROOT/build" -S "$ROOT" >/tmp/setup_cmake.log 2>&1; then
        tail -20 /tmp/setup_cmake.log
        fail "cmake configure 失败，完整日志见 /tmp/setup_cmake.log"
    fi
    if ! cmake --build "$ROOT/build" --target rmdb tpcc_loader tpcc_driver \
           -j"$(nproc)" >/tmp/setup_build.log 2>&1; then
        tail -30 /tmp/setup_build.log
        fail "编译失败，完整日志见 /tmp/setup_build.log（常见原因：g++ 版本 < 9 / 缺 readline-dev）"
    fi
    log "compile done."
else
    log "skip build (per --skip-build)"
fi

# 校验产物
for f in rmdb tpcc_loader tpcc_driver; do
    [[ -x "$BIN/$f" ]] || fail "缺少二进制 $BIN/$f，请去掉 --skip-build 重跑"
done

mkdir -p "$DBROOT"
cd "$DBROOT"

# ---------- 3. 工具函数 ----------
stop_server() {
    if pgrep -f "rmdb $DB" >/dev/null; then
        log "stopping rmdb (SIGINT, 优雅) ..."
        kill -INT $(pgrep -f "rmdb $DB") || true
        for _ in $(seq 1 60); do
            pgrep -f "rmdb $DB" >/dev/null || return 0
            sleep 0.5
        done
        # 兜底：30 秒还没退？大概率 close_db 卡了，但这里仍不 kill -9
        fail "rmdb 30 秒未退出，请手动检查（不要 kill -9，会丢数据）"
    fi
}

start_server() {
    nohup setsid "$BIN/rmdb" "$DB" > server.log 2>&1 < /dev/null &
    for _ in $(seq 1 40); do
        sleep 0.5
        (echo > /dev/tcp/127.0.0.1/$PORT) 2>/dev/null && return 0
    done
    tail -20 server.log
    fail "rmdb 20 秒内未监听 $PORT，请检查 $DBROOT/server.log"
}

make_golden() {
    # $1 = scale_div  $2 = golden_dir  $3 = 预估耗时（人话）
    local SCALE=$1
    local GOLDEN=$2
    local ETA=$3

    if [[ -d "$GOLDEN" && $REBUILD -eq 0 ]]; then
        log "✓ $GOLDEN 已存在，跳过（如需重做请加 --rebuild）"
        return 0
    fi

    log "=== 制作 golden: scale=$SCALE → $GOLDEN  (预计 $ETA) ==="

    stop_server
    rm -rf "$DB"
    [[ $REBUILD -eq 1 ]] && rm -rf "$GOLDEN"

    start_server

    local t0=$(date +%s)
    if ! "$BIN/tpcc_loader" -h 127.0.0.1 -p $PORT -w 1 -S "$SCALE" \
           > "loader_s${SCALE}.log" 2>&1; then
        tail -30 "loader_s${SCALE}.log"
        stop_server
        fail "tpcc_loader 失败（scale=$SCALE），完整日志见 $DBROOT/loader_s${SCALE}.log"
    fi
    local cost=$(( $(date +%s) - t0 ))
    log "loader 完成，耗时 ${cost}s"

    # 关键：备份前必须先 SIGINT 关 server 等 close_db 落盘
    stop_server

    log "复制 $DB → $GOLDEN ..."
    cp -a "$DB" "$GOLDEN"
    log "✓ $GOLDEN 就绪，size=$(du -sh "$GOLDEN" | cut -f1)"
}

# ---------- 4. 按需制作 golden ----------
case "$ONLY" in
    10) make_golden 10 "${DB}.golden_s10" "约 30 秒" ;;
    1)  make_golden 1  "${DB}.golden_s1"  "约 2~3 分钟" ;;
    "") # 默认两个都做，先小后大（小的失败就早点暴露）
        make_golden 10 "${DB}.golden_s10" "约 30 秒"
        make_golden 1  "${DB}.golden_s1"  "约 2~3 分钟"
        ;;
esac

# ---------- 5. 收尾 ----------
stop_server

log "=========================================="
log "✅ 准备完成 ALL READY"
[[ -d "${DB}.golden_s10" ]] && ls -ld "${DB}.golden_s10" | awk '{print "    "$0}'
[[ -d "${DB}.golden_s1"  ]] && ls -ld "${DB}.golden_s1"  | awk '{print "    "$0}'
log "下一步可执行："
log "    bash scripts/02_run_s10_t4_d60.sh    # 中规模 60 秒压测"
log "    bash scripts/03_run_s1_t4_d300.sh    # 全量 5 分钟长压"
log "=========================================="
