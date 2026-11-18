/*
 * Task Profiler for Exynos Mobile Scheduler
 *
 * Copyright (C) 2025 Samsung Electronics Co., Ltd
 */
#include <soc/samsung/game.h>

#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

static LIST_HEAD(task_list);
static DEFINE_RAW_SPINLOCK(task_profiler_lock);

unsigned long min_heavy_threshold;

#define DEFAULT_MIN_HEAVY_THRESHOLD	(70)
#define TASK_PROFILER_UPDATE_INTERVAL	(100 * NSEC_PER_MSEC)
u64 task_profiler_last_updated;

/********************************************************************************/
/*					API					*/
/********************************************************************************/
u64 hist_runnable_task(struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return 0;

	return tps->runnable.hist_runnable;
}

u64 hist_window_task(struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return 0;

	return tps->runnable.hist_window;
}

unsigned long hist_util_task(struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return 0;

	return tps->util.hist_util;
}

unsigned long cluster_hist_util_task(struct task_struct *p, int cluster)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return 0;

	return tps->util.cluster_util[cluster].hist_util;
}

u64 hist_uarch_task(struct task_struct *p, int cluster, int event)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return 0;

	return tps->uarch.uarch_accum[cluster][event];
}

u64 recent_uarch_task(struct task_struct *p, int cluster, int event)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return 0;

	return tps->uarch.uarch_recent[cluster][event];
}

#define DEFAULT_SLEEP_MARGIN	(10 * NSEC_PER_MSEC)
bool prolonged_running_task_sleep(struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;
	ktime_t sleep_margin = DEFAULT_SLEEP_MARGIN;
	ktime_t now = ktime_get();

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return true;

	if (tps->runnable.last_updated + sleep_margin < now)
		return true;
	return false;
}

int prolonged_running_detected(struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return 0;

	return tps->runnable.prolonged_running;
}

/********************************************************************************
 *				Runnable Tracking				*
 ********************************************************************************/
static unsigned long long get_target_window_size(void)
{
	return  get_target_frame_period();
}

static bool need_update_contrib(struct rq *rq, struct task_struct *p, int flag)
{
	if (flag == WAKEUP_TASK || flag == NEW_TASK)
		return false;

	if (flag == PICK_NEXT_TASK && is_idle_task(rq->curr))
		return false;

	if (flag == UPDATE_TASK && !p->on_rq)
		return false;

	return true;
}

static void update_runnable_contrib(struct task_profiler_struct *tps, u64 delta)
{
	struct task_profiler_runnable *runnable = &tps->runnable;

	delta = max_t(s64, delta, 0);

	runnable->contrib += delta;
	runnable->contrib = min_t(u64, runnable->contrib, runnable->window_size);
}

#define NR_RECENT_WINDOWS	(3)
#define PROLONGED_RUNNING_THR	(50)
static void update_runnable_history(struct task_profiler_struct *tps, u64 contrib, u64 window_size,
				int nr_windows)
{
	struct task_profiler_runnable *runnable = &tps->runnable;
	int i, idx, count = 0;
	u64 runnable_sum = 0, window_sum = 0;
	u64 update_period = egg_get_update_period() * NSEC_PER_MSEC;
	u64 recent_runnable = 0, recent_window = 0; /* 'recent' indicates the latest three windows */
	int prolonged_running = 0;

	nr_windows = min_t(int, nr_windows, MAX_NR_HISTORY);

	for (i = 0; i < nr_windows; i++) {
		idx = (runnable->latest_idx + 1) % MAX_NR_HISTORY;

		runnable->history[idx].runnable = contrib;
		runnable->history[idx].window_size = window_size;
		runnable->latest_idx = idx;
	}

	idx = runnable->latest_idx;
	for (i = 0; i < MAX_NR_HISTORY; i++) {
		struct runnable_history *history = &runnable->history[idx];

		if (unlikely(!history->runnable))
			continue;

		if (window_sum + history->window_size > update_period)
			break;

		runnable_sum += history->runnable;
		window_sum += history->window_size;

		if (count < NR_RECENT_WINDOWS) {
			recent_runnable += history->runnable;
			recent_window += history->window_size;
		}

		idx = move_idx(idx, -1, MAX_NR_HISTORY);
		count++;
	}

	runnable->hist_runnable = count > 0 ? div64_u64(runnable_sum, count) : 0;
	runnable->hist_window = count > 0 ? div64_u64(window_sum, count) : 0;

	if (recent_window > 0) {
		u64 running_ratio = mult_frac(recent_runnable, 100, recent_window);
		if (running_ratio >= PROLONGED_RUNNING_THR)
			prolonged_running = running_ratio;
	}
	runnable->prolonged_running = prolonged_running;

	runnable->contrib = 0;

	trace_task_profiler_runnable_history(tps);
}

