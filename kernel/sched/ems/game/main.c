/*
 * Exynos Mobile Scheduler for Game
 *
 * Copyright (C) 2025 Samsung Electronics Co., Ltd
 */

#include <soc/samsung/game.h>
#include <soc/samsung/exynos-ufcc.h>
#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

#define	TEMP_HIST_SIZE		10
#define DEFAULT_HIGH_TEMP	85
#define DEFAULT_LOW_TEMP	60
#define DEFAULT_MAX_FREQ_THROTTLE_RATIO	40

#define EGG_DISABLED	0
#define EGG_ENABLED	1

#define EGG_RUNNING	0
#define EGG_STOP	1

#define RAMP_UP_DISABLE	0
#define RAMP_UP_ENABLE	1

struct eg_thermal_zone {
	struct cpumask cpus;
	struct thermal_zone_device *tz_dev;
	int temp[TEMP_HIST_SIZE];
};

struct egg_boost_domain {
	bool enabled;
	raw_spinlock_t	lock;
	ktime_t expired_time;
};

/* boosting base on rendering time */
#define DEFAULT_BOOST_LV	1
#define DEFAULT_BOOST_TIME	(100 * NSEC_PER_MSEC)
#define DEFAULT_BIG_BOOST_TIME	(70 * NSEC_PER_MSEC)

struct egg_boost {
	/* average frame information */
	int render_boost_thr;
	bool meet_target_temp;
	int mif_boost_lv;
	int dsu_boost_lv;
	int cur_boost_lv;
	bool cpu_rendering_delay;
	bool gpu_rendering_delay;

	/* per frame information */
	int pf_cpu_rendering_delay_cnt;
	u64 prev_frame_cpu_render_time;
	u64 prev_current_frame_time;

	int boosted_task;
	struct egg_boost_domain mif_boost;
	struct egg_boost_domain dsu_boost;
	struct egg_boost_domain big_boost;
	ktime_t 		hf_freq_expired_time;
	bool			hf_freq_boost_enabled;

	struct egg_set_over_freq *fb;
};

struct ems_game_gov {
	struct kobject			*kobj;

	bool initialized;
	int enabled;		/* 0: disabled, 1: egg*/
	int stop;			/* 0: running, 1: stop*/

	int op_mode;		/* egg gov op mode */
	int sub_mode;		/* egg gov sub op mode */
	int user_sched_policy;

	int last_idx;
	int fps[TEMP_HIST_SIZE];
	int target[TEMP_HIST_SIZE];
	int target_from[TEMP_HIST_SIZE]; /* 0: ems_profiler_fn, 1: frame_info*/
	int high_temp;
	int low_temp;
	int frame_drop;
	int demand_cpu;
	bool freq_throttle;
	bool max_freq_throttle;
	bool task_overutil;
	bool gpu_max_freq_throttle;
	int max_freq_throttle_ratio;
	int important_task;
	pid_t shader_compile_task;
	pid_t prev_shader_compile_task;

	struct egg_boost	eb;
	struct eg_thermal_zone *tz[VENDOR_NR_CPUS];
	int thermal_level;
	int num_of_cl;

	raw_spinlock_t			game_pago_lock;
	bool				game_pago_enabled;

	struct irq_work			irq_work;
	struct kthread_work		work;
	struct kthread_worker		worker;
	struct task_struct		*thread;
	struct cpumask			thread_allowed_cpus;
	ktime_t				last_jiffies;
	ktime_t				update_period;

	struct egg_task_struct		major_task[MAX_NR_MAJOR];
	int				nr_major;
	int				nr_over_running;
	int				nr_near_heaviest;
	int				nr_training;
	unsigned long			total_hist_util;
	unsigned long			total_pelt_util;

	raw_spinlock_t			reserve_lock;
	struct cpumask			reserved_cpu_mask;
	unsigned long			reserved_cpu_util[VENDOR_NR_CPUS];
	int				nr_reserved_cpus;
	int				lb_margin;

	u64				training_start_ns;
	u64				training_period;
	u64				execution_start_ns;
	u64				execution_period;
	int				execution_state;
	struct task_struct 		*execution_task;
	int				execution_invalid_cnt;
	struct cpumask			execution_fast_cpus;
	struct cpumask			execution_slow_cpus;
	int				type;
} egg;

int exec_fast_cl_idx;
int exec_slow_cl_idx;

static const char *egg_boost_reason[END_OF_BOOST_TAG] = {
	"TRY_DISABLE",
	"PF_CPU_BOOST",
	"PF_BIG_BOOST",
	"COMPILE",
	"COMPILE_DEL",
	"OVERUTIL",
	"OVERUTIL_DEL",
	"AVG_RENDER_BOOST",
	"SYSFS",
};

static void clear_egg_task_struct(struct egg_task_struct *ets);
static struct egg_task_struct *get_egg_task_struct(struct task_struct *p);

/********************************************************************************
 *				External APIs					*
 ********************************************************************************/
int egg_enabled(void)
{
	return (egg.enabled == EGG_ENABLED) &&
		(egg.stop == EGG_RUNNING) &&
		(emstune_get_cur_level() != EMS_LEVEL_PERFORMANCE);
}

int egg_profile_enabled(void)
{
	return (egg.enabled == EGG_ENABLED) &&
		(emstune_get_cur_level() != EMS_LEVEL_PERFORMANCE);
}

int egg_stopped(void)
{
	return egg.stop;
}

int egg_get_frame_drop(void)
{
	return egg.frame_drop;
}

int egg_get_thermal_level(void)
{
	return egg.thermal_level;
}

int egg_throttle_fastest_cpu(void)
{
	return egg.op_mode != OP_EXTREME_PERFORMANCE;
}

int egg_get_op_mode(void)
{
	return egg.op_mode;
}

int egg_get_sub_mode(void)
{
	return egg.sub_mode;
}

int egg_user_sched_policy(void)
{
	return egg.user_sched_policy;
}

u64 egg_get_update_period(void)
{
	return egg.update_period;
}

struct task_struct *egg_get_execution_task(void)
{
	return egg.execution_task;
}

void egg_set_execution_task(struct task_struct *task)
{
	egg.execution_task = task;
}

int egg_get_execution_state(void)
{
	return egg.execution_state;
}

void egg_set_execution_state(int state)
{
	egg.execution_state = state;
}

unsigned long egg_get_heaviest_task_util(void)
{
	if (egg.major_task[0].task)
		return egg.major_task[0].hist_util;

	return 0;
}

u64 egg_get_binder_contrib(struct task_struct *p)
{
	struct egg_task_struct *ets;
	unsigned long flags;
	int i;
	u64 contrib = 0;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		ets = &egg.major_task[i];

		if (p == ets->task) {
			raw_spin_lock_irqsave(&ets->task_lock, flags);
			contrib = ets->binder_contrib;
			ets->binder_contrib = 0;
			raw_spin_unlock_irqrestore(&ets->task_lock, flags);
			break;
		}
	}

	return contrib;
}

void egg_apply_binder_contrib(struct task_struct *binder, u64 contrib)
{
	struct egg_task_struct *ets;
	int i, caller_pid = binder_caller_pid(binder);
	unsigned long flags;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		ets = &egg.major_task[i];

		raw_spin_lock_irqsave(&ets->task_lock, flags);

		/* this task is no longer active */
		if (unlikely(!ets->task)) {
			raw_spin_unlock_irqrestore(&ets->task_lock, flags);
			continue;
		}

		if (ets->pid != caller_pid) {
			raw_spin_unlock_irqrestore(&ets->task_lock, flags);
			continue;
		}

		/* apply binder task's contrib to caller */
		ets->binder_contrib += contrib;
		raw_spin_unlock_irqrestore(&ets->task_lock, flags);
		return;
	}
}

bool egg_training_task(struct task_struct *p)
{
	struct egg_task_struct *ets;
	int i;

	if (!egg_enabled())
		return false;

	if (egg.sub_mode != EP_TRAINING)
		return false;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		ets = &egg.major_task[i];

		if (p == ets->task && ets->training.active)
			return true;
	}

	return false;
}

bool egg_execution_task(struct task_struct *p)
{
	struct egg_task_struct *ets;
	int i;

	if (egg.sub_mode != EP_EXECUTION)
		return false;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		ets = &egg.major_task[i];

		if (p == ets->task && ets->training.active)
			return true;
	}

	return false;
}

static void backup_training_info(struct egg_training_info *temp)
{
	int i;

	if (egg.sub_mode == EP_NORMAL)
		return;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		struct egg_task_struct *ets = &egg.major_task[i];

		if (ets->task && ets->training.active)
			memcpy(&temp[i], &ets->training, sizeof(struct egg_training_info));
		else
			memset(&temp[i], 0, sizeof(struct egg_training_info));
	}
}

