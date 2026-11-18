/*
 * Exynos Core Sparing Governor - Exynos Mobile Scheduler
 *
 * Copyright (C) 2022 Samsung Electronics Co., Ltd
 * Youngtae Lee <yt0729.lee@samsung.com>
 */

#include <soc/samsung/ems.h>

#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

static struct {
	struct cpumask			cpus;
	struct cpumask			user_cpu_max;
	struct cpumask			user_cpu_min;

	struct ecs_governor		*gov;
	struct list_head		gov_list;

	/* ecs MAX request */
	struct list_head		requests_max;
	struct cpumask			requested_max_cpus;

	/* ecs RECOMMEND request */
	struct list_head		requests_recommend;
	struct cpumask			requested_recommend_cpus;

	/* ecs MIN request */
	struct list_head		requests_min;
	struct cpumask			requested_min_cpus;

	struct kobject *ecs_kobj;
	struct kobject *governor_kobj;
} ecs;

static DEFINE_RAW_SPINLOCK(ecs_lock);
static DEFINE_RAW_SPINLOCK(ecs_req_lock);

static DEFINE_PER_CPU(int, ecs_tick_count);

/******************************************************************************
 *                                HELP Function				      *
 ******************************************************************************/
static inline const struct cpumask *get_governor_cpus(void)
{
	if (likely(ecs.gov))
		return ecs.gov->get_target_cpus();

	return cpu_possible_mask;
}

const struct cpumask *ecs_max_cpus(void)
{
	return &ecs.requested_max_cpus;
}

/******************************************************************************
 *                                core sparing	                              *
 ******************************************************************************/
static struct cpu_stop_work __percpu *ecs_migration_work;

static void detach_any_task(struct rq *src_rq, struct rq *dst_rq,
				struct task_struct *p, struct list_head *tasks)
{
	if (!can_migrate(p, dst_rq->cpu))
		return;

	detach_task(src_rq, dst_rq, p);
	list_add(&p->se.group_node, tasks);
}

static void detach_all_tasks(struct rq *src_rq,
		struct rq *dst_rq, struct list_head *tasks)
{
	struct task_struct *p, *n;

	update_rq_clock(src_rq);

	/* detach cfs tasks */
	list_for_each_entry_safe(p, n,
			&src_rq->cfs_tasks, se.group_node)
		detach_any_task(src_rq, dst_rq, p, tasks);

	/* detach rt tasks */
	plist_for_each_entry_safe(p, n,
			&src_rq->rt.pushable_tasks, pushable_tasks)
		detach_any_task(src_rq, dst_rq, p, tasks);
}

static void attach_all_tasks(struct rq *dst_rq, struct list_head *tasks)
{
	struct task_struct *p;
	struct rq_flags rf;

	rq_lock(dst_rq, &rf);
	update_rq_clock(dst_rq);

	while (!list_empty(tasks)) {
		p = list_first_entry(tasks, struct task_struct, se.group_node);
		list_del_init(&p->se.group_node);

		attach_task(dst_rq, p);
	}

	rq_unlock(dst_rq, &rf);
}

static int ecs_migration_cpu_stop(void *data)
{
	struct rq *src_rq = this_rq(), *dst_rq = data;
	struct list_head tasks = LIST_HEAD_INIT(tasks);
	struct rq_flags rf;

	rq_lock_irq(src_rq, &rf);
	detach_all_tasks(src_rq, dst_rq, &tasks);
	src_rq->active_balance = 0;
	ems_clear_migrated(dst_rq);
	rq_unlock(src_rq, &rf);

	if (!list_empty(&tasks))
		attach_all_tasks(dst_rq, &tasks);

	local_irq_enable();

	return 0;
}

