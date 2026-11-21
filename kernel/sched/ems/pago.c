/*
 * PAGO(PMU based Advanced memory GOvernor) memory latency governor.
 * Copyright (C) 2023, Samsung Electronic Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>

#include <soc/samsung/ems.h>
#include <soc/samsung/exynos_pm_qos.h>
#include <linux/exynos-dsufreq.h>
#include <soc/samsung/cal-if.h>

#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

#define DEFAULT_UPDATE_MS (8)
/* Minimum bandwidth(MB/s): do not request memfreq below this threshold */
#define DEFAULT_BW_THRESHOLD 76
#define BW_IDLE_DURATION 4
#define CME_BOOST_PRIO_LVL 9

enum {
	CYCLE,
	INSTR,
#if !IS_ENABLED(CONFIG_SOC_S5E8535) && !IS_ENABLED(CONFIG_SOC_S5E8365)
	L2_REFILL,
	L3_REFILL,
#endif
	STALL_BACKEND_MEM,
#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
	CME_CYCLE,
	CME_L3_REFILL,
#endif
	MAX_PMU_EVENTS,
};

enum {
	PARSE_DONE = 0,
	PARSE_EMPTY,
};

#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
#define PMUV3_CME_CYCLES 0x3246
#define PMUV3_CME_L3D_CACHE_REFILL 0x3290
static int pmu_event_id[MAX_PMU_EVENTS] = {
	ARMV8_PMUV3_PERFCTR_CPU_CYCLES,
	ARMV8_PMUV3_PERFCTR_INST_RETIRED,
	ARMV8_PMUV3_PERFCTR_L2D_CACHE_REFILL,
	ARMV8_PMUV3_PERFCTR_L3D_CACHE_REFILL,
	ARMV8_AMU_PERFCTR_STALL_BACKEND_MEM,
	PMUV3_CME_CYCLES,
	PMUV3_CME_L3D_CACHE_REFILL,
};
#else
static int pmu_event_id[MAX_PMU_EVENTS] = {
	ARMV8_PMUV3_PERFCTR_CPU_CYCLES,
	ARMV8_PMUV3_PERFCTR_INST_RETIRED,
#if !IS_ENABLED(CONFIG_SOC_S5E8535) && !IS_ENABLED(CONFIG_SOC_S5E8365)
	ARMV8_PMUV3_PERFCTR_L2D_CACHE_REFILL,
	ARMV8_PMUV3_PERFCTR_L3D_CACHE_REFILL,
#endif
	ARMV8_AMU_PERFCTR_STALL_BACKEND_MEM,
};
#endif

struct boost_request {
	bool			activated;
	int			level;

	ktime_t			activated_ts;
};

struct cpu_mem_map {
	unsigned int cpufreq;
	unsigned int memfreq;
};

struct pago_cpu {
	struct pago_domain 	**domains;
	int			nr_domains;

	int			cpu;
	bool			cpu_up;
	bool			enabled;
	bool			idle_sample;
	unsigned int		cycle_freq;
	unsigned int		*target_freq;
	unsigned int 		cme_prio_lvl;
	unsigned int 		cme_prio_lvl_backup;

	ktime_t			last_sampled;
	ktime_t			last_updated;
	raw_spinlock_t		lock;

	/* pmu event data */
	struct perf_event	*pe[MAX_PMU_EVENTS];
	u64			curr[MAX_PMU_EVENTS];
	u64			prev[MAX_PMU_EVENTS];
	s64			delta[MAX_PMU_EVENTS];
};

struct pago_domain {
	struct pago_group	*grp;
	struct cpumask		cpus;
	struct kobject		kobj;

	struct cpu_mem_map	*freq_table;
	unsigned int		table_size;
	unsigned int		ratio;
	unsigned int		prio_lvl;

	bool			perf;
};

struct pago_group {
	struct kobject			kobj;
	const char			*name;

	unsigned int			*miss_idx;
	int				num_miss_idx;

	struct pago_domain		**domains;
	int				nr_domains;

	struct exynos_pm_qos_request	pm_qos;
	unsigned int			pm_qos_class;

	unsigned int			boost_min;
	unsigned int			boost_threshold;
	unsigned int			force_boost_ms;
	unsigned int			bw_threshold;
	unsigned int			ramp_up_enable;
	unsigned int			shift_level;

	unsigned int			max_mem_freq;
	unsigned int			max_boost_freq;
	unsigned int			max_target_freq;
	unsigned int			max_perf_cycle_freq;

	unsigned int			resolved_freq;

	unsigned int			*ftable;
	int				ftable_size;
	unsigned int			*invalid_ftable;
	int				invalid_size;

	unsigned long			total_miss;
	unsigned long			bw_idle_count;

	bool				is_cme;
};

struct pago {
	struct kobject			*kobj;
	struct cpumask			thread_allowed_cpus;
	struct task_struct		*thread;
	struct kthread_worker		worker;
	struct kthread_work		work;
	struct irq_work			irq_work;

	struct pago_group		**groups;
	int				nr_groups;

	struct boost_request		emstune;

	ktime_t				update_period;
	ktime_t				last_updated;
	ktime_t				last_jiffies;

	raw_spinlock_t			lock;

	bool				enable;
	bool				sysbusy_boost;

	unsigned int			cme_freq;
	unsigned long long		cme_cnt;

	unsigned int			stall_threshold;

	atomic_t			suspended;

	unsigned int			cal_id;
};


static struct pago *pago;
static DEFINE_PER_CPU(struct pago_cpu, pago_cpu);
static DEFINE_PER_CPU(unsigned int, cpu_old_freq);

static int cpuhp_state;
static DEFINE_MUTEX(pago_ctrl_lock);


static u64 read_pmu_events(struct pago_cpu *data, int idx)
{
	u64 ret = 0;

	if (data->pe[idx])
		perf_event_read_local(data->pe[idx], &ret, NULL, NULL);

	return ret;
}

static void deactivate_pmu_events(struct pago_cpu *data)
{
	int i;

	for (i = 0; i < MAX_PMU_EVENTS; i++) {
		if (data->pe[i]) {
			exynos_perf_event_release_kernel(data->pe[i]);
			data->pe[i] = NULL;
		}
	}
}