static void restore_training_info(struct egg_task_struct *ets, struct egg_training_info *temp)
{
	int i;

	if (egg.sub_mode == EP_NORMAL)
		return;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		if (ets->task == temp[i].task) {
			memcpy(&ets->training, &temp[i], sizeof(struct egg_training_info));
			break;
		}
	}
}

static void update_execution_info(struct egg_task_struct *ets)
{
	struct egg_training_info *training = &ets->training;
	u64 recent_uarch, min_uarch, min_uarch_thresh;
	int hist_util, hist_util_thresh;

	if (egg_get_execution_task() != ets->task)
		return;

	min_uarch = training->min_uarch[exec_fast_cl_idx][UARCH_IPC];
	min_uarch_thresh = mult_frac(min_uarch, 70, 100);
	recent_uarch = recent_uarch_task(ets->task, exec_fast_cl_idx, UARCH_IPC);
	hist_util = training->hist_util[exec_fast_cl_idx];
	hist_util_thresh = mult_frac(hist_util, 50, 100);

	/*
	 * if the recent uarch is significantly lower than
	 * trained value, re-training is required.
	 */
	if (recent_uarch < min_uarch_thresh)
		training->active = 0;

	if (ets->hist_util < hist_util_thresh)
		training->active = 0;
}

static void update_training_info(struct egg_task_struct *ets)
{
	struct task_struct *p = ets->task;
	struct egg_training_info *training = &ets->training;
	int cluster, event;
	u64 hist_uarch, recent_uarch;

	/* update cluster util and uarch data */
	for (cluster = 0; cluster < MAX_NR_CLUSTERS; cluster++) {
		training->hist_util[cluster] = cluster_hist_util_task(p, cluster);

		for (event = 0; event < MAX_NR_UARCH; event++) {
			hist_uarch = hist_uarch_task(p, cluster, event);
			recent_uarch = recent_uarch_task(p, cluster, event);

				training->hist_uarch[cluster][event] = hist_uarch;
				training->recent_uarch[cluster][event] = recent_uarch;

				if (!recent_uarch)
					continue;

				if (!training->min_uarch[cluster][event])
					training->min_uarch[cluster][event] = recent_uarch;

				if (recent_uarch < training->min_uarch[cluster][event])
					training->min_uarch[cluster][event] = recent_uarch;
		}
	}
}

static u64 scale_hist_runnable(u64 hist_runnable, int cpu)
{
	int avg_freq, avg_max_freq;

	if (invalid_cpu(cpu))
		return 0;

	avg_freq = get_avg_cur_freq(cpu);
	avg_max_freq = get_avg_max_freq(cpu);

	if (!avg_max_freq)
		return 0;

	return mult_frac(hist_runnable, avg_freq, avg_max_freq);
}

static bool check_over_running(struct egg_task_struct *ets)
{
	u64 scaled_runnable = scale_hist_runnable(ets->hist_runnable, ets->reserved_cpu);
	u64 deadline = mult_frac(ets->hist_window, 60, 100);

	return scaled_runnable >= deadline;
}

void egg_update_major_tasks(struct task_struct **new_major_tasks)
{
	struct egg_training_info training_temp[MAX_NR_MAJOR];
	int i, nr_major = 0, nr_over_running = 0, nr_near_heaviest = 0, nr_training = 0;
	unsigned long total_hist_util = 0;
	unsigned long total_pelt_util = 0;
	unsigned long near_heaviest_thresh = 0;

	backup_training_info(training_temp);

	/* near_heaviest_thresh is 80% of the heaviest task's util */
	if (new_major_tasks[0])
		near_heaviest_thresh = mult_frac(hist_util_task(new_major_tasks[0]), 80, 100);

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		struct egg_task_struct *ets = &egg.major_task[i];
		struct task_struct *new_p = new_major_tasks[i];

		raw_spin_lock(&ets->task_lock);

		if (!new_major_tasks[i]) {
			clear_egg_task_struct(ets);
			goto next;
		}

		ets->idx = i;
		ets->task = new_p;
		strncpy(ets->comm, new_p->comm, TASK_COMM_LEN);
		cpumask_copy(&ets->cpus_ptr, new_p->cpus_ptr);
		ets->pid = new_p->pid;
		ets->important = (ets->pid == egg.important_task);
		ets->important_tex = false;

		ets->prolonged_running = prolonged_running_detected(new_p);
		ets->hist_window = hist_window_task(new_p);
		ets->hist_runnable = hist_runnable_task(new_p);
		ets->over_running = check_over_running(ets);
		ets->hist_util = hist_util_task(new_p);
		ets->pelt_util = ml_task_util(new_p);
		ets->binder_contrib = 0;
		ets->near_heaviest = (ets->hist_util >= near_heaviest_thresh) ? true : false;

		/*
		 * reserved_cpu is still not updated at this point.
		 * this is intended to be used as prev_cpu for cleave.
		 */
		ets->prev_reserved_cpu = ets->reserved_cpu = ems_task_reserved_cpu(ets->task);

		/* get backup training info */
		restore_training_info(ets, training_temp);

		/* update training info */
		if (ets->training.active) {
			if (egg.sub_mode == EP_TRAINING)
				update_training_info(ets);

			if (egg.sub_mode == EP_EXECUTION)
				update_execution_info(ets);
		}

		nr_major++;

		if (ets->over_running)
			nr_over_running++;

		if (ets->near_heaviest)
			nr_near_heaviest++;

		if (ets->training.active)
			nr_training++;

		total_hist_util += ets->hist_util;
		total_pelt_util += ets->pelt_util;

		trace_egg_update_major_tasks(i, ets);
next:
		raw_spin_unlock(&ets->task_lock);
	}

	egg.total_hist_util = total_hist_util;
	egg.total_pelt_util = total_pelt_util;

	egg.nr_major = nr_major;
	egg.nr_over_running = nr_over_running;
	egg.nr_near_heaviest = nr_near_heaviest;
	egg.nr_training = nr_training;

	trace_egg_update_major_tasks_total_util(nr_major, nr_over_running,  nr_near_heaviest,
			nr_training, near_heaviest_thresh, total_hist_util, total_pelt_util);
}

void egg_get_major_tasks(struct egg_task_struct *dst[], unsigned long *total_hist_util)
{
	int i;

	for (i = 0; i < MAX_NR_MAJOR; i++)
		dst[i] = &egg.major_task[i];

	*total_hist_util = egg.total_hist_util;
}

void egg_update_reserved_cpus(struct cleave_task *clv_tasks, int nr_clv)
{
	unsigned long flags;
	int i, nr_reserved_cpus = 0;
	int important_idx = -1;

	raw_spin_lock_irqsave(&egg.reserve_lock, flags);

	cpumask_clear(&egg.reserved_cpu_mask);

	for_each_cpu(i, cpu_possible_mask)
		egg.reserved_cpu_util[i] = 0;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		struct egg_task_struct *ets = &egg.major_task[i];
		raw_spin_lock(&ets->task_lock);
		ets->prev_reserved_cpu = ets->reserved_cpu = -1;
		/* To find non-reserved important major task */
		if (ets->important)
			important_idx = i;
		raw_spin_unlock(&ets->task_lock);
	}

	for (i = 0; i < nr_clv; i++) {
		struct cleave_task *ct = &clv_tasks[i];
		struct egg_task_struct *ets;

		if (!ct->ets || invalid_cpu(ct->cpu))
			continue;

		ets = ct->ets;
		raw_spin_lock(&ets->task_lock);

		ets->prev_reserved_cpu = ets->reserved_cpu = ct->cpu;
		ets->misfit = ct->misfit;
		/* important task is reserved */
		if (ets->important)
			important_idx = -1;

		egg.reserved_cpu_util[ct->cpu] = ct->util;
		cpumask_set_cpu(ets->reserved_cpu, &egg.reserved_cpu_mask);
		nr_reserved_cpus++;

		trace_egg_update_reserved_cpus(ets);

		raw_spin_unlock(&ets->task_lock);
	}

	if (important_idx >= 0) {
		struct egg_task_struct *ets = &egg.major_task[important_idx];
		raw_spin_lock(&ets->task_lock);
		ets->important_tex = true;
		raw_spin_unlock(&ets->task_lock);
	}

	egg.nr_reserved_cpus = nr_reserved_cpus;

	raw_spin_unlock_irqrestore(&egg.reserve_lock, flags);
}

/**********************************************************************
 *			   	BOOST 				      *
 **********************************************************************/
#define DEFAULT_RENDER_BOOST_THR_RATIO	95
static int get_gpu_rendering_delay(void)
{
	if (get_avg_gpu_render_us() > egg.eb.render_boost_thr)
		return true;
	return false;
}

static int get_cpu_rendering_delay(void)
{
	if (get_avg_cpu_render_us() > egg.eb.render_boost_thr)
		return true;
	return false;
}

