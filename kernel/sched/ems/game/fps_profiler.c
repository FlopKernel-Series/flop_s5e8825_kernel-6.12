/*
 * FPS profiler for Exynos Mobile Scheduler
 *
 * Copyright (C) 2021 Samsung Electronics Co., Ltd
 */

#include <soc/samsung/game.h>
#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

#define MAX_WINDOW_SIZE			20
#define DEFAULT_WINDOW_SIZE		10
#define MIN_PROFILE_PERIOD		4

#define MAX_FPS_ERROR			1
#define TARGET_MIN			30
#define TARGET_MAX			120
#define TARGET_GAP			15
#define NUM_OF_TARGET			((TARGET_MAX - TARGET_MIN) / TARGET_GAP) + 1
#define DEFAULT_FRAME_PERIOD		(NSEC_PER_SEC / TARGET_MAX)
#define DEFAULT_MARGIN_RATIO		5		/*	0.5%	*/

#define TARGET_FROM_EMS	0
#define TARGET_FROM_GPU	1

static unsigned int target_list[NUM_OF_TARGET];

struct egg_frame_info {
	u64 target_frame_time;
	u64 current_frame_time;
	u64 cpu_time;
	u64 gpu_time;
	ktime_t update_time;
};

static struct ems_fps_profiler {
	struct ems_profiler_fn		*profile_fn;

	u64				last_frame_cnt;
	u64				current_frame_cnt;
	ktime_t				last_frame_time;
	ktime_t				current_frame_time;
	ktime_t				margin_time;
	ktime_t				last_update_time;

	u64				frame_cnt_hist[MAX_WINDOW_SIZE];
	unsigned int			target_idx_hist[MAX_WINDOW_SIZE];

	unsigned int			target_fps_idx;
	unsigned long long		target_frame_period;	/* nsec */
	unsigned int			current_fps;

	unsigned int			window_size;
	unsigned int			cur_window;

	bool				invalid_efi;
	bool				frame_info_enabled;
	bool				frame_info_stop;
	struct egg_frame_info		avg_efi;
	unsigned int			efi_cur_window;
	struct egg_frame_info		efi[MAX_WINDOW_SIZE];
} efp;

/********************************************************************************
 *				External APIs					*
 ********************************************************************************/
bool get_frame_info_enabled(void)
{
	return efp.frame_info_enabled;
}
EXPORT_SYMBOL_GPL(get_frame_info_enabled);

bool invalid_frame_info(void)
{
	return efp.frame_info_stop || efp.invalid_efi;
}

bool frame_info_stop(void)
{
	return efp.frame_info_stop;
}

void update_fps_profile_time_margin(void)
{
	u64 value = egg_get_update_period() * USEC_PER_MSEC * DEFAULT_MARGIN_RATIO / 1000;
	efp.margin_time = value;
}
EXPORT_SYMBOL_GPL(update_fps_profile_time_margin);

int get_target_fps(void)
{
	if (!efp.invalid_efi)
		return DIV_ROUND_UP_ULL(USEC_PER_SEC, efp.avg_efi.target_frame_time);

	return (efp.profile_fn) ?  target_list[efp.target_fps_idx] : -EINVAL;
}
EXPORT_SYMBOL_GPL(get_target_fps);

int get_target_fps_from(void)
{
	if (!efp.invalid_efi)
		return TARGET_FROM_GPU;

	return (efp.profile_fn) ? TARGET_FROM_EMS : -EINVAL;
}
EXPORT_SYMBOL_GPL(get_target_fps_from);

unsigned long long get_target_frame_period(void)
{
	if (!efp.invalid_efi)
		return efp.avg_efi.target_frame_time * NSEC_PER_USEC;

	return (efp.profile_fn) ? efp.target_frame_period : DEFAULT_FRAME_PERIOD;
}
EXPORT_SYMBOL_GPL(get_target_frame_period);

int get_current_fps(void)
{
	return (efp.profile_fn) ? efp.current_fps : -EINVAL;
}
EXPORT_SYMBOL_GPL(get_current_fps);

