/*
 * Copyright (c) 2025 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _KERNEL_SCHED_EMS_GAME_H
#define _KERNEL_SCHED_EMS_GAME_H

#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/ems.h>

#include <soc/samsung/ems.h>
#include <soc/samsung/gpu_cooling.h>

#define DEFAULT_TARGET_PERIOD          (100)

/* Game governor mode */
enum op_mode {
	OP_EXTREME_PERFORMANCE = 0,
	OP_PERFORMANCE,
	OP_SUSTAIN,
	OP_ENERGY,
};

enum ep_sub_mode {
	EP_NORMAL = 0,
	EP_TRAINING,
	EP_EXECUTION,
};

enum ep_execution_state {
	EP_EXECUTION_FAILED = 0,
	EP_EXECUTION_TRY = 1,
	EP_EXECUTION_INVALID = 2,
	EP_EXECUTION_SUCCESS = 3,
};

static char *ep_execution_state_name[] = {
	"ep_execution_failed",
	"ep_execution_try",
	"ep_execution_invalid",
	"ep_execution_success",
};

#define MAX_NUM_OF_CL	5

#define LOW_TEMP	1
#define MID_TEMP	2
#define HIGH_TEMP	3

/*
 * EXTERN for TASK PROFILER
 */
static int egg_pmu_event_id[] = {
	ARMV8_PMUV3_PERFCTR_CPU_CYCLES,
	ARMV8_PMUV3_PERFCTR_INST_RETIRED,
};

enum egg_pmu_events {
	PMU_CPU_CYCLES,
	PMU_INST_RETIRED,
	MAX_NR_EVENTS,
};

enum egg_uarch_events {
	UARCH_IPC,
	MAX_NR_UARCH,
};

#define MAX_NR_HISTORY	(10)
#define MAX_NR_MAJOR	(5)
#define MAX_NR_CLEAVE	(3)
#define invalid_egg_index(idx)	((idx) < 0 || (idx) >= MAX_NR_MAJOR)
#define valid_egg_index(idx)	((idx) >= 0 && (idx) < MAX_NR_MAJOR)
#define MAX_NR_CLUSTERS	(4)

struct runnable_history {
	u64			window_size;
	u64			runnable;
};

struct task_profiler_runnable {
	struct runnable_history history[MAX_NR_HISTORY];
	int                     latest_idx;

	u64                     hist_runnable;
	u64                     hist_window;
	u64                     window_size;
	u64                     window_start;
	u64                     contrib;
	u64                     last_updated;
	int			prolonged_running;
};

struct task_profiler_uarch {
	u64                     start_count[MAX_NR_EVENTS];
	u64                     finish_count[MAX_NR_EVENTS];
	u64                     contrib[MAX_NR_CLUSTERS][MAX_NR_EVENTS];

	u64                     count_recent[MAX_NR_CLUSTERS][MAX_NR_EVENTS];
	u64                     count_accum[MAX_NR_CLUSTERS][MAX_NR_EVENTS];
	u64                     uarch_recent[MAX_NR_CLUSTERS][MAX_NR_UARCH];
	u64                     uarch_accum[MAX_NR_CLUSTERS][MAX_NR_UARCH];

	raw_spinlock_t          lock;
};

struct task_profiler_cluster_util {
	unsigned long		hist_util;

	u64			util_contrib;
	u64			contrib;
	u64			total_util_contrib;
	u64			total_contrib;
};

struct task_profiler_util {
	unsigned long           hist_util;

	u64                     contrib;
	u64                     util_contrib;
	u64                     last_updated;

	raw_spinlock_t          lock;

	struct task_profiler_cluster_util cluster_util[MAX_NR_CLUSTERS];
};

struct task_profiler_struct {
	struct task_struct	*p;
	struct list_head	grp_list;
	bool			attached;

	u64			wakeup_count;
	u64			last_updated_wakeup_count;

	struct task_profiler_util               util;
	struct task_profiler_runnable           runnable;
	struct task_profiler_uarch              uarch;

	bool skip_add_count;
};

