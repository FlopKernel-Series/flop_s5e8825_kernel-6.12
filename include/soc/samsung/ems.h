/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd
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

#ifndef _KERNEL_SCHED_EMS_H
#define _KERNEL_SCHED_EMS_H

#include <linux/ems.h>
#include <linux/platform_device.h>
#include <linux/sched/signal.h>
#include <linux/perf/arm_pmuv3.h>
#include <soc/samsung/exynos-cpu-phyid.h>

/* FIXME: user_ddk_unsafe_headers can't set include path
 * "common/include" is already in path, so temporarily include by "../"
 */
#include "../drivers/android/binder_internal.h"
#include "../kernel/sched/sched.h"
#include <linux/bitmap.h>

#include <soc/samsung/game.h>

/*
 * Maximum supported-by-vendor processors.  Setting this smaller saves quite a
 * bit of memory.  Use nr_cpu_ids instead of this except for static bitmaps.
 */
#ifndef CONFIG_VENDOR_NR_CPUS
/* FIXME: This should be fixed in the arch's Kconfig */
#define CONFIG_VENDOR_NR_CPUS	1
#endif

/* Places which use this should consider cpumask_var_t. */
#define VENDOR_NR_CPUS		CONFIG_VENDOR_NR_CPUS

/* maximum count of tracking tasks in runqueue */
#define TRACK_TASK_COUNT	10

#ifndef WF_ANDROID_VENDOR
#define WF_ANDROID_VENDOR	0
#endif

/* TopApp main / render or Important task's prio */
#define TA_BOOST_PRIO	110

enum ems_level {
	EMS_LEVEL_NORMAL = 0,
	EMS_LEVEL_BOOST,
	EMS_LEVEL_PERFORMANCE,
	EMS_LEVEL_COUNT,
};

enum task_cgroup {
	CGROUP_ROOT,
	CGROUP_FOREGROUND,
	CGROUP_FOREGROUND_WINDOW,
	CGROUP_BACKGROUND,
	CGROUP_TOPAPP,
	CGROUP_RT,
	CGROUP_SYSTEM,
	CGROUP_SYSTEM_BACKGROUND,
	CGROUP_DEX2OAT,
	CGROUP_NNAPI_HAL,
	CGROUP_CAMERA_DAEMON,
	CGROUP_COUNT,
};

enum cluster_max_num {
	CLUSTER_DUAL = 2,
	CLUSTER_TRI,
	CLUSTER_QUAD,
};

/* structure for task placement environment */
struct tp_env {
	struct task_struct *p;
	int cgroup_idx;
	int per_cpu_kthread;

	int sched_policy;
	int cl_sync;
	int sync;
	int cpu_sync;
	int prev_cpu;
	int target_cpu;
	int test_cpu;		/* temp allowed cpu by dct for maverick task */

	int task_class;
	int pl_init_index;
	int pl_depth;
	unsigned long base_cap;

	int reason_of_selection;

	struct cpumask cpus_allowed;
	struct cpumask fit_cpus;
	struct cpumask overcap_cpus;
	struct cpumask migrating_cpus;
	struct cpumask reserved_cpus;

	unsigned long task_util;
	unsigned long task_util_clamped;
	struct {
		unsigned long util;
		unsigned long util_wo;
		unsigned long util_with;
		unsigned long rt_util;
		unsigned long dl_util;
		unsigned long nr_running;
		int runnable;
		int idle;
	} cpu_stat[VENDOR_NR_CPUS];
	int total_nr_running;

	int		ipc;
	int		table_index;
};

/* cpu selection domain */
struct cs_domain {
	struct list_head list;
	struct cpumask cpus;
};

struct cleave_snapshot {
	int			clv_task_idx;	/* cleave task idx */
	unsigned long		util;		/* snapshot for energy computing */
	unsigned long		prev_util;	/* To use backup data */
};

/* core */
enum {
	NON_IDLE = 0,
	IDLE_C1,
	IDLE_C2,
};

extern int core_init(struct device_node *ems_dn);
extern int hook_init(void);
extern void ems_hook_enable(bool enabled);
extern bool ems_disabled(void);

int find_cpus_allowed(struct tp_env *env);

/* List of process element */
struct pe_list {
	struct cpumask *cpus;
	int num_of_clusters;
};

extern struct pe_list *get_pe_list(int index);
extern int get_pe_list_size(void);
extern int get_pe_cluster_size(void);
extern struct cpumask *get_pe_cluster_mask(unsigned int cpu);
extern char *task_cgroup_name[];
extern char *task_cgroup_simple_name[];

/* energy table */
struct energy_state {
	unsigned long capacity;
	unsigned long util;
	unsigned long weight;
	unsigned long frequency;
	unsigned long voltage;
	unsigned long dynamic_power;
	unsigned long static_power;
	int idle;
	int temperature;
};

struct energy_backup {
	unsigned long frequency;
	unsigned long voltage;
	unsigned long energy;
};

struct energy_domain {
	struct cpumask cpus;
};

extern struct energy_domain *get_energy_domains(void);
extern int get_nr_energy_domains(void);

#define MLT_MAX_CLUSTER_NUM	5
#define invalid_cluster_id(id)	((id) < 0 || (id) >= MLT_MAX_CLUSTER_NUM)
#define invalid_cpu(cpu)	((cpu) < 0 || (cpu) >= VENDOR_NR_CPUS)
#define valid_cpu(cpu)		((cpu) >= 0 && (cpu) < VENDOR_NR_CPUS)

#define MLT_MAX_RAW_NUM		8
static char *cpu_uarch_name[] = {
	"ipc",
};
enum mlt_uarch_cpu {
	MLT_UARCH_CPU_IPC,		/* INST_RETIRED / CPU_CYCLE */
	MLT_UARCH_CPU_NUM,
};
enum mlt_uarch_cpu_raw {
	MLT_RAW_CPU_CPU_CYCLE,
	MLT_RAW_CPU_INST_RETIRED,
	MLT_RAW_CPU_NUM,
};

static char *tsk_uarch_name[] = {
	"ipc",
};
enum mlt_uarch_tsk {
	MLT_UARCH_TSK_IPC,		/* INST_RETIRED / CPU_CYCLE */
	MLT_UARCH_TSK_NUM,
};
enum mlt_uarch_tsk_raw {
	MLT_RAW_TSK_CPU_CYCLE,
	MLT_RAW_TSK_INST_RETIRED,
	MLT_RAW_TSK_NUM,
};

/* multi load tracker */
#define INACTIVE	(0)
#define	ACTIVE		(1)
#define MLT_STATE_NOCHANGE	0xBABE

#define MLT_RUNNABLE_UNIT		1000
#define MLT_RUNNABLE_ROUNDS_UNIT	500
#define MLT_RUNNABLE_UP_UNIT		999

#define MLT_PERIOD_SIZE		(4 * NSEC_PER_MSEC)
#define MLT_PERIOD_SIZE_MIN	(MLT_PERIOD_SIZE - (MLT_PERIOD_SIZE >> 4))
#define MLT_PERIOD_COUNT	8
#define MLT_IDLE_THR_TIME	(8 * NSEC_PER_MSEC)
#define MLT_IRQ_LOAD_RATIO	(95)

#define MLT_LOAD_PERIOD_SIZE	(8 * NSEC_PER_MSEC)
#define MLT_LOAD_PERIOD_COUNT	(1)
#define MLT_LOAD_DIVISOR	(MLT_LOAD_PERIOD_SIZE >> SCHED_CAPACITY_SHIFT)
#define MLT_CPU_LOAD_UPDATE	(0)
#define MLT_TASK_LOAD_UPDATE	(1)

enum mlt_load_update_flags {
	NEW_TASK = 0,
	WAKEUP_TASK,
	UPDATE_TASK,
	PUT_PREV_TASK,
	PICK_NEXT_TASK,
	UPDATE_BLOCKED_LOAD,
};
extern const char *mlt_load_flag_name[];

enum mlt_value_type {
	PERIOD,			/* completed period matched with index */
	AVERAGE,		/* average of all periods */
};

