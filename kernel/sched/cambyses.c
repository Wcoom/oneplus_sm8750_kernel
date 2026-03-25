// SPDX-License-Identifier: GPL-2.0
/*
 * Cambyses — Context-Aware Migration Balancer Yielding Scored Entity Selection
 *
 * Scored migration selection for CFS load balancer.
 * Replaces FIFO selection with a multi-feature scoring function that evaluates
 * cache coldness, CPU lightness, voluntary switch ratio, and wakee stability.
 */

/*
 * This file is #included from fair.c (not compiled separately)
 * to access static functions: can_migrate_task(), detach_task(),
 * task_h_load(), task_util_est(), task_fits_cpu(), etc.
 */

/**************************************************************
 * Version Information:
 */

#define CAMBYSES_PROGNAME "Cambyses Migration Selector"
#define CAMBYSES_AUTHOR   "Masahito Suzuki"

#define CAMBYSES_VERSION  "0.3.0-beta4"

/* Runtime toggle — NOP-patched when disabled */
DEFINE_STATIC_KEY_TRUE(sched_cambyses);

/*
 * sysctl_cambyses_config — per-slot (signal, weight) pairs for F0..F3.
 *
 * Format: "src0 w0 src1 w1 src2 w2 src3 w3"
 *   src: 0-7  (signal function index, see CAMBYSES_SIG_* below)
 *   w:  -7..+7 (signed weight; 0 = disable slot entirely via NOP patch)
 *
 * Default: "0 2 1 -5 2 0 3 0"
 *   slot0: sig0 (exec_start delta)      × +2
 *   slot1: sig1 (runnable starvation)   × -5
 *   slot2: sig2 (io_boundness)          × 0 (disabled)
 *   slot3: sig3 (wakee_penalty)         × 0 (disabled)
 *
 * Signal index table:
 *   0: exec_start delta      — log2p1(rq_clock - exec_start)    [inline]
 *   1: runnable starvation   — (runnable_avg - util_avg) >> 4   [inline]
 *   2: io_boundness          — vol_ratio × (1 - util_frac) × 64[cached: cambyses_sig2]
 *   3: wakee_penalty         — log2p1(wakee_flips + 1)          [cached: cambyses_sig3]
 *   4: last_migrate delta    — log2p1(rq_clock - last_migrate)  [inline]
 *   5: nvcsw ratio           — nvcsw / (nvcsw + nivcsw) × 64   [cached: cambyses_sig5]
 *   6: util_avg              — util_avg >> 4 (0-64)             [inline]
 *   7: weighted_load         — util_avg × log2p1(weight) >> 10  [inline]
 */
int sysctl_cambyses_config[8] = {0, 2,  1, -5,  2, 0,  3, 0};

/* Read-only copy of the compiled-in default configuration */
const int sysctl_cambyses_config_default[8] = {0, 2,  1, -5,  2, 0,  3, 0};

/*
 * Per-weight activity keys — NOP-patched when the slot weight is 0.
 * All start FALSE; sched_cambyses_sysctl_init() enables slots with
 * non-zero default weights {2, -5, 0, 0} → w0, w1 enabled; w2, w3 disabled.
 * Updated by sched_cambyses_config_handler() on sysctl write.
 */
DEFINE_STATIC_KEY_FALSE(cambyses_w0_active);   /* config[1] = 2  */
DEFINE_STATIC_KEY_FALSE(cambyses_w1_active);   /* config[3] = -5 */
DEFINE_STATIC_KEY_FALSE(cambyses_w2_active);   /* config[5] = 0  */
DEFINE_STATIC_KEY_FALSE(cambyses_w3_active);   /* config[7] = 0  */

/*
 * Per-slot signal-selection keys — 3 bits (bit2..bit0) per slot encode
 * which of the 8 signal functions to evaluate via a static binary tree.
 * All defined as FALSE; sched_cambyses_sysctl_init() enables those that
 * correspond to non-zero bits in the default src values.
 *
 * Bit encoding for slot N:
 *   cambyses_fN_bit2 = (src >> 2) & 1
 *   cambyses_fN_bit1 = (src >> 1) & 1
 *   cambyses_fN_bit0 = (src >> 0) & 1
 */