static void __move_from_spared_cpus(int src_cpu, int dst_cpu)
{
	struct rq *rq = cpu_rq(src_cpu);
	struct rq *dst_rq = cpu_rq(dst_cpu);
	unsigned long flags;
	bool success;

	if (unlikely(src_cpu == dst_cpu))
		return;

	raw_spin_rq_lock_irqsave(rq, flags);

	if (rq->active_balance) {
		raw_spin_rq_unlock_irqrestore(rq, flags);
		return;
	}

	rq->active_balance = 1;
	ems_set_migrated(dst_rq);

	raw_spin_rq_unlock_irqrestore(rq, flags);

	/* Migrate all tasks from src to dst through stopper */
	success = stop_one_cpu_nowait(src_cpu, ecs_migration_cpu_stop, dst_rq,
			per_cpu_ptr(ecs_migration_work, src_cpu));

	if (!success)
		ems_clear_migrated(dst_rq);
}

static void move_from_spared_cpus(struct cpumask *spared_cpus)
{
	int fcpu, idlest_cpu = 0;

	if (cpumask_empty(spared_cpus))
		return;

	for_each_cpu(fcpu, cpu_active_mask) {
		int cpu, min_util = INT_MAX;

		/* To loop each domain */
		if (fcpu != cpumask_first(cpu_sched_domain_mask(fcpu)))
			continue;

		/* To find idlest cpu in this domain */
		for_each_cpu_and(cpu, &ecs.cpus, cpu_sched_domain_mask(fcpu)) {
			struct rq *rq = cpu_rq(cpu);
			int cpu_util;

			cpu_util = ml_cpu_util(cpu) + cpu_util_rt(rq);
			if (min_util < cpu_util)
				continue;

			min_util = cpu_util;
			idlest_cpu = cpu;
		}

		/* Move task to idlest cpu */
		for_each_cpu_and(cpu, spared_cpus, cpu_sched_domain_mask(fcpu)) {
			if (available_idle_cpu(cpu))
				continue;
			__move_from_spared_cpus(cpu, idlest_cpu);
		}
	}
}

static void ecs_wakeup_cpus(struct cpumask *wakeup_cpus)
{
	int cpu;

	if (cpumask_empty(wakeup_cpus))
		return;

	for_each_cpu(cpu, wakeup_cpus)
		lb_smp_call_newidle_balance(cpu);
}

void update_ecs_cpus(bool migrate)
{
	struct cpumask prev_cpus, update_cpus, wakeup_cpus, spared_cpus;
	unsigned long flags;

	if (unlikely(ems_disabled()))
		return;

	raw_spin_lock_irqsave(&ecs_lock, flags);

	cpumask_copy(&prev_cpus, &ecs.cpus);

	cpumask_and(&ecs.cpus, get_governor_cpus(), cpu_active_mask);
	cpumask_or(&ecs.cpus, &ecs.cpus, &ecs.requested_min_cpus);
	cpumask_and(&ecs.cpus, &ecs.cpus, &ecs.requested_recommend_cpus);
	cpumask_and(&ecs.cpus, &ecs.cpus, &ecs.requested_max_cpus);

	cpumask_xor(&update_cpus, &ecs.cpus, &prev_cpus);

	cpumask_and(&wakeup_cpus, &update_cpus, &ecs.cpus);
	cpumask_andnot(&spared_cpus, &update_cpus, &ecs.cpus);

	if (migrate) {
		ecs_wakeup_cpus(&wakeup_cpus);
		move_from_spared_cpus(&spared_cpus);
	}

	trace_ecs_update(*(unsigned int *)cpumask_bits(&ecs.requested_min_cpus),
			*(unsigned int *)cpumask_bits(&ecs.requested_recommend_cpus),
			*(unsigned int *)cpumask_bits(&ecs.requested_max_cpus),
			*(unsigned int *)cpumask_bits(&ecs.cpus));

	raw_spin_unlock_irqrestore(&ecs_lock, flags);
}

int ecs_cpu_available(int cpu, struct task_struct *p)
{
	if (p && is_per_cpu_kthread(p))
		return true;

	return cpumask_test_cpu(cpu, &ecs.cpus);
}

const struct cpumask *ecs_available_cpus(void)
{
	return &ecs.cpus;
}

const struct cpumask *ecs_cpus_allowed(struct task_struct *p)
{
	if (p && is_per_cpu_kthread(p))
		return cpu_active_mask;

	return &ecs.cpus;
}

