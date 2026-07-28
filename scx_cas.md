# scx_lavd cache-aware 补丁组分析报告

## 1. 基本信息

| 属性 | 内容 |
|-----|------|
| **补丁组主题** | scx_lavd: cache-aware load balancing |
| **目标调度器** | scx_lavd (sched-ext BPF 可编程调度器) |
| **作者** | Zhang Qiao `<zhangqiao22@huawei.com>` / `<7809628+zqiao216@user.noreply.gitee.com>` |
| **分支** | `cas` |
| **对标上游** | Linux 主线 sched/cache 基础设施 (Tim Chen, Peter Zijlstra, commit `6269a532` in linux-next) |
| **时间跨度** | 2026-05-22 ~ 2026-05-29 |
| **提交数** | 10 个 (6 个步骤提交 + 1 个 update + 1 个 testcase + 2 个 fix) |
| **Co-Authored-By** | Claude Sonnet 4.6 / Claude Opus 4.7 `<noreply@anthropic.com>` |

### 补丁时间线 (按提交顺序)

| # | Commit | 日期 | 标题 | 类型 |
|---|--------|------|------|------|
| 1 | `17c7e0fc` | 05-22 17:13 | step 1 – per-process LLC domain tracking infrastructure | Feature |
| 2 | `016c6dab` | 05-22 17:33 | step 2 – per-process epoch-decay LLC domain tracking | Feature |
| 3 | `c46adf47` | 05-22 17:33 | step 3 – eligibility gate for LLC domain tracking | Feature |
| 4 | `b80a9687` | 05-22 17:37 | step 4 – wakeup path bias toward preferred LLC domain | Refactor |
| 5 | `ba9401d2` | 05-27 14:11 | step 5 – steal resistance for preferred LLC domain | Feature |
| 6 | `839600cd` | 05-28 11:01 | add testcase | Test |
| 7 | `63ffdd46` | 05-28 14:37 | update (重构 + 日志) | Refactor |
| 8 | `f003d72a` | 05-28 19:39 | step 6 – hysteresis around preferred LLC util | Feature/Bugfix |
| 9 | `923ba01c` | 05-29 10:53 | fix (BPF verifier 调用深度) | Bugfix |
| 10 | `f8d4a838` | 05-29 16:11 | fix (测试脚本解析) | Bugfix |

## 2. 分类结果

- **整体类型**: **Feature** (新增功能：缓存感知的负载均衡)
- **置信度**: High
- **判定依据**: 该补丁组引入了一个全新的 `--cache-aware` 调度策略，新增数据结构 (`mm_ca_stat`)、BPF map (`mm_ca_map`)、CLI 选项，并修改了选核 (`pick_idle_cpu`)、跨域迁移 (`migrate_to_neighbor`)、任务窃取 (`try_to_steal_task`) 三条关键路径。末尾两个 "fix" 提交修复的是本特性开发过程中引入的问题（BPF 验证器调用深度溢出、测试脚本解析错误），而非修复预先存在的内核 bug，因此整体仍归为 Feature。

## 3. 子系统与受影响模块

- **子系统**: sched-ext / scx_lavd
- **CONFIG 依赖**: `CONFIG_SCHED_CLASS_EXT` (内核侧)；Rust 用户态调度器 `scx_lavd`
- **受影响文件**:

| 文件路径 | 变更类型 | 说明 |
|---------|---------|------|
| `scheds/rust/scx_lavd/src/bpf/intf.h` | 修改 | 新增 `LAVD_CA_*` 常量枚举 |
| `scheds/rust/scx_lavd/src/bpf/lavd.bpf.h` | 修改 | 新增 `struct mm_ca_stat`、`task_ctx.preferred_cpdom_id`、`cpdom_util_above()` |
| `scheds/rust/scx_lavd/src/bpf/main.bpf.c` | 修改 | 新增 `mm_ca_map`、`update_preferred_cpdom()`、exit_task 清理 |
| `scheds/rust/scx_lavd/src/bpf/util.bpf.c` | 修改 | 新增 `is_cache_aware_eligible()`、rodata 变量 |
| `scheds/rust/scx_lavd/src/bpf/util.bpf.h` | 修改 | `is_cache_aware_eligible()` 声明 |
| `scheds/rust/scx_lavd/src/bpf/idle.bpf.c` | 修改 | `pick_idle_cpu` / `migrate_to_neighbor` 注入缓存感知逻辑 |
| `scheds/rust/scx_lavd/src/bpf/balance.bpf.c` | 修改 | `try_to_steal_task` 重构为 `bpf_loop` + `steal_wanderer` |
| `scheds/rust/scx_lavd/src/main.rs` | 修改 | `--cache-aware` / `--cache-aware-max-threads` CLI |
| `scheds/rust/scx_lavd/LOGGING_CHANGES.md` | 新增 | 调试日志说明 |
| `tools/cache_stress/*` | 新增 | 压测工具与自动化脚本 |

