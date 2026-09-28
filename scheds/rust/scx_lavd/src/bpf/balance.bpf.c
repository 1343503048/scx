/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025 Valve Corporation.
 * Author: Changwoo Min <changwoo@igalia.com>
 */

#include <scx/common.bpf.h>
#include <bpf_arena_common.bpf.h>
#include "intf.h"
#include "lavd.bpf.h"
#include "util.bpf.h"
#include <errno.h>
#include <stdbool.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <lib/cgroup.h>


extern const volatile u8	mig_delta_pct;
extern const volatile u8	no_fast_lb;
extern const volatile u64	lb_low_util_wall;

u64 __attribute__ ((noinline)) calc_mig_delta(u64 avg_load_invr, int nz_qlen,
					      u64 mig_delta_factor)
{
	/*
	 * Note that added "noinline" to make the verifier happy.
	 * When mig_delta_factor > 0, the user specified a fixed
	 * migration delta percentage; otherwise use the dynamic
	 * shift-based heuristic.
	 */
	if (mig_delta_factor > 0)
		return avg_load_invr * mig_delta_factor / LAVD_SCALE;
	if (nz_qlen >= sys_stat.nr_active_cpdoms)
		return avg_load_invr >> LAVD_CPDOM_MIG_SHIFT_OL;
	if (nz_qlen == 0)
		return avg_load_invr >> LAVD_CPDOM_MIG_SHIFT_UL;
	return avg_load_invr >> LAVD_CPDOM_MIG_SHIFT;
}

/*
 * Classify a single compute domain as stealer, stealee, or neutral.
 * Returns 1 if the domain became a stealee, 0 otherwise.
 * Marked noinline so the verifier analyses it separately from the
 * calling loop, keeping the jump complexity of the caller manageable.
 */
int __attribute__((noinline))
classify_cpdom(struct cpdom_ctx *cpdomc, u64 total_load_invr,
	       u64 total_cap_sum, int nz_qlen, u64 mig_delta_factor)
{
	u64 x_mig_delta = 0;
	u64 fair_share_invr = 0;
	u64 stealer_threshold = 0;
	u64 stealee_threshold = 0;

	if (!cpdomc)
		return 0;

	if (no_fast_lb && sys_stat.nr_active_cpdoms) {
		u64 avg = total_load_invr / sys_stat.nr_active_cpdoms;
		x_mig_delta = calc_mig_delta(avg, nz_qlen, mig_delta_factor);
		stealer_threshold = avg - x_mig_delta;
		stealee_threshold = avg + x_mig_delta;
	} else if (cpdomc->nr_active_cpus && total_cap_sum > 0) {
		fair_share_invr = total_load_invr *
			     cpdomc->cap_sum_active_cpus /
			     total_cap_sum;

		x_mig_delta = calc_mig_delta(
				fair_share_invr, nz_qlen,
				mig_delta_factor);

		stealer_threshold = fair_share_invr - x_mig_delta;
		stealee_threshold = fair_share_invr + x_mig_delta;
	}

	/*
	 * Under-loaded active domains become a stealer.
	 * Ingress budget = half the deficit below fair share.
	 */
	if (cpdomc->nr_active_cpus &&
	    cpdomc->load_invr <= stealer_threshold) {
		u64 stealer_budget = 0;

		if (fair_share_invr > cpdomc->load_invr)
			stealer_budget = (fair_share_invr -
					  cpdomc->load_invr) / 2;

		WRITE_ONCE(cpdomc->stealer_budget_invr, stealer_budget);
		WRITE_ONCE(cpdomc->stealee_budget_invr, 0);
		WRITE_ONCE(cpdomc->is_stealer, true);
		WRITE_ONCE(cpdomc->is_stealee, false);
		return 0;
	}

	/*
	 * Over-loaded or non-active domains become a stealee.
	 * Egress budget = half the excess above fair share. Halving
	 * (rather than draining the full excess) avoids two failure
	 * modes:
	 * - Ping-pong: prevent overshooting. Moving everything in one
	 *   round may trigger the imbalance and ping-pong effect.
	 * - Thundering-herd: without a per-round limit, every stealer
	 *   that sees this domain can migrate out the tasks from it and
	 *   drain it past the fair share in a single round.
	 *
	 * Also skip domains with nothing to steal: a domain may appear
	 * overloaded due to running task utilization (avg_util_invr_sum)
	 * but have an empty DSQ — trying to steal from it would waste
	 * cycles and could cause oscillation.
	 */
	if (!cpdomc->nr_active_cpus ||
	    cpdomc->load_invr >= stealee_threshold) {
		u64 stealee_budget_invr = 0;

		if (cpdomc->qload_invr == 0)
			goto reset_role;

		if (cpdomc->load_invr > fair_share_invr)
			stealee_budget_invr = (cpdomc->load_invr -
					       fair_share_invr) / 2;

		if (!stealee_budget_invr)
			goto reset_role;

		WRITE_ONCE(cpdomc->stealee_budget_invr, stealee_budget_invr);
		WRITE_ONCE(cpdomc->stealer_budget_invr, 0);
		WRITE_ONCE(cpdomc->is_stealer, false);
		WRITE_ONCE(cpdomc->is_stealee, true);
		return 1;
	}

reset_role:
	WRITE_ONCE(cpdomc->stealee_budget_invr, 0);
	WRITE_ONCE(cpdomc->stealer_budget_invr, 0);
	WRITE_ONCE(cpdomc->is_stealer, false);
	WRITE_ONCE(cpdomc->is_stealee, false);
	return 0;
}

