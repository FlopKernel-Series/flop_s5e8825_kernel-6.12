/*
 * Task Profiler for Exynos Mobile Scheduler
 *
 * Copyright (C) 2025 Samsung Electronics Co., Ltd
 */
#include <soc/samsung/game.h>
#include <soc/samsung/gpu_cooling.h>

#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

struct cpu_profiler_struct cpu_profiler[VENDOR_NR_CPUS];
struct freq_profiler_struct freq_profiler;

/********************************************************************************
 *				Util Tracking					*
 ********************************************************************************/
unsigned long hist_util_cpu(int cpu)
{
	return cpu_profiler[cpu].util.hist_util;
}

static bool need_update_util_contrib(int flag)
{
	if (flag == NEW_TASK || flag == WAKEUP_TASK)
		return false;

	/* UPDATE_TASK, TASK_SWITCH, UPDATE_BLOCKED_LOAD */
	return true;
}

static void update_util_contrib(int cpu, u64 now, int flag)
{
	struct cpu_profiler_util *data = &cpu_profiler[cpu].util;
	struct rq *rq = cpu_rq(cpu);
	u64 delta, cpu_util;

	lockdep_assert_held(&data->lock);

	if (unlikely(now < data->last_updated))
		return;

	if (!need_update_util_contrib(flag))
		goto out;

	delta = now - data->last_updated;

	cpu_util = ml_cpu_util_avg(cpu) + cpu_util_rt(rq) + cpu_util_dl(rq);
	cpu_util = ml_cpu_util_with_irq(rq, cpu_util);
	cpu_util = min(cpu_util, (u64)ems_get_actual_cpu_capacity(cpu));
	data->util_contrib += cpu_util * delta;
	data->contrib += delta;

	trace_update_cpu_hist_contrib(cpu, data->util_contrib, data->contrib, now,
			mlt_load_flag_name[flag]);
out:
	data->last_updated = now;
}

static void update_cpu_avg_util(int cpu, u64 now)
{
	struct cpu_profiler_util *data = &cpu_profiler[cpu].util;
	unsigned long flags;

	raw_spin_lock_irqsave(&data->lock, flags);

	update_util_contrib(cpu, now, UPDATE_TASK);

	if (data->contrib)
		data->hist_util = data->util_contrib / data->contrib;
	else
		data->hist_util = 0;

	data->util_contrib = 0;
	data->contrib = 0;
	data->last_updated = now;

	raw_spin_unlock_irqrestore(&data->lock, flags);

	trace_update_cpu_hist_util(cpu, data->hist_util, now);
}

/********************************************************************************
 *				Freq Tracking					*
 ********************************************************************************/
static int get_cluster_index(int cpu)
{
	return cpu_profiler[cpu].cluster_idx;
}

int get_avg_cur_freq(int cpu)
{
	int cluster = get_cluster_index(cpu);

	return freq_profiler.cpu[cluster].avg_cur_freq;
}

int get_avg_max_freq(int cpu)
{
	int cluster = get_cluster_index(cpu);

	return freq_profiler.cpu[cluster].avg_max_freq;
}

int get_orig_max_freq(int cpu)
{
	int cluster = get_cluster_index(cpu);

	return freq_profiler.cpu[cluster].orig_max_freq;
}

int get_avg_gpu_cur_freq(void)
{
	return freq_profiler.gpu.avg_cur_freq;
}

int get_avg_gpu_max_freq(void)
{
	return freq_profiler.gpu.avg_max_freq;
}

int get_orig_gpu_min_freq(void)
{
	return freq_profiler.gpu.orig_min_freq;
}

static void update_freq_contrib(struct freq_profile_data *data, u64 now)
{
	u64 delta;

	lockdep_assert_held(&data->lock);

	if (unlikely(now < data->last_updated))
		return;

	delta = now - data->last_updated;

	data->cur_contrib += data->cur_freq * delta;
	data->max_contrib += data->max_freq * delta;
	data->contrib += delta;

	data->last_updated = now;
}

static void update_avg_freq(struct freq_profile_data *data)
{
	lockdep_assert_held(&data->lock);

	if (data->contrib) {
		data->avg_cur_freq = div_u64(data->cur_contrib, data->contrib);
		data->avg_max_freq = div_u64(data->max_contrib, data->contrib);
	} else {
		data->avg_cur_freq = 0;
		data->avg_max_freq = 0;
	}

	data->cur_contrib = 0;
	data->max_contrib = 0;
	data->contrib = 0;
}

static void update_cpu_avg_freq(int cpu, u64 now)
{
	struct freq_profile_data *data;
	int cluster = get_cluster_index(cpu);
	unsigned long flags;

	data = &freq_profiler.cpu[cluster];

	raw_spin_lock_irqsave(&data->lock, flags);

	update_freq_contrib(data, now);
	update_avg_freq(data);

	raw_spin_unlock_irqrestore(&data->lock, flags);

	trace_freq_profiler_update(cluster, data->avg_cur_freq, data->avg_max_freq, data->orig_max_freq);
}

