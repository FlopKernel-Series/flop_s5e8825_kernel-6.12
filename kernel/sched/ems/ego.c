/*
 * EGO(Energy-Aware CPUFreq Governor) on Energy and Scheduler-Event.
 * Copyright (C) 2021, Samsung Electronic Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/sched/cpufreq.h>
#include <linux/cpu_pm.h>
#include <linux/cpufreq.h>
#include <trace/hooks/cpuidle.h>

#include <soc/samsung/ems.h>

#include <dt-bindings/soc/samsung/ems.h>
#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

#define IOWAIT_BOOST_MIN	(SCHED_CAPACITY_SCALE / 8)
#define HIST_SIZE		40
#define	RATIO_UNIT		1000
#define BOOST_RATE_LIMIT	(NSEC_PER_MSEC / 2)
#define DEFAULT_PELT_MARGIN (25)
#define DEFAULT_LATENCY (4)

enum {
	PARSE_INVALID = -1,
	PARSE_DONE,
	PARSE_EMPTY,
};

enum {
	ADAP_FMAX_NO_REASON,
	ADAP_FMAX_SYSBUSY,
	ADAP_FMAX_SUSTAIN_HIGH,
	ADAP_FMAX_CONSTANT_LOAD,
};

struct step_table {
	unsigned int	*pelt_margin;
	unsigned int	*margin_freq;
	unsigned int	*latency;
	unsigned int	*latency_freq;
	unsigned long	*raw_latency;
	unsigned long	*raw_pelt_margin;
};

struct step_latency_info {
	unsigned int	*latency_freq;
	unsigned int	*latency;
};

struct step_margin_info {
	unsigned int	*margin_freq;
	unsigned int	*pelt_margin;
};

struct ego_policy {
	struct cpufreq_policy	*policy;

	raw_spinlock_t		update_lock;	/* For shared policies */
	u64			last_freq_update_time;
	s64			freq_update_delay_ns;
	unsigned int		next_freq;	/* final target freq */
	unsigned int		cached_raw_freq;/* util based raw freq */
	unsigned int		org_freq;	/* util based freq in table  */
	unsigned int		eng_freq;	/* lowest energy freq */
	unsigned int		flags;

	unsigned int 		adap_fmax_enable;
	unsigned int		adap_fmax_enable_by_user;
	unsigned int		adap_fmax_freq;
	unsigned int		default_adap_fmax_freq;

	unsigned int		internal_freq_limit;
	/* Step parameters for step latency & margin */
	unsigned int		latency_step;
	unsigned int		latency_step_power;
	unsigned int		margin_step;
	unsigned int		margin_step_power;
	struct step_table			step_table;
	struct step_latency_info	step_latency_info;
	struct step_margin_info		step_margin_info;
	unsigned long		prev_util;	/* previous util is used for freq reversal */

	unsigned int		policy_table_size;

	/* The next fields are only needed if fast switch cannot be used: */
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	bool			limits_changed;
	bool			need_freq_update;
	bool			quick_rampup;
	s32			heavy_task_rate_limit;
	s32			uclamp_task_rate_limit;

	/* EGO specific */
	struct cpumask		cpus;
	struct cpumask		thread_allowed_cpus;
	int			heaviest_cpu;

	/* EGO tunables */
	unsigned int		ratio;
	int			dis_buck_share;	/* ignore buck-share when computing energy */
	int			pelt_boost;	/* dynamic changed boost */
	int			ptb_margin;	/* tunable boost */
	unsigned int		ptb_min_freq;
	int			pelt_margin;
	s64			up_rate_limit_ns;
	s64			down_rate_limit_ns;

	bool			build_somac_wall;
	unsigned int		somac_wall;

	unsigned int		adaptive_high_freq;
	unsigned int		adaptive_high_freq_kernel;
	unsigned int		adaptive_low_freq;
	unsigned int		adaptive_low_freq_kernel;

	unsigned int		fclamp_enable;

	unsigned int		margin_util_target;
	int			margin;
	unsigned int		margin_util_target_by_user;

	int			reason;

	bool			busy;

	struct kobject		kobj;
};

struct ego_cpu {
	struct update_util_data	update_util;
	struct ego_policy	*egp;
	unsigned int		cpu;

	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	unsigned long		bw_dl;
	unsigned long		max;

	unsigned long		util;		/* current pelt util */
	unsigned long		io_util;	/* current io util */
	unsigned long		boosted_util;	/* current boosted util */

	unsigned long		min_cap;

	int			boost_reason;
	unsigned long		uclamp_min;
	unsigned long		uclamp_max;
};

struct kobject *ego_kobj;
static DEFINE_PER_CPU(struct ego_cpu, ego_cpu);
struct cpufreq_governor energy_aware_gov;
static int step_latency_enable = 0;
static int step_margin_enable = 0;
static int adap_fmax_reason = ADAP_FMAX_NO_REASON;
static bool low_power_mode = false;

/*********************************************************************/
/*			    EGO step API			     */
/*********************************************************************/
static unsigned int ego_get_step_level(struct ego_policy *egp, unsigned int next_freq, int flag)
{
	int i;

	if (flag == STEP_LATENCY) {
		if (!step_latency_enable)
			return 0;

		for (i = 1; i < egp->latency_step; i++)
			if (next_freq < egp->step_latency_info.latency_freq[i])
				return i - 1;

		return egp->latency_step - 1;

	} else if (flag == STEP_PELT_MARGIN) {
		if (!step_margin_enable)
			return 0;

		for (i = 1; i < egp->margin_step; i++)
			if (next_freq < egp->step_margin_info.margin_freq[i])
				return i - 1;

		return egp->margin_step - 1;

	}

	return 0;
}

static int ego_get_step_power(struct ego_policy *egp, unsigned int step)
{
	int cpu = cpumask_first(&egp->cpus);

	return (et_max_dpower(cpu) - et_min_dpower(cpu)) / step;
}

/*
 * For calculating default step latency & margin,
 * gather dynamic power and capacity from energy table
 * and divide them(dp/cap) based on each steps.
 * We can get freq zones corresponding to each step powers.
 *
 * Calculate (dynamic power / capacity) averages for all freq zones.
 * After that, match the lowest freq zone of each clusters to the default value.
 * (default value: latency=4ms, pelt-margin=25%)
 * We know the average divided values(dp/cap) of each freq zones,
 * so we can scale the highest freq zone according to the smallest average.
 *
 * Now, we know the smallest and the biggest step value for each clusters.
 * And the rest step values will be set linearly.
 */
static void ego_calculate_step_info(struct ego_policy *egp)
{
	struct cpufreq_frequency_table *freq_table;
	struct cpufreq_policy *policy;
	unsigned long *dp_divide_cap;
	unsigned long dp, cap, sum = 0;
	int index = 1, count = 0, i;
	int cpu = cpumask_first(&egp->cpus);

	/*
	 * Allocate calculating elements and caculate latency_freq.
	 * Then, caculate dp / cap average value for each step
	 * to get step latencies.
	 */
	policy = cpufreq_cpu_get(cpu);

	egp = per_cpu(ego_cpu, cpu).egp;

	freq_table = policy->freq_table;
	egp->policy_table_size = et_table_size(cpu);
	dp = et_min_dpower(cpu);

	egp->latency_step_power =
		ego_get_step_power(egp, egp->latency_step);

	dp_divide_cap =
		kcalloc(egp->policy_table_size,
				sizeof(unsigned long), GFP_KERNEL);

	/* divide freq with step power */
	for (i = 0; i < egp->latency_step; i++) {
		egp->step_table.margin_freq[i] = egp->step_table.latency_freq[i] =
			et_dpower_to_freq(cpu, dp);
		dp += egp->latency_step_power;
	}

	/* calculate dp / cap average value for each step */
	for (i = 0; i < egp->policy_table_size; i++) {
		dp = et_freq_to_dpower(cpumask_first(&egp->cpus),
				freq_table[i].frequency);
		cap = et_freq_to_cap(cpumask_first(&egp->cpus),
				freq_table[i].frequency);

		dp *= SCHED_FIXEDPOINT_SCALE;

		if (freq_table[i].frequency ==
					egp->step_table.latency_freq[index]) {
			dp_divide_cap[index-1] = sum / count;
			index += 1; sum = 0; count = 0;

			if (index == egp->latency_step)
				break;
		}

		sum += (dp / cap);
		count += 1;
	}

	for (; i < egp->policy_table_size; i++) {
		dp = et_freq_to_dpower(cpumask_first(&egp->cpus),
				freq_table[i].frequency);
		cap = et_freq_to_cap(cpumask_first(&egp->cpus),
				freq_table[i].frequency);

		dp *= SCHED_FIXEDPOINT_SCALE;

		sum += (dp / cap);
		count += 1;
	}

	dp_divide_cap[egp->latency_step - 1] = sum / count;

	/* Calculate raw latencies and margins with dp / cap average */
	for (i = 0; i < egp->latency_step; i++) {
		egp->step_table.raw_latency[i] =
			(STEP_MIN_LATENCY * SCHED_FIXEDPOINT_SCALE) *
				dp_divide_cap[i] / dp_divide_cap[0];

		egp->step_table.raw_pelt_margin[i] =
			(STEP_MAX_MARGIN * SCHED_FIXEDPOINT_SCALE) *
				dp_divide_cap[0] / dp_divide_cap[i];
	}

	/* min latency will be always STEP_MIN_LATENCY(4ms) */
	egp->step_table.latency[0] =
		(int)(STEP_MIN_LATENCY * SCHED_FIXEDPOINT_SCALE);

	/* max margin will be always STEP_MAX_MARGIN(25%) */
	egp->step_table.pelt_margin[0] =
		(int)(STEP_MAX_MARGIN * SCHED_FIXEDPOINT_SCALE);

	kfree(dp_divide_cap);
}