extern u64 hist_runnable_task(struct task_struct *p);
extern u64 hist_window_task(struct task_struct *p);
extern unsigned long hist_util_task(struct task_struct *p);
extern unsigned long cluster_hist_util_task(struct task_struct *p, int cluster);
extern u64 hist_uarch_task(struct task_struct *p, int cluster, int event);
extern u64 recent_uarch_task(struct task_struct *p, int cluster, int event);
extern int prolonged_running_detected(struct task_struct *p);
extern bool prolonged_running_task_sleep(struct task_struct *p);
extern void task_profiler_update(u64 profile_interval, u64 now);
extern void task_profiler_update_contrib(struct rq *rq, struct task_struct *p, u64 now, int flag);
extern void task_profiler_init_task(struct task_struct *p);
extern void task_profiler_cgroup_attach(struct cgroup_taskset *tset);
extern void task_profiler_flush_task(struct task_struct *p);
extern void task_profiler_init(struct kobject *egg_kobj);
extern void task_profiler_task_switch(struct rq *rq, struct task_struct *prev, struct task_struct *next);
extern void task_profiler_tick_entry(struct rq *rq);
extern void task_profiler_try_to_wake_up(struct rq *rq, struct task_struct *p);
extern void task_profiler_new_task_stats(struct task_struct *p);

/*
 * EXTERN for CPU PROFILER
 */
struct cpu_profiler_util {
	int			hist_util;
	u64			contrib;
	u64			util_contrib;
	u64			last_updated;
	raw_spinlock_t		lock;
};

struct cpu_profiler_uarch {
	struct perf_event       *perf_event[MAX_NR_EVENTS];
	int                     enabled;
};

struct cpu_profiler_struct {
	int cluster_idx;

	struct cpu_profiler_util	util;
	struct cpu_profiler_uarch	uarch;
};

extern unsigned long hist_util_cpu(int cpu);
extern void cpu_profiler_update(u64 now);
extern void cpu_profiler_update_util(struct rq *rq, u64 now, int flag);
extern void cpu_profiler_update_frequency(struct cpufreq_policy *policy);
extern void reset_cpu_profiler(void);
extern void cpu_profiler_init(struct kobject *egg_kobj);
extern u64 cpu_profiler_read_pmu_event(int cpu, int event);

 struct freq_profile_data {
	int orig_max_freq;
	int orig_min_freq;

	int cur_freq;
	int max_freq;

	int avg_cur_freq;
	int avg_max_freq;

	u64 cur_contrib;
	u64 max_contrib;
	u64 contrib;

	u64 last_updated;
	raw_spinlock_t lock;
};

#define NUM_OF_MAX_CL	4
struct freq_profiler_struct {
	struct freq_profile_data	cpu[NUM_OF_MAX_CL];
	struct freq_profile_data	gpu;
	const struct gpu_dvfs_fn	*gpu_dvfs_fn;
};

enum egg_game_type {
	EGT_ERROR = 0,
	EGT_VERY_HEAVY = 1,
	EGT_HEAVY = 2,
	EGT_NORMAL = 3,
	EGT_LIGHT = 4,
	EGT_MAX,
};

extern int get_avg_cur_freq(int cpu);
extern int get_avg_max_freq(int cpu);
extern int get_orig_max_freq(int cpu);
extern int get_avg_gpu_cur_freq(void);
extern int get_avg_gpu_max_freq(void);
extern int get_orig_gpu_min_freq(void);
extern void gpu_freq_prof_update(void);
extern int freq_profiler_gpu_fn_init(const struct platform_device *pdev, const struct gpu_dvfs_fn* fn);

extern u64 egg_get_update_period(void);
extern int egg_get_frame_drop(void);
extern int egg_get_thermal_level(void);
extern int egg_get_op_mode(void);
extern int egg_get_sub_mode(void);
extern int egg_get_execution_state(void);
extern void egg_set_execution_state(int state);
extern void egg_set_execution_task(struct task_struct *task);
extern struct task_struct *egg_get_execution_task(void);
extern int egg_enabled(void);
extern int egg_profile_enabled(void);
extern int egg_stopped(void);
extern int egg_throttle_fastest_cpu(void);
extern bool egg_training_task(struct task_struct *p);
extern bool egg_execution_task(struct task_struct *p);
extern void egg_get_frame_info(u64 target_frame_time, u64 current_frame_time, u64 cpu_time, u64 gpu_time,
				u64 user_target_temp, u64 user_cur_temp, int type);