static void _update_mif_boost(struct egg_boost_domain *dom, ktime_t expired_time,
			bool enabled, int tag)
{
	int lv = expired_time ? egg.eb.mif_boost_lv : 0;

	pago_change_shift_level_mif(lv);
	dom->expired_time = expired_time;
	trace_egg_update_mif_boost(dom->enabled, dom->expired_time, egg_boost_reason[tag]);
}

static void update_mif_boost(ktime_t now, bool enabled, int tag)
{
	struct egg_boost *eb = &egg.eb;
	struct egg_boost_domain *dom = &eb->mif_boost;
	unsigned long flags;

	raw_spin_lock_irqsave(&dom->lock, flags);

	/* en/disable boost */
	if (unlikely(!now)) {
		_update_mif_boost(dom, 0, enabled, tag);
		check_pago_update_period();
		dom->enabled = enabled;
		goto unlock;
	}

	if (unlikely(!dom->enabled))
		goto unlock;

	if (enabled) {
		u64 expired_time = ktime_add(now, DEFAULT_BOOST_TIME);
		_update_mif_boost(dom, expired_time, enabled, tag);
		reset_update_period();
	} else {
		if (dom->expired_time && dom->expired_time < now) {
			_update_mif_boost(dom, 0, enabled, tag);
			check_pago_update_period();
		}
	}

unlock:
	raw_spin_unlock_irqrestore(&dom->lock, flags);
}

static void _update_dsu_boost(struct egg_boost_domain *dom, ktime_t expired_time,
			bool enabled, int tag)
{
	int lv = expired_time ? egg.eb.dsu_boost_lv : 0;

	pago_change_shift_level_dsu(lv);
	dom->expired_time = expired_time;
	trace_egg_update_dsu_boost(dom->enabled, dom->expired_time, egg_boost_reason[tag]);

}
static void update_dsu_boost(ktime_t now, bool enabled, int tag)
{
	struct egg_boost *eb = &egg.eb;
	struct egg_boost_domain *dom = &eb->dsu_boost;
	unsigned long flags;

	raw_spin_lock_irqsave(&dom->lock, flags);

	/* en/disable boost */
	if (unlikely(!now)) {
		_update_dsu_boost(dom, 0, enabled, tag);
		check_pago_update_period();
		dom->enabled = enabled;
		goto unlock;
	}

	if (unlikely(!dom->enabled))
		goto unlock;

	if (enabled) {
		u64 expired_time = ktime_add(now, DEFAULT_BOOST_TIME);
		_update_dsu_boost(dom, expired_time, enabled, tag);
		reset_update_period();
	} else {
		if (dom->expired_time && dom->expired_time < now) {
			_update_dsu_boost(dom, 0, enabled, tag);
			check_pago_update_period();
		}
	}

unlock:
	raw_spin_unlock_irqrestore(&dom->lock, flags);
}

void egg_flush_task(struct task_struct *p)
{
	struct egg_task_struct *ets;
	unsigned long flags;
	int i;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		ets = &egg.major_task[i];
		raw_spin_lock_irqsave(&ets->task_lock, flags);
		if (ets->task != p) {
			raw_spin_unlock_irqrestore(&ets->task_lock, flags);
			continue;
		}
		ets->task = NULL;
		raw_spin_unlock_irqrestore(&ets->task_lock, flags);
		break;;
	}
}

int get_boost_cpu(void)
{
	return cpumask_first(&egg.execution_fast_cpus);
}

int get_longest_running_task(u64 *task_util)
{
	unsigned long flags;
	struct egg_task_struct *ets;
	int max_ratio = 0, i;
	pid_t pid = -1;

	if (egg_is_normal_game()) {
		pid = profile_heavy_task(task_util);
		if (pid != -1)
			return pid;
	}

	raw_spin_lock_irqsave(&egg.reserve_lock, flags);

	if (egg.shader_compile_task) {
		pid = egg.shader_compile_task;
		goto unlock;
	}

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		unsigned long flag;
		int ratio = 0;
		ets = &egg.major_task[i];

		raw_spin_lock_irqsave(&ets->task_lock, flag);

		if (!ets->task) {
			raw_spin_unlock_irqrestore(&ets->task_lock, flag);
			continue;
		}

		ratio = prolonged_running_detected(ets->task);
		if (ratio > max_ratio) {
			max_ratio = ratio;
			pid = ets->pid;
			*task_util = ets->pelt_util;
		}
		raw_spin_unlock_irqrestore(&ets->task_lock, flag);
	}

unlock:
	raw_spin_unlock_irqrestore(&egg.reserve_lock, flags);
	return pid;
}

bool big_boosted(void)
{
	return egg.eb.boosted_task != -1;
}

int get_boosted_task(void)
{
	return egg.eb.boosted_task;
}

void set_boosted_task(int pid)
{
	egg.eb.boosted_task = pid;
}

int egg_prev_boost_task_get_cpu(struct egg_task_struct *ets, int new_task)
{
	int perf_cpu = 0, cpu, min_util = INT_MAX;
	struct cpumask available_cpus;

	dct_get_available_cpus(&available_cpus);

	/* release big boost and has available reserved cpu */
	if (new_task == -1 || egg.prev_shader_compile_task == new_task) {
		perf_cpu = ets->prev_reserved_cpu;
		if (valid_cpu(perf_cpu)) {
			if (perf_cpu != get_boost_cpu() &&
				!cpumask_test_cpu(perf_cpu, &egg.reserved_cpu_mask))
				return perf_cpu;
		} else {
			perf_cpu = -1;
		}
		return perf_cpu;
	}

	/* switch big boost task */
	for (cpu = VENDOR_NR_CPUS - 1; cpu >= 0; cpu--) {
		int util;

		if (cpu == get_boost_cpu())
			continue;

		if (!cpumask_test_cpu(cpu, &egg.reserved_cpu_mask)) {
			perf_cpu = cpu;
			break;
		}

		util = ml_cpu_util(cpu);
		if (util < min_util) {
			min_util = util;
			perf_cpu = cpu;
		}
	}

	return perf_cpu;
}

int egg_update_reserved_cpu(int new_task, int prev_task)
{
	int i;
	unsigned long flags;
	struct egg_task_struct *ets;
	bool new_task_update = false;

	raw_spin_lock_irqsave(&egg.reserve_lock, flags);
	for (i = 0; i < MAX_NR_MAJOR; i++) {
		bool need_update = false;
		int val, new_cpu = -1;
		ets = &egg.major_task[i];

		raw_spin_lock(&ets->task_lock);

		if (ets->pid == prev_task) {
			new_cpu = egg_prev_boost_task_get_cpu(ets, new_task);
			need_update = true;
		} else if (ets->pid == new_task) {
			new_cpu = get_boost_cpu();
			need_update = true;
			new_task_update = true;
		}

		if (!need_update) {
			raw_spin_unlock(&ets->task_lock);
			continue;
		}

		if (valid_cpu(ets->reserved_cpu)) {
			egg.reserved_cpu_util[ets->reserved_cpu] = 0;
			cpumask_clear_cpu(ets->reserved_cpu, &egg.reserved_cpu_mask);
		}

		if (valid_cpu(new_cpu)) {
			val = ets->hist_util;
			egg.reserved_cpu_util[new_cpu] = val;
			cpumask_set_cpu(new_cpu, &egg.reserved_cpu_mask);
			ets->reserved_cpu = new_cpu;
		}

		trace_egg_update_reserved_cpu(ets);

		raw_spin_unlock(&ets->task_lock);

		continue;;
	}

	if (new_task != -1 && !new_task_update)
		cpumask_set_cpu(get_boost_cpu(), &egg.reserved_cpu_mask);

	if (new_task == -1)
		cpumask_clear_cpu(get_boost_cpu(), &egg.reserved_cpu_mask);

	raw_spin_unlock_irqrestore(&egg.reserve_lock, flags);

	return 0;
}

int ems_task_reserved_cpu(struct task_struct *p)
{
	if (p->pid == get_boosted_task())
		return get_boost_cpu();

	return ems_task_reserved_cpu_wo_boost(p);
}

static void __update_big_boost(struct egg_boost_domain *dom, ktime_t expired_time,
				bool enabled, int boosted_task, int tag)
{
	unsigned int btid = (boosted_task > 0) ? boosted_task : 0;

	cleave_update_boosted_mode(boosted_task);
	dom->expired_time = expired_time;
	trace_clock_set_rate("EGG big rsn", tag, btid);
	trace_egg_update_big_boost(dom->enabled, dom->expired_time,
			    &egg.reserved_cpu_mask, get_boosted_task(), egg_boost_reason[tag]);
}

