# 多 Cache 亲核调度方案

## 1. 背景与问题分析

### 1.1 众核趋势下的负载均衡挑战

随着数据中心 CPU 向高密度演进，单 Socket 512 核以上的众核服务器正逐步成为主流。在这种规模下，Linux 内核现有的 CFS（Completely Fair Scheduler）负载均衡机制面临严峻挑战，主要体现在两个维度：

**（1）负载均衡开销过大，影响调度延迟**

CFS 在每个调度域（sched domain）上周期性执行 `load_balance()`，统计各 CPU 的运行队列负载并进行任务迁移。在 512C+ 的众核场景下，这一过程带来显著的系统开销：

- **遍历开销**：负载均衡需要遍历调度域内所有 CPU 的运行队列，CPU 数量线性增长导致遍历时间同步增长。在 512C 以上的系统中，单次 `load_balance` 遍历数百个 CPU 的 `cfs_rq` 和任务统计信息的开销不容忽视。
- **关中断影响**：`load_balance` 路径中频繁调用 `raw_spin_lock_irqsave` 持有 rq lock 并关闭本地中断（IRQ disable），中断被长时间屏蔽会导致调度延迟抖动增大，对延迟敏感型业务（如网络数据面、存储 IO 路径）产生直接影响。
- **锁竞争**：多个 CPU 同时尝试拉取任务时，rq lock 竞争加剧，尤其是在高负载场景下，锁竞争成为可扩展性瓶颈。`find_busiest_group` / `find_busiest_cpu` 遍历过程中需要读取多个 CPU 的状态，多读少写的共享数据在高核数下 cache line bouncing 问题突出。

**（2）负载均衡算法不完全适配新 CPU 架构**

Linux CFS 负载均衡算法的核心设计基于传统 SMP/NUMA 架构的假设，与当前众核 Chiplet 架构存在结构性不匹配：

- **启发式算法泛化能力有限**：`load_balance` 涉及大量启发式参数（如 `imbalance_pct`、`cache_nice_tries`、`nr_balance_failed` 等），这些参数在 512C+ 的极端规模下缺乏充分的调优指导。`update_sd_lb_stats` 基于 CPU 负载（load avg）的统计推断，在核心数暴增时统计稳定性和决策准确性下降。
- **NUMA 抽象粒度不足**：CFS 将硬件拓扑抽象为调度域层级（SMT → MC → DIE → NUMA），但对于多 Chiplet 架构中 Chiplet 内部共享 L3 Cache、Chiplet 间通过 inter-connect 访问远端 Cache 的层级关系，现有调度域的粒度无法精确表达，导致任务迁移决策缺乏 Cache 拓扑感知能力。
- **不均衡与过度迁移的权衡困难**：在 512C+ 场景下，"拉平负载"与"保持 Cache 亲和"之间的矛盾更加突出。当前 `can_migrate_task` 等判断逻辑缺乏对 Cache 局部性的量化考虑，容易导致频繁的跨 Chiplet 任务迁移，短期内看似负载均衡，实际却造成 Cache 颠簸和性能劣化。

### 1.2 多 Chiplet Cache 架构的访问问题

现代高密度 CPU（如 AMD EPYC、Intel Xeon 多 Chiplet 架构）采用多 Chiplet 封装设计，每个 Chiplet 拥有独立的 L3 Cache（LLC），Chiplet 之间通过片上互联网络（如 AMD Infinity Fabric、Intel EMIB/UCIe）通信。这种架构引入了非均匀 Cache 访问（NUCA, Non-Uniform Cache Access）问题：

**（1）跨 Chiplet Cache 访问延迟显著增大**

- 本地 Chiplet 内 L3 Cache 访问延迟约为 XX ns（平台相关），跨 Chiplet 访问远端 L3 Cache 的延迟显著增加，实测跨 Cache 访问性能劣化约 **20%**。
- 当任务在 Chiplet 间迁移后，其原本位于源 Chiplet L3 Cache 中的热数据变成远端访问，导致平均访存延迟升高，IPC 下降。

**（2）Cache 亲和性丢失与颠簸**

- **迁移导致的 Cache 预热开销**：任务迁移到新 Chiplet 后，其工作集需要在新 Chiplet 的 L3 Cache 中重新建立，这段时间内 Cache 命中率骤降，触发大量远端 Cache 访问和内存访问。
- **频繁迁移加剧问题**：如果调度器缺乏 Cache 感知能力，可能在多个 Chiplet 间反复迁移同一任务，形成"颠簸"效应 —— 任务永远无法享受本地 L3 Cache 的收益。
- **共享数据竞争**：多个任务如果访问共享数据结构，将它们分散在不同 Chiplet 上会导致跨 Chiplet 的 Cache 一致性协议开销（cache snoop / directory lookup），进一步放大延迟。

**（3）众核 + Chiplet 架构带来的复合挑战**

512C+ 的规模放大了上述问题：更多的核心意味着更多的 Chiplet、更复杂的互联拓扑、更大的负载均衡遍历开销，以及更频繁的跨 Chiplet 任务迁移机会。传统的 "先解决负载均衡、再考虑亲和性" 的思路在这一架构下已经难以为继，需要一套系统性的调度方案，从调度域划分、负载均衡策略、唤醒路径等多个维度进行联合优化。

**（4）多 LLC 服务器拓扑示例**

以典型的多 LLC 服务器为例（如 AMD EPYC 多 CCD、Intel 多 tile/SNC 模式），每个 NUMA node 内可能有多个独立 LLC domain：