static void update_runnable(struct rq *rq, struct task_profiler_struct *tps, u64 now, int flag)
{
	struct task_profiler_runnable *runnable = &tps->runnable;
	bool update_contrib = need_update_contrib(rq, tps->p, flag);
	u64 window_start = runnable->window_start;
	u64 window_size = runnable->window_size;
	u64 last_updated = runnable->last_updated;
	u64 window_finish, nr_windows, delta_ns;

	delta_ns = now - window_start;
	delta_ns = max_t(s64, delta_ns, 0);

	/* current window is not expired. keep using it */
	if (delta_ns < window_size) {
		if (update_contrib)
			update_runnable_contrib(tps, now - last_updated);
		goto out;
	}

	/* 1) update old window */
	window_finish = window_start + window_size;
	if (update_contrib)
		update_runnable_contrib(tps, window_finish - last_updated);
	update_runnable_history(tps, runnable->contrib, window_size, 1);

	/*
	 * 2) update fully runned windows
	 * skip fully sleeped windows, only track the time actually runnable.
	 */
	window_start += window_size;
	nr_windows = div64_u64(now - window_start, window_size);
	if (nr_windows && update_contrib)
		update_runnable_history(tps, window_size, window_size, nr_windows);

	/*
	 * 3) start new window
	 * adjust window_start to current time if the task is waking up
	 */
	window_start += nr_windows * window_size;
	delta_ns = now - window_start;
	/* adjust window_start to current */
	if (flag == WAKEUP_TASK || flag == NEW_TASK)
		window_start = now;
	if (update_contrib)
		update_runnable_contrib(tps, delta_ns);

	runnable->window_start = window_start;
	runnable->window_size = get_target_window_size();
out:
	runnable->last_updated = now;
}

/********************************************************************************
 *				Hist Util Tracking				*
 ********************************************************************************/
static void update_util_contrib(struct task_profiler_struct *tps, u64 delta)
{
	struct task_profiler_util *util = &tps->util;
	unsigned long task_util = ml_task_util(tps->p);
	bool training = egg_training_task(tps->p);

	lockdep_assert_held(&util->lock);

	util->util_contrib += task_util * delta;
	util->contrib += delta;

	if (training) {
		struct task_profiler_cluster_util *cl_util;
		int cluster = mlt_cluster_index(task_cpu(tps->p));

		cl_util = &util->cluster_util[cluster];
		cl_util->util_contrib += task_util * delta;
		cl_util->contrib += delta;
	}
}

static void __update_hist_contrib(struct rq *rq, struct task_profiler_struct *tps, u64 now, int flag)
{
	struct task_profiler_util *util = &tps->util;
	u64 delta;

	lockdep_assert_held(&util->lock);

	if (unlikely(now < util->last_updated))
		return;

	if (!need_update_contrib(rq, tps->p, flag))
		goto out;

	delta = now - util->last_updated;
	update_util_contrib(tps, delta);

	/* apply binder task's contrib to caller */
	if ((flag == PUT_PREV_TASK) && (binder_caller_pid(tps->p) != -1))
		egg_apply_binder_contrib(tps->p, delta);

	trace_update_task_hist_contrib(task_cpu(tps->p), tps->p, util->util_contrib, util->contrib,
				now, mlt_load_flag_name[flag]);
out:
	util->last_updated = now;
}

