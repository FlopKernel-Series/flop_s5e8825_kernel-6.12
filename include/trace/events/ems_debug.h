/*
 *  Copyright (C) 2019 Park Bumgyu <bumgyu.park@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM ems_debug

#if !defined(_TRACE_EMS_DEBUG_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_EMS_DEBUG_H

#include <linux/tracepoint.h>

struct mlt_cpu;
struct mlt_runnable;
struct mlt_uarch;
struct mlt_part;
struct mlt_task;
struct rq;
struct tp_env;
struct cpu_profile;
struct ems_dslt_task;
struct ems_dslt_rq;
struct dslt_avg;
struct tex_struct;
struct task_profiler_struct;
struct task_profiler_uarch;
struct task_profiler_cluster_util;
struct dct_cl_load;
struct cleave_task;
struct egg_task_struct;

#define LABEL_LEN 32

/*
 * Tracepoint for multiple test
 */
TRACE_EVENT(ems_test,

	TP_PROTO(struct task_struct *p, int cpu, u64 arg1, u64 arg2, char *event),

	TP_ARGS(p, cpu, arg1, arg2, event),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cpu			)
		__field(	u64,		arg1			)
		__field(	u64,		arg2			)
		__array(	char,		event,	LABEL_LEN	)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p ? p->comm : "no task", TASK_COMM_LEN);
		__entry->pid			= p ? p->pid : -1;
		__entry->cpu			= cpu;
		__entry->arg1			= arg1;
		__entry->arg2			= arg2;
		strncpy(__entry->event, event ? event : "no event", LABEL_LEN - 1);
	),

	TP_printk("comm=%15s, pid=%6d, cpu=%1d, arg1=%20llu, arg2=%20llu, event=%s",
		__entry->comm, __entry->pid, __entry->cpu,
		__entry->arg1, __entry->arg2, __entry->event)
);

/*
 * Tracepoint for taking util snapshot
 */
TRACE_EVENT(ems_take_util_snapshot,

	TP_PROTO(int cpu, struct tp_env *env),

	TP_ARGS(cpu, env),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cpu			)
		__field(	unsigned long,	task_util		)
		__field(	unsigned long,	util			)
		__field(	unsigned long,	util_wo			)
		__field(	unsigned long,	util_with		)
		__field(	int,		runnable		)
		__field(	unsigned long,	rt_util			)
		__field(	unsigned long,	dl_util			)
		__field(	unsigned int,	nr_running		)
		__field(	int,		idle			)
		__field(	int,		table_index		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, env->p->comm, TASK_COMM_LEN);
		__entry->pid			= env->p->pid;
		__entry->cpu       		= cpu;
		__entry->task_util		= env->task_util;
		__entry->util			= env->cpu_stat[cpu].util;
		__entry->util_wo   		= env->cpu_stat[cpu].util_wo;
		__entry->util_with 		= env->cpu_stat[cpu].util_with;
		__entry->runnable 		= env->cpu_stat[cpu].runnable;
		__entry->rt_util   		= env->cpu_stat[cpu].rt_util;
		__entry->dl_util   		= env->cpu_stat[cpu].dl_util;
		__entry->nr_running		= env->cpu_stat[cpu].nr_running;
		__entry->idle			= env->cpu_stat[cpu].idle;
		__entry->table_index		= env->table_index;
	),

	TP_printk("comm=%15s, pid=%6d, cpu=%1d, "
		"task_util=%4lu, "
		"util=%4lu, util_wo=%4lu, util_with=%4lu, runnable=%5d, "
		"rt_util=%4lu, dl_util=%4lu, nr_running=%2u, idle=%1d, tbl_idx=%2d",
		__entry->comm, __entry->pid, __entry->cpu,
		__entry->task_util,
		__entry->util, __entry->util_wo, __entry->util_with,
		__entry->runnable, __entry->rt_util, __entry->dl_util,
		__entry->nr_running, __entry->idle,
		__entry->table_index)
);

/******************************************************************************
 * Tracepoint for TEX                                                         *
 ******************************************************************************/
TRACE_EVENT(tex_boosted_fit_cpus,

	TP_PROTO(struct task_struct *p, int sync, struct cpumask *fit_cpus),

	TP_ARGS(p, sync, fit_cpus),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		prev_cpu		)
		__field(	int,		sync			)
		__field(	unsigned int,	fit_cpus		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->prev_cpu		= task_cpu(p);
		__entry->sync			= sync;
		__entry->fit_cpus		= *(unsigned int *)cpumask_bits(fit_cpus);
	),

	TP_printk("comm=%15s, pid=%6d, prev_cpu=%1d, sync=%1d, fit_cpus=0x%03x",
		__entry->comm, __entry->pid, __entry->prev_cpu, __entry->sync,  __entry->fit_cpus)
);

TRACE_EVENT(tex_update_runtime,

	TP_PROTO(int cpu, struct tex_struct *tex, char *label),

	TP_ARGS(cpu, tex, label),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		prio			)
		__field(	int,		level			)
		__field(	int,		nr_expired		)
		__field(	u64,		window_runtime		)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		memcpy(__entry->comm, tex->task->comm, TASK_COMM_LEN);
		__entry->pid			= tex->task->pid;
		__entry->prio			= tex->task->prio;
		__entry->level			= tex->level;
		__entry->window_runtime		= tex->window_runtime;
		__entry->nr_expired		= tex->nr_expired_windows;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("cpu=%d, comm=%s(%d), prio=%d, level=%d, nr_expired=%d, tex_runtime=%llu, "
		"reason=%s",
		__entry->cpu, __entry->comm, __entry->pid, __entry->prio, __entry->level,
		__entry->nr_expired, __entry->window_runtime, __entry->label)
);

DECLARE_EVENT_CLASS(tex_list_trace_template,

	TP_PROTO(int cpu, struct tex_struct *tex, int nr_tex),

	TP_ARGS(cpu, tex, nr_tex),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		prio			)
		__field(	int,		level			)
		__field(	int,		nr_expired		)
		__field(	u64,		window_runtime		)
		__field(	int,		nr_tex			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		memcpy(__entry->comm, tex->task->comm, TASK_COMM_LEN);
		__entry->pid			= tex->task->pid;
		__entry->prio			= tex->task->prio;
		__entry->level			= tex->level;
		__entry->window_runtime		= tex->window_runtime;
		__entry->nr_expired		= tex->nr_expired_windows;
		__entry->nr_tex			= nr_tex;
	),

	TP_printk("cpu=%d, nr_tex=%d, comm=%s(%d), prio=%3d, level=%1d, nr_expired=%1d, tex_runtime=%llu, ",
		__entry->cpu, __entry->nr_tex, __entry->comm, __entry->pid, __entry->prio, __entry->level,
		__entry->nr_expired, __entry->window_runtime)
);

DEFINE_EVENT(tex_list_trace_template, tex_insert_to_list,
		TP_PROTO(int cpu, struct tex_struct *tex, int nr_tex), TP_ARGS(cpu, tex, nr_tex));

DEFINE_EVENT(tex_list_trace_template, tex_remove_from_list,
		TP_PROTO(int cpu, struct tex_struct *tex, int nr_tex), TP_ARGS(cpu, tex, nr_tex));

TRACE_EVENT(tex_replace_next_task_fair,

	TP_PROTO(int cpu, struct tex_struct *tex),

	TP_ARGS(cpu, tex),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		prio			)
		__field(	int,		level			)
		__field(	int,		nr_expired		)
		__field(	u64,		window_runtime		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		memcpy(__entry->comm, tex->task->comm, TASK_COMM_LEN);
		__entry->pid			= tex->task->pid;
		__entry->prio			= tex->task->prio;
		__entry->level			= tex->level;
		__entry->window_runtime		= tex->window_runtime;
		__entry->nr_expired		= tex->nr_expired_windows;
	),

	TP_printk("cpu=%d, comm=%s(%d), prio=%d, level=%d, nr_expired=%d, tex_runtime=%llu",
		__entry->cpu, __entry->comm, __entry->pid, __entry->prio, __entry->level,
		__entry->nr_expired, __entry->window_runtime)
);

TRACE_EVENT(tex_check_preempt_wakeup,

	TP_PROTO(int cpu, struct tex_struct *curr, struct tex_struct *p, bool preempt, bool ignore),

	TP_ARGS(cpu, curr, p, preempt, ignore),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		curr_comm,	TASK_COMM_LEN	)
		__field(	pid_t,		curr_pid			)
		__field(	int,		curr_tex_lv			)
		__array(	char,		p_comm,		TASK_COMM_LEN	)
		__field(	pid_t,		p_pid				)
		__field(	int,		p_tex_lv			)
		__field(	bool,		preempt				)
		__field(	bool,		ignore				)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		memcpy(__entry->curr_comm, curr->task->comm, TASK_COMM_LEN);
		__entry->curr_pid		= curr->task->pid;
		__entry->curr_tex_lv		= curr->level;
		memcpy(__entry->p_comm, p->task->comm, TASK_COMM_LEN);
		__entry->p_pid			= p->task->pid;
		__entry->p_tex_lv		= p->level;
		__entry->preempt		= preempt;
		__entry->ignore			= ignore;
	),

	TP_printk("cpu=%d, curr(comm=%s(%d), tex_lv=%d), p(comm=%s(%d), tex_lv=%d), preempt=%d, ignore=%d",
		__entry->cpu, __entry->curr_comm, __entry->curr_pid, __entry->curr_tex_lv, __entry->p_comm,
		__entry->p_pid, __entry->p_tex_lv, __entry->preempt, __entry->ignore)
);

/*
 * Tracepoint for finding fallback cpu
 */
TRACE_EVENT(ems_find_fallback_cpu,

	TP_PROTO(int fallback_cpu, int max_spare_cap_cpu, int min_nr_running_cpu,
		int idle_cpu, unsigned long max_spare_cap, int min_nr_running),

	TP_ARGS(fallback_cpu, max_spare_cap_cpu, min_nr_running_cpu,
		idle_cpu, max_spare_cap, min_nr_running),

	TP_STRUCT__entry(
		__field(	int,		fallback_cpu		)
		__field(	int,		max_spare_cap_cpu	)
		__field(	int,		min_nr_running_cpu	)
		__field(	int,		idle_cpu		)
		__field(	unsigned long,	max_spare_cap		)
		__field(	int,		min_nr_running		)
	),

	TP_fast_assign(
		__entry->fallback_cpu		= fallback_cpu;
		__entry->max_spare_cap_cpu	= max_spare_cap_cpu;
		__entry->min_nr_running_cpu	= min_nr_running_cpu;
		__entry->idle_cpu		= idle_cpu;
		__entry->max_spare_cap		= max_spare_cap;
		__entry->min_nr_running		= min_nr_running;
	),

	TP_printk("fallback_cpu=%d, max_spare_cap_cpu=%d, min_nr_running_cpu=%d, "
		"idle_cpu=%d, max_spare_cap=%lu, min_nr_running=%d",
		__entry->fallback_cpu, __entry->max_spare_cap_cpu, __entry->min_nr_running_cpu,
		__entry->idle_cpu, __entry->max_spare_cap, __entry->min_nr_running)
);

/*
 * Tracepoint for finding best perf cpu
 */
TRACE_EVENT(ems_find_best_perf_cpu,

	TP_PROTO(struct tp_env *env, int best_cpu),

	TP_ARGS(env, best_cpu),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		task_util		)
		__field(	int,		best_cpu		)
		__field(	int,		idle			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, env->p->comm, TASK_COMM_LEN);
		__entry->pid			= env->p->pid;
		__entry->task_util		= env->task_util;
		__entry->best_cpu		= best_cpu;
		__entry->idle			= best_cpu < 0 ? -1 : env->cpu_stat[best_cpu].idle;
	),

	TP_printk("comm=%15s, pid=%6d, tsk_util=%4d, best_cpu=%2d, idle=%1d",
		__entry->comm, __entry->pid, __entry->task_util,
		__entry->best_cpu, __entry->idle)
);

TRACE_EVENT(ems_find_best_perf_cpu_info,

	TP_PROTO(int cpu, unsigned long cap, int idle, unsigned long util, unsigned long spare,
		int nr_high_prio_tasks, int nr_launch, int nr_tex),

	TP_ARGS(cpu, cap, idle, util, spare, nr_high_prio_tasks, nr_launch, nr_tex),

	TP_STRUCT__entry(
		__field(	int,		cpu		)
		__field(	unsigned long,	cap		)
		__field(	int,		idle		)
		__field(	unsigned long,	util		)
		__field(	unsigned long,	spare		)
		__field(	int,		nr_high_prio_tasks	)
		__field(	int,		nr_launch	)
		__field(	int,		nr_tex		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->cap			= cap;
		__entry->idle			= idle;
		__entry->util			= util;
		__entry->spare			= spare;
		__entry->nr_high_prio_tasks	= nr_high_prio_tasks;
		__entry->nr_launch		= nr_launch;
		__entry->nr_tex			= nr_tex;
	),

	TP_printk("cpu=%1d, cap=%4lu, idle=%1d, util=%4lu, spare=%4lu, "
		"nr_high_prio_tasks=%2d, nr_launch=%2d, nr_tex=%2d",
		__entry->cpu, __entry->cap, __entry->idle, __entry->util, __entry->spare,
		__entry->nr_high_prio_tasks, __entry->nr_launch, __entry->nr_tex)
);

/*
 * Tracepoint for finding lowest energy cpu
 */
TRACE_EVENT(ems_find_energy_cpu,

	TP_PROTO(struct task_struct *p, struct cpumask *candidates, struct cpumask *overcap_cpus,
		int energy_cpu, int adv_energy_cpu),

	TP_ARGS(p, candidates, overcap_cpus, energy_cpu, adv_energy_cpu),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned int,	candidates		)
		__field(	unsigned int,	overcap_cpus		)
		__field(	int,		energy_cpu		)
		__field(	int,		adv_energy_cpu		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->candidates		= *(unsigned int *)cpumask_bits(candidates);
		__entry->overcap_cpus		= *(unsigned int *)cpumask_bits(overcap_cpus);
		__entry->energy_cpu		= energy_cpu;
		__entry->adv_energy_cpu		= adv_energy_cpu;
	),

	TP_printk("comm=%15s, pid=%6d, candidates=0x%03x, overcap_cpus=ox%03x"
		"energy_cpu=%2d, adv_energy_cpu=%2d",
		__entry->comm, __entry->pid, __entry->candidates, __entry->overcap_cpus,
		__entry->energy_cpu, __entry->adv_energy_cpu)
);

/*
 * Tracepoint for computing energy
 */
TRACE_EVENT(ems_compute_energy,

	TP_PROTO(int cpu, unsigned long util, unsigned long cap,
		unsigned long f, unsigned long v, int temp, unsigned long dp,
		unsigned long sp, unsigned long energy),

	TP_ARGS(cpu, util, cap, f, v, temp, dp, sp, energy),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	util			)
		__field(	unsigned long,	cap			)
		__field(	unsigned long,	f			)
		__field(	unsigned long,	v			)
		__field(	int,		temp			)
		__field(	unsigned long,	dp			)
		__field(	unsigned long,	sp			)
		__field(	unsigned long,	energy			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->util			= util;
		__entry->cap			= cap;
		__entry->f			= f;
		__entry->v			= v;
		__entry->temp			= temp;
		__entry->dp			= dp;
		__entry->sp			= sp;
		__entry->energy			= energy;
	),

	TP_printk("cpu=%1d, util=%4lu, cap=%4lu, f=%7lu, v=%4lu, temp=%4d, "
		"dp=%4lu, sp=%4lu, energy=%7lu",
		__entry->cpu, __entry->util, __entry->cap, __entry->f, __entry->v,
		__entry->temp, __entry->dp,  __entry->sp, __entry->energy)
);

/*
 * Tracepoint for computing DSU energy
 */
TRACE_EVENT(ems_compute_dsu_energy,

	TP_PROTO(unsigned long freq, unsigned long volt,
		unsigned long dp, unsigned long energy),

	TP_ARGS(freq, volt, dp, energy),

	TP_STRUCT__entry(
		__field(	unsigned long,	freq			)
		__field(	unsigned long,	volt			)
		__field(	unsigned long,	dp			)
		__field(	unsigned long,	energy			)
	),

	TP_fast_assign(
		__entry->freq			= freq;
		__entry->volt			= volt;
		__entry->dp			= dp;
		__entry->energy			= energy;
	),

	TP_printk("freq=%7lu, volt=%4lu, dp=%4lu, energy=%7lu",
		__entry->freq, __entry->volt, __entry->dp, __entry->energy)
);

/*
 * Tracepoint for computing system energy
 */
TRACE_EVENT(ems_compute_system_energy,

	TP_PROTO(struct task_struct *p, const struct cpumask *candidates,
			int target_cpu, unsigned long energy),

	TP_ARGS(p, candidates, target_cpu, energy),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned int,	candidates		)
		__field(	int,		target_cpu		)
		__field(	unsigned long,	energy			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->candidates		= *(unsigned int *)cpumask_bits(candidates);
		__entry->target_cpu		= target_cpu;
		__entry->energy			= energy;
	),

	TP_printk("comm=%15s, pid=%6d, candidates=0x%03x, target_cpu=%1d, energy=%7lu",
		__entry->comm, __entry->pid, __entry->candidates,
		__entry->target_cpu, __entry->energy)
);

/*
 * Tracepoint for task fits to capacity
 */
TRACE_EVENT(ems_task_fits_capacity,

	TP_PROTO(struct task_struct *p, int group_idx, int cpu, unsigned long cap,
			int ratio, unsigned long util, bool fit),

	TP_ARGS(p, group_idx, cpu, cap, ratio, util, fit),

	TP_STRUCT__entry(
		__array(        char,           comm,   TASK_COMM_LEN   )
		__field(        pid_t,          pid                     )
		__field(        int,            group_idx               )
		__field(        int,            cpu                     )
		__field(        unsigned long,  cap                     )
		__field(        int,            ratio                   )
		__field(        unsigned long,  util                    )
		__field(        bool,           fit                     )
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid                    = p->pid;
		__entry->group_idx              = group_idx;
		__entry->cpu                    = cpu;
		__entry->cap                    = cap;
		__entry->ratio                  = ratio;
		__entry->util                   = util;
		__entry->fit                    = fit;
	),

	TP_printk("comm=%15s, pid=%6d, group_idx=%1d, cpu=%1d, cap=%4lu, "
		"ratio=%3d, util=%4lu, fit=%1d",
		__entry->comm, __entry->pid, __entry->group_idx, __entry->cpu, __entry->cap,
		__entry->ratio, __entry->util, __entry->fit)
);

/*
 * Tracepoint for cpu overutilized
 */
