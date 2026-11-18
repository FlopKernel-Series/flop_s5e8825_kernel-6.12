/*
 * Profiler for Exynos Mobile Scheduler
 *
 * Copyright (C) 2025 Samsung Electronics Co., Ltd
 */

#include <soc/samsung/game.h>


void profiler_update(u64 profile_interval)
{
	u64 now = sched_clock();

	fps_profiler_update();
	cpu_profiler_update(now);
	task_profiler_update(profile_interval, now);
	dct_cpu_load_update();
}

/********************************************************************************
 *				Vendor Hooks					*
 ********************************************************************************/
void profiler_task_switch(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	u64 now = sched_clock();

	if (prev != next) {
		cpu_profiler_update_util(rq, now, PUT_PREV_TASK);
		task_profiler_update_contrib(rq, prev, now, PUT_PREV_TASK);
		task_profiler_update_contrib(rq, next, now, PICK_NEXT_TASK);
	} else {
		cpu_profiler_update_util(rq, now, UPDATE_TASK);
		task_profiler_update_contrib(rq, prev, now, UPDATE_TASK);
	}
}

void profiler_tick_entry(struct rq *rq)
{
	u64 now = sched_clock();

	cpu_profiler_update_util(rq, now, UPDATE_TASK);
	task_profiler_update_contrib(rq, rq->curr, now, UPDATE_TASK);
}

void profiler_update_blocked_load(int cpu)
{
	cpu_profiler_update_util(cpu_rq(cpu), sched_clock(), UPDATE_BLOCKED_LOAD);
}

void profiler_try_to_wake_up(struct task_struct *p)
{
	struct rq *rq = task_rq(p);
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	task_profiler_try_to_wake_up(rq, p);
	rq_unlock_irqrestore(rq, &rf);
}

void profiler_new_task_stats(struct task_struct *p)
{
	task_profiler_update_contrib(task_rq(p), p, sched_clock(), NEW_TASK);
}

void profiler_init_task(struct task_struct *p)
{
	task_profiler_init_task(p);
}

void profiler_cgroup_attach(struct cgroup_taskset *tset)
{
	task_profiler_cgroup_attach(tset);
}

void profiler_flush_task(struct task_struct *p)
{
	task_profiler_flush_task(p);
}

void profiler_cpu_frequency_limits(struct cpufreq_policy *policy)
{
	cpu_profiler_update_frequency(policy);
}

void profiler_cpufreq_transition(struct cpufreq_policy *policy)
{
	cpu_profiler_update_frequency(policy);
}

/********************************************************************************
 *				Initialization					*
 ********************************************************************************/
void profiler_init(struct kobject *egg_kobj)
{
	fps_profiler_init(egg_kobj);
	cpu_profiler_init(egg_kobj);
	task_profiler_init(egg_kobj);

	pr_info("EMS: EGG Profiler Initialized\n");
}