void ecs_enqueue_update(int prev_cpu, struct task_struct *p)
{
	if (ecs.gov && ecs.gov->enqueue_update)
		ecs.gov->enqueue_update(prev_cpu, p);
}

void ecs_update(void)
{
	if (likely(ecs.gov))
		ecs.gov->update();
}

void ecs_retry_sparing(struct rq *rq)
{
	unsigned long flags;
	cpumask_t spared_cpu;

	if (egg_enabled())
		return;

	/* sched_tick on spared cpu */
	if (!cpumask_test_cpu(cpu_of(rq), ecs_available_cpus())) {
		cpumask_t test;

		cpumask_and(&test, &rq->curr->cpus_mask, ecs_cpus_allowed(rq->curr));
		if (!cpumask_empty(&test) && !ecs_cpu_available(cpu_of(rq), rq->curr)) {
			int count = this_cpu_add_return(ecs_tick_count, 1);

			/* 1 tick is possible during migrating
			 * 2 tick -> retry sparing
			 */
			if (count > 1) {
				raw_spin_lock_irqsave(&ecs_lock, flags);
				cpumask_set_cpu(cpu_of(rq), &spared_cpu);
				move_from_spared_cpus(&spared_cpu);
				raw_spin_unlock_irqrestore(&ecs_lock, flags);
			}
		}
	} else {
		this_cpu_write(ecs_tick_count, 0);
	}
}

static void ecs_governor_set(struct ecs_governor *gov)
{
	if (ecs.gov == gov)
		return;

	/* stop current working governor */
	if (ecs.gov)
		ecs.gov->stop();

	/* change governor pointer and start new governor */
	gov->start();
	ecs.gov = gov;
}

void ecs_governor_register(struct ecs_governor *gov, bool default_gov)
{
	list_add(&gov->list, &ecs.gov_list);

	if (default_gov)
		ecs_governor_set(gov);
}

/******************************************************************************
 *                               ECS requestes                                *
 ******************************************************************************/
struct ecs_request {
	char			name[ECS_USER_NAME_LEN];
	struct cpumask		cpus;
	struct list_head	list;
};

static struct ecs_request *ecs_find_request(char *name, enum ecs_type type)
{
	struct ecs_request *req;
	struct list_head *head;

	switch(type) {
	case ECS_MIN:
		head = &ecs.requests_min;
		break;
	case ECS_RECOMMEND:
		head = &ecs.requests_recommend;
		break;
	case ECS_MAX:
		head = &ecs.requests_max;
		break;
	default:
		return NULL;
	}

	list_for_each_entry(req, head, list)
		if (!strcmp(req->name, name))
			return req;

	return NULL;
}

static void ecs_request_combine_and_apply(bool migrate)
{
	struct cpumask max_mask;
	struct cpumask recommend_mask;
	struct cpumask min_mask;
	struct ecs_request *req;
	char buf[10];

	cpumask_setall(&max_mask);
	cpumask_setall(&recommend_mask);
	cpumask_clear(&min_mask);

	list_for_each_entry(req, &ecs.requests_max, list)
		cpumask_and(&max_mask, &max_mask, &req->cpus);

	if (cpumask_empty(&max_mask) || !cpumask_test_cpu(0, &max_mask)) {
		scnprintf(buf, sizeof(buf), "%*pbl", cpumask_pr_args(&max_mask));
		panic("ECS cpumask(%s) is wrong\n", buf);
	}
	cpumask_copy(&ecs.requested_max_cpus, &max_mask);

	list_for_each_entry(req, &ecs.requests_recommend, list)
		cpumask_and(&recommend_mask, &recommend_mask, &req->cpus);

	cpumask_and(&recommend_mask, &recommend_mask, &max_mask);
	cpumask_copy(&ecs.requested_recommend_cpus, &recommend_mask);

	list_for_each_entry(req, &ecs.requests_min, list)
		cpumask_or(&min_mask, &min_mask, &req->cpus);

	cpumask_and(&min_mask, &min_mask, cpu_possible_mask);
	cpumask_copy(&ecs.requested_min_cpus, &min_mask);

	update_ecs_cpus(migrate);
}