TRACE_EVENT(ems_cpu_overutilized,

	TP_PROTO(int cpu, unsigned long cap, int min_ratio, unsigned long util, bool overutil),

	TP_ARGS(cpu, cap, min_ratio, util, overutil),

	TP_STRUCT__entry(
		__field( int,            cpu                     )
		__field( unsigned long,  cap                     )
		__field( int,            min_ratio               )
		__field( unsigned long,  util                    )
		__field( bool,           overutil                )
	),

	TP_fast_assign(
		__entry->cpu                     = cpu;
		__entry->cap                     = cap;
		__entry->min_ratio               = min_ratio;
		__entry->util                    = util;
		__entry->overutil                = overutil;
	),

	TP_printk("cpu=%1d, cap=%4lu, min_ratio=%3d, util=%4lu overutil=%1d",
		__entry->cpu, __entry->cap, __entry->min_ratio, __entry->util,  __entry->overutil)
);

/*
 * Tracepoint for core selection by sysbusy schedule
 */
TRACE_EVENT(sysbusy_need_active_balance,

	TP_PROTO(int src_cpu, int dst_cpu, const char *label),

	TP_ARGS(src_cpu, dst_cpu, label),

	TP_STRUCT__entry(
		__field(	int,		src_cpu		)
		__field(	int,		dst_cpu		)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->src_cpu		= src_cpu;
		__entry->dst_cpu		= dst_cpu;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("src_cpu=%d dst_cpu=%d label=%s",
		__entry->src_cpu, __entry->dst_cpu, __entry->label)
);

TRACE_EVENT(sysbusy_schedule,

	TP_PROTO(struct task_struct *p, int state, int target_cpu),

	TP_ARGS(p, state, target_cpu),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	util			)
		__field(	int,		state			)
		__field(	int,		target_cpu		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->util			= p->se.avg.util_avg;
		__entry->state			= state;
		__entry->target_cpu		= target_cpu;
	),

	TP_printk("comm=%15s, pid=%6d, util=%4ld, state=%1d, target_cpu=%1d",
		__entry->comm, __entry->pid, __entry->util,
		__entry->state, __entry->target_cpu)
);

/*
 * Tracepoint for monitoring sysbusy state
 */
TRACE_EVENT(ems_cpu_profile,

	TP_PROTO(int cpu, struct cpu_profile *cs, int nr_running, u64 rba, u64 dslt_rba,
		int avg_nr_heavy),

	TP_ARGS(cpu, cs, nr_running, rba, dslt_rba, avg_nr_heavy),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	u64,		rba			)
		__field(	u64,		dslt_rba		)
		__field(	u64,		cpu_util		)
		__field(	int,		cpu_util_busy		)
		__field(	u64,		hratio			)
		__field(	int,		pid			)
		__field(	int,		nr_running		)
		__field(	int,		avg_nr_heavy		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->rba			= rba;
		__entry->dslt_rba		= dslt_rba;
		__entry->cpu_util		= cs[CPU_UTIL].value;
		__entry->cpu_util_busy		= cs[CPU_UTIL].data;
		__entry->hratio			= cs[CPU_HTSK].value;
		__entry->pid			= cs[CPU_HTSK].data;
		__entry->nr_running		= nr_running;
		__entry->avg_nr_heavy		= avg_nr_heavy;
	),

	TP_printk("cpu=%1d, rba=%4llu, dslt_rba=%4lld, cpu_util=%4llu(busy=%1d), "
		"hratio=%4llu(pid=%6d), nr_running=%2d, avg_nr_heavy=%2d",
		__entry->cpu, __entry->rba, __entry->dslt_rba,
		__entry->cpu_util, __entry->cpu_util_busy,
		__entry->hratio, __entry->pid, __entry->nr_running, __entry->avg_nr_heavy)
);

TRACE_EVENT(profile_htask,

	TP_PROTO(int cpu, struct task_struct *p, const char *label),

	TP_ARGS(cpu, p, label),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("cpu=%1d, comm=%15s(%d), label=%s",
		__entry->cpu, __entry->comm, __entry->pid, __entry->label)
);

/*
 * Tracepoint for tracking tasks in system
 */
TRACE_EVENT(ems_profile_tasks,

	TP_PROTO(int busy_cpu_count, unsigned long cpu_util_sum,
		int heavy_task_count, unsigned long heavy_task_util_sum,
		int avg_nr_run_sum, int avg_nr_heavy_sum, int avg_nr_sustain_high_sum,
		int heavy_util, int heavy_pid),

	TP_ARGS(busy_cpu_count, cpu_util_sum, heavy_task_count, heavy_task_util_sum,
		avg_nr_run_sum, avg_nr_heavy_sum, avg_nr_sustain_high_sum,
		heavy_util, heavy_pid),

	TP_STRUCT__entry(
		__field(	int,		busy_cpu_count		)
		__field(	unsigned long,	cpu_util_sum		)
		__field(	int,		heavy_task_count	)
		__field(	unsigned long,	heavy_task_util_sum	)
		__field(	int,		avg_nr_run_sum		)
		__field(	int,		avg_nr_heavy_sum	)
		__field(	int,		avg_nr_sustain_high_sum	)
		__field(	int,		heavy_pid)
		__field(	int,		heavy_util)
	),

	TP_fast_assign(
		__entry->busy_cpu_count		= busy_cpu_count;
		__entry->cpu_util_sum		= cpu_util_sum;
		__entry->heavy_task_count	= heavy_task_count;
		__entry->heavy_task_util_sum	= heavy_task_util_sum;
		__entry->avg_nr_run_sum		= avg_nr_run_sum;
		__entry->avg_nr_heavy_sum	= avg_nr_heavy_sum;
		__entry->avg_nr_sustain_high_sum = avg_nr_sustain_high_sum;
		__entry->heavy_pid		= heavy_pid;
		__entry->heavy_util		= heavy_util;
	),

	TP_printk("busy_cpu_count=%1d, cpu_util_sum=%4lu, "
		"heavy_task_count=%1d, heavy_task_util_sum=%4lu, "
		"avg_nr_run_sum=%d, avg_nr_heavy_sum=%d, avg_nr_sustain_high_sum=%d "
		"heavy_util=%d heavy_pid=%d",
		__entry->busy_cpu_count, __entry->cpu_util_sum,
		__entry->heavy_task_count, __entry->heavy_task_util_sum,
		__entry->avg_nr_run_sum, __entry->avg_nr_heavy_sum, __entry->avg_nr_sustain_high_sum,
		__entry->heavy_util, __entry->heavy_pid)
);

/*
 * Tracepoint for somac scheduling
 */
TRACE_EVENT(sysbusy_somac,

	TP_PROTO(struct task_struct *p, unsigned long task_util,
		int src_cpu, int dst_cpu, unsigned long time),

	TP_ARGS(p, task_util, src_cpu, dst_cpu, time),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	task_util		)
		__field(	int,		src_cpu			)
		__field(	int,		dst_cpu			)
		__field(	unsigned long,	time			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->task_util		= task_util;
		__entry->src_cpu		= src_cpu;
		__entry->dst_cpu		= dst_cpu;
		__entry->time			= time;
	),

	TP_printk("comm=%15s, pid=%6d, task_util=%4lu, src_cpu=%1d, dst_cpu=%1d, time=%10lu",
		__entry->comm, __entry->pid, __entry->task_util,
		__entry->src_cpu, __entry->dst_cpu, __entry->time)
);

/*
 * Tracepoint for somac label
 */
TRACE_EVENT(sysbusy_somac_reason,

	TP_PROTO(int src_cpu, int dst_cpu, char *label),

	TP_ARGS(src_cpu, dst_cpu, label),

	TP_STRUCT__entry(
		__field(	int,		src_cpu			)
		__field(	int,		dst_cpu			)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->src_cpu		= src_cpu;
		__entry->dst_cpu		= dst_cpu;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("src_cpu=%1d, dst_cpu=%1d, reason=%14s",
		__entry->src_cpu, __entry->dst_cpu, __entry->label)
);

/*
 * Tracepoint for determine_sysbusy_state
 */
TRACE_EVENT(sysbusy_determine_state,

	TP_PROTO(int old_state, int next_state, int busy_cpu_cnt,
		unsigned long heavy_task_util_sum, unsigned long cpu_util_sum,
		int avg_nr_run_sum, int heavy_task_cnt, int avg_nr_heavy, int new_task_count),

	TP_ARGS(old_state, next_state, busy_cpu_cnt, heavy_task_util_sum,
		cpu_util_sum, avg_nr_run_sum, heavy_task_cnt, avg_nr_heavy, new_task_count),

	TP_STRUCT__entry(
		__field(	int,		old_state		)
		__field(	int,		next_state		)
		__field(	int,		busy_cpu_cnt		)
		__field(	unsigned long,	heavy_task_util_sum	)
		__field(	unsigned long,	cpu_util_sum		)
		__field(	int,		avg_nr_run_sum		)
		__field(	int,		heavy_task_cnt		)
		__field(	int,		avg_nr_heavy		)
		__field(	int,		new_task_count		)
	),

	TP_fast_assign(
		__entry->old_state		= old_state;
		__entry->next_state		= next_state;
		__entry->busy_cpu_cnt		= busy_cpu_cnt;
		__entry->heavy_task_util_sum	= heavy_task_util_sum;
		__entry->cpu_util_sum		= cpu_util_sum;
		__entry->avg_nr_run_sum		= avg_nr_run_sum;
		__entry->heavy_task_cnt		= heavy_task_cnt;
		__entry->avg_nr_heavy		= avg_nr_heavy;
		__entry->new_task_count		= new_task_count;
	),

	TP_printk("old_state=%d, next_state=%d busy_cpu_cnt=%d heavy_task_util_sum=%lu "
		"cpu_util_sum=%lu avg_nr_run_sum=%d heavy_task_cnt=%d "
		"avg_nr_heavy=%d new_task_count=%d",
		__entry->old_state, __entry->next_state, __entry->busy_cpu_cnt,
		__entry->heavy_task_util_sum, __entry->cpu_util_sum, __entry->avg_nr_run_sum,
		__entry->heavy_task_cnt, __entry->avg_nr_heavy, __entry->new_task_count)
);

TRACE_EVENT(sysbusy_busy_waiting,

	TP_PROTO(int cpu, struct task_struct *p, u64 count),

	TP_ARGS(cpu, p, count),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	u64,		count			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->count			= count;
	),

	TP_printk("comm=%s(%d) is busy waiting. cpu=%d, yield_count=%llu",
		__entry->comm, __entry->pid, __entry->cpu, __entry->count)
);

/*
 * Tracepoint for EGO
 */
TRACE_EVENT(ego_sched_util,

	TP_PROTO(int cpu, unsigned long util, unsigned long cfs, unsigned long rt,
		unsigned long dl, unsigned long bwdl, unsigned long irq,
		unsigned long uclamp_min, unsigned long uclamp_max),

	TP_ARGS(cpu, util, cfs, rt, dl, bwdl, irq, uclamp_min, uclamp_max),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	util			)
		__field(	unsigned long,	cfs			)
		__field(	unsigned long,	rt			)
		__field(	unsigned long,	dl			)
		__field(	unsigned long,	bwdl			)
		__field(	unsigned long,	irq			)
		__field(	unsigned long,	uclamp_min		)
		__field(	unsigned long,	uclamp_max		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->util			= util;
		__entry->cfs			= cfs;
		__entry->rt			= rt;
		__entry->dl			= dl;
		__entry->bwdl			= bwdl;
		__entry->irq			= irq;
		__entry->uclamp_min		= uclamp_min;
		__entry->uclamp_max		= uclamp_max;
	),

	TP_printk("cpu=%1d, util=%4ld, cfs=%4ld, rt=%4ld, dl=%4ld, dlbw=%4ld, irq=%4ld, "
		"uclamp_min=%4ld, uclamp_max=%4ld",
		__entry->cpu, __entry->util, __entry->cfs, __entry->rt,
		__entry->dl, __entry->bwdl, __entry->irq, __entry->uclamp_min,
		__entry->uclamp_max)
);

TRACE_EVENT(ego_cpu_util,

	TP_PROTO(int cpu, unsigned long pelt_boost, unsigned long util,
		unsigned long io_util, unsigned long bst_util),

	TP_ARGS(cpu, pelt_boost, util, io_util, bst_util),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	pelt_boost		)
		__field(	unsigned long,	util			)
		__field(	unsigned long,	io_util			)
		__field(	unsigned long,	bst_util		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->pelt_boost		= pelt_boost;
		__entry->util			= util;
		__entry->io_util		= io_util;
		__entry->bst_util		= bst_util;
	),

	TP_printk("cpu=%1d, pelt_boost=%3ld, util=%4ld, io_util=%4ld, bst_util=%4ld",
		__entry->cpu, __entry->pelt_boost, __entry->util, __entry->io_util,
		__entry->bst_util)
);

TRACE_EVENT(ego_get_heavy_task_util,

	TP_PROTO(int cpu, unsigned long bst_util, unsigned long min_load, unsigned long load),

	TP_ARGS(cpu, bst_util, min_load, load),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	bst_util		)
		__field(	unsigned long,	min_load		)
		__field(	unsigned long,	load			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->bst_util		= bst_util;
		__entry->min_load		= min_load;
		__entry->load			= load;
	),

	TP_printk("cpu=%1d, bst_util=%4lu, min_load=%4lu, load=%4lu",
		__entry->cpu, __entry->bst_util, __entry->min_load, __entry->load)
);

TRACE_EVENT(ego_freq_update_postponed,

	TP_PROTO(int cpu, int next_f, int orig_f,
		int target_latency_ns, int pelt_margin, bool postponed),

	TP_ARGS(cpu, next_f, orig_f, target_latency_ns, pelt_margin, postponed),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		next_f			)
		__field(	int,		orig_f			)
		__field(	int,		target_latency_ns	)
		__field(	int,		pelt_margin		)
		__field(	bool,		postponed		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->next_f			= next_f;
		__entry->orig_f			= orig_f;
		__entry->target_latency_ns	= target_latency_ns;
		__entry->pelt_margin		= pelt_margin;
		__entry->postponed		= postponed;
	),

	TP_printk("cpu=%1d, next_f=%d, orig_f=%d, target_latency_ns=%d, "
		"pelt_margin=%d, postponed=%1d",
		__entry->cpu, __entry->next_f, __entry->orig_f,
		__entry->target_latency_ns, __entry->pelt_margin, __entry->postponed)
);

TRACE_EVENT(ego_resolve_freq_reversal,

	TP_PROTO(int cpu, unsigned long orig_freq,
		unsigned long resolved_freq, int cur_step, int pelt_margin),

	TP_ARGS(cpu, orig_freq, resolved_freq, cur_step, pelt_margin),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	orig_freq		)
		__field(	unsigned long,	resolved_freq		)
		__field(	int,		cur_step		)
		__field(	int,		pelt_margin		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->orig_freq		= orig_freq;
		__entry->resolved_freq		= resolved_freq;
		__entry->cur_step		= cur_step;
		__entry->pelt_margin		= pelt_margin;
	),

	TP_printk("cpu=%1d, orig_freq=%lu, resolved_freq=%lu, cur_step=%1d, pelt_margin=%d",
		__entry->cpu, __entry->orig_freq, __entry->resolved_freq,
		__entry->cur_step, __entry->pelt_margin)
);

TRACE_EVENT(ego_apply_util_target_margin,

	TP_PROTO(int cpu, unsigned long org_util, unsigned long util_target,
		unsigned long util_gap, unsigned long extra_pct, int org_margin, unsigned long margin),

	TP_ARGS(cpu, org_util, util_target, util_gap, extra_pct, org_margin, margin),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,		org_util			)
		__field(	unsigned long,		util_target			)
		__field(	unsigned long,		util_gap			)
		__field(	unsigned long,		extra_pct			)
		__field(	unsigned long,		org_margin			)
		__field(	unsigned long,		margin			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->org_util		= org_util;
		__entry->util_target		= util_target;
		__entry->util_gap		= util_gap;
		__entry->extra_pct		= extra_pct;
		__entry->org_margin		= org_margin;
		__entry->margin			= margin;
	),

	TP_printk("cpu=%1d, org_util=%lu, util_target=%lu, util_gap=%lu, extra_pct=%lu, "
		"org_margin=%lu, margin=%lu",
		__entry->cpu, __entry->org_util, __entry->util_target, __entry->util_gap,
		__entry->extra_pct, __entry->org_margin, __entry->margin)
);

/*
 * Tracepoint for slack timer
 */
TRACE_EVENT(slack_timer,

	TP_PROTO(int cpu),

	TP_ARGS(cpu),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
	),

	TP_printk("cpu=%d slack timer expired", __entry->cpu)
);

/*
 * Tracepoint for checking need for slack timer
 */
TRACE_EVENT(ego_need_slack_timer,

	TP_PROTO(int cpu, unsigned long boosted_util, unsigned long min_cap,
		int heaviest_cpu, unsigned int cur_freq, unsigned int min_freq,
		unsigned int energy_freq, unsigned int orig_freq, int need),

	TP_ARGS(cpu, boosted_util, min_cap, heaviest_cpu, cur_freq,
		min_freq, energy_freq, orig_freq, need),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	boosted_util		)
		__field(	unsigned long,	min_cap			)
		__field(	int,		heaviest_cpu		)
		__field(	unsigned int,	cur_freq		)
		__field(	unsigned int,	min_freq		)
		__field(	unsigned int,	energy_freq		)
		__field(	unsigned int,	orig_freq		)
		__field(	int,		need			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->boosted_util		= boosted_util;
		__entry->min_cap		= min_cap;
		__entry->heaviest_cpu		= heaviest_cpu;
		__entry->cur_freq		= cur_freq;
		__entry->min_freq		= min_freq;
		__entry->energy_freq		= energy_freq;
		__entry->orig_freq		= orig_freq;
		__entry->need			= need;
	),

	TP_printk("cpu=%1d, boosted_util=%4lu, min_cap=%4lu, heaviest_cpu=%1d, "
		"cur_freq=%7u, min_freq=%7u, energy_freq=%7u, orig_freq=%7u, need=%1d",
		__entry->cpu, __entry->boosted_util, __entry->min_cap,
		__entry->heaviest_cpu, __entry->cur_freq, __entry->min_freq,
		__entry->energy_freq, __entry->orig_freq, __entry->need)
);

/*
 * Tracepoint for update cpufreq pressure
 */
