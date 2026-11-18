/*
 * Scheduling status profiler for Exynos Mobile Scheduler
 *
 * Copyright (C) 2021 Samsung Electronics Co., Ltd
 * Park Choonghoon <choong.park@samsung.com>
 */

#include <soc/samsung/ems.h>

#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

#ifdef CONFIG_SOC_S5E9965
#include <soc/samsung/exynos-sci.h>
#endif

static struct system_profile_data *system_profile_data;
static DEFINE_RWLOCK(profile_sched_lock);
#define get_cpu_profile(cpu, profile)	(&system_profile_data->cp[cpu][profile]);

/*
 * Time thresholds for task-burst situation
 *
 * Since the num of new tasks in system_profile_data is less than THRES_NEW_TASK_COUNT,
 * It is decreased once every NORMAL_NEW_TASK_TIME_THRES(4ms).
 *
 * However, if the num of new tasks exceeds THRES_NEW_TASK_COUNT,
 * the system is regarded as being in a task-burst situation.
 * Therefore, not only sysbusy is set to STATE3,
 * but the count of new tasks is also decreased once every BOOSTED_NEW_TASK_TIME_THRES(20ms).
 */
#define BOOSTED_NEW_TASK_TIME_THRES		(20000000)
#define NORMAL_NEW_TASK_TIME_THRES		(4000000)

char *fair_causes_name[END_OF_FAIR_CAUSES] = {
	"no-candidate",
	"only-one-candidate",
	"no-fit-cpu",
	"only-one-fit-cpu",
	"sysbusy",
	"task-express",
	"lowest-energy",
	"performance",
	"sync",
	"fast-track",
	"reserved",
	"fallback",
	"na"
};

struct sched_stat {
	unsigned int fair_sum;
	unsigned int rt_sum;
	unsigned int fair[END_OF_FAIR_CAUSES];
};
static DEFINE_PER_CPU(struct sched_stat, stats);

static unsigned int not_selected_fair;
static unsigned int not_selected_rt;

int heavy_pid;
u64 heavy_task_util;
/*
 * Profile heavy task boost tunable parameter
 *
 * Start boost when operating (short_htask_ratio / 10)% of (window * 4 msec)
 * default : 90% of 8 msec
 */
#define DEFAULT_LONG_RATIO 700
#define DEFAULT_SHORT_RATIO 900
#define DEFAULT_WINDOW 2
struct htask_param {
	int long_heavy_task_ratio;	/* default : 70% */
	int short_heavy_task_ratio;	/* default : 90% */
	int htask_check_window_count;	/* default : 2	 */
};
static DEFINE_PER_CPU(struct htask_param, ht_params);

void update_fair_stat(int cpu, enum fair_causes i)
{
	struct sched_stat *stat;

	if (cpu < 0) {
		not_selected_fair++;
		return;
	}

	stat = per_cpu_ptr(&stats, cpu);

	if (!stat)
		return;

	stat->fair_sum++;
	stat->fair[i]++;
}

void update_rt_stat(int cpu)
{
	struct sched_stat *stat;

	if (cpu < 0) {
		not_selected_rt++;
		return;
	}

	stat = per_cpu_ptr(&stats, cpu);

	if (!stat)
		return;

	stat->rt_sum++;
}

static ssize_t sched_stat_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int value, cpu;

	if (sscanf(buf, "%d", &value) != 1)
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		struct sched_stat *stat = per_cpu_ptr(&stats, cpu);

		memset(stat, 0, sizeof(struct sched_stat));
	}

	return count;
}