int ecs_request_register(char *name, const struct cpumask *mask, enum ecs_type type)
{
	struct ecs_request *req;
	char buf[ECS_USER_NAME_LEN];
	unsigned long flags;

	/* allocate memory for new request */
	req = kzalloc(sizeof(struct ecs_request), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	raw_spin_lock_irqsave(&ecs_req_lock, flags);

	/* check whether name is already register or not */
	if (ecs_find_request(name, type))
		panic("Failed to register ecs request! already existed\n");

	/* init new request information */
	if (mask)
		cpumask_copy(&req->cpus, mask);
	else
		cpumask_clear(&req->cpus);
	strcpy(req->name, name);

	/* register request list */
	if (type == ECS_MAX)
		list_add(&req->list, &ecs.requests_max);
	else if (type == ECS_RECOMMEND)
		list_add(&req->list, &ecs.requests_recommend);
	else
		list_add(&req->list, &ecs.requests_min);

	scnprintf(buf, sizeof(buf), "%*pbl", cpumask_pr_args(&req->cpus));
	pr_info("Register new ECS request [name:%s, mask:%s, type=%d]\n",
				req->name, buf, type);

	/* applying new request */
	ecs_request_combine_and_apply(true);

	raw_spin_unlock_irqrestore(&ecs_req_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(ecs_request_register);

/* remove request on the request list of exynos_core_sparing request */
int ecs_request_unregister(char *name, enum ecs_type type)
{
	struct ecs_request *req;
	unsigned long flags;

	raw_spin_lock_irqsave(&ecs_req_lock, flags);

	req = ecs_find_request(name, type);
	if (!req) {
		raw_spin_unlock_irqrestore(&ecs_req_lock, flags);
		return -ENODEV;
	}

	/* remove request from list and free */
	list_del(&req->list);
	kfree(req);

	ecs_request_combine_and_apply(true);

	raw_spin_unlock_irqrestore(&ecs_req_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(ecs_request_unregister);

static int __ecs_request(char *name, const struct cpumask *mask, enum ecs_type type, bool migrate)
{
	struct ecs_request *req;
	unsigned long flags;

	raw_spin_lock_irqsave(&ecs_req_lock, flags);

	req = ecs_find_request(name, type);
	if (!req) {
		raw_spin_unlock_irqrestore(&ecs_req_lock, flags);
		return -EINVAL;
	}

	if (cpumask_equal(&req->cpus, mask))
		goto unlock;

	cpumask_copy(&req->cpus, mask);

	if (type == ECS_MAX)
		pr_info("[%s] : ECS_MAX requested %*pbl\n", name, cpumask_pr_args(mask));

	ecs_request_combine_and_apply(migrate);

unlock:
	raw_spin_unlock_irqrestore(&ecs_req_lock, flags);

	return 0;
}

int ecs_request(char *name, const struct cpumask *mask, enum ecs_type type)
{
	return __ecs_request(name, mask, type, true);
}
EXPORT_SYMBOL_GPL(ecs_request);

int ecs_request_wo_migrate(char *name, const struct cpumask *mask, enum ecs_type type)
{
	return __ecs_request(name, mask, type, false);
}
EXPORT_SYMBOL_GPL(ecs_request_wo_migrate);

struct kobject *ecs_get_governor_object(void)
{
	return ecs.governor_kobj;
}
/******************************************************************************
 *                               CPU HOTPLUG                                  *
 ******************************************************************************/
static int ecs_cpuhp_online(unsigned int cpu)
{
	update_ecs_cpus(true);
	return 0;
}

static int ecs_cpuhp_offline(unsigned int cpu)
{
	update_ecs_cpus(true);
	return 0;
}

/******************************************************************************
 *                                    sysfs                                   *
 ******************************************************************************/

#define STR_LEN (6)
static int __store_ecs_cpus(const char *buf, struct cpumask *mask, int *type)
{
	int ret;
	char str[STR_LEN];

	if (*type < 0) {
		if (!sscanf(buf, "%1d" "%5s", type, str))
			return -EINVAL;
	} else {
		if (!sscanf(buf, "%5s", str))
			return -EINVAL;
	}

	if (*type != ECS_MIN && *type != ECS_MAX && *type != ECS_RECOMMEND)
		return -EINVAL;

	if (str[0] == '0' && str[1] =='x')
		ret = cpumask_parse(str + 2, mask);
	else
		ret = cpumask_parse(str, mask);

	if (ret){
		pr_err("input of req_cpus(%s) is not correct\n", buf);
		return -EINVAL;
	}

	return ret;
}

static ssize_t store_ecs_cpus(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct cpumask mask;
	int type = -1;

	if (__store_ecs_cpus(buf, &mask, &type))
		return -EINVAL;

	ecs_request("SYSFS", &mask, type);

	return count;
}

static ssize_t show_ecs_cpus(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct ecs_request *req;
	int ret = 0;

	/* All combined CPUs */
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"cur_cpus : %#x\n",
				*(unsigned int *)cpumask_bits(&ecs.cpus));

	/* Governor CPUs */
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"* governor target cpus : %#x\n",
				*(unsigned int *)cpumask_bits(get_governor_cpus()));

	/* Requested from kernel CPUs */
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"* requsted max cpus : %#x\n",
				*(unsigned int *)cpumask_bits(&ecs.requested_max_cpus));
	list_for_each_entry(req, &ecs.requests_max, list)
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"     %s: %#x\n", req->name, *(unsigned int *)cpumask_bits(&req->cpus));

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"* requsted recommend cpus : %#x\n",
				*(unsigned int *)cpumask_bits(&ecs.requested_recommend_cpus));
	list_for_each_entry(req, &ecs.requests_recommend, list)
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"     %s: %#x\n", req->name, *(unsigned int *)cpumask_bits(&req->cpus));

	ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"* requsted min cpus : %#x\n",
				*(unsigned int *)cpumask_bits(&ecs.requested_min_cpus));
	list_for_each_entry(req, &ecs.requests_min, list)
	ret += snprintf(buf + ret, PAGE_SIZE - ret,
			"     %s: %#x\n", req->name, *(unsigned int *)cpumask_bits(&req->cpus));

	return ret;
}
static struct kobj_attribute ecs_cpus_attr =
__ATTR(cpus, 0644, show_ecs_cpus, store_ecs_cpus);

