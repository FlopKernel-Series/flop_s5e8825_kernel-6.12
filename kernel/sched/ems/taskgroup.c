/*
 * Exynos Mobile Scheduler taskgroup
 *
 * Copyright (C) 2023 Samsung Electronics Co., Ltd
 */

#include <soc/samsung/ems.h>
#include <trace/events/ems_debug.h>

enum taskgroup_type {
	TASK_GROUP,
	PROC_GROUP,
};

struct ems_task_group {
	struct kobject kobj;
	int cgroup;
	unsigned int uclamp_pct[UCLAMP_CNT];
	struct uclamp_se uc_req[UCLAMP_CNT];
};

/* pe_list init index request  */
struct pl_request {
	int target_pl_index;
	int cgroup_mask;
};

static int pl_req;
static int max_req;
static struct pl_request *req_entry;

/* from tune.c */
extern char *task_cgroup_name[];
extern char *task_cgroup_simple_name[];
extern int task_cgroup_name_size;

static inline bool check_cred(struct task_struct *p)
{
	const struct cred *cred, *tcred;
	bool ret = true;

	cred = current_cred();
	tcred = get_task_cred(p);
	if (!uid_eq(cred->euid, GLOBAL_ROOT_UID) &&
			!uid_eq(cred->euid, tcred->uid) &&
			!uid_eq(cred->euid, tcred->suid) &&
			!ns_capable(tcred->user_ns, CAP_SYS_NICE)) {
		ret = false;
	}
	put_cred(tcred);
	return ret;
}

static void move_taskgroup(struct task_struct *p, unsigned int cgroup)
{
	int queued, running, queue_flags =
		DEQUEUE_SAVE | DEQUEUE_NOCLOCK;
	struct rq *rq;
	struct ems_task_struct *ep;
	struct rq_flags rf;

	rq = task_rq_lock(p, &rf);

	ep = (struct ems_task_struct *)android_task_vendor_data(p);
	if (ep->cgroup == cgroup) {
		task_rq_unlock(rq, p, &rf);
		return;
	}

	update_rq_clock(rq);

	running = task_current(rq, p);
	queued = task_on_rq_queued(p);

	if (queued)
		deactivate_task(rq, p, queue_flags);
	if (running)
		put_prev_task(rq, p);

	ep->cgroup = cgroup;

	if (queued)
		activate_task(rq, p, queue_flags);
	if (running) {
		set_next_task(rq ,p);
		/*
		 * After changing group, the running task may have joined a
		 * throttled one but it's still the running task. Trigger a
		 * resched to make sure that task can still run.
		 */
		resched_curr(rq);
	}

	task_rq_unlock(rq, p, &rf);
}

static int update_taskgroup(const char *buf, enum taskgroup_type type,
                                         unsigned int cgroup)
{
	struct task_struct *p, *t;
	pid_t pid;

	if (kstrtoint(buf, 0, &pid) || pid <= 0)
		return -EINVAL;

	rcu_read_lock();
	p = find_task_by_vpid(pid);
	if (!p) {
		rcu_read_unlock();
		return -ESRCH;
	}

	get_task_struct(p);

	if (!check_cred(p)) {
		put_task_struct(p);
		rcu_read_unlock();
		return -EACCES;
	}

	switch (type) {
        case TASK_GROUP:
		move_taskgroup(p, cgroup);
		if (gsc_enabled())
			gsc_manage_task_group(p, cgroup == CGROUP_TOPAPP, false);

		trace_ems_update_taskgroup(p, cgroup);
		break;
        case PROC_GROUP:
		for_each_thread(p, t) {
			get_task_struct(t);

			move_taskgroup(t, cgroup);
			if (gsc_enabled())
				gsc_manage_task_group(t, cgroup == CGROUP_TOPAPP, false);

			put_task_struct(t);
			trace_ems_update_taskgroup(t, cgroup);
		}
		break;
	default:
		break;
        }

	put_task_struct(p);
	rcu_read_unlock();
	return 0;
}