__weak
int plan_x_cpdom_migration(void)
{
	struct cpdom_ctx *cpdomc;
	u64 cpdom_id;
	u32 nr_stealee = 0;
	u64 max_avg_util_wall = 0;
	u64 util;
	u64 total_load_invr = 0;
	u64 total_cap_sum = 0;
	bool overflow_running = false;
	int nz_qlen = 0;

	/*
	 * Calculate load for each active compute domain.
	 */
	bpf_for(cpdom_id, 0, nr_cpdoms) {
		if (cpdom_id >= LAVD_CPDOM_MAX_NR)
			break;

		cpdomc = MEMBER_VPTR(cpdom_ctxs, [cpdom_id]);
		if (!cpdomc->nr_active_cpus) {
			if (cpdomc->cur_util_wall_sum > 0)
				overflow_running = true;
			continue;
		}

		util = (cpdomc->avg_util_wall_sum << LAVD_SHIFT) / cpdomc->nr_active_cpus;
		if ((util >> LAVD_SHIFT) > max_avg_util_wall)
			max_avg_util_wall = util >> LAVD_SHIFT;

		/*
		 * Domain load combines running load (avg_util_invr_sum)
		 * and queued load (qload_invr, tracked atomically via
		 * account/unaccount at enqueue/running).
		 */
		if (no_fast_lb) {
			u64 qlen = cpdomc->nr_queued_task;
			u64 qlen_invr = (qlen << (LAVD_SHIFT * 3)) /
					cpdomc->cap_sum_active_cpus;
			cpdomc->load_invr = util + qlen_invr;
			if (qlen)
				nz_qlen++;
		} else {
			cpdomc->load_invr = cpdomc->avg_util_invr_sum +
					    cpdomc->qload_invr;
			if (cpdomc->qload_invr)
				nz_qlen++;
		}
		total_load_invr += cpdomc->load_invr;
		total_cap_sum += cpdomc->cap_sum_active_cpus;
	}

	/*
	 * When the highest per-CPU utilization among all compute
	 * domains is below the low utilization threshold, there is
	 * no meaningful workload worth rebalancing across domains.
	 */
	if (lb_low_util_wall > 0 && max_avg_util_wall < lb_low_util_wall)
		goto reset_and_skip_lb;

	/*
	 * Classify stealer and stealee domains using per-domain
	 * capacity-proportional targets. Each domain's target is its
	 * fair share of total system load scaled by its capacity
	 * proportion.
	 */
	u64 mig_delta_factor = 0;
	if (mig_delta_pct > 0)
		mig_delta_factor = (mig_delta_pct << LAVD_SHIFT) / 100;

	bpf_for(cpdom_id, 0, nr_cpdoms) {
		if (cpdom_id >= LAVD_CPDOM_MAX_NR)
			break;

		cpdomc = MEMBER_VPTR(cpdom_ctxs, [cpdom_id]);

		nr_stealee += classify_cpdom(cpdomc, total_load_invr,
					     total_cap_sum, nz_qlen,
					     mig_delta_factor);
	}

	if (nr_stealee == 0 && !overflow_running)
		goto reset_and_skip_lb;

	sys_stat.nr_stealee = nr_stealee;

	return 0;

reset_and_skip_lb:
	if (sys_stat.nr_stealee > 0) {
		bpf_for(cpdom_id, 0, nr_cpdoms) {
			if (cpdom_id >= LAVD_CPDOM_MAX_NR)
				break;

			cpdomc = MEMBER_VPTR(cpdom_ctxs, [cpdom_id]);
			WRITE_ONCE(cpdomc->stealee_budget_invr, 0);
			WRITE_ONCE(cpdomc->stealer_budget_invr, 0);
			WRITE_ONCE(cpdomc->is_stealer, false);
			WRITE_ONCE(cpdomc->is_stealee, false);
		}
		sys_stat.nr_stealee = 0;
	}
	return 0;
}