static inline void ego_set_highest_step_val(unsigned int *step_val, unsigned long cur_step_raw_val,
					unsigned long big_step_raw_val, unsigned int step,
					unsigned int default_val, int flag)
{
	*step_val = (unsigned int)(cur_step_raw_val * default_val * SCHED_FIXEDPOINT_SCALE)
			/ big_step_raw_val;

	if (flag == STEP_LATENCY)
		*step_val = min(*step_val, default_val * SCHED_FIXEDPOINT_SCALE);
	else if (flag == STEP_PELT_MARGIN)
		*step_val = max(*step_val, default_val * SCHED_FIXEDPOINT_SCALE);
}

static inline void ego_set_remain_step_val(unsigned int *step_val,
                               unsigned int step, unsigned int step_min_val)
{
	int val, i;

	val = (int)step_val[step - 1] - (int)step_val[0];
	val = div_s64(val, step - 1);

	for (i = 1; i < step - 1; i++)
		step_val[i] = step_val[i - 1] + val;

	for (i = 0 ; i < step; i++) {
		step_val[i] /= SCHED_FIXEDPOINT_SCALE;
		step_val[i] = max(step_val[i], step_min_val);
	}
}

static void ego_adjust_step_info(struct ego_policy *max_egp)
{
	int cpu = 0;
	int last_latency_idx, last_margin_idx;
	struct ego_policy *egp;

	for_each_possible_cpu(cpu) {
		if (cpu != cpumask_first(cpu_clustergroup_mask(cpu)))
			continue;

		egp = per_cpu(ego_cpu, cpu).egp;

		last_latency_idx = egp->latency_step - 1;
		last_margin_idx = egp->margin_step - 1;

		/*
		 * For calculating step values(latency, pelt-margin),
		 * The value of the smallest freq range and the biggest freq range
		 * should be calculated first.
		 *
		 * These two values serve as reference points for linearly mapping
		 * values betwwen different freq ranges.
		 */
		ego_set_highest_step_val(&egp->step_table.latency[last_latency_idx],
			   egp->step_table.raw_latency[last_latency_idx],
			   max_egp->step_table.raw_latency[last_latency_idx],
			   egp->latency_step, STEP_MAX_LATENCY, STEP_LATENCY);

		ego_set_highest_step_val(&egp->step_table.pelt_margin[last_margin_idx],
			   egp->step_table.raw_pelt_margin[last_margin_idx],
			   max_egp->step_table.raw_pelt_margin[last_margin_idx],
			   egp->margin_step, STEP_MIN_MARGIN, STEP_PELT_MARGIN);

		ego_set_remain_step_val(egp->step_table.latency,
			  egp->latency_step, STEP_MIN_LATENCY);

		ego_set_remain_step_val(egp->step_table.pelt_margin,
			  egp->margin_step, STEP_MIN_MARGIN);

		kfree(egp->step_table.raw_latency);
		kfree(egp->step_table.raw_pelt_margin);
	}
}

/*********************************************************************/
/*			EGO Specific Implementation		     */
/*********************************************************************/
/* returns wether cpufreq governor is EGO or NOT */
static inline bool ego_is_working(struct ego_policy *egp)
{
	return ((likely(egp)) && (likely(egp->policy)) && (egp->policy->governor_data == egp));
}

/* compute freq level diff between cur freq and given freq */
static unsigned int get_diff_num_levels(struct cpufreq_policy *policy, unsigned int freq)
{
	int index1, index2;

	index1 = cpufreq_frequency_table_get_index(policy, policy->cur);
	index2 = cpufreq_frequency_table_get_index(policy, freq);

	return abs(index1 - index2);
}

static inline bool adap_fmax_enabled(struct ego_policy *egp)
{
	if (!egp)
		return false;

	if (egg_enabled() && emstune_get_cur_mode() == 0)
		return false;

	return egp->adap_fmax_enable && egp->adap_fmax_enable_by_user;
}

#define ESG_MAX_DELAY_PERIODS 5
/*
 * Return true if we can delay frequency update because the requested frequency
 * change is not large enough, and false if it is large enough. The condition
 * for determining large enough compares the number of frequency level change
 * vs., elapsed time since last frequency update. For example,
 * ESG_MAX_DELAY_PERIODS of 5 would mean immediate frequency change is allowed
 * only if the change in frequency level is greater or equal to 5;
 * It also means change in frequency level equal to 1 would need to
 * wait 5 ticks for it to take effect.
 */
static bool ego_postpone_freq_update(struct ego_policy *egp, u64 time, unsigned int *target_freq,
					unsigned int latency_level, s64 target_latency_ns)
{
	unsigned int diff_num_levels, num_periods, elapsed, margin;

	if (egp->need_freq_update || egp->quick_rampup || egp->busy)
		return false;

	elapsed = time - egp->last_freq_update_time;

	if (egp->policy->cur < *target_freq) {
		if (!step_latency_enable)
			return elapsed < egp->up_rate_limit_ns;

		if (elapsed < target_latency_ns) {
			int latency_freq = egp->step_latency_info.latency_freq[latency_level];

			/*
			 * if ramp-up has to be postponed with latency
			 * but cur_freq is lower than step latency freq,
			 * ramp-up wouldn't be postponed and target_freq is set to latency_freq
			 * because of ramp-up speed.
			 */
			if (latency_level && latency_freq > egp->policy->cur) {
				*target_freq = latency_freq;
				egp->reason = EGO_REASON_STEP_ADJUST;
				return false;
			}

			return true;
		}
		else
			return false;
	}

	margin  = egp->freq_update_delay_ns >> 2;
	num_periods = (elapsed + margin) / egp->freq_update_delay_ns;
	if (num_periods > ESG_MAX_DELAY_PERIODS)
		return false;

	diff_num_levels = get_diff_num_levels(egp->policy, *target_freq);
	if (diff_num_levels > ESG_MAX_DELAY_PERIODS - num_periods)
		return false;
	else
		return true;
}

/*********************************************************************/
/*		      Sysbusy state change notifier		     */
/*********************************************************************/
static int ego_sysbusy_notifier_call(struct notifier_block *nb,
		unsigned long val, void *v)
{
	enum sysbusy_state state = *(enum sysbusy_state *)v;
	int cpu;

	if (val != SYSBUSY_STATE_CHANGE)
		return NOTIFY_OK;

	for_each_possible_cpu(cpu) {
		struct ego_policy *egp;

		if (cpu != cpumask_first(cpu_clustergroup_mask(cpu)))
			continue;

		egp = per_cpu(ego_cpu, cpu).egp;
		if (!ego_is_working(egp))
			continue;

		egp->build_somac_wall = (state == SYSBUSY_SOMAC);
	}

	return NOTIFY_OK;
}

static struct notifier_block ego_sysbusy_notifier = {
	.notifier_call = ego_sysbusy_notifier_call,
};

/*********************************************************************/
/*		      EGO mode change notifier		     */
/*********************************************************************/
static bool ego_check_step_tune(int *cur_set_step, int step_count)
{
	int i;

	for (i = 0; i < step_count; i++)
		if (cur_set_step[i])
				return true;

	return false;
}

static inline void ego_mode_update_set_step(int *tune_step_val, int *tune_step_freq,
			unsigned int *table_step_val, unsigned int *table_step_freq,
			unsigned int *step_val, unsigned int *step_freq, bool working,
			bool val_verify, bool freq_verify, int step)
{
	int i;

	if (working) {
		for (i = 0; i < step; i++) {
			if (val_verify)
				step_val[i] = tune_step_val[i];
			else
				step_val[i] = table_step_val[i];

			if (freq_verify)
				step_freq[i] = tune_step_freq[i];
			else
				step_freq[i] = table_step_freq[i];
		}
	} else
		for (i = 0; i < step; i++)
			step_val[i] = step_freq[i] = 0;
}

void ego_resolve_step_val(unsigned int *val, unsigned int min, unsigned int max, int step)
{
	for (int i = 0; i < step; i++)
		val[i] = clamp_val(val[i], min, max);
}

static int ego_mode_update_callback(struct notifier_block *nb, unsigned long val, void *v)
{
	struct emstune_set *cur_set = (struct emstune_set *)v;
	struct emstune_cpufreq_gov freq_gov = cur_set->cpufreq_gov;
	struct ego_policy *egp;
	int cpu;

	step_latency_enable = cur_set->cpufreq_gov.step_latency_enable;
	step_margin_enable = cur_set->cpufreq_gov.step_margin_enable;

	for_each_possible_cpu(cpu) {
		if (cpu != cpumask_first(cpu_clustergroup_mask(cpu)))
			continue;

		egp = per_cpu(ego_cpu, cpu).egp;
		if (!egp)
			continue;

		egp->pelt_boost = freq_gov.pelt_boost[cpu];
		egp->ptb_margin = freq_gov.ptb_margin[cpu];
		egp->ptb_min_freq = freq_gov.ptb_min_freq[cpu];
		egp->adap_fmax_enable = freq_gov.adap_fmax_enable[cpu];
		egp->adap_fmax_freq = freq_gov.adap_fmax_freq[cpu];
		egp->default_adap_fmax_freq = freq_gov.adap_fmax_freq[cpu];

		/*
		 * Set values of each step parameters.
		 * If there are values that are defined from device tree,
		 * These values are used instead of calculating values from ego.
		 */
		ego_mode_update_set_step(freq_gov.step_latency_ns[cpu],
			freq_gov.step_latency_freq[cpu], egp->step_table.latency,
			egp->step_table.latency_freq, egp->step_latency_info.latency,
			egp->step_latency_info.latency_freq, step_latency_enable,
			ego_check_step_tune(freq_gov.step_latency_ns[cpu], egp->latency_step),
			ego_check_step_tune(freq_gov.step_latency_freq[cpu], egp->latency_step),
			egp->latency_step);

		ego_resolve_step_val(egp->step_latency_info.latency,
		       STEP_MIN_LATENCY, STEP_MAX_LATENCY, DEFAULT_LATENCY_STEP);
		if (egp->policy != NULL)
			ego_resolve_step_val(egp->step_latency_info.latency_freq,
				egp->policy->cpuinfo.min_freq, egp->policy->cpuinfo.max_freq,
					DEFAULT_LATENCY_STEP);

		ego_mode_update_set_step(freq_gov.step_pelt_margin[cpu],
			freq_gov.step_margin_freq[cpu], egp->step_table.pelt_margin,
			egp->step_table.margin_freq, egp->step_margin_info.pelt_margin,
			egp->step_margin_info.margin_freq, step_margin_enable,
			ego_check_step_tune(freq_gov.step_pelt_margin[cpu], egp->margin_step),
			ego_check_step_tune(freq_gov.step_margin_freq[cpu], egp->margin_step),
			egp->margin_step);

		ego_resolve_step_val(egp->step_margin_info.pelt_margin,
		       0, DEFAULT_PELT_MARGIN, DEFAULT_MARGIN_STEP);
		if (egp->policy != NULL)
			ego_resolve_step_val(egp->step_margin_info.margin_freq,
				egp->policy->cpuinfo.min_freq, egp->policy->cpuinfo.max_freq,
					DEFAULT_MARGIN_STEP);
	}

	return NOTIFY_OK;
}