static ssize_t proc_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct ems_task_group *tg = container_of(kobj, struct ems_task_group, kobj);
	struct task_struct *p;
	int ret = 0;

	rcu_read_lock();
	for_each_process(p) {
		struct ems_task_struct *ep;

		ep = (struct ems_task_struct *)android_task_vendor_data(p);
		if (ep->cgroup == tg->cgroup)
			ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%u\n", p->pid);
	}
	rcu_read_unlock();

	return ret;
}

static ssize_t proc_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;
	struct ems_task_group *tg = container_of(kobj, struct ems_task_group, kobj);

	ret = update_taskgroup(buf, PROC_GROUP, tg->cgroup);
	return ret ?: count;
}

static struct kobj_attribute proc_attr = __ATTR_RW(proc);

static ssize_t task_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct ems_task_group *tg = container_of(kobj, struct ems_task_group, kobj);
	struct task_struct *g, *p;
	int ret = 0;

	rcu_read_lock();
	for_each_process_thread(g, p) {
		struct ems_task_struct *ep;

		ep = (struct ems_task_struct *)android_task_vendor_data(p);
		if (ep->cgroup == tg->cgroup)
			ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%u\n", p->pid);
	}
	rcu_read_unlock();

	return ret;
}

static ssize_t task_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;
	struct ems_task_group *tg = container_of(kobj, struct ems_task_group, kobj);

	ret = update_taskgroup(buf, TASK_GROUP, tg->cgroup);
	return ret ?: count;
}

static struct kobj_attribute task_attr = __ATTR_RW(task);

#define get_bucket_id(__val)								\
		min_t(unsigned int,							\
		      __val / DIV_ROUND_CLOSEST(SCHED_CAPACITY_SCALE, UCLAMP_BUCKETS),	\
		      UCLAMP_BUCKETS - 1)

static u64 power_of_ten(int power)
{
        u64 v = 1;
        while (power--)
                v *= 10;
        return v;
}

/**
 * parse_float - parse a floating number
 * @input: input string
 * @dec_shift: number of decimal digits to shift
 * @v: output
 *
 * Parse a decimal floating point number in @input and store the result in
 * @v with decimal point right shifted @dec_shift times.  For example, if
 * @input is "12.3456" and @dec_shift is 3, *@v will be set to 12345.
 * Returns 0 on success, -errno otherwise.
 *
 * There's nothing cgroup specific about this function except that it's
 * currently the only user.
 */
int parse_float(const char *input, unsigned dec_shift, s64 *v)
{
        s64 whole, frac = 0;
        int fstart = 0, fend = 0, flen;

        if (!sscanf(input, "%lld.%n%lld%n", &whole, &fstart, &frac, &fend))
                return -EINVAL;
        if (frac < 0)
                return -EINVAL;

        flen = fend > fstart ? fend - fstart : 0;
        if (flen < dec_shift)
                frac *= power_of_ten(dec_shift - flen);
        else
                frac = DIV_ROUND_CLOSEST_ULL(frac, power_of_ten(flen - dec_shift));

        *v = whole * power_of_ten(dec_shift) + frac;
        return 0;
}

/*
 * Integer 10^N with a given N exponent by casting to integer the literal "1eN"
 * C expression. Since there is no way to convert a macro argument (N) into a
 * character constant, use two levels of macros.
 */
#define _POW10(exp) ((unsigned int)1e##exp)
#define POW10(exp) _POW10(exp)

struct uclamp_request {
#define UCLAMP_PERCENT_SHIFT    2
#define UCLAMP_PERCENT_SCALE    (100 * POW10(UCLAMP_PERCENT_SHIFT))
        s64 percent;
        u64 util;
        int ret;
};