int get_avg_cpu_render_us(void)
{
	return efp.avg_efi.cpu_time;
}

int get_avg_gpu_render_us(void)
{
	return efp.avg_efi.gpu_time;
}

void egg_get_frame_info(u64 target_frame_time, u64 current_frame_time,
			u64 cpu_time, u64 gpu_time, u64 user_target_temp, u64 user_cur_temp, int type)
{
	int cur_window = efp.efi_cur_window;

	if (unlikely(!egg_profile_enabled()))
		return;

	efp.efi[cur_window].target_frame_time = target_frame_time;
	efp.efi[cur_window].current_frame_time = current_frame_time;
	efp.efi[cur_window].cpu_time = cpu_time;
	efp.efi[cur_window].gpu_time = gpu_time;
	efp.efi[cur_window].update_time = ktime_get();
	efp.efi_cur_window = move_idx(cur_window, 1, MAX_WINDOW_SIZE);

	if (!current_frame_time && !cpu_time && !gpu_time)
		efp.frame_info_stop = true;
	else
		efp.frame_info_stop = false;

	trace_fps_profiler_frame_info(efp.efi[cur_window].update_time,
			       target_frame_time, current_frame_time,
			       cpu_time, gpu_time,
			       user_target_temp, user_cur_temp, type);

	egg_update_pf_boost_info(target_frame_time, current_frame_time, cpu_time,
				  user_target_temp, user_cur_temp, type);
}
EXPORT_SYMBOL_GPL(egg_get_frame_info);

/********************************************************************************
 *				Profile FPS					*
 ********************************************************************************/
static unsigned int update_target_fps(void)
{
	unsigned int target, window;
	unsigned int max_target = 0;

	efp.cur_window = move_idx(efp.cur_window, 1, MAX_WINDOW_SIZE);

	/* Decision the current window's target FPS */
	for (target = 0; target < NUM_OF_TARGET; ++target) {
		if (efp.current_fps > target_list[target] + MAX_FPS_ERROR)
			continue;

		efp.target_idx_hist[efp.cur_window] = target;

		break;
	}

	/* Find target FPS from frame count history */
	for (window = 0; window < efp.window_size; ++window) {
		if (efp.target_idx_hist[window] < max_target)
			continue;

		max_target = efp.target_idx_hist[window];

		if (max_target == NUM_OF_TARGET - 1)
			break;
	}

	/* compute target frame period */
	efp.target_frame_period = NSEC_PER_SEC / target_list[max_target];

	return max_target;
}

void fps_profiler_average_frame_info(void)
{
	int cnt = 0, window;
	struct egg_frame_info efi = {
		.target_frame_time = 0,
		.current_frame_time = 0,
		.cpu_time = 0,
		.gpu_time = 0,
	};

	if (unlikely(!efp.last_update_time))
		return;

	window = move_idx(efp.efi_cur_window, -1, MAX_WINDOW_SIZE);
	for (cnt = 0; cnt < MAX_WINDOW_SIZE; cnt++) {
		struct egg_frame_info *cur_efi = &efp.efi[window];
		if (cur_efi->update_time < efp.last_update_time)
			break;

		efi.target_frame_time += cur_efi->target_frame_time;
		efi.current_frame_time += cur_efi->current_frame_time;
		efi.cpu_time += cur_efi->cpu_time;
		efi.gpu_time += cur_efi->gpu_time;
		window = move_idx(window, -1, MAX_WINDOW_SIZE);
	}

	if (!cnt) {
		efp.invalid_efi = true;
		efp.avg_efi.target_frame_time = 0;
		efp.avg_efi.current_frame_time = 0;
		efp.avg_efi.cpu_time = 0;
		efp.avg_efi.gpu_time = 0;
		return;
	}

	efp.invalid_efi = false;
	efp.avg_efi.target_frame_time = efi.target_frame_time / cnt;
	efp.avg_efi.current_frame_time = efi.current_frame_time/ cnt;
	efp.avg_efi.cpu_time = efi.cpu_time / cnt;
	efp.avg_efi.gpu_time = efi.gpu_time / cnt;
}