TRACE_EVENT(ems_update_cpu_capacity,

	TP_PROTO(int cpu, unsigned long pressure, unsigned int reason),

	TP_ARGS(cpu, pressure, reason),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	cap			)
		__field(	unsigned long,	orig			)
		__field(	unsigned long,	pressure		)
		__field(	unsigned int,	reason			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->cap			= ems_rq_cpu_capacity(cpu_rq(cpu));
		__entry->orig			= ems_rq_cpu_capacity_orig(cpu_rq(cpu));
		__entry->pressure		= pressure;
		__entry->reason			= reason;
	),

	TP_printk("cpu=%d, cap=%lu, orig=%lu, pressure=%lu, reason=%u",
		__entry->cpu, __entry->cap, __entry->orig, __entry->pressure, __entry->reason)
);

/*
 * Tracepoint for available cpus in FRT
 */

TRACE_EVENT(ems_frt_fast_track,

	TP_PROTO(struct task_struct *p, int cpu, unsigned long tot_util, int overutilized, int nr_running,
		u64 avg_idle, unsigned long scale_freq, int scale_freq_thr),

	TP_ARGS(p, cpu, tot_util, overutilized, nr_running, avg_idle, scale_freq, scale_freq_thr),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cpu			)
		__field(	unsigned long,	tot_util		)
		__field(	unsigned int,	cpus_ptr		)
		__field(	int,		overutilized		)
		__field(	int,		nr_running		)
		__field(	u64,		avg_idle		)
		__field(	unsigned long,	scale_freq		)
		__field(	int,		scale_freq_thr		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p ? p->comm : "no task", TASK_COMM_LEN);
		__entry->pid			= p ? p->pid : -1;
		__entry->cpu			= cpu;
		__entry->tot_util		= tot_util;
		__entry->cpus_ptr		= *(unsigned int *)cpumask_bits(p->cpus_ptr);
		__entry->overutilized		= overutilized;
		__entry->nr_running		= nr_running;
		__entry->avg_idle		= avg_idle;
		__entry->scale_freq		= scale_freq;
		__entry->scale_freq_thr		= scale_freq_thr;
	),

	TP_printk("comm=%15s, pid=%6d, cpu=%1d, cpus_ptr=0x%3x, tot_util=%4lu, overutilized=%1d, nr_running=%d, "
		"avg_idle=%llu, scale_freq=%lu, scale_freq_thr=%d",
		  __entry->comm, __entry->pid, __entry->cpu, __entry->cpus_ptr,
		  __entry->tot_util, __entry->overutilized, __entry->nr_running,
		  __entry->avg_idle, __entry->scale_freq, __entry->scale_freq_thr)
);

TRACE_EVENT(ems_frt_lowest_rq_stat,

	TP_PROTO(struct task_struct *p, int cpu, unsigned long spare,
		unsigned long irq_load, int curr_tex, int idle, unsigned int exit_lat),

	TP_ARGS(p, cpu, spare, irq_load, curr_tex, idle, exit_lat),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cpu			)
		__field(	unsigned long,	spare			)
		__field(	unsigned long,	irq_load		)
		__field(	int,		curr_tex		)
		__field(	unsigned long,	cpu_util		)
		__field(	unsigned int,	exit_lat		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p ? p->comm : "no task", TASK_COMM_LEN);
		__entry->pid			= p ? p->pid : -1;
		__entry->cpu			= cpu;
		__entry->spare			= spare;
		__entry->irq_load		= irq_load;
		__entry->curr_tex		= curr_tex;
		__entry->cpu_util		= idle ? 0 : ml_cpu_util(cpu);
		__entry->exit_lat		= exit_lat;
	),

	TP_printk("comm=%15s, pid=%6d, cpu=%1d, spare=%4lu, irq_load=%4lu, "
		  "curr_tex=%1d, cpu_util=%4lu, exit_lat=%4d",
		  __entry->comm, __entry->pid, __entry->cpu, __entry->spare,
		  __entry->irq_load, __entry->curr_tex, __entry->cpu_util,
		  __entry->exit_lat)
);

TRACE_EVENT(ems_frt_lowest_rq,

	TP_PROTO(struct task_struct *p, unsigned long task_util, int index,
		struct cpumask *lowest_mask, const struct cpumask *ecs_mask, struct cpumask *mask,
		int best_cpu, int max_spare_cpu, unsigned int best_lat,
		unsigned long best_cpu_util, unsigned long max_spare_cap, int fast_track),

	TP_ARGS(p, task_util, index, lowest_mask, ecs_mask, mask, best_cpu, max_spare_cpu,
		best_lat, best_cpu_util, max_spare_cap, fast_track),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	task_util		)
		__field(	int,		index			)
		__field(	unsigned int,	lowest_mask		)
		__field(	unsigned int,	ecs_mask		)
		__field(	unsigned int,	mask			)
		__field(	int,		best_cpu		)
		__field(	int,		max_spare_cpu		)
		__field(	unsigned int,	best_lat		)
		__field(	unsigned long,	best_cpu_util		)
		__field(	unsigned long,	max_spare_cap		)
		__field(	int,		fast_track		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p ? p->comm : "no task", TASK_COMM_LEN);
		__entry->pid			= p ? p->pid : -1;
		__entry->task_util		= task_util;
		__entry->index			= index;
		__entry->lowest_mask		= *(unsigned int *)cpumask_bits(lowest_mask);
		__entry->ecs_mask		= *(unsigned int *)cpumask_bits(ecs_mask);
		__entry->mask			= *(unsigned int *)cpumask_bits(mask);
		__entry->best_cpu		= best_cpu;
		__entry->max_spare_cpu		= max_spare_cpu;
		__entry->best_lat		= best_lat;
		__entry->best_cpu_util		= best_cpu_util;
		__entry->max_spare_cap		= max_spare_cap;
		__entry->fast_track		= fast_track;
	),

	TP_printk("comm=%15s, pid=%6d, task_util=%4lu, init_index=%1d, lowest_mask=0x%3x, ecs_mask=0x%03x "
		  "mask=0x%03x, best_cpu=%1d, max_spare_cpu=%1d best_lat=%4d, best_cpu_util=%4lu "
		  "max_spare_cap=%4lu, fast_track=%1d",
		  __entry->comm, __entry->pid, __entry->task_util, __entry->index, __entry->lowest_mask, __entry->ecs_mask,
		  __entry->mask, __entry->best_cpu, __entry->max_spare_cpu, __entry->best_lat, __entry->best_cpu_util,
		  __entry->max_spare_cap, __entry->fast_track)
);

TRACE_EVENT(mlt_update_load_recent,

	TP_PROTO(int cpu, u64 scaled_delta, u64 delta, u64 scaled_recent, u64 recent, int type),

	TP_ARGS(cpu, scaled_delta, delta, scaled_recent, recent, type),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	u64,		scaled_delta		)
		__field(	u64,		delta			)
		__field(	u64,		scaled_recent		)
		__field(	u64,		recent			)
		__field(	int,		type			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->scaled_delta		= scaled_delta;
		__entry->delta			= delta;
		__entry->scaled_recent		= scaled_recent;
		__entry->recent			= recent;
		__entry->type			= type;
	),

	TP_printk("cpu=%1d, delta=%7llu(%7llu), recent=%7llu(%7llu), type=%1d",
		__entry->cpu, __entry->scaled_delta, __entry->delta, __entry->scaled_recent,
		__entry->recent, __entry->type)
);

TRACE_EVENT(mlt_update_load,

	TP_PROTO(int cpu, struct task_struct *p, u64 delta, int running, int type, const char *label),

	TP_ARGS(cpu, p, delta, running, type, label),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	u64,		delta			)
		__field(	int,		running			)
		__field(	int,		type			)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->delta			= delta;
		__entry->running		= running;
		__entry->type			= type;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("comm=%15s(%d), cpu=%1d, delta=%7llu, running=%1d, type=%1d, label=%s",
		__entry->comm, __entry->pid, __entry->cpu, __entry->delta, __entry->running,
		__entry->type, __entry->label)
);

TRACE_EVENT(mlt_update_task_load,

	TP_PROTO(int cpu, struct task_struct *p, struct mlt_part *part, const char *label),

	TP_ARGS(cpu, p, part, label),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		hist_idx		)
		__field(	u64,		recent			)
		__array(	int,		load,	MLT_LOAD_PERIOD_COUNT	)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		int i;
		for (i = 0; i < MLT_LOAD_PERIOD_COUNT; i++)
			__entry->load[i] = part->load_history[i];
		__entry->cpu			= cpu;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->hist_idx		= part->hist_idx;
		__entry->recent			= div64_u64(part->load_recent, MLT_LOAD_DIVISOR);
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("comm=%15s(%d), cpu=%1d, recent=%4llu, idx=%1d, load=[%4d] label=%s",
		__entry->comm, __entry->pid, __entry->cpu, __entry->recent, __entry->hist_idx,
		__entry->load[0], __entry->label)
);

/*
 * Tracepoint for multi load tracking for cstate
 */
TRACE_EVENT(mlt_update_cpu_part,

	TP_PROTO(int cpu, struct mlt_part *part),

	TP_ARGS(cpu, part),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		cur_period		)
		__field(	int,		state			)
		__field(	u64,		contrib			)
		__field(	int,		recent			)
		__field(	int,		last			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->cur_period		= part->cur_period;
		__entry->state			= part->state;
		__entry->contrib		= part->contrib;
		__entry->recent			= part->recent;
		__entry->last			= part->periods[part->cur_period];
	),

	TP_printk("cpu=%1d cur_period=%1d state=%2d contrib=%8llu recent=%4d last=%4d",
		__entry->cpu, __entry->cur_period, __entry->state,
		__entry->contrib, __entry->recent, __entry->last)
);

TRACE_EVENT(mlt_update_cpu_uarch,

	TP_PROTO(int cpu, struct mlt_part *part, struct mlt_uarch *uarch),

	TP_ARGS(cpu, part, uarch),

	TP_STRUCT__entry(
		__field(	int,	cpu				)
		__array(	u32,	recent,	MLT_UARCH_CPU_NUM	)
		__array(	u32,	last,	MLT_UARCH_CPU_NUM	)
	),

	TP_fast_assign(
		int i;
		__entry->cpu			= cpu;
		for (i = 0; i < MLT_UARCH_CPU_NUM; i++) {
			__entry->last[i]	= uarch[i].periods[part->cur_period];
			__entry->recent[i]	= uarch[i].recent;
		}
	),

	TP_printk("cpu=%1d "
		"ipc_lst=%6u ipc_rct=%6u",
		__entry->cpu,
		__entry->last[0], __entry->recent[0])
);

TRACE_EVENT(mlt_update_tsk_uarch,

	TP_PROTO(struct task_struct *p, struct mlt_part *part, struct mlt_uarch *uarch),

	TP_ARGS(p, part, uarch),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN		)
		__field(	pid_t,		pid			)
		__field(	int,		cpu				)
		__array(	u32,		recent,	MLT_UARCH_TSK_NUM	)
		__array(	u32,		last,	MLT_UARCH_TSK_NUM	)
	),

	TP_fast_assign(
		int i;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->cpu			= task_cpu(p);
		for (i = 0; i < MLT_UARCH_TSK_NUM; i++) {
			__entry->last[i]	= uarch[i].periods[part->active_period];
			__entry->recent[i]	= uarch[i].recent;
		}
	),

	TP_printk("comm=%15s pid=%6d cpu=%1d "
		"ipc_lst=%6u ipc_rct=%6u",
		__entry->comm, __entry->pid, __entry->cpu,
		__entry->last[0], __entry->recent[0]
		)
);

TRACE_EVENT(mlt_update_tsk_uarch_periods,

	TP_PROTO(struct task_struct *p, struct mlt_part *part, struct mlt_uarch *uarch, int idx),

	TP_ARGS(p, part, uarch, idx),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN		)
		__field(	pid_t,		pid				)
		__field(	int,		cpu				)
		__field(	int,		prd				)
		__array(	u32,		periods,	MLT_PERIOD_COUNT)
		__field(	u32,		recent				)
	),

	TP_fast_assign(
		int i;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->cpu			= task_cpu(p);
		__entry->prd			= part->active_period;
		__entry->recent			= uarch[idx].recent;
		for (i = 0; i < MLT_PERIOD_COUNT; i++) {
		//	c = ((part->active_period + i + MLT_PERIOD_COUNT) % MLT_PERIOD_COUNT);
			__entry->periods[i]	= uarch[idx].periods[i];
		}
	),

	TP_printk("comm=%15s pid=%6d cpu=%1d prd=%2d type=ipc"
		"p=%6u,%6u,%6u,%6u,%6u,%6u,%6u,%6u rct=%6u",
		__entry->comm, __entry->pid, __entry->cpu, __entry->prd,
		__entry->periods[7], __entry->periods[6], __entry->periods[5], __entry->periods[4],
		__entry->periods[3], __entry->periods[2], __entry->periods[1], __entry->periods[0],
		__entry->recent
		)
);

TRACE_EVENT(mlt_runnable_ctrb_update,

	TP_PROTO(int cpu, struct mlt_runnable *mlt),

	TP_ARGS(cpu, mlt),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		nr_run			)
		__field(	int,		nr_topapp		)
		__field(	int,		nr_misfit		)
		__field(	u64,		contrib			)
		__field(	u64,		runnable_contrib	)
		__field(	u64,		topapp_runnable_contrib	)
		__field(	u64,		misfit_runnable_contrib	)
	),

	TP_fast_assign(
		__entry->cpu				= cpu;
		__entry->nr_run				= mlt->nr_run;
		__entry->nr_topapp			= mlt->nr_topapp;
		__entry->nr_misfit			= ems_rq_nr_misfited(cpu_rq(cpu));
		__entry->contrib			= mlt->contrib;
		__entry->runnable_contrib		= mlt->runnable_contrib;
		__entry->topapp_runnable_contrib	= mlt->topapp_runnable_contrib;
		__entry->misfit_runnable_contrib	= mlt->misfit_runnable_contrib;
	),

	TP_printk("cpu=%2d, nr_run=%2d, nr_topapp=%2d, nr_misift=%2d, "
		"contrib=%11llu, runnable_contrib=%11llud, "
		"topapp_runnable_contrib=%11llud, misfit_runnable_contrib=%11llud",
		__entry->cpu, __entry->nr_run, __entry->nr_topapp, __entry->nr_misfit,
		__entry->contrib, __entry->runnable_contrib, __entry->topapp_runnable_contrib,
		__entry->misfit_runnable_contrib)
);

TRACE_EVENT(mlt_runnable_update,

	TP_PROTO(int cpu, struct mlt_runnable *mlt),

	TP_ARGS(cpu, mlt),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		runnable		)
		__field(	int,		topapp_runnable		)
		__field(	int,		misfit_runnable		)
		__field(	int,		sustain_high_runnable	)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->runnable		= mlt->runnable;
		__entry->topapp_runnable	= mlt->topapp_runnable;
		__entry->misfit_runnable	= mlt->misfit_runnable;
		__entry->sustain_high_runnable	= mlt->sustain_high_runnable;
	),

	TP_printk("cpu=%2d, runnable=%4d, topapp_runnable=%4d, misfit_runnable=%4d, sustain_high_runnable=%4d",
		__entry->cpu, __entry->runnable, __entry->topapp_runnable, __entry->misfit_runnable, __entry->sustain_high_runnable)
);

/*
 * Tracepoint for multi load tracking for task
 */
TRACE_EVENT(mlt_update_task_part,

	TP_PROTO(struct task_struct *p, struct mlt_part *part),

	TP_ARGS(p, part),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cpu			)
		__field(	int,		cur_period		)
		__field(	int,		state			)
		__field(	u64,		contrib			)
		__field(	int,		recent			)
		__field(	int,		last			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->cpu			= p->wake_cpu;
		__entry->cur_period		= part->cur_period;
		__entry->state			= part->state;
		__entry->contrib		= part->contrib;
		__entry->recent			= part->recent;
		__entry->last			= part->periods[part->cur_period];
	),

	TP_printk("comm=%15s pid=%6d cpu=%d cur_period=%1d state=%2d cntrb=%8llu recent=%4d last=%4d",
		__entry->comm, __entry->pid, __entry->cpu, __entry->cur_period,
		__entry->state, __entry->contrib, __entry->recent, __entry->last)
);

TRACE_EVENT(mlt_update_constant_load,

	TP_PROTO(int cpu, unsigned long target_cap, int highest_cpu,
		unsigned long highest_util, bool last_constant_load, bool constant_load),

	TP_ARGS(cpu, target_cap, highest_cpu, highest_util,
		last_constant_load, constant_load),

	TP_STRUCT__entry(
		__field(	int, 		cpu 			)
		__field( 	unsigned long,	target_cap 		)
		__field(	int,		highest_cpu		)
		__field( 	unsigned long, 	highest_util 		)
		__field(	bool,		last_constant_load	)
		__field( 	bool, 		constant_load 		)
	),

	TP_fast_assign(
		__entry->cpu 			= cpu;
		__entry->target_cap 		= target_cap;
		__entry->highest_cpu		= highest_cpu;
		__entry->highest_util 		= highest_util;
		__entry->last_constant_load	= last_constant_load;
		__entry->constant_load 		= constant_load;
	),

	TP_printk("cpu=%d, target_cap=%lu, highest_cpu(util)=%d(%lu), "
		"last_constant_load=%d, constant_load=%d",
		__entry->cpu, __entry->target_cap, __entry->highest_cpu,
		__entry->highest_util, __entry->last_constant_load, __entry->constant_load)
);

TRACE_EVENT(ntu_apply,

	TP_PROTO(struct task_struct *p, struct sched_avg *sa,
		bool sustain_high, bool constant_load, bool ntu_ipc_min_lock,
		int ntu_ratio, int ntu_ipc),

	TP_ARGS(p, sa, sustain_high, constant_load, ntu_ipc_min_lock,
		ntu_ratio, ntu_ipc),

	TP_STRUCT__entry(
		__array(	char, 		comm, 	TASK_COMM_LEN 	)
		__field(	pid_t,		pid 			)
		__field(	int, 		cgroup_idx 		)
		__field(	bool, 		sustain_high 		)
		__field(	bool, 		constant_load 		)
		__field(	bool,		ntu_ipc_min_lock	)
		__field(	int, 		ntu_ratio 		)
		__field(	unsigned long,	util_avg 		)
		__field(	u64, 		util_sum 		)
		__field(	unsigned long, 	runnable_avg 		)
		__field(	u64, 		runnable_sum 		)
		__field(	int,	ntu_ipc				)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid 			= p->pid;
		__entry->cgroup_idx  		= cpuctl_task_group_idx(p);
		__entry->sustain_high  		= sustain_high;
		__entry->constant_load 		= constant_load;
		__entry->ntu_ipc_min_lock 	= ntu_ipc_min_lock;
		__entry->ntu_ratio	 	= ntu_ratio;
		__entry->util_avg 		= sa->util_avg;
		__entry->util_sum 		= sa->util_sum;
		__entry->runnable_avg 		= sa->runnable_avg;
		__entry->runnable_sum 		= sa->runnable_sum;
		__entry->ntu_ipc		= ntu_ipc;
	),

	TP_printk("comm=%15s, pid=%6d, cgroup=%d, "
		"sustain_high=%d, constant_load=%d, ntu_ipc_min_lock=%d, "
		"ntu_ratio=%d, util_avg=%lu, util_sum=%llu, "
		"r_avg=%lu, r_sum=%llu, ntu_ipc=%d",
		__entry->comm, __entry->pid, __entry->cgroup_idx,
		__entry->sustain_high, __entry->constant_load, __entry->ntu_ipc_min_lock,
		__entry->ntu_ratio, __entry->util_avg, __entry->util_sum,
		__entry->runnable_avg, __entry->runnable_sum, __entry->ntu_ipc)
);

