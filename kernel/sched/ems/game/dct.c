#include <soc/samsung/game.h>

#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>


struct dyn_cpu_throttler {
	struct kobject		*kobj;
	bool			enabled;

	bool			need_update;
	struct cpumask		req_cpus;		/* final mask passed to ecs */
	struct cpumask		fa_cpus;		/* exclude reserved/maverick cpus from feedback_cpus */
	struct cpumask		max_cpus;		/* origin max cpus by dct for 100msec */
	struct cpumask		feedback_max_cpus;	/* additional cpus by cpus load by sched tick */
	struct cpumask		need_cpus;		/* additional cpus by cpus allowed mask */
	struct cpumask		busy_cpus;		/* cpus with runnable ratio overs 120% */
	struct cpumask		busy_cpu_helper_cpus;	/* same as fa_cpus but cleared from idle balance */
	struct cpumask		maverick_cpus;		/* cpus opend by maverick task */

	struct dct_cl_load	loads[NUM_OF_MAX_CL];
	int			demand_sum;

	int			need_cpus_dl_cnt[VENDOR_NR_CPUS];
	int			maverick_task_cnt[VENDOR_NR_CPUS];

	int			runnable_margin;

	raw_spinlock_t		lock;
	raw_spinlock_t		load_update_lock;
} dct;

#define IGNORE_WIDNOW_THR	(MLT_RUNNABLE_UNIT * 30 / 100)			/* 30% */
#define RUNNBLE_CPU_MARGIN	(MLT_RUNNABLE_UNIT - IGNORE_WIDNOW_THR)		/* 70% */
#define ROUND_UP_MARGIN		(MLT_RUNNABLE_UNIT / 2)
#define IGNORE_IDX_CNT		(DCT_HIST_SIZE / 10)			/* 30% */


static const char *dct_update_reason[END_OF_DCT_UPDATE_TAG] = {
	"DCT_UPDATE",
	"DCT_FEEDBACK",
	"DCT_FEEDBACK_MAVERICK",
	"DCT_MAVERICK",
	"DCT_ALLOWED_MASK",
	"DCT_COMPILE",
	"DCT_COMPILE_DEL",
	"DCT_RESET",
};

static bool dct_enabled(void)
{
	return dct.enabled && egg_enabled();
}

void dct_enabled_control(bool enabled)
{
	dct.enabled = enabled;
}

void dct_get_available_cpus(struct cpumask *cpus)
{
	cpumask_copy(cpus, &dct.max_cpus);
}

int dct_get_demand_cpu(void)
{
	return dct.demand_sum;
}

const struct cpumask *dct_fa_cpus(void)
{
	return &dct.fa_cpus;
}

const struct cpumask *dct_busy_cpus(void)
{
	return &dct.busy_cpus;
}

const struct cpumask *dct_busy_cpu_helper_cpus(void)
{
	return &dct.busy_cpu_helper_cpus;
}
void dct_clear_helper_cpus(int cpu)
{
	cpumask_clear_cpu(cpu, &dct.busy_cpu_helper_cpus);
}

static void update_dct_cpus(int tag, bool skip)
{
	struct cpumask reserved_cpus;
	struct cpumask fa_cpus;
	struct cpumask req_cpus;

	if (unlikely(!dct.enabled))
		return;

	/* update fa cpus */
	get_reserved_cpu_mask(&reserved_cpus);
	cpumask_andnot(&fa_cpus, &dct.feedback_max_cpus, &reserved_cpus);
	cpumask_andnot(&fa_cpus, &fa_cpus, &dct.maverick_cpus);
	cpumask_copy(&dct.fa_cpus, &fa_cpus);

	/* combine req cpus */
	cpumask_or(&req_cpus, &dct.max_cpus, &dct.feedback_max_cpus);
	cpumask_or(&req_cpus, &req_cpus, &dct.need_cpus);
	cpumask_or(&req_cpus, &req_cpus, &dct.maverick_cpus);
	if (big_boosted())
		cpumask_set_cpu(get_boost_cpu(), &req_cpus);

	if (!cpumask_equal(&req_cpus, &dct.req_cpus)) {
		halo_update_busy_cpus(&req_cpus);
		if (!skip) {
			cpumask_copy(&dct.req_cpus, &req_cpus);
			ecs_request_wo_migrate("dct", &dct.req_cpus, ECS_RECOMMEND);
		}
	}

	trace_clock_set_rate("EGG DCT req_cpus", *(unsigned int *)cpumask_bits(&dct.req_cpus), tag);
	trace_dct_update(&dct.loads[0], big_boosted(), &dct.maverick_cpus, &dct.busy_cpus,
			&dct.need_cpus, &dct.fa_cpus, &reserved_cpus, &dct.feedback_max_cpus,
			&dct.max_cpus, 	&dct.req_cpus, dct_update_reason[tag]);
}