```
PKG
├── NUMA0
│   ├── LLC0 (CPU 0-15)     ← Chiplet 0
│   └── LLC1 (CPU 16-31)    ← Chiplet 1
└── NUMA1
    ├── LLC2 (CPU 32-47)    ← Chiplet 2
    └── LLC3 (CPU 48-63)    ← Chiplet 3
```

在此拓扑下，LLC0 与 LLC1 虽不共享 L3 Cache，但同属一个 NUMA node，共享内存控制器与互联带宽，存在部分数据局部性（远端 LLC 访问延迟远低于跨 NUMA 访问）。当前调度器将"不共享 LLC"与"跨 NUMA"同等对待，未利用这一中间层次的局部性。

---

## 2. 方案设计与实现

### 2.1 分域调度

#### 1）实现思路

olk-6.6 的 `CONFIG_SCHED_SOFT_DOMAIN` 走的是**在 task_group 上挂载独立"软域"上下文**的第三条路：既不修改 sched_domain 拓扑树，也不依赖 cpuset 硬隔离，而是在 wakeup 与负载均衡热路径对 `task_group(p)->sf_ctx` 做 cache-aware 偏置。

- **软域结构**：每个 NUMA 节点构建一个 `soft_domain`，节点内按 `topology_cluster_cpumask` 划分多个 `soft_subdomain`（对应 cluster/Chiplet 抽象）；每个 task_group 持有 `soft_domain_ctx`，记录绑定策略、期望核数与候选 CPU span。
- **拓扑构建**：在 `sched_init` 完成后遍历所有 NUMA 节点构建软域；用户通过 cgroup 接口把指定 tg 绑定到 LLC。
- **分配算法**：先按空闲 CPU 数/util 选 idlest LLC，再在该 LLC 内按"最少被认领优先 + 最低 util"排序选 subdomain，greedy 累加直到满足期望核数；配额可显式设置或自动从 CFS bandwidth quota 推算。
- **用户接口**：cgroup `cpu.soft_domain`（-1 自动 / 正数绑指定 NUMA / 0 unset）+ `cpu.soft_domain_nr_cpu` 期望核数 + `cpu.soft_domain_cpu_list` 只读 span；运行时总开关 `sched_soft_domain=`、过载阈值 `soft_domain_overutil_pct`、sched_feat `SOFT_DOMAIN`。
- **热路径约束**：wakeup 路径在选 rq 前先把 target 拉回 span 内；`select_idle_sibling` 把扫描域从 `sd_llc` 收窄到 tg span；`load_balance` 在跨 NUMA 域直接禁迁，LLC 内跨 span 时若 src LLC 未过载则视为 cache-hot 拒绝拉出；per-LLC util 由 LB 统计阶段回写 `sd_llc_shared` 供决策使用，newidle 路径跳过该写入避免抖动。

#### 2）方案不足

- **cluster 粒度与 Chiplet/LLC 不一致**：x86 上 `topology_cluster_cpumask` 取自 L2 cache 域（`l2c_id`）而非 L3/Chiplet 域，subdomain 实际并未与背景描述的 L3/Chiplet 对齐，跨架构行为也不统一。**缓解**：引入 L3/Chiplet 级拓扑抽象让 subdomain 粒度对齐。
- **`attached` 是 tg 计数而非负载度量**：大 tg 与小 tg 在 subdomain 选址时同等加权，可能让大 tg 挤到同一 subdomain 局部过载。**缓解**：改为加权 util 比较或按 util/cap 比例排序。
- **`find_idlest_llc` 全局 O(NR_CPUS) 遍历**：512C+ 上每次 cgroup 设置累加数千次 `cpu_util_cfs`，容器平台批量创建时是开销点。**缓解**：复用 `sd_llc_shared` 已缓存的 LLC util，避免重复累加。
- **过载阈值全局固定**：`soft_domain_overutil_pct=85` 对所有 tg 一视同仁，但延迟敏感型与 batch 型 tg 需求差异大。**缓解**：改为 per-tg 属性或按优先级自动分级。
- **跨 NUMA 硬禁令与新 idle 路径冲突**：某 NUMA 极度空闲时 soft_domain tg 无法被 newidle 拉过去，造成 CPU 闲置；newidle 跳过 `sd_llc_shared` 写入但 `soft_domain_cache_hot` 仍读它，决策与统计脱节。**缓解**：NUMA 级 idle 比例超阈值时放开禁令；为 newidle 维护瞬时 idle 比例。
- **policy>0 仅按 NUMA 绑定忽略节点内多 LLC 均衡**：等于"绑 NUMA"而非"绑 LLC"，与 cache 亲和初衷偏离。**缓解**：升级为多级语义或新增 LLC 选择接口。
- **三层开关语义重叠**：Kconfig + 启动参数 + sched_feat 三层叠加且默认值不一致（Kconfig 默认 n、`__soft_domain_switch` 默认开、`SOFT_DOMAIN` 默认 false），"开 Kconfig 不等于生效"容易混淆。**缓解**：收敛为两层并统一默认。

### 2.2 cache aware load balance

#### 1）实现思路

mainline linux 7.1 的 `CONFIG_SCHED_CACHE`（"Cache aware load balance"，默认开启）采取**以进程为单位跟踪 LLC 偏好、在负载均衡路径把任务聚合回偏好 LLC** 的路径，与 2.1 的 task_group 级软域完全独立。整体由偏好跟踪、容量约束、均衡决策三层构成。