/*
* Tracepoint for ecs
*/
TRACE_EVENT(ecs_dynamic_cpu_info,

	TP_PROTO(int cpu, unsigned long util, unsigned long cap, unsigned long busy_ratio,
		int curr_nr, int avg_nr, int avg_misfit),

	TP_ARGS(cpu, util, cap, busy_ratio, curr_nr, avg_nr, avg_misfit),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	util			)
		__field(	unsigned long,	cap			)
		__field(	unsigned long,	busy_ratio		)
		__field(	int,		curr_nr			)
		__field(	int,		avg_nr			)
		__field(	int,		avg_misfit		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->util			= util;
		__entry->cap			= cap;
		__entry->busy_ratio		= busy_ratio;
		__entry->curr_nr		= curr_nr;
		__entry->avg_nr			= avg_nr;
		__entry->avg_misfit		= avg_misfit;
	),

	TP_printk("cpu=%1d, util=%lu, cap=%lu, busy_ratio=%lu, curr_nr=%1d, avg_nr=%1d, avg_misfit=%1d",
		__entry->cpu, __entry->util, __entry->cap, __entry->busy_ratio,
		__entry->curr_nr, __entry->avg_nr, __entry->avg_misfit)
);

struct dynamic_dom;
TRACE_EVENT(ecs_dynamic_domain_info,

	TP_PROTO(int cpu, struct dynamic_dom *domain),

	TP_ARGS(cpu, domain),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		required_cpus		)
		__field(	int,		busy			)
		__field(	int,		nr_run			)
		__field(	int,		nr_misfit		)
		__field(	int,		nr_busy_cpus		)
		__field(	int,		nr_help			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->required_cpus		= domain->required_cpus;
		__entry->busy			= domain->busy;
		__entry->nr_run			= domain->nr_run;
		__entry->nr_misfit		= domain->nr_misfit;
		__entry->nr_busy_cpus		= domain->nr_busy_cpus;
		__entry->nr_help		= domain->nr_help;
	),

	TP_printk("domain=%1d, required_cpus=%1d, busy=%d, nr_run=%1d, nr_misfit=%1d, "
		"nr_busy_cpus=%1d, nr_help=%1d",
		__entry->cpu, __entry->required_cpus, __entry->busy, __entry->nr_run,
		__entry->nr_misfit, __entry->nr_busy_cpus, __entry->nr_help)
);

/*
 * Tracepoint for updating spared cpus
 */
TRACE_EVENT(ecs_update_governor_cpus,

	TP_PROTO(char *label, unsigned int prev_cpus, unsigned int next_cpus,
		unsigned int gov_cpus, unsigned int min_cpus),

	TP_ARGS(label, prev_cpus, next_cpus, gov_cpus, min_cpus),

	TP_STRUCT__entry(
		__field(	unsigned int,	prev_cpus		)
		__field(	unsigned int,	next_cpus		)
		__field(	unsigned int,	gov_cpus		)
		__field(	unsigned int,	min_cpus		)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->prev_cpus		= prev_cpus;
		__entry->next_cpus		= next_cpus;
		__entry->gov_cpus		= gov_cpus;
		__entry->min_cpus		= min_cpus;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("reason=%s, cpus=0x%03x->0x%03x, gov_cpus=0x%03x, min_cpus=0x%03x",
		__entry->label, __entry->prev_cpus, __entry->next_cpus,
		__entry->gov_cpus, __entry->min_cpus)
);

/*
 * Tracepoint for frequency boost
 */
TRACE_EVENT(freqboost_boosted_util,

	TP_PROTO(int cpu, int ratio, unsigned long util, long margin),

	TP_ARGS(cpu, ratio, util, margin),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		ratio			)
		__field(	unsigned long,	util			)
		__field(	long,		margin			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->ratio			= ratio == INT_MIN ? -9999 : ratio;
		__entry->util			= util;
		__entry->margin			= margin;
	),

	TP_printk("cpu=%1d, ratio=%5d, util=%4lu, margin=%4ld",
		__entry->cpu, __entry->ratio, __entry->util, __entry->margin)
);

TRACE_EVENT(freqboost_htsk_boosted_util,

	TP_PROTO(int cpu, int hratio, int ratio, int boost, unsigned long util, long margin),

	TP_ARGS(cpu, hratio, ratio, boost, util, margin),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		hratio			)
		__field(	int,		ratio			)
		__field(	int,		boost			)
		__field(	unsigned long,	util			)
		__field(	long,		margin			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->hratio			= hratio;
		__entry->ratio			= ratio;
		__entry->boost			= boost;
		__entry->util			= util;
		__entry->margin			= margin;
	),

	TP_printk("cpu=%1d, hratio=%3d, ratio=%3d, boost=%3d, util=%4lu, margin=%3ld",
		__entry->cpu, __entry->hratio, __entry->ratio,
		__entry->boost, __entry->util,  __entry->margin)
);

struct fclamp_data;

/*
 * Tracepoint for fclamp applying result
 */
TRACE_EVENT(fclamp_apply,

	TP_PROTO(int cpu, unsigned int orig_freq, unsigned int new_freq, int boost,
		struct fclamp_data *fcd),

	TP_ARGS(cpu, orig_freq, new_freq, boost, fcd),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned int,	orig_freq		)
		__field(	unsigned int,	new_freq		)
		__field(	int,		boost			)
		__field(	int,		clamp_freq		)
		__field(	int,		target_period		)
		__field(	int,		target_ratio		)
		__field(	int,		type			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->orig_freq		= orig_freq;
		__entry->new_freq		= new_freq;
		__entry->boost			= boost;
		__entry->clamp_freq		= fcd->freq;
		__entry->target_period		= fcd->target_period;
		__entry->target_ratio		= fcd->target_ratio;
		__entry->type			= fcd->type;
	),

	TP_printk("cpu=%1d, orig_freq=%7u, new_freq=%7u, boost=%1d, clamp_freq=%7d, "
		"target_period=%3d, target_ratio=%4d, type=%s",
		__entry->cpu, __entry->orig_freq, __entry->new_freq, __entry->boost,
		__entry->clamp_freq, __entry->target_period, __entry->target_ratio,
		__entry->type ? "MAX" : "MIN")
);

/*
 * Tracepoint for checking fclamp release
 */
TRACE_EVENT(fclamp_can_release,

	TP_PROTO(int cpu, int type, int period, int target_period,
			int target_ratio, int active_ratio),

	TP_ARGS(cpu, type, period, target_period, target_ratio, active_ratio),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		type			)
		__field(	int,		period			)
		__field(	int,		target_period		)
		__field(	int,		target_ratio		)
		__field(	int,		active_ratio		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->type			= type;
		__entry->period			= period;
		__entry->target_period		= target_period;
		__entry->target_ratio		= target_ratio;
		__entry->active_ratio		= active_ratio;
	),

	TP_printk("cpu=%1d, type=%3s, period=%3d, target_period=%3d, "
		"target_ratio=%4d, active_ratio=%4d",
		__entry->cpu, __entry->type ? "MAX" : "MIN",
		__entry->period, __entry->target_period,
		__entry->target_ratio, __entry->active_ratio)
);

/*
 * Tracepoint for cfs_rq load tracking:
 */
TRACE_EVENT(sched_load_cfs_rq,

	TP_PROTO(struct cfs_rq *cfs_rq),

	TP_ARGS(cfs_rq),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	load			)
		__field(	unsigned long,	rbl_load		)
		__field(	unsigned long,	util			)
	),

	TP_fast_assign(
		struct rq *rq = __trace_get_rq(cfs_rq);

		__entry->cpu			= rq->cpu;
		__entry->load			= cfs_rq->avg.load_avg;
		__entry->rbl_load		= cfs_rq->avg.runnable_avg;
		__entry->util			= cfs_rq->avg.util_avg;
	),

	TP_printk("cpu=%1d, load=%4lu, rbl_load=%4lu, util=%4lu",
		__entry->cpu, __entry->load, __entry->rbl_load,__entry->util)
);

TRACE_EVENT(dslt_cfs,

	TP_PROTO(struct cfs_rq *cfs_rq, struct ems_dslt_rq *drq),

	TP_ARGS(cfs_rq, drq),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		sensitivity		)
		__field(	unsigned long,	util			)
		__field(	unsigned long,	rbl_load		)
		__field(	unsigned long,	dslt_rba		)
		__field(	unsigned long,	dslt_util_est		)
	),

	TP_fast_assign(
		struct rq *rq = __trace_get_rq(cfs_rq);
		__entry->cpu			= rq->cpu;
		__entry->sensitivity		= drq->sensitivity;
		__entry->util			= cfs_rq->avg.util_avg;
		__entry->rbl_load		= cfs_rq->avg.runnable_avg;
		__entry->dslt_rba		= drq->avg.runnable_avg;
		__entry->dslt_util_est		= drq->avg.util_est;
	),

	TP_printk("cpu=%1d, sensitivity=%3d, util=%4lu, rbl_load=%4lu, dslt_rba=%4lu, dslt_ue=%4lu",
		__entry->cpu, __entry->sensitivity,
		__entry->util, __entry->rbl_load, __entry->dslt_rba,
		__entry->dslt_util_est)
);

/*
 * Tracepoint for rt_rq load tracking:
 */
TRACE_EVENT(sched_load_rt_rq,

	TP_PROTO(struct rq *rq),

	TP_ARGS(rq),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	util			)
	),

	TP_fast_assign(
		__entry->cpu			= rq->cpu;
		__entry->util			= rq->avg_rt.util_avg;
	),

	TP_printk("cpu=%1d, util=%4lu", __entry->cpu, __entry->util)
);

/*
 * Tracepoint for dl_rq load tracking:
 */
TRACE_EVENT(sched_load_dl_rq,

	TP_PROTO(struct rq *rq),

	TP_ARGS(rq),

	TP_STRUCT__entry(
		__field(	int,		cpu 			)
		__field(	unsigned long,	util			)
	),

	TP_fast_assign(
		__entry->cpu			= rq->cpu;
		__entry->util			= rq->avg_dl.util_avg;
	),

	TP_printk("cpu=%1d util=%4lu", __entry->cpu, __entry->util)
);

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
/*
 * Tracepoint for irq load tracking:
 */
TRACE_EVENT(sched_load_irq,

	TP_PROTO(struct rq *rq),

	TP_ARGS(rq),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	util			)
	),

	TP_fast_assign(
		__entry->cpu			= rq->cpu;
		__entry->util			= rq->avg_irq.util_avg;
	),

	TP_printk("cpu=%1d util=%4lu", __entry->cpu, __entry->util)
);
#endif

#ifdef CREATE_TRACE_POINTS
static inline
struct rq * __trace_get_rq(struct cfs_rq *cfs_rq)
{
#ifdef CONFIG_FAIR_GROUP_SCHED
	return cfs_rq->rq;
#else
	return container_of(cfs_rq, struct rq, cfs);
#endif
}
#endif /* CREATE_TRACE_POINTS */

/*
 * Tracepoint for sched	entity load tracking:
 */
TRACE_EVENT(sched_load_se,

	TP_PROTO(struct sched_entity *se),

	TP_ARGS(se),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	load			)
		__field(	unsigned long,	rbl_load		)
		__field(	unsigned long,	util			)
	),

	TP_fast_assign(
#ifdef CONFIG_FAIR_GROUP_SCHED
		struct cfs_rq *gcfs_rq = se->my_q;
#else
		struct cfs_rq *gcfs_rq = NULL;
#endif
		struct task_struct *p = gcfs_rq ? NULL : container_of(se, struct task_struct, se);
		struct rq *rq = gcfs_rq ? __trace_get_rq(gcfs_rq) : NULL;

		__entry->cpu			= rq ? rq->cpu
						: task_cpu((container_of(se, struct task_struct, se)));
		memcpy(__entry->comm, p ? p->comm : "(null)", p ? TASK_COMM_LEN : sizeof("(null)"));
		__entry->pid			= p ? p->pid : -1;
		__entry->load			= se->avg.load_avg;
		__entry->rbl_load		= se->avg.runnable_avg;
		__entry->util			= se->avg.util_avg;
	),

	TP_printk("cpu=%1d, comm=%15s, pid=%6d, load=%4lu, rbl_load=%4lu, util=%4lu",
		__entry->cpu, __entry->comm, __entry->pid,
		__entry->load, __entry->rbl_load, __entry->util)
);

TRACE_EVENT(dslt_se,

	TP_PROTO(struct sched_entity *se, struct ems_dslt_task *dtsk),

	TP_ARGS(se, dtsk),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	util			)
		__field(	unsigned long,	rbl_load		)
		__field(	unsigned long,	dslt_rba		)
	),

	TP_fast_assign(
#ifdef CONFIG_FAIR_GROUP_SCHED
		struct cfs_rq *gcfs_rq = se->my_q;
#else
		struct cfs_rq *gcfs_rq = NULL;
#endif
		struct task_struct *p = gcfs_rq ? NULL : container_of(se, struct task_struct, se);
		struct rq *rq = gcfs_rq ? __trace_get_rq(gcfs_rq) : NULL;

		__entry->cpu			= rq ? rq->cpu
						: task_cpu((container_of(se, struct task_struct, se)));
		memcpy(__entry->comm, p ? p->comm : "(null)", p ? TASK_COMM_LEN : sizeof("(null)"));
		__entry->pid			= p ? p->pid : -1;
		__entry->util			= se->avg.util_avg;
		__entry->rbl_load		= se->avg.runnable_avg;
		__entry->dslt_rba		= dtsk->avg.runnable_avg;
	),

	TP_printk("cpu=%1d, comm=%15s, pid=%6d, util=%4lu, rbl_load=%4lu, dslt_rba=%4lu",
		__entry->cpu, __entry->comm, __entry->pid,
		__entry->util, __entry->rbl_load, __entry->dslt_rba)
);

TRACE_EVENT(dslt_enqueue_sensitivity,

	TP_PROTO(struct task_struct *p, struct ems_dslt_rq *drq, struct dslt_avg *da, int org_rq_sensitivity, int flags, int running),

	TP_ARGS(p, drq, da, org_rq_sensitivity, flags, running),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		prio			)
		__field(	int,		cgroup_idx		)
		__field(	int,		cpu			)
		__field(	int,		org_rq_sensitivity	)
		__field(	int,		rq_sensitivity		)
		__field(	int,		tsk_runnable_sensitivity)
		__field(	int,		flags			)
		__field(	int,		running			)
		__field(	int,		tsk_running_sensitivity )
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->prio			= p->prio;
		__entry->cgroup_idx = cpuctl_task_group_idx(p);
		__entry->cpu			= drq->cpu;
		__entry->org_rq_sensitivity	= org_rq_sensitivity;
		__entry->rq_sensitivity		= drq->sensitivity;
		__entry->tsk_runnable_sensitivity = da->runnable_sensitivity;
		__entry->flags			= flags;
		__entry->running		= running;
		__entry->tsk_running_sensitivity = da->running_sensitivity;
	),

	TP_printk("cpu=%1d, comm=%15s, pid=%6d, prio=%d, cgroup=%d "
		"org_rq_sens=%d, rq_sens=%d, running=%d, runnable_sens=%d, running_sens=%d, flags=0x%x",
		__entry->cpu, __entry->comm, __entry->pid, __entry->prio,
		__entry->cgroup_idx, __entry->org_rq_sensitivity,
		__entry->rq_sensitivity, __entry->running, __entry->tsk_runnable_sensitivity,
		__entry->tsk_running_sensitivity, __entry->flags)
);

TRACE_EVENT(dslt_dequeue_sensitivity,

	TP_PROTO(struct task_struct *p, struct ems_dslt_rq *drq, struct dslt_avg *da, int org_rq_sensitivity,
		struct task_struct *c, int flags, int running),

	TP_ARGS(p, drq, da, org_rq_sensitivity, c, flags, running),

	TP_STRUCT__entry(
		__array(	char,		curr,	TASK_COMM_LEN	)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		prio			)
		__field(	int,		cgroup_idx		)
		__field(	int,		cpu			)
		__field(	int,		org_rq_sensitivity	)
		__field(	int,		rq_sensitivity		)
		__field(	int,		tsk_runnable_sensitivity)
		__field(	int,		flags			)
		__field(	int,		running			)
		__field(	int,		tsk_running_sensitivity )
	),

	TP_fast_assign(
		memcpy(__entry->curr, c->comm, TASK_COMM_LEN);
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->prio			= p->prio;
		__entry->cgroup_idx = cpuctl_task_group_idx(p);
		__entry->cpu			= drq->cpu;
		__entry->org_rq_sensitivity	= org_rq_sensitivity;
		__entry->rq_sensitivity		= drq->sensitivity;
		__entry->tsk_runnable_sensitivity = da->runnable_sensitivity;
		__entry->flags			= flags;
		__entry->running		= running;
		__entry->tsk_running_sensitivity = da->running_sensitivity;
	),

	TP_printk("cpu=%1d, comm=%15s, pid=%6d, prio=%d, cgroup=%d "
		"org_rq_sens=%d, rq_sens=%d, running=%d, runnable_sens=%d, running_sens=%d, flags=0x%x",
		__entry->cpu, __entry->comm, __entry->pid, __entry->prio,
		__entry->cgroup_idx, __entry->org_rq_sensitivity,
		__entry->rq_sensitivity, __entry->running, __entry->tsk_runnable_sensitivity,
		__entry->tsk_running_sensitivity, __entry->flags)
);

