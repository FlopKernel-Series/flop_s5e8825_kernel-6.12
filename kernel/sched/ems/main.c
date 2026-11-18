/*
 * Core Exynos Mobile Scheduler
 *
 * Copyright (C) 2022 Samsung Electronics Co., Ltd
 */

#include <linux/pm_qos.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/panic_notifier.h>
#include <linux/version.h>

#include <soc/samsung/ems.h>

#define CREATE_TRACE_POINTS
#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

#include <dt-bindings/soc/samsung/ems.h>

static bool init_done = false;

bool ems_disabled(void)
{
	return !init_done;
}
/******************************************************************************
 * Functions for handling task attribute                                      *
 ******************************************************************************/
static int high_prio_threshold[CGROUP_COUNT];

static bool launch_boost_condition(struct task_struct *p)
{
	if (emstune_get_cur_level() == EMS_LEVEL_NORMAL)
		return false;

	if (get_sched_class(p) != EMS_SCHED_FAIR)
		return false;

	if (cpuctl_task_group_idx(p) != CGROUP_TOPAPP)
		return false;

	return true;
}

bool is_render_task(struct task_struct *p)
{
	if (!launch_boost_condition(p))
		return false;

	if (!emstune_task_boost())
		return false;

	return ems_render_task(p);
}

bool is_zygote_task(struct task_struct *p)
{
	if (!launch_boost_condition(p))
		return false;

	if (emstune_task_boost())
		return false;

	if (strcmp(p->comm, "main"))
		return false;

	return true;
}

bool is_launch_task(struct task_struct *p)
{
	if (ems_boosted_by_user(p))
		return true;

	/* boost high prio RenderThread */
	if (is_render_task(p) && p->prio <= TA_BOOST_PRIO)
		return true;

	/* boost zygote */
	if (is_zygote_task(p))
		return true;

	return false;
}

bool is_low_latency_task(struct task_struct *p)
{
	return ems_binder_task(p) || ems_low_latency_task(p);
}

bool is_high_prio_task(struct task_struct *p)
{
	int group_idx;

	if (get_sched_class(p) != EMS_SCHED_FAIR)
		return false;

	group_idx = cpuctl_task_group_idx(p);

	return p->prio <= high_prio_threshold[group_idx];
}

bool is_important_task(struct task_struct *p)
{
	if (is_launch_task(p))
		return true;

	if (is_reserved_task(p))
		return true;

	if (is_low_latency_task(p))
		return true;

	if (is_render_task(p))
		return true;

	if (is_high_prio_task(p))
		return true;

	return false;
}

bool is_io_boost_task(struct task_struct *p)
{
	return emstune_sched_io_boost() && is_top_app_task(p);
}

void check_enqueue_task_attr(struct rq *rq, struct task_struct *p)
{
	if (get_sched_class(p) != EMS_SCHED_FAIR)
		return;

	if (is_launch_task(p)) {
		ems_launch_task(p) = 1;
		ems_rq_nr_launch(rq) += 1;
	}

	if (get_reserved_cpu(p) >= 0) {
		ems_reserved_task(p) = 1;
		ems_rq_nr_reserved_task(rq) += 1;
	}

	if (is_high_prio_task(p)) {
		ems_high_prio_task(p) = 1;
		ems_rq_nr_high_prio(rq) += 1;
	}
}

void check_dequeue_task_attr(struct rq *rq, struct task_struct *p)
{
	if (get_sched_class(p) != EMS_SCHED_FAIR)
		return;

	if (ems_launch_task(p)) {
		ems_launch_task(p) = 0;
		ems_rq_nr_launch(rq) -= 1;
	}

	if (ems_reserved_task(p)) {
		ems_reserved_task(p) = 0;
		ems_rq_nr_reserved_task(rq) -= 1;
	}

	if (ems_high_prio_task(p)) {
		ems_high_prio_task(p) = 0;
		ems_rq_nr_high_prio(rq) -= 1;
	}
}

void init_task_attr(struct task_struct *p)
{
	ems_boosted_by_user(p) = 0;
	ems_launch_task(p) = 0;
	ems_binder_task(p) = 0;
	ems_low_latency_task(p) = 0;
	ems_render_task(p) = 0;
	ems_high_prio_task(p) = 0;
}