- **per-process LLC 偏好跟踪**：在进程地址空间结构上挂一个偏好统计结构，记录每个 CPU 的 runtime 累加、首选 CPU、epoch、滚动线程数、cache footprint 等；每个任务持有一个"偏好 LLC"标记；每个运行队列维护"本队列上有多少任务偏好本 LLC"的计数。
- **per-epoch 选择偏好 LLC**：以 10ms 为一个 epoch，tick 周期触发 task_work，比较该进程在各 CPU 上累计的 runtime，选出最常跑的那个 CPU，导出其所在 LLC 写到每个线程的"偏好 LLC"标记；50ms 内未触发扫描则把偏好清空，避免陈旧偏好影响决策。
- **与 NUMA balancing 协调**：当 NUMA balancing 给任务指定了偏好 NUMA 节点，且与进程偏好 CPU 不同节点时，把 LLC 偏好置空让步，避免 cache 偏好与 NUMA 迁移互相打架。
- **容量与 footprint 检查**：线程数过多（超过 LLC 核数 × 容差）或工作集超过 LLC 物理容量时，主动放弃聚合，避免"小 LLC 装不下大工作集"还硬凑上去。
- **LB 路径 4 处集成**：
  1. **per-LLC util 缓存**：负载均衡统计每个 sched_group 时，若该组跨多个 LLC 且非 newidle 路径，把组级 util/capacity 写到对应 LLC 的共享数据中，供后续决策复用；newidle 路径主动跳过写入避免抖动。
  2. **busiest group 选型**：当某组存在偏好目标 LLC 的任务且聚合迁移被判定为允许时，把该组标为"LLC 聚合均衡"类型，作为比"有富余容量"更高优先级的 busiest 候选。
  3. **imbalance 计算**：在"LLC 聚合均衡"类型下，迁移类型设为"LLC 任务迁移"、不均衡量设为 1，即一次只拉一个偏好任务，避免一次拉多个引发过聚合。
  4. **迁移决策**：迁移前先判断是否会破坏 LLC 局部性；若本次负载均衡的目的就是 LLC 聚合但被考察的任务不偏好目标 LLC，跳过；若决策矩阵判定禁止迁移，则标记"LLC pinned"且不增加迁移失败计数，避免触发更激进的迁移重试。
- **决策矩阵**：以源/目标 LLC 的 util 为输入，按"是否走向偏好 LLC"分两种情形——走向偏好 LLC 时若目标已超载且目标 util 比源高出 20% 以上，禁止；离开偏好 LLC 时若源还有空间或源 util 不比目标高 20% 以上，禁止；其余允许。两者都已超载则不限制，交回通用负载均衡。
- **运行时控制**：编译期 Kconfig 默认开启，运行时通过 debugfs 总开关与若干百分比参数（聚合容差、LLC 不均衡百分比、过聚合百分比、epoch 周期等）调控。

#### 2）方案不足

- **per-process 偏好粒度对多进程协作场景偏粗**：以进程为单位选 LLC，但数据中心常见多进程共享同一份 shmem/数据文件，各自独立选 LLC 后可能扎堆同一 LLC 形成跨进程聚合过载，反而互相驱逐。**缓解**：引入 per-inode/shmem 或 cgroup 级 LLC 偏好，让共享同一份数据的进程组协商。
- **epoch 10ms 与 50ms 超时在 512C+ 上偏短**：高核数下 LB 周期本身就要数十毫秒，LLC 偏好可能未到一次 LB 就过期导致频繁抖动；同时 tick 触发的 task_work 在 CPU 数极大时是显著的 per-cpu 开销。**缓解**：epoch 周期按 CPU 数或 sd 层级自适应，或仅在 LLC util 跨过阈值时才触发扫描。
- **footprint 容量检查依赖 NUMA balancing**：footprint 统计需要 NUMA balancing 开启，未启用 NUMA balancing 的低延迟部署实际拿不到 footprint，导致容量判断恒为"未超"，大工作集场景仍硬聚合。**缓解**：fallback 用 RSS 扫描或硬件采样（PEBS Load Latency）获取工作集，让容量检查不绑死 NUMA balancing。
- **聚合容差等参数全局统一**：不同延迟敏感度的任务对"是否聚合"的偏好不同，全局一个容差值对延迟敏感型业务过于激进。**缓解**：参数 per-tg 化或按优先级自动分级。
- **"LLC 聚合均衡"类型在 busiest 选型中可能压住真正的过载迁移**：该类型优先级高于"有富余容量"，当某组同时存在"偏好目标 LLC 的任务"与"真正过载任务"时，可能优先走聚合只拉一个偏好任务，把真正过载任务留到下次 LB。**缓解**：在与"过载/过利用"共存时降级聚合优先级，让过载先解决。
- **"LLC pinned" 不增加失败计数可能拖延问题**：被偏好挡住的迁移不计数确实避免抖动，但当源 LLC 真的过载且偏好方向一致时，可能长期不被迁移形成沉默饥饿。**缓解**：单独维护 LLC pinned 计数，超过阈值后强制走一次通用 LB。
- **newidle 路径跳过统计但仍读共享数据**：与 2.1 类似，newidle 不写 LLC util 缓存但决策矩阵仍读它，若该值长期未被 idle 路径更新则决策基于过期数据。**缓解**：允许 newidle 增量更新或为 newidle 维护独立的瞬时 util。
- **依赖 mm 与线程数判断，对 kthread/daemon 不友好**：内核线程没有 mm，常驻 daemon 的 mm 长期稳定但 footprint 巨大可能被容量检查直接踢出，这类负载实际很依赖 cache 亲和却得不到收益。**缓解**：为 kthread 引入 per-task 偏好（基于最近 N 次 wakeup CPU 推断），对 daemon 放宽 footprint 阈值或单独跟踪。

### 2.3 wake affine

#### 1）实现思路