struct mlt_part {
	int		state;
	int		cur_period;
	int		active_period;
	u64		period_start;
	u64		last_updated;
	int		periods[MLT_PERIOD_COUNT];
	int		recent;
	u64		contrib;

	int		load_history[MLT_LOAD_PERIOD_COUNT];
	int		hist_idx;
	u64		load_recent;
	u64		load_period_start;
	u64		last_load_updated;

	struct task_struct *task;
};

struct mlt_uarch {
	u32		periods[MLT_PERIOD_COUNT];
	u32		recent; /* recent = (contrib[X] * 1000) / contrib[Y] */
};

struct mlt_uarch_raw {
	u64		contrib;	/* current_uarch_raw - last_uarch_raw */
	u64		last;
};

struct mlt_runnable {
	int		runnable;
	int		topapp_runnable;
	int		misfit_runnable;
	int		sustain_high_runnable;
	int		nr_run;
	int		nr_topapp;
	int		nr_sustain_high;
	u64		contrib;
	u64		runnable_contrib;
	u64		topapp_runnable_contrib;
	u64		misfit_runnable_contrib;
	u64		sustain_high_runnable_contrib;
	u64		last_updated;
};

struct mlt_cluster {
	int			idx;
	unsigned int		cur_freq;
	unsigned int		max_freq;
	unsigned int		max_freq_org;

	struct cpumask		cpus;
	u64			constant_high_util_ns;
	u64			constant_low_util_ns;
};

struct mlt_cpu {
	int			cpu;
	int			cluster;
	u64			runtime_scale;

	struct mlt_part		part;
	struct mlt_runnable	runnable;
	struct mlt_uarch	uarch[MLT_UARCH_CPU_NUM];
	struct mlt_uarch_raw	uarch_raw[MLT_RAW_CPU_NUM];
};

struct mlt_task {
	struct mlt_part		part;
	struct mlt_uarch	uarch[MLT_UARCH_TSK_NUM];
	struct mlt_uarch_raw	uarch_raw[MLT_RAW_TSK_NUM];
};

struct mlt_env {
	struct mlt_part		*part;
	struct mlt_uarch	*uarch;
	struct mlt_uarch_raw	*raw;
	int			next_state;
	bool			is_task;
	bool			update_uarch;
	bool			tick;
};

extern u32 mlt_tsk_uarch(struct task_struct *p, enum mlt_uarch_tsk type, int size);
extern u32 mlt_cpu_uarch(int cpu, enum mlt_uarch_cpu type, int size);
extern int mlt_runnable(int cpu);
extern int mlt_topapp_runnable(int cpu);
extern int mlt_misfit_runnable(int cpu);
extern void mlt_update_runnable_ctrb(struct rq *rq);
extern void mlt_enqueue_task(struct rq *rq, struct task_struct *p);
extern void mlt_dequeue_task(struct rq *rq, struct task_struct *p);
extern void mlt_update_task(struct task_struct *p, int next_state, u64 now, bool update_uarch, bool tick);
extern void mlt_update_cpu(int cpu, int next_state, u64 now, bool update_uarch, bool tick);
extern void mlt_tick(void);
extern void mlt_task_switch(int cpu, struct task_struct *prev, struct task_struct *next);
extern int mlt_cur_period(int cpu);
extern int mlt_period_with_delta(int idx, int delta);
extern u64 mlt_cpu_value(int cpu, int arg, enum mlt_value_type type, int state);
extern void mlt_init_task(struct task_struct *p);
extern u64 mlt_task_value(struct task_struct *p, int arg, enum mlt_value_type type);
extern int mlt_task_cur_period(struct task_struct *p);
extern int mlt_init(struct kobject *ems_kobj, struct device_node *dn);
extern bool need_sustain_high_boost(void);
extern unsigned long _ml_task_util_est(struct task_struct *p);
extern unsigned long ml_task_util(struct task_struct *p);
extern unsigned long ml_task_util_est(struct task_struct *p);
extern unsigned long ml_cpu_util(int cpu);
extern unsigned long ml_cpu_util_avg(int cpu);
extern unsigned long ml_cpu_util_with_irq(struct rq *rq, int util);
extern unsigned long ml_cpu_util_with(struct task_struct *p, int dst_cpu);
extern unsigned long ml_cpu_util_without(int cpu, struct task_struct *p);
extern unsigned long ml_cpu_irq_load(int cpu);
extern int mlt_sustain_high_runnable(int cpu);
extern void mlt_update_load(struct rq *rq, struct task_struct *p, u64 now, int flag);
extern unsigned long mlt_latest_task_load(struct task_struct *p);
extern bool mlt_get_constant_load(void);
extern int mlt_cluster_index(int cpu);
extern struct cpumask *mlt_cluster_cpus(int index);

extern void check_enqueue_task_attr(struct rq *rq, struct task_struct *p);
extern void check_dequeue_task_attr(struct rq *rq, struct task_struct *p);

extern void init_task_attr(struct task_struct *p);
extern void high_prio_init(void);

/* For ecs governor - dynamic */
struct dynamic_dom {
	struct list_head	node;
	unsigned int		id;
	struct cpumask		cpus;			/* sibling cpus of this domain */
	struct cpumask		default_cpus;		/* minimum cpus */
	struct cpumask		governor_cpus;		/* governor target cpus */
	bool			has_spared_cpu;
	int			nr_cpus;

	/* help domains */
	struct dynamic_dom	**nr_help_dom;
	int			num_of_nr_help_dom;
	struct dynamic_dom	**misfit_help_dom;
	int			num_of_misfit_help_dom;

	/* data */
	int			required_cpus;
	int			nr_busy_cpus;
	int			nr_run;
	int			nr_misfit;
	int			nr_help;
	int			busy;

	/* tunable */
	unsigned int		busy_ratio;
	int			nr_run_up_ratio;
	int			help_misfit_thr_by_user;
	int			help_misfit_thr_by_emstune;
};

#define ECS_USER_NAME_LEN 	(16)
struct ecs_governor {
	char name[ECS_USER_NAME_LEN];
	struct list_head	list;
	void (*update)(void);
	void (*enqueue_update)(int prev_cpu, struct task_struct *p);
	void (*start)(void);
	void (*stop)(void);
	const struct cpumask *(*get_target_cpus)(void);
};

/*
 * Priority-pinning
 */
struct tex_struct {
	struct task_struct	*task;
	struct list_head	list;
	u64			window_runtime;
	u64			last_updated;
	int			nr_expired_windows;
	int			level;
};

enum {
	NOT_TEX = 0,
	HIGH_PRIO_TEX,
	RENDER_TEX,
	LOW_LATENCY_TEX,
	RESERVED_TEX,
	LAUNCH_BOOST_TEX,
};

#define TEX_WINDOW_SIZE	(4 * NSEC_PER_MSEC)
#define TEX_SINGLE_WINDOW	(1)
#define TEX_FULL_WINDOWS	(3)

/* gsc */
enum gsc_level {
	GSC_LEVEL1 = 1,
	GSC_LEVEL2,
	GSC_LEVEL_COUNT,
};
#define valid_gsc_level(level)	(GSC_LEVEL1 <= (level) && (level) < GSC_LEVEL_COUNT)

/* emstune - freq boost */
struct emstune_freqboost {
	int ratio[CGROUP_COUNT][VENDOR_NR_CPUS];
};

#define DEFAULT_LATENCY_STEP	4
#define DEFAULT_MARGIN_STEP 	4
#define STEP_MIN_LATENCY	4
#define STEP_MAX_LATENCY	24
#define STEP_MIN_MARGIN		12
#define STEP_MAX_MARGIN		25

enum step_flags {
	STEP_LATENCY = 0,
	STEP_LATENCY_FREQ,
	STEP_PELT_MARGIN,
	STEP_MARGIN_FREQ,
};