static int high_prio_emstune_notifier_call(struct notifier_block *nb,
				unsigned long val, void *v)
{
	struct emstune_set *cur_set = (struct emstune_set *)v;
	int i;

	for (i = 0; i < CGROUP_COUNT; i++)
		high_prio_threshold[i] = cur_set->high_prio.threshold[i];

	return NOTIFY_OK;
}

static struct notifier_block high_prio_emstune_notifier = {
	.notifier_call = high_prio_emstune_notifier_call,
};

void high_prio_init(void)
{
	emstune_register_notifier(&high_prio_emstune_notifier);
}

/******************************************************************************
 * panic handler for scheduler debugging                                      *
 ******************************************************************************/
static inline void print_task_info(struct task_struct *p)
{
	pr_info("\t\tcomm=%s pid=%d prio=%d isa=%s cpus_ptr=%*pbl\n",
		p->comm, p->pid, p->prio,
		test_ti_thread_flag(&p->thread_info, TIF_32BIT)
		? "32bit" : "64bit",
		cpumask_pr_args(p->cpus_ptr));
}

static int ems_panic_notifier_call(struct notifier_block *nb,
				   unsigned long l, void *buf)
{
	int cpu;
	struct rq *rq;

	pr_info("[SCHED-SNAPSHOT]\n");
	for_each_possible_cpu(cpu) {
		struct task_struct *curr, *p, *temp;
		struct plist_head *head;
		struct rb_node *next_node;

		if (!cpu_active(cpu)) {
			pr_info("cpu%d: offline\n", cpu);
			continue;
		}

		pr_info("<cpu%d>\n", cpu);

		rq = cpu_rq(cpu);

		pr_info("\tcurr:\n");
		curr = rq->curr;
		print_task_info(curr);

		if (list_empty(&rq->cfs_tasks))
			pr_info("\tcfs: no task\n");
		else {
			pr_info("\tcfs:\n");
			list_for_each_entry(p, &rq->cfs_tasks, se.group_node) {
				if (curr == p)
					continue;
				print_task_info(p);
			}
		}

		head = &rq->rt.pushable_tasks;
		if (plist_head_empty(head))
			pr_info("\trt: no task\n");
		else {
			pr_info("\t rt:\n");
			plist_for_each_entry_safe(p, temp, head, pushable_tasks)
				print_task_info(p);
		}

		if (RB_EMPTY_ROOT(&rq->dl.pushable_dl_tasks_root.rb_root))
			pr_info("\tdl: no task\n");
		else {
			pr_info("\t dl:\n");
			next_node = rq->dl.pushable_dl_tasks_root.rb_leftmost;
			while (next_node) {
				p = rb_entry(next_node, struct task_struct,
							pushable_dl_tasks);
				print_task_info(p);
				next_node = rq->dl.pushable_dl_tasks_root.rb_leftmost;
			}
		}

		pr_info("\n");
	}

	return 0;
}

static struct notifier_block ems_panic_nb = {
	.notifier_call = ems_panic_notifier_call,
	.priority = INT_MIN,
};

struct pe_list *pe_list;
static int num_of_pe_list;

struct pe_list *get_pe_list(int index)
{
	if (unlikely(!pe_list))
		return NULL;

	if (index >= num_of_pe_list)
		return NULL;

	return &pe_list[index];
}

int get_pe_list_size(void)
{
	return num_of_pe_list;
}

int get_pe_cluster_size(void)
{
	struct pe_list *pe_list = get_pe_list(0);

	return pe_list->num_of_clusters;
}

struct cpumask *get_pe_cluster_mask(unsigned int cpu)
{
	struct pe_list *pe_list = get_pe_list(0);
	int cluster;

	if (cpu >= VENDOR_NR_CPUS)
		return NULL;

	if (!pe_list)
		return NULL;

	for (cluster = 0; cluster < pe_list->num_of_clusters; cluster++) {
		if (cpumask_test_cpu(cpu, &pe_list->cpus[cluster]))
			return &pe_list->cpus[cluster];
	}

	return NULL;
}

/******************************************************************************
 * IOCTL interface                                                            *
 ******************************************************************************/
struct ems_bridge {
	int					req_count;
	wait_queue_head_t			ioctl_wq;

	struct file_operations			fops;
	struct miscdevice			miscdev;
};
static struct ems_bridge *bridge;

static ssize_t
ems_fops_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	return count;
}