mainline linux 7.1 的 wake 路径由 `wake_affine`（决定 waker 与 wakee 谁靠谁）与 `select_idle_sibling`（在 cache 域内找空闲 CPU）两段组成，由 sched_feat `WA_IDLE`/`WA_WEIGHT` 与 SIS_UTIL 等开关控制。目标是"在保持 cache 亲和的前提下尽快选到能跑的 CPU"。

唤醒路径的 CPU 选择入口是 `select_task_rq_fair()`，整体流程如下：

```
try_to_wake_up()
  → select_task_rq_fair()
      → [EAS路径] find_energy_efficient_cpu()  (仅 !overutilized 时)
      → [Affine路径] wake_affine()
      → [Fast path] select_idle_sibling()
      → [Slow path] sched_balance_find_dst_cpu() (仅 fork/exec)
```

**（1）入口判断与域匹配**

`select_task_rq_fair` 中首先计算 `want_affine`：

```c

static void record_wakee(struct task_struct *p)
{
	/*
	 * Only decay a single time; tasks that have less then 1 wakeup per
	 * jiffy will not have built up many flips.
	 */
	if (time_after(jiffies, current->wakee_flip_decay_ts + HZ)) {
		current->wakee_flips >>= 1;
		current->wakee_flip_decay_ts = jiffies;
	}

	if (current->last_wakee != p) {
		current->last_wakee = p;
		current->wakee_flips++;
	}
}

/*
 * Detect M:N waker/wakee relationships via a switching-frequency heuristic.
 *
 * A waker of many should wake a different task than the one last awakened
 * at a frequency roughly N times higher than one of its wakees.
 *
 * In order to determine whether we should let the load spread vs consolidating
 * to shared cache, we look for a minimum 'flip' frequency of llc_size in one
 * partner, and a factor of lls_size higher frequency in the other.
 *
 * With both conditions met, we can be relatively sure that the relationship is
 * non-monogamous, with partner count exceeding socket size.
 *
 * Waker/wakee being client/server, worker/dispatcher, interrupt source or
 * whatever is irrelevant, spread criteria is apparent partner count exceeds
 * socket size.
 */
static int wake_wide(struct task_struct *p)
{
	unsigned int master = current->wakee_flips;
	unsigned int slave = p->wakee_flips;

    //（4）SMT 干扰：唤醒任务数超过物理核数时的负收益
	int factor = __this_cpu_read(sd_llc_size);

	if (master < slave)
		swap(master, slave);
	if (slave < factor || master < slave * factor)
		return 0;
	return 1;
}


/*
 * The purpose of wake_affine() is to quickly determine on which CPU we can run
 * soonest. For the purpose of speed we only consider the waking and previous
 * CPU.
 *
 * wake_affine_idle() - only considers 'now', it check if the waking CPU is
 *			cache-affine and is (or	will be) idle.
 *
 * wake_affine_weight() - considers the weight to reflect the average
 *			  scheduling latency of the CPUs. This seems to work
 *			  for the overloaded case.
 */
static int
wake_affine_idle(int this_cpu, int prev_cpu, int sync)
{
	/*
	 * If this_cpu is idle, it implies the wakeup is from interrupt
	 * context. Only allow the move if cache is shared. Otherwise an
	 * interrupt intensive workload could force all tasks onto one
	 * node depending on the IO topology or IRQ affinity settings.
	 *
	 * If the prev_cpu is idle and cache affine then avoid a migration.
	 * There is no guarantee that the cache hot data from an interrupt
	 * is more important than cache hot data on the prev_cpu and from
	 * a cpufreq perspective, it's better to have higher utilisation
	 * on one CPU.
	 */
	if (available_idle_cpu(this_cpu) && cpus_share_cache(this_cpu, prev_cpu))
		return available_idle_cpu(prev_cpu) ? prev_cpu : this_cpu;

	if (sync) {
		struct rq *rq = cpu_rq(this_cpu);

		if ((rq->nr_running - cfs_h_nr_delayed(rq)) == 1)
			return this_cpu;
	}

	if (available_idle_cpu(prev_cpu))
		return prev_cpu;

	return nr_cpumask_bits;
}

static int
wake_affine_weight(struct sched_domain *sd, struct task_struct *p,
		   int this_cpu, int prev_cpu, int sync)
{
	s64 this_eff_load, prev_eff_load;
	unsigned long task_load;

    // 只考虑单个CPU的负载，没有考虑整个LLC的负载，导致负载很轻时，仍然可能无法及时wake affine

	this_eff_load = cpu_load(cpu_rq(this_cpu));

	if (sync) {
		unsigned long current_load = task_h_load(current);

		if (current_load > this_eff_load)
			return this_cpu;

		this_eff_load -= current_load;
	}

	task_load = task_h_load(p);

	this_eff_load += task_load;
	if (sched_feat(WA_BIAS))
		this_eff_load *= 100;
	this_eff_load *= capacity_of(prev_cpu);

	prev_eff_load = cpu_load(cpu_rq(prev_cpu));
	prev_eff_load -= task_load;
	if (sched_feat(WA_BIAS))
		prev_eff_load *= 100 + (sd->imbalance_pct - 100) / 2;
	prev_eff_load *= capacity_of(this_cpu);

	/*
	 * If sync, adjust the weight of prev_eff_load such that if
	 * prev_eff == this_eff that select_idle_sibling() will consider
	 * stacking the wakee on top of the waker if no other CPU is
	 * idle.
	 */
	if (sync)
		prev_eff_load += 1;

	return this_eff_load < prev_eff_load ? this_cpu : nr_cpumask_bits;
}

static int wake_affine(struct sched_domain *sd, struct task_struct *p,
		       int this_cpu, int prev_cpu, int sync)
{
	int target = nr_cpumask_bits;

    // wake_affine 只比较两个 CPU，缺乏第三候选

	if (sched_feat(WA_IDLE))
		target = wake_affine_idle(this_cpu, prev_cpu, sync);

	if (sched_feat(WA_WEIGHT) && target == nr_cpumask_bits)
		target = wake_affine_weight(sd, p, this_cpu, prev_cpu, sync);

	schedstat_inc(p->stats.nr_wakeups_affine_attempts);
	if (target != this_cpu)
		return prev_cpu;

	schedstat_inc(sd->ttwu_move_affine);
	schedstat_inc(p->stats.nr_wakeups_affine);
	return target;
}

// 不同业务对"聚合 vs 分散"的需求差异未被表达
want_affine = !wake_wide(p) && cpumask_test_cpu(cpu, p->cpus_ptr);



```