TRACE_EVENT(dslt_switch_sensitivity,

	TP_PROTO(int cpu, struct task_struct *prev, struct task_struct *next, int org_rq_sensitivity,
		struct ems_dslt_rq *drq, struct dslt_avg *psa, struct dslt_avg *nsa),

	TP_ARGS(cpu, prev, next, org_rq_sensitivity, drq, psa, nsa),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		prev_comm,	TASK_COMM_LEN	)
		__field(	pid_t,		prev_pid			)
		__field(	int,		prev_prio			)
		__array(	char,		next_comm,	TASK_COMM_LEN	)
		__field(	pid_t,		next_pid			)
		__field(	int,		next_prio			)
		__field(	int,		org_rq_sensitivity		)
		__field(	int,		rq_sensitivity		)
		__field(	int,		ptsk_runnable_sensitivity		)
		__field(	int,		ptsk_running_sensitivity		)
		__field(	int,		ntsk_runnable_sensitivity		)
		__field(	int,		ntsk_running_sensitivity		)
	),

	TP_fast_assign(
		__entry->cpu			= drq->cpu;
		memcpy(__entry->prev_comm, prev->comm, TASK_COMM_LEN);
		__entry->prev_pid		= prev->pid;
		__entry->prev_prio		= prev->prio;
		memcpy(__entry->next_comm, next->comm, TASK_COMM_LEN);
		__entry->next_pid		= next->pid;
		__entry->next_prio		= next->prio;
		__entry->org_rq_sensitivity	= org_rq_sensitivity;
		__entry->rq_sensitivity		= drq->sensitivity;
		__entry->ptsk_runnable_sensitivity = psa->runnable_sensitivity;
		__entry->ptsk_running_sensitivity = psa->running_sensitivity;
		__entry->ntsk_runnable_sensitivity = nsa->runnable_sensitivity;
		__entry->ntsk_running_sensitivity = nsa->running_sensitivity;
	),

	TP_printk("cpu=%1d, prev_comm=%15s, prev_pid=%6d, prev_prio=%d "
		  "next_comm=%15s, next_pid=%6d, next_prio=%d, org_rq_sens=%d "
		  "rq_sens=%d, prev_runnable_sens=%d, prev_running_sens=%d "
		  "next_runnable_sens=%d, next_running_sens=%d",
		__entry->cpu, __entry->prev_comm, __entry->prev_pid, __entry->prev_prio,
		__entry->next_comm, __entry->next_pid, __entry->next_prio,
		__entry->org_rq_sensitivity, __entry->rq_sensitivity, __entry->ptsk_runnable_sensitivity,
		__entry->ptsk_running_sensitivity, __entry->ntsk_runnable_sensitivity, __entry->ntsk_running_sensitivity)
);

/*
 * Tracepoint for system overutilized flag
 */
TRACE_EVENT(sched_overutilized,

	TP_PROTO(int overutilized),

	TP_ARGS(overutilized),

	TP_STRUCT__entry(
		__field(	int,		overutilized		)
	),

	TP_fast_assign(
		__entry->overutilized	= overutilized;
	),

	TP_printk("overutilized=%d",
		__entry->overutilized)
);

TRACE_EVENT(gsc_decision_activate,

	TP_PROTO(unsigned long group_load, u64 now, char *label, int lv2_hits),

	TP_ARGS(group_load, now, label, lv2_hits),

	TP_STRUCT__entry(
		__field(	unsigned long,	group_load		)
		__field(	u64,		now			)
		__array(	char,		label,	LABEL_LEN	)
		__field(	int,		lv2_hits		)
	),

	TP_fast_assign(
		__entry->group_load		= group_load;
		__entry->now			= now;
		strncpy(__entry->label, label, LABEL_LEN - 1);
		__entry->lv2_hits		= lv2_hits;
	),

	TP_printk("group_load=%lu, now=%llu, reason=%s, lv2_hits=%d",
		__entry->group_load, __entry->now, __entry->label, __entry->lv2_hits)
);

TRACE_EVENT(gsc_freq_boost,

	TP_PROTO(int cpu, int avg_runnable, int ratio, int boosted),

	TP_ARGS(cpu, avg_runnable, ratio, boosted),

	TP_STRUCT__entry(
		__field(	int,		cpu		)
		__field(	int,		avg_runnable	)
		__field(	int,		ratio		)
		__field(	int,		boosted)
	),

	TP_fast_assign(
		__entry->cpu		= cpu;
		__entry->avg_runnable	= avg_runnable;
		__entry->ratio		= ratio;
		__entry->boosted	= boosted;
	),

	TP_printk("cpu=%d, avg_runnable=%6d boost_ratio=%3d boosted=%2d",
		__entry->cpu, __entry->avg_runnable,
		__entry->ratio, __entry->boosted)
);

/* CPUIDLE Governor */
TRACE_EVENT(halo_reflect,

	TP_PROTO(unsigned int cpu, unsigned int waker, u64 sleep_length),

	TP_ARGS(cpu, waker, sleep_length),

	TP_STRUCT__entry(
		__field(	unsigned int,	cpu			)
		__field(	unsigned int,	waker			)
		__field(	u64,		sleep_length		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->waker			= waker;
		__entry->sleep_length		= sleep_length;
	),

	TP_printk("cpu=%1d, waker=%1d, sleep_length=%10llu",
		__entry->cpu, __entry->waker, __entry->sleep_length)
);

TRACE_EVENT(halo_timer_fn,

	TP_PROTO(unsigned int cpu, unsigned int worked),

	TP_ARGS(cpu, worked),

	TP_STRUCT__entry(
		__field(	unsigned int,	cpu			)
		__field(	unsigned int,	worked			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->worked			= worked;
	),

	TP_printk("cpu=%1d, worked=%1d",
		__entry->cpu, __entry->worked)
);

TRACE_EVENT(halo_tick,

	TP_PROTO(unsigned int cpu, bool triggerd, s64 last_tick_ns),

	TP_ARGS(cpu, triggerd, last_tick_ns),

	TP_STRUCT__entry(
		__field(	unsigned int,	cpu			)
		__field(	bool,		triggerd		)
		__field(	s64,		last_tick_ns		)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->triggerd		= triggerd;
		__entry->last_tick_ns		= last_tick_ns;
	),

	TP_printk("cpu=%d, triggerd=%d, last_tick_ns=%lld",
		__entry->cpu, __entry->triggerd, __entry->last_tick_ns)
);

TRACE_EVENT(halo_ipi,

	TP_PROTO(int cpu, int heavy, s64 min, s64 max, s64 cur, s64 avg, s64 correct, s64 pred),

	TP_ARGS(cpu, heavy, min, max, cur, avg, correct, pred),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		heavy			)
		__field(	s64,		min			)
		__field(	s64,		max			)
		__field(	s64,		cur			)
		__field(	s64,		avg			)
		__field(	s64,		correct			)
		__field(	s64,		pred			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->heavy			= heavy;
		__entry->min			= min;
		__entry->max			= max;
		__entry->cur			= cur;
		__entry->avg			= avg;
		__entry->correct		= correct;
		__entry->pred			= pred;
	),

	TP_printk("cpu=%1d, heavy=%4d, min=%10lld, max=%10lld, cur=%10lld, "
		"avg=%10lld, corrrect=%10lld, pred=%10lld",
		__entry->cpu, __entry->heavy, __entry->min, __entry->max, __entry->cur,
		__entry->avg, __entry->correct, __entry->pred)
);

TRACE_EVENT(halo_hstate,

	TP_PROTO(int cpu, int idx, u64 hit, u64 miss),

	TP_ARGS(cpu, idx, hit, miss),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		idx			)
		__field(	u64,		hit			)
		__field(	u64,		miss			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->idx			= idx;
		__entry->hit			= hit;
		__entry->miss			= miss;
	),

	TP_printk("cpu=%1d, idx=%1d, hit=%4llu, miss=%4llu",
		  __entry->cpu, __entry->idx, __entry->hit, __entry->miss)
);

/*
 * Tracepoint for lb cpu util
 */
TRACE_EVENT(lb_cpu_util,

	TP_PROTO(int cpu, char *label),

	TP_ARGS(cpu, label),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		active_balance		)
		__field(	int,		idle			)
		__field(	int,		nr_running		)
		__field(	int,		cfs_nr_running		)
		__field(	int,		nr_misfits		)
		__field(	unsigned long,	cpu_util		)
		__field(	unsigned long,	capacity_orig		)
		__array(	char,		label,	LABEL_LEN	)
		),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->active_balance		= cpu_rq(cpu)->active_balance;
		__entry->idle			= available_idle_cpu(cpu);
		__entry->nr_running		= cpu_rq(cpu)->nr_running;
		__entry->cfs_nr_running		= cpu_rq(cpu)->cfs.h_nr_running;
		__entry->nr_misfits		= ems_rq_nr_misfited(cpu_rq(cpu));
		__entry->cpu_util		= ml_cpu_util(cpu);
		__entry->capacity_orig		= ems_rq_cpu_capacity_orig(cpu_rq(cpu));
		strncpy(__entry->label, label, LABEL_LEN - 1);
		),

	TP_printk("cpu=%d, active_balance=%1d, idle=%1d, nr_running=%2d, cfs_nr_running=%2d, "
		"nr_misfits=%1d, cpu_util=%4lu, capacity=%4lu, reason=%s",
		__entry->cpu, __entry->active_balance, __entry->idle,
		__entry->nr_running, __entry->cfs_nr_running, __entry->nr_misfits,
		__entry->cpu_util, __entry->capacity_orig, __entry->label)
);


/*
 * Tracepoint for task migration control
 */
TRACE_EVENT(lb_can_migrate_task,

	TP_PROTO(struct task_struct *tsk, int dst_cpu, int migrate, char *label),

	TP_ARGS(tsk, dst_cpu, migrate, label),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		src_cpu			)
		__field(	int,		dst_cpu			)
		__field(	int,		migrate			)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		memcpy(__entry->comm, tsk->comm, TASK_COMM_LEN);
		__entry->pid			= tsk->pid;
		__entry->src_cpu		= task_cpu(tsk);
		__entry->dst_cpu		= dst_cpu;
		__entry->migrate		= migrate;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("comm=%15s, pid=%6d, src_cpu=%1d, dst_cpu=%1d, migrate=%1d, reason=%s",
		__entry->comm, __entry->pid, __entry->src_cpu, __entry->dst_cpu,
		__entry->migrate, __entry->label)
);

 /*
 * Tracepoint for misfit
 */
TRACE_EVENT(lb_update_misfit,
	TP_PROTO(struct task_struct *p, bool old, bool cur, int cpu, u64 rq_nr_misfit, char *label),

	TP_ARGS(p, old, cur, cpu, rq_nr_misfit, label),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	util			)
		__field(	bool,		old			)
		__field(	bool,		cur			)
		__field(	int,		cpu			)
		__field(	u64,		rq_nr_misfit		)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->util			= ml_uclamp_task_util(p);
		__entry->old			= old;
		__entry->cur			= cur;
		__entry->cpu			= cpu;
		__entry->rq_nr_misfit		= rq_nr_misfit;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("comm=%15s, pid=%6d, util=%4ld, old=%1d, cur=%1d, cpu=%1d, "
		"rq_nr_misfit=%3llu, reason=%s",
		__entry->comm, __entry->pid, __entry->util, __entry->old,
		__entry->cur, __entry->cpu, __entry->rq_nr_misfit, __entry->label)
);

/*
 * Tracepoint for nohz balancer kick
 */
TRACE_EVENT(lb_nohz_balancer_kick,

	TP_PROTO(struct rq *rq),

	TP_ARGS(rq),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned int,	nr_running		)
		__field(	unsigned int,	cfs_nr_running		)
	),

	TP_fast_assign(
		__entry->cpu			= rq->cpu;
		__entry->nr_running		= rq->nr_running;
		__entry->cfs_nr_running		= rq->cfs.h_nr_running;
	),

	TP_printk("cpu=%1d, nr_running=%2u, cfs_nr_running=%2u",
		__entry->cpu, __entry->nr_running, __entry->cfs_nr_running)
);

TRACE_EVENT(lb_aggressive_idle_pull,

	TP_PROTO(int src_cpu, int dst_cpu, int pulled, const char *label),

	TP_ARGS(src_cpu, dst_cpu, pulled, label),

	TP_STRUCT__entry(
		__field(	int,		src_cpu			)
		__field(	int,		dst_cpu			)
		__field(	int,		pulled			)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->src_cpu		= src_cpu;
		__entry->dst_cpu		= dst_cpu;
		__entry->pulled			= pulled;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("busiest_cpu=%d, dst_cpu=%d, pulled=%d, reason=%s",
		__entry->src_cpu, __entry->dst_cpu, __entry->pulled, __entry->label)
);

/*
 * Tracepoint for mhdvfs profile
 */
TRACE_EVENT(mhdvfs_profile,

	TP_PROTO(u64 ipc_sum, u64 mspc_sum),

	TP_ARGS(ipc_sum, mspc_sum),

	TP_STRUCT__entry(
		__field(	u64,		ipc_sum			)
		__field(	u64,		mspc_sum		)
	),

	TP_fast_assign(
		__entry->ipc_sum		= ipc_sum;
		__entry->mspc_sum		= mspc_sum;
	),

	TP_printk("ipc_sum=%10llu, mspc_sum=%10llu",
		  __entry->ipc_sum, __entry->mspc_sum)
);

/*
 * Tracepoint for kicking dsufreq by mhdvfs
 */
TRACE_EVENT(mhdvfs_kick_dsufreq,

	TP_PROTO(u64 mspc, int ratio),

	TP_ARGS(mspc, ratio),

	TP_STRUCT__entry(
		__field(	u64,		mspc			)
		__field(	int,		ratio			)
	),

	TP_fast_assign(
		__entry->mspc			= mspc;
		__entry->ratio			= ratio;
	),

	TP_printk("mspc=%10llu, ratio=%4d", __entry->mspc, __entry->ratio)
);

/*
 * Tracepoint for kicking miffreq by mhdvfs
 */
TRACE_EVENT(mhdvfs_kick_miffreq,

	TP_PROTO(u64 mspc, unsigned int ratio, u64 duration_ms),

	TP_ARGS(mspc, ratio, duration_ms),

	TP_STRUCT__entry(
		__field(	u64,		mspc			)
		__field(	unsigned int,	ratio			)
		__field(	u64,		duration_ms		)
	),

	TP_fast_assign(
		__entry->mspc			= mspc;
		__entry->ratio			= ratio;
		__entry->duration_ms		= duration_ms;
	),

	TP_printk("mspc=%10llu, ratio=%4u, duration=%10llu",
		__entry->mspc, __entry->ratio, __entry->duration_ms)
);

/*
 * Tracepoint for kicking cpufreq by mhdvfs
 */
TRACE_EVENT(mhdvfs_kick_cpufreq,

	TP_PROTO(int cpu, u64 ipc, int ratio),

	TP_ARGS(cpu, ipc, ratio),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	u64,		ipc			)
		__field(	int,		ratio			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->ipc			= ipc;
		__entry->ratio			= ratio;
	),

	TP_printk("cpu=%1d, ipc=%10llu, ratio=%4d",
		__entry->cpu, __entry->ipc, __entry->ratio)
);

/*
 * Tracepoint for emstune task boost
 */
TRACE_EVENT(emstune_task_boost,

	TP_PROTO(struct task_struct *p, int val, char *label),

	TP_ARGS(p, val, label),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__array(	char,		val,	10		)
		__array( 	char,		label,	64		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		strncpy(__entry->val, val ? "on" : "off", 9);
		strncpy(__entry->label, label, 63);
	),

	TP_printk("%s: comm=%s pid=%d val=%s",
		__entry->label, __entry->comm, __entry->pid, __entry->val)
);

TRACE_EVENT(pago_data_snapshot,

	TP_PROTO(int cpu, int idle_sample, u64 last_sampled, u64 last_updated, u64 *data, char *label),

	TP_ARGS(cpu, idle_sample, last_sampled, last_updated, data, label),

	TP_STRUCT__entry(
		__field( int,		cpu		)
		__field( int,		idle_sample	)
		__field( u64,		last_sampled	)
		__field( u64,		last_updated	)
		__field( u64,		cycle		)
		__field( u64,		inst		)
		__field( u64,		l2_refill	)
		__field( u64,		l3_refill	)
		__field( u64,		mem_stall	)
#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
		__field( u64,		cme_cycle	)
		__field( u64,		cme_l3_refill	)
#endif
		__array( char,		label,	64	)
	),

	TP_fast_assign(
		__entry->cpu		= cpu;
		__entry->idle_sample	= idle_sample;
		__entry->last_sampled	= last_sampled;
		__entry->last_updated	= last_updated;
		__entry->cycle		= data[0];
		__entry->inst		= data[1];
		__entry->l2_refill	= data[2];
		__entry->l3_refill	= data[3];
		__entry->mem_stall	= data[4];
#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
		__entry->cme_cycle	= data[5];
		__entry->cme_l3_refill	= data[6];
#endif
		strncpy(__entry->label, label, 63);
	),

#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
	TP_printk("%s: cpu=%d idle_sample=%d last_sampled=%llu last_updated=%llu "
			"cycle=%llu inst=%llu l2_refill=%llu l3_refill=%llu mem_stall=%llu "
			"cme_cycle=%llu cme_l3_refill=%llu",
		__entry->label, __entry->cpu, __entry->idle_sample,
		__entry->last_sampled, __entry->last_updated, __entry->cycle,
		__entry->inst, __entry->l2_refill, __entry->l3_refill, __entry->mem_stall,
		__entry->cme_cycle, __entry->cme_l3_refill)
#else
	TP_printk("%s: cpu=%d idle_sample=%d last_sampled=%llu last_updated=%llu "
			"cycle=%llu inst=%llu l2_refill=%llu l3_refill=%llu mem_stall=%llu",
		__entry->label, __entry->cpu, __entry->idle_sample,
		__entry->last_sampled, __entry->last_updated, __entry->cycle,
		__entry->inst, __entry->l2_refill, __entry->l3_refill, __entry->mem_stall)
#endif

);

TRACE_EVENT(pago_delta_snapshot,

	TP_PROTO(int cpu, int idle_sample, s64 delta_us, u64 *data),

	TP_ARGS(cpu, idle_sample, delta_us, data),

	TP_STRUCT__entry(
		__field( int,		cpu		)
		__field( int,		idle_sample	)
		__field( s64,		delta_us	)
		__field( u64,		cycle		)
		__field( u64,		inst		)
		__field( u64,		l2_refill	)
		__field( u64,		l3_refill	)
		__field( u64,		mem_stall	)
#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
		__field( u64,		cme_cycle	)
		__field( u64,		cme_l3_refill	)
#endif
	),

	TP_fast_assign(
		__entry->cpu		= cpu;
		__entry->idle_sample	= idle_sample;
		__entry->delta_us	= delta_us;
		__entry->cycle		= data[0];
		__entry->inst		= data[1];
		__entry->l2_refill	= data[2];
		__entry->l3_refill	= data[3];
		__entry->mem_stall	= data[4];
#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
		__entry->cme_cycle	= data[5];
		__entry->cme_l3_refill	= data[6];
#endif
	),

#if IS_ENABLED(CONFIG_SCHED_EMS_CME_GOV)
	TP_printk("cpu=%d idle_sample=%d delta_us=%lld cycle=%llu inst=%llu l2_refill=%llu "
			"l3_refill=%llu mem_stall=%llu cme_cycle=%llu cme_l3_refill=%llu",
		__entry->cpu, __entry->idle_sample, __entry->delta_us, __entry->cycle,
		__entry->inst, __entry->l2_refill, __entry->l3_refill, __entry->mem_stall,
		__entry->cme_cycle, __entry->cme_l3_refill)
#else
	TP_printk("cpu=%d idle_sample=%d delta_us=%lld cycle=%llu inst=%llu l2_refill=%llu "
			"l3_refill=%llu mem_stall=%llu",
		__entry->cpu, __entry->idle_sample, __entry->delta_us, __entry->cycle,
		__entry->inst, __entry->l2_refill, __entry->l3_refill, __entry->mem_stall)
#endif
);