/* emstune - cpufreq governor parameters */
struct emstune_cpufreq_gov {
	int ptb_margin[VENDOR_NR_CPUS];
	int ptb_min_freq[VENDOR_NR_CPUS];
	int step_latency_enable;
	int step_margin_enable;
	int step_latency_ns[VENDOR_NR_CPUS][DEFAULT_LATENCY_STEP];
	int step_latency_freq[VENDOR_NR_CPUS][DEFAULT_LATENCY_STEP];
	int step_pelt_margin[VENDOR_NR_CPUS][DEFAULT_MARGIN_STEP];
	int step_margin_freq[VENDOR_NR_CPUS][DEFAULT_MARGIN_STEP];
	int pelt_boost[VENDOR_NR_CPUS];
	int window_count[VENDOR_NR_CPUS];
	int htask_ratio_threshold[VENDOR_NR_CPUS];
	int adap_fmax_enable[VENDOR_NR_CPUS];
	int adap_fmax_freq[VENDOR_NR_CPUS];
};

struct emstune_cpuidle_gov {
	int expired_ratio[VENDOR_NR_CPUS];
};

/* emstune - high priority threshold */
struct emstune_high_prio {
	int threshold[CGROUP_COUNT];
};

/* emstune - fclamp */
struct emstune_fclamp {
	int min_freq[VENDOR_NR_CPUS];
	int min_target_period[VENDOR_NR_CPUS];
	int min_target_ratio[VENDOR_NR_CPUS];

	int max_freq[VENDOR_NR_CPUS];
	int max_target_period[VENDOR_NR_CPUS];
	int max_target_ratio[VENDOR_NR_CPUS];

	int monitor_group[CGROUP_COUNT];
};

/* SCHED CLASS */
#define EMS_SCHED_STOP		(1 << 0)
#define EMS_SCHED_DL		(1 << 1)
#define EMS_SCHED_RT		(1 << 2)
#define EMS_SCHED_FAIR		(1 << 3)
#define EMS_SCHED_IDLE		(1 << 4)
#define NUM_OF_SCHED_CLASS	5

/* emstune - cpus allowed */
struct emstune_cpus_binding {
	struct cpumask mask[CGROUP_COUNT];
};

struct emstune_ecs_dynamic {
	int dynamic_busy_ratio[VENDOR_NR_CPUS];
	int dynamic_nr_run_up_ratio[VENDOR_NR_CPUS];
	int dynamic_help_misfit_thr[VENDOR_NR_CPUS];
	struct cpumask min_cpus;
};

/* emstune - new task util */
struct emstune_ntu {
	int ratio[CGROUP_COUNT];
};

struct emstune_support_uclamp {
	int enabled;
};

struct emstune_small_task {
	int small_task_thr;
};

struct emstune_gsc {
	unsigned int up_threshold[GSC_LEVEL_COUNT];
	unsigned int down_threshold[GSC_LEVEL_COUNT];
	unsigned int boost_en_threshold;
	unsigned int boost_dis_threshold;
	unsigned int boost_ratio;
};

struct emstune_should_spread {
	int enabled;
};

struct emstune_frt {
	int boost_enabled[CGROUP_COUNT];
	int scale_freq_threshold[VENDOR_NR_CPUS];
};

struct emstune_cgroup_boost {
	int enabled[CGROUP_COUNT];
	unsigned int enabled_bit;
};

struct emstune_energy_table {
	unsigned int default_idx;
	int uarch_selection;
};

/* emstune - util_est */
struct emstune_util_est {
	unsigned int enabled[CGROUP_COUNT];
};

struct emstune_dslt {
	int runnable_sensitivity[CGROUP_COUNT];
	int running_sensitivity[CGROUP_COUNT];
};

#ifdef CONFIG_SCHED_EMS_DSU_SLICE_DOWN
struct emstune_dsu_slice_down {
	int enabled;
};
#endif

struct emstune_misfit_ratio {
	int ratio[CGROUP_COUNT][VENDOR_NR_CPUS];
};

struct emstune_set {
	int					mode;
	int					level;

	struct emstune_freqboost		freqboost;
	struct emstune_cpufreq_gov		cpufreq_gov;
	struct emstune_cpuidle_gov		cpuidle_gov;
	struct emstune_high_prio		high_prio;
	struct emstune_cpus_binding		cpus_binding;
	struct emstune_ecs_dynamic		ecs_dynamic;
	struct emstune_ntu			ntu;
	struct emstune_fclamp			fclamp;
	struct emstune_support_uclamp		support_uclamp;
	struct emstune_small_task		small_task;
	struct emstune_gsc			gsc;
	struct emstune_should_spread		should_spread;
	struct emstune_frt			frt;
	struct emstune_cgroup_boost		cgroup_boost;
	struct emstune_energy_table		et;
	struct emstune_util_est			util_est;
	struct emstune_dslt			dslt;
#ifdef CONFIG_SCHED_EMS_DSU_SLICE_DOWN
	struct emstune_dsu_slice_down		dsu_slice_down;
#endif
	struct emstune_misfit_ratio             misfit_ratio;

	int					cpu_dsu_table_index;
};

extern int emstune_sched_boost(void);
extern bool emstune_adaptive_cap(void);
extern int emstune_et_default_idx(void);
extern int emstune_sched_applaunch(void);
extern int emstune_support_uclamp(void);
extern int emstune_small_task_thr(void);
extern int emstune_should_spread(void);
extern int emstune_frt_boost_enabled(int cgroup);
extern int emstune_frt_scale_freq_threshold(int cpu);
extern bool emstune_need_cgroup_boost(struct task_struct *p);
extern int emstune_sched_io_boost(void);
extern struct cpumask *override_ta_binding_mask(struct task_struct *p);
extern int emstune_get_dslt_running(int cgroup);
extern int emstune_get_dslt_runnable(int cgroup);

enum {
	PROFILE_CPU_IDLE,	/* cpu is not busy */
	PROFILE_CPU_BUSY,	/* cpu busy */
};

enum {
	CPU_UTIL,	/* cpu util */
	CPU_HTSK,	/* cpu heaviest task */
	CPU_PROFILE_MAX,
};

/* Acrive ratio based profile */
struct cpu_profile {
	u64	value;	/* util, wratio, htsk */
	u64	data;	/* busy,   busy,  pid */
};

/* profile */
struct system_profile_data {
	/* util based profile */
	int			busy_cpu_count;
	int			heavy_task_count;

	unsigned long		cpu_util_sum;
	unsigned long		avg_nr_run_sum;
	unsigned long		avg_nr_heavy_sum;
	unsigned long		heavy_task_util_sum;
	int			avg_nr_sustain_high_sum;
	unsigned int		new_task_count;

	/* cpu profile data */
	struct cpu_profile	cp[VENDOR_NR_CPUS][CPU_PROFILE_MAX];
};

#define HEAVY_TASK_UTIL_RATIO	(40)
#define MISFIT_TASK_UTIL_RATIO	(80)
#define check_busy(util, cap)	((util * 100) >= (cap * 80))
#define check_busy_half(util, cap) ((util * 100) >= (cap * 50))

static inline int is_heavy_task_util(unsigned long util)
{
	return util > (SCHED_CAPACITY_SCALE * HEAVY_TASK_UTIL_RATIO / 100);
}

static inline int is_misfit_task_util(unsigned long util)
{
	return util > (SCHED_CAPACITY_SCALE * MISFIT_TASK_UTIL_RATIO / 100);
}

enum fair_causes {
	FAIR_ALLOWED_0,
	FAIR_ALLOWED_1,
	FAIR_FIT_0,
	FAIR_FIT_1,
	FAIR_SYSBUSY,
	FAIR_EXPRESS,
	FAIR_ENERGY,
	FAIR_PERFORMANCE,
	FAIR_SYNC,
	FAIR_FAST_TRACK,
	FAIR_RESERVED,
	FAIR_FALLBACK,
	FAIR_FAILED,
	END_OF_FAIR_CAUSES,
};

extern void update_fair_stat(int cpu, enum fair_causes);
extern void update_rt_stat(int cpu);