`wake_wide()` 通过 `wakee_flips`（每 jiffy 翻转数）配合 `sd_llc_size` 作为阈值判断 waker/wakee 是否"非专一关系"，超过则不走 affine。然后从底向上遍历调度域，找到第一个同时包含 `this_cpu` 和 `prev_cpu` 且带 `SD_WAKE_AFFINE` 标志的域，调用 `wake_affine()`。

**（2）wake_affine 决策**

分两路——`wake_affine_idle` 看"现在"，若唤醒 CPU 空闲且与 prev CPU 共享 cache，或 sync 唤醒且 waker 自己独占 rq，则把 wakee 放到 waker CPU；`wake_affine_weight` 看"权重"，比较 this/prev 的 effective load（含 task_h_load 与 imbalance_pct bias），倾向把 wakee 放到 waker。两路都失败则保持 prev CPU。

**（3）select_idle_sibling 多级回退**

依次尝试：①target 自身空闲 → ②prev 空闲且与 target 共享 cache → ③per-cpu kthread sync 唤醒回退 prev → ④recent_used_cpu 空闲且与 target 共享 cache → ⑤异构容量系统走 sd_asym_cpucapacity 域的 capacity 扫描 → ⑥否则取 sd_llc，先 SMT 域 select_idle_smt，再 LLC 域 select_idle_cpu。

`cpus_share_cache()` 是纯二元判断：比较两个 CPU 的 `sd_llc_id` 是否相等，同 LLC → 共享，否则 → 不共享。无中间层次。

**（4）SIS_UTIL 扫描量自适应**

`select_idle_cpu` 用 `sd->shared->nr_idle_scan`（基于 LLC idle 比例动态维护）和 `avg_idle / avg_scan_cost` 比例限制扫描数；LLC 过载（`nr_idle_scan` 降为 0）时直接返回不扫。扫描域仍是 LLC span，512C+ 下即使限扫也可能跨多个 Chiplet。

**（5）EAS 协调**

开启 EAS 时 `find_energy_efficient_cpu` 优先于 wake_affine，按能量模型选 CPU；wake_affine 仅在 EAS 关闭或 EAS 失败时生效。

**（6）NUMA 距离感知**

调度域拓扑中，远端 NUMA 域（distance > `node_reclaim_distance`）会被清除 `SD_WAKE_AFFINE` 标志，因此 wake_affine 不会在跨远端 NUMA 唤醒时触发。同 NUMA 内跨 LLC 的唤醒仍会触发 wake_affine。

#### 2）方案不足

**（1）`wake_wide` 阈值随 LLC 大小线性放大，512C+ 下扩散难触发**

阈值是 `slave >= sd_llc_size 且 master >= slave * sd_llc_size`，在 512 核 LLC 下需要极高的翻转频率才会被判定为"非专一"，导致本应扩散的高扇出工作负载（如千级 worker 的 Web/存储后端）长时间困在单个 LLC 内自旋，反而降低吞吐。**缓解**：阈值改为按工作负载扇出与 LLC 大小二者中较小者，或按 CPU 数对数缩放（如 `ilog2(sd_llc_size)`）。

**（2）wake_affine 只比较两个 CPU，缺乏第三候选**

`wake_affine()` 只在 `this_cpu`（waker）和 `prev_cpu`（wakee 上次运行的 CPU）之间做二元选择。在多 LLC 系统中，最优目标可能是**第三个 CPU**——同 NUMA 的另一个 LLC 中的 idle CPU，该 CPU 虽然不与 waker 共享 LLC，但 NUMA 距离近、且可能更空闲。当前逻辑完全不考虑这个候选。

**（3）select_idle_sibling 的搜索被锁死在单个 LLC 内**

`select_idle_sibling` 最终走 `select_idle_cpu(p, sd, ...)` 时，`sd` 是 `per_cpu(sd_llc, target)`，即**只扫描 target 所在的单个 LLC 域**。如果 target LLC 全部 busy，即使相邻 LLC（同 NUMA）完全空闲，也不会去那里找。对于短生命周期任务（如网络 softirq、RPC 处理），等不到 load balancer 就已经产生延迟。

**（4）SMT 干扰：唤醒任务数超过物理核数时的负收益**

当 wake_affine 倾向于把任务聚合到同一 LLC，且 LLC 内**可运行任务数 > 物理核数**时，SMT 干扰带来的吞吐损失可能远超 cache 局部性收益。具体来说：`wake_affine_weight` 中使用 `capacity_of(cpu)` 反映剩余算力，但在 SMT 场景下，当 LLC 内 18 个任务跑在 16 个物理核上，SMT 干扰导致每个任务吞吐下降约 20-30%。此时若分散到相邻 LLC1（只有 4 个任务），即使 cache cold，吞吐可能反而更高。当前代码对此无感知——cache 局部性收益与 SMT 干扰损失之间没有量化权衡。