static bool available_big_boost(u64 used)
{
	struct cpumask reserved_cpus;
	int big_cpu = get_boost_cpu();
	int mid_cpu = cpumask_first(&egg.execution_slow_cpus);

	if (unlikely(invalid_cpu(big_cpu)))
		return false;

	if (!cpumask_test_cpu(big_cpu, cpu_active_mask))
		return false;

	if (!cpumask_test_cpu(big_cpu, ecs_max_cpus()))
		return false;

	if (likely(valid_cpu(mid_cpu))) {
		unsigned long big_capacity, mid_capacity;
		big_capacity = ems_get_actual_cpu_capacity(big_cpu);
		mid_capacity = ems_get_actual_cpu_capacity(mid_cpu);
		if (big_capacity < mid_capacity)
			return false;
	}

	get_reserved_cpu_mask(&reserved_cpus);
	if (!used && cpumask_test_cpu(big_cpu, &reserved_cpus))
		return false;

	return true;
}

#define FREQ_BOOST_CPU	6
static bool need_hf_freq_boost(struct egg_boost *eb, struct cpumask *reserved_cpus,
				ktime_t prev_expired_time, int boosted_task)
{
	if (!(egg_is_normal_game() || egg.eb.hf_freq_boost_enabled))
		return false;

	if (!cpumask_intersects(reserved_cpus, cpu_clustergroup_mask(FREQ_BOOST_CPU)))
		return false;

	if (!prev_expired_time)
		return true;

	if (egg.eb.boosted_task != boosted_task)
		return true;

	return false;
}

void update_big_boost(ktime_t now, bool enabled, bool force, int tag)
{
	struct egg_boost *eb = &egg.eb;
	struct egg_boost_domain *dom = &eb->big_boost;
	struct cpumask reserved_cpus;
	unsigned long flags;

	get_reserved_cpu_mask(&reserved_cpus);

	raw_spin_lock_irqsave(&dom->lock, flags);

	/* en/disable boost */
	if (unlikely(!now)) {
		dom->enabled = enabled;
		__update_big_boost(dom, 0, enabled, -1, tag);
		goto unlock;
	}

	if (unlikely(!dom->enabled))
		goto unlock;

	if (enabled && !available_big_boost(dom->expired_time)) {
		force = true;
		enabled = false;
	}

	if (enabled) {
		ktime_t expired_time = ktime_add(now, DEFAULT_BIG_BOOST_TIME);
		u64 task_util = 0;
		int boosted_task = get_longest_running_task(&task_util);
		ktime_t prev_expired_time = dom->expired_time;

		if (boosted_task == -1)
			goto unlock;

		__update_big_boost(dom, expired_time, enabled, boosted_task, tag);

		if (need_hf_freq_boost(eb, &reserved_cpus, prev_expired_time, boosted_task)) {
			eb->hf_freq_expired_time = expired_time;
			task_util = task_util * 125 / 100;
			ego_set_margin_util_target(FREQ_BOOST_CPU, task_util);
		}

	} else {
		if ((dom->expired_time && dom->expired_time < now) || force) {
			__update_big_boost(dom, 0, enabled, -1, tag);
			eb->hf_freq_expired_time = 0;
			ego_set_margin_util_target(FREQ_BOOST_CPU, 0);
		}
	}

	if (eb->hf_freq_expired_time < now) {
		eb->hf_freq_expired_time = 0;
		ego_set_margin_util_target(FREQ_BOOST_CPU, 0);
	}

unlock:
	raw_spin_unlock_irqrestore(&dom->lock, flags);
}

bool egg_is_busy_game(void)
{
	if (unlikely(!egg.type))
		return false;

	if (egg.type <= EGT_NORMAL)
		return true;;

	return false;
}

bool egg_is_normal_game(void)
{
	return egg.type == EGT_NORMAL;

}

/* check to enable boost for per frame */
#define DEFAULT_PF_CPU_RENDERING_DELAY_CNT	2
void egg_update_pf_boost_info(u64 target_frame_time,
			      u64 current_frame_time, u64 cpu_time,
			      u64 user_target_temp, u64 user_cur_temp, int type)
{
	ktime_t now = ktime_get();
	u64 frame_time_diff = ((current_frame_time * 10) > (target_frame_time * 15)) ?
			((current_frame_time * 10) - (target_frame_time * 15)) : 0;

	if (user_cur_temp >= user_target_temp)
		egg.eb.meet_target_temp = true;
	else
		egg.eb.meet_target_temp = false;

	if (invalid_frame_info())
		return;

	if (target_frame_time < cpu_time)
		egg.eb.pf_cpu_rendering_delay_cnt++;
	else
		egg.eb.pf_cpu_rendering_delay_cnt = 0;

	egg.eb.mif_boost_lv = egg.eb.meet_target_temp ? 0 : egg.eb.cur_boost_lv;
	egg.eb.dsu_boost_lv = egg.eb.cur_boost_lv;
	egg.type = type;

	if (egg.eb.pf_cpu_rendering_delay_cnt >= DEFAULT_PF_CPU_RENDERING_DELAY_CNT) {
		update_mif_boost(now, true, EB_PF_CPU_BOOST);
		update_dsu_boost(now, true, EB_PF_CPU_BOOST);
	}

	trace_clock_set_rate("EGG PF", frame_time_diff, egg.type);

	/* check cpu busrt rendrering job  */
	if (frame_time_diff) {
		update_mif_boost(now, true, EB_PF_BIG_BOOST);
		update_dsu_boost(now, true, EB_PF_BIG_BOOST);
		if (!egg.shader_compile_task)
			update_big_boost(now, true, true, EB_PF_BIG_BOOST);
	}

	egg.eb.prev_frame_cpu_render_time = cpu_time;
	egg.eb.prev_current_frame_time = current_frame_time;
}

bool egg_has_shader_compile_task(void)
{
	return !!egg.shader_compile_task;
}

static void set_shader_compile_task(pid_t pid)
{
	egg.prev_shader_compile_task = egg.shader_compile_task;
	egg.shader_compile_task = pid;
	trace_egg_set_shader_compile_task(pid);
}

static void update_shader_compile_task(pid_t pid)
{
	ktime_t now = ktime_get();
	int cpu = cpumask_first(cpu_fastest_mask());

	set_shader_compile_task(pid);

	if (pid && !egg.eb.meet_target_temp) {
		egg.eb.fb->ctrl_boost_freq(true, cpu, UINT_MAX);
		update_big_boost(now, true, true, EB_COMPILE);
		update_dct_cpus_w_lock(DCT_COMPILE, true);
	} else {
		egg.eb.fb->ctrl_boost_freq(false, cpu, 0);
		update_big_boost(now, false, true, EB_COMPILE_DEL);
		update_dct_cpus_w_lock(DCT_COMPILE_DEL, true);
	}
}

void egg_update_shader_compile_task(pid_t pid)
{
	update_shader_compile_task(pid);
}
EXPORT_SYMBOL_GPL(egg_update_shader_compile_task);

static void update_egg_boost(void)
{
	ktime_t now = ktime_get();
	struct egg_boost *eb = &egg.eb;

	if (invalid_frame_info())
		return;

	egg.eb.render_boost_thr = (ktime_to_us(get_target_frame_period()) / 100)
					* DEFAULT_RENDER_BOOST_THR_RATIO;

	egg.eb.cpu_rendering_delay = get_cpu_rendering_delay();
	egg.eb.gpu_rendering_delay = get_gpu_rendering_delay();

	if (eb->cpu_rendering_delay)
		update_dsu_boost(now, true, EB_AVG_RENDER_BOOST);

	if (eb->gpu_rendering_delay || eb->cpu_rendering_delay)
		update_mif_boost(now, true, EB_AVG_RENDER_BOOST);
}

static void try_to_disable_egg_boost(void)
{
	ktime_t now = ktime_get();
	update_mif_boost(now, false, EB_TRY_DISABLE);
	update_dsu_boost(now, false, EB_TRY_DISABLE);

	if (egg.shader_compile_task)
		update_big_boost(now, true, true, EB_COMPILE);
	else
		update_big_boost(now, false, false, EB_TRY_DISABLE);
}

static void egg_reset_boost(bool enabled)
{
	update_mif_boost(0, enabled, EB_SYSFS);
	update_dsu_boost(0, enabled, EB_SYSFS);
	update_big_boost(0, enabled, true, EB_SYSFS);
}

static void egg_init_boost(void)
{
	raw_spin_lock_init(&egg.eb.mif_boost.lock);
	raw_spin_lock_init(&egg.eb.dsu_boost.lock);
	raw_spin_lock_init(&egg.eb.big_boost.lock);
}

/**********************************************************************
 *			    Reserve CPUs			      *
 **********************************************************************/
bool is_reserved_task(struct task_struct *p)
{
	if (!egg_enabled())
		return false;

	if (get_sched_class(p) != EMS_SCHED_FAIR)
		return false;

	if (invalid_cpu(ems_task_reserved_cpu(p)))
		return false;

	return true;
}