static ssize_t sched_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sched_stat *stat;
	int ret = 0;
	int cpu, i;
	char *line = "--------------------------------------------------"
		"--------------------------------------------------"
		"---------------------";

	ret += sprintf(buf + ret, "%s\n", line);
	ret += sprintf(buf + ret, "%20s | ", "FAIR schedule");
	for_each_possible_cpu(cpu)
		ret += sprintf(buf + ret, "      CPU%d ", cpu);
	ret += sprintf(buf + ret, "\n%s\n", line);

	for (i = 0; i < END_OF_FAIR_CAUSES; i++) {
		ret += sprintf(buf + ret, "%20s | ", fair_causes_name[i]);
		for_each_possible_cpu(cpu) {
			stat = per_cpu_ptr(&stats, cpu);
			ret += sprintf(buf + ret, "%10u ", stat->fair[i]);
		}
		ret += sprintf(buf + ret, "\n");
	}

	ret += sprintf(buf + ret, "%s\n", line);
	ret += sprintf(buf + ret, "%20s | ", "total");
	for_each_possible_cpu(cpu) {
		stat = per_cpu_ptr(&stats, cpu);
		ret += sprintf(buf + ret, "%10u ", stat->fair_sum);
	}
	ret += sprintf(buf + ret, "\n%s\n", line);
	ret += sprintf(buf + ret, "not_selected: %u\n\n", not_selected_fair);

	ret += sprintf(buf + ret, "%s\n", line);
	ret += sprintf(buf + ret, "%20s | ", "RT schedule");
	for_each_possible_cpu(cpu)
		ret += sprintf(buf + ret, "      CPU%d ", cpu);

	ret += sprintf(buf + ret, "\n%s\n", line);
	ret += sprintf(buf + ret, "%20s | ", "total");
	for_each_possible_cpu(cpu) {
		stat = per_cpu_ptr(&stats, cpu);
		ret += sprintf(buf + ret, "%10u ", stat->rt_sum);
	}
	ret += sprintf(buf + ret, "\n%s\n", line);
	ret += sprintf(buf + ret, "not_selected: %u\n\n", not_selected_rt);

	return ret;
}
DEVICE_ATTR_RW(sched_stat);

/****************************************************************
 *		SYSBUSY base on Active Ratio			*
 ****************************************************************/
static bool is_longterm_htask(struct task_struct *p)
{
	int ratio, cpu = task_cpu(p);
	int grp = cpuctl_task_group_idx(p);
	struct htask_param *ht_param = per_cpu_ptr(&ht_params, cpu);
	bool htask_util, htask_ratio;

	if (grp != CGROUP_TOPAPP)
		return false;

	ratio = mlt_task_value(p, MLT_PERIOD_COUNT, AVERAGE);

	htask_ratio = ratio >= ht_param->long_heavy_task_ratio;
	htask_util = is_misfit_task_util(ml_task_util_est(p));

	return htask_ratio || htask_util;
}

/****************************************************************
 *		HEAVY TASK base on Active Ratio		*
 ****************************************************************/

/* heavy task should be included in TOP-APP or FOREGOUND */
static inline bool htask_enabled_grp(struct task_struct *p)
{
	int grp = cpuctl_task_group_idx(p);

	if (grp != CGROUP_TOPAPP && grp != CGROUP_FOREGROUND)
		return false;

	return true;
}

static bool is_heavy_task(struct task_struct *p)
{
	int ratio, cpu = task_cpu(p);
	struct htask_param *ht_param = per_cpu_ptr(&ht_params, cpu);

	if (!htask_enabled_grp(p) || egg_enabled())
		return false;

	ratio = mlt_task_value(p, ht_param->htask_check_window_count, AVERAGE);

	return ratio >= ht_param->short_heavy_task_ratio;
}

/* get_htask_ratio - if this task is heavy task, return heaviness ratio */
static int get_htask_ratio(struct task_struct *p)
{
	if (!is_heavy_task(p))
		return 0;

	return mlt_task_value(p, MLT_PERIOD_COUNT, AVERAGE);
}

static void profile_update_cpu_htask(int cpu, unsigned long hratio, unsigned long pid)
{
	struct cpu_profile *cs = get_cpu_profile(cpu, CPU_HTSK);

	cs->value = hratio;
	cs->data = pid;
}

static bool profile_has_htask(struct rq *rq, struct task_struct *skip)
{
	struct task_struct *p;
	int loop = 10;

	if (!rq->cfs.h_nr_running)
		return false;

	list_for_each_entry(p, &rq->cfs_tasks, se.group_node) {
		if (!loop)
			break;

		if (p == skip)
			continue;

		if (egg_enabled() && invalid_egg_index(egg_task_idx(p)))
			continue;

		if (is_heavy_task(p)) {
			ems_rq_htask(rq) = (u64)p;
			ems_rq_htask_load(rq) = mlt_latest_task_load(p);
			return true;
		}

		loop--;
	}

	return false;
}

/*
 * profile_enqueue_task
 * check whether enqueued task is heavy or not, if it is true,
 * apply htask_ratio to this rq immediately to increase frequency latency
 */
void profile_enqueue_task(struct rq *rq, struct task_struct *p, int *flags)
{
	if (!is_heavy_task(p))
		return;

	ems_rq_htask(rq) = (u64)p;
	ems_rq_htask_load(rq) = mlt_latest_task_load(p);

	trace_profile_htask(cpu_of(rq), p, "enqueue");

	*flags |= HEAVY_TASK;
}