static void update_cluster_hist_util(struct task_profiler_struct *tps)
{
	struct task_profiler_util *util = &tps->util;
	struct task_profiler_cluster_util *cl_util;
	bool training = egg_training_task(tps->p);
	int cluster;

	if (!training) {
		memset(util->cluster_util, 0, sizeof(struct task_profiler_cluster_util) * MAX_NR_CLUSTERS);
		return;
	}

	for (cluster = 0; cluster < MAX_NR_CLUSTERS; cluster++) {
		cl_util = &util->cluster_util[cluster];

		cl_util->total_util_contrib += cl_util->util_contrib;
		cl_util->total_contrib += cl_util->contrib;
		if (cl_util->total_contrib)
			cl_util->hist_util = div64_u64(cl_util->total_util_contrib, cl_util->total_contrib);

		cl_util->util_contrib = 0;
		cl_util->contrib = 0;
	}
}

static void update_hist_util(struct task_struct *p, u64 now)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;
	struct task_profiler_util *util;
	unsigned long flags;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return;

	util = &tps->util;
	raw_spin_lock_irqsave(&util->lock, flags);

	/* compute recent contrib */
	__update_hist_contrib(task_rq(p), tps, now, UPDATE_TASK);

	/* update task's hist util */
	util->hist_util = div64_u64(util->util_contrib, util->contrib);
	util->util_contrib = 0;
	util->contrib = 0;

	update_cluster_hist_util(tps);

	util->last_updated = now;

	raw_spin_unlock_irqrestore(&util->lock, flags);

	trace_update_task_hist_util(p, util->hist_util, util->cluster_util, now);
}

static void update_hist_contrib(struct rq *rq, struct task_profiler_struct *tps, u64 now, int flag)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&tps->util.lock, flags);
	__update_hist_contrib(rq, tps, now, flag);
	raw_spin_unlock_irqrestore(&tps->util.lock, flags);
}

/********************************************************************************
 *				Uarch Tracking					*
 ********************************************************************************/
static void update_uarch_contrib(struct rq *rq, struct task_profiler_struct *tps, int flag)
{
	struct task_profiler_uarch *uarch = &tps->uarch;
	unsigned long flags;
	int cpu = cpu_of(rq), cluster_idx = mlt_cluster_index(cpu), event;

	/* this task is not a major task */
	if (invalid_egg_index(egg_task_idx(tps->p)))
		return;

	if (flag != PUT_PREV_TASK && flag != PICK_NEXT_TASK)
		return;

	raw_spin_lock_irqsave(&uarch->lock, flags);

	for (event = 0; event < MAX_NR_EVENTS; event++) {
		if (flag == PICK_NEXT_TASK) {
			uarch->start_count[event] = cpu_profiler_read_pmu_event(cpu, event);
			uarch->finish_count[event] = 0;
			trace_update_task_uarch_contrib(cpu_of(rq), tps->p, event,
					uarch->start_count[event], uarch->finish_count[event]);
		}
		else { /* PUT_PREV_TASK */
			uarch->finish_count[event] = cpu_profiler_read_pmu_event(cpu, event);
			uarch->contrib[cluster_idx][event] +=
					uarch->finish_count[event] - uarch->start_count[event];
			trace_update_task_uarch_contrib(cpu_of(rq), tps->p, event,
					uarch->start_count[event], uarch->finish_count[event]);
		}
	}

	raw_spin_unlock_irqrestore(&uarch->lock, flags);
}

static u64 compute_uarch(int cluster, int event, u64 (*count)[MAX_NR_EVENTS])
{
	u64 cycle_count, inst_count, ipc;

	switch (event) {
		case UARCH_IPC:
			cycle_count = count[cluster][PMU_CPU_CYCLES];
			inst_count = count[cluster][PMU_INST_RETIRED];
			ipc = 0;

			if (cycle_count)
				ipc = div64_u64(inst_count << 10, cycle_count);

			return ipc;
	}

	return 0;
}