int is_reserved_cpu(int cpu)
{
	int ret;
	unsigned long flags;

	raw_spin_lock_irqsave(&egg.reserve_lock, flags);
	ret = cpumask_test_cpu(cpu, &egg.reserved_cpu_mask);
	raw_spin_unlock_irqrestore(&egg.reserve_lock, flags);

	return ret;
}

int get_reserved_cpu(struct task_struct *p)
{
	if (!is_reserved_task(p))
		return -1;

	return ems_task_reserved_cpu(p);
}

void get_reserved_cpu_mask(struct cpumask *mask)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&egg.reserve_lock, flags);
	cpumask_copy(mask, &egg.reserved_cpu_mask);
	raw_spin_unlock_irqrestore(&egg.reserve_lock, flags);
}

int get_reserved_cpu_util(int cpu)
{
	return egg.reserved_cpu_util[cpu];
}

static void reset_reserved_cpus(void)
{
	int cpu;
	unsigned long flags;

	raw_spin_lock_irqsave(&egg.reserve_lock, flags);
	for_each_possible_cpu(cpu)
		egg.reserved_cpu_util[cpu] = 0;

	cpumask_clear(&egg.reserved_cpu_mask);
	egg.nr_reserved_cpus = 0;
	raw_spin_unlock_irqrestore(&egg.reserve_lock, flags);
}

static bool misfit_reserved_task(struct task_struct *p)
{
	struct egg_task_struct *ets = get_egg_task_struct(p);

	if (!ets)
		return false;

	if (invalid_cpu(ets->reserved_cpu))
		return false;

	return ets->misfit;
}

bool egg_lb_tick_allowed(int src_cpu, int *force_test)
{
	struct task_struct *p = cpu_rq(src_cpu)->curr;
	unsigned long task_hist_util = hist_util_task(p);
	unsigned long task_util = ml_task_util(p);
	int reserved_cpu = ems_task_reserved_cpu(p);

	if (src_cpu != reserved_cpu)
		return true;

	if (task_util > task_hist_util + egg.lb_margin)
		return true;

	if (misfit_reserved_task(p))
		return true;

	return false;
}

static void clear_egg_task_struct(struct egg_task_struct *ets)
{
	if (unlikely(!ets))
		return;

	ets->task = NULL;
	memset(ets->comm, 0, sizeof(char) * TASK_COMM_LEN);
	ets->pid = 0;
	cpumask_clear(&ets->cpus_ptr);
	ets->idx = 0;
	ets->misfit = 0;
	ets->reserved_cpu = -1;
	ets->prev_reserved_cpu = -1;
	ets->near_heaviest = 0;
	ets->prolonged_running = 0;
	ets->over_running = 0;
	ets->hist_runnable = 0;
	ets->hist_window = 0;
	ets->hist_util = 0;
	ets->pelt_util = 0;
	ets->binder_contrib = 0;
	ets->important = 0;
	ets->important_tex = 0;
	memset(&ets->training, 0, sizeof(struct egg_training_info));
}

static void egg_reserved_cpus_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		egg.reserved_cpu_util[cpu] = 0;

	raw_spin_lock_init(&egg.reserve_lock);
	cpumask_clear(&egg.reserved_cpu_mask);
	egg.nr_reserved_cpus = 0;
}

/********************************************************************************
 *				NOTIFY						*
 ********************************************************************************/
static RAW_NOTIFIER_HEAD(egg_chain);

static int egg_notify(int enabled, int event)
{
	 int ret;

	 ret = raw_notifier_call_chain(&egg_chain, event, &enabled);

	 return notifier_to_errno(ret);
}

int egg_register_notifier(struct notifier_block *nb)
{
	 return raw_notifier_chain_register(&egg_chain, nb);
}
EXPORT_SYMBOL_GPL(egg_register_notifier);

int egg_unregister_notifier(struct notifier_block *nb)
{
	 return raw_notifier_chain_unregister(&egg_chain, nb);
}
EXPORT_SYMBOL_GPL(egg_unregister_notifier);

/********************************************************************************
 *				Internal Functions				*
 ********************************************************************************/
static void game_pago_enable_control(bool enabled)
{
	egg.game_pago_enabled = enabled;
}

static void update_game_pago(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&egg.game_pago_lock, flags);

	if(unlikely(!egg.game_pago_enabled))
		goto unlock;

	check_pago_update_period();

	if (egg_enabled()){
		switch (egg.op_mode){
			case OP_SUSTAIN:
				pago_ramp_up_ctrl(RAMP_UP_DISABLE);
				break;
			case OP_PERFORMANCE:
			case OP_EXTREME_PERFORMANCE:
				pago_ramp_up_ctrl(RAMP_UP_ENABLE);
				break;
			case OP_ENERGY:
				pago_ramp_up_ctrl(RAMP_UP_DISABLE);
				break;
			default:
				pago_ramp_up_ctrl(RAMP_UP_ENABLE);
		}
	}
	else{
		pago_ramp_up_ctrl(RAMP_UP_ENABLE);
	}

unlock:
	raw_spin_unlock_irqrestore(&egg.game_pago_lock, flags);
}

static void reset_game_pago(bool enable)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&egg.game_pago_lock, flags);
	game_pago_enable_control(enable);
	reset_update_period();
	pago_ramp_up_ctrl(RAMP_UP_ENABLE);
	raw_spin_unlock_irqrestore(&egg.game_pago_lock, flags);
}

static void egg_stopping_func(int enable)
{
	reset_cleave();
	reset_dct(enable);
	reset_reserved_cpus();
	reset_game_pago(enable);
	dynamic_control_governor(GOV_CTRL_BY_EGG, !enable);
	egg_reset_boost(enable);
	set_shader_compile_task(0);
}

#define FIND_HIST_IDX_SIZE	5
#define LOW_FPS	15

int low_fps_cnt = 0, frame_info_cnt = 0;
static void check_egg_stop_condition(void)
{
	int cur_idx = egg.last_idx;
	int stopped = egg_stopped();

	low_fps_cnt = 0, frame_info_cnt = 0;

	for (int i = 0; i < FIND_HIST_IDX_SIZE; i++) {
		if (egg.target_from[cur_idx])
			frame_info_cnt++;

		if (egg.fps[cur_idx] < LOW_FPS)
			low_fps_cnt++;

		cur_idx = move_idx(cur_idx, -1, TEMP_HIST_SIZE);
	}

	if ((get_frame_info_enabled() && (frame_info_cnt <= 1) && !frame_info_stop()) ||
				(low_fps_cnt > 3) || (!egg.nr_major)) {
		egg.stop = EGG_STOP;
		egg.type = EGT_ERROR;
		pr_info("%s: EGG STOP enabled=%d, stop=%d, fic=%d lfc=%d, nr_major-%d\n",
				  __func__, get_frame_info_enabled(), frame_info_stop(),
				  frame_info_cnt, low_fps_cnt, egg.nr_major);

		if (!stopped) {
			egg_stopping_func(false);
			egg_notify(egg_enabled(), EGG_STOP_CONTROL);
		}

		return;
	}

	egg.stop = EGG_RUNNING;
	if(stopped) {
		egg_stopping_func(true);
		egg_notify(egg_enabled(), EGG_STOP_CONTROL);
	}

	return;
}

static struct egg_task_struct *get_egg_task_struct(struct task_struct *p)
{
	struct egg_task_struct *ets;
	int i;

	if (!egg_enabled())
		return NULL;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		ets = &egg.major_task[i];

		if (!ets->task)
			break;
		else if (ets->task == p)
			return ets;
	}

	return NULL;
}

void egg_assign_reserved_cpu(struct task_struct *p)
{
	struct egg_task_struct *ets = get_egg_task_struct(p);

	/* this task is a major task */
	if (ets) {
		ems_task_reserved_cpu_wo_boost(p) = ets->reserved_cpu;
		egg_task_idx(p) = ets->idx;
		ems_low_latency_task(p) = (ets->important_tex) ? 1 : 0;
		return;
	}

	ems_task_reserved_cpu_wo_boost(p) = -1;
	egg_task_idx(p) = -1;
	ems_low_latency_task(p) = 0;
}

#define PERF_SEARCH_SIZE	3
static int get_frame_drop(int window_idx)
{
	int cnt, fd_cnt = 0;

	if (egg.fps[window_idx] < egg.target[window_idx])
		return true;

	for (cnt = 0; cnt < PERF_SEARCH_SIZE; cnt++) {
		window_idx = move_idx(window_idx, -1, TEMP_HIST_SIZE);
		if (egg.fps[window_idx] < egg.target[window_idx])
			fd_cnt++;
	}

	return fd_cnt > 1;
}