**（5）`select_idle_cpu` 扫描域仍是 LLC span，512C+ 下跨 Chiplet 抖动**

SIS_UTIL 限制扫描数量但 `for_each_cpu_wrap` 起点是 target+1，扫描仍可能跨多个 Chiplet，选中的 idle CPU 与 waker cache 距离不确定。**缓解**：扫描时优先按 cache 距离排序候选 CPU，或把扫描起点收敛到与 target cache 距离最近的子集。

**（6）`wake_affine_weight` 的 load 模型对跨 Chiplet 代价无量化**

effective load 比较只看 cpu_load 与 task_h_load，没有把"跨 LLC/Chiplet 迁移带来的 cache 预热损失"折算成额外负载，决策可能倾向把 wakee 放到 waker 而忽略实际 cache 代价。**缓解**：在 prev_eff_load 上加 cache 距离惩罚项（同 LLC 加 0、同 NUMA 加 N、跨 NUMA 加 M），让 wake_affine_weight 显式权衡 cache 距离。

**（7）SD_WAKE_AFFINE 域匹配粗糙，跨 LLC 时使用过高层域的参数**

`select_task_rq_fair` 从底向上遍历域，找到第一个同时包含 `this_cpu` 和 `prev_cpu` 且带 `SD_WAKE_AFFINE` 的域就 break。当 `this_cpu` 在 LLC0、`prev_cpu` 在 LLC1（同 PKG/同 NUMA），第一个匹配域是 PKG 或 NUMA 级。此时 `wake_affine_weight()` 传入的 `sd->imbalance_pct` 是高层域的值（默认 117），但实际负载比较只在两个 CPU 间进行，PKG 级的 imbalance_pct 对 LLC 级别的决策没有参考意义——同 LLC 内迁移是无 cache 代价的，跨 LLC 迁移有代价，两者的阈值应该不同。

**（8）sync 唤醒判断只看 waker 自己 `nr_running`，未看 wakee 历史**

sync 路径在"waker 队列只有自己"时倾向把 wakee 堆到 waker；但 wakee 此前在 prev 上可能有大量热 cache，被堆到 waker 后热数据失效。**缓解**：sync 判断结合 wakee 的 prev cache 热度（最近 N ms 内是否刚执行）与 waker 是否与 wakee 共享数据。

**（9）没有 waker/wakee 共享数据亲和度量**

现有逻辑只判 CPU 间 cache 共享，未判 waker 与 wakee 是否访问共享数据。两个访问同一 shmem 的进程本应优先同 LLC 调度，但 wake_affine 看不到这种"数据亲和"。**缓解**：引入 waker/wakee 数据亲和度量（参考 mm-level 共享或 cgroup 内 task 关系），在 wake_affine_weight 中作为额外加权。

**（10）`record_wakee` 用 jiffy 粒度衰减，对高频唤醒场景判断粗**

`wakee_flips` 每 jiffy 衰减一次，但在 kHz 级唤醒率下"翻转数"含义被高频噪声主导；512C+ 上 waker 数量爆炸，wake_wide 的"非专一"判断更容易被噪声触发或漏判。**缓解**：衰减窗口按唤醒频率自适应，或用 EWMA 替代 jiffy 衰减。

**（11）不同业务对"聚合 vs 分散"的需求差异未被表达**

当前 wake_affine 是全局统一策略，但不同业务场景对聚合还是分散的需求截然不同：延迟敏感型（RPC/DB）期望分散以降低 SMT 干扰，批处理吞吐型（HPC）期望聚合以提高 cache 命中率，IO 密集型期望 sync 聚合，混部隔离型期望禁止 affine 防 cache 污染。全局一个策略无法兼顾。

#### 3）改进方案

**改进 A：wake_affine 引入 LLC 级 fallback（对应不足 2）**

在 `wake_affine()` 中，当原有的 idle/weight 路径未选中 `this_cpu` 时，增加一个 LLC 级负载比较作为"第三仲裁"：

```c
static int wake_affine(struct sched_domain *sd, struct task_struct *p,
                       int this_cpu, int prev_cpu, int sync)
{
    int target = nr_cpumask_bits;

    if (sched_feat(WA_IDLE))
        target = wake_affine_idle(this_cpu, prev_cpu, sync);
    if (sched_feat(WA_WEIGHT) && target == nr_cpumask_bits)
        target = wake_affine_weight(sd, p, this_cpu, prev_cpu, sync);

    /* 新增：LLC 级 fallback —— 同 NUMA 跨 LLC 时考虑第三候选 */
    if (target != this_cpu && sched_feat(WA_LLC_AWARE)) {
        int llc_alt = wake_affine_llc_fallback(p, this_cpu, prev_cpu);
        if (llc_alt != nr_cpumask_bits)
            return llc_alt;
    }

    schedstat_inc(p->stats.nr_wakeups_affine_attempts);
    if (target != this_cpu)
        return prev_cpu;
    schedstat_inc(sd->ttwu_move_affine);
    schedstat_inc(p->stats.nr_wakeups_affine);
    return target;
}
```

`wake_affine_llc_fallback` 的核心逻辑：
- 只在 `this_cpu` 与 `prev_cpu` 跨 LLC 但同 NUMA 时考虑
- 比较 `this_cpu` 所在 LLC 与 `prev_cpu` 所在 LLC 的负载（复用 `sd->shared->util_avg`）
- 只有 `prev_llc` 负载比 `this_llc` 高出 30% 以上时才迁移到 `this_cpu`，避免抖动