static inline struct uclamp_request
capacity_from_percent(const char *buf)
{
	char *cbuf = (char *)buf;
        struct uclamp_request req = {
                .percent = UCLAMP_PERCENT_SCALE,
                .util = SCHED_CAPACITY_SCALE,
                .ret = 0,
        };

        cbuf = strim(cbuf);
        if (strcmp(buf, "max")) {
                req.ret = parse_float(cbuf, UCLAMP_PERCENT_SHIFT,
                                             &req.percent);
                if (req.ret)
                        return req;
                if ((u64)req.percent > UCLAMP_PERCENT_SCALE) {
                        req.ret = -ERANGE;
                        return req;
                }

                req.util = req.percent << SCHED_CAPACITY_SHIFT;
                req.util = DIV_ROUND_CLOSEST_ULL(req.util, UCLAMP_PERCENT_SCALE);
        }

        return req;
}

static ssize_t uclamp_min_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct ems_task_group *tg = container_of(kobj, struct ems_task_group, kobj);
	u64 util_clamp;
        u64 percent;
        u32 rem;

	util_clamp = tg->uc_req[UCLAMP_MIN].value;

	if (util_clamp == SCHED_CAPACITY_SCALE) {
		return sprintf(buf, "max\n");
	}

	percent = tg->uclamp_pct[UCLAMP_MIN];
        percent = div_u64_rem(percent, POW10(UCLAMP_PERCENT_SHIFT), &rem);
	return sprintf(buf, "%llu.%0*u\n", percent, UCLAMP_PERCENT_SHIFT, rem);
}

static ssize_t uclamp_min_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct ems_task_group *tg = container_of(kobj, struct ems_task_group, kobj);
        struct uclamp_request req;

        req = capacity_from_percent(buf);
        if (req.ret)
                return req.ret;

	if (req.util == tg->uc_req[UCLAMP_MIN].value)
		return count;

	tg->uc_req[UCLAMP_MIN].value = req.util;
	tg->uc_req[UCLAMP_MIN].bucket_id = get_bucket_id(req.util);
	tg->uc_req[UCLAMP_MIN].user_defined = false;

	tg->uclamp_pct[UCLAMP_MIN] = req.percent;

	trace_ems_update_uclamp_min(tg->cgroup, req.util);
	return count;
}

static struct kobj_attribute uclamp_min_attr = __ATTR_RW(uclamp_min);

static ssize_t uclamp_max_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct ems_task_group *tg = container_of(kobj, struct ems_task_group, kobj);
	u64 util_clamp;
	u64 percent;
        u32 rem;

	util_clamp = tg->uc_req[UCLAMP_MAX].value;

	if (util_clamp == SCHED_CAPACITY_SCALE) {
		return sprintf(buf, "max\n");
	}

	percent = tg->uclamp_pct[UCLAMP_MAX];
        percent = div_u64_rem(percent, POW10(UCLAMP_PERCENT_SHIFT), &rem);
	return sprintf(buf, "%llu.%0*u\n", percent, UCLAMP_PERCENT_SHIFT, rem);
}

static ssize_t uclamp_max_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct ems_task_group *tg = container_of(kobj, struct ems_task_group, kobj);
        struct uclamp_request req;

        req = capacity_from_percent(buf);
        if (req.ret)
                return req.ret;

        if (req.util == tg->uc_req[UCLAMP_MAX].value)
                return count;

	tg->uc_req[UCLAMP_MAX].value = req.util;
	tg->uc_req[UCLAMP_MAX].bucket_id = get_bucket_id(req.util);
	tg->uc_req[UCLAMP_MAX].user_defined = false;

	tg->uclamp_pct[UCLAMP_MAX] = req.percent;

	trace_ems_update_uclamp_max(tg->cgroup, req.util);
	return count;
}

static struct kobj_attribute uclamp_max_attr = __ATTR_RW(uclamp_max);

static struct attribute *taskgroup_attrs[] = {
	&proc_attr.attr,
	&task_attr.attr,
	&uclamp_min_attr.attr,
	&uclamp_max_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(taskgroup);

static struct kobj_type ktype_taskgroup = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = taskgroup_groups,
};

struct ems_task_group ems_group[CGROUP_COUNT];