void profile_dequeue_task(struct rq *rq, struct task_struct *p, int *flags)
{
	struct task_struct *htask = (struct task_struct *)ems_rq_htask(rq);

	if (p != htask)
		return;

	ems_rq_htask(rq) = 0;
	ems_rq_htask_load(rq) = 0;

	trace_profile_htask(cpu_of(rq), p, "dequeue");

	if(profile_has_htask(rq, p)) {
		htask = (struct task_struct *)ems_rq_htask(rq);
		trace_profile_htask(cpu_of(rq), htask, "already-enqueued");
		*flags |= HEAVY_TASK;
	}
}

void profile_update_htask(struct rq *rq, int *flags)
{
	ems_rq_htask(rq) = 0;
	ems_rq_htask_load(rq) = 0;

	if (profile_has_htask(rq, NULL)) {
		struct task_struct *htask = (struct task_struct *)ems_rq_htask(rq);
		trace_profile_htask(cpu_of(rq), htask, "tick");
		*flags |= HEAVY_TASK;
	}
}

int profile_get_htask_ratio(int cpu)
{
	struct cpu_profile *cp = get_cpu_profile(cpu, CPU_HTSK);
	return cp->value;
}

int profile_heavy_task(u64 *task_util)
{
	*task_util = heavy_task_util;
	return heavy_pid;
}

int avg_nr_margin = MLT_RUNNABLE_ROUNDS_UNIT / 2;
static int get_avg_nr_heavy(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	int avg_nr;

	if (cpumask_test_cpu(cpu, cpu_slowest_mask()))
		return ems_rq_nr_misfited(rq);

	avg_nr = mlt_topapp_runnable(cpu);
	return (avg_nr + avg_nr_margin) / MLT_RUNNABLE_UNIT;
}

static ssize_t avg_nr_margin_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int value;

	if (sscanf(buf, "%d", &value) != 1)
		return -EINVAL;

	avg_nr_margin = value;

	return count;
}

static ssize_t avg_nr_margin_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;

	ret += sprintf(buf + ret, "%d\n", avg_nr_margin);

	return ret;
}
DEVICE_ATTR_RW(avg_nr_margin);

/****************************************************************
 *			CPU UTIL				*
 ****************************************************************/
static void profile_update_cpu_util(int cpu, int *busy_cnt, unsigned long *util_sum)
{
	int cpu_util;
	struct cpu_profile *cs = get_cpu_profile(cpu, CPU_UTIL);

	cpu_util = ml_cpu_util(cpu) + cpu_util_rt(cpu_rq(cpu));
	(*util_sum) += cpu_util;

	if (check_busy(cpu_util, ems_get_actual_cpu_capacity(cpu))) {
		cs->data = PROFILE_CPU_BUSY;
		(*busy_cnt) += 1;
	} else {
		cs->data = PROFILE_CPU_IDLE;
	}

	cs->value = cpu_util;
}

/****************************************************************
 *			External APIs				*
 ****************************************************************/