TRACE_EVENT(pago_target_snapshot,

	TP_PROTO(int cpu, unsigned int target_freq, unsigned int cycle_freq, unsigned int scale_freq,
		unsigned int cur_freq, unsigned int shift_level, const char *label),

	TP_ARGS(cpu, target_freq, cycle_freq, scale_freq, cur_freq, shift_level, label),

	TP_STRUCT__entry(
		__field( int,		cpu		)
		__field( unsigned int,	target_freq	)
		__field( unsigned int,	cycle_freq	)
		__field( unsigned int,	scale_freq	)
		__field( unsigned int,	cur_freq	)
		__field( unsigned int,	shift_level	)
		__array( char,		label,	64	)
	),

	TP_fast_assign(
		__entry->cpu		= cpu;
		__entry->target_freq	= target_freq;
		__entry->cycle_freq	= cycle_freq;
		__entry->scale_freq	= scale_freq;
		__entry->cur_freq	= cur_freq;
		__entry->shift_level	= shift_level;
		strncpy(__entry->label, label, 63);
	),

	TP_printk("%s: cpu=%d target_freq=%u cycle_freq=%u scale_freq=%u cur_freq=%u shift_level=%u",
		__entry->label, __entry->cpu, __entry->target_freq, __entry->cycle_freq,
		__entry->scale_freq, __entry->cur_freq, __entry->shift_level)
);

TRACE_EVENT(pago_calc_target_freq,

	TP_PROTO(unsigned int bw_threshold, unsigned long total_miss, unsigned int max_stall_ratio,
		unsigned long threshold, unsigned long bw_idle_count, unsigned int target_freq,
		const char *label),

	TP_ARGS(bw_threshold, total_miss, max_stall_ratio, threshold, bw_idle_count, target_freq, label),

	TP_STRUCT__entry(
		__field( unsigned int,	bw_threshold	)
		__field( unsigned long,	total_miss	)
		__field( unsigned int,	max_stall_ratio	)
		__field( unsigned long,	threshold	)
		__field( unsigned long,	bw_idle_count	)
		__field( unsigned int,	target_freq	)
		__array( char,		label,	64	)
	),

	TP_fast_assign(
		__entry->bw_threshold	= bw_threshold;
		__entry->total_miss	= total_miss;
		__entry->max_stall_ratio = max_stall_ratio;
		__entry->threshold	= threshold;
		__entry->bw_idle_count	= bw_idle_count;
		__entry->target_freq	= target_freq;
		strncpy(__entry->label, label, 63);
	),

	TP_printk("%s: bw_threshold=%u total_miss=%lu max_stall_ratio=%u "
			"threshold=%lu bw_idle_count=%lu target_freq=%u",
		__entry->label, __entry->bw_threshold, __entry->total_miss, __entry->max_stall_ratio,
		__entry->threshold, __entry->bw_idle_count, __entry->target_freq)
);

TRACE_EVENT(pago_mem_stall_ratio,

	TP_PROTO(int cpu, unsigned int stall_ratio),

	TP_ARGS(cpu, stall_ratio),

	TP_STRUCT__entry(
		__field( int,		cpu		)
		__field( unsigned int,	stall_ratio	)
	),

	TP_fast_assign(
		__entry->cpu		= cpu;
		__entry->stall_ratio 	= stall_ratio;
	),

	TP_printk("cpu=%d, stall_ratio=%u", __entry->cpu, __entry->stall_ratio)
);

TRACE_EVENT(pago_emstune_boost,

	TP_PROTO(int boost, s64 activated_ts),

	TP_ARGS(boost, activated_ts),

	TP_STRUCT__entry(
		__field( int,		boost		)
		__field( s64,		activated_ts	)
	),

	TP_fast_assign(
		__entry->boost		= boost;
		__entry->activated_ts 	= activated_ts;
	),

	TP_printk("boost=%d, activated_ts=%lld", __entry->boost, __entry->activated_ts)
);

TRACE_EVENT(pago_boost,

	TP_PROTO(unsigned int boost_freq, const char *label),

	TP_ARGS(boost_freq, label),

	TP_STRUCT__entry(
		__field( unsigned int, boost_freq )
		__array( char, label, LABEL_LEN )
	),

	TP_fast_assign(
		__entry->boost_freq = boost_freq;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("%s: boost_freq=%u", __entry->label, __entry->boost_freq)
);

TRACE_EVENT(pago_work,

	TP_PROTO(unsigned int target_freq, unsigned int boost_freq, unsigned int resolved_freq,
		const char *label),

	TP_ARGS(target_freq, boost_freq, resolved_freq, label),

	TP_STRUCT__entry(
		__field( unsigned int, target_freq )
		__field( unsigned int, boost_freq )
		__field( unsigned int, resolved_freq )
		__array( char, label, LABEL_LEN )
	),

	TP_fast_assign(
		__entry->target_freq = target_freq;
		__entry->boost_freq = boost_freq;
		__entry->resolved_freq = resolved_freq;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("%s: target_freq=%u boost_freq=%u resolved_freq=%u",
		__entry->label, __entry->target_freq, __entry->boost_freq, __entry->resolved_freq)
);

TRACE_EVENT(pago_cme_prio,

	TP_PROTO(int cpu, unsigned int cme_prio_lvl, unsigned int cme_prio_lvl_backup,
		const char *label),

	TP_ARGS(cpu, cme_prio_lvl, cme_prio_lvl_backup, label),

	TP_STRUCT__entry(
		__field( int, cpu )
		__field( unsigned int, cme_prio_lvl )
		__field( unsigned int, cme_prio_lvl_backup )
		__array( char, label, LABEL_LEN )
	),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->cme_prio_lvl = cme_prio_lvl;
		__entry->cme_prio_lvl_backup = cme_prio_lvl_backup;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("%s: cpu=%d cme_prio_lvl=%u cme_prio_lvl_backup=%u",
		__entry->label, __entry->cpu, __entry->cme_prio_lvl, __entry->cme_prio_lvl_backup)
);

TRACE_EVENT(pago_shift_level,

	TP_PROTO(unsigned int shift_level, const char *label),

	TP_ARGS(shift_level, label),

	TP_STRUCT__entry(
		__field( unsigned int, shift_level )
		__array( char, label, LABEL_LEN )
	),

	TP_fast_assign(
		__entry->shift_level = shift_level;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("%s: shift_level=%u", __entry->label, __entry->shift_level)
);

/*
 * Tracepoint for ems taskgroup
 */
TRACE_EVENT(ems_update_taskgroup,

	TP_PROTO(struct task_struct *p, int group),

	TP_ARGS(p, group),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		group			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		__entry->group = group
	),

	TP_printk("comm=%s pid=%d taskgroup=%d",
		__entry->comm, __entry->pid, __entry->group)
);

TRACE_EVENT(ems_update_uclamp_min,

	TP_PROTO(int group, unsigned int uclamp_min),

	TP_ARGS(group, uclamp_min),

	TP_STRUCT__entry(
		__field(	int,		group			)
		__field(	unsigned int,	uclamp_min		)
	),

	TP_fast_assign(
		__entry->group = group;
		__entry->uclamp_min = uclamp_min
	),

	TP_printk("taskgroup=%d uclamp_min=%u",
		__entry->group, __entry->uclamp_min)
);

TRACE_EVENT(ems_update_uclamp_max,

	TP_PROTO(int group, unsigned int uclamp_max),

	TP_ARGS(group, uclamp_max),

	TP_STRUCT__entry(
		__field(	int,		group			)
		__field(	unsigned int,	uclamp_max		)
	),

	TP_fast_assign(
		__entry->group = group;
		__entry->uclamp_max = uclamp_max
	),

	TP_printk("taskgroup=%d uclamp_max=%u",
		__entry->group, __entry->uclamp_max)
);

TRACE_EVENT(ems_update_pl_request,

	TP_PROTO(int next, int prev),

	TP_ARGS(next, prev),

	TP_STRUCT__entry(
		__field(	int,		next			)
		__field(	int,		prev			)
	),

	TP_fast_assign(
		__entry->next = next;
		__entry->prev = prev;
	),

	TP_printk("next=%d prev=%d",
		__entry->next, __entry->prev)
);

/*
 * Tracepoint for dslt_util_est_update
 */
TRACE_EVENT(dslt_util_est_update,

	TP_PROTO(struct task_struct *p, unsigned int last_ewma, unsigned int ewma, unsigned int dequeued, int shift),

	TP_ARGS(p, last_ewma, ewma, dequeued, shift),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cgroup_idx		)
		__field(	unsigned long,	util_avg		)
		__field(	unsigned long,	runnable_avg		)
		__field(	unsigned int,	dequeued		)
		__field(	unsigned int,	last_ewma		)
		__field(	unsigned int,	ewma			)
		__field(	int,		shift			)
		__field(	int,		last_ewma_diff		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		__entry->cgroup_idx = cpuctl_task_group_idx(p);
		__entry->util_avg = p->se.avg.util_avg;
		__entry->runnable_avg = ems_dslt_task(p)->avg.runnable_avg;
		__entry->dequeued = dequeued;
		__entry->last_ewma = last_ewma;
		__entry->ewma = ewma;
		__entry->shift = shift;
		__entry->last_ewma_diff = last_ewma - dequeued;
	),

	TP_printk("comm=%s pid=%d cgroup_idx=%d util=%lu runnable=%lu "
			"dequeued=%u last_ewma=%u ewma=%u shift=%d last_ewma_diff=%d",
		__entry->comm, __entry->pid, __entry->cgroup_idx, __entry->util_avg,
		__entry->runnable_avg, __entry->dequeued, __entry->last_ewma,
		__entry->ewma, __entry->shift, __entry->last_ewma_diff)
);

TRACE_EVENT(emstune_set_ta_binding,

	TP_PROTO(struct cpumask *mask),

	TP_ARGS(mask),

	TP_STRUCT__entry(
		__field(	unsigned int,	override_ta_binding	)
	),

	TP_fast_assign(
		__entry->override_ta_binding = (mask == NULL ? 0 : *(unsigned int *)cpumask_bits(mask));
	),

	TP_printk("override_ta_binding=0x%03x", __entry->override_ta_binding)
);

TRACE_EVENT(profile_sustain_high,

	TP_PROTO(struct task_struct *p, unsigned long util, unsigned long sustain_high_cap,
			unsigned char sustain_high_hist),

	TP_ARGS(p, util, sustain_high_cap, sustain_high_hist),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	util			)
		__field(	unsigned long,	sustain_high_cap	)
		__field(	unsigned char,	sustain_high_hist	)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		__entry->util = util;
		__entry->sustain_high_cap = sustain_high_cap;
		__entry->sustain_high_hist = sustain_high_hist;
	),

	TP_printk("comm=%s pid=%d util=%lu sustain_high_cap=%lu sustain_high_hist=%u",
		__entry->comm, __entry->pid, __entry->util, __entry->sustain_high_cap,
		__entry->sustain_high_hist)
);

TRACE_EVENT(adap_fmax_reason,

	TP_PROTO(int reason),

	TP_ARGS(reason),

	TP_STRUCT__entry(
		__field(int,	reason			)
	),

	TP_fast_assign(
		__entry->reason = reason;
	),

	TP_printk("reason=%d", __entry->reason)
);

TRACE_EVENT(task_profiler_handler,

	TP_PROTO(struct task_struct *p, char *label),

	TP_ARGS(p, label),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cgroup			)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		__entry->cgroup = cpuctl_task_group_idx(p);
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("comm=%s(%d), cgroup=%d, label=%s",
		__entry->comm, __entry->pid, __entry->cgroup, __entry->label)
);

TRACE_EVENT(task_profiler_major_task_snapshot,

	TP_PROTO(struct task_struct *p, unsigned long hist_util, u64 hist_runnable, u64 hist_window,
		int prolonged_running),

	TP_ARGS(p, hist_util, hist_runnable, hist_window, prolonged_running),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	hist_util		)
		__field(	unsigned long,	util_avg		)
		__field(	u64,		hist_runnable		)
		__field(	u64,		hist_window		)
		__field(	int,		prolonged_running	)
		__field(	int,		major_count		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		__entry->hist_util = hist_util;
		__entry->util_avg = ml_task_util(p);
		__entry->hist_runnable = hist_runnable / NSEC_PER_MSEC;
		__entry->hist_window = hist_window / NSEC_PER_MSEC;
		__entry->prolonged_running = prolonged_running;
		__entry->major_count = egg_major_count(p);
	),

	TP_printk("comm=%s pid=%d hist_util=%lu util_avg=%lu hist_runnable=%llu hist_window=%llu "
		"prolonged_running=%d major_count=%d",
		__entry->comm, __entry->pid, __entry->hist_util, __entry->util_avg, __entry->hist_runnable,
		__entry->hist_window, __entry->prolonged_running, __entry->major_count)
);

TRACE_EVENT(egg_update_major_tasks,

	TP_PROTO(int index, struct egg_task_struct *ets),

	TP_ARGS(index, ets),

	TP_STRUCT__entry(
		__field(	int,		index			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned int,	cpus_ptr		)
		__field(	unsigned long,	hist_util		)
		__field(	unsigned long,	pelt_util		)
		__field(	int,		prolonged_running	)
		__field(	int,		major_count		)
		__field(	int,		over_running		)
		__field(	int,		near_heaviest		)
		__field(	int,		training_task		)
		__field(	int,		training_active		)
		__field(	int,		training_count		)
		__field(	u64,		hist_runnable		)
		__field(	u64,		hist_window		)
		__array(	u64,		hist_ipc,	MAX_NR_CLUSTERS	)
		__array(	u64,		recent_ipc,	MAX_NR_CLUSTERS	)
		__array(	u64,		min_ipc,	MAX_NR_CLUSTERS	)
	),

	TP_fast_assign(
		int i;
		__entry->index = index + 1;
		memcpy(__entry->comm, ets->comm, TASK_COMM_LEN);
		__entry->pid = ets->pid;
		__entry->cpus_ptr = *(unsigned int *)cpumask_bits(&ets->cpus_ptr);
		__entry->hist_util = ets->hist_util;
		__entry->pelt_util = ets->pelt_util;
		__entry->prolonged_running = ets->prolonged_running;
		__entry->major_count = egg_major_count(ets->task);
		__entry->over_running = ets->over_running;
		__entry->near_heaviest = ets->near_heaviest;
		__entry->training_task = ets->training.task ? 1 : 0;
		__entry->training_active = ets->training.active;
		__entry->training_count = ets->training.count[FAST_CPU];
		__entry->hist_runnable = ets->hist_runnable / NSEC_PER_MSEC;
		__entry->hist_window = ets->hist_window / NSEC_PER_MSEC;
		for (i = 0; i < MAX_NR_CLUSTERS; i++) {
			__entry->hist_ipc[i] = ets->training.hist_uarch[i][0];
			__entry->recent_ipc[i] = ets->training.recent_uarch[i][0];
			__entry->min_ipc[i] = ets->training.min_uarch[i][0];
		}
	),

	TP_printk("TOP%d, comm=%s, pid=%d, cpus_ptr=0x%03x, major_count=%d, prolonged_running=%d, "
		"over_running=%d, hist_runnable=%llums, hist_window=%llums, hist_util=%lu, pelt_util=%lu, "
		"near_heaviest=%d, training_task=%d, training_active=%d, training_count=%d, "
		"IPC_hist(%llu, %llu, %llu, %llu), IPC_recent(%llu, %llu, %llu, %llu) "
		"IPC_min(%llu, %llu, %llu, %llu)",
		__entry->index, __entry->comm, __entry->pid, __entry->cpus_ptr, __entry->major_count,
		__entry->prolonged_running, __entry->over_running, __entry->hist_runnable,
		__entry->hist_window, __entry->hist_util, __entry->pelt_util, __entry->near_heaviest,
		__entry->training_task, __entry->training_active, __entry->training_count,
		__entry->hist_ipc[0], __entry->hist_ipc[1], __entry->hist_ipc[2],
		__entry->hist_ipc[3], __entry->recent_ipc[0], __entry->recent_ipc[1],
		__entry->recent_ipc[2], __entry->recent_ipc[3], __entry->min_ipc[0],
		__entry->min_ipc[1], __entry->min_ipc[2], __entry->min_ipc[3]
		)
);

TRACE_EVENT(egg_update_major_tasks_total_util,

	TP_PROTO(int nr_major, int nr_over_running, int nr_near_heaviest, int nr_training,
		unsigned long heavy_util_thresh, unsigned long total_hist_util,
		unsigned long total_pelt_util),

	TP_ARGS(nr_major, nr_over_running, nr_near_heaviest, nr_training, heavy_util_thresh,
		total_hist_util, total_pelt_util),

	TP_STRUCT__entry(
		__field(	int,		nr_major		)
		__field(	int,		nr_over_running		)
		__field(	int,		nr_near_heaviest	)
		__field(	int,		nr_training		)
		__field(	unsigned long,	heavy_util_thresh	)
		__field(	unsigned long,	total_hist_util		)
		__field(	unsigned long,	total_pelt_util		)
	),

	TP_fast_assign(
		__entry->nr_major = nr_major;
		__entry->nr_over_running = nr_over_running;
		__entry->nr_near_heaviest = nr_near_heaviest;
		__entry->nr_training = nr_training;
		__entry->heavy_util_thresh = heavy_util_thresh;
		__entry->total_hist_util = total_hist_util;
		__entry->total_pelt_util = total_pelt_util;
	),

	TP_printk("nr_major=%d, nr_over_running=%d, nr_near_heaviest=%d, nr_training=%d, "
		"heavy_util_thresh=%lu, total_hist_util=%lu, total_pelt_util=%lu",
		__entry->nr_major, __entry->nr_over_running, __entry->nr_near_heaviest,
		__entry->nr_training, __entry->heavy_util_thresh,
		__entry->total_hist_util, __entry->total_pelt_util)
);