static int activate_pmu_events(struct pago_cpu *data)
{
	struct perf_event *pe;
	struct perf_event_attr *pe_attr;
	int i;

	pe_attr = kzalloc(sizeof(struct perf_event_attr), GFP_KERNEL);
	if (!pe_attr) {
		pr_err("failed to allocate perf event attr\n");
		return -ENOMEM;
	}

	pe_attr->size = (u32)sizeof(struct perf_event_attr);
	pe_attr->type = PERF_TYPE_RAW;
	pe_attr->pinned = 1;

	/* enable 64-bit counter */
	pe_attr->config1 = 1;

	for (i = 0; i < MAX_PMU_EVENTS; i++) {
		pe_attr->config = pmu_event_id[i];
#if IS_ENABLED(CONFIG_SOC_S5E9945) || IS_ENABLED(CONFIG_SOC_S5E9945_EVT0)
		/* Root hunter cpu errata: L3D_CACHE_REFILL is incorrect
		 * use LL_CACHE_RD
		 */
		if (i == L3_REFILL && data->cpu >= 4 && data->cpu <= 8) {
			pe_attr->config = ARMV8_PMUV3_PERFCTR_LL_CACHE_RD;
		}
#endif
		pe = exynos_perf_create_kernel_counter(pe_attr, data->cpu, NULL, NULL, NULL);
		if (!pe) {
			pr_err("failed to create kernel perf event. CPU=%d\n", data->cpu);
			kfree(pe_attr);
			return -ENOMEM;
		}

		data->pe[i] = pe;
	}

	kfree(pe_attr);

	return 0;
}

static unsigned int cpufreq_to_memfreq(unsigned int scale_freq, struct pago_domain *dom)
{
	struct cpu_mem_map *table = dom->freq_table;
	struct pago_group *grp = dom->grp;
	unsigned int mem_freq = 0;
	int idx;

	if (!table)
		goto out;

	for (idx = 0; idx < dom->table_size; idx++) {
		if (scale_freq < table[idx].cpufreq)
                        break;
		else if (dom->grp->pm_qos_class && scale_freq == table[idx].cpufreq)
                        break;

                mem_freq = table[idx].memfreq;
	}

	if (mem_freq > 0 && grp->shift_level > 0) {
		idx--;
		idx += grp->shift_level;
		if (idx >= dom->table_size)
			idx = dom->table_size - 1;

		mem_freq = table[idx].memfreq;
	}

out:
	return mem_freq;
}

#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
static unsigned int get_cme_freq(struct pago_domain *dom, unsigned int scale_freq)
{
	int i;
	struct pago_group *grp = dom->grp;
	unsigned int freq;

	if (grp->ftable == NULL)
		return 0;

	freq = grp->ftable[grp->ftable_size - 1];

	scale_freq = scale_freq * dom->ratio / 100;

	for (i = 0; i < grp->ftable_size; i++) {
		if (grp->ftable[i] >= scale_freq) {
			freq = grp->ftable[i];
			break;
		}
	}

	return freq;
}
#endif
static unsigned int get_max_freq_from_cur(struct pago_group *grp)
{
	struct pago_domain *dom;
	unsigned int cur_freq, mem_freq, max_mem_freq = 0;
	int i, cpu;

	if (grp->pm_qos_class)
		return grp->max_mem_freq;

	for (i = 0; i < grp->nr_domains; i++) {
		dom = grp->domains[i];

		cpu = cpumask_any_and(&dom->cpus, cpu_online_mask);
		if (cpu >= VENDOR_NR_CPUS)
			continue;

		cur_freq = cpufreq_quick_get(cpu);
		mem_freq = cpufreq_to_memfreq(cur_freq, dom);
		if (mem_freq > max_mem_freq)
			max_mem_freq = mem_freq;
	}

	return max_mem_freq;
}

int get_freq_idx(struct pago_group *grp, unsigned int mem_freq)
{
	int i = 0;

	if (grp->ftable == NULL)
		return -1;

	for (i = 0; i < grp->ftable_size; i++) {
		if (grp->ftable[i] == mem_freq)
			return i;
	}

	return -1;
}

static void pago_calc_target_freq(ktime_t now)
{
	struct boost_request *emstune_boost = &pago->emstune;
	struct pago_cpu *data;
	int i, cpu;
	unsigned long flags;
	s64 delta, update_delta;
	unsigned int max_stall_ratio = 0;

	update_delta = ktime_us_delta(now, pago->last_updated);

	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];
		grp->total_miss = 0;
	}

	/* update pmu counter */
	for_each_possible_cpu(cpu) {
		unsigned int cur_freq;
		unsigned int *old_freq;
		unsigned int stall_ratio;

		data = &per_cpu(pago_cpu, cpu);
		old_freq = &per_cpu(cpu_old_freq, cpu);

		raw_spin_lock_irqsave(&data->lock, flags);
		if (!data->cpu_up || !data->enabled) {
			raw_spin_unlock_irqrestore(&data->lock, flags);
			continue;
		}

		/* calculate counter delta */
		for (i = 0; i < MAX_PMU_EVENTS; i++) {
			data->delta[i] = data->curr[i] - data->prev[i];
			if (data->delta[i] < 0)
				data->delta[i] = 0;
			data->prev[i] = data->curr[i];
		}

		if (data->idle_sample) {
			delta = update_delta;
			data->last_updated = now;
		}
		else {
			delta = ktime_us_delta(data->last_sampled, data->last_updated);
			if (delta < 0)
				delta = update_delta;

			data->last_updated = data->last_sampled;
		}

		data->cycle_freq = (unsigned int)DIV_ROUND_CLOSEST_ULL(data->delta[CYCLE], delta) * 1000;

		trace_pago_delta_snapshot(cpu, data->idle_sample, delta, data->delta);

		cur_freq = cpufreq_quick_get(cpu);

		/* update stall ratio */
		if (data->delta[CYCLE] > 0) {
			stall_ratio = (unsigned int)DIV_ROUND_CLOSEST_ULL(data->delta[STALL_BACKEND_MEM] * 100,
					data->delta[CYCLE]);
			trace_pago_mem_stall_ratio(cpu, stall_ratio);

			if (stall_ratio > max_stall_ratio)
				max_stall_ratio = stall_ratio;
		}

		/* update domain & group */
		for (i = 0; i < data->nr_domains; i++) {
			struct pago_domain *dom = data->domains[i];
			struct pago_group *grp = dom->grp;
			unsigned int i, scale_freq = data->cycle_freq;
			unsigned long miss = 0;

			for (i = 0; i < grp->num_miss_idx; i++) {
				miss = data->delta[grp->miss_idx[i]];
				grp->total_miss += miss;
			}

			if (!emstune_boost->activated && cur_freq > *old_freq) {
				/* cpufreq lamp up */
				if (grp->ramp_up_enable)
					scale_freq = cur_freq;
			}

			if (dom->perf)
				grp->max_perf_cycle_freq = max(grp->max_perf_cycle_freq, data->cycle_freq);

#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
			if (grp->is_cme) {
				if (dom->ratio && data->delta[CME_CYCLE] > 0) {
					if (data->target_freq[i] == 0)
						data->target_freq[i] = get_cme_freq(dom, cur_freq);
					else
						data->target_freq[i] = get_cme_freq(dom, scale_freq);
				} else {
					data->target_freq[i] = 0;
				}
			} else {
				data->target_freq[i] = cpufreq_to_memfreq(scale_freq, dom);
			}
#else
			data->target_freq[i] = cpufreq_to_memfreq(scale_freq, dom);
#endif
			grp->max_target_freq = max(grp->max_target_freq, data->target_freq[i]);

			trace_pago_target_snapshot(cpu, data->target_freq[i], data->cycle_freq,
					scale_freq, cur_freq, grp->shift_level, grp->name);
		}

		raw_spin_unlock_irqrestore(&data->lock, flags);
		*old_freq = cur_freq;
	}

	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];
		unsigned long threshold;

		/* save cme freq to request cme freq as dsu freq */
		if (grp->is_cme)
			pago->cme_freq = grp->max_target_freq;

		if (grp->num_miss_idx == 0)
			continue;

		/* bandwidth(MB/s) = miss * cache-line(64b) * (1000 / period:8ms) / 1024 / 1024
		 * miss = bw << 20 / (1000 / period:8ms) >> 6 = bw << 14 / (1000 / period:8ms)
		 */
		threshold = (grp->bw_threshold << 14) / (1000 / pago->update_period);

		if (grp->total_miss < threshold && max_stall_ratio < pago->stall_threshold) {
			grp->bw_idle_count++;

			/* prevent overflow */
			if (grp->bw_idle_count > INT_MAX)
				grp->bw_idle_count = BW_IDLE_DURATION;
		} else {
			grp->bw_idle_count = 0;
		}

		if (grp->bw_idle_count >= BW_IDLE_DURATION) {
			grp->max_target_freq = 0;
		}

		trace_pago_calc_target_freq(grp->bw_threshold, grp->total_miss, max_stall_ratio,
				threshold, grp->bw_idle_count, grp->max_target_freq, grp->name);
	}

	pago->last_updated = now;
}