static u64 last_profile_time;
static int profile_interval = 1; /* 1 tick = 4ms */
static int new_task_profile_time = 0;
int profile_sched_data(void)
{
	unsigned long flags;
	unsigned long now = jiffies;
	unsigned long cpu_util_sum = 0;
	unsigned long heavy_task_util_sum = 0;
	int avg_nr_run_sum = 0;
	int avg_nr_heavy_sum = 0;
	int avg_nr_sustain_high_sum = 0;
	int busy_cpu_count = 0;
	int heavy_task_count = 0;
	int cpu, pid = -1;
	int new_task_time_thres = NORMAL_NEW_TASK_TIME_THRES;
	u64 now_clock = sched_clock();
	u64 heavy_util = 0;

	if (!write_trylock_irqsave(&profile_sched_lock, flags))
		return -EBUSY;

	if (now < last_profile_time + profile_interval)
		goto unlock;

	last_profile_time = now;

	for_each_cpu(cpu, cpu_active_mask) {
		struct rq *rq = cpu_rq(cpu);
		struct ems_dslt_rq *drq = ems_rq_dslt(rq);
		struct task_struct *p;
		unsigned long task_util;
		u64 max_hratio = 0, max_hratio_pid = 0;
		int track_count;
		int avg_nr_heavy;
		int avg_nr_sustain_high;

		profile_update_cpu_util(cpu, &busy_cpu_count, &cpu_util_sum);

		raw_spin_rq_lock(rq);

		/* Explictly clear count */
		track_count = 0;
		avg_nr_run_sum += mlt_runnable(cpu);

		avg_nr_heavy = get_avg_nr_heavy(cpu);
		avg_nr_heavy_sum += avg_nr_heavy;
		avg_nr_sustain_high = (mlt_sustain_high_runnable(cpu) + MLT_RUNNABLE_UP_UNIT)
				/ MLT_RUNNABLE_UNIT;
		avg_nr_sustain_high_sum += avg_nr_sustain_high;

		list_for_each_entry(p, &rq->cfs_tasks, se.group_node) {
			/* update heavy task base on active ratio */
			u64 task_hratio = get_htask_ratio(p);
			if (task_hratio > max_hratio) {
				max_hratio_pid = p->pid;
				max_hratio = task_hratio;
			}

			/* update heavy task base on cpu util */
			task_util = ml_task_util(p);
			if (is_longterm_htask(p)) {
				heavy_task_util_sum += task_util;
				heavy_task_count++;
			}

			if ((heavy_util < task_util) && is_reserved_task(p)) {
				heavy_util = task_util;
				pid = p->pid;
			}

			if (++track_count >= TRACK_TASK_COUNT)
				break;
		}

		/* save heaviest task data */
		profile_update_cpu_htask(cpu, max_hratio, max_hratio_pid);

		trace_ems_cpu_profile(cpu, &system_profile_data->cp[cpu][0],
					rq->cfs.h_nr_running,
					rq->cfs.avg.runnable_avg,
					drq->avg.runnable_avg,
					avg_nr_heavy);

		raw_spin_rq_unlock(rq);
	}

	for_each_cpu(cpu, cpu_active_mask) {
		int capacity;

		if (cpumask_test_cpu(cpu, cpu_fastest_mask()))
			continue;

		capacity = ems_get_actual_cpu_capacity(cpu);
		if (!ems_cpu_overutilized(cpu, capacity, heavy_util)) {
			heavy_util = 0;
			pid = -1;
		}
	}
	heavy_pid = pid;
	heavy_task_util = heavy_util;

	if (heavy_pid != -1 && !egg_has_shader_compile_task() && egg_is_normal_game())
		update_big_boost(ktime_get(), true, true, EB_OVERUTIL);
	else
		update_big_boost(ktime_get(), false, false, EB_OVERUTIL_DEL);

	avg_nr_run_sum = (avg_nr_run_sum + MLT_RUNNABLE_UP_UNIT) / MLT_RUNNABLE_UNIT;;
	trace_ems_profile_tasks(busy_cpu_count, cpu_util_sum,
				heavy_task_count, heavy_task_util_sum,
				avg_nr_run_sum, avg_nr_heavy_sum, avg_nr_sustain_high_sum,
				heavy_util, heavy_pid);

	/* Fill profile data */
	system_profile_data->busy_cpu_count = busy_cpu_count;
	system_profile_data->heavy_task_count = heavy_task_count;
	system_profile_data->cpu_util_sum = cpu_util_sum;
	system_profile_data->avg_nr_run_sum = avg_nr_run_sum;
	system_profile_data->avg_nr_heavy_sum = avg_nr_heavy_sum;
	system_profile_data->heavy_task_util_sum = heavy_task_util_sum;
	system_profile_data->avg_nr_sustain_high_sum = avg_nr_sustain_high_sum;

	/* Decrease new_task_count per every new_task_time_thres */
	if (system_profile_data->new_task_count >= THRES_NEW_TASK_COUNT)
		new_task_time_thres = BOOSTED_NEW_TASK_TIME_THRES;

	if (!new_task_profile_time || (now_clock - new_task_profile_time) > new_task_time_thres) {
		new_task_profile_time = now_clock;

		if (system_profile_data->new_task_count)
			system_profile_data->new_task_count -= 1;
	}

unlock:
	write_unlock_irqrestore(&profile_sched_lock, flags);

	return 0;
}

/* Caller MUST disable irq before calling this function. */
void get_system_sched_data(struct system_profile_data *data)
{
	read_lock(&profile_sched_lock);
	memcpy(data, system_profile_data, sizeof(struct system_profile_data));
	read_unlock(&profile_sched_lock);
}

#define MAX_NEW_TASK_COUNT	(15)
void profile_accumulate_new_task_count(void)
{
	if (system_profile_data->new_task_count <= MAX_NEW_TASK_COUNT)
		system_profile_data->new_task_count += 1;
}

bool profile_get_sustain_high(void)
{
	return system_profile_data->avg_nr_sustain_high_sum >= 1;
}