/* Sysbusy */
#define TICK_SEC	250
#define SYSBUSY_SOMAC	SYSBUSY_STATE3
int sysbusy_monitor_interval(int state);
int sysbusy_energy_table_idx(int state);

/* MH DVFS */
#ifdef CONFIG_MHDVFS
extern void mhdvfs(void);
extern void mhdvfs_init(struct kobject *ems_kobj);
#else
static inline void mhdvfs(void) { };
static inline void mhdvfs_init(struct kobject *ems_kobj) { };
#endif

struct dslt_avg {
	u64				last_update_time;
	u64				runnable_sum;
	u32				period_contrib;
	unsigned long			runnable_avg;
	unsigned int			util_est;

	int				running_sensitivity;
	int				runnable_sensitivity;
} ____cacheline_aligned;

/* DSLT */
struct ems_dslt_rq {
	int cpu;
	struct dslt_avg	avg;
	int	sensitivity;
	struct {
		raw_spinlock_t	lock ____cacheline_aligned;

		int		nr;
		unsigned long	runnable_avg;
	} removed;
};

struct ems_dslt_task {
	struct dslt_avg avg;
	int cgroup_idx;
};

struct ems_cpu_topology {
	struct cpumask	cluster_cpus;
	int		cluster_id;
};

struct ems_sched_domain {
	struct cpumask	domain_cpus;
	int		domain_id;
};

struct _rq_avd1 {
	struct list_head tex_list;
	unsigned long somac_dequeued;
	int nr_misfited;
	unsigned long flag;
	int cluster_idx;
	int nr_high_prio;
	int nr_tex;
	int nr_launch;
	int nr_reserved_task;
	struct ems_dslt_rq dslt;
	u64 htask;
	unsigned long htask_load;
	unsigned long capacity_orig;
	unsigned long capacity;
	int nr_sustain_high;
};

struct ems_task_struct {
	int			target_cpu;
	bool			task_misfited;
	u32			task_maverick;
	int			maverick_cpu;	/* temp allowed_cpu to lb tick */
	bool			boosted_by_user;
	bool			launch_task;
	bool			reserved_task;
	bool			binder_task;
	bool			low_latency_task;
	bool			render_task;
	bool			high_prio_task;
	u64			last_waked;
	u64			gsc_task;
	u64			enqueue_ts;
	int			prev_cgroup;
	u64			hts_config;
	u64			yield_cnt;
	u64			yield_last;
	bool			busy_waiting;
	bool			topapp;
	u64			task_profiler;
	int			reserved_cpu;
	int			binder_caller_pid;
	int			egg_task_idx;
	int			major_count;
#ifdef CONFIG_SCHED_EMS_TASK_GROUP
	int			cgroup;
#endif
#if IS_ENABLED(CONFIG_EXYNOS_MPAM_POLICY)
	int			partid_i;
	int			partid_d;
#endif
	/* before refactoring mlt, put new items above here */
	struct mlt_task		task_mlt;
	struct ems_dslt_task	dslt_task;
	struct tex_struct	tex_struct;
	bool			sustain_high;
	unsigned char		sustain_high_hist;
};

extern void ems_task_init(struct task_struct *p);

#define EMS_VENDOR_DATA_SIZE_CHECK(ems_struct, k_struct)	\
        BUILD_BUG_ON(sizeof(ems_struct) > (sizeof(u64) *	\
                ARRAY_SIZE(((k_struct *)0)->android_vendor_data1)))

extern DEFINE_PER_CPU(struct _rq_avd1, ems_runqueues);

#define cpu_rq_avd(cpu)			(&per_cpu(ems_runqueues, (cpu)))
#define rq_avd(rq)			(cpu_rq_avd(rq->cpu))

#define ems_rq_tex_list(rq)		((struct list_head *)&(rq_avd(rq)->tex_list))
#define ems_somac_dequeued(rq)		(rq_avd(rq)->somac_dequeued)
#define ems_rq_nr_misfited(rq)		(rq_avd(rq)->nr_misfited)
#define ems_rq_flag(rq)			(rq_avd(rq)->flag)
#define ems_rq_cluster_idx(rq)		(rq_avd(rq)->cluster_idx)
#define ems_rq_nr_tex(rq)		(rq_avd(rq)->nr_tex)
#define ems_rq_nr_high_prio(rq)		(rq_avd(rq)->nr_high_prio)
#define ems_rq_nr_launch(rq)		(rq_avd(rq)->nr_launch)
#define ems_rq_nr_reserved_task(rq)	(rq_avd(rq)->nr_reserved_task)
#define ems_rq_dslt(rq)			((struct ems_dslt_rq *)&(rq_avd(rq)->dslt))
#define ems_rq_htask(rq)		(rq_avd(rq)->htask)
#define ems_rq_htask_load(rq)		(rq_avd(rq)->htask_load)
#define ems_rq_cpu_capacity_orig(rq)	(rq_avd(rq)->capacity_orig)
#define ems_rq_cpu_capacity(rq)		(rq_avd(rq)->capacity)
#define ems_rq_nr_sustain_high(rq)	(rq_avd(rq)->nr_sustain_high)

#define task_avd(task)			((struct ems_task_struct *)android_task_vendor_data(task))

#define ems_target_cpu(task)		(task_avd(task)->target_cpu)
#define ems_task_misfited(task)		(task_avd(task)->task_misfited)
#define ems_task_maverick(task)		(task_avd(task)->task_maverick)
#define ems_boosted_by_user(task)	(task_avd(task)->boosted_by_user)
#define ems_launch_task(task)		(task_avd(task)->launch_task)
#define ems_reserved_task(task)		(task_avd(task)->reserved_task)
#define ems_binder_task(task)		(task_avd(task)->binder_task)
#define ems_low_latency_task(task)	(task_avd(task)->low_latency_task)
#define ems_render_task(task)		(task_avd(task)->render_task)
#define ems_high_prio_task(task)	(task_avd(task)->high_prio_task)
#define ems_last_waked(task)		(task_avd(task)->last_waked)
#define ems_gsc_task(task)		(task_avd(task)->gsc_task)
#define ems_enqueue_ts(task)		(task_avd(task)->enqueue_ts)
#define ems_prev_cgroup(task)		(task_avd(task)->prev_cgroup)
#define ems_yield_cnt(task)		(task_avd(task)->yield_cnt)
#define ems_yield_last(task)		(task_avd(task)->yield_last)
#define ems_busy_waiting(task)		(task_avd(task)->busy_waiting)
#define ems_topapp_task(task)		(task_avd(task)->topapp)
#define ems_task_profiler_struct(task)	(task_avd(task)->task_profiler)
#define ems_task_reserved_cpu_wo_boost(task)	(task_avd(task)->reserved_cpu)
#define binder_caller_pid(task)		(task_avd(task)->binder_caller_pid)
#define egg_task_idx(task)		(task_avd(task)->egg_task_idx)
#define egg_major_count(task)		(task_avd(task)->major_count)
#ifdef CONFIG_SCHED_EMS_TASK_GROUP
#define ems_cgroup(task)		(task_avd(task)->cgroup)
#endif
#if IS_ENABLED(CONFIG_EXYNOS_MPAM_POLICY)
#define mpam_partid_i(task)		(task_avd(task)->partid_i)
#define mpam_partid_d(task)		(task_avd(task)->partid_d)
#endif
#define ems_sustain_high(task)		(task_avd(task)->sustain_high)
#define ems_sustain_high_hist(task)	(task_avd(task)->sustain_high_hist)

#define task_mlt(task)			((struct mlt_task *)&(task_avd(task)->task_mlt))
#define ems_dslt_task(task)		((struct ems_dslt_task *)&(task_avd(task)->dslt_task))
#define get_tex_struct(task)		((struct tex_struct *)&(task_avd(task)->tex_struct))

extern DEFINE_PER_CPU(struct ems_cpu_topology, topo_info);
#define percpu_topology(cpu)		(&per_cpu(topo_info, (cpu)))