static void pago_calc_boost_freq(ktime_t now)
{
	struct boost_request *emstune_boost = &pago->emstune;
	int i;

	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];
		unsigned int max_boost_freq = 0;

		if (grp->is_cme)
			continue;

		/* check emstune boost */
		if (emstune_boost->activated) {
			s64 boosted_time = now - emstune_boost->activated_ts;

			unsigned int max_from_cur = get_max_freq_from_cur(grp);

			if (emstune_sched_applaunch())
				max_boost_freq = max_from_cur;
			else if (boosted_time < ms_to_ktime(grp->force_boost_ms))
				max_boost_freq = max_from_cur;
			else if (grp->boost_min) {
				if (grp->max_target_freq > grp->boost_min)
					max_boost_freq = max_from_cur;
				else
					max_boost_freq = grp->boost_min;
			}
		}

		grp->max_boost_freq = max_boost_freq;
	}
}

static void pago_request_group_target(struct pago_group *grp, unsigned int target_freq)
{
	if (grp->pm_qos_class)
		exynos_pm_qos_update_request(&grp->pm_qos, target_freq);

#ifdef CONFIG_SCHED_EMS_DSU_FREQ_SELECT
	if (!grp->pm_qos_class) {
		if (grp->is_cme) {
			trace_clock_set_rate("CME-DSU", target_freq * 1000, raw_smp_processor_id());
			if (target_freq > 0) pago->cme_cnt++;
		} else {
			target_freq = max(target_freq, pago->cme_freq);
			exynos_dsufreq_target(target_freq);
		}
	}
#endif
}

static void pago_boost(void)
{
	int i;
	unsigned int max_from_cur;

	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];
		if (grp->is_cme)
			continue;

		max_from_cur = get_max_freq_from_cur(grp);
		/* request new boost freq */
		pago_request_group_target(grp, max_from_cur);

		trace_pago_boost(max_from_cur, grp->name);
	}
}

static void pago_work(struct kthread_work *work)
{
	int i;
	ktime_t now = ktime_get();

	/* calc target_freq using PMU events */
	pago_calc_target_freq(now);

	/* calc boost_freq */
	pago_calc_boost_freq(now);

	/* request new mem freq */
	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];

		grp->resolved_freq = max(grp->max_target_freq, grp->max_boost_freq);

		pago_request_group_target(grp, grp->resolved_freq);

		trace_pago_work(grp->max_target_freq, grp->max_boost_freq, grp->resolved_freq, grp->name);

		/* clear calculated freq */
		grp->max_boost_freq = 0;
		grp->max_target_freq = 0;
		grp->max_perf_cycle_freq = 0;
	}
	pago->cme_freq = 0;
}

static void pago_irq_work(struct irq_work *irq_work)
{
	if (atomic_read(&pago->suspended))
		return;

	kthread_queue_work(&pago->worker, &pago->work);
}

#define UPDATE_MARGIN          ((NSEC_PER_SEC / HZ) >> 1)
#define UPDATE_DELAY           (100 * NSEC_PER_USEC)
#define TARGET_PERIOD          (4)
#define ACTIVE_THRESHOLD       (20)
static bool pago_need_update(ktime_t now)
{
	ktime_t update_period = pago->update_period;
	int cpu, i, active_ratio;

	if (atomic_read(&pago->suspended))
		return false;

	if (now - pago->last_jiffies + UPDATE_MARGIN < ms_to_ktime(update_period))
		return false;

	for_each_possible_cpu(cpu) {
		active_ratio = mlt_cpu_value(cpu, TARGET_PERIOD, AVERAGE, ACTIVE)
				* 100 / SCHED_CAPACITY_SCALE;

		if (active_ratio > ACTIVE_THRESHOLD)
			return true;
	}

	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];

		/* if pm_qos requested, force update for release */
		if (grp->pm_qos_class && grp->pm_qos.node.prio > 0)
			return true;
		else if (grp->resolved_freq > 0)
			return true;
	}

	return false;
}

void pago_update(void)
{
	ktime_t now = ktime_get();

	if (unlikely(!pago))
		return;

	if (unlikely(!pago->enable))
		return;

	if (!pago_need_update(now))
		return;

	irq_work_queue(&pago->irq_work);

	pago->last_jiffies = now;
}