#define TEMP_SEARCH_SIZE	5
static int get_thermal_level(int window_idx)
{
	struct cpumask visit_cpus;
	int cnt, high_cnt = 0, low_cnt = 0;

	for (cnt = 0; cnt < TEMP_SEARCH_SIZE; cnt++) {
		int cpu, temp_high_cnt = 0, temp_low_cnt = 0;

		cpumask_clear(&visit_cpus);

		for_each_cpu(cpu, cpu_online_mask) {
			struct eg_thermal_zone *tz;

			if (cpumask_test_cpu(cpu, &visit_cpus))
				continue;

			tz = egg.tz[cpu];
			if (unlikely(!tz))
				continue;

			if (tz->temp[window_idx] > egg.high_temp)
				temp_high_cnt++;

			if (tz->temp[window_idx] < egg.low_temp)
				temp_low_cnt++;

			cpumask_or(&visit_cpus, &visit_cpus, &tz->cpus);
		}

		if (temp_high_cnt >= (egg.num_of_cl >> 1))
			high_cnt++;

		if (temp_low_cnt == egg.num_of_cl)
			low_cnt++;

		window_idx = move_idx(window_idx, -1, TEMP_HIST_SIZE);
	}

	if (high_cnt >= (TEMP_SEARCH_SIZE - 1))
		return HIGH_TEMP;

	else if (low_cnt >= (TEMP_SEARCH_SIZE - 1))
		return LOW_TEMP;

	return MID_TEMP;
}

#define DEFAULT_EXECUTION_INVALID_THR	3
#define DISABLE_TRAINING_MODE_THR	10
static int __determine_sub_mode(int next_op_mode, u64 now)
{
	int prev_op_mode = egg.op_mode;

	if (prev_op_mode != OP_EXTREME_PERFORMANCE || next_op_mode != OP_EXTREME_PERFORMANCE)
		return EP_NORMAL;

	if (egg.nr_major < 2)
		return EP_NORMAL;

	/* enter training mode */
	if (egg.sub_mode == EP_NORMAL && egg.nr_near_heaviest >= DISABLE_TRAINING_MODE_THR)
		return EP_TRAINING;

	if (egg.sub_mode == EP_TRAINING) {
		/* training tasks changed during training mode. re-training is required. */
		if (egg.nr_training < 2)
			return EP_NORMAL;

		/* training has completed. enter execution mode. */
		if(egg.training_start_ns + egg.training_period < now) {
			egg_set_execution_state(EP_EXECUTION_TRY);
			egg.execution_invalid_cnt = 0;
			return EP_EXECUTION;
		}

		/* stay in training mode */
		return EP_TRAINING;
	}

	if (egg.sub_mode == EP_EXECUTION) {
		/* if cleave failed to bind task, go back NORMAL mode */
		if (egg.execution_state == EP_EXECUTION_FAILED)
			return EP_NORMAL;

		/* training tasks changed during execution mode. re-training is required. */
		if (egg.nr_training < 2)
			return EP_NORMAL;

		/* execution mode is finished. enter normal mode to clear remaining data. */
		if (egg.execution_start_ns + egg.execution_period < now)
			return EP_NORMAL;

		if (egg.execution_state == EP_EXECUTION_INVALID) {
			if (++egg.execution_invalid_cnt >= DEFAULT_EXECUTION_INVALID_THR)
				return EP_NORMAL;

			return EP_EXECUTION;
		}

		/* stay in execution mode */
		egg.execution_invalid_cnt = 0;
		return EP_EXECUTION;
	}

	return EP_NORMAL;
}

static void mark_training_task(bool training_start)
{
	int i;
	unsigned long flags;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		struct egg_task_struct *ets = &egg.major_task[i];

		raw_spin_lock_irqsave(&ets->task_lock, flags);

		memset(&ets->training, 0, sizeof(struct egg_training_info));

		if (training_start && ets->near_heaviest) {
			ets->training.task = ets->task;
			ets->training.active = true;
			trace_egg_mark_training_task(ets->task);
		}

		raw_spin_unlock_irqrestore(&ets->task_lock, flags);
	}
}

static int determine_sub_mode(int op_mode)
{
	int prev_mode = egg.sub_mode, next_mode;
	u64 now = sched_clock();

	next_mode = __determine_sub_mode(op_mode, now);

	if (prev_mode != next_mode) {
		if (next_mode == EP_TRAINING) {
			egg.training_start_ns = now;
			egg.execution_start_ns = 0;
			mark_training_task(true);
		} else if (next_mode == EP_EXECUTION) {
			egg.training_start_ns = 0;
			egg.execution_start_ns = now;
		} else {
			egg.training_start_ns = 0;
			egg.execution_start_ns = 0;
			mark_training_task(false);
		}
	}

	if (trace_egg_determine_sub_mode_enabled()) {
		u64 delta = 0;

		if (next_mode == EP_TRAINING)
			delta = now - egg.training_start_ns;
		else if (next_mode == EP_EXECUTION)
			delta = now - egg.execution_start_ns;

		trace_egg_determine_sub_mode(prev_mode, next_mode, delta);
	}

	return next_mode;
}

static int determine_op_mode(void)
{
	if (egg.thermal_level == HIGH_TEMP)
		return OP_SUSTAIN;

	if (egg.thermal_level == MID_TEMP ){
		if (egg.frame_drop)
			return OP_PERFORMANCE;

		return OP_ENERGY;
	}

	if (egg.frame_drop) {
		if (egg.demand_cpu > cpumask_weight(cpu_active_mask))
			return OP_EXTREME_PERFORMANCE;

		if (egg.freq_throttle && egg.max_freq_throttle &&
			(egg.task_overutil || (egg.nr_over_running && egg.gpu_max_freq_throttle)))
			return OP_EXTREME_PERFORMANCE;

		return OP_PERFORMANCE;
	}

	return OP_ENERGY;
}

static void update_op_mode(void)
{
	struct cpumask visit_cpus;
	int mode, sub_mode, stopped, cpu, cl = 0;
	int cur_idx = egg.last_idx;
	int temp[4] = { 0, };
	unsigned long heaviest_task_util = egg_get_heaviest_task_util();

	/* update performance index*/
	egg.target[cur_idx] = get_target_fps();
	egg.target_from[cur_idx] = get_target_fps_from();
	egg.fps[cur_idx] = get_current_fps();
	egg.frame_drop = get_frame_drop(cur_idx);

	/* update thermal index */
	cpumask_clear(&visit_cpus);
	for_each_cpu(cpu, cpu_online_mask) {
		struct eg_thermal_zone *tz;
		int val = -1;

		if (cpumask_test_cpu(cpu, &visit_cpus))
			continue;

		tz = egg.tz[cpu];
		if (!tz)
			continue;

		thermal_zone_get_temp(tz->tz_dev, &val);
		temp[cl] = val < 0 ?  0 : val / 1000;

		tz->temp[cur_idx] = temp[cl];

		cpumask_or(&visit_cpus, &visit_cpus, &tz->cpus);
		cl++;
	}
	egg.thermal_level = get_thermal_level(cur_idx);

	/* update freq/max_freq throttle and overutilization */
	egg.max_freq_throttle = false;
	egg.freq_throttle = false;
	egg.task_overutil = false;
	egg.gpu_max_freq_throttle = false;
	cpumask_clear(&visit_cpus);
	for_each_cpu(cpu, cpu_active_mask)
	{
		if (cpumask_test_cpu(cpu, &visit_cpus))
			continue;

		if ((ems_get_actual_cpu_capacity(cpu) * 1024) <= (heaviest_task_util * 1280))
			egg.task_overutil = true;

		if ((get_avg_cur_freq(cpu) * 120) > (get_avg_max_freq(cpu) * 100))
			egg.freq_throttle = true;

		if ((get_avg_max_freq(cpu) * 100) < (get_orig_max_freq(cpu) * egg.max_freq_throttle_ratio))
			egg.max_freq_throttle = true;

		cpumask_or(&visit_cpus, &visit_cpus, cpu_clustergroup_mask(cpu));
	}

	if ((get_avg_gpu_cur_freq() * 100) < (get_orig_gpu_min_freq() * 110))
			egg.gpu_max_freq_throttle = true;

	egg.demand_cpu = dct_get_demand_cpu();

	update_egg_boost();

	mode = determine_op_mode();
	sub_mode = determine_sub_mode(mode);

	check_egg_stop_condition();
	stopped = egg_stopped();

	trace_clock_set_rate("EGG op_mode", mode, stopped);
	trace_egg_update_op_mode(mode, sub_mode, stopped, egg.frame_drop, egg.thermal_level,
			egg.freq_throttle, egg.max_freq_throttle, egg.task_overutil,
			egg.demand_cpu, egg.fps[cur_idx], egg.target[cur_idx], &temp[0]);

	egg.last_idx = move_idx(cur_idx, 1, TEMP_HIST_SIZE);
	egg.op_mode = mode;
	egg.sub_mode = sub_mode;
}

static void egg_work(struct kthread_work *work)
{
	if (egg.enabled == EGG_DISABLED)
		return;

	profiler_update(egg.update_period);

	update_op_mode();

	if (!egg_enabled())
		return;

	update_game_pago();

	cleave_update();

	dct_update(egg.op_mode);
}