**改进 B：受控的跨 LLC idle 搜索（对应不足 3）**

在 `select_idle_sibling` 中，当 `select_idle_cpu` 在 target LLC 内未找到 idle CPU 时，增加受控的跨 LLC 搜索：

```c
static int select_idle_sibling(struct task_struct *p, int prev, int target)
{
    /* ... 原有逻辑：target/prev/recent_used/sd_llc 内搜索 ... */

    i = select_idle_cpu(p, sd, has_idle_core, target);
    if ((unsigned)i < nr_cpumask_bits)
        return i;

    /* 新增：受控的跨 LLC 搜索 */
    if (sched_feat(SIS_CROSS_LLC))
        i = select_idle_cross_llc(p, prev, target);
    if ((unsigned)i < nr_cpumask_bits)
        return i;

    return target;
}
```

三个控制点防止过度迁移：

| 控制点 | 目的 | 实现 |
|--------|------|------|
| 只在 target LLC 满时触发 | 避免不必要的跨 LLC 迁移 | 前置 `llc_has_idle_cpu` 检查 |
| 只在同 NUMA 内搜索 | 保证内存局部性 | 用 `sd_numa` 做掩码，排除 target LLC |
| 限制扫描深度 | 控制开销和迁移幅度 | `min(llc_size, 4)` 个 CPU |

此外引入 `nr_cross_llc_migrations` 计数器，当一段时间内跨 LLC 迁移率过高时自动关闭 `SIS_CROSS_LLC`，由 load balancer 接管。

**改进 C：SMT 过载感知（对应不足 4）**

在 `wake_affine()` 前置 SMT 过载检查，当 `this_cpu` 所在 LLC 的可运行任务数超过物理核数时，禁止 affine：

```c
static int wake_affine_smt_check(int this_cpu, int prev_cpu)
{
    if (!sched_smt_active())
        return SMT_NO_OVERRIDE;

    int threads_per_core = cpu_smt_num_threads;
    struct sched_domain *llc_sd = per_cpu(sd_llc, this_cpu);
    if (!llc_sd)
        return SMT_NO_OVERRIDE;

    int physical_cores = cpumask_weight(sched_domain_span(llc_sd)) / threads_per_core;
    int llc_running = llc_nr_running(this_cpu);

    /* LLC 内可运行任务超过物理核数 → SMT 干扰风险，禁止 affine */
    if (llc_running > physical_cores)
        return SMT_OVERRIDE_SPREAD;

    return SMT_NO_OVERRIDE;
}
```

核心逻辑：计算 `this_cpu` 所在 LLC 的可运行任务数 vs 物理核数，超过物理核数则禁止 affine，让任务留在/分散到 `prev_cpu` 的 LLC。同 LLC 内的 affine 不受影响。

**改进 D：跨 LLC 时使用更严格的 imbalance_pct（对应不足 7）**

在 `wake_affine_weight` 中根据实际 cache 关系选择阈值：

```c
int imbalance_pct;
if (cpus_share_cache(this_cpu, prev_cpu))
    imbalance_pct = 110;  /* 同 LLC：更宽松，鼓励聚合 */
else
    imbalance_pct = 125;  /* 跨 LLC：更严格，抑制迁移 */
```

引入独立的 `wake_imbalance_pct` 变量，只在 wake 路径使用，不影响 balance 逻辑。

**改进 E：cgroup 级别 wake affine 配置（对应不足 11）**

引入 cgroup 级别的 wake affine 策略控制，与 `cpuset` 正交，子 cgroup 未设置时继承父级策略：

```c
enum wake_affine_policy {
    WA_POLICY_DEFAULT      = 0,  /* 使用全局配置 */
    WA_POLICY_FORCE_AFFINE = 1, /* 强制聚合到 waker 附近 */
    WA_POLICY_FORCE_SPREAD = 2, /* 禁止聚合，优先分散到 idle core/LLC */
    WA_POLICY_SMT_AWARE    = 3, /* 允许 affine 但避免 SMT 干扰 */
};
```

在 `select_task_rq_fair` 中根据策略覆盖 `want_affine`：

- `force_spread`：禁止 affine，走 spread 路径，优先在不同 LLC 找 idle core
- `force_affine`：无条件 affine
- `smt_aware`：允许 affine 但检查 SMT 过载（联动改进 C）
- `default`：原有逻辑

接口设计：`/sys/fs/cgroup/<group>/cpu.wake_affine_policy`，可结合 RDT/CAT 配合实现强制数据局部性。

不同业务的期望行为：

| 业务类型 | 期望策略 | 理由 |
|----------|---------|------|
| 延迟敏感型（RPC/DB） | force_spread | 降低 SMT 干扰，最大化每核吞吐 |
| 批处理吞吐型（HPC） | force_affine | 数据共享，cache 命中率优先 |
| IO 密集型 | default/force_affine | sync 唤醒模式，数据刚产生 |
| 混部隔离型 | force_spread | 防止互相 cache 污染 |

**改进优先级建议**

| 优先级 | 改进项 | 收益/风险 |
|--------|--------|-----------|
| P0 | 改进 C：SMT 过载感知 | 高收益/低风险：防止 SMT 干扰导致的负优化 |
| P0 | 改进 E：cgroup 级别策略 | 高收益/中风险：不同业务差异化调度 |
| P1 | 改进 B：受控跨 LLC 搜索 | 中收益/中风险：需仔细控制迁移率 |
| P1 | 改进 A：LLC 级 fallback | 中收益/中风险：需负载聚合指标 |
| P2 | 改进 D：跨 LLC 更严格阈值 | 低风险但收益有限 |