static void update_hist_uarch(struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;
	struct task_profiler_uarch *uarch;
	int cluster, event;
	bool training = egg_training_task(p);
	bool execution = egg_execution_task(p);
	unsigned long flags;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return;

	/* this task is not a major task */
	if (invalid_egg_index(egg_task_idx(tps->p)))
		return;

	uarch = &tps->uarch;
	raw_spin_lock_irqsave(&uarch->lock, flags);

	/* update uarch contrib */
	for (cluster = 0; cluster < MAX_NR_CLUSTERS; cluster++) {
		for (event = 0; event < MAX_NR_EVENTS; event++) {
			u64 contrib = uarch->contrib[cluster][event];

			if (training) {
				uarch->count_recent[cluster][event] = contrib;
				uarch->count_accum[cluster][event] += contrib;
			}
			else if (execution) {
				uarch->count_recent[cluster][event] = contrib;
				uarch->count_accum[cluster][event] = 0;
			}
			else {
				uarch->count_recent[cluster][event] = 0;
				uarch->count_accum[cluster][event] = 0;
			}

			uarch->contrib[cluster][event] = 0;

			trace_update_task_hist_uarch_raw(tps->p, cluster, event, contrib, training,
					execution, uarch->count_recent[cluster][event],
					uarch->count_accum[cluster][event]);
		}
	}

	/* 2. compute uarch */
	for (cluster = 0; cluster < MAX_NR_CLUSTERS; cluster++) {
		for (event = 0; event < MAX_NR_UARCH; event++) {
			uarch->uarch_recent[cluster][event] =
				compute_uarch(cluster, event, uarch->count_recent);
			uarch->uarch_accum[cluster][event] =
				compute_uarch(cluster, event, uarch->count_accum);
		}
	}

	trace_update_task_hist_uarch(tps->p, cluster, event, uarch, training, execution);

	raw_spin_unlock_irqrestore(&uarch->lock, flags);
}

static void set_initial_value_once(struct task_profiler_struct *tps, u64 now)
{
	struct task_profiler_runnable *runnable = &tps->runnable;
	struct task_profiler_util *util = &tps->util;

	if (!runnable->window_start)
		runnable->window_start = now;

	if (!runnable->window_size)
		runnable->window_size = get_target_window_size();

	if (!runnable->last_updated)
		runnable->last_updated = now;

	if (!util->last_updated)
		util->last_updated = now;
}

/************************************************************************
 *			Binder Contrib Tracking				*
 ************************************************************************/
static void update_binder_contrib(struct task_profiler_struct *tps, int flag)
{
	unsigned long flags;
	u64 binder_contrib;

	if (flag != PICK_NEXT_TASK)
		return;

	binder_contrib = egg_get_binder_contrib(tps->p);

	update_runnable_contrib(tps, binder_contrib);

	raw_spin_lock_irqsave(&tps->util.lock, flags);
	update_util_contrib(tps, binder_contrib);
	raw_spin_unlock_irqrestore(&tps->util.lock, flags);
}

/************************************************************************
 *			Task Profiler Update Function			*
 ************************************************************************/
void task_profiler_update_contrib(struct rq *rq, struct task_struct *p, u64 now, int flag)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	if (get_sched_class(p) != EMS_SCHED_FAIR)
		return;

	if (is_idle_task(p))
		return;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return;

	set_initial_value_once(tps, now);

	update_binder_contrib(tps, flag);
	update_runnable(rq, tps, now, flag);
	update_hist_contrib(rq, tps, now, flag);
	update_uarch_contrib(rq, tps, flag);
}

/************************************************************************
 *			Major Task Update Function			*
 ************************************************************************/