struct uclamp_se
ems_uclamp_tg_restrict(struct task_struct *p, enum uclamp_id clamp_id)
{
	/* Copy by value as we could modify it */
	struct uclamp_se uc_req = p->uclamp_req[clamp_id];
	struct ems_task_struct *ep = (struct ems_task_struct *)android_task_vendor_data(p);

#ifdef CONFIG_UCLAMP_TASK_GROUP
	unsigned int tg_min, tg_max, vnd_min, vnd_max, value;

        /*
         * Tasks in root task group will be
         * restricted by system defaults.
         */
	if (task_group(p) == &root_task_group) {
		tg_min = 0;
		tg_max = SCHED_CAPACITY_SCALE;
	} else {
		// task group restriction
		tg_min = task_group(p)->uclamp[UCLAMP_MIN].value;
		tg_max = task_group(p)->uclamp[UCLAMP_MAX].value;
	}

	// vendor group restriction
	vnd_min = ems_group[ep->cgroup].uc_req[UCLAMP_MIN].value;
	vnd_max = ems_group[ep->cgroup].uc_req[UCLAMP_MAX].value;

	value = uc_req.value;
	value = clamp(value, max(tg_min, vnd_min), min(tg_max, vnd_max));

	uc_req.value = value;
	uc_req.bucket_id = get_bucket_id(value);
	uc_req.user_defined = false;
#endif

	return uc_req;
}

void taskgroup_enqueue_task(struct rq *rq, struct task_struct *p, int *flags)
{
	unsigned long uclamp_min_util;

	uclamp_min_util = uclamp_eff_value(p, UCLAMP_MIN);

	if (uclamp_min_util > 0)
		*flags |= UCLAMP_TASK;
}

int taskgroup_pl_init_index(struct task_struct *p)
{
	int enabled, idx;

	if (!req_entry)
		return 0;

	idx = READ_ONCE(pl_req);
	if (idx > max_req || idx < 0)
		return -EINVAL;

	enabled = req_entry[idx].cgroup_mask & (1 << cpuctl_task_group_idx(p));

	return (!!enabled) ? req_entry[idx].target_pl_index : 0;
}

static int debug_pid = 1;

static ssize_t debug_group_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int ret;
	struct task_struct *p;
	struct ems_task_struct *ep;

	rcu_read_lock();
	p = find_task_by_vpid(debug_pid);
	if (!p) {
		rcu_read_unlock();
		return -ESRCH;
	}

	get_task_struct(p);
	ep = (struct ems_task_struct *)android_task_vendor_data(p);
	put_task_struct(p);
	rcu_read_unlock();

	ret = sprintf(buf, "comm=%s pid=%d taskgroup=%s(%d)\n", p->comm,
			p->pid, task_cgroup_name[ep->cgroup], ep->cgroup);

	return ret;
}

static ssize_t debug_group_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int pid;

	if (sscanf(buf, "%d", &pid) != 1)
		return -EINVAL;

	debug_pid = pid;

	return count;
}

static struct kobj_attribute debug_group_attr = __ATTR_RW(debug_group);

static ssize_t pl_req_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int i, cgroup;
	int ret = 0;

	if (!req_entry) {
		ret += sprintf(buf + ret, "pl_req not support");
		return ret;
	}

	ret += sprintf(buf + ret, "REQ |");
	for (cgroup = 0; cgroup < task_cgroup_name_size; cgroup++) {
		ret += sprintf(buf + ret, " %4s", task_cgroup_simple_name[cgroup]);
	}
	ret += sprintf(buf + ret, "\n");

	for (i = 0; i <= max_req; i++) {
		ret += sprintf(buf + ret, "%3d |", i);
		for (cgroup = 0; cgroup < task_cgroup_name_size; cgroup++) {
			int enabled = req_entry[i].cgroup_mask & (1 << cgroup);
			ret += sprintf(buf + ret, " %4d",
						(!!enabled) ? req_entry[i].target_pl_index : 0);
		}
		ret += sprintf(buf + ret, "\n");
	}

	ret += sprintf(buf + ret, "Request:%d\n\n", READ_ONCE(pl_req));

	return ret;
}