#define ems_cpulist_parse(buf, mask) (bitmap_parselist(buf, cpumask_bits(mask), (CONFIG_VENDOR_NR_CPUS)))

/******************************************************************************
 * common API                                                                 *
 ******************************************************************************/
extern unsigned long ems_update_cpu_capacity(int cpu, unsigned int reason);
extern unsigned long ems_get_actual_cpu_capacity(int cpu);
extern unsigned long ems_get_origin_cpu_capacity(int cpu);
extern unsigned long ems_get_enforced_adaptive_capacity(int cpu);
extern void ems_rq_cpu_capacity_init(void);
extern bool min_perf_cluster;

#ifdef CONFIG_SCHED_HW_PRESSURE
static inline u64 hw_load_avg(struct rq *rq)
{
	return READ_ONCE(rq->avg_hw.load_avg);
}
#else
static inline u64 hw_load_avg(struct rq *rq)
{
	return 0;
}
#endif

static inline int cpu_overutilized_rt(int cpu, unsigned long cpu_util)
{
	return (ems_get_actual_cpu_capacity(cpu) * 1024) < (cpu_util * 1280);
}

static inline struct sched_entity *get_task_entity(struct sched_entity *se)
{
#ifdef CONFIG_FAIR_GROUP_SCHED
	struct cfs_rq *cfs_rq = se->my_q;

	while (cfs_rq) {
		se = cfs_rq->curr;
		cfs_rq = se->my_q;
	}
#endif

	return se;
}

static inline bool can_migrate(struct task_struct *p, int dst_cpu)
{
	if (p->exit_state)
		return false;

	if (is_per_cpu_kthread(p))
		return false;

	if (p->migration_disabled)
		return false;

#ifdef CONFIG_SMP
	if (task_on_cpu(NULL, p))
		return false;
#endif

	if (!cpumask_test_cpu(dst_cpu, p->cpus_ptr))
		return false;

	return cpu_active(dst_cpu);
}

static inline int get_idle_exit_latency(struct rq *rq)
{
	struct cpuidle_state *idle;
	unsigned int exit_latency;

	rcu_read_lock();
	idle = idle_get_state(rq);
	exit_latency =  idle ? idle->exit_latency : 0;
	rcu_read_unlock();

	return exit_latency;
}

static inline bool is_small_task(struct task_struct *p)
{
	unsigned long util = ml_task_util_est(p);
	unsigned long threshold = emstune_small_task_thr();

	return util < threshold;
}

#ifdef CONFIG_UCLAMP_TASK
static inline bool ml_uclamp_boosted(struct task_struct *p)
{
	if (!emstune_support_uclamp() || is_small_task(p))
		return false;

	return uclamp_eff_value(p, UCLAMP_MIN) > 0;
}

static inline bool ml_uclamp_latency_sensitive(struct task_struct *p)
{
	struct cgroup_subsys_state *css = task_css(p, cpu_cgrp_id);
	struct task_group *tg;

	if (!css)
		return false;

	tg = container_of(css, struct task_group, css);

	return tg->latency_sensitive;
}

static inline unsigned long ml_uclamp_task_util(struct task_struct *p)
{
	return clamp(ml_task_util_est(p),
		     uclamp_eff_value(p, UCLAMP_MIN),
		     uclamp_eff_value(p, UCLAMP_MAX));
}
#else
static inline bool ml_uclamp_boosted(struct task_struct *p)
{
	return false;
}

static inline bool uclamp_latency_sensitive(struct task_struct *p)
{
	return false;
}

static inline unsigned long ml_uclamp_task_util(struct task_struct *p)
{
	return ml_task_util_est(p);
}
#endif

#define BUSY_CPU_RATIO	150
static inline bool __is_busy_cpu(unsigned long util,
				 unsigned long runnable,
				 unsigned long capacity,
				 unsigned long nr_running)
{
	if (runnable < capacity)
		return false;

	if (!nr_running)
		return false;

	if (util * BUSY_CPU_RATIO >= runnable * 100)
		return false;

	return true;
}

extern int get_sched_class(struct task_struct *p);
extern int cpuctl_task_group_idx(struct task_struct *p);
extern const struct cpumask *cpu_clustergroup_mask(int cpu);
extern int cpu_clustergroup_id(int cpu);
extern const struct cpumask *cpu_sched_domain_mask(int cpu);
extern int cpu_sched_domain_id(int cpu);
extern const struct cpumask *cpu_slowest_mask(void);
extern const struct cpumask *cpu_fastest_mask(void);
extern int send_uevent(char *msg);

extern void detach_task(struct rq *src_rq, struct rq *dst_rq, struct task_struct *p);
extern int detach_one_task(struct rq *src_rq, struct rq *dst_rq, struct task_struct *target);
extern void attach_task(struct rq *dst_rq, struct task_struct *p);
extern void attach_one_task(struct rq *dst_rq, struct task_struct *p);
extern int ems_task_reserved_cpu(struct task_struct *p);

static inline u64 amu_core_cycles(void)
{
	return read_sysreg_s(SYS_AMEVCNTR0_CORE_EL0);
}

static inline u64 amu_const_cycles(void)
{
	return read_sysreg_s(SYS_AMEVCNTR0_CONST_EL0);
}

static inline u64 amu_inst_ret(void)
{
	return read_sysreg_s(SYS_AMEVCNTR0_INST_RET_EL0);
}

static inline u64 amu_mem_stall(void)
{
	return read_sysreg_s(SYS_AMEVCNTR0_MEM_STALL);
}

/* Load Balance */
static inline bool ems_rq_migrated(struct rq *rq)
{
	return test_bit(1, &ems_rq_flag(rq));
}

static inline bool ems_set_migrated(struct rq *rq)
{
	return test_and_set_bit(1, &ems_rq_flag(rq));
}

static inline void ems_clear_migrated(struct rq *rq)
{
	clear_bit(1, &ems_rq_flag(rq));
}

#define MAX_CLUSTER_NUM		(4)
static inline void ems_rq_update_nr_misfited(struct rq *rq, bool misfit)
{
	int val = misfit ? 1 : -1;

	ems_rq_nr_misfited(rq) += val;

	BUG_ON(ems_rq_nr_misfited(rq) < 0);
}

static inline bool is_top_app_task(struct task_struct *p)
{
	return cpuctl_task_group_idx(p) == CGROUP_TOPAPP;
}

static inline bool is_rt_task(struct task_struct *p)
{
	return p->prio < MAX_RT_PRIO;
}

extern int profile_sched_init(struct kobject *);
extern int profile_sched_data(void);
extern int profile_get_htask_ratio(int cpu);
extern void profile_enqueue_task(struct rq *rq, struct task_struct *p, int *flags);
extern void profile_dequeue_task(struct rq *rq, struct task_struct *p, int *flags);
extern void profile_update_htask(struct rq *rq, int *flags);
extern void get_system_sched_data(struct system_profile_data *);
extern void profile_accumulate_new_task_count(void);
extern bool profile_get_sustain_high(void);
extern int profile_heavy_task(u64 *task_util);

extern void ntu_apply(struct task_struct *p);
extern void ntu_init(struct kobject *ems_kobj);

/* dslt */
extern void dslt_sync_entity_load_avg(struct ems_dslt_rq *dslt_rq, struct sched_entity *se);
extern void dslt_update_blocked_fair(struct rq *rq);
extern void dslt_update_blocked_load(void);
extern void dslt_set_task_rq_fair(struct sched_entity *se, struct cfs_rq *prev, struct cfs_rq *next);
extern void dslt_migrate_se_dslt_lag(struct sched_entity *se);
extern void dslt_remove_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se);
extern void dslt_detach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se);
extern void dslt_attach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se);
extern void dslt_update_load_avg(u64 now, struct cfs_rq *cfs_rq, struct sched_entity *se);
extern void dslt_util_est_dequeue(struct rq *rq, struct task_struct *p);
extern void dslt_util_est_update(struct cfs_rq *cfs_rq, struct task_struct *p, bool task_sleep);
extern void dslt_dequeue_task(struct rq *rq, struct task_struct *p, int flags);
extern void dslt_enqueue_task(struct rq *rq, struct task_struct *p, int flags);
extern void dslt_post_init_entity_util_avg(struct sched_entity *se);
extern void dslt_init_entity_runnable_average(struct task_struct *p);
extern void dslt_new_task_wakeup(struct task_struct *p);
extern void dslt_switch_sensitivity(int cpu, struct task_struct *prev, struct task_struct *next);
extern void dslt_init_task(struct task_struct *p);
extern int dslt_init(struct kobject *ems_kobj, struct device_node *dn);