void update_dct_cpus_w_lock(int tag, bool skip)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&dct.lock, flags);

	update_dct_cpus(tag, true);

	raw_spin_unlock_irqrestore(&dct.lock, flags);
}

void reset_dct(bool enable)
{
	int cpu, idx;
	unsigned long flags;
	struct cpumask reserved_cpus;

	raw_spin_lock_irqsave(&dct.lock, flags);
	if (dct.enabled == enable)
		goto dct_reset_unlock;

	get_reserved_cpu_mask(&reserved_cpus);

	for_each_possible_cpu(cpu)
		dct.maverick_task_cnt[cpu] = 0;
	cpumask_clear(&dct.maverick_cpus);

	for (idx = 0; idx < NUM_OF_MAX_CL; idx++)
		dct.loads[idx].idx = 0;
	dct.demand_sum = cpumask_weight(cpu_possible_mask);

	cpumask_copy(&dct.max_cpus, cpu_possible_mask);
	cpumask_copy(&dct.feedback_max_cpus, cpu_possible_mask);
	cpumask_copy(&dct.req_cpus, cpu_possible_mask);
	cpumask_clear(&dct.need_cpus);
	cpumask_clear(&dct.busy_cpus);
	cpumask_clear(&dct.fa_cpus);
	cpumask_clear(&dct.busy_cpu_helper_cpus);

	ecs_request_wo_migrate("dct", &dct.req_cpus, ECS_RECOMMEND);

	trace_dct_update(&dct.loads[0], big_boosted(), &dct.maverick_cpus, &dct.busy_cpus,
			&dct.need_cpus, &dct.fa_cpus, &reserved_cpus, &dct.feedback_max_cpus,
			&dct.max_cpus, 	&dct.req_cpus, dct_update_reason[DCT_RESET]);

	halo_update_busy_cpus(NULL);

	dct_enabled_control(enable);

dct_reset_unlock:
	raw_spin_unlock_irqrestore(&dct.lock, flags);
}

bool is_maverick_cpu(int cpu)
{
	return cpumask_test_cpu(cpu, &dct.maverick_cpus);
}

/* return temperal allowed cpu for maverick task */
int dct_get_maverick_cpu(struct task_struct *p, int force_test)
{
	int cpu, mcpu = -1;
	unsigned long src_cap;

	if (unlikely(!dct_enabled()))
		return 0;

	src_cap = ems_get_origin_cpu_capacity(task_cpu(p));

	for_each_cpu(cpu, cpu_active_mask) {
		unsigned long dst_cap = ems_get_origin_cpu_capacity(cpu);

		if (src_cap >= dst_cap)
			continue;

		if (is_reserved_cpu(cpu))
			continue;

		if (cpumask_test_cpu(cpu, &dct.max_cpus))
			continue;

		if (!cpumask_test_cpu(cpu, ecs_max_cpus()))
			continue;

		mcpu = cpu;
		break;
	}

	if (mcpu == -1)
		return mcpu;

	if (cpumask_test_cpu(mcpu, cpu_fastest_mask())) {
		if ((egg_throttle_fastest_cpu() && !force_test) || big_boosted())
			return -1;
	}

	return mcpu;
}