extern void egg_update_pf_boost_info(u64 target_frame_time, u64 current_frame_time, u64 cpu_time, u64 user_target_temp, u64 user_cur_temp, int type);
extern bool egg_is_normal_game(void);
extern bool egg_is_busy_game(void);
extern void egg_update_shader_compile_task(pid_t pid);

static inline int move_idx(int cur, int cnt, int size)
{
	return (cur + cnt + size) % size;
}

/* Dynamic CPU Throttler */
#define DCT_HIST_SIZE	(DEFAULT_TARGET_PERIOD / 2)
struct dct_cl_load {
	int demand_cpu_raw[DCT_HIST_SIZE];
	int demand_cpu;
	int idx;
};

enum dct_update_tag {
	DCT_UPDATE = 0,
	DCT_FEEDBACK,
	DCT_FEEDBACK_MAVERICK,
	DCT_MAVERICK,
	DCT_ALLOWED_MASK,
	DCT_COMPILE,
	DCT_COMPILE_DEL,
	DCT_RESET,
	END_OF_DCT_UPDATE_TAG,
};

#define MAVERICK_DISABLE	(0)
#define MAVERICK_MIGRATING	(1)
#define MAVERICK_MIGRATED	(1 << 2)

extern int dct_init (struct kobject *ems_kobj);
extern void reset_dct(bool enable);
extern void dct_get_feedback(void);
extern void dct_update(int op_mode);
extern void dct_cpu_load_update(void);
extern void dct_get_available_cpus(struct cpumask *cpus);
extern bool is_maverick_cpu(int cpu);
extern int dct_get_maverick_cpu(struct task_struct *p, int force_test);
extern int dct_get_demand_cpu(void);
extern void dct_update_maverick_dequeue(int cpu, struct task_struct *p);
extern void dct_update_maverick_enqueue(int cpu, struct task_struct *p);
extern const struct cpumask *dct_fa_cpus(void);
extern const struct cpumask *dct_busy_cpus(void);
extern const struct cpumask *dct_busy_cpu_helper_cpus(void);
extern void dct_clear_helper_cpus(int cpu);
extern int dct_change_max_cpus(int cpu, bool open, int tag);
extern void update_dct_cpus_w_lock(int tag, bool skip);

/* TEST */
extern int egg_user_sched_policy(void);

/*
 * EXTERN for FPS PROFILER
 */
extern void fps_profiler_update(void);
extern int get_target_fps(void);
extern int get_target_fps_from(void);
extern int get_current_fps(void);
extern int fps_profiler_init(struct kobject *egg_kobj);
extern unsigned long long get_target_frame_period(void);
extern void update_fps_profile_time_margin(void);
extern bool get_frame_info_enabled(void);
extern bool invalid_frame_info(void);
extern bool frame_info_stop(void);
extern int get_avg_cpu_render_us(void);
extern int get_avg_gpu_render_us(void);

/*
 * EXTERN for CLEAVE
 */
struct cleave_task {
	struct egg_task_struct	*ets;

	int                     cpu;
	int			misfit;
	unsigned long		util;

	struct cpumask		available_cpus_org;
	struct cpumask		available_cpus;
	int			visiting_cpu;

	int			reason; /* for tracing information */
};

extern struct cpumask *get_cleaved_cpus(void);
extern void cleave_update(void);
extern void cleave_update_boosted_mode(int boosted_task);
extern void reset_cleave(void);
extern int cleave_init(struct kobject *egg_kobj);
extern void cleave_get_policy_cpus(struct cpumask *cpus);

/*
 * EXTERN for PROFILER
 */
extern void profiler_task_switch(struct rq *rq, struct task_struct *prev, struct task_struct *next);
extern void profiler_tick_entry(struct rq *rq);
extern void profiler_update_blocked_load(int cpu);
extern void profiler_try_to_wake_up(struct task_struct *p);
extern void profiler_new_task_stats(struct task_struct *p);
extern void profiler_init_task(struct task_struct *p);
extern void profiler_cgroup_attach(struct cgroup_taskset *tset);
extern void profiler_flush_task(struct task_struct *p);
extern void profiler_cpu_frequency_limits(struct cpufreq_policy *policy);
extern void profiler_cpufreq_transition(struct cpufreq_policy *policy);
extern void profiler_update(u64 profile_interval);
extern void profiler_init(struct kobject *egg_kobj);

/*
 * EXTERN for GAME SCHED
 */