/*
 * dsq_id: candidate DSQ to consume from, can be per-cpdom or per-cpu.
 */
static bool consume_dsq(struct cpdom_ctx *cpdomc, u64 dsq_id)
{
	bool ret;
	u64 before = 0;

	if (is_monitored)
		before = bpf_ktime_get_ns();
	/*
	 * Try to consume a task on the associated DSQ.
	 */
	ret = scx_bpf_dsq_move_to_local(dsq_id, 0);

	if (is_monitored)
		cpdomc->dsq_consume_lat = time_delta(bpf_ktime_get_ns(), before);

	return ret;
}

u64 __attribute__((noinline)) dsq_peek_task_load(u64 dsq_id)
{
	struct task_struct *peek_p = __COMPAT_scx_bpf_dsq_peek(dsq_id);

	if (peek_p) {
		task_ctx *peek_taskc = get_task_ctx(peek_p);
		if (peek_taskc)
			return task_load_metric(peek_taskc);
	}
	return 0;
}

u64 __attribute__((noinline)) pick_most_loaded_dsq(struct cpdom_ctx *cpdomc)
{
	u64 pick_dsq_id = -ENOENT;
	s32 highest_queued = -1;

	if (!cpdomc) {
		scx_bpf_error("Invalid cpdom context");
		return -ENOENT;
	}

	/*
	 * For simplicity, try to just steal from the (either per-CPU or
	 * per-domain) DSQs with the highest number of queued_tasks
	 * in this domain.
	 */
	if (use_cpdom_dsq()) {
		pick_dsq_id = cpdom_to_dsq(cpdomc->id);
		highest_queued = scx_bpf_dsq_nr_queued(pick_dsq_id);
	}

	/*
	 * When tasks on a per-CPU DSQ are not migratable
	 * (e.g., pinned_slice_ns is on but per_cpu_dsq is not),
	 * there is no need to check per-CPU DSQs.
	 */
	if (is_per_cpu_dsq_migratable()) {
		int pick_cpu = -ENOENT, cpu, i, j, k;

		bpf_for(i, 0, LAVD_CPU_ID_MAX/64) {
			u64 cpumask = cpdomc->__cpumask[i];
			bpf_for(k, 0, 64) {
				s32 queued;
				j = cpumask_next_set_bit(&cpumask);
				if (j < 0)
					break;
				cpu = (i * 64) + j;
				if (cpu >= nr_cpu_ids)
					break;
				queued = scx_bpf_dsq_nr_queued(cpu_to_dsq(cpu)) +
					 scx_bpf_dsq_nr_queued(SCX_DSQ_LOCAL_ON | cpu);
				if (queued > highest_queued) {
					highest_queued = queued;
					pick_cpu = cpu;
				}
			}
		}

		if (pick_cpu != -ENOENT)
			pick_dsq_id = cpu_to_dsq(pick_cpu);
	}

	return pick_dsq_id;
}

/*
 * ca_home_dsq - the home (preferred-LLC) DSQ of @p, if usable.
 *
 * Returns cpdom_to_dsq(preferred) only when the preferred domain is known
 * and valid and @p's affinity actually reaches at least one CPU of that
 * domain; returns 0 otherwise. Zero is never a valid domain DSQ id since
 * the DSQ type bits are always non-zero. The caller supplies @p's task
 * context so no redundant lookup is needed.
 */
static __always_inline u64
ca_home_dsq(struct task_struct *p, task_ctx *taskc)
{
	struct cpdom_ctx *cpdomc;
	struct bpf_cpumask *cpd_mask;
	u8 pref;

	pref = taskc->preferred_cpdom_id;
	if (pref == LAVD_CA_UNSET_CPDOM)
		return 0;

	cpdomc = MEMBER_VPTR(cpdom_ctxs, [pref]);
	if (!cpdomc || !cpdomc->is_valid)
		return 0;

	cpd_mask = MEMBER_VPTR(cpdom_cpumask, [pref]);
	if (!cpd_mask ||
	    !bpf_cpumask_intersects(p->cpus_ptr, cast_mask(cpd_mask)))
		return 0;

	return cpdom_to_dsq((u64)pref);
}

/*
 * ca_dsq_move - move @p from DSQ iteration @it to @dsq_id using the
 * protocol the kernel expects for the target DSQ type.
 *
 * The two move kfuncs must not be mixed: a vtime (PRIQ) move into a
 * built-in DSQ triggers scx_error("cannot use vtime ordering for built-in
 * DSQs") — an outright scheduler abort — and a plain FIFO move into a
 * vtime-ordered user DSQ mixes ordering classes, which the kernel also
 * rejects. So a domain (user, vtime) DSQ must be moved to with
 * set_slice + set_vtime + move_vtime, and SCX_DSQ_LOCAL must be moved to
 * with the plain FIFO move. The explicit set_slice() also clears any
 * stale slice override a previous move on the same iterator may have
 * staged in the iterator.
 */