static __poll_t
ems_fops_poll(struct file *filp, struct poll_table_struct *wait)
{
	__poll_t mask = 0;

	poll_wait(filp, &bridge->ioctl_wq, wait);

	/* To manage kernel side request */
	if (bridge->req_count > 0)
		mask = EPOLLIN | EPOLLRDNORM;

	return mask;
}

static long
ems_fops_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static int ems_fops_release(struct inode *node, struct file *filp)
{
	return 0;
}

static int ems_register_misc_device(void)
{
	int ret;
	struct file_operations *fops = &bridge->fops;
	struct miscdevice *miscdev = &bridge->miscdev;

	fops->owner		= THIS_MODULE;
	fops->read		= ems_fops_read;
	fops->poll		= ems_fops_poll;
	fops->unlocked_ioctl	= ems_fops_ioctl;
	fops->compat_ioctl	= ems_fops_ioctl;
	fops->release		= ems_fops_release;

	miscdev->minor		= MISC_DYNAMIC_MINOR;
	miscdev->name		= "ems";
	miscdev->fops		= fops;

	ret = misc_register(miscdev);
	if (ret) {
		pr_err("ems couldn't register misc device %d!", ret);
		return ret;
	}

	return 0;
}

static int ems_init_ioctl(void)
{
	bridge = kzalloc(sizeof(struct ems_bridge), GFP_KERNEL);
	if (!bridge) {
		pr_err("Failed to allocate bridge\n");
		return -ENOMEM;
	}

	bridge->req_count = 0;
	init_waitqueue_head(&bridge->ioctl_wq);

	ems_register_misc_device();

	return 0;
}

/******************************************************************************
 * common function for ems                                                    *
 ******************************************************************************/
int get_sched_class(struct task_struct *p)
{
	if (is_idle_task(p))
		return EMS_SCHED_IDLE;
	else if (dl_prio(p->prio))
		return EMS_SCHED_DL;
	else if (rt_prio(p->prio))
		return EMS_SCHED_RT;
	else
		return EMS_SCHED_FAIR;
}

int cpuctl_task_group_idx(struct task_struct *p)
{
#ifndef CONFIG_SCHED_EMS_TASK_GROUP
	int idx;
	struct cgroup_subsys_state *css;

	rcu_read_lock();
	css = task_css(p, cpu_cgrp_id);
	idx = css->id - 1;
	rcu_read_unlock();

	/* if customer add new group, use the last group */
	if (idx >= CGROUP_COUNT)
		idx = CGROUP_COUNT - 1;

	return max(idx, 0);
#else
	int idx;
	struct ems_task_struct *ep = (struct ems_task_struct *)android_task_vendor_data(p);

	if (!ep) {
		pr_warn("Failed to get ems_task_struct\n");
		return 0;
	}

	idx = ep->cgroup;

	if (idx >= CGROUP_COUNT)
		idx = CGROUP_COUNT - 1;

	return max(idx, 0);
#endif
}
EXPORT_SYMBOL(cpuctl_task_group_idx);

const struct cpumask *cpu_clustergroup_mask(int cpu)
{
	struct ems_cpu_topology *topo = percpu_topology(cpu);

	return &topo->cluster_cpus;
}

int cpu_clustergroup_id(int cpu)
{
	struct ems_cpu_topology *topo = percpu_topology(cpu);

	return topo->cluster_id;
}

static DEFINE_PER_CPU(struct ems_sched_domain, sched_domains);

const struct cpumask *cpu_sched_domain_mask(int cpu)
{
	struct ems_sched_domain *esd = &per_cpu(sched_domains, cpu);

	return &esd->domain_cpus;
}

int cpu_sched_domain_id(int cpu)
{
	struct ems_sched_domain *esd = &per_cpu(sched_domains, cpu);

	return esd->domain_id;
}

struct cpumask slowest_mask;
struct cpumask fastest_mask;

const struct cpumask *cpu_slowest_mask(void)
{
	return &slowest_mask;
}

const struct cpumask *cpu_fastest_mask(void)
{
	return &fastest_mask;
}