DEFINE_STATIC_KEY_FALSE(cambyses_f0_bit0);   /* slot0 src=0: 000 */
DEFINE_STATIC_KEY_FALSE(cambyses_f0_bit1);
DEFINE_STATIC_KEY_FALSE(cambyses_f0_bit2);
DEFINE_STATIC_KEY_FALSE(cambyses_f1_bit0);   /* slot1 src=1: 001, enabled at init */
DEFINE_STATIC_KEY_FALSE(cambyses_f1_bit1);
DEFINE_STATIC_KEY_FALSE(cambyses_f1_bit2);
DEFINE_STATIC_KEY_FALSE(cambyses_f2_bit0);   /* slot2 src=2: 010, bit1 enabled */
DEFINE_STATIC_KEY_FALSE(cambyses_f2_bit1);
DEFINE_STATIC_KEY_FALSE(cambyses_f2_bit2);
DEFINE_STATIC_KEY_FALSE(cambyses_f3_bit0);   /* slot3 src=3: 011, bit0+1 enabled */
DEFINE_STATIC_KEY_FALSE(cambyses_f3_bit1);
DEFINE_STATIC_KEY_FALSE(cambyses_f3_bit2);

/*
 * Cached-signal update gate keys — NOP-patch the update call sites when
 * no active slot uses the corresponding cached signal.
 *
 * cambyses_sig3_active:  any slot with non-zero weight uses sig3
 * cambyses_ctxsw_active: any slot with non-zero weight uses sig2 or sig5
 *
 * Enabled/disabled by cambyses_update_cached_sig_keys() at init and on
 * every sysctl_cambyses_config write.
 */
DEFINE_STATIC_KEY_FALSE(cambyses_sig3_active);
DEFINE_STATIC_KEY_FALSE(cambyses_ctxsw_active);

#ifdef CONFIG_SCHED_CAMBYSES_SIMD
#ifdef CONFIG_X86_64
DEFINE_STATIC_KEY_FALSE(cambyses_has_avx2);
DEFINE_STATIC_KEY_FALSE(cambyses_has_ssse3);
DEFINE_STATIC_KEY_FALSE(cambyses_has_sse41);
#endif
#ifdef CONFIG_ARM64
DEFINE_STATIC_KEY_FALSE(cambyses_has_neon);
#endif
#endif

/*
 * log2p1_u64_u8fp2 — fixed-point log2(v+1) with 2-bit mantissa
 *
 * Returns (exponent << 2) | mantissa, giving 4× finer granularity than
 * fls64().  Based on BORE scheduler's log2p1_u64_u32fp().
 *
 * Result fits in u8 for all practical scheduler values (max ~163 for
 * nanosecond-scale deltas).
 */
static inline u8 log2p1_u64_u8fp2(u64 v)
{
	int clz, exponent;
	u8 mantissa;

	if (unlikely(!v))
		return 0;
	clz = __builtin_clzll(v);
	exponent = 64 - clz;
	mantissa = (u8)((v << clz) << 1 >> 62);
	return (u8)(exponent << 2 | mantissa);
}

/*
 * cambyses_update_sig3 — cache sig3 wakee_penalty.
 * Called from record_wakee() after modifying wakee_flips.
 * Returns immediately if no active slot uses sig3.
 */
void cambyses_update_sig3(struct task_struct *p)
{
	if (!static_branch_unlikely(&cambyses_sig3_active))
		return;
	/* Cap at fp2=128 (log2≈32, wakee_flips≈UINT_MAX). Safety clamp only. */
	p->se.cambyses_sig3 = (u8)min_t(int,
		log2p1_u64_u8fp2(p->wakee_flips + 1), 128);
}

/*
 * cambyses_update_ctxsw — cache sig5 (nvcsw_ratio) and sig2 (io_boundness).
 * Called from __schedule() as prev is being switched out (nvcsw/nivcsw
 * have just been incremented for this context switch).
 * Returns immediately if no active slot uses sig2 or sig5.
 */
void cambyses_update_ctxsw(struct task_struct *p)
{
	unsigned long total, vol_ratio, util;

	if (!static_branch_unlikely(&cambyses_ctxsw_active))
		return;

	total = p->nvcsw + p->nivcsw;

	if (unlikely(!total)) {
		p->se.cambyses_sig5 = 0;
		p->se.cambyses_sig2 = 0;
		return;
	}
	vol_ratio = (p->nvcsw >= total ? 64 : p->nvcsw * 64 / total);
	p->se.cambyses_sig5 = (u8)vol_ratio;

	util = min_t(unsigned long, READ_ONCE(p->se.avg.util_avg), 1023);
	p->se.cambyses_sig2 = (u8)(vol_ratio * (1024 - util) >> 10);
}

/*
 * Signal functions — one per signal index (0-7).
 * Time-relative signals (0, 4) take env for rq_clock_task().
 * Cached signals (2, 3, 5) read se fields directly.
 * PELT-fresh signals (1, 6) read se.avg directly.
 */
static inline int cambyses_sig0(struct task_struct *p, struct lb_env *env)
{
	return log2p1_u64_u8fp2(rq_clock_task(env->src_rq) - p->se.exec_start);
}