void pago_tick(struct rq *rq)
{
	struct pago_cpu *data = &per_cpu(pago_cpu, rq->cpu);
	int i;
	unsigned long flags;
	ktime_t now, update_period;

	if (unlikely(!pago))
		return;

	now = ktime_get();
	update_period = pago->update_period;

	raw_spin_lock_irqsave(&data->lock, flags);

	if (!data->enabled || !data->cpu_up)
		goto out;

	if (now - data->last_updated + UPDATE_MARGIN < ms_to_ktime(update_period))
		goto out;

	/* Read pmu counter here */
	for (i = 0; i < MAX_PMU_EVENTS; i++)
		data->curr[i] = read_pmu_events(data, i);

	data->last_sampled = now;
	data->idle_sample = false;

	trace_pago_data_snapshot(rq->cpu, data->idle_sample, data->last_sampled,
			data->last_updated, data->curr, "tick");

out:
	raw_spin_unlock_irqrestore(&data->lock, flags);
}

void pago_idle_enter(int cpu)
{
	struct pago_cpu *data = &per_cpu(pago_cpu, cpu);
	int i;
	unsigned long flags;

	raw_spin_lock_irqsave(&data->lock, flags);

	if (!data->enabled || !data->cpu_up)
		goto out;

	for (i = 0; i < MAX_PMU_EVENTS; i++)
		data->curr[i] = read_pmu_events(data, i);

	data->idle_sample = true;

	trace_pago_data_snapshot(cpu, data->idle_sample, data->last_sampled,
			data->last_updated, data->curr, "idle-enter");

out:
	raw_spin_unlock_irqrestore(&data->lock, flags);
}

void pago_idle_exit(int cpu)
{
	struct pago_cpu *data = &per_cpu(pago_cpu, cpu);

	if (system_supports_sme())
		write_sysreg_s(data->cme_prio_lvl, SYS_SMPRI_EL1);
}

static int pago_cpu_up(unsigned int cpu)
{
	struct pago_cpu *data = &per_cpu(pago_cpu, cpu);
	unsigned long flags;

	if (activate_pmu_events(data) < 0) {
		pr_err("%s: failed to enable pmu events. CPU=%d\n", __func__, cpu);
		return 0;
	}

	raw_spin_lock_irqsave(&data->lock, flags);
	data->cpu_up = true;
	raw_spin_unlock_irqrestore(&data->lock, flags);

	if (system_supports_sme())
		write_sysreg_s(data->cme_prio_lvl, SYS_SMPRI_EL1);

	trace_pago_data_snapshot(cpu, data->cpu_up, data->last_sampled,
			data->last_updated, data->curr, "cpu-up");
	return 0;
}

static int pago_cpu_down(unsigned int cpu)
{
	struct pago_cpu *data = &per_cpu(pago_cpu, cpu);
	unsigned long flags;

	raw_spin_lock_irqsave(&data->lock, flags);
	data->cpu_up = false;
	raw_spin_unlock_irqrestore(&data->lock, flags);

	deactivate_pmu_events(data);

	trace_pago_data_snapshot(cpu, data->cpu_up, data->last_sampled,
			data->last_updated, data->curr, "cpu-down");

	return 0;
}

void pago_ramp_up_ctrl(int ramp_up_enable)
{
	int i;

	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];

		if (!grp->is_cme) {
			grp->ramp_up_enable = ramp_up_enable;
		}
	}
}

void pago_change_shift_level_dsu(unsigned int level)
{
	int i;

	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];

		if (!grp->is_cme && !grp->pm_qos_class) {
			grp->shift_level = level;
			trace_pago_shift_level(grp->shift_level, grp->name);
			break;
		}
	}
}

void pago_change_shift_level_mif(unsigned int level)
{
	int i;

	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];

		if (grp->pm_qos_class) {
			grp->shift_level = level;
			trace_pago_shift_level(grp->shift_level, grp->name);
			break;
		}
	}
}

void reset_update_period(void)
{
	pago->update_period = DEFAULT_UPDATE_MS;
}

void check_pago_update_period(void)
{
	if (egg_enabled())
		pago->update_period = get_target_frame_period() / NSEC_PER_MSEC;
	else
		pago->update_period = DEFAULT_UPDATE_MS;
}

int pago_emstune_boost(bool boost)
{
	struct boost_request *emstune;

	if (unlikely(!pago))
		return 0;

	if (unlikely(!pago->enable))
		return 0;

	emstune = &pago->emstune;

	raw_spin_lock(&pago->lock);

	if (!emstune->activated && !boost) {
		raw_spin_unlock(&pago->lock);
		return 0;
	}

	emstune->activated = boost;
	emstune->activated_ts = boost ? ktime_get() : 0;
	raw_spin_unlock(&pago->lock);

	if (boost)
		pago_boost();

	trace_pago_emstune_boost(boost, emstune->activated_ts);

	return 0;
}
EXPORT_SYMBOL_GPL(pago_emstune_boost);

static void pago_disable(void)
{
	struct pago_cpu *data;
	int cpu;
	unsigned long flags;

	mutex_lock(&pago_ctrl_lock);

	if (!pago->enable) {
		mutex_unlock(&pago_ctrl_lock);
		return;
	}

	if (cpuhp_state > 0)
		cpuhp_remove_state_nocalls(cpuhp_state);

	for_each_possible_cpu(cpu) {
		data = &per_cpu(pago_cpu, cpu);

		raw_spin_lock_irqsave(&data->lock, flags);
		data->enabled = false;
		data->cpu_up = false;
		data->idle_sample = true;
		raw_spin_unlock_irqrestore(&data->lock, flags);

		deactivate_pmu_events(data);
	}

	pago->enable = false;

	mutex_unlock(&pago_ctrl_lock);
}

static void pago_enable(void)
{
	struct pago_cpu *data;
	int cpu;
	unsigned long flags;

	mutex_lock(&pago_ctrl_lock);

	if (pago->enable) {
		mutex_unlock(&pago_ctrl_lock);
		return;
	}

	cpus_read_lock();

	for_each_possible_cpu(cpu) {
		data = &per_cpu(pago_cpu, cpu);

		if (activate_pmu_events(data) < 0) {
			pr_err("%s: failed to enable pmu events. CPU=%d\n", __func__, cpu);
			continue;
		}

		raw_spin_lock_irqsave(&data->lock, flags);
		data->enabled = true;
		data->cpu_up = cpumask_test_cpu(cpu, cpu_online_mask);
		data->idle_sample = false;
		raw_spin_unlock_irqrestore(&data->lock, flags);
	}

	cpuhp_state = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_AP_ONLINE_DYN, "pago", pago_cpu_up, pago_cpu_down);

	pago->enable = true;

	cpus_read_unlock();

	mutex_unlock(&pago_ctrl_lock);
}

static int pago_cme_prio_update(void *__unused)
{
	struct pago_cpu *data = &per_cpu(pago_cpu, smp_processor_id());

	write_sysreg_s(data->cme_prio_lvl, SYS_SMPRI_EL1);

	return 0;
}