void cpumask_speed_init(void)
{
	int cpu;
	unsigned long min_cap = ULONG_MAX, max_cap = 0;
	struct cpumask temp_slowest_mask;
	struct cpumask temp_fastest_mask;

	cpumask_clear(&temp_slowest_mask);
	cpumask_clear(&temp_fastest_mask);

	for_each_cpu(cpu, cpu_possible_mask) {
		unsigned long cap;

		cap = et_max_cap(cpu);
		if (cap < min_cap)
			min_cap = cap;
		if (cap > max_cap)
			max_cap = cap;
	}

	for_each_cpu(cpu, cpu_possible_mask) {
		unsigned long cap;

		cap = et_max_cap(cpu);
		if (cap == min_cap)
			cpumask_set_cpu(cpu, &temp_slowest_mask);

		if (cap == max_cap)
			cpumask_set_cpu(cpu, &temp_fastest_mask);
	}

	cpumask_copy(&slowest_mask, &temp_slowest_mask);
	cpumask_copy(&fastest_mask, &temp_fastest_mask);
}

static void convert_to_logical_mask(struct cpumask *cpu_mask)
{
	struct cpumask logical_mask;
	int cpu;

	cpumask_clear(&logical_mask);

	for_each_cpu(cpu, cpu_mask) {
		int log_cpu = get_cpu_log_id(cpu);
		if (log_cpu >= 0)
			cpumask_set_cpu(log_cpu, &logical_mask);
	}

	cpumask_copy(cpu_mask, &logical_mask);
}

static void adjust_pe_list(struct pe_list *raw_list, int num_of_raw_list)
{
	int raw_i, raw_j, pl_i, pl_j;
	int num_of_clusters;

	/*
	 * 1) Convert the given cpu mask to logical mask
	 */
	for (raw_i = 0; raw_i < num_of_raw_list; raw_i++) {
		struct pe_list *pl = &raw_list[raw_i];

		for (raw_j = 0; raw_j < pl->num_of_clusters; raw_j++)
			convert_to_logical_mask(&pl->cpus[raw_j]);

		if (!cpumask_empty(&pl->cpus[0]))
			num_of_pe_list++;
	}
	BUG_ON(!num_of_pe_list);

	/*
	 * 2) Build pe_list using logical mask
	 */
	pe_list = kmalloc_array(num_of_pe_list, sizeof(struct pe_list), GFP_KERNEL);
	BUG_ON(!pe_list);

	for (pl_i = 0, raw_i = 0; raw_i < num_of_raw_list; raw_i++) {
		struct pe_list *dst = &pe_list[pl_i];
		struct pe_list *src = &raw_list[raw_i];

		/* Do not build pe_list that starts with unavailable cpus */
		if (cpumask_empty(&src->cpus[0]))
			continue;

		num_of_clusters = 0;
		for (pl_j = 0, raw_j = 0; raw_j < src->num_of_clusters; raw_j++)
			if (!cpumask_empty(&src->cpus[raw_j]))
				++num_of_clusters;

		dst->num_of_clusters = num_of_clusters;
		dst->cpus = kmalloc_array(num_of_clusters, sizeof(struct cpumask), GFP_KERNEL);
		BUG_ON(!dst->cpus);

		for (pl_j = 0, raw_j = 0; raw_j < src->num_of_clusters; raw_j++) {
			if (cpumask_empty(&src->cpus[raw_j]))
				continue;

			cpumask_copy(&dst->cpus[pl_j++], &src->cpus[raw_j]);
		}

		pl_i++;
	}
}

static void pe_list_init(void)
{
	struct pe_list *raw_list;
	struct device_node *dn, *child;
	int index = 0, num_of_raw_list;

	dn = of_find_node_by_path("/ems/pe-list");
	if (!dn) {
		pr_err("%s: Fail to get pe-list node\n", __func__);
		return;
	}

	num_of_raw_list = of_get_child_count(dn);
	if (num_of_raw_list == 0) {
		pr_err("%s: pe-list is empty\n", __func__);
		return;
	}

	raw_list = kmalloc_array(num_of_raw_list, sizeof(struct pe_list), GFP_KERNEL);

	for_each_child_of_node(dn, child) {
		struct pe_list *pl = &raw_list[index];
		const char *buf[VENDOR_NR_CPUS];
		int i;

		pl->num_of_clusters = of_property_count_strings(child, "cpus");
		if (pl->num_of_clusters == 0)
			continue;

		pl->cpus = kmalloc_array(pl->num_of_clusters, sizeof(struct cpumask), GFP_KERNEL);

		of_property_read_string_array(child, "cpus", buf, pl->num_of_clusters);
		for (i = 0; i < pl->num_of_clusters; i++)
			ems_cpulist_parse(buf[i], &pl->cpus[i]);

		index++;
	}

	adjust_pe_list(raw_list, num_of_raw_list);

	for (index = 0; index < num_of_raw_list; index++)
		kfree(raw_list[index].cpus);
	kfree(raw_list);
}