#define MAJOR_COUNT_LIMIT	(10)
#define ADD_COUNT		(1)
#define DECAY_COUNT		(0)
static void update_major_count(struct task_struct *p, int flag)
{
	int next_count;

	if (!p)
		return;

	if (flag == ADD_COUNT) {
		next_count = egg_major_count(p) + 2;
		egg_major_count(p) = min_t(int, next_count, MAJOR_COUNT_LIMIT);
	} else { /* flag == DECAY_COUNT */
		next_count = egg_major_count(p) - 1;
		egg_major_count(p) = max_t(int, next_count, 0);
	}
}

void task_profiler_update(u64 profile_interval, u64 now)
{
	struct ems_task_struct *ets;
	struct task_struct *major_tasks[MAX_NR_MAJOR];
	struct task_profiler_struct *tps;
	int i;
	u64 wakeup_threshold, runnable_threshold;
	unsigned long flags;

	for (i = 0; i < MAX_NR_MAJOR; i++)
		major_tasks[i] = NULL;

	wakeup_threshold = (profile_interval * NSEC_PER_MSEC) / get_target_window_size();
	wakeup_threshold /= 2; /* FIX. margin */

	raw_spin_lock_irqsave(&task_profiler_lock, flags);

	list_for_each_entry(tps, &task_list, grp_list) {
		struct task_struct *p = tps->p, *target;
		s64 wakeup_count;

		update_hist_util(p, now);
		update_hist_uarch(p);
		update_major_count(p, DECAY_COUNT);

		/* the task has not been waking up periodically in recent */
		wakeup_count = tps->wakeup_count - tps->last_updated_wakeup_count;
		tps->skip_add_count = (wakeup_count < wakeup_threshold);

		/* as it has been a major task consistently, this task retains its major status */
		if (egg_major_count(p) > 0)
			goto skip_exception;

		if (wakeup_count < wakeup_threshold)
			goto next;

		if (hist_util_task(p) < min_heavy_threshold)
			goto next;

		/* if the task runs for less than 25% of the window, do not treat it as a major task */
		runnable_threshold = hist_window_task(p) >> 2;
		if (hist_runnable_task(p) < runnable_threshold)
			goto next;

skip_exception:
		/* find top heavy tasks */
		target = p;
		for (i = 0; i < MAX_NR_MAJOR; i++) {
			struct task_struct *temp;
			int target_hist_util, iter_hist_util;

			if (!major_tasks[i]) {
				major_tasks[i] = target;
				break;
			}

			target_hist_util = hist_util_task(target);
			iter_hist_util = hist_util_task(major_tasks[i]);

			if (target_hist_util < iter_hist_util)
				continue;
			else if (target_hist_util == iter_hist_util) {
				if (ml_task_util(target) < ml_task_util(major_tasks[i]))
					continue;
			}

			temp = major_tasks[i];
			major_tasks[i] = target;
			target = temp;
		}

next:
		tps->last_updated_wakeup_count = tps->wakeup_count;

		trace_task_profiler_major_task_snapshot(p, hist_util_task(p), hist_runnable_task(p),
					  hist_window_task(p), prolonged_running_detected(p));
	}

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		ets = (struct ems_task_struct *)android_task_vendor_data(major_tasks[i]);
		tps = (struct task_profiler_struct *)ets->task_profiler;

		if (!tps->skip_add_count)
			update_major_count(major_tasks[i], ADD_COUNT);
	}

	egg_update_major_tasks(major_tasks);

	task_profiler_last_updated = now;

	raw_spin_unlock_irqrestore(&task_profiler_lock, flags);
}

/********************************************************************************/
/*				Task List Handling				*/
/********************************************************************************/
static void task_profiler_attach(struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps = (struct task_profiler_struct *)ets->task_profiler;

	if (tps->attached)
		return;

	list_add(&tps->grp_list, &task_list);
	tps->attached = true;

	trace_task_profiler_handler(p, "attach");
}

static void task_profiler_detach(struct task_struct *p, bool flush)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps = (struct task_profiler_struct *)ets->task_profiler;

	if (!tps->attached)
		goto out;

	list_del_init(&tps->grp_list);
	tps->attached = false;

	trace_task_profiler_handler(p, "detach");