/********************************************************************************
 *				Vendor Hooks					*
 ********************************************************************************/
static void egg_irq_work(struct irq_work *irq_work)
{
	kthread_queue_work(&egg.worker, &egg.work);
}

#define UPDATE_MARGIN          ((NSEC_PER_SEC / HZ) >> 1)
static bool egg_need_update(ktime_t now)
{
	ktime_t update_period = egg.update_period;

	if (now - egg.last_jiffies + UPDATE_MARGIN < ms_to_ktime(update_period))
		return false;

	return true;
}

void ems_game_update(void)
{
	ktime_t now = ktime_get();

	if (egg.enabled == EGG_DISABLED)
		return;

	if (!egg_need_update(now)) {
		if (egg_profile_enabled()) {
			dct_get_feedback();
			gpu_freq_prof_update();
			try_to_disable_egg_boost();
		}
		return;
	}

	irq_work_queue(&egg.irq_work);

	egg.last_jiffies = now;
}

void ems_game_cgroup_attach(struct cgroup_taskset *tset)
{
	profiler_cgroup_attach(tset);
}

void ems_game_task_switch(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	if (!egg_profile_enabled())
		return;

	profiler_task_switch(rq, prev, next);
}

void ems_game_tick_entry(struct rq *rq)
{
	if (!egg_profile_enabled())
		return;

	profiler_tick_entry(rq);
}

void ems_game_try_to_wake_up(struct task_struct *p)
{
	if (!egg_profile_enabled()) {
		ems_task_reserved_cpu_wo_boost(p) = -1;
		egg_task_idx(p) = -1;
		return;
	}

	profiler_try_to_wake_up(p);
	egg_assign_reserved_cpu(p);
}

void ems_game_task_init(struct task_struct *p)
{
	profiler_init_task(p);
}

void ems_game_new_task_stats(struct task_struct *p)
{
	if (!egg_profile_enabled())
		return;

	profiler_new_task_stats(p);
}

void ems_game_flush_task(struct task_struct *p)
{
	egg_flush_task(p);
	profiler_flush_task(p);
}

void ems_game_cpu_frequency_limits(struct cpufreq_policy *policy)
{
	if (!egg_profile_enabled())
		return;

	profiler_cpu_frequency_limits(policy);
}

void ems_game_cpufreq_transition(struct cpufreq_policy *policy)
{
	if (!egg_profile_enabled())
		return;

	profiler_cpufreq_transition(policy);
}

int egg_eb_register(struct egg_set_over_freq *fn)
{
	if (!egg.eb.fb) {
		egg.eb.fb = fn;
		return 0;
	}
	return -1;
}
EXPORT_SYMBOL_GPL(egg_eb_register);

/********************************************************************************
 *				SYSFS						*
 ********************************************************************************/

static ssize_t show_update_boost_lv(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 3, "%d\n", egg.eb.cur_boost_lv);
}
static ssize_t store_update_boost_lv(struct kobject *kobj,
				     struct kobj_attribute *attr, const char *buf, size_t count)
{
	s32 input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	egg.eb.cur_boost_lv = input;
	return count;
}
static struct kobj_attribute update_boost_lv_attr =
	__ATTR(boost_lv, 0644, show_update_boost_lv, store_update_boost_lv);

static ssize_t show_update_big_boost(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 1024, "eanbled=%d\n", egg.eb.big_boost.enabled);
}
static ssize_t store_update_big_boost(struct kobject *kobj,
				      struct kobj_attribute *attr, const char *buf, size_t count)
{
	s32 input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	update_big_boost(0, !!input, true, EB_SYSFS);
	return count;
}
static struct kobj_attribute update_big_boost_attr =
	__ATTR(big_boost, 0644, show_update_big_boost, store_update_big_boost);

static ssize_t show_update_dsu_boost(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 1024, "eanbled=%d\n", egg.eb.dsu_boost.enabled);
}
static ssize_t store_update_dsu_boost(struct kobject *kobj,
				      struct kobj_attribute *attr, const char *buf, size_t count)
{
	s32 input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	update_dsu_boost(0, !!input, EB_SYSFS);
	return count;
}
static struct kobj_attribute update_dsu_boost_attr =
	__ATTR(dsu_boost, 0644, show_update_dsu_boost, store_update_dsu_boost);

static ssize_t show_update_mif_boost(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 3, "%d\n", egg.eb.mif_boost.enabled);
}
static ssize_t store_update_mif_boost(struct kobject *kobj,
				      struct kobj_attribute *attr, const char *buf, size_t count)
{
	s32 input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	update_mif_boost(0, !!input, EB_SYSFS);
	return count;
}
static struct kobj_attribute update_mif_boost_attr =
	__ATTR(mif_boost, 0644, show_update_mif_boost, store_update_mif_boost);

static ssize_t show_need_hf_freq_boost(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 1024, "enabled=%d\n", egg.eb.hf_freq_boost_enabled);
}
static ssize_t store_need_hf_freq_boost(struct kobject *kobj,
				      struct kobj_attribute *attr, const char *buf, size_t count)
{
	s32 input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	egg.eb.hf_freq_boost_enabled = !!input;
	return count;
}
static struct kobj_attribute need_hf_freq_boost_attr =
	__ATTR(hf_freq_boost, 0644, show_need_hf_freq_boost, store_need_hf_freq_boost);

static ssize_t show_update_period(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%llu\n", egg.update_period);
}

static ssize_t store_update_period(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	u64 input;

	if (sscanf(buf, "%llu", &input) != 1)
		return -EINVAL;

	egg.update_period = input;
	update_fps_profile_time_margin();

	return count;
}

static struct kobj_attribute update_period_attr =
__ATTR(update_period, 0644, show_update_period, store_update_period);

static ssize_t show_egg_enable(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 1024, "enabled=%d stop=%d (fic=%d, lfc=%d, nr_major=%d)\n",
			egg.enabled, egg.stop, frame_info_cnt, low_fps_cnt, egg.nr_major);
}

static ssize_t store_egg_enable(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	if (input < 0)
		return -EINVAL;

	if (!egg.initialized)
		return -EINVAL;

	reset_cpu_profiler();
	reset_cleave();
	reset_dct(!!input);
	reset_reserved_cpus();
	reset_game_pago(!!input);
	egg_reset_boost(!!input);
	dynamic_control_governor(GOV_CTRL_BY_EGG, !input);
	egg.enabled = !!input;
	egg.stop = !input;
	egg.type = EGT_ERROR;
	set_shader_compile_task(0);
	egg_notify(egg.enabled, EGG_SYSFS_CONTROL);

	return count;
}

static struct kobj_attribute egg_enable_attr =
__ATTR(enable, 0644, show_egg_enable, store_egg_enable);

/* TEST */
static ssize_t show_egg_sched_policy(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int ret = 0;

	ret += snprintf(buf + ret, 70, "current sched_policy: %d\n", egg.user_sched_policy);
	ret += snprintf(buf + ret, 70, "[0]: Governor\n");
	ret += snprintf(buf + ret, 70, "[1]: Performance\n");
	ret += snprintf(buf + ret, 70, "[2]: Energy\n");

	return ret;
}

static ssize_t store_egg_sched_policy(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	egg.user_sched_policy = input;

	return count;
}

static struct kobj_attribute egg_sched_policy_attr =
__ATTR(sched_policy, 0644, show_egg_sched_policy, store_egg_sched_policy);

static ssize_t show_high_temp(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%d\n", egg.high_temp);
}

static ssize_t store_high_temp(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	egg.high_temp = input;

	return count;
}

static struct kobj_attribute high_temp =
__ATTR(high_temp, 0644, show_high_temp, store_high_temp);

static ssize_t show_low_temp(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%d\n", egg.low_temp);
}

static ssize_t store_low_temp(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	egg.low_temp = input;

	return count;
}

static struct kobj_attribute low_temp =
__ATTR(low_temp, 0644, show_low_temp, store_low_temp);

static ssize_t show_lb_margin(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%d\n", egg.lb_margin);
}

static ssize_t store_lb_margin(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;
	unsigned long flags;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	raw_spin_lock_irqsave(&egg.reserve_lock, flags);
	egg.lb_margin = input;
	raw_spin_unlock_irqrestore(&egg.reserve_lock, flags);

	return count;
}

static struct kobj_attribute lb_margin_attr =
__ATTR(lb_margin, 0644, show_lb_margin, store_lb_margin);

static ssize_t show_max_freq_throttle_ratio(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%d\n", egg.max_freq_throttle_ratio);
}

static ssize_t store_max_freq_throttle_ratio(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	egg.max_freq_throttle_ratio = input;

	return count;
}

static struct kobj_attribute max_freq_throttle_ratio_attr =
__ATTR(max_freq_throttle_ratio, 0644, show_max_freq_throttle_ratio, store_max_freq_throttle_ratio);