TRACE_EVENT(task_profiler_runnable_history,

	TP_PROTO(struct task_profiler_struct *tps),

	TP_ARGS(tps),

	TP_STRUCT__entry(
		__array(	char,		comm,		TASK_COMM_LEN	)
		__field(	pid_t,		pid				)
		__array(	u64,		rnb_hist,	MAX_NR_HISTORY	)
		__array(	u64,		win_hist,	MAX_NR_HISTORY	)
		__field(	u64,		hist_runnable			)
		__field(	u64,		hist_window			)
		__field(	int,		prolonged_running		)
	),

	TP_fast_assign(
		struct task_profiler_runnable *runnable = &tps->runnable;
		int i, idx = runnable->latest_idx;
		for (i = 0; i < MAX_NR_HISTORY; i++) {
			__entry->rnb_hist[i] = runnable->history[idx].runnable / NSEC_PER_MSEC;
			__entry->win_hist[i] = runnable->history[idx].window_size / NSEC_PER_MSEC;
			idx--;
			if (idx < 0)
				idx = MAX_NR_HISTORY - 1;
		}
		memcpy(__entry->comm, tps->p->comm, TASK_COMM_LEN);
		__entry->pid = tps->p->pid;
		__entry->hist_runnable = runnable->hist_runnable;
		__entry->hist_window = runnable->hist_window;
		__entry->prolonged_running = runnable->prolonged_running;
	),

	TP_printk("comm=%s(%d) hist_runnable=%llu, hist_window=%llu, prolonged_running=%d, "
		"history={%llu(%llums), %llu(%llums), %llu(%llums), %llu(%llums), %llu(%llums), "
		"%llu(%llums), %llu(%llums), %llu(%llums), %llu(%llums), %llu(%llums)}",
		__entry->comm, __entry->pid, __entry->hist_runnable, __entry->hist_window,
		__entry->prolonged_running,
		__entry->rnb_hist[0], __entry->win_hist[0],
		__entry->rnb_hist[1], __entry->win_hist[1],
		__entry->rnb_hist[2], __entry->win_hist[2],
		__entry->rnb_hist[3], __entry->win_hist[3],
		__entry->rnb_hist[4], __entry->win_hist[4],
		__entry->rnb_hist[5], __entry->win_hist[5],
		__entry->rnb_hist[6], __entry->win_hist[6],
		__entry->rnb_hist[7], __entry->win_hist[7],
		__entry->rnb_hist[8], __entry->win_hist[8],
		__entry->rnb_hist[9], __entry->win_hist[9])
);

TRACE_EVENT(freq_profiler_update,

	TP_PROTO(int cl, int avg_cur_freq, int avg_max_freq, int orig_max_freq),

	TP_ARGS(cl, avg_cur_freq, avg_max_freq, orig_max_freq),

	TP_STRUCT__entry(
		__field(	int,		cl			)
		__field(	int,		avg_cur_freq			)
		__field(	int,		avg_max_freq			)
		__field(	int,		orig_max_freq			)
	),

	TP_fast_assign(
		__entry->cl = cl;
		__entry->avg_cur_freq = avg_cur_freq;
		__entry->avg_max_freq = avg_max_freq;
		__entry->orig_max_freq = orig_max_freq;
	),

	TP_printk("cluster=%d avg_cur_freq=%d avg_max_freq=%d orig_max_freq=%d",
		__entry->cl, __entry->avg_cur_freq, __entry->avg_max_freq, __entry->orig_max_freq)
);

TRACE_EVENT(egg_update_reserved_cpu,

	TP_PROTO(struct egg_task_struct *p),

	TP_ARGS(p),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		prev_reserved_cpu	)
		__field(	int,		reserved_cpu		)
		__field(	int,		misfit			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		__entry->prev_reserved_cpu = p->prev_reserved_cpu;
		__entry->reserved_cpu = p->reserved_cpu;
		__entry->misfit = p->misfit;
	),

	TP_printk("prev_reserved_cpu=%d reserved_cpu=%d comm=%s pid=%d misfit=%d",
		__entry->prev_reserved_cpu, __entry->reserved_cpu,
		__entry->comm, __entry->pid, __entry->misfit)
);

TRACE_EVENT(egg_update_reserved_cpus,

	TP_PROTO(struct egg_task_struct *p),

	TP_ARGS(p),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	hist_util		)
		__field(	int,		reserved_cpu		)
		__field(	int,		misfit			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
		__entry->hist_util = p->hist_util;
		__entry->reserved_cpu = p->reserved_cpu;
		__entry->misfit = p->misfit;
	),

	TP_printk("reserved_cpu=%d, comm=%s pid=%d, hist_util=%lu, misfit=%d",
		__entry->reserved_cpu, __entry->comm, __entry->pid, __entry->hist_util, __entry->misfit)
);

TRACE_EVENT(cleave_heavy_task,

	TP_PROTO(struct cleave_task *ct, unsigned long total_util, int nr_cleave),

	TP_ARGS(ct, total_util, nr_cleave),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	util			)
		__field(	unsigned long,	total_util		)
		__field(	int,		nr_cleave		)
	),

	TP_fast_assign(
		strncpy(__entry->comm, ct->ets->comm, TASK_COMM_LEN);
		__entry->pid = ct->ets->pid;
		__entry->util = ct->util;
		__entry->total_util = total_util;
		__entry->nr_cleave = nr_cleave;
	),

	TP_printk("comm=%s pid=%d util=%lu, total_util=%lu, nr_cleave=%d",
		__entry->comm, __entry->pid, __entry->util, __entry->total_util,
		__entry->nr_cleave)
);

TRACE_EVENT(cleave_setup_util_env,

	TP_PROTO(int cpu, unsigned long base_util_orig, unsigned long base_util,
		struct egg_task_struct *ets, unsigned long task_util),

	TP_ARGS(cpu, base_util_orig, base_util, ets, task_util),

	TP_STRUCT__entry(
		__field(        int,            cpu                     )
		__field(        unsigned long,  base_util_orig          )
		__field(        unsigned long,  base_util               )
		__array(        char,           comm,   TASK_COMM_LEN   )
		__field(        pid_t,          pid                     )
		__field(        unsigned long,  task_util               )
		__field(        int,            prev                    )
	),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->base_util_orig = base_util_orig;
		__entry->base_util = base_util;
		memcpy(__entry->comm, ets ? ets->comm : "NULL", TASK_COMM_LEN);
		__entry->pid = ets ? ets->pid : -1;
		__entry->task_util = task_util;
		__entry->prev = ets ? ets->reserved_cpu : -1;
	),

	TP_printk("cpu=%d, base_util_orig=%lu, base_util=%lu, "
		"comm=%s, pid=%d, task_util=%lu, prev=%d",
		__entry->cpu, __entry->base_util_orig, __entry->base_util,
		__entry->comm, __entry->pid, __entry->task_util, __entry->prev)
);

TRACE_EVENT(cleave_setup_available_cpus_env,

	TP_PROTO(struct cleave_task *ct, const char *reason),

	TP_ARGS(ct, reason),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		util			)
		__field(	int,		misfit			)
		__field(	int,		fast_cnt		)
		__field(	int,		slow_cnt		)
		__field(	unsigned int,	raw			)
		__field(	unsigned int,	available		)
		__array(	char,		reason,	LABEL_LEN)
	),

	TP_fast_assign(
		memcpy(__entry->comm, ct->ets->comm, TASK_COMM_LEN);
		__entry->pid			= ct->ets->pid;
		__entry->util			= ct->util;
		__entry->misfit			= ct->misfit;
		__entry->fast_cnt		= ct->ets->training.count[FAST_CPU];
		__entry->slow_cnt		= ct->ets->training.count[SLOW_CPU];
		__entry->available		= *(unsigned int *)cpumask_bits(&ct->available_cpus);
		strncpy(__entry->reason, reason, LABEL_LEN - 1);
	),

	TP_printk("comm=%15s, pid=%6d, util=%d misfit=%d train_cnt=(%d,%d) available=0x%03x reason=%s",
		__entry->comm, __entry->pid, __entry->util, __entry->misfit,
		__entry->fast_cnt, __entry->slow_cnt,
		__entry->available, __entry->reason)
);

TRACE_EVENT(cleave_snapshot,

	TP_PROTO(int cpu, unsigned long util, struct egg_task_struct *ets),

	TP_ARGS(cpu, util, ets),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	util			)
	),

	TP_fast_assign(
		__entry->cpu		= cpu;
		memcpy(__entry->comm, 	ets ? ets->comm : "NULL", TASK_COMM_LEN);
		__entry->pid		= ets ? ets->pid : -1;
		__entry->util		= util;
	),

	TP_printk("cpu=%d comm=%15s, pid=%6d, util=%lu",
		__entry->cpu, __entry->comm, __entry->pid, __entry->util)
);


TRACE_EVENT(cleave_energy_cpu,

	TP_PROTO(struct cleave_task *ct),

	TP_ARGS(ct),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		prev_cpu		)
		__field(	int,		energy_cpu		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, ct->ets->comm, TASK_COMM_LEN);
		__entry->pid			= ct->ets->pid;
		__entry->prev_cpu		= ct->ets->reserved_cpu;
		__entry->energy_cpu		= ct->cpu;
	),

	TP_printk("comm=%15s, pid=%6d, prev_cpu=%1d, energy_cpu=%1d",
		__entry->comm, __entry->pid, __entry->prev_cpu, __entry->energy_cpu)
);

TRACE_EVENT(cleave_compute_system_energy,

	TP_PROTO(int cnt, struct cleave_task *ct, int nr_tasks, int energy),

	TP_ARGS(cnt, ct, nr_tasks, energy),

	TP_STRUCT__entry(
		__array(	char,		t0,	TASK_COMM_LEN	)
		__array(	char,		t1,	TASK_COMM_LEN	)
		__array(	char,		t2,	TASK_COMM_LEN	)
		__array(	int,		cpu,	5		)
		__field(	int,		energy			)
	),

	TP_fast_assign(
		int i;
		__entry->energy	= energy;
		for (i = 0; i < nr_tasks; i++) {
			if (i == 0)
				memcpy(__entry->t0, ct[i].ets ? (ct[i].ets->comm) : "NULL", TASK_COMM_LEN);
			else if (i == 1)
				memcpy(__entry->t1, ct[i].ets ? (ct[i].ets->comm) : "NULL", TASK_COMM_LEN);
			else if (i == 2)
				memcpy(__entry->t2, ct[i].ets ? (ct[i].ets->comm) : "NULL", TASK_COMM_LEN);

			__entry->cpu[i] = ct[i].visiting_cpu;
		}
	),

	TP_printk("cpu%d(%s) cpu%d(%s) cpu%d(%s) energy=%d",
		 __entry->cpu[0], __entry->t0,
		 __entry->cpu[1], __entry->t1,
		 __entry->cpu[2], __entry->t2,
		 __entry->energy)
);

TRACE_EVENT(cleave_setup_execution_clv_task,

	TP_PROTO(struct cleave_task *ct, int util_reduction),

	TP_ARGS(ct, util_reduction),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		slow_ipc		)
		__field(	int,		fast_ipc		)
		__field(	int,		slow_util		)
		__field(	int,		fast_util		)
		__field(	int,		util_reduction		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, ct->ets->comm, TASK_COMM_LEN);
		__entry->pid			= ct->ets->pid;
		__entry->slow_ipc		= ct->ets->training.hist_uarch[exec_slow_cl_idx][UARCH_IPC];
		__entry->fast_ipc		= ct->ets->training.hist_uarch[exec_fast_cl_idx][UARCH_IPC];
		__entry->slow_util		= ct->ets->training.hist_util[exec_slow_cl_idx];
		__entry->fast_util		= ct->ets->training.hist_util[exec_fast_cl_idx];
		__entry->util_reduction		= util_reduction;
	),

	TP_printk("comm=%15s, pid=%6d, slow_ipc=%d, fast_ipc=%d slow_util=%d fast_util=%d util_reduction=%d",
		__entry->comm, __entry->pid, __entry->slow_ipc, __entry->fast_ipc,
		__entry->slow_util, __entry->fast_util, __entry->util_reduction)
);

TRACE_EVENT(cleave_set_execution_state,

	TP_PROTO(const char *state),

	TP_ARGS(state),

	TP_STRUCT__entry(
		__array(	char,		state,	LABEL_LEN	)
	),

	TP_fast_assign(
		strncpy(__entry->state, state, LABEL_LEN - 1);
	),

	TP_printk("execution state=%s",
		__entry->state)
);

TRACE_EVENT(egg_determine_sub_mode,

	TP_PROTO(int prev_mode, int next_mode, u64 delta),

	TP_ARGS(prev_mode, next_mode, delta),

	TP_STRUCT__entry(
		__field(	int,		prev_mode	)
		__field(	int,		next_mode	)
		__field(	u64,		delta		)
	),

	TP_fast_assign(
		__entry->prev_mode = prev_mode;
		__entry->next_mode = next_mode;
		__entry->delta = delta;
	),

	TP_printk("prev=%d, next=%d, delta=%llu",
		__entry->prev_mode, __entry->next_mode,  __entry->delta
	)
);

TRACE_EVENT(egg_update_op_mode,

	TP_PROTO(int nxt_mode, int sub_mode, int egg_stopped, int fd, int thermal_lv, bool freq_throttle,
		bool mfreq_throttle, bool ou_task, int demand_cpu, int fps, int target, int *temp),

	TP_ARGS(nxt_mode, sub_mode, egg_stopped, fd, thermal_lv, freq_throttle, mfreq_throttle, ou_task,
		demand_cpu, fps, target, temp),

	TP_STRUCT__entry(
		__field(	int,		nxt_mode	)
		__field(	int,		sub_mode	)
		__field(	int,		egg_stopped	)
		__field(	int,		fd		)
		__field(	int,		thermal_lv	)
		__field(	bool,		freq_throttle	)
		__field(	bool,		mfreq_throttle	)
		__field(	bool,		ou_task		)
		__field(	int,		demand_cpu	)
		__field(	int,		fps		)
		__field(	int,		target		)
		__array(	int,		temp,	4	)
	),

	TP_fast_assign(
		int i;
		__entry->nxt_mode = nxt_mode;
		__entry->sub_mode = sub_mode;
		__entry->egg_stopped = egg_stopped;
		__entry->fd = fd;
		__entry->thermal_lv = thermal_lv;
		__entry->freq_throttle	= freq_throttle;
		__entry->mfreq_throttle	= mfreq_throttle;
		__entry->ou_task = ou_task;
		__entry->demand_cpu = demand_cpu;
		__entry->fps = fps;
		__entry->target = target;
		for (i = 0; i < 4; i++) {
			__entry->temp[i]	= temp[i];
		};
	),

	TP_printk("mode=%d sub_mode=%d egg_stopped=%d fd=%d tlv=%d freq_trl=%d mfreq_trl=%d "
			"ou_task=%d demand_cpu=%d fps=%d target=%d temp=(%d %d %d %d)",
		__entry->nxt_mode, __entry->sub_mode, __entry->egg_stopped,  __entry->fd,
		__entry->thermal_lv, __entry->freq_throttle, __entry->mfreq_throttle,
		__entry->ou_task, __entry->demand_cpu, __entry->fps, __entry->target,
		__entry->temp[0], __entry->temp[1], __entry->temp[2], __entry->temp[3]
	)
);

TRACE_EVENT(egg_update_big_boost,

	TP_PROTO(bool enabled, u64 expired_ms, struct cpumask *reserved_cpus, int pid,  const char *tag),

	TP_ARGS(enabled, expired_ms, reserved_cpus, pid, tag),

	TP_STRUCT__entry(
		__field(	bool,		enabled			)
		__field(	u64,		expired_ms		)
		__field(	int,		pid			)
		__field(	int,		reserved_cpus		)
		__array(	char,		tag,	TASK_COMM_LEN	)
	),

	TP_fast_assign(
		__entry->enabled 	= enabled;
		__entry->expired_ms 	= expired_ms / NSEC_PER_MSEC;
		__entry->pid		= pid;
		__entry->reserved_cpus	=  *(unsigned int *)cpumask_bits(reserved_cpus);
		memcpy(__entry->tag, tag, TASK_COMM_LEN);
	),

	TP_printk("enabled=%d expired_ms=%llu pid=%d reserved_cpus=%03x comm=%s",
		__entry->enabled, __entry->expired_ms, __entry->pid,
		__entry->reserved_cpus, __entry->tag)
);

TRACE_EVENT(egg_update_dsu_boost,

	TP_PROTO(bool enabled, u64 expired_ms, const char *tag),

	TP_ARGS(enabled, expired_ms, tag),

	TP_STRUCT__entry(
		__field(	bool,		enabled)
		__field(	u64,		expired_ms)
		__array(	char,		tag,	TASK_COMM_LEN	)
	),

	TP_fast_assign(
		memcpy(__entry->tag, tag, TASK_COMM_LEN);
		__entry->expired_ms = expired_ms / NSEC_PER_MSEC;
		__entry->enabled = enabled;
	),

	TP_printk("enabled=%d expired_ms=%llu comm=%s",
		__entry->enabled, __entry->expired_ms, __entry->tag)
);

TRACE_EVENT(egg_update_mif_boost,

	TP_PROTO(bool enabled, u64 expired_ms, const char *tag),

	TP_ARGS(enabled, expired_ms, tag),

	TP_STRUCT__entry(
		__field(	bool,		enabled)
		__field(	u64,		expired_ms)
		__array(	char,		tag,	TASK_COMM_LEN	)
	),

	TP_fast_assign(
		memcpy(__entry->tag, tag, TASK_COMM_LEN);
		__entry->expired_ms = expired_ms / NSEC_PER_MSEC;
		__entry->enabled = enabled;
	),

	TP_printk("enabled=%d expired_ms=%llu comm=%s",
		__entry->enabled, __entry->expired_ms, __entry->tag)
);

TRACE_EVENT(ems_maverick,

	TP_PROTO(struct task_struct *p, int cpu, int mav_cnt, const char *state),

	TP_ARGS(p, cpu, mav_cnt, state),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cpu			)
		__field(	int,		mav_cnt			)
		__array(	char,		state,	LABEL_LEN	)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->cpu			= cpu;
		__entry->mav_cnt		= mav_cnt;
		strncpy(__entry->state, state, LABEL_LEN - 1);
	),

	TP_printk("comm=%15s, pid=%6d, cpu=%d, mav_cnt=%d, state=%s",
		__entry->comm, __entry->pid, __entry->cpu, __entry->mav_cnt, __entry->state)
);