static struct notifier_block ego_mode_update_notifier = {
	.notifier_call = ego_mode_update_callback,
};

/*********************************************************************/
/*		      	   EGO step notifier			     */
/*********************************************************************/
static int ego_step_callback(struct notifier_block *nb, unsigned long val, void *v)
{
	int cpu = *(int *)v;
	struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);
	struct ego_policy *egp = egc->egp;

	ego_calculate_step_info(egp);

	if (cpumask_test_cpu(cpu, cpu_clustergroup_mask(cpumask_last(cpu_possible_mask))))
		ego_adjust_step_info(egp);

	return NOTIFY_OK;
}

static struct notifier_block ego_step_notifier = {
	.notifier_call = ego_step_callback,
};


/*********************************************************************/
/* 			      SLACK TIMER			     */
/*********************************************************************/
static void ego_update_min_cap(struct cpufreq_policy *policy)
{
	unsigned int cpu;
	unsigned long max_cap, min_cap;

	max_cap = ems_get_actual_cpu_capacity(policy->cpu);

	/* min_cap is minimum value making higher frequency than policy->min */
	min_cap = (max_cap * policy->min) / policy->max;
	min_cap -= 1;

	for_each_cpu(cpu, policy->cpus)
		per_cpu(ego_cpu, cpu).min_cap = min_cap;
}

static int ego_need_slack_timer(void)
{
	unsigned int cpu = raw_smp_processor_id();
	struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);
	struct ego_policy *egp = egc->egp;
	int need = 0;

	if (!ego_is_working(egp))
		return 0;

	if (egc->boosted_util > egc->min_cap) {
		need = 1;
		goto out;
	}

	/* want to add timer heaviest cpu only in this domain */
	if (egp->heaviest_cpu == cpu) {
		/* want to add timer when freq is high with energy freq, not min lock */
		if (egp->policy->cur > egp->policy->cpuinfo.min_freq &&
				egp->eng_freq > egp->org_freq)
			need = 1;
	}

out:
	trace_ego_need_slack_timer(cpu, egc->boosted_util, egc->min_cap,
			egp->heaviest_cpu, egp->policy->cur,
			egp->policy->cpuinfo.min_freq,
			egp->eng_freq, egp->org_freq, need);

	return need;
}

static int ego_get_next_cap(struct tp_env *env, struct cpumask *cpus, int dst_cpu)
{
	unsigned int cpu = raw_smp_processor_id();
	struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);
	struct ego_policy *egp = egc->egp;
	unsigned long next_cap = 0;

	if (!ego_is_working(egp))
		return -1;

	for_each_cpu_and(cpu, cpus, cpu_active_mask) {
		unsigned long util;

		egc = &per_cpu(ego_cpu, cpu);
		egp = egc->egp;

		if (cpu == dst_cpu) /* util with task */
			util = env->cpu_stat[cpu].util_with;
		else /* util without task */
			util = env->cpu_stat[cpu].util_wo;

		/* apply pelt margin to calculate next cap */
		util = (util * (100 + egp->pelt_margin)) / 100;

		/*
		 * The cpu in the coregroup has same capacity and the
		 * capacity depends on the cpu with biggest utilization.
		 * Find biggest utilization in the coregroup and use it
		 * as max floor to know what capacity the cpu will have.
		 */
		if (util > next_cap)
			next_cap = util;
	}

	return next_cap;
}

/************************************************************************/
/* 			APIs for Adaptive Freq				*/
/************************************************************************/
static inline unsigned int get_adaptive_low_freq(struct ego_policy *egp)
{
	return max(egp->adaptive_low_freq, egp->adaptive_low_freq_kernel);
}

static inline unsigned int get_adaptive_high_freq(struct ego_policy *egp)
{
	return max(egp->adaptive_high_freq, egp->adaptive_high_freq_kernel);
}

int ego_set_adaptive_freq(unsigned int cpu, unsigned int low_freq, unsigned int high_freq)
{
	struct ego_policy *egp;

	if (!cpu_possible(cpu))
		return -EFAULT;

	egp = per_cpu(ego_cpu, cpu).egp;

	if (!egp->policy)
		return 0;

	egp->adaptive_low_freq_kernel = low_freq;
	egp->adaptive_high_freq_kernel = high_freq;

	return 0;

}
EXPORT_SYMBOL(ego_set_adaptive_freq);

int ego_get_adaptive_freq(unsigned int cpu, unsigned int *low_freq, unsigned int *high_freq)
{
	struct ego_policy *egp;

	if (!cpu_possible(cpu))
		return -EFAULT;

	egp = per_cpu(ego_cpu, cpu).egp;

	if (!virt_addr_valid(low_freq) || !virt_addr_valid(high_freq))
		return -EINVAL;

	*low_freq = get_adaptive_low_freq(egp);
	*high_freq = get_adaptive_high_freq(egp);

	return 0;
}
EXPORT_SYMBOL(ego_get_adaptive_freq);

static void ego_limits(struct cpufreq_policy *policy);
int ego_reset_adaptive_freq(unsigned int cpu, bool release)
{
	struct ego_policy *egp;
	bool idle = true;

	if (!cpu_possible(cpu))
		return -EFAULT;

	egp = per_cpu(ego_cpu, cpu).egp;
	if (unlikely(!egp))
		return -ENOMEM;

	egp->adaptive_low_freq_kernel = 0;
	egp->adaptive_high_freq_kernel = 0;

	if (release && ego_is_working(egp)) {
		for_each_cpu(cpu, &egp->cpus) {
			if (!cpu_active(cpu))
				continue;
			if (!available_idle_cpu(cpu)) {
				idle = false;
				break;
			}
		}
		if (idle)
			ego_limits(egp->policy);
	}

	return 0;
}
EXPORT_SYMBOL(ego_reset_adaptive_freq);

int ego_get_update_reason(struct cpufreq_policy *policy)
{
	struct ego_policy *egp;

	if (unlikely(!policy))
		return -1;

	if (unlikely(policy->governor != &energy_aware_gov))
		return -1;

	egp = policy->governor_data;
	if (unlikely(!egp))
		return -1;

	return egp->reason;
}
EXPORT_SYMBOL(ego_get_update_reason);

int ego_get_flag(struct cpufreq_policy *policy)
{
	struct ego_policy *egp;

	if (unlikely(!policy))
		return -1;

	if (unlikely(policy->governor != &energy_aware_gov))
		return -1;

	egp = policy->governor_data;
	if (unlikely(!egp))
		return -1;

	return egp->flags;
}
EXPORT_SYMBOL(ego_get_flag);

/************************ Governor internals ***********************/
static unsigned int ego_resolve_freq_wo_clamp(struct cpufreq_policy *policy,
					unsigned int freq)
{
	int index;

	index = cpufreq_table_find_index_al(policy, freq, false);
	if (index < 0) {
		pr_err("target frequency(%d) out of range\n", freq);
		return 0;
	}

	return policy->freq_table[index].frequency;
}

/*
 * DVFS request is sent once every 4ms, the request could be delayed.
 * To ensure the performance, support quick rampup when boosting task(uclamp_min task) is running.
 */
static bool ego_need_quick_rampup(struct ego_policy *egp, s64 delta_ns)
{
	s64 rate_limit_ns = LLONG_MAX;
	struct cpufreq_policy *policy = egp->policy;
	int cpu;

	for_each_cpu(cpu, policy->cpus) {
		struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);
		struct rq *rq = cpu_rq(cpu);

		if (egc->uclamp_min != uclamp_rq_get(rq, UCLAMP_MIN)
			|| egc->uclamp_max != uclamp_rq_get(rq, UCLAMP_MAX)) {
			rate_limit_ns = min(BOOST_RATE_LIMIT, rate_limit_ns);
			break;
		}
	}

	if (egp->flags & HEAVY_TASK)
		rate_limit_ns = min(egp->heavy_task_rate_limit, rate_limit_ns);
	if (egp->flags & UCLAMP_TASK)
		rate_limit_ns = min(egp->uclamp_task_rate_limit, rate_limit_ns);
	if (egp->flags & SUSTAIN_HIGH_TASK)
		rate_limit_ns = 0;

	return delta_ns >= rate_limit_ns;
}