## 4. 整体设计目标与上游对标

该补丁组在 scx_lavd 中复刻 Linux 主线 **sched/cache** 基础设施（Tim Chen、Peter Zijlstra）的核心思想：跟踪每个进程"最热"的 LLC（Last-Level Cache）域，并在选核/唤醒/窃取路径上偏向该域，以提升缓存命中率、降低跨域迁移。

### 4.1 与上游的关键差异

| 维度 | 上游 sched/cache | scx_lavd 实现 |
|------|-----------------|--------------|
| **跟踪粒度** | per-mm (`mm->sc_stat`) | per-mm (`mm_ca_map` keyed by `p->mm`) — 一致 |
| **epoch 衰减** | `__update_mm_sched` 异步 task_work 扫描 | 在 `stopping` 路径内联更新，**无需异步扫描**（因 scx_lavd 的 cpdom 已是 LLC 级） |
| **单线程进程** | 排除（目标为线程间共享） | **不排除**（目标更广：任何任务都尽量留在热 LLC） |
| **域切换条件** | `if (m_a_occ > 2 * curr_m_a_occ) switch` | 2x 滞回切换 — 一致 |
| **wakeup 偏向** | wake_affine 拉向热 LLC | `pick_idle_cpu` 注入 preferred 域优先选 idle core |
| **窃取抵抗** | `can_migrate_llc_task()` per-task 过滤 | `steal_wanderer()` DSQ 扫描 + 概率跳过 |

> 引自 step 1 commit message：
> > "Unlike the previous per-task EWMA approach, this implementation tracks preferred LLC domain at per-process (per-mm) granularity so that all threads of the same process share a single preferred domain. This matches the upstream design where `mm->sc_stat` is the authoritative state shared across threads."

## 5. 各步骤详细分析

### Step 1 (`17c7e0fc`): 基础设施

**作用**：引入数据结构与 CLI 开关，不做任何行为变更（`--cache-aware` 默认关闭）。

**关键改动**：

1. `intf.h` 新增常量（最终态）：
```c
enum {
    LAVD_CA_EPOCH_NS    = 10000000, /* 10 ms per epoch (== EPOCH_PERIOD) */
    LAVD_CA_UNSET_CPDOM = 0xFF,     /* preferred_cpdom_id sentinel */
    LAVD_CA_MAX_CPDOMS  = 16,       /* max LLC domains tracked per process */
};
```

2. `lavd.bpf.h` 新增 per-process 状态结构：
```c
struct mm_ca_stat {
    struct bpf_spin_lock    lock;
    u8  preferred_cpdom_id;
    u8  __pad[3];
    u32 cpdom_runtime[LAVD_CA_MAX_CPDOMS]; /* per-LLC decayed runtime (ns >> 10) */
    u64 last_epoch_ns;
};
```
   - 使用 `bpf_spin_lock` 保护并发线程更新
   - per-LLC runtime 数组独立累积，支持 >2 个 LLC 域的正确迁移

3. `lavd.bpf.h` 在 `task_ctx` 新增读缓存字段：
```c
u8 preferred_cpdom_id; /* LAVD_CA_UNSET_CPDOM if not yet determined */
```
   - 在 `stopping` 路径写入，在 `pick_idle_cpu` 唤醒路径零开销读取（避免 map lookup）

4. `main.bpf.c` 新增 BPF map：
```c
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, u64);              /* mm_struct pointer */
    __type(value, struct mm_ca_stat);
} mm_ca_map SEC(".maps");
```