static void sched_domain_init(void)
{
	int cluster, cpu;
	struct pe_list *pl = &pe_list[0];

	for (cluster = 0; cluster < pl->num_of_clusters; cluster++) {
		struct cpumask *cluster_mask = &pl->cpus[cluster];

		for_each_cpu(cpu, cluster_mask) {
			struct ems_sched_domain *esd = &per_cpu(sched_domains, cpu);

			cpumask_copy(&esd->domain_cpus, cluster_mask);
			esd->domain_id = cluster;
		}
	}
}

static struct kobject *ems_kobj;
int send_uevent(char *msg)
{
	char *envp[] = { msg, NULL };
	int ret;

	ret = kobject_uevent_env(ems_kobj, KOBJ_CHANGE, envp);
	if (ret)
		pr_warn("%s: Failed to send uevent\n", __func__);

	return ret;
}

void detach_task(struct rq *src_rq, struct rq *dst_rq, struct task_struct *p)
{
	deactivate_task(src_rq, p, DEQUEUE_NOCLOCK);
	set_task_cpu(p, dst_rq->cpu);
}

int detach_one_task(struct rq *src_rq, struct rq *dst_rq,
					struct task_struct *target)
{
	struct task_struct *p, *n;

	list_for_each_entry_safe(p, n, &src_rq->cfs_tasks, se.group_node) {
		if (p != target)
			continue;

		if (!can_migrate(p, dst_rq->cpu))
			break;

		update_rq_clock(src_rq);
		detach_task(src_rq, dst_rq, p);
		return 1;
	}

	return 0;
}

void attach_task(struct rq *dst_rq, struct task_struct *p)
{
	activate_task(dst_rq, p, ENQUEUE_NOCLOCK);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,12,0)
	wakeup_preempt(dst_rq, p, 0);
#else
	check_preempt_curr(dst_rq, p, 0);
#endif
}

void attach_one_task(struct rq *dst_rq, struct task_struct *p)
{
	struct rq_flags rf;

	rq_lock(dst_rq, &rf);
	update_rq_clock(dst_rq);
	attach_task(dst_rq, p);
	rq_unlock(dst_rq, &rf);
}

/******************************************************************************
 * cpufreq notifier                                                           *
 ******************************************************************************/
static cpumask_var_t cpus_to_visit;
static void parsing_done_workfn(struct work_struct *work);
static DECLARE_WORK(parsing_done_work, parsing_done_workfn);

static int ems_cpufreq_policy_notify_callback(struct notifier_block *nb,
					unsigned long val, void *data)
{
	struct cpufreq_policy *policy = data;

	if (val != CPUFREQ_CREATE_POLICY)
		return 0;

	cpumask_andnot(cpus_to_visit, cpus_to_visit, policy->related_cpus);

	et_init_table(policy);
	fv_init_table(policy);

	if (cpumask_empty(cpus_to_visit)) {
		ems_game_late_init(ems_kobj);

		pr_debug("ems: energy: init energy table done\n");
		schedule_work(&parsing_done_work);
	}

	cpumask_speed_init();
	set_capacity_fixup_mask();
	set_maxfreq_fixup_value(policy);
	return 0;
}

static struct notifier_block ems_cpufreq_policy_notifier = {
	.notifier_call = ems_cpufreq_policy_notify_callback,
	.priority = INT_MIN,
};

static void parsing_done_workfn(struct work_struct *work)
{
	cpufreq_unregister_notifier(&ems_cpufreq_policy_notifier,
					 CPUFREQ_POLICY_NOTIFIER);
	free_cpumask_var(cpus_to_visit);

	ems_rq_cpu_capacity_init();
}

static void ems_init_cpufreq_notifier(void)
{
	if (!alloc_cpumask_var(&cpus_to_visit, GFP_KERNEL))
		return;

	cpumask_copy(cpus_to_visit, cpu_possible_mask);

	if (cpufreq_register_notifier(&ems_cpufreq_policy_notifier,
					CPUFREQ_POLICY_NOTIFIER))
		free_cpumask_var(cpus_to_visit);
}