static bool ego_should_update_freq(struct ego_policy *egp, u64 time)
{
	bool should_update = false;
	s64 delta_ns, rate_limit_ns;

	if (low_power_mode)
		egp->busy = false;
	else
		egp->busy = (sysbusy_activated() || gsc_boost_activated()) ? true : false;

	/*
	 * Since cpufreq_update_util() is called with rq->lock held for
	 * the @target_cpu, our per-CPU data is fully serialized.
	 *
	 * However, drivers cannot in general deal with cross-CPU
	 * requests, so while get_next_freq() will work, our
	 * ego_update_commit() call may not for the fast switching platforms.
	 *
	 * Hence stop here for remote requests if they aren't supported
	 * by the hardware, as calculating the frequency is pointless if
	 * we cannot in fact act on it.
	 *
	 * This is needed on the slow switching platforms too to prevent CPUs
	 * going offline from leaving stale IRQ work items behind.
	 */
	if (!cpufreq_this_cpu_can_update(egp->policy))
		return false;

	if (unlikely(egp->limits_changed)) {
		egp->limits_changed = false;
		egp->need_freq_update = true;
		return true;
	}

	delta_ns = time - egp->last_freq_update_time;

	/*
	 * EGO doesn't know target frequency at this point, so consider
	 * the minimum value between up/down rate limit to cover all cases.
	 * The exact rate limit will be considered in ego_postpone_freq_update().
	 */
	rate_limit_ns = min(egp->up_rate_limit_ns, egp->down_rate_limit_ns);

	if (delta_ns >= rate_limit_ns)
		should_update = true;
	else if (ego_need_quick_rampup(egp, delta_ns)) {
		/*
		 * If dvfs is requested too frequently, acpm can be hang.
		 * To avoid this problem, at least 500us were required to request quick rampup.
		 */
		should_update = true;
		egp->quick_rampup = true;
	}

	return should_update;
}

static bool ego_request_freq_change(struct ego_policy *egp, u64 time, unsigned int next_freq)
{
	struct cpufreq_policy *policy = egp->policy;
	int cpu;

	for_each_cpu(cpu, policy->cpus) {
		struct rq *rq = cpu_rq(cpu);
		struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);

		egc->uclamp_min = uclamp_rq_get(rq, UCLAMP_MIN);
		egc->uclamp_max = uclamp_rq_get(rq, UCLAMP_MAX);
	}

	egp->quick_rampup = false;

	if (!egp->need_freq_update) {
		if (egp->policy->cur == next_freq)
			return false;
	} else {
		egp->need_freq_update = false;
	}

	return true;
}

/* update next freq and last frequency change requesting time  */
static void ego_update_next_freq(struct ego_policy *egp, u64 time, unsigned int next_freq)
{
	egp->next_freq = next_freq;
	egp->last_freq_update_time = time;
}

static void ego_fast_switch(struct ego_policy *egp, u64 time, unsigned int next_freq)
{
	struct cpufreq_policy *policy = egp->policy;

	if (!ego_request_freq_change(egp, time, next_freq))
		return;

	ego_update_next_freq(egp, time, next_freq);
	cpufreq_driver_fast_switch(policy, next_freq);
}


static void ego_deferred_update(struct ego_policy *egp, u64 time, unsigned int next_freq)
{
	if (!ego_request_freq_change(egp, time, next_freq))
		return;

	ego_update_next_freq(egp, time, next_freq);

	if (!egp->work_in_progress) {
		egp->work_in_progress = true;
		irq_work_queue(&egp->irq_work);
	}
}

static inline unsigned long ego_resolve_freq_reversal(struct ego_policy *egp,
		unsigned long freq, int cur_step)
{
	int i;
	unsigned long resolved_freq = 0;

	for (i = cur_step + 1; i < egp->margin_step; i++) {
		if (freq < egp->step_margin_info.margin_freq[i]) {
			resolved_freq = egp->step_margin_info.margin_freq[i-1];
			goto out;
		}
	}

	resolved_freq = egp->step_margin_info.margin_freq[egp->margin_step - 1];

out:
	return resolved_freq;
}

static unsigned long ego_check_resolve_freq_reversal(struct ego_policy *egp,
		unsigned long util, unsigned long freq)
{
	int cur_step = ego_get_step_level(egp, egp->policy->cur, STEP_PELT_MARGIN);
	int next_step = ego_get_step_level(egp, freq, STEP_PELT_MARGIN);
	unsigned long resolved_freq = freq;

	if (cur_step < next_step)
		resolved_freq = ego_resolve_freq_reversal(egp, freq, cur_step);
	else if (!low_power_mode && egp->prev_util <= util && freq < egp->step_margin_info.margin_freq[cur_step])
		resolved_freq = egp->step_margin_info.margin_freq[cur_step];
	else
		goto out;

	trace_ego_resolve_freq_reversal(cpumask_first(&egp->cpus),
			freq, resolved_freq, cur_step, egp->pelt_margin);

out:
	return resolved_freq;
}

static unsigned int get_margin_util_target(struct ego_policy *egp)
{
	return max(egp->margin_util_target, egp->margin_util_target_by_user);
}

#define EXTRA_GAIN         	3U
static int apply_util_target_margin(struct ego_policy *egp, unsigned long util, int org_margin)
{
	unsigned long org_util;
	unsigned long util_gap;
	unsigned long numer;
	unsigned long denom;
	unsigned long extra_pct;
	unsigned long margin;
	unsigned long util_target = get_margin_util_target(egp);

	org_util = (100 + org_margin) * util / 100;

	if (org_util >= util_target)
		return -1;

	util_gap = util_target - org_util;

	numer = util_gap * util_gap;
	denom = util_target * util_target;

	extra_pct = (100UL * EXTRA_GAIN * numer) / denom;

	margin = org_margin + extra_pct;

	trace_ego_apply_util_target_margin(egp->policy->cpu, org_util, util_target, util_gap, extra_pct, org_margin, margin);

	return (int)margin;
}

int ego_set_margin_util_target(int cpu, unsigned int target_util)
{
	struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);
	struct ego_policy *egp = egc->egp;

	if (unlikely(!egp))
		return -EINVAL;

	egp->margin_util_target_by_user = target_util;

	return 0;
}
EXPORT_SYMBOL(ego_set_margin_util_target);

static bool support_util_target_margin(struct ego_policy *egp)
{
	if (!egg_enabled())
		return false;

	if (egp->margin_util_target != 0 && egg_is_busy_game())
		return true;

	if (egp->margin_util_target_by_user != 0)
		return true;

	return false;
}

static inline unsigned long ego_map_util_freq(struct ego_policy *egp, unsigned long util,
		unsigned long freq, unsigned long cap)
{
	int margin = egp->busy ? DEFAULT_PELT_MARGIN : egp->pelt_margin;
	int util_target_margin;
	unsigned long return_freq;

	if (support_util_target_margin(egp)) {
		util_target_margin = apply_util_target_margin(egp, util, margin);
		if (util_target_margin != -1)
			margin = util_target_margin;
	}

	egp->margin = margin;

	return_freq = ((freq * (100 + margin)) / 100) * util / cap;

	if (step_margin_enable && !egp->busy)
		return_freq = ego_check_resolve_freq_reversal(egp, util, return_freq);

	egp->prev_util = util;

	return return_freq;
}

/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @egp: schedutil policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * If the utilization is frequency-invariant, choose the new frequency to be
 * proportional to it, that is
 *
 * next_freq = C * max_freq * util / max
 *
 * Otherwise, approximate the would-be frequency-invariant utilization by
 * util_raw * (curr_freq / max_freq) which leads to
 *
 * next_freq = C * curr_freq * util_raw / max
 *
 * Take C = 1.25 for the frequency tipping point at (util / max) = 0.8.
 *
 * The lowest driver-supported frequency which is equal or greater than the raw
 * next_freq (as calculated above) is returned, subject to policy min/max and
 * cpufreq driver limitations.
 */

static unsigned int get_next_freq(struct ego_policy *egp, unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = egp->policy;
	unsigned int freq, org_freq, fclamp_freq;
	unsigned int low_freq, high_freq;

	/* compute pure frequency base on util */
	org_freq = ego_map_util_freq(egp, util, policy->cpuinfo.max_freq, max);
	if ((org_freq == egp->cached_raw_freq || egp->work_in_progress)
			&& !egp->need_freq_update) {
		freq = max(egp->org_freq, egp->next_freq);
		goto skip_find_next_freq;
	}
	egp->cached_raw_freq = org_freq;

	/* find freq from table */
	org_freq = ego_resolve_freq_wo_clamp(policy, org_freq);
	if (egp->org_freq != org_freq) {
		egp->org_freq = org_freq;
		/* inform new freq to et */
		et_update_freq(policy->cpu, org_freq);
	}

	freq = egp->eng_freq = org_freq;

skip_find_next_freq:
	/* Apply adaptive_high/low_freq */
	low_freq = get_adaptive_low_freq(egp);
	high_freq = get_adaptive_high_freq(egp);
	if (high_freq) {
		if (org_freq < low_freq) {
			freq = low_freq;
			egp->reason = EGO_REASON_ADAPTIVE_LOW;
		} else if (org_freq <= high_freq) {
			freq = high_freq;
			egp->reason = EGO_REASON_ADAPTIVE_HIGH;
		} else {
			/* To keep calculated freq */
			egp->reason = EGO_REASON_ADAPTIVE_NONE;
		}
		goto out;
	}

	/* Apply fclamp */
	if (egp->fclamp_enable) {
		fclamp_freq = fclamp_apply(policy, freq);
		if (fclamp_freq != freq) {
			freq = fclamp_freq;
			egp->reason = EGO_REASON_FCLAMP;
		}
	}

	/* Apply somac wall */
	if (egp->build_somac_wall && (freq > egp->somac_wall)) {
		freq = egp->somac_wall;
		egp->reason = EGO_REASON_SOMAC_WALL;
	}

out:
	freq = clamp_val(freq, policy->min, policy->max);

	/* adaptive fmax */
	if (adap_fmax_enabled(egp) && egp->adap_fmax_freq < freq &&
			adap_fmax_reason == ADAP_FMAX_NO_REASON) {
		freq = egp->adap_fmax_freq;
		egp->reason = EGO_REASON_ADAP_FMAX;
	}

	trace_ego_req_freq(policy->cpu, egp->heaviest_cpu, freq, policy->min, policy->max,
			org_freq, util, egp->margin, max, ego_cpufreq_reason_name[egp->reason]);

	return freq;
}