static __always_inline bool
ca_dsq_move(struct bpf_iter_scx_dsq *it, struct task_struct *p, u64 dsq_id)
{
	if (dsq_id == SCX_DSQ_LOCAL) {
		scx_bpf_dsq_move_set_slice(it, p->scx.slice);
		return scx_bpf_dsq_move(it, p, SCX_DSQ_LOCAL, 0);
	}

	scx_bpf_dsq_move_set_slice(it, p->scx.slice);
	scx_bpf_dsq_move_set_vtime(it, p->scx.dsq_vtime);
	return scx_bpf_dsq_move_vtime(it, p, dsq_id, 0);
}

/*
 * ca_head_is_home - should the head task of @dsq_id be left at its home
 * domain?
 *
 * Peeks only the DSQ head: if the head's preferred domain is
 * @cpdomc_pick itself (the task is "at home") and the source domain still
 * has more than LAVD_CA_KEEP_REQ of its capacity unused (util < 95%), the
 * task's cache affinity wins over load balancing and the caller must not
 * steal from this DSQ. Only once the domain is virtually saturated may
 * its home tasks be stolen out.
 */
static __attribute__((noinline)) bool
ca_head_is_home(u64 dsq_id, struct cpdom_ctx *cpdomc_pick)
{
	struct task_struct *p;
	task_ctx *taskc;

	if (!cache_aware)
		return false;

	p = __COMPAT_scx_bpf_dsq_peek(dsq_id);
	if (!p)
		return false;

	taskc = get_task_ctx(p);
	if (!taskc)
		return false;

	return taskc->preferred_cpdom_id != LAVD_CA_UNSET_CPDOM &&
	       (u64)taskc->preferred_cpdom_id == cpdomc_pick->id &&
	       cpdom_headroom_above(cpdomc_pick, LAVD_CA_KEEP_REQ);
}

/*
 * steal_wanderer - cache-aware DSQ scan for "wanderer" tasks.
 *
 * Walks up to LAVD_CA_STEAL_SEARCH_DEPTH tasks in @dsq_id looking for
 * tasks whose preferred LLC domain differs from @cpdomc_pick (the source
 * domain), i.e., tasks that do not belong here. Outcomes:
 *
 *   > 0  A wanderer heading home to @cpdomc (the stealer) was moved to
 *        SCX_DSQ_LOCAL and both budgets were decremented. The caller
 *        should return true (steal done).
 *   = 0  Nothing was taken for the stealer itself; the caller falls
 *        through — ca_head_is_home() may then intercept, otherwise the
 *        normal head-of-DSQ consume decides. Side effect: zero or more
 *        wanderers heading to third domains may have been redirected
 *        back to their home domain DSQ ("send-back"), each decrementing
 *        only the stealee (egress) budget of the source domain.
 *
 * Details:
 *   - Head fast path: the DSQ head is peeked first. When it is at home
 *     or untracked, the iterator is not even created — those cases are
 *     handled by the caller's interception/normal consume — so an
 *     undisturbed DSQ costs one peek, not an iterator setup plus scan.
 *   - LOCAL target affinity check: a task moved to SCX_DSQ_LOCAL must be
 *     able to run on the stealing CPU, otherwise the kernel's dispatch
 *     path aborts the scheduler. Checked before every LOCAL move.
 *   - Send-back redirection: a wanderer whose home is a third domain —
 *     or one that cannot run on this CPU even though it is heading
 *     home — is moved to its home domain's vtime DSQ instead, so it
 *     converges to its warm LLC without our CPU having to run it.
 *
 * Uses a plain for loop (not bpf_loop) to stay within the BPF 8-frame
 * call depth limit. noinline so the verifier analyses it from a single
 * abstract call site.
 */