改进 C（SMT 感知）与改进 E（cgroup 策略）可以联动：`smt_aware` 策略本质上是改进 C 的 cgroup 粒度版本——当业务选择 `smt_aware` 策略时，自动启用 SMT 过载检查逻辑。

### 2.4 reduce new idle loadbalance

#### 1）实现思路

mainline linux 7.1 的 newidle LB 在 CPU 即将进入 idle 前触发，目的是"赶在 idle 前从其他 rq 拉一个任务过来"，但代价是关中断期间持 rq lock 扫描多个调度域，可能造成延迟抖动与 cache 颠簸。因此 mainline 内置多层"减少 newidle"机制，整体是"代价-收益自适应 + 概率收敛"，而非一刀切禁用。

- **早退闸门**：进入 newidle 后先看是否有挂起任务、CPU 是否活跃、系统是否过载、期望 idle 时长是否低于该域历史最大 newidle 代价四道闸门，任一不满足即早退。
- **代价滚动统计**：每次 newidle 实际扫描的耗时累加到域级与 rq 级"最大 newidle 代价"，作为下次"代价 vs 收益"判断的依据；rq 维护的期望 idle 时长越短，越倾向早退。
- **概率触发**：sched_feat 概率触发开关用 1024 面骰子按域级历史成功率随机化触发，避免所有 idle CPU 同步抢任务造成的 cache line bouncing；成功率高则更频繁触发，成功率低则几乎不触发。
- **遍历域收敛**：自下而上遍历调度域，每层先看"期望 idle 时长 vs 已累计代价 + 该域最大代价"决定是否提前 break；只有带 NEWIDLE 标志的域才执行实际拉取。
- **nohz 协同**：本轮 newidle 没拉到任务时，进一步走 nohz 路径让 idle CPU 数量统计在 nohz 框架内集中处理；nohz 路径自己也有"期望 idle 时长低于迁移代价阈值"的早退。
- **无 cache 拓扑感知**：上述机制全部基于"代价-收益"统计，没有 cache 距离判断——newidle 可能从远端 Chiplet 拉一个任务过来，造成 cache 预热损失与跨 Chiplet 一致性开销；同时 LLC util 缓存在 newidle 路径主动跳过，决策与统计脱节。

#### 2）方案不足

- **无 cache 拓扑感知，newidle 倾向跨 Chiplet 拉任务**：自下而上遍历调度域，每层都可能拉，没有"先看本地 LLC 是否有候选，再考虑跨 LLC"的层次化优先级，512C+ 下 idle CPU 进入睡眠前很容易从远端 Chiplet 拉来一个冷 cache 任务。**缓解**：在拉取路径内对候选 busiest group 按 cache 距离排序，本地 LLC 候选优先；或在 newidle 路径先尝试 LLC 层拉取未果再上升。
- **NEWIDLE 标志默认在 NUMA 层也设置，跨 NUMA newidle 抖动**：默认拓扑在 MC/DIE/NUMA 各层都带 NEWIDLE 标志，意味着 idle CPU 会跨 NUMA 主动拉任务，对延迟敏感业务造成 NUMA 距离惩罚。**缓解**：NUMA 层默认清掉 NEWIDLE 标志，仅在 LLC 层保留；或允许 per-tg 配置 NEWIDLE 范围。
- **代价统计是 rq 级单值，无法区分 cache 距离代价**：同一个"最大 newidle 代价"不区分"从 LLC 内拉"和"从跨 NUMA 拉"的代价差异，远端拉取的高代价可能被本地拉取的低代价平均掉，导致远端拉取过早被允许。**缓解**：代价统计按 cache 距离分桶（per-LLC、per-NUMA、cross-NUMA 各一个），早退判断用对应桶的代价。
- **概率触发仅按成功率，不看 cache 代价**：成功率高的远端拉取（短期能拉到任务）会被频繁触发，但远端拉的 cache 代价未纳入成功率计算，可能让"高频但高代价"的远端 newidle 反复触发。**缓解**：成功率改为按"成功收益 - cache 代价"净值，或对跨 Chiplet 拉取单独维护概率。
- **LLC util 缓存在 newidle 路径跳过，决策与统计脱节**：与 2.1/2.2 同样的根因——newidle 不写 LLC util 缓存但决策矩阵仍读它，导致 newidle 路径下 LLC 是否过载的判断基于陈旧数据，可能误判"还有空间"。**缓解**：允许 newidle 路径增量更新 LLC util 缓存，或为 newidle 单独维护瞬时 idle 比例。
- **"最大 newidle 代价"用 max 而非滚动平均，长尾样本固化**：单个偶发的慢 newidle（如 CPU 热插拔期间）会让该代价长期偏高，导致后续 newidle 早退过于激进。**缓解**：改为 EWMA 或 P99 滚动统计，让慢样本随时间衰减。
- **遍历早退基于累计代价，未考虑后续域的潜在收益**：到达 NUMA 层时代价已累加 LLC/DIE 层，可能因为前几层慢而早退错过 NUMA 层的空闲 CPU，反之亦可能因为前几层快而误入 NUMA 层造成跨 NUMA 抖动。**缓解**：早退判断纳入"本层及更高层的预期空闲 CPU 数"，让收益期望更显式。
- **nohz 路径的全局迁移代价阈值固定**：默认 5ms 才会早退对延迟敏感型业务偏大，对 batch 型业务偏小，512C+ 下两类业务混部时该值难以两全。**缓解**：阈值 per-rq 按 sched_class 或 per-tg 配置，或按 CPU 数自适应缩放。

---