/*
 * This function computes an effective utilization for the given CPU, to be
 * used for frequency selection given the linear relation: f = u * f_max.
 *
 * The scheduler tracks the following metrics:
 *
 *   cpu_util_{cfs,rt,dl,irq}()
 *   cpu_bw_dl()
 *
 * Where the cfs,rt and dl util numbers are tracked with the same metric and
 * synchronized windows and are thus directly comparable.
 *
 * The cfs,rt,dl utilization are the running times measured with rq->clock_task
 * which excludes things like IRQ and steal-time. These latter are then accrued
 * in the IRQ utilization.
 *
 * The DL bandwidth number OTOH is not a measured metric but a value computed
 * based on the task model parameters and gives the minimal utilization
 * required to meet deadlines.
 */
unsigned long ego_cpu_util(int cpu, unsigned long util_cfs,
				unsigned long scale,
				unsigned long *min,
				unsigned long *max)
{
	unsigned long util, irq;
	struct rq *rq = cpu_rq(cpu);
	struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);

	/*
	 * Early check to see if IRQ/steal time saturates the CPU, can be
	 * because of inaccuracies in how we track these -- see
	 * update_irq_load_avg().
	 */
	irq = cpu_util_irq(rq);
	if (unlikely(irq >= scale)) {
		if (min)
			*min = scale;
		if (max)
			*max = scale;

		util = scale;
		goto out;
	}

	if (min) {
		/*
		 * The minimum utilization returns the highest level between:
		 * - the computed DL bandwidth needed with the IRQ pressure which
		 *   steals time to the deadline task.
		 * - The minimum performance requirement for CFS and/or RT.
		 */
		*min = max(irq + egc->bw_dl, uclamp_rq_get(rq, UCLAMP_MIN));
	}

	/*
	 * Because the time spend on RT/DL tasks is visible as 'lost' time to
	 * CFS tasks and we use the same metric to track the effective
	 * utilization (PELT windows are synchronized) we can directly add them
	 * to obtain the CPU's actual utilization.
	 */
	util = util_cfs + cpu_util_rt(rq);
	util += cpu_util_dl(rq);

	/*
	 * The maximum hint is a soft bandwidth requirement, which can be lower
	 * than the actual utilization because of uclamp_max requirements.
	 */
	if (max)
		*max = min(scale, uclamp_rq_get(rq, UCLAMP_MAX));

	if (util >= scale) {
		util = scale;
		goto out;
	}

	/*
	 * There is still idle time; further improve the number by using the
	 * IRQ metric. Because IRQ/steal time is hidden from the task clock we
	 * need to scale the task numbers:
	 *
	 *              max - irq
	 *   U' = irq + --------- * U
	 *                 max
	 */
	util = scale_irq_capacity(util, irq, scale);
	util += irq;

out:
	trace_ego_sched_util(cpu, util, util_cfs, cpu_util_rt(rq),
			cpu_util_dl(rq), egc->bw_dl, cpu_util_irq(rq),
			uclamp_rq_get(rq, UCLAMP_MIN), uclamp_rq_get(rq, UCLAMP_MAX));

	return min(scale, util);
}

static unsigned long ego_get_util(struct ego_cpu *egc)
{
	unsigned long util = ml_cpu_util(egc->cpu);
	unsigned long scale = ems_get_origin_cpu_capacity(egc->cpu);
	unsigned long min_util, max_util;

	egc->max = scale;
	util = ego_cpu_util(egc->cpu, util, scale, &min_util, &max_util);

	if (util < max_util)
		max_util = util;

	return max(min_util, max_util);
}

/**
 * ego_iowait_reset() - Reset the IO boost status of a CPU.
 * @egc: the ego data for the CPU to boost
 * @time: the update time from the caller
 * @set_iowait_boost: true if an IO boost has been requested
 *
 * The IO wait boost of a task is disabled after a tick since the last update
 * of a CPU. If a new IO wait boost is requested after more then a tick, then
 * we enable the boost starting from IOWAIT_BOOST_MIN, which improves energy
 * efficiency by ignoring sporadic wakeups from IO.
 */
static bool ego_iowait_reset(struct ego_cpu *egc, u64 time, bool set_iowait_boost)
{
	s64 delta_ns = time - egc->last_update;

	/* Reset boost only if a tick has elapsed since last request */
	if (delta_ns <= TICK_NSEC)
		return false;

	egc->iowait_boost = set_iowait_boost ? IOWAIT_BOOST_MIN : 0;
	egc->iowait_boost_pending = set_iowait_boost;

	return true;
}

/**
 * ego_iowait_boost() - Updates the IO boost status of a CPU.
 * @egc: the ego data for the CPU to boost
 * @time: the update time from the caller
 * @flags: SCHED_CPUFREQ_IOWAIT if the task is waking up after an IO wait
 *
 * Each time a task wakes up after an IO operation, the CPU utilization can be
 * boosted to a certain utilization which doubles at each "frequent and
 * successive" wakeup from IO, ranging from IOWAIT_BOOST_MIN to the utilization
 * of the maximum OPP.
 *
 * To keep doubling, an IO boost has to be requested at least once per tick,
 * otherwise we restart from the utilization of the minimum OPP.
 */
static void ego_iowait_boost(struct ego_cpu *egc, u64 time, unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;

	/* Reset boost if the CPU appears to have been idle enough */
	if (egc->iowait_boost &&
			ego_iowait_reset(egc, time, set_iowait_boost))
		return;

	/* Boost only tasks waking up after IO */
	if (!set_iowait_boost)
		return;

	/* Ensure boost doubles only one time at each request */
	if (egc->iowait_boost_pending)
		return;
	egc->iowait_boost_pending = true;

	/* Double the boost at each request */
	if (egc->iowait_boost) {
		egc->iowait_boost =
			min_t(unsigned int, egc->iowait_boost << 1, SCHED_CAPACITY_SCALE);
		return;
	}

	/* First wakeup after IO: start with minimum boost */
	egc->iowait_boost = IOWAIT_BOOST_MIN;
}

/**
 * ego_iowait_apply() - Apply the IO boost to a CPU.
 * @egc: the ego data for the cpu to boost
 * @time: the update time from the caller
 * @util: the utilization to (eventually) boost
 * @max: the maximum value the utilization can be boosted to
 *
 * A CPU running a task which woken up after an IO operation can have its
 * utilization boosted to speed up the completion of those IO operations.
 * The IO boost value is increased each time a task wakes up from IO, in
 * ego_iowait_apply(), and it's instead decreased by this function,
 * each time an increase has not been requested (!iowait_boost_pending).
 *
 * A CPU which also appears to have been idle for at least one tick has also
 * its IO boost utilization reset.
 *
 * This mechanism is designed to boost high frequently IO waiting tasks, while
 * being more conservative on tasks which does sporadic IO operations.
 */
static unsigned long ego_iowait_apply(struct ego_cpu *egc, u64 time,
		unsigned long util, unsigned long max)
{
	unsigned long boost;

	/* No boost currently required */
	if (!egc->iowait_boost)
		return 0;

	/* Reset boost if the CPU appears to have been idle enough */
	if (ego_iowait_reset(egc, time, false))
		return 0;

	if (!egc->iowait_boost_pending) {
		/*
		 * No boost pending; reduce the boost value.
		 */
		egc->iowait_boost >>= 1;
		if (egc->iowait_boost < IOWAIT_BOOST_MIN) {
			egc->iowait_boost = 0;
			return 0;
		}
	}

	egc->iowait_boost_pending = false;

	/*
	 * @util is already in capacity scale; convert iowait_boost
	 * into the same scale so we can compare.
	 */
	boost = (egc->iowait_boost * max) >> SCHED_CAPACITY_SHIFT;
	return boost;
}

/*
 * Make ego_should_update_freq() ignore the rate limit when DL
 * has increased the utilization.
 */
static inline void ignore_dl_rate_limit(struct ego_cpu *egc, struct ego_policy *egp)
{
	if (cpu_bw_dl(cpu_rq(egc->cpu)) > egc->bw_dl)
		egp->limits_changed = true;
}

static int get_boost_pelt_util(int capacity, int util, int boost)
{
	long long margin;

	margin = util * boost / 100;
	return util + margin;
}

static void update_max_util(struct ego_policy *egp, struct ego_cpu *egc, unsigned long util,
			unsigned long *max, int reason)
{
	if (egp->pelt_boost && (reason == EGO_REASON_PELT_BOOST))
		goto update;

	if (util <= *max)
		return;

update:
	*max = util;
	egc->boost_reason = reason;
}

static unsigned long freq_to_cap(struct ego_policy *egp, unsigned int freq)
{
	unsigned long max_cap = ems_get_origin_cpu_capacity(egp->policy->cpu);
	unsigned long cap;

	cap = mult_frac(max_cap, freq, egp->policy->cpuinfo.max_freq);
	cap = mult_frac(cap, 80, 100);

	return cap;
}

static unsigned long __get_heavy_task_util(struct ego_policy *egp, struct rq *rq)
{
	unsigned long load, margin;

	if (egp->ptb_margin < 0)
		return 0;

	load = ems_rq_htask_load(rq);
	margin = mult_frac(load, egp->ptb_margin, 100);

	return load + margin;
}

static unsigned long get_heavy_task_util(struct ego_policy *egp, int cpu)
{
	unsigned long boosted_util, min_load, load;
	struct rq *rq = cpu_rq(cpu);

	if (!ems_rq_htask(rq))
		return 0;

	min_load = freq_to_cap(egp, egp->ptb_min_freq);
	load = __get_heavy_task_util(egp, rq);

	boosted_util = max(min_load, load);

	trace_ego_get_heavy_task_util(cpu, boosted_util, min_load, load);

	return boosted_util;
}