5. `init_task` 中重置 `preferred_cpdom_id = LAVD_CA_UNSET_CPDOM`，避免子进程继承父进程的陈旧值（exec 后新 mm 的场景）。

6. `main.rs` 新增 CLI：`--cache-aware`（默认 off）、`--cache-aware-max-threads`（默认 16）。

### Step 2 (`016c6dab`): 核心 epoch-decay 算法

**作用**：实现 `update_preferred_cpdom()`，在 `stopping` 路径按 epoch 几何衰减、累积、滞回切换。

**算法四步**（`main.bpf.c`）：
```c
/* Step 1: advance epochs, decay all per-LLC runtime counters. */
n = (u32)((now - mcs->last_epoch_ns) / LAVD_CA_EPOCH_NS);
if (n > 31) n = 31;                 /* 限制移位量，防溢出 */
for (i = 0; i < LAVD_CA_MAX_CPDOMS; i++)
    mcs->cpdom_runtime[i] >>= n;    /* r = 0.5 per epoch */

/* Step 2: accumulate this slice into the current LLC's counter. */
delta = (u32)(run_ns >> 10);
mcs->cpdom_runtime[cur_cpdom] = min(..., (u32)U32_MAX);

/* Step 3: find the LLC with highest accumulated runtime. */
for (i = 0; i < LAVD_CA_MAX_CPDOMS; i++)
    if (mcs->cpdom_runtime[i] > best_runtime) { ... }

/* Step 4: 2x hysteresis — switch only if best beats current preferred 2x. */
if (best_runtime > 2 * pref_runtime)
    mcs->preferred_cpdom_id = best_cpdom;
```

**设计要点**：
- **独立累积所有 LLC**：确保跨 >2 个 LLC 域迁移时，真正的最热域能胜出，而非单一"挑战者"被每次新迁移驱逐。
- **2x 滞回**：对齐上游 `if (m_a_occ > 2 * curr_m_a_occ) switch` 条件，避免频繁抖动。
- **ns >> 10**：将 ns 量级压缩到 u32 范围，节省 map 空间。
- **回写 task_ctx**：`taskc->preferred_cpdom_id = mcs->preferred_cpdom_id;` 让 `pick_idle_cpu()` 零开销读取。

**生命周期清理**（`lavd_exit_task`）：
```c
if (!is_kernel_task(p) &&
    BPF_CORE_READ(p, signal, nr_threads) <= 1) {
    u64 mm_key = (u64)BPF_CORE_READ(p, mm);
    if (mm_key)
        bpf_map_delete_elem(&mm_ca_map, &mm_key);
}
```
   - 仅当最后一个线程退出时删除 map 条目，避免内存泄漏。

### Step 3 (`c46adf47`): 资格门控 + 选核注入

**作用**：定义哪些任务参与缓存感知；在 `idle.bpf.c` 两条路径注入 preferred 域偏向。

**资格判定** `is_cache_aware_eligible()`：
```c
if (!cache_aware)              return false;  /* 总开关 */
if (is_kernel_task(p))        return false;  /* 无 mm，无数据局部性 */
if (is_pinned(p))             return false;  /* 单 CPU 亲和，无放置自由度 */
if (nr_threads > max_threads) return false;  /* 防过度聚合 */
return true;
```

> 关键设计差异（引自 commit message）：
> > "Single-threaded processes are intentionally NOT excluded. The upstream sched/cache implementation targets inter-thread sharing and therefore invalidates `sc_stat.cpu` for `nr_threads <= 1`. scx_lavd's goal is broader: keep any task – including single-threaded ones – on its warm LLC, so the exclusion logic is inverted at the low end."

**注入点 1**：`migrate_to_neighbor`（跨域捐赠）— 在距离遍历前先尝试 preferred 域：
```c
if (ctx->taskc) {
    u8 pref = ctx->taskc->preferred_cpdom_id;
    if (cache_aware && pref != LAVD_CA_UNSET_CPDOM && (u64)pref != cpdc->id) {
        mig_cpdc = MEMBER_VPTR(cpdom_ctxs, [pref]);
        if (mig_cpdc && READ_ONCE(mig_cpdc->is_stealer)) {
            cpu = pick_idle_cpu_at_cpdom(ctx, (s64)pref, scope, is_idle);
            if (cpu >= 0) { *sticky_cpdom = (s64)pref; return cpu; }
        }
    }
}
```