static void update_gpu_avg_freq(u64 now)
{
	struct freq_profile_data *data = &freq_profiler.gpu;
	unsigned long flags;

	raw_spin_lock_irqsave(&data->lock, flags);

	update_freq_contrib(data, now);
	update_avg_freq(data);

	raw_spin_unlock_irqrestore(&data->lock, flags);

	trace_freq_profiler_update(4, data->avg_cur_freq, data->avg_max_freq, data->orig_max_freq);
}

/********************************************************************************
 *				Uarch Tracking					*
 ********************************************************************************/
u64 cpu_profiler_read_pmu_event(int cpu, int event)
{
	struct cpu_profiler_uarch *uarch = &cpu_profiler[cpu].uarch;
	u64 ret = 0;

	if (uarch->enabled && uarch->perf_event[event])
		perf_event_read_local(uarch->perf_event[event], &ret, NULL, NULL);

	return ret;
}

static void deactivate_pmu_events(int cpu)
{
	struct cpu_profiler_uarch *uarch = &cpu_profiler[cpu].uarch;
	int i;

	uarch->enabled = 0;

	for (i = 0; i < MAX_NR_EVENTS; i++) {
		if (uarch->perf_event[i]) {
			exynos_perf_event_release_kernel(uarch->perf_event[i]);
			uarch->perf_event[i] = NULL;
		}
	}
}

static int activate_pmu_events(int cpu)
{
	struct cpu_profiler_uarch *uarch = &cpu_profiler[cpu].uarch;
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

	for (i = 0; i < MAX_NR_EVENTS; i++) {
		pe_attr->config = egg_pmu_event_id[i];

		pe = exynos_perf_create_kernel_counter(pe_attr, cpu, NULL, NULL, NULL);
		if (!pe) {
			pr_err("failed to create kernel perf event. CPU=%d\n", cpu);
			kfree(pe_attr);
			return -ENOMEM;
		}

		uarch->perf_event[i] = pe;
	}

	uarch->enabled = 1;

	kfree(pe_attr);

	pr_info("CPU Profiler: pmu events are activated on CPU=%d\n", cpu);

	return 0;
}

static int cpu_profiler_cpu_up(unsigned int cpu)
{
	if (activate_pmu_events(cpu) < 0)
		pr_err("%s: failed to enable pmu events. CPU=%d\n", __func__, cpu);

	return 0;
}

static int cpu_profiler_cpu_down(unsigned int cpu)
{
	deactivate_pmu_events(cpu);

	return 0;
}

/********************************************************************************
*				 Profiler Update				*
 ********************************************************************************/
void cpu_profiler_update_util(struct rq *rq, u64 now, int flag)
{
	struct cpu_profiler_util *data = &cpu_profiler[cpu_of(rq)].util;
	unsigned long flags;

	raw_spin_lock_irqsave(&data->lock, flags);
	update_util_contrib(cpu_of(rq), now, flag);
	raw_spin_unlock_irqrestore(&data->lock, flags);
}

void cpu_profiler_update_frequency(struct cpufreq_policy *policy)
{
	struct freq_profile_data *data;
	int cluster = get_cluster_index(policy->cpu);
	unsigned long flags;

	data = &freq_profiler.cpu[cluster];

	raw_spin_lock_irqsave(&data->lock, flags);

	update_freq_contrib(data, sched_clock());
	data->cur_freq = policy->cur;
	data->max_freq = policy->max;

	raw_spin_unlock_irqrestore(&data->lock, flags);
}

void cpu_profiler_update(u64 now)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (cpu == cpumask_first(cpu_clustergroup_mask(cpu)))
			update_cpu_avg_freq(cpu, now);

		update_cpu_avg_util(cpu, now);
	}

	update_gpu_avg_freq(now);
}

void gpu_freq_prof_update(void)
{
	struct freq_profile_data *data = &freq_profiler.gpu;
	const struct gpu_dvfs_fn *gpu_dvfs_fn = freq_profiler.gpu_dvfs_fn;
	int cur_freq = 0;
	unsigned long flags;

	if (gpu_dvfs_fn)
		cur_freq = gpu_dvfs_fn->get_cur_freq();

	raw_spin_lock_irqsave(&data->lock, flags);

	update_freq_contrib(data, sched_clock());
	data->cur_freq = cur_freq;

	raw_spin_unlock_irqrestore(&data->lock, flags);
}

/********************************************************************************
 *				Initialization					*
 ********************************************************************************/