static int pago_sysbusy_notifier_call(struct notifier_block *nb,
                       unsigned long val, void *v)
{
	enum sysbusy_state state = *(enum sysbusy_state *)v;
	struct pago_cpu *data;
	bool boost;
	int cpu;

	if (unlikely(!pago->enable))
		return NOTIFY_OK;

	if (val != SYSBUSY_STATE_CHANGE)
		return NOTIFY_OK;

	boost = state > SYSBUSY_STATE1;
	if (pago->sysbusy_boost == boost)
		return NOTIFY_OK;

	pago->sysbusy_boost = boost;

	for_each_cpu(cpu, cpu_fastest_mask()) {
		data = &per_cpu(pago_cpu, cpu);
		if (boost) {
			data->cme_prio_lvl_backup = data->cme_prio_lvl;
			data->cme_prio_lvl = CME_BOOST_PRIO_LVL;
			trace_pago_cme_prio(cpu, data->cme_prio_lvl, data->cme_prio_lvl_backup, "boost");
		} else {
			data->cme_prio_lvl = data->cme_prio_lvl_backup;
			trace_pago_cme_prio(cpu, data->cme_prio_lvl, data->cme_prio_lvl_backup, "release");
		}
		stop_one_cpu(cpu, pago_cme_prio_update, NULL);
	}

	return NOTIFY_OK;
}

static struct notifier_block pago_sysbusy_notifier = {
       .notifier_call = pago_sysbusy_notifier_call,
};

void pago_suspend(void)
{
	int i;

	if (!pago->enable)
		return;

	atomic_set(&pago->suspended, true);

	irq_work_sync(&pago->irq_work);
	kthread_cancel_work_sync(&pago->work);

	for (i = 0; i < pago->nr_groups; i++) {
		struct pago_group *grp = pago->groups[i];

		if (grp->pm_qos_class && grp->pm_qos.node.prio > 0) {
			exynos_pm_qos_update_request(&grp->pm_qos, 0);
		}
	}

	return;
}

void pago_resume(void)
{
	if (!pago->enable)
		return;

	atomic_set(&pago->suspended, false);

	return;
}

/**********************************************************************
 *			    SYSFS interface			      *
 **********************************************************************/

static ssize_t show_cme_prio(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct pago_cpu *data;
	int cpu, count = 0;

	for_each_online_cpu(cpu) {
		data = &per_cpu(pago_cpu, cpu);

		count += snprintf(buf + count, PAGE_SIZE - count,
				"cpu %d : %d\n", cpu, data->cme_prio_lvl);
	}

	return count;
}

static ssize_t store_cme_prio(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct pago_cpu *data;
	int prio, cpu;

	if (!sscanf(buf, "%d %d", &cpu, &prio))
		return -EINVAL;

	if (cpu < 0 || cpu >= NR_CPUS || prio < 0)
		return -EINVAL;

	data = &per_cpu(pago_cpu, cpu);

	if (prio > 15)
		prio = 15;

	data->cme_prio_lvl = prio;

	stop_one_cpu(cpu, pago_cme_prio_update, NULL);

	return count;
}

static struct kobj_attribute cme_prio_attr =
__ATTR(cme_prio, 0644, show_cme_prio, store_cme_prio);

static ssize_t show_cme_cnt(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 30, "%llu\n", pago->cme_cnt);
}

static struct kobj_attribute cme_cnt_attr =
__ATTR(cme_cnt, 0444, show_cme_cnt, NULL);

static ssize_t show_update_period(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%llu\n", pago->update_period);
}

static ssize_t store_update_period(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	u64 input;

	if (sscanf(buf, "%llu", &input) != 1)
		return -EINVAL;

	pago->update_period = input;

	return count;
}

static struct kobj_attribute update_period_attr =
__ATTR(update_period, 0644, show_update_period, store_update_period);

static ssize_t show_pago_enable(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%d\n", pago->enable);
}

static ssize_t store_pago_enable(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	input = !!input;

	if (input)
		pago_enable();
	else
		pago_disable();

	return count;
}

static struct kobj_attribute pago_enable_attr =
__ATTR(enable, 0644, show_pago_enable, store_pago_enable);

static ssize_t show_stall_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", pago->stall_threshold);
}

static ssize_t store_stall_threshold(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	u32 input;

	if (sscanf(buf, "%u", &input) != 1)
		return -EINVAL;

	pago->stall_threshold = input;

	return count;
}

static struct kobj_attribute stall_threshold_attr =
__ATTR(stall_threshold, 0644, show_stall_threshold, store_stall_threshold);

struct pago_attr {
	struct attribute attr;
	ssize_t (*show)(struct kobject *, char *);
	ssize_t (*store)(struct kobject *, const char *, size_t count);
};

static ssize_t show_freq_table(struct kobject *k, char *buf)
{
	struct pago_domain *dom = container_of(k, struct pago_domain, kobj);
	unsigned int count = 0;
	int i;

	count += scnprintf(buf, PAGE_SIZE, "CPU freq\tMem freq\n");

	for (i = 0; i < dom->table_size; i++) {
		count += scnprintf(buf + count, PAGE_SIZE - count, "%8u\t%8u\n",
				dom->freq_table[i].cpufreq, dom->freq_table[i].memfreq);
	}

	count += scnprintf(buf + count, PAGE_SIZE - count, "\n");

	return count;
}

static struct pago_attr freq_table_attr = __ATTR(freq_table, 0444, show_freq_table, NULL);