static inline int cambyses_sig1(struct task_struct *p, struct lb_env *env)
{
	unsigned long runnable = READ_ONCE(p->se.avg.runnable_avg);
	unsigned long util     = READ_ONCE(p->se.avg.util_avg);

	return (int)(runnable > util ? (runnable - util) >> 4 : 0);
}

static inline int cambyses_sig2(struct task_struct *p, struct lb_env *env)
{
	return (int)p->se.cambyses_sig2;
}

static inline int cambyses_sig3(struct task_struct *p, struct lb_env *env)
{
	return (int)p->se.cambyses_sig3;
}

static inline int cambyses_sig4(struct task_struct *p, struct lb_env *env)
{
	return log2p1_u64_u8fp2(sched_clock_cpu(env->src_cpu) -
				 p->se.cambyses_last_migrate);
}

static inline int cambyses_sig5(struct task_struct *p, struct lb_env *env)
{
	return (int)p->se.cambyses_sig5;
}

static inline int cambyses_sig6(struct task_struct *p, struct lb_env *env)
{
	return (int)(READ_ONCE(p->se.avg.util_avg) >> 4);
}

static inline int cambyses_sig7(struct task_struct *p, struct lb_env *env)
{
	return (int)((unsigned long)READ_ONCE(p->se.avg.util_avg) *
		     (unsigned long)log2p1_u64_u8fp2(p->se.load.weight))
		    >> SCHED_FIXEDPOINT_SHIFT;
}

/*
 * CAMBYSES_SLOT_SIGNAL — binary decision tree over 3 static keys.
 * Selects one of 8 signal functions for slot N without runtime branches
 * (keys are NOP-patched in steady state).
 *
 *   bit2 bit1 bit0  → signal
 *    0    0    0    → sig0
 *    0    0    1    → sig1
 *    0    1    0    → sig2
 *    0    1    1    → sig3
 *    1    0    0    → sig4
 *    1    0    1    → sig5
 *    1    1    0    → sig6
 *    1    1    1    → sig7
 */