/* Governor Disabled Type */
#define GOV_CTRL_BY_USER	(1 << 0)
#define GOV_CTRL_BY_SYSBUSY	(1 << 1)
#define GOV_CTRL_BY_RATIO	(1 << 2)
#define GOV_CTRL_BY_EGG		(1 << 3)
#define GOV_CTRL_BY_INIT	(1 << 4)

/* For HALO */
/* time and slen table */
#define HIST_NUM	7
struct time_table {
	s64 time;
	s64 slen;
};
struct time_hist {
	s32 cur_idx;
	struct time_table tbl[HIST_NUM];
};

/* hit/miss history */
#define PULSE		1024
#define DECAY_SHIFT	2
#define IGNORE_NUM	(1 << 7)
struct hit_state {
	u64 hit;	/* hit ratio */
	u64 miss;	/* miss ratio */
};

/* c-state data */
struct cstate_data {
	u64 entry_latency_ns;
	u64 exit_latency_ns;
	u64 target_residency_ns;
};

/* IPI's history */
struct halo_ipi {
	s64 ipi_slen_ns;	/* predicted sleep length to next ipi */
	struct time_hist time_hist;
};

/* Tick info */
struct halo_tick {
	int triggered;		/* tick stopped */
	s64 last_tick_ns;	/* last tick time */
	s64 tick_slen_ns;	/* sleep length to next tick */
	int tick_wakeup;
};

/* halo htimer */
struct halo_timer {
	struct hrtimer timer;
	u32 triggered;		/* timer expired status */
	int type;		/* timer type */
	struct hit_state hstate;
};

/* halo sched-info */
struct halo_sched {
	u32 avg_nr_run;		/* number of running cpus */
	s32 heaviness;		/* system heaviness value from sched info */
};

/* periodic irq requester */
#define IRQ_NAME_LEN	20
struct halo_irq {
	struct list_head list;
	int num;		/* irq num */
	char name[IRQ_NAME_LEN];/* irq name */
	struct cpumask cpus;	/* irq affinity mask */
	u64 cnt;		/* irq accumulate count */
	ktime_t last_update_ns;	/* last irq triggered time */
	ktime_t period_ns;	/* irq period */

	void (*fn)(u64 *cnt, ktime_t *time);	/* irq validation check function */

	int enabled;		/* irq validate status */
	bool validated;		/* irq period is valid */
};

/* periodic irq list */
struct halo_irqs {
	struct list_head list;
	raw_spinlock_t	lock;
	s64 irq_slen_ns;
};

/* halo tuanble and sysfs */
#define	HALO_RATIO_UNIT		100
struct halo_tune {
	struct cpumask cpus;
	int expired_ratio;		/* def-timer expired time ratio */
	struct kobject		kobj;
};

/*
 * struct halo_cpu - CPU data used by the HAO Governor
 */
struct halo_cpu {
	u32 last_idx;			/* last selected c-state */
	u32 last_waker;			/* last wakeup source */
	s64 last_slen_ns;		/* last sleep length */
	s64 timer_slen_ns;		/* sleep length to next timer event without tick */
	s64 req_latency_ns;		/* requested latency */
	s64 det_slen_ns;		/* determined sleep length to next event like(timer, tick, periodic irqs) */
	int tick_wakeup;

	struct halo_ipi hipi;		/* ipi history */
	struct halo_tick htick;		/* tick information */
	struct halo_sched hsched;	/* sched information */
	struct halo_irqs hirqs;		/* periodic irqs list */
	struct halo_timer htimer;	/* timer struct to correct c-state */
	struct halo_tune *htune;	/* for tunable */

	struct cstate_data csd[CPUIDLE_STATE_MAX];	/* dynamic cstate data */

	struct cpuidle_device *dev;
};

/*
 *EXTERN for IDLE SELECTION
 */
#ifdef CONFIG_SCHED_EMS_IDLE_SELECT
extern void ems_idle_select_init(struct platform_device *pdev);

extern int ecs_init(struct kobject *ems_kobj);
extern int ecs_gov_dynamic_init(struct kobject *ems_kobj);
extern bool dynamic_domain_first_cpu(int cpu);
extern void dynamic_control_governor(int user, int enable);
extern const struct cpumask *ecs_cpus_allowed(struct task_struct *p);
extern const struct cpumask *ecs_available_cpus(void);
extern const struct cpumask *ecs_max_cpus(void);
extern void ecs_update(void);
extern void ecs_retry_sparing(struct rq *rq);
extern void ecs_enqueue_update(int prev_cpu, struct task_struct *p);
extern int ecs_cpu_available(int cpu, struct task_struct *p);
extern void ecs_governor_register(struct ecs_governor *gov, bool default_gov);
extern void update_ecs_cpus(bool migrate);
extern struct kobject *ecs_get_governor_object(void);

extern int halo_governor_init(struct kobject *ems_kobj);
extern void halo_tick(struct rq *rq);
extern void update_idle_allowed_mask(const struct cpumask *mask);
extern void halo_update_busy_cpus(const struct cpumask *mask);

/* fvr */
extern void fv_init_table(struct cpufreq_policy *policy);
extern int fv_init(struct kobject *ems_kobj);
extern u64 fv_get_residency(int cpu, int state);
extern u64 fv_get_exit_latency(int cpu, int state);

#else /* CONFIG_SCHED_EMS_IDLE_SELECT */
static inline void fv_init_table(struct cpufreq_policy *policy) { };

static inline void ems_idle_select_init(struct platform_device *pdev) { };

static inline bool dynamic_domain_first_cpu(int cpu) { return false; };
static inline void dynamic_control_governor(int user, int enable) { };
static inline const struct cpumask *ecs_cpus_allowed(struct task_struct *p) { return cpu_active_mask; };
static inline const struct cpumask *ecs_available_cpus(void) { return cpu_active_mask; };
static inline void ecs_update(void) { };
static inline void ecs_enqueue_update(int prev_cpu, struct task_struct *p) { };
static inline int ecs_cpu_available(int cpu, struct task_struct *p) { return true; };

static inline int halo_governor_init(struct kobject *ems_kobj) { return 0; };
static inline void halo_tick(struct rq *rq) { };
static inline void update_idle_allowed_mask(const struct cpumask *mask) { }
static inline void halo_update_busy_cpus(const struct cpumask *mask) { return; };
#endif /* CONFIG_SCHED_EMS_IDLE_SELECT */

struct fclamp_data {
	int freq;
	int target_period;
	int target_ratio;
	int type;
};

#define HEAVY_TASK		(1 << 1)
#define UCLAMP_TASK		(1 << 2)
#define SUSTAIN_HIGH_TASK	(1 << 3)

/*
 * EXTERN for FREQ SELECTION
 */
#ifdef CONFIG_SCHED_EMS_FREQ_SELECT
extern void ems_freq_select_init(struct platform_device *pdev);

/* cpufreq */
enum update_capacity_reason {
	UPDATE_CPU_CAPACITY_HOOK,
	CPUFREQ_LIMITS_PRESSURE,
	INTERNAL_LIMIT_PRESSURE,
	UPDATE_CPU_CAPACITY_ORIG,
};
extern void cpufreq_register_hook(int cpu, int (*func_get_next_cap)(struct tp_env *env,
			struct cpumask *cpus, int dst_cpu), int (*func_need_slack_timer)(void));