static int get_num_of_maverick(void)
{
	return cpumask_weight(&dct.maverick_cpus);
}
#define DEFAULT_DL_CNT	10
#define MIN_MAX_UTIL	50
#define BUSY_RESERVED_CPUS_THR		(MLT_RUNNABLE_UNIT + (MLT_RUNNABLE_UNIT >> 2))
/* check resereved cpus is busy */
void dct_check_allowed_mask(void)
{
	int cpu;
	unsigned long flags;
	struct cpumask reserved_cpus, available_cpus;
	struct cpumask new_need_cpus, new_busy_cpus;

	get_reserved_cpu_mask(&reserved_cpus);
	cpumask_copy(&dct.busy_cpu_helper_cpus, &dct.fa_cpus);
	cpumask_clear(&new_need_cpus);
	cpumask_clear(&new_busy_cpus);

	/* decrease need_cpus_dl_cnt */
	for_each_cpu(cpu, cpu_active_mask)
		dct.need_cpus_dl_cnt[cpu] = max((int) 0, (dct.need_cpus_dl_cnt[cpu] -1));

	for_each_cpu(cpu, cpu_active_mask) {
		struct rq *rq = cpu_rq(cpu);

		if (rq->nr_running <= 1)
			continue;

		if (mlt_runnable(cpu) < BUSY_RESERVED_CPUS_THR)
			continue;

		cpumask_set_cpu(cpu, &new_busy_cpus);
	}

	cpumask_andnot(&available_cpus, &dct.max_cpus, &reserved_cpus);
	cpumask_andnot(&available_cpus, &available_cpus, &new_busy_cpus);

	for_each_cpu(cpu, &new_busy_cpus) {
		struct rq *rq = cpu_rq(cpu);
		struct task_struct *p;
		struct cpumask active_cpus_ptr;
		int target_cpu;;

		raw_spin_rq_lock(rq);

		list_for_each_entry(p, &rq->cfs_tasks, se.group_node) {
			int group_idx = cpuctl_task_group_idx(p);

			if (group_idx != CGROUP_TOPAPP && group_idx != CGROUP_FOREGROUND)
				continue;

			if (cpumask_intersects(&available_cpus, p->cpus_ptr))
				continue;

			cpumask_and(&active_cpus_ptr, cpu_active_mask, p->cpus_ptr);
			cpumask_andnot(&active_cpus_ptr, &active_cpus_ptr, &reserved_cpus);
			cpumask_andnot(&active_cpus_ptr, &active_cpus_ptr, &new_busy_cpus);
			target_cpu = cpumask_first(&active_cpus_ptr);

			if (invalid_cpu(target_cpu))
				continue;

			dct.need_cpus_dl_cnt[target_cpu] = DEFAULT_DL_CNT;
		}

		raw_spin_rq_unlock(rq);
	}

	for_each_cpu(cpu, cpu_active_mask) {
		if (dct.need_cpus_dl_cnt[cpu])
			cpumask_set_cpu(cpu, &new_need_cpus);
	}

	raw_spin_lock_irqsave(&dct.lock, flags);

	cpumask_copy(&dct.busy_cpus, &new_busy_cpus);

	if (cpumask_equal(&dct.need_cpus, &new_need_cpus))
		goto unlock;

	cpumask_copy(&dct.need_cpus, &new_need_cpus);
	update_dct_cpus(DCT_ALLOWED_MASK, false);

unlock:
	raw_spin_unlock_irqrestore(&dct.lock, flags);

	if(!cpumask_empty(&dct.busy_cpus))
		for_each_cpu(cpu, &dct.need_cpus)
			lb_send_ipi_wakeup(cpu);
}

