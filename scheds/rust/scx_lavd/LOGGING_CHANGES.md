# scx_lavd 负载均衡日志增强说明

## 修改概述

为了帮助定位 LLC 间的负载不均衡问题，在 `scx_lavd` 调度器中添加了详细的负载均衡日志。

## 修改的文件

1. `src/bpf/balance.bpf.c` - 负载均衡核心逻辑
2. `src/bpf/sys_stat.bpf.c` - 系统统计收集

## 添加的日志点

### 1. 负载均衡阈值计算 (`balance.bpf.c`)

在 `plan_x_cpdom_migration` 函数中添加了系统级负载均衡阈值日志：

```c
/* Debug: Log load balancing thresholds */
debugln("LAVD_LB: System avg_load_invr=%llu, mig_delta=%llu, "
        "stealer_th=%llu, stealee_th=%llu, nr_stealee=%u, nz_qlen=%d",
        avg_load_invr, x_mig_delta, stealer_threshold,
        stealee_threshold, nr_stealee, nz_qlen);
```

**日志含义**：
- `avg_load_invr`: 系统平均负载逆序值
- `mig_delta`: 迁移阈值增量
- `stealer_th`: Stealer 域阈值（低于此值为 Stealer）
- `stealee_th`: Stealee 域阈值（高于此值为 Stealee）
- `nr_stealee`: Stealee 域数量
- `nz_qlen`: 有排队任务的域数量

### 2. 域分类日志 (`balance.bpf.c`)

在域分类逻辑中添加了详细日志：

```c
/* Stealer 域 */
debugln("LAVD_LB: Domain %llu is STEALER: load_invr=%llu (th=%llu), "
        "util=%llu, qlen=%u, active_cpus=%u",
        cpdom_id, cpdomc->load_invr, stealer_threshold,
        util, qlen, cpdomc->nr_active_cpus);

/* Stealee 域 */
debugln("LAVD_LB: Domain %llu is STEALEE: load_invr=%llu (th=%llu), "
        "util=%llu, qlen=%u, active_cpus=%u",
        cpdom_id, cpdomc->load_invr, stealee_threshold,
        util, qlen, cpdomc->nr_active_cpus);

/* Neutral 域（既不是 Stealer 也不是 Stealee） */
debugln("LAVD_LB: Domain %llu is NEUTRAL: load_invr=%llu (stealer_th=%llu, stealee_th=%llu), "
        "util=%llu, qlen=%u, active_cpus=%u",
        cpdom_id, cpdomc->load_invr, stealer_threshold, stealee_threshold,
        util, qlen, cpdomc->nr_active_cpus);
```

**日志含义**：
- `Domain %llu`: 计算域 ID（通常对应 LLC）
- `load_invr`: 域的负载逆序值
- `util`: CPU 利用率
- `qlen`: 队列长度
- `active_cpus`: 活跃 CPU 数量

### 3. 任务迁移决策日志 (`balance.bpf.c`)

在 `try_to_steal_task` 函数中添加迁移尝试和结果日志：

```c
/* 迁移尝试 */
debugln("LAVD_LB: Stealer %llu (load=%llu) attempt steal from %llu (load=%llu), "
        "DSQ %llu queued=%d",
        cpdomc->id, cpdomc->load_invr, cpdomc_pick->id, cpdomc_pick->load_invr,
        dsq_id, scx_bpf_dsq_nr_queued(dsq_id));

/* 迁移成功 */
debugln("LAVD_LB: Successfully stole task from domain %llu (DSQ: %llu)",
        cpdomc_pick->id, dsq_id);
```

**日志含义**：
- `Stealer %llu`: Stealer 域 ID
- `attempt steal from %llu`: 尝试从 Stealee 域偷取任务
- `DSQ %llu queued=%d`: 目标 DSQ 的排队任务数
- `Successfully stole`: 成功偷取任务

### 4. 域统计日志 (`sys_stat.bpf.c`)

在 `collect_sys_stat` 函数中添加域统计日志：

```c
/* Debug: Log domain stats */
debugln("LAVD_LB: Domain %llu stats: util_wall_sum=%llu, avg_util_wall_sum=%llu, "
        "qlen=%u, nr_active_cpus=%u",
        cpdom_id, cpdomc->cur_util_wall_sum, cpdomc->avg_util_wall_sum,
        cpdomc->nr_queued_task, cpdomc->nr_active_cpus);
```

**日志含义**：
- `util_wall_sum`: 当前 CPU 利用率总和
- `avg_util_wall_sum`: 平均 CPU 利用率总和
- `qlen`: 队列长度
- `nr_active_cpus`: 活跃 CPU 数量

### 5. 负载均衡完成日志 (`balance.bpf.c`)

在 `plan_x_cpdom_migration` 函数结束时添加总结日志：

```c
/* Debug: Log final stealer/stealee counts */
debugln("LAVD_LB: Load balancing finished: %u stealee domains, min_load_invr=%llu, max_load_invr=%llu",
        nr_stealee, min_load_invr, max_load_invr);
```

**日志含义**：
- `stealee domains`: Stealee 域数量
- `min_load_invr`: 最小负载逆序值
- `max_load_invr`: 最大负载逆序值

## 使用方法

### 1. 启用日志

设置 `verbose` 参数为 1 或更高：

```bash
# 启用基础日志
scx_lavd --verbose 1

# 启用详细日志
scx_lavd --verbose 2
```

### 2. 查看日志

使用 `trace_pipe` 查看 BPF 日志：

```bash
# 查看所有日志
cat /sys/kernel/debug/tracing/trace_pipe

# 过滤 LAVD_LB 日志
cat /sys/kernel/debug/tracing/trace_pipe | grep "LAVD_LB"
```

### 3. 日志分析

通过日志可以分析：

1. **负载分布不均衡**：检查各 LLC 域的 `load_invr` 差异
2. **阈值设置问题**：检查 `stealer_th` 和 `stealee_th` 是否合理
3. **迁移决策问题**：检查 Stealer/Stealee 分类是否正确
4. **迁移效果**：检查任务是否成功从高负载域迁移到低负载域

## 示例日志输出

```
[plan_x_cpdom_migration:123] LAVD_LB: System avg_load_invr=123456, mig_delta=12345, stealer_th=111111, stealee_th=135801, nr_stealee=2, nz_qlen=1
[plan_x_cpdom_migration:180] LAVD_LB: Domain 0 is STEALER: load_invr=100000 (th=111111), util=50000, qlen=0, active_cpus=8
[plan_x_cpdom_migration:190] LAVD_LB: Domain 1 is STEALEE: load_invr=140000 (th=135801), util=80000, qlen=5, active_cpus=8
[try_to_steal_task:364] LAVD_LB: Stealer 0 (load=100000) attempt steal from 1 (load=140000), DSQ 123456 queued=3
[try_to_steal_task:381] LAVD_LB: Successfully stole task from domain 1 (DSQ: 123456)
[plan_x_cpdom_migration:211] LAVD_LB: Load balancing finished: 1 stealee domains, min_load_invr=100000, max_load_invr=140000
```

## 注意事项

1. **性能影响**：日志会增加 BPF 程序的开销，建议只在调试时启用
2. **日志量**：如果系统负载高或域数量多，日志量可能较大
3. **调试建议**：结合 `perf sched` 或 `trace-cmd` 等工具进行更深入的分析