extern void cpufreq_unregister_hook(int cpu);
extern int cpufreq_init(void);
extern int cpufreq_get_next_cap(struct tp_env *env, struct cpumask *cpus, int dst_cpu);
extern void slack_timer_cpufreq(int cpu, bool idle_start, bool idle_end);
extern unsigned int fclamp_apply(struct cpufreq_policy *policy, unsigned int orig_freq);
extern void ems_update_cpufreq_pressure(struct cpufreq_policy *policy, unsigned int reason);
extern unsigned long ems_get_cpufreq_pressure(int cpu);

/* freqboost */
extern int freqboost_init(void);
extern void freqboost_enqueue_task(struct task_struct *p, int cpu, int flags);
extern void freqboost_dequeue_task(struct task_struct *p, int cpu, int flags);
extern int freqboost_get_task_ratio(struct task_struct *p);
extern void freqboost_set_boost_ratio(int tcpu, int grp, int boost);
extern unsigned long freqboost_cpu_boost(int cpu, unsigned long util);
extern unsigned long heavytask_cpu_boost(int cpu, unsigned long util, int ratio);

/* ego */
extern unsigned long ego_cpu_util(int cpu, unsigned long util_cfs, unsigned long scale,
				unsigned long *min, unsigned long *max);
extern int ego_pre_init(struct kobject *ems_kobj);
extern void ego_update_pelt_boost(int cpu, int val);
extern void ego_update_freq(struct rq *rq, u64 time, unsigned int flags);
extern void adap_fmax_update(void);
extern unsigned int ego_get_internal_freq_limit(struct cpufreq_policy *policy);

/* energy_table */
extern int et_init(struct kobject *ems_kobj);
extern void et_init_table(struct cpufreq_policy *policy);
extern unsigned int et_cur_freq_idx(int cpu);
extern unsigned int et_table_size(int cpu);
extern unsigned long et_cur_cap(int cpu);
extern unsigned long et_max_cap(int cpu);
extern unsigned long et_freq_to_cap(int cpu, unsigned long freq);
extern unsigned long et_freq_to_dpower(int cpu, unsigned long freq);
extern unsigned long et_dpower_to_cap(int cpu, unsigned long dpower);
extern unsigned long et_dpower_to_freq(int cpu, unsigned long dpower);
extern void et_update_freq(int cpu, unsigned long freq);
extern void et_fill_energy_state(struct tp_env *env, struct cpumask *cpus,
		struct energy_state *states, unsigned long capacity, int dsu_cpu);
extern void et_fill_energy_state_cleave(struct cleave_snapshot *csc, struct cpumask *cpus,
		struct energy_state *states, unsigned long capacity);
extern unsigned long et_compute_system_energy(struct energy_state *states, int target_cpu,
					struct energy_backup *backup);
extern int et_get_table_index_by_ipc(struct tp_env *env);
extern unsigned long et_min_dpower(int cpu);
extern unsigned long et_max_dpower(int cpu);
extern unsigned long et_find_next_cap(int cpu, unsigned long capacity);
#else /* CONFIG_SCHED_EMS_FREQ_SELECT */
static inline void ems_freq_select_init(struct platform_device *pdev) { };

/* cpufreq */
static inline int cpufreq_get_next_cap(struct tp_env *env, struct cpumask *cpus, int dst_cpu) { return -1; };
static inline void slack_timer_cpufreq(int cpu, bool idle_start, bool idle_end) { };

/* freqboost */
static inline void freqboost_enqueue_task(struct task_struct *p, int cpu, int flags) { };
static inline void freqboost_dequeue_task(struct task_struct *p, int cpu, int flags) { };

/* energy_table */
static inline void et_init_table(struct cpufreq_policy *policy) { };
static inline unsigned int et_cur_freq_idx(int cpu) { return 0; };
static inline unsigned long et_cur_cap(int cpu) { return 1024; };
static inline void et_fill_energy_state(struct tp_env *env, struct cpumask *cpus,
		struct energy_state *states, unsigned long capacity, int dsu_cpu) { };
static inline unsigned long et_compute_system_energy(struct energy_state *states, int target_cpu,
						struct energy_backup *backup) { return 0; };
static inline unsigned long et_max_cap(int cpu) { return ems_get_actual_cpu_capacity(cpu); };
static inline unsigned long et_find_next_cap(int cpu, unsigned long capacity) { return 0; };
#endif /* CONFIG_SCHED_EMS_FREQ_SELECT */

#ifdef CONFIG_SCHED_EMS_PAGO
/* pago */
extern int pago_init(struct kobject *ems_kobj);
extern void pago_update(void);
extern void pago_tick(struct rq *rq);
extern void pago_idle_enter(int cpu);
extern void pago_idle_exit(int cpu);
extern void pago_suspend(void);
extern void pago_resume(void);
extern void pago_ramp_up_ctrl(int ramp_up_enable);
extern void check_pago_update_period(void);
extern void reset_update_period(void);
extern void pago_change_shift_level_dsu(unsigned int level);
extern void pago_change_shift_level_mif(unsigned int level);
#else /* CONFIG_SCHED_EMS_PAGO */
/* pago */
static inline int pago_init(struct kobject *ems_kobj) { return -1; };
static inline void pago_update(void) {};
static inline void pago_tick(struct rq *rq) {};
static inline void pago_idle_enter(int cpu) {};
static inline void pago_idle_exit(int cpu) {};
static inline void pago_change_shift_level_dsu(unsigned int level);
extern inline void pago_change_shift_level_mif(unsigned int level);
#endif /* CONFIG_SCHED_EMS_PAGO */

/*
 * EXTERN for FREQ SELECTION
 */
#ifdef CONFIG_SCHED_EMS_CORE_SELECT
extern void ems_core_select_init(struct platform_device *pdev);

extern int ems_select_task_rq_fair(struct task_struct *p, int prev_cpu,
					int sd_flag, int wake_flag, int test_cpu);
extern const struct cpumask *cpus_binding_mask(struct task_struct *p);

/* frt */
extern void frt_init(void);
extern int frt_find_lowest_rq(struct task_struct *task, struct cpumask *lowest_mask, int ret);
extern int frt_select_task_rq_rt(struct task_struct *p, int prev_cpu, int sd_flag, int wake_flags);

extern void lb_init(void);
extern void lb_enqueue_misfit_task(struct task_struct *p, struct rq *rq);
extern void lb_dequeue_misfit_task(struct task_struct *p, struct rq *rq);
extern void lb_update_misfit_status(struct task_struct *p, struct rq *rq, bool *need_update);
extern void lb_nohz_balancer_kick(struct rq *rq, unsigned int *flag, int *done);
extern void lb_can_migrate_task(struct task_struct *p, int dst_cpu, int *can_migrate);
extern void lb_find_busiest_queue(int dst_cpu, struct sched_group *group,
		struct cpumask *env_cpus, struct rq **busiest, int *done);
extern void lb_newidle_balance(struct rq *this_rq, struct rq_flags *rf, int *pulled_task, int *done,
		bool smp);
extern void lb_smp_call_newidle_balance(int cpu);
extern void lb_tick(struct rq *rq);
extern int lb_rebalance_domains(struct rq *rq);
extern int lb_find_new_ilb(struct cpumask *nohz_idle_cpus_mask);
extern bool lb_migrate_running_task(int src_cpu, int dst_cpu, struct task_struct *pp);
extern void lb_send_ipi_wakeup(int wakeup_cpu);

extern void set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);
extern bool is_launch_task(struct task_struct *p);
extern bool is_low_latency_task(struct task_struct *p);
extern bool is_high_prio_task(struct task_struct *p);
extern bool is_render_task(struct task_struct *p);
extern bool is_zygote_task(struct task_struct *p);
extern bool is_important_task(struct task_struct *p);
extern bool is_io_boost_task(struct task_struct *p);

extern void tex_enqueue_task(struct rq *rq, struct task_struct *p);
extern void tex_dequeue_task(struct rq *rq, struct task_struct *p);
extern void tex_replace_next_task_fair(struct rq *rq, struct task_struct **p_ptr,
				       struct task_struct *prev);