static void dct_feedback_ctrl(int demand_sum)
{
	struct cpumask new_feedback_max_cpus;
	struct cpumask reserved_cpus;
	int num_cur_max, num_dct_max, up_threshold, down_threshold;
	int cpu, cnt;
	int tag = DCT_UPDATE;
	unsigned long flags;
	bool feedback_update = false;

	raw_spin_lock_irqsave(&dct.lock, flags);
	if (unlikely(!dct_enabled()))
		goto dct_feedback_unlock;

	get_reserved_cpu_mask(&reserved_cpus);
	cpumask_or(&new_feedback_max_cpus, &dct.max_cpus, &reserved_cpus);
	num_dct_max = cpumask_weight(&dct.max_cpus);
	num_cur_max = cpumask_weight(&new_feedback_max_cpus);
	num_cur_max = max(num_dct_max, num_cur_max);
	up_threshold = num_cur_max + ((num_cur_max * 3) / 10);
	down_threshold = num_cur_max - (num_cur_max / 10);
	demand_sum = max((demand_sum - cpumask_weight(&reserved_cpus)), (int) 0);

	if (demand_sum > up_threshold) {
		demand_sum -= cpumask_weight(&new_feedback_max_cpus);
		cnt = 0;
		for_each_cpu(cpu, cpu_active_mask) {
			if (cnt >= demand_sum)
				break;

			if (cpumask_test_cpu(cpu, &new_feedback_max_cpus))
				continue;

			if (cpumask_test_cpu(cpu, &reserved_cpus))
				continue;

			if (egg_throttle_fastest_cpu() &&
				cpumask_test_cpu(cpu, cpu_fastest_mask()))
				continue;

			cpumask_set_cpu(cpu, &new_feedback_max_cpus);
			cnt++;
		}

	} else if ((demand_sum < down_threshold) && (num_dct_max < num_cur_max)) {
		demand_sum = (demand_sum * 8) / 10;
		demand_sum -= cpumask_weight(&dct.max_cpus);

		cnt = 0;
		for_each_cpu(cpu, cpu_active_mask) {
			if (cnt >= demand_sum)
				break;

			if (cpumask_test_cpu(cpu, &new_feedback_max_cpus))
				continue;

			if (cpumask_test_cpu(cpu, &reserved_cpus))
				continue;

			if (egg_throttle_fastest_cpu() &&
				cpumask_test_cpu(cpu, cpu_fastest_mask()))
				continue;

			cpumask_set_cpu(cpu, &new_feedback_max_cpus);
			cnt++;
		}

	}

	if (big_boosted())
		cpumask_set_cpu(get_boost_cpu(), &new_feedback_max_cpus);

	/* check need update to apply maverick cpus */
	if(!cpumask_equal(&new_feedback_max_cpus, &dct.feedback_max_cpus)) {
		feedback_update = true;
		cpumask_copy(&dct.feedback_max_cpus, &new_feedback_max_cpus);
		tag = DCT_FEEDBACK;
	}

	if (dct.need_update)
		feedback_update ? tag = DCT_FEEDBACK_MAVERICK : DCT_MAVERICK;

	if (feedback_update || dct.need_update) {
		update_dct_cpus(tag, false);
		dct.need_update = false;
	}

dct_feedback_unlock:
	raw_spin_unlock_irqrestore(&dct.lock, flags);
}

static int get_runnable_margin(void)
{
	int op_mode = egg_get_op_mode();

	if (op_mode == OP_PERFORMANCE || op_mode == OP_EXTREME_PERFORMANCE)
		return RUNNBLE_CPU_MARGIN;
	else
		return RUNNBLE_CPU_MARGIN >> 1;
}