static __attribute__((noinline)) int
steal_wanderer(u64 dsq_id, struct cpdom_ctx *cpdomc, struct cpdom_ctx *cpdomc_pick)
{
	struct bpf_iter_scx_dsq it;
	task_ctx *picked_taskc = NULL;
	struct task_struct *head;
	bool picked = false;
	u32 cpu_cur;
	int k;

	/* Head fast path. */
	head = __COMPAT_scx_bpf_dsq_peek(dsq_id);
	if (!head)
		return 0;

	picked_taskc = get_task_ctx(head);
	if (!picked_taskc)
		return 0;

	if (picked_taskc->preferred_cpdom_id == cpdomc_pick->id ||
	    picked_taskc->preferred_cpdom_id == LAVD_CA_UNSET_CPDOM)
		return 0;	/* head at home or untracked */

	/* The head is a wanderer: scan (head first) for actionable tasks. */
	if (bpf_iter_scx_dsq_new(&it, dsq_id, 0) != 0) {
		bpf_iter_scx_dsq_destroy(&it);
		return 0;
	}

	cpu_cur = bpf_get_smp_processor_id();

	for (k = 0; k < LAVD_CA_STEAL_SEARCH_DEPTH; k++) {
		struct task_struct *p;
		task_ctx *tc;
		u64 home_dsq;

		p = bpf_iter_scx_dsq_next(&it);
		if (!p)
			break;

		tc = get_task_ctx(p);
		if (!tc)
			continue;

		/* At home here, or not tracked: leave for the normal path. */
		if (tc->preferred_cpdom_id == cpdomc_pick->id ||
		    tc->preferred_cpdom_id == LAVD_CA_UNSET_CPDOM)
			continue;

		/*
		 * A wanderer heading home to us: take it on this CPU,
		 * after checking it can actually run here.
		 */
		if (tc->preferred_cpdom_id == cpdomc->id &&
		    bpf_cpumask_test_cpu(cpu_cur, p->cpus_ptr)) {
			if (ca_dsq_move(&it, p, SCX_DSQ_LOCAL)) {
				picked = true;
				picked_taskc = tc;
				break;
			}
			continue;	/* raced away; keep scanning */
		}

		/*
		 * A wanderer heading elsewhere (or home to us but not
		 * runnable here): send it back to its home domain's
		 * vtime DSQ. It left the source domain, so only the
		 * stealee (egress) budget is decremented.
		 */
		home_dsq = ca_home_dsq(p, tc);
		if (home_dsq && ca_dsq_move(&it, p, home_dsq) &&
		    !no_fast_lb)
			decrement_stealee_budget(cpdomc_pick,
						 task_load_metric(tc));
	}
	bpf_iter_scx_dsq_destroy(&it);

	if (picked) {
		u64 task_load = no_fast_lb ? 0 : task_load_metric(picked_taskc);

		if (no_fast_lb) {
			WRITE_ONCE(cpdomc_pick->is_stealee, false);
			WRITE_ONCE(cpdomc->is_stealer, false);
		} else {
			decrement_stealee_budget(cpdomc_pick, task_load);
			decrement_stealer_budget(cpdomc, task_load);
		}
		return 1;
	}

	/*
	 * Our CPU still needs a task (redirections do not run here):
	 * fall through so the caller can intercept or consume.
	 */
	return 0;
}

/*
 * try_to_steal_task iterates over neighbor domains in distance order.
 *
 * A single flat bpf_loop over LAVD_CPDOM_MAX_DIST × LAVD_CPDOM_MAX_NR
 * replaces the previous two nested bpf_loop callbacks.  Flattening saves one
 * call frame compared to the nested approach (Rule 3 — 8-frame limit):
 *
 *   lavd_dispatch(0) → consume_task(1) → try_to_steal_task(2)
 *   → try_steal_flat_cb(3) → steal_wanderer(4)
 *   → __get_task_ctx_slowpath(5) → scx_task_data(6)
 *   → scx_arena_subprog_init(7)          ← 8 frames, at limit
 *
 * force_to_steal_task() follows the same flat-callback pattern
 * (force_steal_flat_cb): a plain nested for loop inlined into
 * ops.dispatch() makes the verifier's pending-state stack of the
 * lavd_dispatch walk exceed the 8192 jump-sequence limit once the
 * cache-aware insertion points are in the loop body, while the
 * bpf_loop callback is explored as a bounded, separate state subtree.
 *
 * Within the callback:
 *   i = idx / LAVD_CPDOM_MAX_NR   (distance level)
 *   j = idx % LAVD_CPDOM_MAX_NR   (neighbor index within that level)
 *
 * LAVD_CPDOM_MAX_NR == 128 is a power of 2, so the compiler emits shift/mask
 * and the verifier derives i < LAVD_CPDOM_MAX_DIST, j < LAVD_CPDOM_MAX_NR
 * from the explicit idx bound check (Rule 4).
 *
 * cpdomc is re-derived via MEMBER_VPTR inside the callback rather than stored
 * as a pointer in ctx — bpf_loop loses map-value type on pointer loads (Rule 5).
 */
struct try_steal_flat_ctx {
	u64  cpdomc_id;
	bool stolen;
};