out:
	if (flush) {
		kfree(tps);
		ets->task_profiler = 0;
		trace_task_profiler_handler(p, "flush");
	}
}

static void task_profiler_handler(struct task_struct *p, bool attach, bool flush)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;
	unsigned long flags;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	raw_spin_lock(&task_profiler_lock);

	if (attach)
		task_profiler_attach(p);
	else
		task_profiler_detach(p, flush);

	raw_spin_unlock(&task_profiler_lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);
}

/********************************************************************************/
/*				Vendor Hooks					*/
/********************************************************************************/
void task_profiler_try_to_wake_up(struct rq *rq, struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return;

	task_profiler_update_contrib(rq, p, sched_clock(), WAKEUP_TASK);
	tps->wakeup_count++;

	/* skip historical profile for non top-app tasks */
	if (!is_top_app_task(p)) {
		memset(&tps->util, 0, sizeof(struct task_profiler_util));
		memset(&tps->runnable, 0, sizeof(struct task_profiler_runnable));
	}
}

void task_profiler_init_task(struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (!tps) {
		tps = kzalloc(sizeof(struct task_profiler_struct), GFP_ATOMIC | GFP_NOWAIT);
		if (unlikely(!tps)) {
			pr_err("Failed to allocate task_profiler_struct\n");
			return;
		}
	}

	INIT_LIST_HEAD(&tps->grp_list);
	tps->attached = false;
	tps->p = p;

	raw_spin_lock_init(&tps->util.lock);
	raw_spin_lock_init(&tps->uarch.lock);

	ets->task_profiler = (u64)tps;

	task_profiler_handler(p, is_top_app_task(p), false);
}

void task_profiler_cgroup_attach(struct cgroup_taskset *tset)
{
	struct task_struct *p;
	struct cgroup_subsys_state *css;

	cgroup_taskset_first(tset, &css);
	if (!css)
		return;

	cgroup_taskset_for_each(p, css, tset)
		task_profiler_handler(p, is_top_app_task(p), false);
}

void task_profiler_flush_task(struct task_struct *p)
{
	struct ems_task_struct *ets = (struct ems_task_struct *)android_task_vendor_data(p);
	struct task_profiler_struct *tps;

	tps = (struct task_profiler_struct *)ets->task_profiler;
	if (unlikely(!tps))
		return;

	task_profiler_handler(p, false, true);
}

/**********************************************************************
 *			    SYSFS support			      *
 **********************************************************************/
static struct kobject *task_profiler_kobj;

static ssize_t show_min_heavy_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%lu\n", min_heavy_threshold);
}

static ssize_t store_min_heavy_threshold(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;
	unsigned long flags;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	if (input < 0)
		return -EINVAL;

	raw_spin_lock_irqsave(&task_profiler_lock, flags);
	min_heavy_threshold = input;
	raw_spin_unlock_irqrestore(&task_profiler_lock, flags);

	return count;
}

static struct kobj_attribute min_heavy_threshold_attr =
__ATTR(min_heavy_threshold, 0644, show_min_heavy_threshold, store_min_heavy_threshold);

static int task_profiler_sysfs_init(struct kobject *egg_kobj)
{
	int ret;

	task_profiler_kobj = kobject_create_and_add("task_profiler", egg_kobj);
	if (!task_profiler_kobj) {
		pr_info("%s: fail to create node\n", __func__);
		return -EINVAL;
	}

	ret = sysfs_create_file(task_profiler_kobj, &min_heavy_threshold_attr.attr);
	if (ret)
		pr_warn("%s: failed to create min_heavy_threshold sysfs\n", __func__);

	return 0;
}

void task_profiler_init(struct kobject *egg_kobj)
{
	task_profiler_sysfs_init(egg_kobj);

	min_heavy_threshold = DEFAULT_MIN_HEAVY_THRESHOLD;

	pr_info("EMS: Task Profiler Initialized\n");
}