void dct_get_feedback(void)
{
	struct cpumask visit_mask;
	struct cpumask reserved_cpus;
	int fcpu, cpu, cl = -1;
	int runnable_margin, demand_sum = 0;
	unsigned long flags;

	if (unlikely(!dct_enabled()))
		return;

	if (unlikely(egg_stopped()))
		return;

	if (!raw_spin_trylock_irqsave(&dct.load_update_lock, flags))
		return;

	get_reserved_cpu_mask(&reserved_cpus);

	runnable_margin = get_runnable_margin();

	cpumask_clear(&visit_mask);
	for_each_cpu(fcpu, cpu_active_mask) {
		int runnable_sum = 0, util_sum = 0;
		int max_runnable = 0, max_util = 0;
		int num_by_runnable, num_by_util, idx;
		int num_cl_cpus = cpumask_weight(cpu_clustergroup_mask(fcpu));
		struct dct_cl_load *ptr_cl;
		bool has_reserved_cpu;

		if (cpumask_test_cpu(fcpu, &visit_mask))
			continue;

		/* check this cluster */
		cl++;
		cpumask_or(&visit_mask, &visit_mask, cpu_clustergroup_mask(fcpu));
		ptr_cl = &dct.loads[cl];
		idx = ptr_cl->idx;

		if (unlikely(idx >= DCT_HIST_SIZE))
			continue;

		has_reserved_cpu =
			cpumask_intersects(cpu_clustergroup_mask(fcpu), &reserved_cpus);

		/* get the cpu load */
		for_each_cpu(cpu, cpu_clustergroup_mask(fcpu)) {
			struct rq *rq = cpu_rq(cpu);
			int runnable, util;

			runnable = mlt_runnable(cpu);
			runnable_sum += runnable;
			if (runnable > max_runnable)
				max_runnable = runnable;

			util = ml_cpu_util(cpu) + cpu_util_rt(rq) + cpu_util_dl(rq);
			util = ml_cpu_util_with_irq(rq, util);
			util = min(util, (int)ems_get_actual_cpu_capacity(cpu));
			util_sum += util;

			if (has_reserved_cpu) {
				if (!cpumask_test_cpu(cpu, &reserved_cpus))
					continue;

				util = max(util, get_reserved_cpu_util(cpu));
				if (util > max_util)
					max_util = util;
			} else {
				if (util > max_util)
					max_util = util;
			}
		}

		/* when cpu in running very shortly, ignore this window */
		if (max_runnable < IGNORE_WIDNOW_THR)
			continue;

		max_util = max(max_util, (int) MIN_MAX_UTIL);
		num_by_util = (util_sum + (max_util >> 1))  / max_util;
		num_by_util = min(num_by_util, num_cl_cpus);

		num_by_runnable = (runnable_sum + runnable_margin) / MLT_RUNNABLE_UNIT;

		ptr_cl->demand_cpu_raw[idx] = max(num_by_util, num_by_runnable);
		demand_sum += ptr_cl->demand_cpu_raw[idx];

		trace_dct_feedback(cl, idx, max_util, util_sum, num_by_util,
				max_runnable, runnable_sum, runnable_margin, num_by_runnable,
				ptr_cl->demand_cpu_raw[idx]);

		ptr_cl->idx = move_idx(idx, 1, DCT_HIST_SIZE);
	}
	raw_spin_unlock_irqrestore(&dct.load_update_lock, flags);

	dct_feedback_ctrl(demand_sum);

	dct_check_allowed_mask();

}

void dct_update_maverick_enqueue(int cpu, struct task_struct *p)
{
	unsigned long flags;

	if (!dct_enabled())
		return;

	if (likely(ems_task_maverick(p) != MAVERICK_MIGRATING))
		return;

	ems_task_maverick(p) = MAVERICK_MIGRATED;

	raw_spin_lock_irqsave(&dct.lock, flags);

	dct.maverick_task_cnt[cpu]++;
	if (dct.maverick_task_cnt[cpu] == 1) {
		cpumask_set_cpu(cpu, &dct.maverick_cpus);
		update_dct_cpus(DCT_MAVERICK, true);
	}

	raw_spin_unlock_irqrestore(&dct.lock, flags);

	trace_ems_maverick(p, cpu, dct.maverick_task_cnt[cpu], "maverick_migrated");
}

void dct_update_maverick_dequeue(int cpu, struct task_struct *p)
{
	unsigned long flags;

	if (!dct_enabled())
		return;

	if (likely(ems_task_maverick(p) != MAVERICK_MIGRATED))
		return;

	ems_task_maverick(p) = MAVERICK_DISABLE;

	if (dct.maverick_task_cnt[cpu] == 0) {
		cpumask_clear_cpu(cpu, &dct.maverick_cpus);
		return;
	}
	raw_spin_lock_irqsave(&dct.lock, flags);

	dct.maverick_task_cnt[cpu]--;
	if (dct.maverick_task_cnt[cpu] == 0) {
		cpumask_clear_cpu(cpu, &dct.maverick_cpus);
		update_dct_cpus(DCT_MAVERICK, true);
	}

	raw_spin_unlock_irqrestore(&dct.lock, flags);

	trace_ems_maverick(p, cpu, dct.maverick_task_cnt[cpu], "maverick_disable");
}

/*
 * TODO: Move it to cpu load profiler
 * compute num of demand cpus before update cleave,
 * because cleave need cpu load information to decide using fastest cpus
 */