static ssize_t show_execution_cluster(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int ret = 0;

	ret += snprintf(buf + ret, 50, "[slow_cluster]\n");
	ret += snprintf(buf + ret, 50, "mask:%*pbl\n", cpumask_pr_args(&egg.execution_slow_cpus));
	ret += snprintf(buf + ret, 50, "idx:%d\n", exec_slow_cl_idx);

	ret += snprintf(buf + ret, 50, "[fast_cluster]\n");
	ret += snprintf(buf + ret, 50, "mask:%*pbl\n", cpumask_pr_args(&egg.execution_fast_cpus));
	ret += snprintf(buf + ret, 50, "idx:%d\n", exec_fast_cl_idx);

	return ret;
}

static struct kobj_attribute execution_cluster_attr =
__ATTR(execution_cluster, 0444, show_execution_cluster, NULL);

static ssize_t show_important_task(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%d\n", egg.important_task);
}

static void release_prev_important(int pid)
{
	struct task_struct *task;

	task = get_pid_task(find_vpid(pid), PIDTYPE_PID);
	if (!task)
		return;

	struct egg_task_struct *ets = get_egg_task_struct(task);
	if (ets) {
		ets->important = 0;
		ets->important_tex = 0;
	}

	ems_low_latency_task(task) = 0;
	put_task_struct(task);
}

static ssize_t store_important_task(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input, prev;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	if (input < 0)
		input = -1;

	prev = egg.important_task;

	if (prev == input)
		return count;

	if (prev != -1)
		release_prev_important(prev);

	egg.important_task = input;

	trace_egg_important_task(prev, egg.important_task);

	return count;
}

static struct kobj_attribute important_task_attr =
__ATTR(important_task, 0644, show_important_task, store_important_task);

static ssize_t show_shader_compile_task(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%d\n", egg.shader_compile_task);
}

static ssize_t store_shader_compile_task(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	pid_t pid;

	if (sscanf(buf, "%d", &pid) != 1)
		return -EINVAL;

	if (pid < 0)
		return -EINVAL;

	update_shader_compile_task(pid);

	return count;
}

static struct kobj_attribute shader_compile_task_attr =
__ATTR(shader_compile_task, 0644, show_shader_compile_task, store_shader_compile_task);

static int egg_sysfs_create(struct kobject *ems_kobj)
{
	egg.kobj = kobject_create_and_add("egg", ems_kobj);
	if (!egg.kobj)
		return -ENOMEM;

	egg.update_period = DEFAULT_TARGET_PERIOD;
	update_fps_profile_time_margin();
	if (sysfs_create_file(egg.kobj, &update_period_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &egg_enable_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &egg_sched_policy_attr.attr))
		return -ENOMEM;

	egg.high_temp = DEFAULT_HIGH_TEMP;
	if (sysfs_create_file(egg.kobj, &high_temp.attr))
		return -ENOMEM;

	egg.low_temp = DEFAULT_LOW_TEMP;
	if (sysfs_create_file(egg.kobj, &low_temp.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &lb_margin_attr.attr))
		return -ENOMEM;

	egg.max_freq_throttle_ratio = DEFAULT_MAX_FREQ_THROTTLE_RATIO;
	if (sysfs_create_file(egg.kobj, &max_freq_throttle_ratio_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &execution_cluster_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &important_task_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &shader_compile_task_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &update_mif_boost_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &update_dsu_boost_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &update_big_boost_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &update_boost_lv_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(egg.kobj, &need_hf_freq_boost_attr.attr))
		return -ENOMEM;

	return 0;
}

static int egg_kthread_create(void)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO / 2 };

	init_irq_work(&egg.irq_work, egg_irq_work);
	kthread_init_work(&egg.work, egg_work);
	kthread_init_worker(&egg.worker);

	thread = kthread_create(kthread_worker_fn, &egg.worker, "egg");
	if (IS_ERR(thread)) {
		pr_err("failed to create egg thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	if (sched_setscheduler_nocheck(thread, SCHED_FIFO, &param)) {
		kthread_stop(thread);
		pr_err("failed to set SCHED_FIFO\n");
		return -1;
	}
	cpumask_copy(&egg.thread_allowed_cpus, cpu_slowest_mask());

	set_cpus_allowed_ptr(thread, &egg.thread_allowed_cpus);
	thread->flags |= PF_NO_SETAFFINITY;
	egg.thread = thread;

	wake_up_process(egg.thread);

	return 0;
}

static void egg_major_task_init(void)
{
	int i;

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		struct egg_task_struct *ets = &egg.major_task[i];

		raw_spin_lock_init(&ets->task_lock);
		clear_egg_task_struct(ets);
	}

	egg.nr_major = 0;
	egg.nr_near_heaviest = 0;
}

int ems_game_late_init(struct kobject *ems_kobj)
{
	struct device_node *np, *child;
	int num_of_cl = 0;
	int cpu;

	np = of_find_node_by_path("/power-data/cpu");
	if (!np)
		return -ENODATA;

	for_each_child_of_node(np, child) {
		struct eg_thermal_zone *tz;
		const char *buf;

		tz = kzalloc(sizeof(struct eg_thermal_zone), GFP_KERNEL);
		if (unlikely(!tz))
			goto fail;

		if (of_property_read_string(child, "cpus", &buf)) {
			pr_info("%s: failed to parse cpus\n", __func__);
			goto fail;
		}
		cpulist_parse(buf, &tz->cpus);

		if (of_property_read_string(child, "tz-name", &buf)) {
			pr_info("%s: failed to parse tz-name\n", __func__);
			goto fail;
		}

		tz->tz_dev = thermal_zone_get_zone_by_name(buf);
		if (IS_ERR(tz->tz_dev)) {
			pr_info("%s: failed to get tz-zone\n", __func__);
			tz->tz_dev = NULL;
			goto fail;
		}

		for_each_cpu(cpu, &tz->cpus)
			egg.tz[cpu] = tz;
		num_of_cl++;
	}

	egg.user_sched_policy = 0;
	egg.num_of_cl = num_of_cl;
	egg.important_task = -1;

	return 0;

fail:
	for_each_possible_cpu(cpu) {
		if (egg.tz[cpu])
			kfree(egg.tz[cpu]);
	}
	return -1;
}

static int egg_parse_dt(void)
{
	struct device_node *dn;
	const char *buf;
	int cpu;

	dn = of_find_node_by_path("/ems/egg");
	if (!dn) {
		pr_err("%s: failed to find device node\n", __func__);
		return -EINVAL;
	}

	if (of_property_read_u32(dn, "lb-margin", &egg.lb_margin))
		egg.lb_margin = 0;

	if (!of_property_read_string(dn, "exec-fast-cpus", &buf)) {
		ems_cpulist_parse(buf, &egg.execution_fast_cpus);
		cpu = cpumask_first(&egg.execution_fast_cpus);
		if (valid_cpu(cpu))
			exec_fast_cl_idx = mlt_cluster_index(cpu);
	}

	if (!of_property_read_string(dn, "exec-slow-cpus", &buf)) {
		ems_cpulist_parse(buf, &egg.execution_slow_cpus);
		cpu = cpumask_first(&egg.execution_slow_cpus);
		if (valid_cpu(cpu))
			exec_slow_cl_idx = mlt_cluster_index(cpu);
	}

	egg.initialized = true;

	return 0;
}

static void egg_init(struct kobject *ems_kobj)
{
	egg.initialized = false;
	egg.enabled = EGG_DISABLED;
	egg.eb.mif_boost.enabled = true;
	egg.eb.dsu_boost.enabled = true;
	egg.eb.big_boost.enabled = true;
	egg.eb.hf_freq_boost_enabled = false;
	egg.eb.cur_boost_lv = DEFAULT_BOOST_LV;
	egg.eb.mif_boost_lv = DEFAULT_BOOST_LV;
	egg.eb.dsu_boost_lv = DEFAULT_BOOST_LV;
	egg.eb.boosted_task = -1;

	egg_parse_dt();
	egg_sysfs_create(ems_kobj);
	egg_kthread_create();
	egg_major_task_init();
	egg_reserved_cpus_init();
	egg_init_boost();

	egg.training_start_ns = 0;
	egg.training_period = 1000 * NSEC_PER_MSEC;
	egg.execution_start_ns = 0;
	egg.execution_period = 20000 * NSEC_PER_MSEC;

	raw_spin_lock_init(&egg.game_pago_lock);

	pr_info("EMS: EGG Initialized\n");
}

void ems_game_sched_init(struct platform_device *pdev)
{
	struct kobject *ems_kobj = &pdev->dev.kobj;

	egg_init(ems_kobj);
	profiler_init(egg.kobj);
	cleave_init(egg.kobj);
	dct_init(egg.kobj);

	return;
}