enum training_cpu {
	FAST_CPU = 0,
	SLOW_CPU,
	NUM_OF_TRAINING_CPU,
};

struct egg_training_info {
	struct task_struct      *task;
	bool                    active;
	unsigned long           hist_util[MAX_NR_CLUSTERS];
	u64                     hist_uarch[MAX_NR_CLUSTERS][MAX_NR_UARCH];
	u64                     recent_uarch[MAX_NR_CLUSTERS][MAX_NR_UARCH];
	u64                     min_uarch[MAX_NR_CLUSTERS][MAX_NR_UARCH];

	/* Managed by CLEAVE */
	int			count[NUM_OF_TRAINING_CPU]; /* training count on cpu */
};

struct egg_task_struct {
	struct task_struct	*task;
	char			comm[TASK_COMM_LEN];
	int			pid;
	cpumask_t		cpus_ptr;
	int			idx;
	int			misfit;
	int			reserved_cpu;
	int			prev_reserved_cpu;
	bool			near_heaviest;
	int			prolonged_running;
	bool			over_running;
	u64			hist_runnable;
	u64			hist_window;
	unsigned long		hist_util;
	unsigned long		pelt_util;
	u64			binder_contrib;
	bool			important;
	bool			important_tex;
	raw_spinlock_t		task_lock;

	struct egg_training_info training;
};

enum egg_boost_tag {
	EB_TRY_DISABLE = 0,
	EB_PF_CPU_BOOST,
	EB_PF_BIG_BOOST,
	EB_COMPILE,
	EB_COMPILE_DEL,
	EB_OVERUTIL,
	EB_OVERUTIL_DEL,
	EB_AVG_RENDER_BOOST,
	EB_SYSFS,
	END_OF_BOOST_TAG,
};

extern int exec_fast_cl_idx;
extern int exec_slow_cl_idx;

extern void ems_game_update(void);
extern int ems_game_late_init(struct kobject *ems_kobj);
extern void ems_game_sched_init(struct platform_device *pdev);
extern void ems_game_cgroup_attach(struct cgroup_taskset *tset);
extern void ems_game_task_switch(struct rq *rq, struct task_struct *prev, struct task_struct *next);
extern void ems_game_tick_entry(struct rq *rq);
extern void ems_game_try_to_wake_up(struct task_struct *p);
extern void ems_game_task_init(struct task_struct *p);
extern void ems_game_new_task_stats(struct task_struct *p);
extern void ems_game_flush_task(struct task_struct *p);
extern void ems_game_cpu_frequency_limits(struct cpufreq_policy *policy);
extern void ems_game_cpufreq_transition(struct cpufreq_policy *policy);
extern u64 egg_get_binder_contrib(struct task_struct *p);
extern void egg_apply_binder_contrib(struct task_struct *p, u64 contrib);
extern void egg_update_major_tasks(struct task_struct **new_major_tasks);
extern unsigned long egg_get_heaviest_task_util(void);
extern void egg_get_major_tasks(struct egg_task_struct *dst[], unsigned long *total_hist_util);
extern void egg_update_reserved_cpus(struct cleave_task *clv_tasks, int nr_clv);
extern int egg_update_reserved_cpu(int new_task, int prev_task);
extern void egg_assign_reserved_cpu(struct task_struct *p);
extern bool is_reserved_task(struct task_struct *p);
extern int is_reserved_cpu(int cpu);
extern int get_reserved_cpu(struct task_struct *p);
extern void get_reserved_cpu_mask(struct cpumask *mask);
extern int get_reserved_cpu_util(int cpu);
extern bool egg_lb_tick_allowed(int src_cpu, int *force_test);
extern int update_boosted_task(void);
extern int get_boost_cpu(void);
extern int update_boosted_task_reserved_cpu(int boost_task);
extern int get_boosted_task(void);
extern void set_boosted_task(int boosted_task);
extern bool big_boosted(void);
extern void update_big_boost(ktime_t now, bool enabled, bool force, int tag);
extern bool egg_has_shader_compile_task(void);

#define EGG_SYSFS_CONTROL	(0)
#define EGG_STOP_CONTROL	(1)
extern int egg_register_notifier(struct notifier_block *nb);
extern int egg_unregister_notifier(struct notifier_block *nb);

#endif	/* ENDIF _KERNEL_SCHED_EMS_GAME_H */