static unsigned long ego_apply_boost(struct ego_policy *egp, struct ego_cpu *egc,
				unsigned long util)
{
	unsigned long max_boosted_util = util, boosted_util;
	int cpu = egc->cpu;
	unsigned long capacity = ems_get_actual_cpu_capacity(cpu);

	boosted_util = freqboost_cpu_boost(cpu, util);
	update_max_util(egp, egc, boosted_util, &max_boosted_util, EGO_REASON_FREQ_BOOST);

	boosted_util = get_heavy_task_util(egp, cpu);
	update_max_util(egp, egc, boosted_util, &max_boosted_util, EGO_REASON_HEAVY_TASK);

	boosted_util = get_boost_pelt_util(capacity, max_boosted_util, egp->pelt_boost);
	update_max_util(egp, egc, boosted_util, &max_boosted_util, EGO_REASON_PELT_BOOST);

	if (need_sustain_high_boost() && ems_rq_nr_sustain_high(cpu_rq(cpu))) {
		boosted_util = 1 << SCHED_CAPACITY_SHIFT;
		update_max_util(egp, egc, boosted_util, &max_boosted_util, EGO_REASON_SUSTAIN_HIGH);
	}

	return max_boosted_util;
}

static unsigned int ego_next_freq_shared(struct ego_policy *egp, u64 time)
{
	struct cpufreq_policy *policy = egp->policy;
	unsigned long util = 0, max = 1;
	unsigned int cpu;

	egp->reason = EGO_REASON_NONE;

	for_each_cpu(cpu, policy->cpus) {
		struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);
		unsigned long cpu_util, cpu_boosted_util, cpu_io_util;
		unsigned long cpu_max = egc->max;

		cpu_util = ego_get_util(egc);
		if ((cpu_util > util) && !egp->pelt_boost) {
			util = cpu_util;
			egp->heaviest_cpu = cpu;
			egp->reason = EGO_REASON_UTIL;
		}

		cpu_boosted_util = ego_apply_boost(egp, egc, cpu_util);
		if (cpu_boosted_util > util) {
			util = cpu_boosted_util;
			egp->heaviest_cpu = cpu;
			egp->reason = egc->boost_reason;
		}

		cpu_io_util = ego_iowait_apply(egc, time, cpu_util, cpu_max);
		if (cpu_io_util > util) {
			util = cpu_io_util;
			egp->heaviest_cpu = cpu;
			egp->reason = EGO_REASON_IO_UTIL;
		}

		egc->util = cpu_util;
		egc->io_util = cpu_io_util;
		egc->boosted_util = cpu_boosted_util;

		/* find heaviest max */
		max = max(max, cpu_max);

		trace_ego_cpu_util(cpu, egp->pelt_boost, cpu_util, cpu_io_util,
				cpu_boosted_util);
	}

	return get_next_freq(egp, util, max);
}

static bool internal_freq_limit_changed(struct ego_policy *egp)
{
	struct cpufreq_policy *policy = egp->policy;
	unsigned int old = egp->internal_freq_limit;

	egp->internal_freq_limit = policy->cpuinfo.max_freq;

	if (adap_fmax_enabled(egp) && adap_fmax_reason == ADAP_FMAX_NO_REASON)
		egp->internal_freq_limit = min(egp->internal_freq_limit, egp->adap_fmax_freq);

	return old != egp->internal_freq_limit;
}

static void ego_update_shared(struct ego_cpu *egc, u64 time, unsigned int flags)
{
	struct ego_policy *egp = egc->egp;
	unsigned int next_f = 0, orig_f = 0, latency_level = 0, margin_level = 0;
	s64 target_latency_ns = 0;

	if (internal_freq_limit_changed(egp))
		ems_update_cpufreq_pressure(egp->policy, INTERNAL_LIMIT_PRESSURE);

	ego_iowait_boost(egc, time, flags);
	egc->last_update = time;

	if (egc->iowait_boost || egp->limits_changed)
		raw_spin_lock(&egp->update_lock);
	else
		if (!raw_spin_trylock(&egp->update_lock))
			return;

	egp->flags = flags;
	egp->margin = 0;

	if (ego_should_update_freq(egp, time)) {
		bool postponed = false;

		next_f = orig_f = ego_next_freq_shared(egp, time);

		if (step_latency_enable) {
			latency_level = ego_get_step_level(egp, next_f, STEP_LATENCY);
			target_latency_ns = egp->step_latency_info.latency[latency_level] * NSEC_PER_MSEC;
		}

		postponed = ego_postpone_freq_update(egp, time, &next_f,
				latency_level, target_latency_ns);

		if (postponed)
			goto out;

		if (step_margin_enable && !egp->busy) {
			margin_level = ego_get_step_level(egp, next_f, STEP_PELT_MARGIN);
			egp->pelt_margin = egp->step_margin_info.pelt_margin[margin_level];
		} else
			egp->pelt_margin = DEFAULT_PELT_MARGIN;

		if (egp->policy->fast_switch_enabled)
			ego_fast_switch(egp, time, next_f);
		else
			ego_deferred_update(egp, time, next_f);
out:
		trace_ego_freq_update_postponed(cpumask_first(&egp->cpus), next_f, orig_f,
			target_latency_ns, egp->pelt_margin, postponed);
	}

	raw_spin_unlock(&egp->update_lock);
}

static void ego_update_util_hook(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct ego_cpu *egc = container_of(hook, struct ego_cpu, update_util);

	ego_update_shared(egc, time, flags);
}

void ego_update_freq(struct rq *rq, u64 time, unsigned int flags)
{
	struct ego_cpu *egc = &per_cpu(ego_cpu, cpu_of(rq));

	if (!ego_is_working(egc->egp))
		return;

	ego_update_shared(egc, time, flags);
}

static void ego_work(struct kthread_work *work)
{
	struct ego_policy *egp = container_of(work, struct ego_policy, work);
	unsigned int freq;
	unsigned long flags;

	/*
	 * Hold egp->update_lock shortly to handle the case where:
	 * incase egp->next_freq is read here, and then updated by
	 * ego_deferred_update() just before work_in_progress is set to false
	 * here, we may miss queueing the new update.
	 *
	 * Note: If a work was queued after the update_lock is released,
	 * ego_work() will just be called again by kthread_work code; and the
	 * request will be proceed before the ego thread sleeps.
	 */
	raw_spin_lock_irqsave(&egp->update_lock, flags);
	freq = egp->next_freq;
	egp->work_in_progress = false;
	raw_spin_unlock_irqrestore(&egp->update_lock, flags);

	mutex_lock(&egp->work_lock);
	__cpufreq_driver_target(egp->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&egp->work_lock);
}

static void ego_irq_work(struct irq_work *irq_work)
{
	struct ego_policy *egp;

	egp = container_of(irq_work, struct ego_policy, irq_work);

	kthread_queue_work(&egp->worker, &egp->work);
}

unsigned int ego_get_internal_freq_limit(struct cpufreq_policy *policy)
{
	struct ego_policy *egp = policy->governor_data;
	if (!egp)
		return policy->max;

	return egp->internal_freq_limit;
}

void adap_fmax_update(void)
{
	int cpu = 0;
	struct ego_policy *egp;
	bool adap_fmax_disable = true;
	int reason = ADAP_FMAX_NO_REASON;
	int prev_reason = adap_fmax_reason;

	for_each_possible_cpu(cpu) {
		if (cpu != cpumask_first(cpu_clustergroup_mask(cpu)))
			continue;

		egp = per_cpu(ego_cpu, cpu).egp;
		if (!egp)
			continue;

		if (adap_fmax_enabled(egp)) {
			adap_fmax_disable = false;
			break;
		}
	}

	/* all cluster disabled -> don't need to check */
	if (adap_fmax_disable) {
		goto out;
	}

	if (sysbusy_activated()) {
		reason = ADAP_FMAX_SYSBUSY;
	} else if (profile_get_sustain_high()) {
		reason = ADAP_FMAX_SUSTAIN_HIGH;
	} else if (mlt_get_constant_load())
		reason = ADAP_FMAX_CONSTANT_LOAD;

out:
	if (prev_reason != reason) {
		adap_fmax_reason = reason;
		trace_adap_fmax_reason(reason);
	}
}

/************************** sysfs interface ************************/
struct ego_attr {
	struct attribute attr;
	ssize_t (*show)(struct kobject *, char *);
	ssize_t (*store)(struct kobject *, const char *, size_t count);
};

#define ego_attr_rw(name)			\
static struct ego_attr name##_attr =		\
__ATTR(name, 0644, show_##name, store_##name)

#define ego_attr_ro(name)			\
static struct ego_attr name##_attr =		\
__ATTR(name, 0444, show_##name, NULL)

#define ego_show(name)								\
static ssize_t show_##name(struct kobject *k, char *buf)			\
{										\
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);	\
										\
	return sprintf(buf, "%d\n", egp->name);					\
}										\

#define ego_store(name)								\
static ssize_t store_##name(struct kobject *k, const char *buf, size_t count)	\
{										\
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);	\
	int data;								\
										\
	if (!sscanf(buf, "%d", &data))						\
		return -EINVAL;							\
										\
	egp->name = data;							\
	return count;								\
}

#define ego_latency_show(name)							\
static ssize_t show_##name(struct kobject *k, char *buf)			\
{										\
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);	\
	ssize_t total = 0;							\
	int i;									\
	for (i = 0; i < egp->latency_step; i++) {				\
		int ret = sprintf(buf, "%u ", egp->step_latency_info.name[i]);	\
		if(ret < 0) return ret;						\
		buf += ret;							\
		total += ret;							\
	}									\
	total += sprintf(buf, "\n");						\
										\
	return total;								\
}										\