/******************************************************************************
*				SYSFS						*
 ******************************************************************************/
static ssize_t show_sched_topology(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int cpu;
	struct sched_domain *sd;
	int ret = 0;

	rcu_read_lock();
	for_each_possible_cpu(cpu) {
		int sched_domain_level = 0;

		sd = rcu_dereference_check_sched_domain(cpu_rq(cpu)->sd);
		if (!sd)
			continue;
		while (sd->parent) {
			sched_domain_level++;
			sd = sd->parent;
		}

		for (; sd; sd = sd->child) {
			ret += snprintf(buf + ret, 70,
					"[lv%d] cpu%d: flags=%#x sd->span=%#x sg->span=%#x\n",
					sched_domain_level, cpu, sd->flags,
					*(unsigned int *)cpumask_bits(sched_domain_span(sd)),
					*(unsigned int *)cpumask_bits(sched_group_span(sd->groups)));
			sched_domain_level--;
		}
		ret += snprintf(buf + ret,
				50, "----------------------------------------\n");
	}
	ret += snprintf(buf + ret, 70, "slowest_cpus=%#x\n",
			*(unsigned int *)cpumask_bits(cpu_slowest_mask()));
	ret += snprintf(buf + ret, 70, "fastest_cpus=%#x\n",
			*(unsigned int *)cpumask_bits(cpu_fastest_mask()));

	for_each_possible_cpu(cpu) {
		ret += snprintf(buf + ret, 70, "cpu%d cap_origin=%5lu cap=%5lu freq_cap=%5lu\n",
					cpu, ems_get_origin_cpu_capacity(cpu),
					ems_get_actual_cpu_capacity(cpu),
					arch_scale_freq_capacity(cpu));
	}
	rcu_read_unlock();

	return ret;
}

static DEVICE_ATTR(sched_topology, S_IRUGO, show_sched_topology, NULL);

static ssize_t show_pe_list(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int index, i;
	int ret = 0;

	for (index = 0; index < num_of_pe_list; index++) {
		struct pe_list *pl = &pe_list[index];

		ret += snprintf(buf + ret, PAGE_SIZE - ret, "pe_list[%d]: ", index);
		for (i = 0; i < pl->num_of_clusters; i++)
			ret += snprintf(buf + ret, PAGE_SIZE - ret, "<%*pbl> ",
					cpumask_pr_args(&pl->cpus[i]));
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");
	}

	ret += snprintf(buf + ret, PAGE_SIZE - ret, "pe_list_size : %d\n", num_of_pe_list);
	ret += snprintf(buf + ret, PAGE_SIZE - ret, "pe_cluster_size : %d\n", get_pe_cluster_size());

	return ret;
}

static DEVICE_ATTR(pe_list, S_IRUGO, show_pe_list, NULL);

DEFINE_PER_CPU(struct _rq_avd1, ems_runqueues);
DEFINE_PER_CPU(struct ems_cpu_topology, topo_info);

static void ems_cpu_topology_init(void)
{
	struct device_node *np, *child;
	struct cpumask cpus;
	const char *buf;
	int cpu, nr_cluster = 0, ret = 0;

	np = of_find_node_by_path("/power-data/cpu");
	if (!np)
		return;

	for_each_child_of_node(np, child) {
		ret = of_property_read_string(child, "cpus", &buf);
		if (ret)
			return;
		cpulist_parse(buf, &cpus);

		for_each_cpu(cpu, &cpus) {
			struct ems_cpu_topology *topo = percpu_topology(cpu);

			cpumask_copy(&topo->cluster_cpus, &cpus);
			topo->cluster_id = nr_cluster;
		}

		nr_cluster++;
	}
}

void ems_task_init(struct task_struct *p)
{
	struct ems_task_struct *ets;

	ets = (struct ems_task_struct *)android_task_vendor_data(p);

	memset(ets, 0, sizeof(struct ems_task_struct));
}

static int ems_suspend(struct device *dev)
{
	sysbusy_suspend();
	pago_suspend();
	pr_info("EMS suspend\n");

	return 0;
}

static int ems_resume(struct device *dev)
{
	pago_resume();

	return 0;
}