extern bool is_tex_task(struct task_struct *p);
extern void tex_tick(struct rq *rq);
extern void tex_do_yield(struct rq *rq);
extern void tex_task_init(struct task_struct *p);
extern void tex_check_preempt_wakeup(struct rq *rq, struct task_struct *p,
		bool *preempt, bool *ignore);

extern void gsc_init(struct kobject *ems_kobj);
extern void gsc_update_tick(void);
extern bool is_gsc_task(struct task_struct *p);
extern void gsc_init_new_task(struct task_struct *p);
extern void gsc_cpu_cgroup_attach(struct cgroup_taskset *tset);
extern void gsc_flush_task(struct task_struct *p);
extern int gsc_manage_task_group(struct task_struct *p, bool attach, bool flush);
extern bool gsc_enabled(void);
extern bool gsc_boost_activated(void);

extern int sysbusy_init(struct kobject *ems_kobj);
extern void monitor_sysbusy(void);
extern int sysbusy_activated(void);
extern void somac_tasks(void);
extern int sysbusy_on_somac(void);
extern int sysbusy_boost_task(struct task_struct *p);
extern int sysbusy_shuffle(struct rq *src_rq);
extern void sysbusy_dequeue_task(struct task_struct *p);
extern void sysbusy_do_yield(struct task_struct *p);
extern void sysbusy_update_busy_wait(struct rq *rq);
extern void sysbusy_suspend(void);

/* EMSTune */
extern int emstune_init(struct kobject *ems_kobj, struct device_node *dn, struct device *dev);
extern bool emstune_task_boost(void);
extern void emstune_capacity_fixup(unsigned long *capacity, int cpu);
extern void emstune_maxfreq_fixup(unsigned int *max_freq, int cpu);
extern void set_maxfreq_fixup_value(struct cpufreq_policy *policy);
extern void set_capacity_fixup_mask(void);
extern bool ems_task_fits_capacity(struct task_struct *p, int cpu,
		unsigned long cap, unsigned long util);
extern bool ems_cpu_overutilized(int cpu, unsigned long cap, unsigned long util);

#ifdef CONFIG_SCHED_EMS_TASK_GROUP
extern struct uclamp_se
ems_uclamp_tg_restrict(struct task_struct *p, enum uclamp_id clamp_id);
extern int task_group_init(struct kobject *ems_kobj);
extern void taskgroup_enqueue_task(struct rq *rq, struct task_struct *p, int *flags);
extern int taskgroup_pl_init_index(struct task_struct *p);
#else
static inline void taskgroup_enqueue_task(struct rq *rq, struct task_struct *p, int *flags) { };
static inline int taskgroup_pl_init_index(struct task_struct *p) { return 0 };
#endif /* CONFIG_SCHED_EMS_TASK_GROUP */

#else	/* CONFIG_SCHED_EMS_CORE_SELECT */
static inline int emstune_init(struct kobject *ems_kobj, struct device_node *dn, struct device *dev) { return 0; };
static inline bool emstune_task_boost(void) { return false; };
static bool ems_task_fits_capacity(struct task_struct *p, int cpu,
				unsigned long cap, unsigned long util);
		{ return cap * SCHED_CAPACITY_SCALE > ml_task_util_est(p) * 1280; };
static bool ems_cpu_overutilized(int cpu, unsigned long cap, unsigned long util)
		{ return cap * SCHED_CAPACITY_SCALE < util * 1280; };

static inline void monitor_sysbusy(void) { };
static inline void somac_tasks(void) { };
static inline void sysbusy_dequeue_task(struct task_struct *p) { };
static inline void sysbusy_do_yield(struct task_struct *p) { };
static inline void sysbusy_update_busy_wait(struct rq *rq) { };
static inline void sysbusy_suspend(void) { };

static inline void gsc_update_tick(void) { };
static inline void gsc_init_new_task(struct task_struct *p) { };
static inline void gsc_cpu_cgroup_attach(struct cgroup_taskset *tset) { };
static inline void gsc_flush_task(struct task_struct *p) { };
static inline int gsc_manage_task_group(struct task_struct *p, bool attach, bool flush) { return 0; };

static inline void set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se) { };
static inline void tex_enqueue_task(struct rq *rq, struct task_struct *p) { };
static inline void tex_dequeue_task(struct rq *rq, struct task_struct *p) { };
static inline void tex_replace_next_task_fair(struct rq *rq, struct task_struct **p_ptr,
				       struct task_struct *prev) { };
static inline bool is_tex_task(struct task_struct *p) { return false; };
static inline void tex_tick(struct rq *rq) { };
static inline void tex_do_yield(struct rq *rq) { };
static inline void tex_task_init(struct task_struct *p) { };
static inline void tex_check_preempt_wakeup(struct rq *rq, struct task_struct *p,
		bool *preempt, bool *ignore) { };

static inline void ems_core_select_init(struct platform_device *pdev) { };
static inline int ems_select_task_rq_fair(struct task_struct *p, int prev_cpu,
				   int sd_flag, int wake_flag, int test_cpu) { return -1; };
/* frt */
static inline void frt_init(void) { };
static inline int frt_find_lowest_rq(struct task_struct *task, struct cpumask *lowest_mask, int ret) { return -1; };
static inline int frt_select_task_rq_rt(struct task_struct *p, int prev_cpu, int sd_flag, int wake_flags) { return -1; };

static inline void lb_init(void) { };
static inline void lb_enqueue_misfit_task(struct task_struct *p, struct rq *rq) { };
static inline void lb_dequeue_misfit_task(struct task_struct *p, struct rq *rq) { };
static inline void lb_update_misfit_status(struct task_struct *p, struct rq *rq, bool *need_update) { };
static inline void lb_nohz_balancer_kick(struct rq *rq, unsigned int *flag, int *done) { };
static inline void lb_can_migrate_task(struct task_struct *p, int dst_cpu, int *can_migrate) { };
static inline void lb_find_busiest_queue(int dst_cpu, struct sched_group *group,
		struct cpumask *env_cpus, struct rq **busiest, int *done) { };
static inline void lb_newidle_balance(struct rq *this_rq, struct rq_flags *rf, int *pulled_task, int *done,
		bool smp) { };
static inline void lb_smp_call_newidle_balance(int cpu) { };
static inline void lb_tick(struct rq *rq) { };
static inline int lb_rebalance_domains(struct rq *rq) { return false; };
static inline int lb_find_new_ilb(struct cpumask *nohz_idle_cpus_mask) { return 0; };
static inline bool lb_migrate_running_task(int src_cpu, int dst_cpu, struct task_struct *pp) { return false; };
static inline void lb_send_ipi_wakeup(int wakeup_cpu) { };
#endif /* CONFIG_SCHED_EMS_CORE_SELECT */

/* perf events */
extern int exynos_perf_events_init(struct kobject *ems_kobj);

#ifdef CONFIG_SCHED_EMS_DSU_SLICE_DOWN
extern void dsu_slice_down_init(void);
#endif

/* TODO: should be reconsier in 6.12 */
/**
 * idle_cpu - is a given CPU idle currently?
 * @cpu: the processor in question.
 *
 * Return: 1 if the CPU is currently idle. 0 otherwise.
 */
static inline int idle_cpu_ems(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (rq->curr != rq->idle)
		return 0;

	if (rq->nr_running)
		return 0;

#ifdef CONFIG_SMP
	if (rq->ttwu_pending)
		return 0;
#endif

	return 1;
}

/**
 * available_idle_cpu - is a given CPU idle for enqueuing work.
 * @cpu: the CPU in question.
 *
 * Return: 1 if the CPU is currently idle. 0 otherwise.
 */
static inline int available_idle_cpu_ems(int cpu)
{
	if (!idle_cpu_ems(cpu))
		return 0;

	if (vcpu_is_preempted(cpu))
		return 0;

	return 1;
}

#undef avaiable_idle_cpu
#define available_idle_cpu available_idle_cpu_ems

#endif	/* ENDIF _KERNEL_SCHED_EMS_H */