**注入点 2**：`pick_idle_cpu`（唤醒选核）— 在 sticky 域无 idle core 后，优先尝试 preferred 域的 idle core：
```c
if (cache_aware && ctx->taskc) {
    u8 pref = ctx->taskc->preferred_cpdom_id;
    if (pref != LAVD_CA_UNSET_CPDOM &&
        (s64)pref != sticky_cpdom &&
        can_run_on_domain(ctx, (s64)pref)) {
        cpu = pick_idle_cpu_at_cpdom(ctx, (s64)pref,
                                     SCX_PICK_IDLE_CORE, is_idle);
        if (cpu >= 0) { sticky_cpdom = pref; goto unlock_out; }
    }
}
```
   - 仅尝试 **fully-idle core**（`SCX_PICK_IDLE_CORE`），无匹配则静默 fall through，对非缓存感知任务无回退。

### Step 4 (`b80a9687`): 代码组织（实为 Refactor）

**作用**：将 `is_cache_aware_eligible()` 从 `main.bpf.c` 移至 `util.bpf.c` 并在 `util.bpf.h` 声明，使 `main.bpf.c` 与 `idle.bpf.c` 共享同一份实现，消除重复。

**注意**：commit message 标题为 "wakeup path bias"，但实际的 `pick_idle_cpu` 注入在第 3 步已完成；本提交主要是去重与函数迁移，类型上更接近 Refactor。仅有的非空格文本变更是 `pick_idle_cpu` 中两处注释的引号风格统一（`let's` → `let's`）。

### Step 5 (`ba9401d2`): 窃取抵抗

**作用**：防止任务在唤醒时被放到热 LLC 后，立即被邻居域窃走。

**新增常量**：
```c
LAVD_CA_IMB_PCT             = 20,  /* 相对不平衡阈值 */
LAVD_CA_STEAL_SEARCH_DEPTH  = 16,  /* DSQ 扫描深度 */
```

**`try_to_steal_task` 改造**：在原 head-of-DSQ consume 前插入缓存感知扫描：
1. 用 `bpf_iter_scx_dsq` 遍历邻居 DSQ 前 16 个任务
2. 找到第一个 "wanderer"（`preferred_cpdom_id != source 域`），通过 `scx_bpf_dsq_move()` 直接搬走，**绕过排在前面的 home 任务**
3. 若窗口内全为 home 任务，应用负载门控概率跳过：源域非严重过载时给 home 任务 50% 留存概率

**`severely_imbalanced()`**（本步引入，step 6 移除）：相对 per-capacity 利用率比较：
```c
return src->load_invr * dst_cap * 100 >
       dst->load_invr * src_cap * (100 + LAVD_CA_IMB_PCT);
```

> 对标上游：`can_migrate_llc_task()` (commit `53da65f3d59d`) 的 per-task LLC 过滤。

**work-conservation 优先级**：`force_to_steal_task` 不变 — 本地域空闲时，工作 conservation 优先于缓存局部性。

### "update" (`63ffdd46`): 重构 + 日志

**作用**：解决 BPF 验证器状态空间问题；新增调试日志文档。

1. 抽取 `steal_wanderer()` 为独立 `noinline` 函数，将 `bpf_iter` + `bpf_for` 循环隔离分析，避免内联进 `try_to_steal_task()` 复杂状态空间。
2. `severely_imbalanced()`、`try_to_steal_task()` 标记 `__attribute__((noinline))`。
3. 新增 `LOGGING_CHANGES.md`，记录 `plan_x_cpdom_migration` 等处的 `debugln` 日志点。

### Step 6 (`f003d72a`): 滞回死区（关键改进）

**问题**：step 5 的 `severely_imbalanced()` 是**相对**比较（src vs dst），导致不对称：
- `try_to_steal_task` 可在 >20% 不平衡时把任务拉离 preferred LLC
- 但 `pick_idle_cpu` / `migrate_to_neighbor` **无负载门控**，会在下次唤醒时只要 preferred 有 idle core 就迁回 → **ping-pong 循环**

**方案**：改用**绝对**自利用率指标 + 双阈值死区，使窃出与拉回在同一量上反向运动：