static int try_steal_flat_cb(u32 idx, void *data)
{
	struct try_steal_flat_ctx *ctx = data;
	struct cpdom_ctx *cpdomc, *cpdomc_pick;
	s64 cpdom_id, nr_nbr;
	u64 dsq_id, task_load;
	u32 i, j;

	/* Rule 4: bpf_loop does not constrain idx in the verifier. */
	if (idx >= LAVD_CPDOM_MAX_DIST * LAVD_CPDOM_MAX_NR)
		return 1;

	i = idx / LAVD_CPDOM_MAX_NR;   /* verifier derives: i < LAVD_CPDOM_MAX_DIST */
	j = idx % LAVD_CPDOM_MAX_NR;   /* verifier derives: j < LAVD_CPDOM_MAX_NR   */

	cpdomc = MEMBER_VPTR(cpdom_ctxs, [ctx->cpdomc_id]);
	if (!cpdomc)
		return 1;

	/* i < LAVD_CPDOM_MAX_DIST so nr_neighbors[i] is in-bounds. */
	nr_nbr = cpdomc->nr_neighbors[i];

	if (j == 0) {
		/* Mirror outer-loop break: no neighbors at this distance → stop. */
		if (nr_nbr == 0)
			return 1;

		/*
		 * Mirror outer-loop prob check: after exhausting the previous
		 * distance without stealing, stop farther search with increasing
		 * probability.
		 */
		if (i > 0 && !prob_x_out_of_y(1, LAVD_CPDOM_MIG_PROB_FT))
			return 1;
	}

	/* Skip j values beyond the actual neighbor count for this distance. */
	if ((s64)j >= nr_nbr)
		return 0;

	cpdom_id = get_neighbor_id(cpdomc, i, j);
	if (cpdom_id < 0)
		return 0;

	cpdomc_pick = MEMBER_VPTR(cpdom_ctxs, [cpdom_id]);
	if (!cpdomc_pick) {
		scx_bpf_error("Failed to lookup cpdom_ctx for %lld", cpdom_id);
		return 1;
	}

	if (!READ_ONCE(cpdomc_pick->is_stealee) || !cpdomc_pick->is_valid)
		return 0;

	if (READ_ONCE(cpdomc_pick->stealee_budget_invr) <= 0)
		return 0;

	dsq_id = pick_most_loaded_dsq(cpdomc_pick);

	/*
	 * Cache-aware steal, gated by the overhead door: only domains that
	 * received an enqueue within the last sys_stat interval
	 * (ca_tracked_active) can hold tracked tasks worth scanning.
	 */
	if (cache_aware && READ_ONCE(cpdomc_pick->ca_tracked_active)) {
		/*
		 * Steal a wanderer heading home to us, or send wanderers
		 * back to their home domains (budgets decremented inside).
		 */
		if (steal_wanderer(dsq_id, cpdomc, cpdomc_pick) > 0) {
			ctx->stolen = true;
			return 1;
		}

		/*
		 * Fall-through interception: the head task is at home in
		 * the source domain and the domain still has headroom —
		 * keep it there and try the next neighbor.
		 */
		if (ca_head_is_home(dsq_id, cpdomc_pick))
			return 0;
	}

	/*
	 * TOCTOU: the task peeked here may not be the one actually consumed
	 * by consume_dsq() below; budget is a hint and self-corrects each round.
	 */
	task_load = no_fast_lb ? 0 : dsq_peek_task_load(dsq_id);

	if (consume_dsq(cpdomc_pick, dsq_id)) {
		if (no_fast_lb) {
			WRITE_ONCE(cpdomc_pick->is_stealee, false);
			WRITE_ONCE(cpdomc->is_stealer, false);
		} else {
			decrement_stealee_budget(cpdomc_pick, task_load);
			decrement_stealer_budget(cpdomc, task_load);
		}
		ctx->stolen = true;
		return 1;
	}

	return 0;
}

static __attribute__((noinline)) bool try_to_steal_task(struct cpdom_ctx *cpdomc)
{
	struct try_steal_flat_ctx ctx = {
		.cpdomc_id = cpdomc->id,
		.stolen    = false,
	};

	if (!cpdomc->nr_active_cpus)
		return false;

	if (no_fast_lb &&
	    !prob_x_out_of_y(1, cpdomc->nr_active_cpus * LAVD_CPDOM_MIG_PROB_FT))
		return false;

	bpf_loop(LAVD_CPDOM_MAX_DIST * LAVD_CPDOM_MAX_NR, try_steal_flat_cb, &ctx, 0);

	return ctx.stolen;
}