int freq_profiler_gpu_fn_init(const struct platform_device *pdev, const struct gpu_dvfs_fn* fn)
{
	if (!fn) {
		pr_err("EMS: %s: failed init gpu_dvfs_fn\n", __func__);
		return -EINVAL;
	}

	freq_profiler.gpu_dvfs_fn = fn;

	return 0;
}
EXPORT_SYMBOL(freq_profiler_gpu_fn_init);

static void reset_cpu_util_data(int cpu, u64 now)
{
	struct cpu_profiler_util *data = &cpu_profiler[cpu].util;
	unsigned long flags;

	raw_spin_lock_irqsave(&data->lock, flags);
	data->hist_util = 0;
	data->contrib = 0;
	data->util_contrib = 0;
	data->last_updated = now;
	raw_spin_unlock_irqrestore(&data->lock, flags);
}

static void reset_cpu_freq_data(int cpu, u64 now)
{
	struct freq_profile_data *data = &freq_profiler.cpu[get_cluster_index(cpu)];
	struct cpufreq_policy *policy;
	int orig_max_freq = 0;
	unsigned long flags;

	policy = cpufreq_cpu_get_raw(cpu);
	if (policy) {
		orig_max_freq = policy->cpuinfo.max_freq;
	}

	raw_spin_lock_irqsave(&data->lock, flags);
	data->orig_min_freq = 0;
	data->orig_max_freq = orig_max_freq;
	data->cur_freq = data->max_freq = 0;
	data->avg_cur_freq = data->avg_max_freq = 0;
	data->cur_contrib = data->max_contrib = 0;
	data->contrib = 0;
	data->last_updated = now;
	raw_spin_unlock_irqrestore(&data->lock, flags);
}

static void reset_gpu_freq_data(u64 now)
{
	struct freq_profile_data *data = &freq_profiler.gpu;
	const struct gpu_dvfs_fn *gpu_dvfs_fn = freq_profiler.gpu_dvfs_fn;
	int orig_min_freq = 0, cur_freq = 0;
	unsigned long flags;

	if (gpu_dvfs_fn) {
		int gpu_num_lv = gpu_dvfs_fn->get_num_lv();
		orig_min_freq = gpu_dvfs_fn->get_freq(gpu_num_lv - 1);
		cur_freq = gpu_dvfs_fn->get_cur_freq();
	}

	raw_spin_lock_irqsave(&data->lock, flags);
	data->avg_cur_freq = data->avg_max_freq = 0;
	data->max_freq = data->orig_max_freq = 0;
	data->cur_contrib = data->max_contrib = 0;
	data->contrib = 0;
	data->orig_min_freq = orig_min_freq;
	data->cur_freq = cur_freq;
	data->last_updated = now;
	raw_spin_unlock_irqrestore(&data->lock, flags);
}

void reset_cpu_profiler(void)
{
	int cpu;
	u64 now = sched_clock();

	for_each_possible_cpu(cpu) {
		if (cpu == cpumask_first(cpu_clustergroup_mask(cpu)))
			reset_cpu_freq_data(cpu, now);

		reset_cpu_util_data(cpu, now);
	}

	reset_gpu_freq_data(now);
}

void cpu_profiler_init(struct kobject *egg_kobj)
{
	int cpuhp_state, cluster_cpu, cluster = 0;
	u64 now = sched_clock();

	for_each_possible_cpu(cluster_cpu) {
		if (cluster_cpu == cpumask_first(cpu_clustergroup_mask(cluster_cpu))) {
			struct freq_profile_data *freq_data = &freq_profiler.cpu[cluster];
			int cpu;

			memset(freq_data, 0, sizeof(struct freq_profile_data));

			raw_spin_lock_init(&freq_data->lock);
			freq_data->last_updated = now;

			for_each_cpu(cpu, cpu_clustergroup_mask(cluster_cpu)) {
				struct cpu_profiler_struct *cps = &cpu_profiler[cpu];

				memset(cps, 0, sizeof(struct cpu_profiler_struct));

				raw_spin_lock_init(&cps->util.lock);

				if (activate_pmu_events(cpu) < 0) {
					cps->uarch.enabled = 0;
					pr_err("%s: failed to enable pmu events. CPU=%d\n", __func__, cpu);
				}

				cps->cluster_idx = cluster;
			}

			cluster++;
		}
	}

	cpuhp_state = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
			"cpu_profiler", cpu_profiler_cpu_up, cpu_profiler_cpu_down);
	if (cpuhp_state < 0)
		pr_err("%s: failed to init cpu_profiler hotplug\n", __func__);

	memset(&freq_profiler.gpu, 0, sizeof(struct freq_profile_data));
	raw_spin_lock_init(&freq_profiler.gpu.lock);
	freq_profiler.gpu.last_updated = now;

	pr_info("EMS: CPU Profiler Initialized\n");
}