```c
LAVD_CA_UTIL_HI = 50;  /* preferred LLC 自利用 >50% 时允许窃出 */
LAVD_CA_UTIL_LO = 40;  /* preferred LLC 自利用 <40% 时允许拉回 (HI*0.8) */
```

`cpdom_util_above()` 复用 `plan_x_cpdom_migration` 已计算的 `avg_util_wall_sum / nr_active_cpus`：
```c
static __always_inline bool
cpdom_util_above(struct cpdom_ctx *cpdc, u32 pct)
{
    u32 acpus = cpdc->nr_active_cpus;
    if (!acpus) return false;
    return (u64)cpdc->avg_util_wall_sum > (u64)pct * acpus;
}
```

**死区行为**（preferred LLC 自利用）：
| 区间 | 窃出 | 拉回 | 结果 |
|------|------|------|------|
| < 40% | 不触发 | 允许 | 返回 preferred |
| 40–50% | 不触发 | 拒绝 | 原地稳定 |
| > 50% | 触发 | 拒绝 | 留在外面 |

`severely_imbalanced()` 与 `LAVD_CA_IMB_PCT` 被移除。三处调用点统一改用 `cpdom_util_above(..., LAVD_CA_UTIL_HI/LO)`。

> 设计意图（引自 commit message）：
> > "Aligns with the 'aggregate while capacity allows, disperse only when preferred is actually busy' intent rather than a relative balance test."

### fix (`923ba01c`): BPF 验证器调用深度溢出

**问题**：嵌套 `bpf_loop` + `steal_wanderer` + `__get_task_ctx_slowpath` + `scx_arena_subprog_init` 突破 BPF 8 帧调用深度限制。

**方案**：将两层嵌套 `bpf_loop`（外层 distance、内层 neighbor）拍平为单层 `bpf_loop` + `try_steal_flat_cb` 回调，节省一帧：
```
lavd_dispatch(0) → consume_task(1) → try_to_steal_task(2)
→ try_steal_flat_cb(3) → steal_wanderer(4)
→ __get_task_ctx_slowpath(5) → scx_task_data(6)
→ scx_arena_subprog_init(7)          ← 8 frames, at limit
```

回调内用 `i = idx / LAVD_CPDOM_MAX_NR; j = idx % LAVD_CPDOM_MAX_NR;` 分解（128 是 2 的幂，编译器优化为 shift/mask）。`steal_wanderer` 内改用普通 `for` 循环替代 `bpf_for`。

**BPF 验证器技巧文档化**（commit message 中明确记录 5 条规则，如 Rule 4: `bpf_loop` 不约束 idx 需显式 bound check；Rule 5: `bpf_loop` 会丢失 map-value 指针类型需 `MEMBER_VPTR` 重新派生）。

### fix (`f8d4a838`): 测试脚本解析修正

- 增加 `context-switches,cpu-migrations` perf 事件
- 修复 X-MIG% 解析：先 `sed` 去 ANSI 颜色码，再用 `awk -F'|'` 取第 9 列
- 修复 ops/s 解析：`awk '{print $8}'` 而非 `$NF`

### testcase (`839600cd`): cache_stress 工具

- `cache_stress.c`：多线程共享内存 RMW 压测，~4MB 数组（典型 LLC 大小），两组线程分别打前/后半，构造两个受益于同 LLC 的 working set
- `run_test.sh`：自动跑 baseline vs `--cache-aware`，对比 X-MIG%、L3 hit rate、吞吐、ctx-sw、cpu-mig；AMD Zen 优先用 `amd_l3` PMU，否则回退通用 `cache-misses`

## 6. 核心算法总览（最终态）