#define ego_margin_show(name)							\
static ssize_t show_##name(struct kobject *k, char *buf)			\
{										\
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);	\
	ssize_t total = 0;							\
	int i;									\
	for (i = 0; i < egp->margin_step; i++) {				\
		int ret = sprintf(buf, "%u ", egp->step_margin_info.name[i]);	\
		if(ret < 0) return ret;						\
		buf += ret;							\
		total += ret;							\
	}									\
	total += sprintf(buf, "\n");						\
										\
	return total;								\
}										\

static ssize_t store_enable_adaptive_fmax(struct kobject *k, const char *buf, size_t count)
{
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);
	unsigned int enable;

	if (!sscanf(buf, "%u", &enable))
		return -EINVAL;

	egp->adap_fmax_enable_by_user = enable;
	return count;
}

static ssize_t show_enable_adaptive_fmax(struct kobject *k, char *buf)
{
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);
	return sprintf(buf, "%u\n", egp->adap_fmax_enable_by_user);
}

static ssize_t store_enable_fclamp(struct kobject *k, const char *buf, size_t count)
{
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);
	unsigned int enable;

	if (!sscanf(buf, "%u", &enable))
		return -EINVAL;

	egp->fclamp_enable = enable;
	return count;
}

static ssize_t show_enable_fclamp(struct kobject *k, char *buf)
{
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);
	return sprintf(buf, "%u\n", egp->fclamp_enable);
}

static ssize_t store_margin_util_target(struct kobject *k, const char *buf, size_t count)
{
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);
	unsigned int target;

	if (!sscanf(buf, "%u", &target))
		return -EINVAL;

	egp->margin_util_target = target;
	return count;
}

static ssize_t show_margin_util_target(struct kobject *k, char *buf)
{
	struct ego_policy *egp = container_of(k, struct ego_policy, kobj);
	return sprintf(buf, "%u\n", egp->margin_util_target);
}

ego_show(ratio);
ego_store(ratio);
ego_attr_rw(ratio);
ego_show(dis_buck_share);
ego_store(dis_buck_share);
ego_attr_rw(dis_buck_share);

ego_show(somac_wall);
ego_store(somac_wall);
ego_attr_rw(somac_wall);

ego_margin_show(pelt_margin);
ego_attr_ro(pelt_margin);
ego_margin_show(margin_freq);
ego_attr_ro(margin_freq);

ego_latency_show(latency);
ego_attr_ro(latency);
ego_latency_show(latency_freq);
ego_attr_ro(latency_freq);

ego_attr_rw(enable_adaptive_fmax);
ego_attr_rw(enable_fclamp);
ego_attr_rw(margin_util_target);

static ssize_t show(struct kobject *kobj, struct attribute *at, char *buf)
{
	struct ego_attr *fvattr = container_of(at, struct ego_attr, attr);

	return fvattr->show(kobj, buf);
}

static ssize_t store(struct kobject *kobj, struct attribute *at,
		const char *buf, size_t count)
{
	struct ego_attr *fvattr = container_of(at, struct ego_attr, attr);

	return fvattr->store(kobj, buf, count);
}

static const struct sysfs_ops ego_sysfs_ops = {
	.show	= show,
	.store	= store,
};

static struct attribute *ego_attrs[] = {
	&ratio_attr.attr,
	&somac_wall_attr.attr,
	&dis_buck_share_attr.attr,
	&latency_attr.attr,
	&latency_freq_attr.attr,
	&pelt_margin_attr.attr,
	&margin_freq_attr.attr,
	&enable_adaptive_fmax_attr.attr,
	&enable_fclamp_attr.attr,
	&margin_util_target_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(ego);

static struct kobj_type ktype_ego = {
	.sysfs_ops	= &ego_sysfs_ops,
	.default_groups	= ego_groups,
};

#define adaptive_store(name)						\
static ssize_t store_##name(struct cpufreq_policy *policy,		\
		const char *buf, size_t count)				\
{									\
	struct ego_policy *egp = per_cpu(ego_cpu, policy->cpu).egp;	\
	unsigned int freq;						\
	if (sscanf(buf, "%u", &freq) != 1)				\
		return -EINVAL;						\
	egp->name = freq;						\
	return count;							\
}

#define adaptive_show(name)							\
static ssize_t show_##name(struct cpufreq_policy *policy,		\
		char *buf)						\
{									\
	struct ego_policy *egp = per_cpu(ego_cpu, policy->cpu).egp;	\
	return sprintf(buf, "%u\n", egp->name);				\
}

adaptive_store(adaptive_high_freq);
adaptive_show(adaptive_high_freq);
cpufreq_freq_attr_rw(adaptive_high_freq);

adaptive_show(adaptive_high_freq_kernel);
cpufreq_freq_attr_ro(adaptive_high_freq_kernel);

adaptive_store(adaptive_low_freq);
adaptive_show(adaptive_low_freq);
cpufreq_freq_attr_rw(adaptive_low_freq);

adaptive_show(adaptive_low_freq_kernel);
cpufreq_freq_attr_ro(adaptive_low_freq_kernel);

static int ego_adaptive_init(struct cpufreq_policy *policy)
{
	int ret = 0;

	ret |= sysfs_create_file(&policy->kobj, &adaptive_high_freq.attr);
	ret |= sysfs_create_file(&policy->kobj, &adaptive_high_freq_kernel.attr);
	ret |= sysfs_create_file(&policy->kobj, &adaptive_low_freq.attr);
	ret |= sysfs_create_file(&policy->kobj, &adaptive_low_freq_kernel.attr);

	return ret;
}

static void ego_alloc_step_info(struct ego_policy *egp)
{
	/* alloc step latency info */
	egp->step_latency_info.latency_freq =
		kcalloc(DEFAULT_LATENCY_STEP, sizeof(unsigned int), GFP_KERNEL);
	egp->step_latency_info.latency =
		kcalloc(DEFAULT_LATENCY_STEP, sizeof(unsigned int), GFP_KERNEL);

	/* alloc step margin info */
	egp->step_margin_info.margin_freq =
		kcalloc(DEFAULT_MARGIN_STEP, sizeof(unsigned int), GFP_KERNEL);
	egp->step_margin_info.pelt_margin =
		kcalloc(DEFAULT_PELT_MARGIN, sizeof(unsigned int), GFP_KERNEL);


	/* alloc step table info */
	egp->step_table.latency_freq =
		kcalloc(DEFAULT_LATENCY_STEP, sizeof(unsigned int), GFP_KERNEL);

	egp->step_table.latency =
		kcalloc(DEFAULT_LATENCY_STEP, sizeof(unsigned int), GFP_KERNEL);
	for (int i = 0; i < DEFAULT_LATENCY_STEP; i++)
		egp->step_table.latency[i] = DEFAULT_LATENCY;

	egp->step_table.raw_latency =
		kcalloc(DEFAULT_LATENCY_STEP, sizeof(unsigned long), GFP_KERNEL);

	egp->step_table.margin_freq =
		kcalloc(DEFAULT_MARGIN_STEP, sizeof(unsigned int), GFP_KERNEL);

	egp->step_table.pelt_margin =
		kcalloc(DEFAULT_MARGIN_STEP, sizeof(unsigned int), GFP_KERNEL);
	for (int i = 0; i < DEFAULT_MARGIN_STEP; i++)
		egp->step_table.pelt_margin[i] = DEFAULT_PELT_MARGIN;

	egp->step_table.raw_pelt_margin =
		kcalloc(DEFAULT_MARGIN_STEP, sizeof(unsigned long), GFP_KERNEL);

	egp->latency_step = DEFAULT_LATENCY_STEP;
	egp->margin_step = DEFAULT_MARGIN_STEP;

	return;
}

static void ego_free_step_info(struct ego_policy *egp)
{
	/* free step latency info */
	if (egp->step_latency_info.latency_freq)
		kfree(egp->step_latency_info.latency_freq);
	if (egp->step_latency_info.latency)
		kfree(egp->step_latency_info.latency);

	/* free step margin info */
	if (egp->step_margin_info.margin_freq)
		kfree(egp->step_margin_info.margin_freq);
	if (egp->step_margin_info.pelt_margin)
		kfree(egp->step_margin_info.pelt_margin);

	/* free step table info */
	if (egp->step_table.latency_freq)
		kfree(egp->step_table.latency_freq);
	if (egp->step_table.latency)
		kfree(egp->step_table.latency);
	if (egp->step_table.raw_latency)
		kfree(egp->step_table.raw_latency);
	if (egp->step_table.margin_freq)
		kfree(egp->step_table.margin_freq);
	if (egp->step_table.pelt_margin)
		kfree(egp->step_table.pelt_margin);
	if (egp->step_table.raw_pelt_margin)
		kfree(egp->step_table.raw_pelt_margin);

	return;
}

/********************** cpufreq governor interface *********************/
static int ego_kthread_create(struct ego_policy *egp)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO / 2 };
	struct cpufreq_policy *policy = egp->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&egp->work, ego_work);
	kthread_init_worker(&egp->worker);
	thread = kthread_create(kthread_worker_fn, &egp->worker,
			"ego:%d", cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create ego thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	set_cpus_allowed_ptr(thread, &egp->thread_allowed_cpus);
	thread->flags |= PF_NO_SETAFFINITY;
	egp->thread = thread;
	init_irq_work(&egp->irq_work, ego_irq_work);
	mutex_init(&egp->work_lock);

	pr_info("%s: cpus=%#x, allowed-cpu=%#x\n", __func__,
			*(unsigned int *)cpumask_bits(&egp->cpus),
			*(unsigned int *)cpumask_bits(&egp->thread_allowed_cpus));

	return 0;
}