static int store_ecs_user_cpus(struct cpumask *dst_mask,
			       int type, const char *buf, char *name)
{
	struct cpumask user_mask;
	int ret = 0;

	if (__store_ecs_cpus(buf, &user_mask, &type))
		return -EINVAL;

	cpumask_copy(dst_mask, &user_mask);
	ecs_request(name, &user_mask, type);

	return ret;
}

static ssize_t store_ecs_user_cpu_max(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (store_ecs_user_cpus(&ecs.user_cpu_max, ECS_MAX, buf, "USER_MAX"))
		return -EINVAL;

	return count;
}

static ssize_t show_ecs_user_cpu_max(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "user_cpu_max: %*pbl\n", cpumask_pr_args(&ecs.user_cpu_max));
}

static struct kobj_attribute ecs_user_cpu_max_attr =
__ATTR(user_cpu_max, 0644, show_ecs_user_cpu_max, store_ecs_user_cpu_max);

static ssize_t store_ecs_user_cpu_min(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (store_ecs_user_cpus(&ecs.user_cpu_min, ECS_MIN, buf, "USER_MIN"))
		return -EINVAL;

	return count;
}

static ssize_t show_ecs_user_cpu_min(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "user_cpu_min: %*pbl\n", cpumask_pr_args(&ecs.user_cpu_min));
}

static struct kobj_attribute ecs_user_cpu_min_attr =
__ATTR(user_cpu_min, 0644, show_ecs_user_cpu_min, store_ecs_user_cpu_min);