void dct_cpu_load_update(void)
{
	struct cpumask visit_mask;
	struct dct_cl_load *ptr_cl;
	int demand_sum = 0;
	int cpu, cl = -1;
	unsigned long flags;

	if (unlikely(!dct_enabled()))
		return;

	raw_spin_lock_irqsave(&dct.load_update_lock, flags);

	cpumask_clear(&visit_mask);
	for_each_cpu(cpu, cpu_active_mask) {
		int demand_cpu, sum = 0, idx, max_idx;

		if (cpumask_test_cpu(cpu, &visit_mask))
			continue;

		if (unlikely(++cl >= NUM_OF_MAX_CL))
			break;

		ptr_cl = &dct.loads[cl];
		max_idx = ptr_cl->idx;
		if (max_idx > IGNORE_IDX_CNT) {
			for (idx = 0; idx < max_idx; idx++)
				sum += ptr_cl->demand_cpu_raw[idx];
		}

		demand_cpu = ((sum * MLT_RUNNABLE_UNIT) + (max_idx * ROUND_UP_MARGIN));
		demand_cpu = (demand_cpu / MLT_RUNNABLE_UNIT) / max_idx;
		ptr_cl->demand_cpu = demand_cpu;

		if (cpumask_intersects(cpu_clustergroup_mask(cpu), cpu_active_mask))
			demand_sum += demand_cpu;

		trace_dct_update_cluster(cl, max_idx, sum, demand_cpu);

		/* check this cluster */
		cpumask_or(&visit_mask, &visit_mask, cpu_clustergroup_mask(cpu));

		/* reset window count */
		ptr_cl->idx = 0;
	}

	dct.demand_sum = demand_sum;

	raw_spin_unlock_irqrestore(&dct.load_update_lock, flags);
}

void dct_update(int op_mode)
{
	struct cpumask new_max_cpus;
	struct cpumask reserved_cpus;
	int cnt = 0, cpu, demand_sum, num_of_maverick;
	unsigned long flags;

	/* compute need_mask from feedback data */
	get_reserved_cpu_mask(&reserved_cpus);

	/* compute new_max cpus */
	cpumask_clear(&new_max_cpus);
	cpumask_set_cpu(0, &new_max_cpus);

	/* include reserved cpus */
	cpumask_or(&new_max_cpus, &new_max_cpus, &reserved_cpus);
	demand_sum = max((dct.demand_sum - cpumask_weight(&reserved_cpus)), (int) 0);

	raw_spin_lock_irqsave(&dct.lock, flags);
	if (unlikely(!dct_enabled()))
		goto dct_unlock;

	/* include maverick cpu */
	for_each_cpu_and(cpu, cpu_active_mask, ecs_max_cpus())
		if ((dct.maverick_task_cnt[cpu]) > 0)
			cpumask_set_cpu(cpu, &new_max_cpus);
	num_of_maverick = get_num_of_maverick();
	demand_sum = max((demand_sum - num_of_maverick), (int) 0);

	cnt = 0;
	for_each_cpu(cpu, cpu_active_mask) {
		if (cnt >= demand_sum)
			break;

		if (cpumask_test_cpu(cpu, &new_max_cpus))
			continue;

		cpumask_set_cpu(cpu, &new_max_cpus);
		cnt++;
	}

	if (big_boosted())
		cpumask_set_cpu(get_boost_cpu(), &new_max_cpus);


	cpumask_copy(&dct.max_cpus, &new_max_cpus);
	update_dct_cpus(DCT_UPDATE, false);

dct_unlock:
	raw_spin_unlock_irqrestore(&dct.lock, flags);

}

static int dct_emstune_notifier_call(struct notifier_block *nb,
				unsigned long val, void *v)
{
	struct emstune_set *cur_set = (struct emstune_set *)v;

	if (cur_set->level == EMS_LEVEL_PERFORMANCE)
		reset_dct(false);
	else
		reset_dct(true);

	return NOTIFY_OK;
}

static struct notifier_block dct_emstune_notifier = {
	.notifier_call = dct_emstune_notifier_call,
};

int dct_init(struct kobject *egg_kobj)
{
	raw_spin_lock_init(&dct.lock);
	raw_spin_lock_init(&dct.load_update_lock);
	ecs_request_register("dct", cpu_possible_mask, ECS_RECOMMEND);
	emstune_register_notifier(&dct_emstune_notifier);
	dct_enabled_control(true);
	reset_dct(false);

	return 0;
}