static int ego_init(struct cpufreq_policy *policy)
{
	struct ego_policy *egp = NULL;
	int cpu;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	egp = per_cpu(ego_cpu, policy->cpu).egp;
	if (!egp) {
		pr_info("%s: ego_policy is not ready\n", __func__);
		goto fail_ego_init;
	}

	if (egp->policy) {
		egp->policy = policy;
		pr_info("%s: Already ego_policy was initialized\n", __func__);
		goto complete_ego_init;
	}
	egp->policy = policy;

	if (ego_adaptive_init(policy)) {
		pr_info("%s: failed to create adaptive freq sysfs\n", __func__);
		goto fail_ego_init;
	}

	if (ego_kthread_create(egp)) {
		pr_info("%s: failed to create kthread\n", __func__);
		goto fail_ego_init;
	}

complete_ego_init:
	if (!policy->fast_switch_enabled)
		wake_up_process(egp->thread);

	policy->governor_data = egp;

	for_each_cpu(cpu, policy->related_cpus)
		cpufreq_register_hook(cpu, ego_get_next_cap, ego_need_slack_timer);

	pr_info("%s: ego init complete: cpus=%#x, allowed-cpu=%#x\n", __func__,
			*(unsigned int *)cpumask_bits(&egp->cpus),
			*(unsigned int *)cpumask_bits(&egp->thread_allowed_cpus));
	return 0;

fail_ego_init:
	cpufreq_disable_fast_switch(policy);
	pr_err("initialization failed\n");
	return -1;
}

static void ego_exit(struct cpufreq_policy *policy)
{
	int cpu;

	policy->governor_data = NULL;
	cpufreq_disable_fast_switch(policy);

	for_each_cpu(cpu, policy->related_cpus)
		cpufreq_unregister_hook(cpu);
}

static int ego_start(struct cpufreq_policy *policy)
{
	struct ego_policy *egp = policy->governor_data;
	unsigned int cpu;

	egp->pelt_margin		= DEFAULT_PELT_MARGIN;
	egp->freq_update_delay_ns	= 4 * NSEC_PER_MSEC;
	egp->up_rate_limit_ns		= 4 * NSEC_PER_MSEC;
	egp->down_rate_limit_ns		= 4 * NSEC_PER_MSEC;
	egp->last_freq_update_time	= 0;
	egp->next_freq			= 0;
	egp->work_in_progress		= false;
	egp->limits_changed		= false;
	egp->need_freq_update		= false;
	egp->cached_raw_freq		= 0;
	egp->quick_rampup		= false;
	egp->adap_fmax_enable_by_user	= true;
	egp->fclamp_enable		= true;
	egp->prev_util			= 0;

	for_each_cpu(cpu, policy->cpus) {
		struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);

		egc->iowait_boost_pending = false;
		egc->iowait_boost = 0;
		egc->last_update = 0;
		egc->bw_dl = 0;
		egc->max = 0;
		egc->util = 0;
		egc->boosted_util = 0;
		egc->egp = egp;
		egc->cpu = cpu;
		egc->min_cap = ULONG_MAX;
		egc->uclamp_min = 0;
		egc->uclamp_max = SCHED_CAPACITY_SCALE;
	}

	for_each_cpu(cpu, policy->cpus) {
		struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &egc->update_util, ego_update_util_hook);
	}
	return 0;
}

static void ego_stop(struct cpufreq_policy *policy)
{
	struct ego_policy *egp = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&egp->irq_work);
		kthread_cancel_work_sync(&egp->work);
	}
}

static void __ego_limits(struct cpufreq_policy *policy, unsigned int target_freq)
{
	struct ego_policy *egp = policy->governor_data;

	ego_update_min_cap(policy);
	ego_update_next_freq(egp, egp->last_freq_update_time, target_freq);
	egp->reason = EGO_REASON_LIMITS;
}

static void ego_limits(struct cpufreq_policy *policy)
{
	struct ego_policy *egp = policy->governor_data;
	unsigned int target_freq;
	unsigned long flags;

	target_freq = max(egp->org_freq, egp->eng_freq);
	target_freq = clamp_val(target_freq, policy->min, policy->max);

	if (!policy->fast_switch_enabled) {
		mutex_lock(&egp->work_lock);

		raw_spin_lock_irqsave(&egp->update_lock, flags);
		__ego_limits(policy, target_freq);
		raw_spin_unlock_irqrestore(&egp->update_lock, flags);

		__cpufreq_driver_target(policy, target_freq, CPUFREQ_RELATION_H);
		mutex_unlock(&egp->work_lock);
	} else {
		raw_spin_lock_irqsave(&egp->update_lock, flags);

		__ego_limits(policy, target_freq);
		cpufreq_driver_fast_switch(policy, target_freq);

		raw_spin_unlock_irqrestore(&egp->update_lock, flags);
	}
}

struct cpufreq_governor energy_aware_gov = {
	.name			= "energy_aware",
	.owner			= THIS_MODULE,
	.flags			= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init			= ego_init,
	.exit			= ego_exit,
	.start			= ego_start,
	.stop			= ego_stop,
	.limits			= ego_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ENERGYAWARE
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &energy_aware_gov;
}
#endif

static int ego_register(struct kobject *ems_kobj)
{
	ego_kobj = kobject_create_and_add("ego", ems_kobj);
	if (!ego_kobj)
		return -EINVAL;

	sysbusy_register_notifier(&ego_sysbusy_notifier);
	emstune_register_notifier(&ego_mode_update_notifier);

	if (cpumask_equal(cpu_online_mask, cpu_possible_mask))
		et_register_notifier(&ego_step_notifier);

	return cpufreq_register_governor(&energy_aware_gov);
}

static struct ego_policy *ego_policy_alloc(void)
{
	return kzalloc(sizeof(struct ego_policy), GFP_KERNEL);
}

static int ego_parse_dt(struct device_node *dn, struct ego_policy *egp)
{
	struct cpumask phy_cpus, phy_pol_cpus, mask;
	struct ems_cpu_topology *topo;
	const char *buf;

	if (of_property_read_string(dn, "cpus", &buf)) {
		pr_err("%s: physical cpus property is omitted\n", __func__);
		return PARSE_INVALID;
	}

	if (cpumask_equal(cpu_possible_mask, get_possible_phy_cpus())) {
		ems_cpulist_parse(buf, &egp->cpus);
	} else {
		ems_cpulist_parse(buf, &phy_cpus);
		cpumask_and(&phy_pol_cpus, &phy_cpus, get_possible_phy_cpus());

		if (cpumask_empty(&phy_pol_cpus)) {
			pr_info("%s: %d: empty phy_pol_cpus\n",__func__, cpumask_first(&phy_pol_cpus));
			return PARSE_EMPTY;
		}

		topo = percpu_topology(get_cpu_log_id(cpumask_first(&phy_pol_cpus)));
		cpumask_copy(&egp->cpus, &topo->cluster_cpus);
	}

	if (!of_property_read_string(dn, "thread-run-on", &buf))
		cpulist_parse(buf, &mask);
	else
		cpumask_copy(&mask, cpu_possible_mask);
	cpumask_copy(&egp->thread_allowed_cpus, &mask);

	if (of_property_read_u32(dn, "ratio", &egp->ratio))
		egp->ratio = RATIO_UNIT;

	if (of_property_read_u32(dn, "margin-util-target", &egp->margin_util_target))
		egp->margin_util_target = 0;

	if (of_property_read_u32(dn, "dis-buck-share", &egp->dis_buck_share))
		egp->dis_buck_share = 0;

	if (of_property_read_u32(dn, "somac_wall", &egp->somac_wall))
		egp->somac_wall = UINT_MAX;

	if (of_property_read_s32(dn, "heavy-task-rate-limit", &egp->heavy_task_rate_limit))
		egp->heavy_task_rate_limit = BOOST_RATE_LIMIT;

	if (of_property_read_s32(dn, "uclamp-task-rate-limit", &egp->uclamp_task_rate_limit))
		egp->uclamp_task_rate_limit = BOOST_RATE_LIMIT;

	ego_alloc_step_info(egp);

	return PARSE_DONE;
}

int ego_pre_init(struct kobject *ems_kobj)
{
	struct device_node *dn, *child;
	int cpu, ret;

	dn = of_find_node_by_path("/ems/ego");
	if (!dn)
		goto fail;

	ego_register(ems_kobj);

	if (of_property_read_bool(dn, "low-power-mode")) {
		pr_info("ego: low_power_mode enabled");
		low_power_mode = true;
	}

	for_each_child_of_node(dn, child) {
		struct ego_policy *egp;

		egp = ego_policy_alloc();
		if (!egp) {
			pr_err("%s: failed to alloc ego_policy\n", __func__);
			goto fail;
		}

		/* Parse device tree */
		ret = ego_parse_dt(child, egp);

		if (ret == PARSE_INVALID)
			goto fail;
		else if (ret == PARSE_EMPTY)
			continue;

		/* Init Sysfs */
		if (kobject_init_and_add(&egp->kobj, &ktype_ego, ego_kobj,
					"coregroup%d", cpumask_first(&egp->cpus)))
			goto fail;

		/* init policy spin lock */
		raw_spin_lock_init(&egp->update_lock);

		for_each_cpu(cpu, &egp->cpus) {
			struct ego_cpu *egc = &per_cpu(ego_cpu, cpu);
			egc->egp = egp;
		}
	}

	return 0;

fail:
	for_each_possible_cpu(cpu) {
		if (per_cpu(ego_cpu, cpu).egp) {
			ego_free_step_info(per_cpu(ego_cpu, cpu).egp);
			kfree(per_cpu(ego_cpu, cpu).egp);
		}
		per_cpu(ego_cpu, cpu).egp = NULL;
	}

	return -1;
}

void ego_update_pelt_boost(int cpu, int value) {
	struct ego_policy *egp = per_cpu(ego_cpu, cpu).egp;

	if (egp)
		egp->pelt_boost = value;
}