TRACE_EVENT(dct_update_cluster,

	TP_PROTO(int cl, int idx, int need_sum, int need_num),

	TP_ARGS(cl, idx, need_sum, need_num),

	TP_STRUCT__entry(
		__field(	int,		cl			)
		__field(	int,		idx			)
		__field(	int,		need_sum		)
		__field(	int,		need_num		)
	),

	TP_fast_assign(
		__entry->cl			= cl;
		__entry->idx			= idx;
		__entry->need_sum		= need_sum;
		__entry->need_num		= need_num;
	),

	TP_printk("cl=%d idx=%d need_sum=%d need_num=%d",
		__entry->cl, __entry->idx,__entry->need_sum, __entry->need_num)
);

TRACE_EVENT(dct_update,

	TP_PROTO(struct dct_cl_load *loads, bool big_boost,
			struct cpumask *maverick_cpus, struct cpumask *need_cpus,
			struct cpumask *busy_cpus, struct cpumask *fa_cpus,
			struct cpumask *reserved_cpus, struct cpumask *feedback_cpus,
			struct cpumask *max_cpus, struct cpumask *req_cpus, const char *tag),

	TP_ARGS(loads, big_boost, maverick_cpus, need_cpus, busy_cpus, fa_cpus,
			reserved_cpus, feedback_cpus, max_cpus, req_cpus, tag),

	TP_STRUCT__entry(
		__array(	int,		demand_cpu, NUM_OF_MAX_CL	)
		__field(	bool,		big_boost			)
		__field(	int,		maverick_cpus			)
		__field(	int,		need_cpus			)
		__field(	int,		busy_cpus			)
		__field(	int,		fa_cpus				)
		__field(	int,		reserved_cpus			)
		__field(	int,		feedback_cpus			)
		__field(	int,		max_cpus			)
		__field(	int,		req_cpus			)
		__array(	char,		tag,		LABEL_LEN	)
	),

	TP_fast_assign(
		int i;
		if (loads) {
			for (i = 0; i < NUM_OF_MAX_CL; i++)
				__entry->demand_cpu[i]	= loads[i].demand_cpu;
		} else {
			for (i = 0; i < NUM_OF_MAX_CL; i++)
				__entry->demand_cpu[i]	= -1;
		}
		__entry->big_boost		=  big_boost;
		__entry->maverick_cpus		=  *(unsigned int *)cpumask_bits(maverick_cpus);
		__entry->need_cpus		=  *(unsigned int *)cpumask_bits(need_cpus);
		__entry->busy_cpus		=  *(unsigned int *)cpumask_bits(busy_cpus);
		__entry->fa_cpus		=  *(unsigned int *)cpumask_bits(fa_cpus);
		__entry->reserved_cpus		=  *(unsigned int *)cpumask_bits(reserved_cpus);
		__entry->feedback_cpus		=  *(unsigned int *)cpumask_bits(feedback_cpus);
		__entry->max_cpus		=  *(unsigned int *)cpumask_bits(max_cpus);
		__entry->req_cpus		=  *(unsigned int *)cpumask_bits(req_cpus);
		strncpy(__entry->tag, tag, LABEL_LEN - 1);
	),

	TP_printk("demand=(%d, %d, %d, %d) big_bst=%d mvr=%03x need=%03x busy=%03x"
			" fa=%03x reserved=%03x fbk=%03x max=0x%03x req=%03x, tag=%s",
		__entry->demand_cpu[0], __entry->demand_cpu[1],
		__entry->demand_cpu[2], __entry->demand_cpu[3],
		__entry->big_boost,  __entry->maverick_cpus, __entry->need_cpus,
		__entry->busy_cpus, __entry->fa_cpus,
		__entry->reserved_cpus, __entry->feedback_cpus, __entry->max_cpus,
		__entry->req_cpus, __entry->tag)
);


TRACE_EVENT(dct_feedback,

	TP_PROTO(int cl, int idx, int max_util, int util_sum, int num_by_util,
		int max_runnable, int runnable_sum, int runnable_margin, int num_by_runnable,
		int demand_cpu),

	TP_ARGS(cl, idx, max_util, util_sum, num_by_util, max_runnable, runnable_sum,
			runnable_margin, num_by_runnable, demand_cpu),

	TP_STRUCT__entry(
		__field(	int,		cl			)
		__field(	int,		idx			)
		__field(	int,		max_util		)
		__field(	int,		util_sum		)
		__field(	int,		num_by_util		)
		__field(	int,		max_runnable		)
		__field(	int,		runnable_sum		)
		__field(	int,		runnable_margin		)
		__field(	int,		num_by_runnable		)
		__field(	int,		demand_cpu)
	),

	TP_fast_assign(
		__entry->cl			= cl;
		__entry->idx			= idx;
		__entry->max_util		= max_util;
		__entry->util_sum		= util_sum;
		__entry->num_by_util		= num_by_util;
		__entry->max_runnable		= max_runnable;
		__entry->runnable_sum		= runnable_sum;
		__entry->runnable_margin	= runnable_margin;
		__entry->num_by_runnable	= num_by_runnable;
		__entry->demand_cpu		= demand_cpu;
	),

	TP_printk("cl=%d idx=%d mx_ut=%d ut_sm=%d ut_num=%d mx_run=%d run_sm=%d run_mrgn=%d run_num=%d demand_cpu=%d",
		__entry->cl, __entry->idx, __entry->max_util, __entry->util_sum,
		__entry->num_by_util, __entry->max_runnable, __entry->runnable_sum,
		__entry->runnable_margin, __entry->num_by_runnable, __entry->demand_cpu)
);


TRACE_EVENT(dct_throttle_fasted_cpu,

	TP_PROTO(int overutil, int freq_throttle, int max_freq_throttle),

	TP_ARGS(overutil, freq_throttle, max_freq_throttle),

	TP_STRUCT__entry(
		__field(	int,		overutil			)
		__field(	int,		freq_throttle			)
		__field(	int,		max_freq_throttle			)
	),

	TP_fast_assign(
		__entry->overutil = overutil;
		__entry->freq_throttle = freq_throttle;
		__entry->max_freq_throttle = max_freq_throttle;
	),

	TP_printk("overutil=%d freq_throttle=%d max_freq_throttle=%d",
		__entry->overutil, __entry->freq_throttle, __entry->max_freq_throttle)
);

TRACE_EVENT(update_cpu_hist_contrib,

	TP_PROTO(int cpu, u64 util_contrib, u64 contrib, u64 now,
		const char *label),

	TP_ARGS(cpu, util_contrib, contrib, now, label),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		hist_util_temp		)
		__field(	u64,		util_contrib		)
		__field(	u64,		contrib			)
		__field(	u64,		now			)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->hist_util_temp		= contrib ? util_contrib / contrib : 0;
		__entry->util_contrib		= util_contrib;
		__entry->contrib		= contrib;
		__entry->now			= now;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("cpu=%d, hist_util_temp=%d, util_contrib=%llu, contrib=%llu, now=%llu, flag=%s",
		__entry->cpu, __entry->hist_util_temp, __entry->util_contrib, __entry->contrib,
		__entry->now, __entry->label)
);

TRACE_EVENT(update_cpu_hist_util,

	TP_PROTO(int cpu, int hist_util, u64 now),

	TP_ARGS(cpu, hist_util, now),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	int,		hist_util		)
		__field(	u64,		now			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->hist_util		= hist_util;
		__entry->now			= now;
	),

	TP_printk("cpu=%d, hist_util=%d, now=%llu",
		__entry->cpu, __entry->hist_util, __entry->now)
);

TRACE_EVENT(update_task_hist_contrib,

	TP_PROTO(int cpu, struct task_struct *p, u64 util_contrib, u64 contrib,
		u64 now, const char *label),

	TP_ARGS(cpu, p, util_contrib, contrib, now, label),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		hist_util_temp		)
		__field(	u64,		util_contrib		)
		__field(	u64,		contrib			)
		__field(	u64,		now			)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->hist_util_temp		= contrib ? util_contrib / contrib : 0;
		__entry->util_contrib		= util_contrib;
		__entry->contrib		= contrib;
		__entry->now			= now;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("cpu=%d, comm=%s, pid=%d, hist_util_temp=%d, util_contrib=%llu, contrib=%llu, "
		"now=%llu, flag=%s",
		__entry->cpu, __entry->comm, __entry->pid, __entry->hist_util_temp,
		__entry->util_contrib, __entry->contrib, __entry->now, __entry->label)
);

TRACE_EVENT(update_task_hist_util,

	TP_PROTO(struct task_struct *p, int hist_util, struct task_profiler_cluster_util *cl_util,
		u64 now),

	TP_ARGS(p, hist_util, cl_util, now),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	unsigned long,	hist_util		)
		__array(	unsigned long,	cl_util, MAX_NR_CLUSTERS)
		__field(	u64,		now			)
	),

	TP_fast_assign(
		int i;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->hist_util		= hist_util;
		__entry->now			= now;
		for (i = 0; i < MAX_NR_CLUSTERS; i++) {
			__entry->cl_util[i] = cl_util[i].hist_util;
		}
	),

	TP_printk("comm=%s, pid=%d, hist_util=%lu, cl0_util=%lu, cl1_util=%lu, cl2_util=%lu, cl3_util=%lu, "
		"now=%llu",
		__entry->comm, __entry->pid, __entry->hist_util,
		__entry->cl_util[0], __entry->cl_util[1], __entry->cl_util[2], __entry->cl_util[3],
		__entry->now)
);

TRACE_EVENT(update_task_uarch_contrib,

	TP_PROTO(int cpu, struct task_struct *p, int event, u64 start, u64 finish),

	TP_ARGS(cpu, p, event, start, finish),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		event			)
		__field(	u64,		start			)
		__field(	u64,		finish			)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->event			= event;
		__entry->start			= start;
		__entry->finish			= finish;
	),

	TP_printk("cpu=%d, comm=%s, pid=%d, event=%d, start=%llu, finish=%llu",
		__entry->cpu, __entry->comm, __entry->pid, __entry->event,
		__entry->start, __entry->finish)
);

TRACE_EVENT(update_task_hist_uarch_raw,

	TP_PROTO(struct task_struct *p, int cluster, int event, u64 contrib,
		int training, int execution, u64 count_recent, u64 count_accum),

	TP_ARGS(p, cluster, event, contrib, training, execution, count_recent, count_accum),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cluster			)
		__field(	int,		event			)
		__field(	u64,		contrib			)
		__field(	int,		training		)
		__field(	int,		execution		)
		__field(	u64,		count_recent		)
		__field(	u64,		count_accum		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->cluster		= cluster;
		__entry->event			= event;
		__entry->contrib		= contrib;
		__entry->training		= training;
		__entry->execution		= execution;
		__entry->count_recent		= count_recent;
		__entry->count_accum		= count_accum;
	),

	TP_printk("comm=%s, pid=%d, cluster=%d, event=%d, contrib=%llu, training=%d, execution=%d "
		"count_recent=%llu, count_accum=%llu",
		__entry->comm, __entry->pid, __entry->cluster, __entry->event, __entry->contrib,
		__entry->training, __entry->execution, __entry->count_recent, __entry->count_accum)
);

TRACE_EVENT(update_task_hist_uarch,

	TP_PROTO(struct task_struct *p, int cluster, int event, struct task_profiler_uarch *uarch,
		int training, int execution),

	TP_ARGS(p, cluster, event, uarch, training, execution),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
		__field(	int,		cluster			)
		__field(	int,		event			)
		__field(	int,		training		)
		__field(	int,		execution		)
		__array(	u64,		ipc_recent,	MAX_NR_CLUSTERS	)
		__array(	u64,		ipc_accum,	MAX_NR_CLUSTERS	)
	),

	TP_fast_assign(
		int i;
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid			= p->pid;
		__entry->cluster		= cluster;
		__entry->event			= event;
		__entry->training		= training;
		__entry->execution		= execution;
		for (i = 0; i < MAX_NR_CLUSTERS; i++) {
			__entry->ipc_recent[i] = uarch->uarch_recent[i][0];
			__entry->ipc_accum[i] = uarch->uarch_accum[i][0];
		}
	),

	TP_printk("comm=%s, pid=%d, cluster=%d, event=%d, training=%d, execution=%d, "
		"recent_ipc_cl0=%llu, recent_ipc_cl1=%llu, recent_ipc_cl2=%llu, recent_ipc_cl3=%llu "
		"accum_ipc_cl0=%llu, accum_ipc_cl1=%llu, accum_ipc_cl2=%llu, accum_ipc_cl3=%llu",
		__entry->comm, __entry->pid, __entry->cluster, __entry->event,
		__entry->training, __entry->execution,
		__entry->ipc_recent[0], __entry->ipc_recent[1], __entry->ipc_recent[2], __entry->ipc_recent[3],
		__entry->ipc_accum[0], __entry->ipc_accum[1], __entry->ipc_accum[2], __entry->ipc_accum[3])
);

TRACE_EVENT(dslt_update_blocked_load,

	TP_PROTO(int cpu, unsigned long main_util, unsigned long main_prev_util,
		unsigned long dslt_util, unsigned long dslt_prev_util, char *label),

	TP_ARGS(cpu, main_util, main_prev_util, dslt_util, dslt_prev_util, label),

	TP_STRUCT__entry(
		__field(	int,		cpu			)
		__field(	unsigned long,	main_util		)
		__field(	unsigned long,	main_prev_util		)
		__field(	long,		main_diff		)
		__field(	unsigned long,	dslt_util		)
		__field(	unsigned long,	dslt_prev_util		)
		__field(	long,		dslt_diff		)
		__array(	char,		label,	LABEL_LEN	)
	),

	TP_fast_assign(
		__entry->cpu			= cpu;
		__entry->main_util		= main_util;
		__entry->main_prev_util		= main_prev_util;
		__entry->main_diff		= main_util - main_prev_util;
		__entry->dslt_util		= dslt_util;
		__entry->dslt_prev_util		= dslt_prev_util;
		__entry->dslt_diff		= dslt_util - dslt_prev_util;
		strncpy(__entry->label, label, LABEL_LEN - 1);
	),

	TP_printk("cpu=%d, main_util=%lu, main_prev_util=%lu, main_diff=%ld, "
		"dslt_util=%lu, dslt_prev_util=%lu, dslt_diff=%ld, label=%s",
		__entry->cpu, __entry->main_util, __entry->main_prev_util, __entry->main_diff,
		__entry->dslt_util, __entry->dslt_prev_util, __entry->dslt_diff, __entry->label)
);

TRACE_EVENT(egg_mark_training_task,

	TP_PROTO(struct task_struct *p),

	TP_ARGS(p),

	TP_STRUCT__entry(
		__array(	char,		comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid			)
	),

	TP_fast_assign(
		memcpy(__entry->comm, p->comm, TASK_COMM_LEN);
		__entry->pid = p->pid;
	),

	TP_printk("mark comm=%s, pid=%d",
		__entry->comm, __entry->pid)
);

TRACE_EVENT(egg_set_shader_compile_task,

	TP_PROTO(pid_t pid),

	TP_ARGS(pid),

	TP_STRUCT__entry(
		__field(	pid_t,		pid)
	),

	TP_fast_assign(
		__entry->pid		= pid;
	),

	TP_printk("pid=%d",
		__entry->pid)
);


TRACE_EVENT(egg_important_task,

	TP_PROTO(int prev, int new),

	TP_ARGS(prev, new),

	TP_STRUCT__entry(
		__field(	int,		prev		)
		__field(	int,		new		)
	),

	TP_fast_assign(
		__entry->prev			= prev;
		__entry->new			= new;
	),

	TP_printk("prev=%d, new=%d",
		__entry->prev, __entry->new)
);

TRACE_EVENT(fps_profiler_update,

	TP_PROTO(u64 tgt_frame_us, u64 cur_frame_us, u64 cpu_ren_us, u64 gpu_ren_us,
			int frame_cnt, u64 frame_time, int target, int fps),

	TP_ARGS(tgt_frame_us, cur_frame_us, cpu_ren_us, gpu_ren_us,

	 frame_cnt, frame_time, target, fps),
	TP_STRUCT__entry(
		__field(	u64,		tgt_frame_us		)
		__field(	u64,		cur_frame_us		)
		__field(	u64,		cpu_ren_us		)
		__field(	u64,		gpu_ren_us		)
		__field(	int,		frame_cnt		)
		__field(	u64,		frame_time		)
		__field(	int,		target			)
		__field(	int,		fps			)
	),

	TP_fast_assign(
		__entry->tgt_frame_us		= tgt_frame_us;
		__entry->cur_frame_us		= cur_frame_us;
		__entry->cpu_ren_us		= cpu_ren_us;
		__entry->gpu_ren_us		= gpu_ren_us;
		__entry->frame_cnt		= frame_cnt;
		__entry->frame_time		= frame_time /1000;
		__entry->target			= target;
		__entry->fps			= fps;
	),

	TP_printk("tgt_frame_us=%llu cur_frame_us=%llu cpu_ren_us=%llu gpu_ren_us=%llu frame_cnt=%d, frame_dur_ms=%llu, target=%d, fps=%d",
		__entry->tgt_frame_us, __entry->cur_frame_us, __entry->cpu_ren_us, __entry->gpu_ren_us,
		__entry->frame_cnt, __entry->frame_time, __entry->target, __entry->fps)
);

TRACE_EVENT(fps_profiler_frame_info,

	TP_PROTO(u64 now, u64 target_frame_time, u64 current_frame_time,
	  u64 cpu_time, u64 gpu_time, u64 target_temp, u64 cur_temp, int type),

	TP_ARGS(now, target_frame_time, current_frame_time, cpu_time, gpu_time,
							target_temp, cur_temp, type),

	TP_STRUCT__entry(
		__field(	u64,		now			)
		__field(	u64,		target_frame_time	)
		__field(	u64,		current_frame_time	)
		__field(	u64,		cpu_time		)
		__field(	u64,		gpu_time		)
		__field(	u64,		target_temp		)
		__field(	u64,		cur_temp		)
		__field(	int,		type		)
	),

	TP_fast_assign(
		__entry->now			= ktime_to_ms(now);
		__entry->target_frame_time	= target_frame_time;
		__entry->current_frame_time	= current_frame_time;
		__entry->cpu_time		= cpu_time;
		__entry->gpu_time		= gpu_time;
		__entry->target_temp		= target_temp;
		__entry->cur_temp		= cur_temp;
		__entry->type			= type;
	),

	TP_printk("update_ms=%llu, tgt_frame_us=%7llu, cur_frame_us=%7llu cpu_time_us=%7llu gpu_time_us=%7llu target_temp=%llu, cur_temp=%llu type=%d",
		__entry->now, __entry->target_frame_time, __entry->current_frame_time,
		__entry->cpu_time, __entry->gpu_time, __entry->target_temp, __entry->cur_temp, __entry->type)
);


#endif /* _TRACE_EMS_DEBUG_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