```
┌───────────────────────────── stopping path ─────────────────────────────┐
│ update_stat_for_stopping()                                             │
│   └─ if is_cache_aware_eligible(p):                                     │
│        update_preferred_cpdom(p, taskc, cpuc, run_ns)                   │
│          ├─ mm_key = p->mm (per-process)                                │
│          ├─ epoch decay: cpdom_runtime[16] >>= n  (r=0.5/10ms)         │
│          ├─ accumulate: cpdom_runtime[cur] += run_ns>>10               │
│          ├─ find max → 2x hysteresis switch                            │
│          └─ writeback taskc->preferred_cpdom_id (read cache)            │
└────────────────────────────────────────────────────────────────────────┘

┌──────────────────────── wakeup path ───────────────────────────────────┐
│ pick_idle_cpu()                                                         │
│   └─ if cache_aware && pref != UNSET && pref != sticky:                 │
│        if !cpdom_util_above(pref, LAVD_CA_UTIL_LO=40):                  │
│          try pick_idle_cpu_at_cpdom(pref, SCX_PICK_IDLE_CORE)           │
│          → hit: sticky = pref; return                                  │
└────────────────────────────────────────────────────────────────────────┘

┌──────────────────────── steal path ───────────────────────────────────┐
│ try_to_steal_task()  →  bpf_loop → try_steal_flat_cb()                │
│   └─ if cache_aware:                                                   │
│        steal_wanderer(dsq_id, cpdomc, cpdomc_pick)                      │
│          ├─ scan 16 tasks for "wanderer" (pref != source)              │
│          │   → move via scx_bpf_dsq_move, bypass home tasks             │
│          └─ all home: if !cpdom_util_above(src, UTIL_HI=50) && 50%      │
│                       probability → skip (resist steal)                 │
└────────────────────────────────────────────────────────────────────────┘
```

## 7. 关键设计评估

### 7.1 优点

1. **对齐上游设计**：per-mm 粒度、epoch decay、2x hysteresis 均与主线 sched/cache 一致，便于后续主线演进同步。
2. **读路径零开销**：通过 `task_ctx.preferred_cpdom_id` 读缓存，`pick_idle_cpu` 唤醒热路径无 map lookup。
3. **死区滞回（step 6）**：用绝对自利用 + HI/LO 双阈值消除 step 5 引入的 ping-pong，是本组补丁最关键的修正。
4. **work-conservation 不破坏**：`force_to_steal_task` 不加缓存感知门控；wanderer 找不到时仍 fall through 到 head consume。
5. **eligibility gate 合理**：排除 kernel thread、pinned task、超线程进程，避免无效追踪与过度聚合。
6. **BPF 验证器友好**：`noinline` 隔离复杂循环；拍平 `bpf_loop` 控制调用深度；显式 idx bound check 满足验证器约束。
7. **可配置**：`--cache-aware` 默认 off，`--cache-aware-max-threads` 可调，向后兼容。

### 7.2 潜在风险与改进点

1. **`mm_ca_map` 容量固定 4096**：长生命周期进程多时可能哈希冲突。当前 exit_task 清理依赖 `nr_threads <= 1`，若进程异常退出（被 kill）未走 `lavd_exit_task`，存在条目泄漏风险。
2. **`preferred_cpdom_id` 读缓存一致性**：`taskc->preferred_cpdom_id` 在 `stopping` 写、`pick_idle_cpu` 读，跨 CPU 唤醒场景下可能读到稍旧值（非强同步）。但因为是偏向性 hint 而非正确性约束，影响可接受。
3. **`bpf_spin_lock` 粒度**：`mm_ca_stat.lock` 是 per-process 锁，同进程多线程同时 stopping 会串行。对高并发线程数进程（虽然被 max_threads gate 排除，但 16 线程仍可能竞争）有一定开销。
4. **`steal_wanderer` 扫描深度固定 16**：DSQ 长度 > 16 时可能漏掉 wanderer。但 16 已覆盖典型场景，且更深扫描会增加 BPF 验证器复杂度。
5. **常量均为编译期固定**：`UTIL_HI=50`、`UTIL_LO=40`、`EPOCH_NS=10ms` 不可运行时调，调优需重编 BPF。
6. **Step 4 标题与内容不符**：commit message 称 "wakeup path bias"，但实际注入在 step 3 完成，step 4 仅做函数迁移。建议提交时拆分或更正描述。
7. **测试覆盖**：`cache_stress` 仅覆盖共享内存 RMW 单一场景；缺少 NUMA 跨域、异构核心、超线程进程排除等场景的回归测试。
8. **无公开基准数据**：commit message 未附 before/after 性能数据（X-MIG%、L3 hit rate、吞吐），仅 `run_test.sh` 提供自动化采集框架。

## 8. 测试建议

