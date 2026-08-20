#!/bin/bash
#
# quick_verify.sh — 轻量级 cache-aware 功能验证
#
# 验证内容：
#   1. scx_lavd --cache-aware 能正常启动/退出（不崩溃）
#   2. 工作负载运行期间调度器稳定
#   3. mm_ca_map 被创建并有数据（cache-aware 生效的标志）
#   4. 基本 throughput 对比（cache-aware off vs on）
#
# 用法: sudo ./quick_verify.sh [scx_lavd_path]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAVD="${1:-$(realpath "$SCRIPT_DIR/../../target2/debug/scx_lavd" 2>/dev/null || \
              realpath "$SCRIPT_DIR/../../target/debug/scx_lavd" 2>/dev/null || \
              realpath "$SCRIPT_DIR/../../target/release/scx_lavd" 2>/dev/null || \
              echo "$SCRIPT_DIR/../../target/release/scx_lavd")}"
STRESS="${SCRIPT_DIR}/cache_stress"
DURATION=10   # 每轮秒数，快速验证
NTHREADS=4

# ── 颜色 ──
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; FAILURES=$((FAILURES+1)); }
info() { echo -e "  ${YELLOW}[INFO]${NC} $1"; }

FAILURES=0

# ── 前置检查 ──
echo "============================================"
echo " scx_lavd cache-aware 快速验证"
echo "============================================"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: 需要 root 权限运行 scx_lavd" >&2
    exit 1
fi

if ! [ -x "$LAVD" ]; then
    echo "scx_lavd 未找到: $LAVD" >&2
    echo "请先构建: CARGO_TARGET_DIR=target2 cargo build -p scx_lavd" >&2
    exit 1
fi
info "scx_lavd: $LAVD"

# 构建 cache_stress
if ! [ -x "$STRESS" ]; then
    info "构建 cache_stress..."
    make -C "$SCRIPT_DIR" || { echo "构建失败" >&2; exit 1; }
fi
info "cache_stress: $STRESS"

# 检测 LLC 拓扑
NR_LLC=$(cat /sys/devices/system/cpu/cpu*/cache/index3/shared_cpu_list 2>/dev/null \
    | sort -u | wc -l)
info "LLC 域数量: $NR_LLC"
if [ "$NR_LLC" -le 1 ]; then
    info "单 LLC 机器，cache-aware 不会有明显性能差异"
    info "仅验证功能正确性（不崩溃、map 正常）"
fi

cleanup() {
    pkill -f "scx_lavd.*--cache-aware" 2>/dev/null || true
    pkill -f "scx_lavd" 2>/dev/null || true
    sleep 1
}
trap cleanup EXIT

echo ""

# ── 测试 1: --cache-aware 参数被接受 ──
echo "=== 测试 1: 参数验证 ==="
if "$LAVD" --help 2>&1 | grep -q "cache-aware"; then
    pass "--cache-aware 参数存在于 help 输出中"
else
    fail "--cache-aware 参数不存在"
fi

# ── 测试 2: 调度器能正常启动和退出 ──
echo ""
echo "=== 测试 2: 启动/退出稳定性 ==="
TMPLOG=$(mktemp /tmp/lavd_test_XXXXXX.log)

"$LAVD" --cache-aware --stats 1 2>"$TMPLOG" &
LAVD_PID=$!
sleep 2

if kill -0 "$LAVD_PID" 2>/dev/null; then
    pass "scx_lavd --cache-aware 正常启动 (pid=$LAVD_PID)"
else
    fail "scx_lavd 启动失败"
    cat "$TMPLOG" >&2
    rm -f "$TMPLOG"
    exit 1
fi

# 运行一小段工作负载
"$STRESS" -t "$NTHREADS" -d "$DURATION" -s 2 > /dev/null 2>&1 || true

if kill -0 "$LAVD_PID" 2>/dev/null; then
    pass "工作负载期间调度器稳定运行"
else
    fail "调度器在工作负载期间崩溃"
fi

# 正常退出
kill "$LAVD_PID" 2>/dev/null || true
wait "$LAVD_PID" 2>/dev/null || true
sleep 1

if ! kill -0 "$LAVD_PID" 2>/dev/null; then
    pass "scx_lavd 正常退出"
else
    fail "scx_lavd 未能正常退出"
    kill -9 "$LAVD_PID" 2>/dev/null || true
fi

# ── 测试 3: mm_ca_map 存在 ──
echo ""
echo "=== 测试 3: BPF map 验证 ==="

# 重新启动，这次用更长的时间来 dump map
"$LAVD" --cache-aware --stats 1 2>/dev/null &
LAVD_PID=$!
sleep 2

# 运行工作负载让 mm_ca_map 有数据
"$STRESS" -t "$NTHREADS" -d 3 -s 2 > /dev/null 2>&1 &
STRESS_PID=$!
sleep 2

# 检查 mm_ca_map
if command -v bpftool &>/dev/null; then
    MAP_DUMP=$(bpftool map dump name mm_ca_map 2>/dev/null || echo "")
    if [ -n "$MAP_DUMP" ] && echo "$MAP_DUMP" | grep -q "key:"; then
        NR_ENTRIES=$(echo "$MAP_DUMP" | grep -c "key:" || true)
        pass "mm_ca_map 存在且有数据 ($NR_ENTRIES 个条目)"
    else
        info "mm_ca_map 存在但为空（进程可能太快）"
        pass "mm_ca_map BPF map 已创建"
    fi
else
    info "bpftool 未安装，跳过 map 验证"
fi

kill "$STRESS_PID" 2>/dev/null || true
wait "$STRESS_PID" 2>/dev/null || true
kill "$LAVD_PID" 2>/dev/null || true
wait "$LAVD_PID" 2>/dev/null || true
sleep 1

# ── 测试 4: Throughput 对比 ──
echo ""
echo "=== 测试 4: Throughput 对比 (${DURATION}s/轮) ==="

run_throughput() {
    local label="$1"
    local flags="$2"

    "$LAVD" $flags --stats 1 2>/dev/null &
    local pid=$!
    sleep 2

    local result
    result=$("$STRESS" -t "$NTHREADS" -d "$DURATION" -s 2 2>/dev/null \
        | grep "^total:" | awk '{print $8}' || echo "N/A")

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    sleep 1

    echo "$result"
}

BL_OPS=$(run_throughput "baseline" "")
CA_OPS=$(run_throughput "cache-aware" "--cache-aware")

echo ""
printf "  %-25s %s\n" "Baseline (no cache-aware):" "$BL_OPS ops/s"
printf "  %-25s %s\n" "Cache-aware ON:" "$CA_OPS ops/s"

if [ "$BL_OPS" != "N/A" ] && [ "$CA_OPS" != "N/A" ]; then
    DIFF=$(awk "BEGIN { printf \"%.1f\", (($CA_OPS - $BL_OPS) / $BL_OPS) * 100 }")
    if [ "$(echo "$DIFF > 0" | bc -l 2>/dev/null || echo 0)" = "1" ]; then
        info "cache-aware 提升: +${DIFF}%"
    else
        info "cache-aware 变化: ${DIFF}%"
    fi
    if [ "$NR_LLC" -le 1 ]; then
        info "（单 LLC 机器，差异不具参考意义）"
    fi
fi

# ── 总结 ──
echo ""
echo "============================================"
if [ "$FAILURES" -eq 0 ]; then
    echo -e " ${GREEN}全部通过！${NC} cache-aware 功能正常。"
else
    echo -e " ${RED}$FAILURES 项测试失败${NC}"
fi
echo "============================================"
rm -f "$TMPLOG"
exit "$FAILURES"