#define CAMBYSES_SLOT_SIGNAL(n, p, env)					\
	(static_branch_unlikely(&cambyses_f##n##_bit2)			\
	 ? (static_branch_unlikely(&cambyses_f##n##_bit1)		\
	    ? (static_branch_unlikely(&cambyses_f##n##_bit0)		\
	       ? cambyses_sig7(p, env) : cambyses_sig6(p, env))	\
	    : (static_branch_unlikely(&cambyses_f##n##_bit0)		\
	       ? cambyses_sig5(p, env) : cambyses_sig4(p, env)))	\
	 : (static_branch_unlikely(&cambyses_f##n##_bit1)		\
	    ? (static_branch_unlikely(&cambyses_f##n##_bit0)		\
	       ? cambyses_sig3(p, env) : cambyses_sig2(p, env))	\
	    : (static_branch_unlikely(&cambyses_f##n##_bit0)		\
	       ? cambyses_sig1(p, env) : cambyses_sig0(p, env))))

/*
 * prefetch_migration_task — prefetch task_struct cache lines that
 * can_migrate_task() + score_task_cambyses() will access.
 *
 * On in-order CPUs (Cortex-A55, Atom Bonnell, RISC-V), the fields
 * accessed by scoring span multiple separate cache lines.  Without
 * OOO execution to overlap the loads, each miss serializes (~400cy).
 * Issuing prefetches one iteration ahead hides most of that latency.
 *
 * F2 (migration residency) reads se.cambyses_last_migrate, near
 * nr_migrations — may be a separate cache line.
 * F3 (wakee_penalty) is cached in se.cambyses_sig3, co-located with
 * group_node on the same cache line — no prefetch needed.
 *
 * On modern OOO CPUs (Zen 4, Golden Cove), this is effectively free:
 * prefetch instructions for already-in-flight loads are NOPs.
 */
static inline void prefetch_migration_task(struct task_struct *p)
{
	/* exec_start — same cache line as group_node; benefits can_migrate_task too */
	prefetch(&p->se.exec_start);
	/* se.avg: util_avg, runnable_avg (sig1,6,7) — separate cache line */
	prefetch(&p->se.avg.util_avg);
	/* cpus_ptr — always needed by can_migrate_task */
	prefetch(&p->cpus_ptr);
	/* cambyses_last_migrate — near nr_migrations (sig4) */
	prefetch(&p->se.cambyses_last_migrate);
	/* cambyses_sig2/sig3/sig5 — same cache line as group_node, no prefetch needed */
}

/*
 * score_task_cambyses — compute migration suitability score for a task.
 *
 * score = Σ config[2i+1] × signal(config[2i], p, env)  for i in 0..3
 *
 * Each slot's signal is selected at NOP-patch speed via a 3-bit static
 * binary tree (CAMBYSES_SLOT_SIGNAL).  The slot is skipped entirely when
 * its weight is 0 (cambyses_wN_active NOP-patched).  Score fits in s16.
 */
static s16 score_task_cambyses(struct task_struct *p, struct lb_env *env)
{
	int score = 0;

	if (static_branch_unlikely(&cambyses_w0_active))
		score += sysctl_cambyses_config[1] * CAMBYSES_SLOT_SIGNAL(0, p, env);
	if (static_branch_unlikely(&cambyses_w1_active))
		score += sysctl_cambyses_config[3] * CAMBYSES_SLOT_SIGNAL(1, p, env);
	if (static_branch_unlikely(&cambyses_w2_active))
		score += sysctl_cambyses_config[5] * CAMBYSES_SLOT_SIGNAL(2, p, env);
	if (static_branch_unlikely(&cambyses_w3_active))
		score += sysctl_cambyses_config[7] * CAMBYSES_SLOT_SIGNAL(3, p, env);

	return (s16)score;
}

/*
 * check_imbalance_cambyses — coarse filter for Phase 1 candidate inclusion
 *
 * Mirrors the existing detach_tasks() switch logic but does NOT consume
 * imbalance (that happens in Phase 3 after sorting by score).
 */
static bool check_imbalance_cambyses(struct task_struct *p,
				     struct lb_env *env)
{
	switch (env->migration_type) {
	case migrate_load: {
		unsigned long load = max_t(unsigned long, task_h_load(p), 1);

		if (sched_feat(LB_MIN) &&
		    load < 16 && !env->sd->nr_balance_failed)
			return false;
		if (shr_bound(load, env->sd->nr_balance_failed) > env->imbalance)
			return false;
		return true;
	}
	case migrate_util: {
		unsigned long util = task_util_est(p);

		if (shr_bound(util, env->sd->nr_balance_failed) > env->imbalance)
			return false;
		return true;
	}
	case migrate_task:
		return true;
	case migrate_misfit:
		return !task_fits_cpu(p, env->src_cpu);
	}
	return false;
}

/*
 * consume_imbalance_cambyses — deduct task cost from imbalance budget
 *
 * Called in Phase 3 for each detached task, in score-descending order.
 * Matches Vanilla behavior: allows imbalance to go negative on the last task.
 */
static void consume_imbalance_cambyses(struct task_struct *p,
				       struct lb_env *env)
{
	switch (env->migration_type) {
	case migrate_load:
		env->imbalance -= max_t(unsigned long, task_h_load(p), 1);
		break;
	case migrate_util:
		env->imbalance -= task_util_est(p);
		break;
	case migrate_task:
		env->imbalance--;
		break;
	case migrate_misfit:
		env->imbalance = 0;
		break;
	}
}

#ifdef CONFIG_SCHED_CAMBYSES_SIMD

/*
 * cambyses_simd_begin/end — FPU/FPSIMD context management for
 * IRQ-disabled scheduler context.
 *
 * x86: uses kernel_fpu_begin/end (handles save, lazy restore, and
 *      FPU register cache invalidation).
 *
 * ARM64: kernel_neon_begin() BUG_ON's with irqs_disabled(), so we
 *        use fpsimd_save_and_flush_cpu_state() which saves the user
 *        FPSIMD/SVE/SME state and invalidates the CPU's register
 *        cache.  If TIF_FOREIGN_FPSTATE is already set (common case
 *        after context switch), the state is already saved — cost ≈ 0.
 */

#ifdef CONFIG_X86_64
#include <asm/fpu/api.h>

static __always_inline void cambyses_simd_begin(void)
{
	kernel_fpu_begin();
}

static __always_inline void cambyses_simd_end(void)
{
	kernel_fpu_end();
}
#endif /* CONFIG_X86_64 */

#ifdef CONFIG_ARM64
#include <asm/fpsimd.h>

static __always_inline void cambyses_simd_begin(void)
{
	fpsimd_save_and_flush_cpu_state();
}

static __always_inline void cambyses_simd_end(void)
{
	/* Nothing — lazy restore via fpsimd_restore_current_state() */
}
#endif /* CONFIG_ARM64 */

#endif /* CONFIG_SCHED_CAMBYSES_SIMD */


/*
 * argmax_scores — find index of highest score via 4-way parallel reduction.
 *
 * Four independent accumulators break the serial dependency chain so the
 * OOO engine can overlap 4 CMP+CMOV streams in parallel.  A 3-comparison
 * tree merge produces the global best.  GPR-only — no FPU context needed.
 *
 * For 32 candidates: ~27-43 cycles vs ~104 for serial scan.
 * Repeated extraction (K times) costs O(K*n) — for typical K=1..4
 * this is 5-20x faster than a full sort.
 */
static __always_inline int argmax_scores(const s16 *scores, int nr_cands)
{
	s16 bv0 = S16_MIN, bv1 = S16_MIN, bv2 = S16_MIN, bv3 = S16_MIN;
	int bi0 = 0, bi1 = 0, bi2 = 0, bi3 = 0;
	int j;

	for (j = 0; j + 3 < nr_cands; j += 4) {
		if (scores[j]     > bv0) { bv0 = scores[j];     bi0 = j;     }
		if (scores[j + 1] > bv1) { bv1 = scores[j + 1]; bi1 = j + 1; }
		if (scores[j + 2] > bv2) { bv2 = scores[j + 2]; bi2 = j + 2; }
		if (scores[j + 3] > bv3) { bv3 = scores[j + 3]; bi3 = j + 3; }
	}
	/* Handle 0–3 remaining elements in chain 0 */
	for (; j < nr_cands; j++) {
		if (scores[j] > bv0) { bv0 = scores[j]; bi0 = j; }
	}
	/* Tree merge: 3 comparisons */
	if (bv1 > bv0) { bv0 = bv1; bi0 = bi1; }
	if (bv3 > bv2) { bv2 = bv3; bi2 = bi3; }
	if (bv2 > bv0) { bi0 = bi2; }
	return bi0;
}

/*
 * detach_tasks_cambyses — scored migration selection for Pull path
 *
 * Called from detach_tasks() when sched_cambyses is active.
 * Operates under rq_lock_irqsave (inherited from caller).
 *
 * Phase 1: Sample candidates from cfs_tasks, score each one
 * Phase 2: Repeated argmax extraction (SIMD or scalar), consuming
 *          imbalance budget
 */
static int detach_tasks_cambyses(struct lb_env *env)
{
	struct list_head *tasks = &env->src_rq->cfs_tasks;
	struct cambyses_candidate cands[SCHED_NR_MIGRATE_BREAK];
#ifdef CONFIG_SCHED_CAMBYSES_SIMD
	s16 scores[CAMBYSES_SIMD_SCORES_SIZE] = {
		[0 ... CAMBYSES_SIMD_SCORES_SIZE - 1] = S16_MIN
	};
#else
	s16 scores[SCHED_NR_MIGRATE_BREAK];
#endif
	LIST_HEAD(cand_tasks);
	int nr_cands = 0;
	int detached = 0;
	struct task_struct *p;

	/*
	 * Phase 1: Sampling — collect eligible candidates and score them.
	 *
	 * Mirrors the loop structure of vanilla detach_tasks():
	 * same loop_max, loop_break, idle checks.
	 *
	 * Accepted candidates are moved to cand_tasks (off cfs_tasks)
	 * to prevent the list rotation from re-collecting the same task
	 * as the loop wraps around.  Skipped tasks stay on cfs_tasks.
	 */
	while (!list_empty(tasks)) {
		if (env->idle && env->src_rq->nr_running <= 1)
			break;

		env->loop++;
		if (env->loop > env->loop_max)
			break;
		if (env->loop > env->loop_break) {
			env->loop_break += SCHED_NR_MIGRATE_BREAK;
			env->flags |= LBF_NEED_BREAK;
			break;
		}

		p = list_last_entry(tasks, struct task_struct, se.group_node);

		/*
		 * Prefetch next candidate (one-ahead).  We scan from
		 * the tail, so the next candidate is prev in the list.
		 * On in-order CPUs this hides ~60% of DRAM stall time
		 * by overlapping the prefetch with current task scoring.
		 * On OOO CPUs the hardware already parallelizes the
		 * loads, so these prefetches are essentially free NOPs.
		 */
		if (p->se.group_node.prev != tasks)
			prefetch_migration_task(
				list_prev_entry(p, se.group_node));

		if (!can_migrate_task(p, env)) {
			list_move(&p->se.group_node, tasks);
			continue;
		}

		if (!check_imbalance_cambyses(p, env))
			goto skip;

		/* Score and collect */
		cands[nr_cands].p = p;
		scores[nr_cands] = score_task_cambyses(p, env);
		nr_cands++;

		/*
		 * Remove from cfs_tasks so it cannot be re-scanned.
		 * detach_task() in Phase 3 will list_del this node;
		 * non-detached candidates are spliced back below.
		 */
		list_move(&p->se.group_node, &cand_tasks);

		if (nr_cands >= SCHED_NR_MIGRATE_BREAK)
			break;
		continue;
skip:
		list_move(&p->se.group_node, tasks);
	}

	if (!nr_cands) {
		/* cand_tasks is empty here — nothing to splice back */
		return 0;
	}

	/*
	 * Phase 2: Repeated argmax extraction, consuming imbalance budget.
	 *
	 * Repeatedly extract the single best candidate in O(n) and mark
	 * it consumed.  For typical K=1..4, this is 5-20x faster than
	 * a full sort (O(n log²n)).
	 *
	 * SIMD path (AVX2/SSSE3/NEON): loads the entire scores[] array
	 * into vector registers, uses PMAXSW reduction + broadcast compare
	 * + bitmask scan.  ~60% faster per extraction than scalar.
	 * FPU context cost ≈ 0 (TIF_NEED_FPU_LOAD already set after
	 * context switch in the common scheduler path).
	 *
	 * Scalar fallback: branchless CMP+CMOV loop, ~20 cycles / extraction.
	 *
	 * S16_MIN (-32768) is used as a tombstone: real scores range
	 * from -777 to +1746, so it can never be a valid score.
	 */
#ifdef CONFIG_SCHED_CAMBYSES_SIMD
	if (nr_cands >= CAMBYSES_SIMD_THRESHOLD) {
		int selected[SCHED_NR_MIGRATE_BREAK];
		int nr_selected = 0;
		int j;

		/*
		 * Phase 2a: SIMD selection — pick winners inside FPU context.
		 * Repeated argmax extracts candidates in score order,
		 * consuming imbalance budget.  detach_task() is deferred
		 * to Phase 2b (outside kernel_fpu) to avoid holding
		 * kernel_fpu_begin across dequeue_entity callbacks.
		 */

		cambyses_simd_begin();
		while (env->imbalance > 0) {
			int best;

#ifdef CONFIG_X86_64
			if (static_branch_likely(&cambyses_has_avx2))
				best = cambyses_simd_argmax_avx2(scores);
			else if (static_branch_likely(&cambyses_has_ssse3))
				best = cambyses_simd_argmax_ssse3(scores);
			else
				best = argmax_scores(scores, nr_cands);
#endif
#ifdef CONFIG_ARM64
			if (static_branch_likely(&cambyses_has_neon))
				best = cambyses_simd_argmax_neon(scores);
			else
				best = argmax_scores(scores, nr_cands);
#endif

			if (scores[best] == S16_MIN)
				break;

			scores[best] = S16_MIN;
			selected[nr_selected++] = best;

			consume_imbalance_cambyses(cands[best].p, env);

#ifdef CONFIG_PREEMPTION
			if (env->idle == CPU_NEWLY_IDLE)
				break;
#endif
		}
		cambyses_simd_end();

		/* Phase 2b: detach selected candidates (outside FPU context) */
		for (j = 0; j < nr_selected; j++) {
			p = cands[selected[j]].p;
			detach_task(p, env);
			list_add(&p->se.group_node, &env->tasks);
			detached++;
		}
	} else
#endif /* CONFIG_SCHED_CAMBYSES_SIMD */
	{
		while (env->imbalance > 0) {
			int best = argmax_scores(scores, nr_cands);

			if (scores[best] == S16_MIN)
				break;

			p = cands[best].p;
			scores[best] = S16_MIN;

			consume_imbalance_cambyses(p, env);
			detach_task(p, env);
			list_add(&p->se.group_node, &env->tasks);
			detached++;

#ifdef CONFIG_PREEMPTION
			if (env->idle == CPU_NEWLY_IDLE)
				break;
#endif
		}
	}

	/*
	 * Return non-detached candidates to cfs_tasks.
	 * These were removed in Phase 1 but not selected in Phase 3
	 * (imbalance exhausted or preemption break).
	 */
	list_splice(&cand_tasks, tasks);

	schedstat_add(env->sd->lb_gained[env->idle], detached);
	return detached;
}

/*
 * detach_one_task_cambyses — scored selection for Push path (active balancing)
 *
 * Replaces the FIFO "first migratable task" policy in detach_one_task().
 * Scans all migratable tasks on src_rq and selects the one with the
 * highest migration score.
 *
 * Push moves exactly 1 task, so no sort is needed — simple max scan.
 * The stop_machine context is already heavy, so scoring overhead is negligible.
 */
static struct task_struct *detach_one_task_cambyses(struct lb_env *env)
{
	struct task_struct *p, *best = NULL;
	s16 best_score = S16_MIN;

	lockdep_assert_rq_held(env->src_rq);

	list_for_each_entry_reverse(p,
			&env->src_rq->cfs_tasks, se.group_node) {
		s16 score;

		/* Prefetch next candidate (one-ahead in reverse) */
		if (p->se.group_node.prev != &env->src_rq->cfs_tasks)
			prefetch_migration_task(
				list_prev_entry(p, se.group_node));

		if (!can_migrate_task(p, env))
			continue;

		score = score_task_cambyses(p, env);
		if (score > best_score) {
			best_score = score;
			best = p;
		}
	}

	if (!best)
		return NULL;

	detach_task(best, env);
	schedstat_inc(env->sd->lb_gained[env->idle]);
	return best;
}

#ifdef CONFIG_SCHED_CAMBYSES_SIMD
#include <asm/cpufeature.h>

static int __init cambyses_init(void)
{
	const char *simd_name;

#ifdef CONFIG_X86_64
	if (boot_cpu_has(X86_FEATURE_AVX2)) {
		static_branch_enable(&cambyses_has_avx2);
		simd_name = "AVX2";
	} else if (boot_cpu_has(X86_FEATURE_SSSE3)) {
		static_branch_enable(&cambyses_has_ssse3);
		if (boot_cpu_has(X86_FEATURE_XMM4_1)) {
			static_branch_enable(&cambyses_has_sse41);
			simd_name = "SSE4.1";
		} else {
			simd_name = "SSSE3";
		}
	} else {
		simd_name = "none";
	}
#endif

#ifdef CONFIG_ARM64
	if (system_supports_fpsimd()) {
		static_branch_enable(&cambyses_has_neon);
		simd_name = "NEON";
	} else {
		simd_name = "none";
	}
#endif

	pr_info("%s v%s by %s [SIMD argmax: %s]\n",
		CAMBYSES_PROGNAME, CAMBYSES_VERSION,
		CAMBYSES_AUTHOR, simd_name);

	return 0;
}
#else /* !CONFIG_SCHED_CAMBYSES_SIMD */
static int __init cambyses_init(void)
{
	pr_info("%s v%s by %s\n",
		CAMBYSES_PROGNAME, CAMBYSES_VERSION,
		CAMBYSES_AUTHOR);

	return 0;
}
#endif /* CONFIG_SCHED_CAMBYSES_SIMD */
late_initcall(cambyses_init);

/* ======== sysctl interface ======== */

/*
 * cambyses_update_cached_sig_keys — recompute cambyses_sig3_active and
 * cambyses_ctxsw_active from the current sysctl_cambyses_config.
 *
 * Called at init and on every config write so update functions can be
 * NOP-patched out when no active slot needs the cached signals.
 */
static void cambyses_update_cached_sig_keys(void)
{
	bool need_sig3 = false, need_ctxsw = false;
	int i;

	for (i = 0; i < 4; i++) {
		int src = sysctl_cambyses_config[i * 2];
		int wt  = sysctl_cambyses_config[i * 2 + 1];

		if (!wt)
			continue;
		if (src == 3)
			need_sig3 = true;
		if (src == 2 || src == 5)
			need_ctxsw = true;
	}

	if (need_sig3)
		static_branch_enable(&cambyses_sig3_active);
	else
		static_branch_disable(&cambyses_sig3_active);

	if (need_ctxsw)
		static_branch_enable(&cambyses_ctxsw_active);
	else
		static_branch_disable(&cambyses_ctxsw_active);
}

#ifdef CONFIG_SYSCTL
static int sched_cambyses_handler(struct ctl_table *table,
					  int write, void *buffer,
					  size_t *lenp, loff_t *ppos)
{
	static u8 sched_cambyses_val;
	struct ctl_table tmp = {
		.data	= &sched_cambyses_val,
		.maxlen	= sizeof(u8),
		.mode	= table->mode,
		.extra1	= SYSCTL_ZERO,
		.extra2	= SYSCTL_ONE,
	};
	int ret;

	if (!write)
		sched_cambyses_val = static_key_enabled(&sched_cambyses);

	ret = proc_dou8vec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (sched_cambyses_val)
		static_branch_enable(&sched_cambyses);
	else
		static_branch_disable(&sched_cambyses);

	return 0;
}

/*
 * cambyses_update_src_keys — update the 3 bit static keys for one slot
 * when its signal source changes from old_src to new_src.
 */
static void cambyses_update_src_keys(int slot, int old_src, int new_src)
{
	static struct static_key_false * const bit_keys[4][3] = {
		{ &cambyses_f0_bit0, &cambyses_f0_bit1, &cambyses_f0_bit2 },
		{ &cambyses_f1_bit0, &cambyses_f1_bit1, &cambyses_f1_bit2 },
		{ &cambyses_f2_bit0, &cambyses_f2_bit1, &cambyses_f2_bit2 },
		{ &cambyses_f3_bit0, &cambyses_f3_bit1, &cambyses_f3_bit2 },
	};
	int bit;

	for (bit = 0; bit < 3; bit++) {
		int old_b = (old_src >> bit) & 1;
		int new_b = (new_src >> bit) & 1;

		if (old_b == new_b)
			continue;
		if (new_b)
			static_key_enable(&bit_keys[slot][bit]->key);
		else
			static_key_disable(&bit_keys[slot][bit]->key);
	}
}

/*
 * sched_cambyses_config_handler — sysctl write handler for sched_cambyses_config.
 *
 * Format: "src0 w0 src1 w1 src2 w2 src3 w3"
 *   src: 0-7   (signal function index)
 *   w:  -3..+3 (signed weight; 0 disables the slot via NOP patch)
 *
 * Reads via proc_dointvec into a staging buffer, validates per-element
 * ranges, then atomically applies the new config and patches static keys.
 */
static int sched_cambyses_config_handler(struct ctl_table *table,
					 int write, void *buffer,
					 size_t *lenp, loff_t *ppos)
{
	static struct static_key * const wkeys[4] = {
		&cambyses_w0_active.key,
		&cambyses_w1_active.key,
		&cambyses_w2_active.key,
		&cambyses_w3_active.key,
	};
	int tmp[8], old[8], ret, i;
	struct ctl_table tmptbl;

	if (!write) {
		/* Read path: use the real table directly */
		return proc_dointvec(table, write, buffer, lenp, ppos);
	}

	/* Write path: parse into staging buffer, validate, then apply */
	memcpy(old, sysctl_cambyses_config, sizeof(old));
	memcpy(tmp, sysctl_cambyses_config, sizeof(tmp));

	tmptbl       = *table;
	tmptbl.data  = tmp;
	ret = proc_dointvec(&tmptbl, write, buffer, lenp, ppos);
	if (ret)
		return ret;

	/* Validate: even indices are src (0-7), odd are weight (-3..+3) */
	for (i = 0; i < 4; i++) {
		if (tmp[i * 2] < 0 || tmp[i * 2] > 7)
			return -EINVAL;
		if (tmp[i * 2 + 1] < -7 || tmp[i * 2 + 1] > 7)
			return -EINVAL;
	}

	memcpy(sysctl_cambyses_config, tmp, sizeof(tmp));

	/* Update static keys for each slot */
	for (i = 0; i < 4; i++) {
		int new_src = sysctl_cambyses_config[i * 2];
		int new_wt  = sysctl_cambyses_config[i * 2 + 1];
		int old_src = old[i * 2];
		int old_wt  = old[i * 2 + 1];

		cambyses_update_src_keys(i, old_src, new_src);

		if (!old_wt != !new_wt) {
			if (new_wt)
				static_key_enable(wkeys[i]);
			else
				static_key_disable(wkeys[i]);
		}
	}

	cambyses_update_cached_sig_keys();
	return 0;
}

static struct ctl_table sched_cambyses_sysctls[] = {
	{
		.procname	= "sched_cambyses",
		.data		= NULL, /* handled by custom handler */
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler	= sched_cambyses_handler,
	},
	{
		.procname	= "sched_cambyses_config",
		.data		= sysctl_cambyses_config,
		.maxlen		= sizeof(sysctl_cambyses_config),
		.mode		= 0644,
		.proc_handler	= sched_cambyses_config_handler,
	},
	{
		.procname	= "sched_cambyses_config_default",
		.data		= (void *)sysctl_cambyses_config_default,
		.maxlen		= sizeof(sysctl_cambyses_config_default),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
	},
};

static int __init sched_cambyses_sysctl_init(void)
{
	static struct static_key * const wkeys[4] = {
		&cambyses_w0_active.key,
		&cambyses_w1_active.key,
		&cambyses_w2_active.key,
		&cambyses_w3_active.key,
	};
	int i;

	/* Enable bit keys for default src values and weight activity keys */
	for (i = 0; i < 4; i++) {
		cambyses_update_src_keys(i, 0, sysctl_cambyses_config[i * 2]);
		if (sysctl_cambyses_config[i * 2 + 1])
			static_key_enable(wkeys[i]);
	}

	cambyses_update_cached_sig_keys();
	register_sysctl_init("kernel", sched_cambyses_sysctls);
	return 0;
}
late_initcall(sched_cambyses_sysctl_init);
#endif /* CONFIG_SYSCTL */