### 8.1 测试策略（Feature 型）

| 维度 | 重点 |
|------|------|
| **功能正确性** | preferred 域收敛、2x 滞回切换、死区行为 |
| **回归** | `--cache-aware` 关闭时行为与基线一致 |
| **性能** | X-MIG%↓、L3 hit rate↑、吞吐↑、ctx-sw↓ |
| **并发** | 多线程同进程 stopping 竞争 `bpf_spin_lock` |
| **边界** | 进程退出 map 清理、wanderer 深度溢出、超线程进程排除 |

### 8.2 推荐测试用例

1. **cache_stress 基线对比**（已有 `run_test.sh`）
   - 步骤：`sudo ./run_test.sh -d 30 -t 8 -s 4`
   - 通过标准：`--cache-aware` 开启后 X-MIG% 与 cpu-mig 显著下降，L3 hit rate 上升，吞吐不退化。

2. **多 LLC 域 wanderer 验证**
   - 目的：验证 `steal_wanderer` 能正确识别并迁移非 home 任务
   - 步骤：在 ≥2 LLC 域机器上，绑组绑域跑两进程，观察 `bpftool map dump mm_ca_map` 中 `preferred_cpdom_id` 收敛
   - 通过标准：preferred 域与实际热域一致，跨域迁移率下降

3. **死区滞回验证**
   - 目的：验证 step 6 的 40-50% 死区不产生 ping-pong
   - 步骤：构造负载使 preferred LLC 自利用在 40-50% 区间，观察迁移频率
   - 通过标准：死区内任务原地稳定，无周期性来回迁移

4. **退出清理验证**
   - 目的：验证 `mm_ca_map` 条目在进程退出后释放
   - 步骤：循环启动/退出短生命周期进程，观察 `bpftool map lookup` 条目数
   - 通过标准：条目数不单调增长

### 8.3 静态检查

```bash
# checkpatch (若项目带内核 scripts)
# BPF 验证器日志
sudo bpftool prog profile name lavd_... --duration 5
# map 状态
sudo bpftool map dump name mm_ca_map
```

## 9. Backport / 部署评估

- **目标版本**: scx 仓库 `cas` 分支当前态；要求内核 ≥ 6.12（sched-ext 主线化版本）
- **API 兼容性**: 依赖 `bpf_iter_scx_dsq`、`scx_bpf_dsq_move`、`bpf_loop` 等 sched-ext BPF helper，需较新 libbpf 与内核
- **依赖补丁**: 无额外主线内核补丁依赖（对标但未直接依赖上游 sched/cache）
- **风险等级**: Medium（生产部署前需验证多 LLC 拓扑下的稳定性与性能收益）
- **建议**：
  - 默认 off，按场景灰度开启
  - 大型多 LLC 服务器（AMD Zen CCD / 多 socket NUMA）优先试点
  - 单 LLC 桌面/移动端无明显收益，可不开

## 10. 总结

| 维度 | 评分 (1-5) | 说明 |
|-----|-----------|------|
| **算法完整性** | ⭐⭐⭐⭐⭐ | 覆盖 stopping→wakeup→steal 三条路径，6 步递进式落地 |
| **上游对标度** | ⭐⭐⭐⭐ | per-mm 粒度、epoch decay、2x hysteresis 对齐 sched/cache |
| **工程健壮性** | ⭐⭐⭐⭐ | 死区滞回、BPF 验证器调用深度处理、noinline 隔离均考虑到位 |
| **可维护性** | ⭐⭐⭐ | 常量不可调、step 4 标题与内容不符、缺公开基准数据 |
| **测试完备性** | ⭐⭐⭐ | 有 cache_stress + run_test.sh，但场景覆盖单一 |

**整体评价**：这是一组设计严谨、对标主线 sched/cache 的 Feature 实现。step 6 的死区滞回修正与 `923ba01c` 的 BPF 调用深度修复是两组关键的工程性修正，体现了"先实现→发现问题→修正"的迭代过程。核心创新在于：(1) per-mm 粒度但内联更新（无异步扫描）；(2) 单线程进程不排除（比上游目标更广）；(3) 绝对自利用死区替代相对不平衡比较。建议补充公开基准数据与更全面的回归测试后再考虑生产默认开启。