static ssize_t pl_req_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int request;
	int prev = READ_ONCE(pl_req);

	if (sscanf(buf, "%d", &request) != 1)
		return -EINVAL;

	if (!req_entry)
		return -EINVAL;

	if (request > max_req || request < 0)
		return -EINVAL;

	if (prev == request)
		return count;

	WRITE_ONCE(pl_req, request);

	trace_ems_update_pl_request(request, prev);
	return count;
}

static struct kobj_attribute pl_req_attr = __ATTR_RW(pl_req);

static void taskgroup_pl_req_free(void)
{
	if (req_entry) {
		kfree(req_entry);
		req_entry = NULL;
		max_req = 0;
		pl_req = 0;
	}
}

static int taskgroup_pl_req_parse(struct device_node *np)
{
	struct device_node *child;
	int ret = 0, req_idx = 0;
	int cgroup;
	int req_count;

	req_count = of_get_child_count(np);
	if (req_count <= 0) {
		pr_err("no requests found\n");
		ret = -EINVAL;
		goto out_put_node;
	}

	/* There is always req0 */
	req_entry = kcalloc(req_count + 1, sizeof(*req_entry), GFP_KERNEL);
	if (!req_entry) {
		pr_err("cant alloc req entry\n");
		ret = -ENOMEM;
		goto out_put_node;
	}

	for_each_child_of_node(np, child) {
		u32 val;

		/* Because req0, request idx start with 1 */
		++req_idx;
		if (!of_property_read_u32(child, "target-pl-index", &val))
			req_entry[req_idx].target_pl_index = val;

		for (cgroup = 0; cgroup < task_cgroup_name_size; cgroup++) {
			if (!of_property_read_u32(child, task_cgroup_name[cgroup], &val))
				req_entry[req_idx].cgroup_mask |= (1 << cgroup);
		}
	}

	max_req = req_idx;

out_put_node:
	of_node_put(np);
	return ret;
}

int task_group_init(struct kobject *ems_kobj)
{
	struct device_node *dn;
	struct kobject *tg_kobj;
	int i;
	unsigned int min = 0;
	unsigned int max = SCHED_CAPACITY_SCALE;

	tg_kobj = kobject_create_and_add("taskgroup", ems_kobj);

	if (sysfs_create_file(tg_kobj, &debug_group_attr.attr)) {
		pr_err("sysfs_create_file dev_attr_debug_group error!\n");
	}

	for (i = 0; i < task_cgroup_name_size; i++) {
		ems_group[i].cgroup = i;
		ems_group[i].uc_req[UCLAMP_MIN].value = min;
		ems_group[i].uc_req[UCLAMP_MIN].bucket_id = get_bucket_id(min);
		ems_group[i].uc_req[UCLAMP_MIN].user_defined = false;
		ems_group[i].uc_req[UCLAMP_MAX].value = max;
		ems_group[i].uc_req[UCLAMP_MAX].bucket_id = get_bucket_id(max);
		ems_group[i].uc_req[UCLAMP_MAX].user_defined = false;

		if (i > 0 && kobject_init_and_add(&ems_group[i].kobj,
				&ktype_taskgroup, tg_kobj, "%s",
				task_cgroup_name[i])) {
			pr_err("sysfs_create_group error!\n");
			return -1;
		}
	}

	dn = of_find_node_by_path("/ems/taskgroup");
	if (!dn) {
		pr_err("missing taskgroup node in ems device tree\n");
		return 0;
	}

	if (taskgroup_pl_req_parse(dn)) {
		pr_err("cant parse taskgroup node\n");
		taskgroup_pl_req_free();
		return 0;
	}

	if (sysfs_create_file(tg_kobj, &pl_req_attr.attr)) {
		pr_err("sysfs_create_file dev_attr_pl_req error!\n");
		taskgroup_pl_req_free();
		return 0;
	}

	return 0;
}