#define pago_attr_rw(scale, name)			\
static struct pago_attr name##_attr =		\
__ATTR(name, 0644, show_##name, store_##name)

#define pago_show(scale, name)								\
static ssize_t show_##name(struct kobject *k, char *buf)				\
{											\
	struct pago_##scale *scale = container_of(k, struct pago_##scale, kobj);	\
											\
	return sprintf(buf, "%u\n", scale->name);						\
}											\

#define pago_store(scale, name)								\
static ssize_t store_##name(struct kobject *k, const char *buf, size_t count)		\
{											\
	struct pago_##scale *scale = container_of(k, struct pago_##scale, kobj);	\
	unsigned int data;								\
											\
	if (!sscanf(buf, "%u", &data))							\
		return -EINVAL;								\
											\
	scale->name = data;								\
	return count;									\
}

pago_show(domain, perf);
pago_store(domain, perf);
pago_attr_rw(domain, perf);

pago_show(domain, ratio);
pago_store(domain, ratio);
pago_attr_rw(domain, ratio);

pago_show(group, boost_min);
pago_store(group, boost_min);
pago_attr_rw(group, boost_min);

pago_show(group, boost_threshold);
pago_store(group, boost_threshold);
pago_attr_rw(group, boost_threshold);

pago_show(group, force_boost_ms);
pago_store(group, force_boost_ms);
pago_attr_rw(group, force_boost_ms);

pago_show(group, bw_threshold);
pago_store(group, bw_threshold);
pago_attr_rw(group, bw_threshold);

pago_show(group, ramp_up_enable);
pago_store(group, ramp_up_enable);
pago_attr_rw(group, ramp_up_enable);

pago_show(group, shift_level);
pago_store(group, shift_level);
pago_attr_rw(group, shift_level);

static ssize_t show(struct kobject *kobj, struct attribute *at, char *buf)
{
	struct pago_attr *fvattr = container_of(at, struct pago_attr, attr);

	return fvattr->show(kobj, buf);
}

static ssize_t store(struct kobject *kobj, struct attribute *at,
		const char *buf, size_t count)
{
	struct pago_attr *fvattr = container_of(at, struct pago_attr, attr);

	return fvattr->store(kobj, buf, count);
}

static const struct sysfs_ops pago_sysfs_ops = {
	.show	= show,
	.store	= store,
};

static struct attribute *pago_domain_attrs[] = {
	&perf_attr.attr,
	&freq_table_attr.attr,
	&ratio_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(pago_domain);

static struct kobj_type ktype_pago_domain = {
	.sysfs_ops	= &pago_sysfs_ops,
	.default_groups	= pago_domain_groups,
};

static struct attribute *pago_group_attrs[] = {
	&boost_min_attr.attr,
	&boost_threshold_attr.attr,
	&force_boost_ms_attr.attr,
	&bw_threshold_attr.attr,
	&ramp_up_enable_attr.attr,
	&shift_level_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(pago_group);

static struct kobj_type ktype_pago_group = {
	.sysfs_ops	= &pago_sysfs_ops,
	.default_groups	= pago_group_groups,
};

static void pago_free(void)
{
	int i, j, cpu;

	if (!pago)
		return;

	if (pago->groups) {
		for (i = 0; i < pago->nr_groups; i++) {
			struct pago_group *grp = pago->groups[i];

			if (grp) {
				if (grp->domains) {
					for (j = 0; j < grp->nr_domains; j++) {
						struct pago_domain *dom = grp->domains[j];

						if (dom) {
							if (dom->freq_table)
								kfree(dom->freq_table);
							kfree(dom);
						}
					}

					kfree(grp->domains);
				}
				if (grp->miss_idx)
					kfree(grp->miss_idx);
				kfree(grp);
			}
		}

		for_each_possible_cpu(cpu) {
			struct pago_cpu *data = &per_cpu(pago_cpu, cpu);

			if (data->domains)
				kfree(data->domains);
			if (data->target_freq)
				kfree(data->target_freq);

			deactivate_pmu_events(data);
		}

		kfree(pago->groups);
	}

	if (pago->thread)
		kthread_stop(pago->thread);
	if (pago->kobj)
		kfree(pago->kobj);
	kfree(pago);
}

static int pago_sysfs_create(struct kobject *ems_kobj)
{
	pago->kobj = kobject_create_and_add("pago", ems_kobj);
	if (!pago->kobj)
		return -ENOMEM;

	if (sysfs_create_file(pago->kobj, &update_period_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(pago->kobj, &pago_enable_attr.attr))
		return -ENOMEM;

	if (sysfs_create_file(pago->kobj, &cme_cnt_attr.attr))
		return -ENOMEM;

	if (system_supports_sme()) {
		if (sysfs_create_file(pago->kobj, &cme_prio_attr.attr))
			return -ENOMEM;
	}

	if (sysfs_create_file(pago->kobj, &stall_threshold_attr.attr))
		return -ENOMEM;

	return 0;
}

static int pago_kthread_create(struct device_node *dn)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO / 2 };
	const char *buf;

	init_irq_work(&pago->irq_work, pago_irq_work);
	kthread_init_work(&pago->work, pago_work);
	kthread_init_worker(&pago->worker);

	thread = kthread_create(kthread_worker_fn, &pago->worker, "pagov");
	if (IS_ERR(thread)) {
		pr_err("failed to create pago thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	if (sched_setscheduler_nocheck(thread, SCHED_FIFO, &param)) {
		kthread_stop(thread);
		pr_err("failed to set SCHED_FIFO\n");
		return -1;
	}

	if (!of_property_read_string(dn, "thread-run-on", &buf))
		cpulist_parse(buf, &pago->thread_allowed_cpus);
	else
		cpumask_copy(&pago->thread_allowed_cpus, cpu_possible_mask);

	set_cpus_allowed_ptr(thread, &pago->thread_allowed_cpus);
	thread->flags |= PF_NO_SETAFFINITY;
	pago->thread = thread;

	wake_up_process(pago->thread);

	return 0;
}

static int remove_invalid_freq_from_table(struct device_node *dn, struct pago_group *grp,
				unsigned int *raw_table, int raw_table_size)
{
	unsigned long cal_table[100];
	int cal_table_size, i, j, ret;
	bool found;
	unsigned int *temp_table;
	int valid_count = 0, invalid_count = 0;
	unsigned int *invalid_table;

	ret = of_property_read_u32(dn, "cal-id", &pago->cal_id);
	if (ret)
		return -EINVAL;

	cal_table_size = cal_dfs_get_rate_table(pago->cal_id, cal_table);
	if (!cal_table_size)
		return -ENOENT;

	temp_table = kcalloc(raw_table_size, sizeof(unsigned int), GFP_KERNEL);
	if (!temp_table)
		return -ENOMEM;

	invalid_table = kcalloc(raw_table_size, sizeof(unsigned int), GFP_KERNEL);
	if (!invalid_table) {
		kfree(temp_table);
		return -ENOMEM;
	}

	for (i = 0; i < raw_table_size; i++) {
		found = false;
		for (j = 0; j < cal_table_size; j++) {
			if (raw_table[i] == (unsigned int)cal_table[j]) {
				found = true;
				break;
			}
		}

		if (found) {
			temp_table[valid_count++] = raw_table[i];
		} else {
			if (invalid_count < raw_table_size) {
				invalid_table[invalid_count] = raw_table[i];
				invalid_count++;
			}
		}
	}

	memcpy(raw_table, temp_table, valid_count * sizeof(unsigned int));
	grp->invalid_ftable = invalid_table;
	grp->invalid_size = invalid_count;

	kfree(temp_table);

	return valid_count;
}

unsigned int *pago_init_freq_table(struct device_node *dn, struct pago_group *grp, int *table_size)
{
	int i, raw_table_size, ret = 0;
	unsigned int *raw_table;
	unsigned long *temp_table = NULL;

	raw_table_size = of_property_count_u32_elems(dn, "mif-table");
	if (raw_table_size < 0)
		temp_table = ems_get_dsufreq_table(&raw_table_size);

	raw_table = kcalloc(raw_table_size, sizeof(u32), GFP_KERNEL);
	if (!raw_table) {
		*table_size = 0;
		return NULL;
	}

	if (temp_table) {
		for (i = 0; i < raw_table_size; i++)
			raw_table[i] = (unsigned int)temp_table[i];
	} else {
		ret = of_property_read_u32_array(dn, "mif-table", raw_table, raw_table_size);
		if (ret) {
			kfree(raw_table);
			*table_size = 0;
			return NULL;
		}

		raw_table_size = remove_invalid_freq_from_table(dn, grp, raw_table, raw_table_size);
		if (raw_table_size < 0)
			return NULL;
	}

	*table_size = raw_table_size;
	return raw_table;
}

static int pago_register(struct kobject *ems_kobj, struct device_node *dn)
{
	pago = kzalloc(sizeof(struct pago), GFP_KERNEL);
	if (!pago) {
		pr_err("failed to allocate PAGO\n");
		return -ENOMEM;
	}

	if (pago_sysfs_create(ems_kobj)) {
		pr_err("failed to create sysfs\n");
		return -ENOMEM;
	}

	if (pago_kthread_create(dn)) {
		pr_err("failed to create kthread\n");
		return -ENOMEM;
	}

	pago->nr_groups = of_get_child_count(dn);
	if (pago->nr_groups <= 0) {
		pr_err("%s: failed to get pago group\n", __func__);
		return -EINVAL;
	}

	pago->groups = kmalloc_array(pago->nr_groups, sizeof(struct pago_group *), GFP_KERNEL);
	if (!pago->groups) {
		pr_err("%s: failed to alloc pago group\n", __func__);
		return -ENOMEM;
	}

	pago->update_period = DEFAULT_UPDATE_MS;
	raw_spin_lock_init(&pago->lock);

	pago->emstune.activated = false;
	pago->emstune.activated_ts = 0;
	atomic_set(&pago->suspended, false);

	return 0;
}

static int pago_group_init(struct device_node *dn, struct pago_group *grp)
{
	if (of_property_read_string(dn, "desc", &grp->name)) {
		pr_err("%s: failed to get group name\n", __func__);
		return -EINVAL;
	}

	if (kobject_init_and_add(&grp->kobj, &ktype_pago_group, pago->kobj,
				"%s", grp->name)) {
		pr_err("%s: failed to init kobject\n", __func__);
		return -ENOMEM;
	}

	grp->nr_domains = of_get_child_count(dn);
	if (grp->nr_domains <= 0) {
		pr_err("%s: failed to get domains for pago group\n", __func__);
		return -EINVAL;
	}

	grp->domains = kmalloc_array(grp->nr_domains, sizeof(struct pago_domain *), GFP_KERNEL);
	if (!grp->domains) {
		pr_err("%s: failed to alloc domains for pago group\n", __func__);
		return -ENOMEM;
	}

	if (!of_property_read_u32(dn, "pm-qos-class", &grp->pm_qos_class))
		exynos_pm_qos_add_request(&grp->pm_qos, grp->pm_qos_class, 0);
	else
		grp->pm_qos_class = 0;

	if (of_property_read_u32(dn, "force-boost-ms", &grp->force_boost_ms))
		grp->force_boost_ms = DEFAULT_UPDATE_MS * 10;

	if (of_property_read_u32(dn, "boost-threshold", &grp->boost_threshold))
		grp->boost_threshold = UINT_MAX;

	if (of_property_read_u32(dn, "boost-min", &grp->boost_min))
		grp->boost_min = 0;

	if (of_property_read_s32(dn, "bw-threshold", &grp->bw_threshold))
		grp->bw_threshold = DEFAULT_BW_THRESHOLD;

	if (of_property_read_s32(dn, "ramp-up-enable", &grp->ramp_up_enable))
		grp->ramp_up_enable = 1;

	grp->num_miss_idx = of_property_count_u32_elems(dn, "miss-idx");
	if (grp->num_miss_idx > 0) {
		grp->miss_idx = kmalloc_array(grp->num_miss_idx, sizeof(unsigned int), GFP_KERNEL);
		if (!grp->miss_idx) {
			pr_err("%s: failed to alloc miss_idx for pago group\n", __func__);
			return -ENOMEM;
		}

		if (of_property_read_u32_array(dn, "miss-idx", grp->miss_idx, grp->num_miss_idx)) {
			pr_err("%s: failed to get miss-idx for pago group\n", __func__);
			return -ENOMEM;
		}
	} else {
		grp->num_miss_idx = 0;
		grp->miss_idx = NULL;
	}

	grp->is_cme = of_property_read_bool(dn, "cme");

	grp->ftable = pago_init_freq_table(dn, grp, &grp->ftable_size);

	grp->max_mem_freq = 0;
	grp->max_boost_freq = 0;
	grp->max_target_freq = 0;
	grp->max_perf_cycle_freq = 0;

	grp->resolved_freq = 0;

	return 0;
}

static int pago_init_domain(struct device_node *dn, struct pago_group *grp,
		struct pago_domain *dom)
{
	const char *buf;
	unsigned int *raw_table;
	int raw_table_size, i, ret, table_size = 0;
	struct cpu_mem_map *table;
	struct cpumask phy_cpus, phy_pol_cpus;
	struct ems_cpu_topology *topo;
	int cpu;

	if (!of_property_read_string(dn, "cpus", &buf)) {
		if (cpumask_equal(cpu_possible_mask, get_possible_phy_cpus()))
			ems_cpulist_parse(buf, &dom->cpus);
		else {
			ems_cpulist_parse(buf, &phy_cpus);
			cpumask_and(&phy_pol_cpus, &phy_cpus, get_possible_phy_cpus());
			if (cpumask_empty(&phy_pol_cpus))
				return PARSE_EMPTY;

			topo = percpu_topology(get_cpu_log_id(cpumask_first(&phy_pol_cpus)));
			cpumask_copy(&dom->cpus, &topo->cluster_cpus);

			pr_info("%s: phy_cpus:0x%03x pol_cpus:0x%03x", __func__,
					*(unsigned int *)cpumask_bits(&phy_cpus),
					*(unsigned int *)cpumask_bits(&dom->cpus));
		}
	} else {
		pr_err("%s: failed to parse cpus\n", __func__);
		return -EINVAL;
	}

	ret = kobject_init_and_add(&dom->kobj, &ktype_pago_domain, &grp->kobj,
				"domain%d", cpumask_first(&dom->cpus));
	if (ret) {
		pr_err("%s: failed to init kobject\n", __func__);
		return ret;
	}

	dom->perf = of_property_read_bool(dn, "perf");

	for_each_cpu(cpu, &dom->cpus) {
		struct pago_cpu *data = &per_cpu(pago_cpu, cpu);

		if (!of_property_read_u32(dn, "prio_lvl", &dom->prio_lvl))
			data->cme_prio_lvl = dom->prio_lvl;
		else
			data->cme_prio_lvl = 0;

		data->cme_prio_lvl_backup = data->cme_prio_lvl;
	}

	if (!of_property_read_u32(dn, "ratio", &dom->ratio)) {
		dom->table_size = 0;
		dom->freq_table = NULL;
		return ret;
	} else {
		dom->ratio = 0;
	}

	raw_table_size = of_property_count_u32_elems(dn, "cpu-mem-table");
	if (raw_table_size <= 0) {
		pr_err("%s: failed to count raw table size\n", __func__);
		return -EINVAL;
	}

	raw_table = kmalloc_array(raw_table_size, sizeof(unsigned int), GFP_KERNEL);
	if (!raw_table) {
		pr_err("%s: failed to alloc raw table\n", __func__);
		return -ENOMEM;
	}

	ret = of_property_read_u32_array(dn, "cpu-mem-table", raw_table, raw_table_size);
	if (ret) {
		pr_err("%s: failed to parse raw table\n", __func__);
		goto out;
	}

	table = kcalloc(raw_table_size / 2, sizeof(struct cpu_mem_map), GFP_KERNEL);
	if (!table) {
		pr_err("%s: failed to alloc cpu mem table\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < raw_table_size; i += 2) {
		unsigned int cpufreq = raw_table[i];
		unsigned int memfreq = raw_table[i + 1];
		int j;

		if (grp->invalid_ftable && grp->invalid_size > 0) {
			bool found = false;
			for (j = 0; j < grp->invalid_size; j++) {
				if (memfreq == grp->invalid_ftable[j]) {
					found = true;
					break;
				}
			}
			if (found)
				continue;
		}

		table[table_size].cpufreq = cpufreq;
		table[table_size].memfreq = memfreq;
		table_size++;
	}

	dom->table_size = table_size;
	dom->freq_table = table;

out:
	kfree(raw_table);

	return ret;
}

static int pago_init_cpus(void)
{
	int cpu, i, j, ret = 0;

	cpus_read_lock();

	for_each_possible_cpu(cpu) {
		struct pago_cpu *data = &per_cpu(pago_cpu, cpu);

		data->nr_domains = pago->nr_groups;
		data->domains = kmalloc_array(data->nr_domains, sizeof(struct pago_domain *), GFP_KERNEL);
		if (!data->domains) {
			pr_err("%s: failed to alloc domains. CPU=%d\n", __func__, cpu);
			ret = -ENOMEM;
			goto unlock;
		}

		data->target_freq = kmalloc_array(data->nr_domains, sizeof(unsigned int), GFP_KERNEL);
		if (!data->target_freq) {
			pr_err("%s: failed to alloc target_freq. CPU=%d\n", __func__, cpu);
			ret = -ENOMEM;
			goto unlock;
		}

		for (i = 0; i < pago->nr_groups; i++) {
			struct pago_group *grp = pago->groups[i];

			for (j = 0; j < grp->nr_domains; j++) {
				struct pago_domain *dom = grp->domains[j];

				if (cpumask_test_cpu(cpu, &dom->cpus))
					data->domains[i] = dom;
			}
		}

		data->cpu = cpu;
		data->cpu_up = cpumask_test_cpu(cpu, cpu_online_mask);
		data->enabled = true;
		data->idle_sample = false;
		data->last_sampled = 0;
		data->last_updated = 0;
		raw_spin_lock_init(&data->lock);

		if (activate_pmu_events(data) < 0) {
			data->enabled = false;
			pr_err("%s: failed to enable pmu events. CPU=%d\n", __func__, cpu);
		}

		pr_info("cpu%d: pmu event init complete.\n", cpu);
	}

	cpuhp_state = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_AP_ONLINE_DYN, "pago", pago_cpu_up, pago_cpu_down);
	if (cpuhp_state < 0) {
		pr_err("%s: failed to init pago hotplug\n", __func__);
		ret = cpuhp_state;
	}

unlock:
	cpus_read_unlock();

	return ret;
}

static int pago_domain_register(struct device_node *dn, struct pago_group *grp)
{
	struct device_node *child;
	int idx = 0, ret = 0;

	for_each_child_of_node(dn, child) {
		struct pago_domain *dom;

		dom = kzalloc(sizeof(struct pago_domain), GFP_KERNEL);
		if (!dom) {
			pr_err("%s: failed to alloc pago domain\n", __func__);
			return -ENOMEM;
		}

		ret = pago_init_domain(child, grp, dom);
		if (ret == PARSE_EMPTY) {
			kfree(dom);
			continue;
		} else if (ret) {
			return ret;
		}

		dom->grp = grp;
		grp->domains[idx] = dom;
		if (dom->freq_table) {
			grp->max_mem_freq = max(grp->max_mem_freq,
					dom->freq_table[dom->table_size - 1].memfreq);
		}
		idx++;
	}

	/* update nr_domains for removed cpus */
	grp->nr_domains = idx;
	return 0;
}

int pago_init(struct kobject *ems_kobj)
{
	struct device_node *dn, *child;
	int idx = 0;

	dn = of_find_node_by_path("/ems/pago");
	if (!dn)
		return -1;

	if (pago_register(ems_kobj, dn))
		goto fail;

	if (of_property_read_u32(dn, "stall-threshold", &pago->stall_threshold))
		pago->stall_threshold = 50;

	/* Initialize pago group and domain */
	for_each_child_of_node(dn, child) {
		struct pago_group *grp;

		grp = kzalloc(sizeof(struct pago_group), GFP_KERNEL);
		if (!grp) {
			pr_err("%s: failed to alloc pago group\n", __func__);
			return -ENOMEM;
		}

		if (pago_group_init(child, grp)) {
			pr_err("%s: failed to init pago group\n", __func__);
			goto fail;
		}

		if (pago_domain_register(child, grp)) {
			pr_err("%s: failed to regist pago domain\n", __func__);
			goto fail;
		}

		pago->groups[idx] = grp;
		idx++;
	}

	if (pago_init_cpus())
		goto fail;

	if (system_supports_sme()) {
		stop_machine(pago_cme_prio_update, NULL, cpu_online_mask);
		pr_info("pago cme prio initialized");

		pago->sysbusy_boost = false;
		sysbusy_register_notifier(&pago_sysbusy_notifier);
	}

	pago->enable = true;

	pr_info("PAGO init complete.\n");

	return 0;

fail:
	pago_free();

	return -1;
}