/*
 * Cache-aware noinline wrappers for the force-steal path.
 *
 * Both are called from force_steal_flat_cb(), a bpf_loop callback verified
 * as its own state subtree. Keeping them noinline is still essential: the
 * force path folds a LAVD_CPDOM_MAX_DIST x LAVD_CPDOM_MAX_NR neighbor
 * traversal into ops.dispatch(), and inlining the cache-aware peek/scan
 * bodies there explodes the verifier's pending-state stack past the 8192
 * jump-sequence limit (BPF_COMPLEXITY_LIMIT_JMP_SEQ) for the lavd_dispatch
 * program — measured on the dispatch path as roughly 2x dispatch overhead
 * when the scans ran ungated. The wrappers stay a cheap gated branch —
 * skipped entirely unless the source domain was recently tracked
 * (ca_tracked_active) — on every iteration.
 */
static __attribute__((noinline)) bool
ca_force_head_is_home(u64 dsq_id, struct cpdom_ctx *cpdomc_pick)
{
	if (!cache_aware || !READ_ONCE(cpdomc_pick->ca_tracked_active))
		return false;
	return ca_head_is_home(dsq_id, cpdomc_pick);
}

static __attribute__((noinline)) bool
ca_force_home(u64 dsq_id, struct cpdom_ctx *cpdomc,
	      struct cpdom_ctx *cpdomc_pick)
{
	if (!cache_aware || !READ_ONCE(cpdomc_pick->ca_tracked_active))
		return false;
	return steal_wanderer(dsq_id, cpdomc, cpdomc_pick) > 0;
}

/*
 * Force-steal flat callback: idx -> (i, j) decomposition over
 * LAVD_CPDOM_MAX_DIST x LAVD_CPDOM_MAX_NR neighbors, identical to
 * try_steal_flat_cb() (see the comment there). force_to_steal_task()
 * used to be a plain nested for loop inlined into ops.dispatch(); that
 * was fine while its per-iteration body was small, but the cache-aware
 * insertion points below made the pending-state stack of the
 * lavd_dispatch verification walk exceed the 8192 jump-sequence limit.
 * The flat bpf_loop callback restores a bounded, separately explored
 * verification subtree.
 */
struct force_steal_flat_ctx {
	u64  cpdomc_id;
	bool stolen;
};

static int force_steal_flat_cb(u32 idx, void *data)
{
	struct force_steal_flat_ctx *ctx = data;
	struct cpdom_ctx *cpdomc, *cpdomc_pick;
	s64 cpdom_id, nr_nbr;
	u64 dsq_id, task_load;
	u32 i, j;

	/* bpf_loop does not constrain idx in the verifier. */
	if (idx >= LAVD_CPDOM_MAX_DIST * LAVD_CPDOM_MAX_NR)
		return 1;

	i = idx / LAVD_CPDOM_MAX_NR;   /* i < LAVD_CPDOM_MAX_DIST */
	j = idx % LAVD_CPDOM_MAX_NR;   /* j < LAVD_CPDOM_MAX_NR   */

	cpdomc = MEMBER_VPTR(cpdom_ctxs, [ctx->cpdomc_id]);
	if (!cpdomc)
		return 1;

	nr_nbr = cpdomc->nr_neighbors[i];

	if (j == 0) {
		/* Mirror outer-loop break: no neighbors at this distance. */
		if (nr_nbr == 0)
			return 1;
	}

	/* Skip j values beyond the actual neighbor count for this distance. */
	if ((s64)j >= nr_nbr)
		return 0;

	cpdom_id = get_neighbor_id(cpdomc, i, j);
	if (cpdom_id < 0)
		return 0;

	cpdomc_pick = MEMBER_VPTR(cpdom_ctxs, [cpdom_id]);
	if (!cpdomc_pick) {
		scx_bpf_error("Failed to lookup cpdom_ctx for %lld", cpdom_id);
		return 1;
	}

	if (!cpdomc_pick->is_valid)
		return 0;

	dsq_id = pick_most_loaded_dsq(cpdomc_pick);

	/*
	 * Insertion point 1: opportunistically steal a wanderer heading
	 * home to this domain; budgets are decremented inside
	 * steal_wanderer().
	 */
	if (ca_force_home(dsq_id, cpdomc, cpdomc_pick)) {
		ctx->stolen = true;
		return 1;
	}

	/*
	 * Insertion point 2: keep the head task at its home domain when it
	 * belongs there and the domain still has headroom; try the next
	 * neighbor instead.
	 */
	if (ca_force_head_is_home(dsq_id, cpdomc_pick))
		return 0;

	/*
	 * Peek at the head task to get its size. Skip the peek when
	 * no_fast_lb is set since the budget accounting below is bypassed
	 * and the value would be unused.
	 */
	task_load = no_fast_lb ? 0 : dsq_peek_task_load(dsq_id);

	/*
	 * Force steal is unconditional for work conservation. Decrement
	 * budgets to keep the accounting consistent.
	 */
	if (consume_dsq(cpdomc_pick, dsq_id)) {
		if (!no_fast_lb) {
			decrement_stealee_budget(cpdomc_pick, task_load);
			decrement_stealer_budget(cpdomc, task_load);
		}
		ctx->stolen = true;
		return 1;
	}

	return 0;
}