void fps_profiler_update(void)
{
	u64 frame_cnt;
	s64 frame_time;

	if (unlikely(!efp.profile_fn))
		return;

	if (unlikely(!efp.profile_fn->get_frame_cnt))
		return;

	efp.profile_fn->get_frame_cnt(&efp.current_frame_cnt, &efp.current_frame_time);

	frame_time = ktime_to_us(ktime_sub(efp.current_frame_time, efp.last_frame_time));
	frame_time = ktime_sub(frame_time, efp.margin_time);
	frame_cnt = efp.current_frame_cnt - efp.last_frame_cnt;

	efp.current_fps = USEC_PER_SEC * frame_cnt / frame_time;
	efp.target_fps_idx = update_target_fps();

	fps_profiler_average_frame_info();

	trace_fps_profiler_update(efp.avg_efi.target_frame_time, efp.avg_efi.current_frame_time,
			   efp.avg_efi.cpu_time, efp.avg_efi.gpu_time,
			   frame_cnt, frame_time, get_target_fps(), efp.current_fps);

	efp.last_frame_cnt = efp.current_frame_cnt;
	efp.last_frame_time = efp.current_frame_time;
	efp.last_update_time = ktime_get();
}

/********************************************************************************
 *				Sysfs node					*
 ********************************************************************************/
static ssize_t window_size_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "window_size=%d (max=%d)\n", efp.window_size, MAX_WINDOW_SIZE);
}

static ssize_t window_size_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int size;

	if (sscanf(buf, "%d", &size) != 1)
		return -EINVAL;

	if (size < 0)
		size = 0;

	if (size >= MAX_WINDOW_SIZE)
		size = MAX_WINDOW_SIZE;

	efp.window_size = size;

	return count;
}
static struct kobj_attribute window_size_attr =
__ATTR(window_size, 0644, window_size_show, window_size_store);

static ssize_t frame_info_enabled_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "frame_info_enabled=%d frame_info_stop=%d\n",
				efp.frame_info_enabled, efp.frame_info_stop);
}

static ssize_t frame_info_enabled_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int input;

	if (sscanf(buf, "%d", &input) != 1)
		return -EINVAL;

	efp.frame_info_enabled = !!input;
	return count;
}
static struct kobj_attribute frame_info_enabled_attr =
__ATTR(frame_info_enabled, 0644, frame_info_enabled_show, frame_info_enabled_store);

static struct attribute *efp_attrs[] = {
	&window_size_attr.attr,
	&frame_info_enabled_attr.attr,
	NULL,
};

static struct attribute_group efp_attr_group = {
	.name		= "fps_profiler",
	.attrs		= efp_attrs,
};

/********************************************************************************
 *				Initialization					*
 ********************************************************************************/
void ems_profiler_register_fn(struct ems_profiler_fn *fn)
{
	efp.profile_fn = fn;
	if (efp.profile_fn) {
		efp.profile_fn->get_frame_cnt(&efp.last_frame_cnt, &efp.last_frame_time);
		efp.cur_window = 0;
		efp.window_size = DEFAULT_WINDOW_SIZE;
	}
}
EXPORT_SYMBOL_GPL(ems_profiler_register_fn);

int fps_profiler_init(struct kobject *egg_kobj)
{
	int i;
	int target = TARGET_MIN;

	efp.efi_cur_window = 0;
	efp.frame_info_enabled = true;
	efp.frame_info_stop = false;
	efp.invalid_efi = true;

	if (sysfs_create_group(egg_kobj, &efp_attr_group)) {
		pr_err("%s:failed to create sysfs\n", __func__);
		return -ENOMEM;
	}

	for (i = 0; i < NUM_OF_TARGET; ++i) {
		target_list[i] = target;
		target += TARGET_GAP;
	}

	if (unlikely(target_list[NUM_OF_TARGET - 1] > TARGET_MAX)) {
		pr_err("%s:failed to create target_fps_list\n", __func__);
		return -EINVAL;
	}

	pr_info("EMS: FPS Profiler Initialized\n");

	return 0;
}