#ifdef CONFIG_SOC_S5E9965
#define CRPCONTROL_BASE (0x0300B0000)

static bool sysbusy_perf_tune_enable = false;

static void write(void *addr, unsigned int bit, unsigned int mask, unsigned int value, unsigned int reg)
{
	reg &= (~((mask) << (bit)));
	reg |= (((value) & (mask)) << (bit));
	__raw_writel(reg, addr);
}

static void sysbusy_perf_tune(bool enable)
{
	bool prev_enable = sysbusy_perf_tune_enable;
	void *crp_control_base;
	int i = 0;

	if (prev_enable == enable)
		return;

	crp_control_base = ioremap(CRPCONTROL_BASE, 0x80);
	if (!crp_control_base) {
		pr_err("%s: ioremap failed\n", __func__);
		return;
	}

	if (enable) {
		llc_retention_en(0);

		for (i = 0; i < 4; i++) {
			void *addr = crp_control_base + 0x4 + (i * 0x28);
			unsigned int reg = __raw_readl(addr);

			write(addr, 29, 0x1, 0x0, reg);
			pr_debug("%s: enable %x=%x\n", __func__,
					CRPCONTROL_BASE + 0x4 + (i * 0x28), __raw_readl(addr));
		}
	} else {
		llc_retention_en(1);

		for (i = 0; i < 4; i++) {
			void *addr = crp_control_base + 0x4 + (i * 0x28);
			unsigned int reg = __raw_readl(addr);

			write(addr, 29, 0x1, 0x1, reg);
			pr_debug("%s: disable %x=%x\n", __func__,
					CRPCONTROL_BASE + 0x4 + (i * 0x28), __raw_readl(addr));
		}
	}

	sysbusy_perf_tune_enable = enable;
	iounmap(crp_control_base);
}
#endif

/****************************************************************
 *		  sysbusy state change notifier			*
 ****************************************************************/
static int profile_sysbusy_notifier_call(struct notifier_block *nb,
					unsigned long val, void *v)
{
	enum sysbusy_state state = *(enum sysbusy_state *)v;

	if (val != SYSBUSY_STATE_CHANGE)
		return NOTIFY_OK;

	profile_interval = sysbusy_monitor_interval(state);

#ifdef CONFIG_SOC_S5E9965
	if (state == SYSBUSY_STATE1 || state == SYSBUSY_STATE3) {
		sysbusy_perf_tune(true);
	} else {
		sysbusy_perf_tune(false);
	}
#endif

	return NOTIFY_OK;
}

static struct notifier_block profile_sysbusy_notifier = {
	.notifier_call = profile_sysbusy_notifier_call,
};

/****************************************************************
 *		  emstune htask change notifier			*
 ****************************************************************/
static int htask_emstune_notifier_call(struct notifier_block *nb,
		unsigned long val, void *v)
{
	struct emstune_set *cur_set = (struct emstune_set *)v;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct htask_param *ht_param = per_cpu_ptr(&ht_params, cpu);

		ht_param->short_heavy_task_ratio = cur_set->cpufreq_gov.htask_ratio_threshold[cpu];
		ht_param->htask_check_window_count = cur_set->cpufreq_gov.window_count[cpu];
	}

	return NOTIFY_OK;
}

static struct notifier_block htask_emstune_notifier = {
	.notifier_call = htask_emstune_notifier_call,
};

/****************************************************************
 *			Initialization				*
 ****************************************************************/
static void htask_emstune_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct htask_param *ht_param = per_cpu_ptr(&ht_params, cpu);

		ht_param->long_heavy_task_ratio = DEFAULT_LONG_RATIO;
		ht_param->short_heavy_task_ratio = DEFAULT_SHORT_RATIO;
		ht_param->htask_check_window_count = DEFAULT_WINDOW;
	}

	emstune_register_notifier(&htask_emstune_notifier);
}

int profile_sched_init(struct kobject *ems_kobj)
{
	system_profile_data =
		kzalloc(sizeof(struct system_profile_data), GFP_KERNEL);
	if (!system_profile_data) {
		pr_err("Failed to allocate profile_system_data\n");
		return -ENOMEM;
	}

	sysbusy_register_notifier(&profile_sysbusy_notifier);
	htask_emstune_init();

	if (sysfs_create_file(ems_kobj, &dev_attr_sched_stat.attr))
		pr_warn("failed to create sched_stat\n");

	if (sysfs_create_file(ems_kobj, &dev_attr_avg_nr_margin.attr))
		pr_warn("failed to create avg_nr_margin\n");

	return 0;
}