static void ems_idle_task_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct task_struct *idle_task = cpu_rq(cpu)->idle;
		ems_game_task_init(idle_task);
		egg_assign_reserved_cpu(idle_task);
	}
}

static int ems_init_stop_handler(void *data)
{
	int cpu;
	struct task_struct *g, *p;

	read_lock(&tasklist_lock);
	for_each_possible_cpu(cpu)
		raw_spin_lock(&cpu_rq(cpu)->__lock);

	for_each_process_thread(g, p) {
		get_task_struct(p);
		ems_game_task_init(p);
		init_task_attr(p);
		mlt_init_task(p);
		dslt_init_task(p);
		gsc_init_new_task(p);
		tex_task_init(p);
		egg_assign_reserved_cpu(p);
		put_task_struct(p);
	}

	ems_hook_enable(true);

	for_each_possible_cpu(cpu) {
		raw_spin_unlock(&cpu_rq(cpu)->__lock);
	}
	read_unlock(&tasklist_lock);

	return 0;
}

static void ems_init_finisher(void)
{
	stop_machine(ems_init_stop_handler, NULL, NULL);
}

static struct platform_device *ems_pdev;
static void ems_probe(struct work_struct *work)
{
	struct platform_device *pdev = ems_pdev;
	int ret;

	ems_cpu_topology_init();

	ret = hook_init();
	if (ret) {
		WARN_ON("EMS failed to register vendor hook\n");
		return;
	}
	ems_kobj = &pdev->dev.kobj;

	ems_idle_task_init();

	cpumask_copy(&slowest_mask, cpu_possible_mask);
	cpumask_copy(&fastest_mask, cpu_possible_mask);

	/* compile time data size check */
	//EMS_VENDOR_DATA_SIZE_CHECK(struct ems_task_struct, struct task_struct);

	pe_list_init();
	sched_domain_init();
	mlt_init(ems_kobj, pdev->dev.of_node);
	dslt_init(ems_kobj, pdev->dev.of_node);
	profile_sched_init(ems_kobj);

	ems_idle_select_init(pdev);
	ems_freq_select_init(pdev);
	ems_core_select_init(pdev);
	ems_game_sched_init(pdev);

#ifdef CONFIG_SCHED_EMS_DSU_SLICE_DOWN
	dsu_slice_down_init();
#endif
	ems_init_ioctl();

	if (sysfs_create_file(ems_kobj, &dev_attr_sched_topology.attr))
		pr_warn("failed to create sched_topology\n");

	if (sysfs_create_file(ems_kobj, &dev_attr_pe_list.attr))
		pr_warn("failed to create pe_list\n");

	if (sysfs_create_link(kernel_kobj, ems_kobj, "ems"))
		pr_warn("failed to link ems\n");

#ifdef CONFIG_SCHED_EMS_TASK_GROUP
	if (task_group_init(ems_kobj))
		pr_warn("failed to taskgroup_init\n");
#endif

	atomic_notifier_chain_register(&panic_notifier_list, &ems_panic_nb);

	ems_init_cpufreq_notifier();

	/* it must be called at last step of init sequence*/
	ems_init_finisher();

	init_done = true;
	pr_info("EMS initialized\n");
}

static DECLARE_WORK(ems_init_work, ems_probe);
static int ems_module_init(struct platform_device *pdev)
{
	ems_pdev = pdev;

	ecs_init(&pdev->dev.kobj);
	exynos_perf_events_init(&pdev->dev.kobj);

	schedule_work(&ems_init_work);

	return 0;
}

static const struct of_device_id of_ems_match[] = {
	{ .compatible = "samsung,ems", },
	{ },
};
MODULE_DEVICE_TABLE(of, of_ems_match);

static const struct dev_pm_ops ems_pm_ops = {
	.suspend = ems_suspend,
	.resume = ems_resume,
};

static struct platform_driver ems_driver = {
	.driver = {
		.name = "ems",
		.owner = THIS_MODULE,
		.pm = &ems_pm_ops,
		.of_match_table = of_ems_match,
	},
	.probe          = ems_module_init,
};

module_platform_driver(ems_driver);

MODULE_SOFTDEP("pre: exynos-cpu-phyid");
MODULE_DESCRIPTION("EMS");
MODULE_LICENSE("GPL");