static ssize_t store_ecs_scaling_governor(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	char str[ECS_USER_NAME_LEN];
	struct ecs_governor *gov = NULL;

	if (strlen(buf) >= ECS_USER_NAME_LEN)
		return -EINVAL;

	if (!sscanf(buf, "%s", str))
		return -EINVAL;

	list_for_each_entry(gov, &ecs.gov_list, list) {
		if (!strncasecmp(str, gov->name, ECS_USER_NAME_LEN)) {
			ecs_governor_set(gov);
			return count;
		}
	}

	pr_err("input of govenor name(%s) is not correct\n", buf);
	return -EINVAL;
}

static ssize_t show_ecs_scaling_governor(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	if (!ecs.gov)
		return snprintf(buf, PAGE_SIZE, "there is no working governor\n");

	return scnprintf(buf, ECS_USER_NAME_LEN, "%s\n", ecs.gov->name);
}

static struct kobj_attribute ecs_scaling_governor_attr =
__ATTR(scaling_governor, 0644, show_ecs_scaling_governor, store_ecs_scaling_governor);

static int ecs_sysfs_init(struct kobject *ems_kobj)
{
	int ret;

	ecs.ecs_kobj = kobject_create_and_add("ecs", ems_kobj);
	if (!ecs.ecs_kobj) {
		pr_info("%s: fail to create node\n", __func__);
		return -EINVAL;
	}

	ecs.governor_kobj = kobject_create_and_add("governors", ecs.ecs_kobj);
	if (!ecs.governor_kobj) {
		pr_info("%s: fail to create node for governors\n", __func__);
		return -EINVAL;
	}

	ret = sysfs_create_file(ecs.ecs_kobj, &ecs_cpus_attr.attr);
	if (ret)
		pr_warn("%s: failed to create ecs_cpus sysfs\n", __func__);

	ret = sysfs_create_file(ecs.governor_kobj, &ecs_scaling_governor_attr.attr);
	if (ret)
		pr_warn("%s: failed to create ecs_scaling_governor sysfs\n", __func__);

	ret = sysfs_create_file(ecs.ecs_kobj, &ecs_user_cpu_max_attr.attr);
	if (ret)
		pr_warn("%s: failed to create user cpu max sysfs\n", __func__);

	ret = sysfs_create_file(ecs.ecs_kobj, &ecs_user_cpu_min_attr.attr);
	if (ret)
		pr_warn("%s: failed to create user cpu min sysfs\n", __func__);

	return 0;
}

int ecs_init(struct kobject *ems_kobj)
{
	ecs_migration_work = alloc_percpu(struct cpu_stop_work);
	if (!ecs_migration_work) {
		pr_err("falied to allocate ecs_migration_work\n");
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&ecs.requests_max);
	INIT_LIST_HEAD(&ecs.requests_recommend);
	INIT_LIST_HEAD(&ecs.requests_min);
	INIT_LIST_HEAD(&ecs.gov_list);

	ecs.gov = NULL;
	cpumask_copy(&ecs.cpus, cpu_possible_mask);
	cpumask_copy(&ecs.user_cpu_max, cpu_possible_mask);
	cpumask_copy(&ecs.requested_max_cpus, cpu_possible_mask);
	cpumask_copy(&ecs.requested_recommend_cpus, cpu_possible_mask);
	cpumask_clear(&ecs.requested_min_cpus);

	ecs_request_register("SYSFS", cpu_possible_mask, ECS_MAX);
	ecs_request_register("SYSFS", cpu_possible_mask, ECS_RECOMMEND);
	ecs_request_register("SYSFS", NULL, ECS_MIN);
	ecs_request_register("USER_MAX", cpu_possible_mask, ECS_MAX);
	ecs_request_register("USER_MIN", NULL, ECS_MIN);

	cpuhp_setup_state(CPUHP_AP_ARM_TWD_STARTING,
			"AP_EXYNOS_CPU_POWER_UP_CONTROL",
			ecs_cpuhp_online, NULL);
	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
			"AP_EXYNOS_CPU_POWER_DOWN_CONTROL",
			NULL, ecs_cpuhp_offline);

	ecs_sysfs_init(ems_kobj);

	return 0;
}