static bool force_to_steal_task(struct cpdom_ctx *cpdomc)
{
	struct force_steal_flat_ctx ctx = {
		.cpdomc_id = cpdomc->id,
		.stolen    = false,
	};

	bpf_loop(LAVD_CPDOM_MAX_DIST * LAVD_CPDOM_MAX_NR,
		 force_steal_flat_cb, &ctx, 0);

	return ctx.stolen;
}

__hidden
bool consume_task(u64 cpu_dsq_id, u64 cpdom_dsq_id)
{
	struct cpdom_ctx *cpdomc;
	struct cpu_ctx *cpuc;
	u64 cpdom_turb_dsq_id;
	bool turbulent;
	struct dsq_entry dsqs[3];

	cpdomc = MEMBER_VPTR(cpdom_ctxs, [dsq_to_cpdom(cpdom_dsq_id)]);
	if (!cpdomc) {
		scx_bpf_error("Failed to lookup cpdom_ctx for %llu", dsq_to_cpdom(cpdom_dsq_id));
		return false;
	}

	cpdom_turb_dsq_id = cpdom_to_turb_dsq(dsq_to_cpdom(cpdom_dsq_id));

	/*
	 * Determine if this CPU is turbulent (high IRQ/steal time).
	 * Non-turbulent CPUs consume from all 3 DSQs.
	 * Turbulent CPUs only consume from the turbulent DSQ
	 * (which holds non-latency-critical tasks).
	 */
	cpuc = get_cpu_ctx();
	if (!cpuc)
		return false;
	turbulent = cpuc->lat_headroom < LAVD_LC_LATENCY_SENSITIVE_THRESH;

	/*
	 * If the current compute domain is a stealer, try to steal
	 * a task from any of stealee domains probabilistically.
	 */
	if (nr_cpdoms > 1 && READ_ONCE(cpdomc->is_stealer) &&
	    try_to_steal_task(cpdomc))
		goto x_domain_migration_out;

	/*
	 * Collect eligible DSQs and consume in lowest-vtime-first order.
	 * Non-turbulent CPUs always see the cpdom DSQ. Turbulent CPUs
	 * also see it when it has more queued tasks than the turbulent
	 * DSQ (to prevent starvation) or when there are no steady CPUs
	 * to drain it.
	 */
	dsqs[0] = (struct dsq_entry){ cpu_dsq_id,       U64_MAX, use_per_cpu_dsq() };
	dsqs[1] = (struct dsq_entry){ cpdom_dsq_id,     U64_MAX, use_cpdom_dsq() &&
		(!turbulent ||
		 scx_bpf_dsq_nr_queued(cpdom_dsq_id) > scx_bpf_dsq_nr_queued(cpdom_turb_dsq_id) ||
		 cpdomc->nr_steady_cpus == 0) };
	dsqs[2] = (struct dsq_entry){ cpdom_turb_dsq_id, U64_MAX, use_cpdom_dsq() };

	if (dsqs[0].eligible)
		dsqs[0].vtime = peek_dsq_vtime(dsqs[0].dsq_id);
	if (dsqs[1].eligible)
		dsqs[1].vtime = peek_dsq_vtime(dsqs[1].dsq_id);
	if (dsqs[2].eligible)
		dsqs[2].vtime = peek_dsq_vtime(dsqs[2].dsq_id);

	sort_dsqs(&dsqs[0], &dsqs[1], &dsqs[2]);

	if (dsqs[0].eligible && consume_dsq(cpdomc, dsqs[0].dsq_id))
		return true;
	if (dsqs[1].eligible && consume_dsq(cpdomc, dsqs[1].dsq_id))
		return true;
	if (dsqs[2].eligible && consume_dsq(cpdomc, dsqs[2].dsq_id))
		return true;

	/*
	 * If there is no task in the associated DSQ, traverse neighbor
	 * compute domains in distance order -- task stealing.
	 * Skip force stealing when mig_delta_pct is set (> 0) to rely
	 * only on the is_stealer/is_stealee thresholds.
	 */
	if (nr_cpdoms > 1 && mig_delta_pct == 0 && force_to_steal_task(cpdomc))
		goto x_domain_migration_out;

	return false;

	/*
	 * Task migration across compute domains happens.
	 */
x_domain_migration_out:
	return true;
}
