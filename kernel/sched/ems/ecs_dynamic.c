/*
 * Exynos Core Sparing Governor - Exynos Mobile Scheduler
 *
 * Copyright (C) 2022 Samsung Electronics Co., Ltd
 * Youngtae Lee <yt0729.lee@samsung.com>
 */

#include <soc/samsung/ems.h>

#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

#define DEFAULT_BUSY_RATIO	(50)

static struct {
	struct list_head		domain_list;
	struct cpumask			governor_cpus;
	struct cpumask			min_cpus;
	int				nr_domains;
	int				governor_disabled;
	unsigned int			default_gov;
	unsigned long			last_update_jiffies;
	unsigned long			last_execute_jiffies;

	struct kobject *kobj;

} dynamic;

static DEFINE_RAW_SPINLOCK(dynamic_lock);

static void update_governor_cpus(const struct cpumask *new_cpus, char *event)
{
	struct dynamic_dom *domain;
	struct cpumask spared_cpus;
	struct cpumask target_cpus;

	cpumask_clear(&target_cpus);
	cpumask_or(&target_cpus, new_cpus, &dynamic.min_cpus);

	trace_ecs_update_governor_cpus(event,
			*(unsigned int *)cpumask_bits(&dynamic.governor_cpus),
			*(unsigned int *)cpumask_bits(&target_cpus),
			*(unsigned int *)cpumask_bits(new_cpus),
			*(unsigned int *)cpumask_bits(&dynamic.min_cpus));

	/* update governor cpus */
	cpumask_copy(&dynamic.governor_cpus, &target_cpus);

	/* update wether domain has spared_cpus or not and domain->governor_cpus */
	cpumask_andnot(&spared_cpus, cpu_active_mask, &dynamic.governor_cpus);
	list_for_each_entry(domain, &dynamic.domain_list, node) {
		cpumask_and(&domain->governor_cpus, &domain->cpus, &target_cpus);
		domain->has_spared_cpu = cpumask_intersects(&domain->cpus, &spared_cpus);
	}

	update_ecs_cpus(true);
}

static struct dynamic_dom *dynamic_get_domain(int id)
{
	struct dynamic_dom *domain;

	list_for_each_entry(domain, &dynamic.domain_list, node) {
		if (domain->id == id)
			return domain;
	}
	return NULL;
}

/* Should be called with ECS LOCK */
void dynamic_control_governor(int user, int enable)
{
	unsigned long flags;
	bool prev_mode, new_mode;

	raw_spin_lock_irqsave(&dynamic_lock, flags);

	prev_mode = dynamic.governor_disabled ? false : true;

	if (enable)
		dynamic.governor_disabled = dynamic.governor_disabled & (~user);
	else
		dynamic.governor_disabled = dynamic.governor_disabled | user;

	new_mode = dynamic.governor_disabled ? false : true;

	if (prev_mode == new_mode) {
		raw_spin_unlock_irqrestore(&dynamic_lock, flags);
		return;
	}

	update_governor_cpus(cpu_possible_mask, "governor-ctrl");

	raw_spin_unlock_irqrestore(&dynamic_lock, flags);

}

#define NR_THRESHOLD	(3)
static void update_domain_info(struct dynamic_dom *domain)
{
	int cpu, nr_busy_cpus = 0, nr_run_sum = 0, nr_misfit_sum = 0;
	unsigned long util_sum = 0, capacity_sum = 0, domain_busy_ratio = 0;
	unsigned long busy_ratio_threshold = domain->busy_ratio;

	for_each_cpu(cpu, &domain->cpus) {
		struct rq *rq = cpu_rq(cpu);
		unsigned long util = 0, capacity = 0, busy_ratio = 0;
		unsigned long min_util, max_util;
		int avg_nr = 0, avg_misfit = 0, curr_nr = 0;

		if (!cpumask_test_cpu(cpu, &domain->governor_cpus))
			goto next;

		curr_nr = rq->nr_running;
		capacity = ems_get_origin_cpu_capacity(cpu);
		util = ego_cpu_util(cpu, ml_cpu_util(cpu), capacity, &min_util, &max_util);
		util = clamp_val(util, min_util, max_util);

		/* 1. compute average nr_run */
		avg_nr = mlt_runnable(cpu);
		avg_nr = (avg_nr + domain->nr_run_up_ratio) / MLT_RUNNABLE_UNIT;

		/* 2. compute average nr_misfit */
		avg_misfit = mlt_misfit_runnable(cpu);
		avg_misfit = (avg_misfit + domain->nr_run_up_ratio) / MLT_RUNNABLE_UNIT;

		/* 3. compute busy cpu */
		capacity = ems_get_actual_cpu_capacity(cpu);
		busy_ratio = (util * 100) / capacity;

		/* 4. check busy */
		if (busy_ratio >= busy_ratio_threshold)
			nr_busy_cpus++;
		else if (avg_nr >= NR_THRESHOLD)
			nr_busy_cpus++;

		util_sum += util;
		capacity_sum += capacity;
		nr_run_sum += avg_nr;
		nr_misfit_sum += avg_misfit;
next:
		trace_ecs_dynamic_cpu_info(cpu, util, capacity, busy_ratio,
					curr_nr, avg_nr, avg_misfit);
	}

	/* consider this domain not busy when it has any spare cpu */
	if (domain->has_spared_cpu || !capacity_sum)
		domain_busy_ratio = 0;
	else
		domain_busy_ratio = (util_sum * 100) / capacity_sum;

	domain->busy = domain_busy_ratio >= busy_ratio_threshold;
	domain->nr_run = nr_run_sum;
	domain->nr_misfit = nr_misfit_sum;
	domain->nr_busy_cpus = nr_busy_cpus;
}

static int domain_overpacked(struct dynamic_dom *domain)
{
	return domain->nr_run >= domain->nr_cpus;
}

static int compute_helping_cpus(struct dynamic_dom *domain)
{
	int help_nr = 0, help_misfit = 0, nr_busy_domains = 0, help_misfit_thr;
	bool has_spared_cpu = false;
	int i;

	for (i = 0; i < domain->num_of_nr_help_dom; i++) {
		struct dynamic_dom *helped_dom = domain->nr_help_dom[i];

		if (helped_dom->has_spared_cpu) {
			has_spared_cpu = true;
			continue;
		}

		/* help this domain when it is overpacked */
		if (domain_overpacked(helped_dom))
			help_nr += helped_dom->nr_run - helped_dom->nr_cpus;

		if (helped_dom->busy)
			nr_busy_domains++;
	}

	/* help when all helped_domain are busy */
	if (nr_busy_domains > 0 && nr_busy_domains == domain->num_of_nr_help_dom)
		help_nr++;

	/* even if helped_domain is overpacked, use spared_cpu first if it is available */
	if (has_spared_cpu)
		help_nr = 0;

	for (i = 0; i < domain->num_of_misfit_help_dom; i++) {
		struct dynamic_dom *helped_dom = domain->misfit_help_dom[i];

		/* help this domain when it has misfit tasks */
		help_misfit += helped_dom->nr_misfit;
	}

	help_misfit_thr = max(domain->help_misfit_thr_by_user, domain->help_misfit_thr_by_emstune);
	if (help_misfit <= help_misfit_thr)
		help_misfit = 0;

	return help_nr + help_misfit;
}

static int compute_required_cpus(struct dynamic_dom *domain)
{
	int required_cpus = 0;

	/* this domain is overpacked, all cpus are needed */
	if (domain_overpacked(domain))
		return domain->nr_cpus;

	required_cpus = domain->nr_busy_cpus + domain->nr_help;

	/* wake up one more cpu if there are more tasks than required_cpu in this domain */
	if (required_cpus < domain->nr_run)
		required_cpus++;

	return required_cpus;
}

static void compute_governor_cpus(struct dynamic_dom *domain, struct cpumask *new_cpus)
{
	int cpu, cnt;

	domain->nr_help = compute_helping_cpus(domain);
	domain->required_cpus = compute_required_cpus(domain);

	cnt = domain->required_cpus;
	for_each_cpu(cpu, &domain->cpus) {
		if (--cnt < 0)
			break;
		cpumask_set_cpu(cpu, new_cpus);
	}

	trace_ecs_dynamic_domain_info(cpumask_first(&domain->cpus), domain);
}

static void update_dynamic_governor_cpus(void)
{
	struct dynamic_dom *domain;
	struct cpumask new_cpus;

	cpumask_clear(&new_cpus);

	if (egg_enabled()) {
		cpumask_copy(&new_cpus, cpu_active_mask);
		goto update;
	}

	list_for_each_entry(domain, &dynamic.domain_list, node)
		update_domain_info(domain);

	list_for_each_entry(domain, &dynamic.domain_list, node)
		compute_governor_cpus(domain, &new_cpus);

update:
	update_governor_cpus(&new_cpus, "governor");
}

/******************************************************************************
 *                       emstune mode update notifier                         *
 ******************************************************************************/
bool dynamic_domain_first_cpu(int cpu)
{
	struct dynamic_dom *domain;

	list_for_each_entry(domain, &dynamic.domain_list, node) {
		if (cpumask_test_cpu(cpu, &domain->cpus)) {
			if (cpu == cpumask_first(&domain->cpus))
				return true;
			else
				return false;
		}
	}

	return false;
}

static int dynamic_mode_update_callback(struct notifier_block *nb,
				unsigned long val, void *v)
{
	struct emstune_set *cur_set = (struct emstune_set *)v;
	struct dynamic_dom *dom;
	int prev_en, new_en = false;

	/* update min_cpus */
	cpumask_copy(&dynamic.min_cpus, &cur_set->ecs_dynamic.min_cpus);

	/* update busy ratio */
	list_for_each_entry(dom, &dynamic.domain_list, node) {
		int cpu = cpumask_first(&dom->cpus);
		dom->busy_ratio = cur_set->ecs_dynamic.dynamic_busy_ratio[cpu];
		dom->nr_run_up_ratio = cur_set->ecs_dynamic.dynamic_nr_run_up_ratio[cpu];
		if (dom->busy_ratio)
			new_en = true;

		dom->help_misfit_thr_by_emstune = cur_set->ecs_dynamic.dynamic_help_misfit_thr[cpu];
	}

	prev_en = !(dynamic.governor_disabled & GOV_CTRL_BY_RATIO);
	if (new_en != prev_en)
		dynamic_control_governor(GOV_CTRL_BY_RATIO, new_en);

	return NOTIFY_OK;
}

static struct notifier_block dynamic_mode_update_notifier = {
	.notifier_call = dynamic_mode_update_callback,
};

/******************************************************************************
 *                      sysbusy state change notifier                         *
 ******************************************************************************/
static int dynamic_sysbusy_notifier_call(struct notifier_block *nb,
					unsigned long val, void *v)
{
	enum sysbusy_state state = *(enum sysbusy_state *)v;
	int enable;

	if (val != SYSBUSY_STATE_CHANGE)
		return NOTIFY_OK;

	enable = (state == SYSBUSY_STATE0);
	dynamic_control_governor(GOV_CTRL_BY_SYSBUSY, enable);

	return NOTIFY_OK;
}

static struct notifier_block dynamic_sysbusy_notifier = {
	.notifier_call = dynamic_sysbusy_notifier_call,
};

/******************************************************************************
 *				SYSFS		                              *
 ******************************************************************************/
static ssize_t show_dynamic_dom(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int ret = 0, i;
	struct dynamic_dom *domain;
	struct cpumask min_cpus, nr_help_cpus, misfit_help_cpus;

	if (list_empty(&dynamic.domain_list)) {
		ret += sprintf(buf + ret, "domain list empty\n");
		return ret;
	}

	list_for_each_entry(domain, &dynamic.domain_list, node) {
		cpumask_clear(&nr_help_cpus);
		for (i = 0; i < domain->num_of_nr_help_dom; i++)
			cpumask_or(&nr_help_cpus, &nr_help_cpus, &domain->nr_help_dom[i]->cpus);
		cpumask_clear(&misfit_help_cpus);
		for (i = 0; i < domain->num_of_misfit_help_dom; i++)
			cpumask_or(&misfit_help_cpus, &misfit_help_cpus, &domain->misfit_help_dom[i]->cpus);
		cpumask_and(&min_cpus, &domain->cpus, &dynamic.min_cpus);

		ret += sprintf(buf + ret, "[domain%d]\n", domain->id);
		ret += sprintf(buf + ret, "cpus : %*pbl\n",
					cpumask_pr_args(&domain->cpus));
		ret += sprintf(buf + ret, "nr_help_cpus : %*pbl\n",
					cpumask_pr_args(&nr_help_cpus));
		ret += sprintf(buf + ret, "misfit_help_cpus : %*pbl\n",
					cpumask_pr_args(&misfit_help_cpus));
		ret += sprintf(buf + ret, "min-cpus : %*pbl\n",
					cpumask_pr_args(&dynamic.min_cpus));
		ret += sprintf(buf + ret, "governor_cpus : %*pbl\n",
					cpumask_pr_args(&domain->governor_cpus));
		ret += sprintf(buf + ret, "emstune busy ratio : %u\n",
					domain->busy_ratio);
		ret += sprintf(buf + ret, "emstune nr_run_up_ratio : %d\n",
					domain->nr_run_up_ratio);
		ret += sprintf(buf + ret, "emstune help_misfit_thr : %d\n",
					domain->help_misfit_thr_by_emstune);
		ret += sprintf(buf + ret, "user help_misfit_thr : %d\n",
					domain->help_misfit_thr_by_user);
		ret += sprintf(buf + ret, " --------------------------------\n");
	}
	return ret;
}

static struct kobj_attribute dynamic_dom_attr =
__ATTR(domains, 0444, show_dynamic_dom, NULL);

static ssize_t show_dynamic_governor_disabled(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "disabled=%d\n", dynamic.governor_disabled);
}

static ssize_t store_dynamic_governor_disabled(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int disable;

	if (sscanf(buf, "%d", &disable) != 1)
		return -EINVAL;

	dynamic_control_governor(GOV_CTRL_BY_USER, !disable);

	return count;
}

static struct kobj_attribute dynamic_governor_enable_attr =
__ATTR(governor_disabled, 0644, show_dynamic_governor_disabled,
				store_dynamic_governor_disabled);

static ssize_t show_dynamic_ratio(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int ret = 0;
	struct dynamic_dom *domain;

	list_for_each_entry(domain, &dynamic.domain_list, node) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"domain=%d busy-ratio=%d\n",
				domain->id, domain->busy_ratio);
	}

	return ret;
}

static ssize_t store_dynamic_ratio(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int domain_id, ratio;
	struct dynamic_dom *domain;

	if (sscanf(buf, "%d %d", &domain_id, &ratio) != 2)
		return -EINVAL;

	if (domain_id < 0 || domain_id >= VENDOR_NR_CPUS)
		return -EINVAL;

	domain = dynamic_get_domain(domain_id);
	if (!domain)
		return -EINVAL;

	domain->busy_ratio = ratio;

	return count;
}
static struct kobj_attribute dynamic_ratio =
__ATTR(ratio, 0644, show_dynamic_ratio, store_dynamic_ratio);

static ssize_t show_dynamic_nr_run_up_unit(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int ret = 0;
	struct dynamic_dom *domain;

	list_for_each_entry(domain, &dynamic.domain_list, node) {
		ret += snprintf(buf + ret, PAGE_SIZE - ret,
				"domain=%d nr_run_up_ratio=%d\n",
				domain->id, domain->nr_run_up_ratio);
	}

	return ret;
}

static ssize_t store_dynamic_nr_run_up_unit(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int domain_id, ratio;
	struct dynamic_dom *domain;

	if (sscanf(buf, "%d %d", &domain_id, &ratio) != 2)
		return -EINVAL;

	if (domain_id < 0 || domain_id >= VENDOR_NR_CPUS)
		return -EINVAL;

	domain = dynamic_get_domain(domain_id);
	if (!domain)
		return -EINVAL;

	domain->nr_run_up_ratio = ratio;

	return count;
}
static struct kobj_attribute dynamic_nr_run_up_unit =
__ATTR(avg_nr_run_unit, 0644, show_dynamic_nr_run_up_unit, store_dynamic_nr_run_up_unit);

static ssize_t show_dynamic_min_cpus(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "min-cpus: %*pbl\n",cpumask_pr_args(&dynamic.min_cpus));
}

#define STR_LEN 7
static ssize_t store_dynamic_min_cpus(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct cpumask new_cpus;
	char str[STR_LEN], *str_ptr;

	if (sscanf(buf, "%6s", str) != 2)
		return -EINVAL;

	str_ptr = &str[0];
	if (str[0] == '0' && toupper(str[1]) == 'X')
		str_ptr = str_ptr + 2;
	if (cpumask_parse(str_ptr, &new_cpus) || cpumask_empty(&new_cpus))
		return -EINVAL;

	cpumask_copy(&dynamic.min_cpus, &new_cpus);

	return count;
}

static struct kobj_attribute dynamic_min_cpus =
__ATTR(min_cpus, 0644, show_dynamic_min_cpus, store_dynamic_min_cpus);

static ssize_t show_dynamic_misfit_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	ssize_t count = 0;
	struct dynamic_dom *domain;

	list_for_each_entry(domain, &dynamic.domain_list, node) {
		count += snprintf(buf + count, PAGE_SIZE - count,
				"domain=%d help_misfit_thr_by_user=%d\n",
				domain->id, domain->help_misfit_thr_by_user);
	}

	return count;
}

static bool parse_input(char *input, int *data)
{
	char *arg, *ptr;
	int i = 0;

	arg = input;
	ptr = strsep(&arg, ",");

	while (ptr && i < 100) {
		if (kstrtoint(ptr, 10, &data[i]))
			break;

		ptr = strsep(&arg, ",");
		i++;
	}

	return i == dynamic.nr_domains;
}

static ssize_t store_dynamic_misfit_threshold(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct dynamic_dom *domain;
	char str[100];
	int data[100], id = 0;

	if (sscanf(buf, "%99s", str) != 1)
		return -EINVAL;

	if (!parse_input(str, data))
		return -EINVAL;

	list_for_each_entry(domain, &dynamic.domain_list, node)
		domain->help_misfit_thr_by_user = data[id++];

	return count;
}
static struct kobj_attribute dynamic_help_misfit_threshold =
__ATTR(help_misfit_thr, 0644, show_dynamic_misfit_threshold, store_dynamic_misfit_threshold);

static int dynamic_sysfs_init(struct kobject *ecs_gov_kobj)
{
	int ret;

	dynamic.kobj = kobject_create_and_add("dynamic", ecs_gov_kobj);
	if (!dynamic.kobj) {
		pr_info("%s: fail to create node\n", __func__);
		return -EINVAL;
	}

	ret = sysfs_create_file(dynamic.kobj, &dynamic_dom_attr.attr);
	if (ret)
		pr_warn("%s: failed to create dynamic sysfs\n", __func__);
	ret = sysfs_create_file(dynamic.kobj, &dynamic_governor_enable_attr.attr);
	if (ret)
		pr_warn("%s: failed to create dynamic sysfs\n", __func__);

	ret = sysfs_create_file(dynamic.kobj, &dynamic_ratio.attr);
	if (ret)
		pr_warn("%s: failed to create dynamic sysfs\n", __func__);

	ret = sysfs_create_file(dynamic.kobj, &dynamic_nr_run_up_unit.attr);
	if (ret)
		pr_warn("%s: failed to create dynamic sysfs\n", __func__);

	ret = sysfs_create_file(dynamic.kobj, &dynamic_min_cpus.attr);
	if (ret)
		pr_warn("%s: failed to create dynamic sysfs\n", __func__);

	ret = sysfs_create_file(dynamic.kobj, &dynamic_help_misfit_threshold.attr);
	if (ret)
		pr_warn("%s: failed to create dynamic sysfs\n", __func__);

	return ret;
}

/******************************************************************************
 *                               initialization                               *
 ******************************************************************************/
static int
init_dynamic_dom(struct device_node *dn, struct list_head *domain_list, int domain_id)
{
	int cpu, ret = 0;
	const char *buf;
	struct dynamic_dom *domain;
	struct cpumask mask;

	domain = kzalloc(sizeof(struct dynamic_dom), GFP_KERNEL);
	if (!domain) {
		pr_err("%s: fail to alloc dynamic domain\n", __func__);
		return -ENOMEM;
	}

	if (!of_property_read_string(dn, "cpus", &buf))
		ems_cpulist_parse(buf, &domain->cpus);

	/* check whether there is available physical cpu in this domain or not */
	cpumask_copy(&mask, get_possible_phy_cpus());
	if (!cpumask_intersects(&domain->cpus, &mask)) {
		kfree(domain);
		return -1;
	}

	/* re-mapping physical cpu id into logical cpu id */
	cpumask_clear(&mask);
	for_each_cpu(cpu, &domain->cpus) {
		int logid = get_cpu_log_id(cpu);
		if (logid != -1)
			cpumask_set_cpu(logid, &mask);
	}
	cpumask_copy(&domain->cpus, &mask);

	domain->id = domain_id;
	domain->busy_ratio = DEFAULT_BUSY_RATIO;
	cpumask_copy(&domain->governor_cpus, &domain->cpus);
	domain->nr_cpus = cpumask_weight(&domain->cpus);

	list_add_tail(&domain->node, domain_list);

	return ret;
}

static struct dynamic_dom *find_domain_by_phy_cpus(struct cpumask *phy_cpus)
{
	struct dynamic_dom *domain;
	struct cpumask log_cpus;
	int cpu;

	/* check whether there is available physical cpu in this domain or not */
	if (!cpumask_intersects(phy_cpus, get_possible_phy_cpus()))
		return NULL;

	/* re-mapping physical cpu id into logical cpu id */
	cpumask_clear(&log_cpus);
	for_each_cpu(cpu, phy_cpus) {
		int lcpu = get_cpu_log_id(cpu);
		if (lcpu != -1)
			cpumask_set_cpu(lcpu, &log_cpus);
	}

	/* find domain */
	list_for_each_entry(domain, &dynamic.domain_list, node) {
		if (cpumask_equal(&domain->cpus, &log_cpus))
			return domain;
	}

	return NULL;
}

static void build_help_domains(struct device_node *dn, int nr_domains)
{
	struct device_node *child;

	for_each_child_of_node(dn, child) {
		struct cpumask phy_cpus;
		struct dynamic_dom *dom;
		const char *buf[VENDOR_NR_CPUS];
		int i, num_of_doms;

		/* find this domain using phy_cpus */
		if (!of_property_read_string(child, "cpus", &buf[0]))
			ems_cpulist_parse(buf[0], &phy_cpus);

		dom = find_domain_by_phy_cpus(&phy_cpus);
		if (!dom)
			continue;

		/* alloc help domains */
		dom->nr_help_dom = kmalloc_array(nr_domains, sizeof(struct dynamic_dom), GFP_KERNEL);
		dom->misfit_help_dom = kmalloc_array(nr_domains, sizeof(struct dynamic_dom), GFP_KERNEL);

		/* build nr_help domains */
		num_of_doms = of_property_count_strings(child, "nr-help");
		if (num_of_doms > 0) {
			of_property_read_string_array(child, "nr-help", buf, num_of_doms);

			for (i = 0; i < num_of_doms; i++) {
				struct cpumask help_mask;
				struct dynamic_dom *help_dom;

				/* find help domain */
				ems_cpulist_parse(buf[i], &help_mask);
				help_dom = find_domain_by_phy_cpus(&help_mask);
				if (!help_dom)
					continue;

				dom->nr_help_dom[dom->num_of_nr_help_dom++] = help_dom;
			}
		}

		/* build misfit_help domains */
		num_of_doms = of_property_count_strings(child, "misfit-help");
		if (num_of_doms > 0) {
			of_property_read_string_array(child, "misfit-help", buf, num_of_doms);

			for (i = 0; i < num_of_doms; i++) {
				struct cpumask help_mask;
				struct dynamic_dom *help_dom;

				/* find help domain */
				ems_cpulist_parse(buf[i], &help_mask);
				help_dom = find_domain_by_phy_cpus(&help_mask);
				if (!help_dom)
					continue;

				dom->misfit_help_dom[dom->num_of_misfit_help_dom++] = help_dom;
			}
		}
	}
}

static int init_dynamic_dom_list(void)
{
	int ret = 0, domain_id = 0;
	struct device_node *dn, *domain_dn;

	dn = of_find_node_by_path("/ems/ecs_dynamic");
	if (!dn) {
		pr_err("%s: fail to get dynamic.device node\n", __func__);
		return -EINVAL;
	}

	INIT_LIST_HEAD(&dynamic.domain_list);

	for_each_child_of_node(dn, domain_dn) {
		if (!init_dynamic_dom(domain_dn, &dynamic.domain_list, domain_id))
			domain_id++;
	}

	build_help_domains(dn, domain_id);

	cpumask_copy(&dynamic.min_cpus, cpu_possible_mask);

	of_property_read_u32(dn, "default-gov", &dynamic.default_gov);

	dynamic.nr_domains = domain_id;

	return ret;
}

static void dynamic_start(void)
{
	dynamic_control_governor(GOV_CTRL_BY_USER, true);
}

#define UPDATE_PERIOD	(1)		/* 1 jiffies */
static void dynamic_update(void)
{
	unsigned long now = jiffies;

	if (!raw_spin_trylock(&dynamic_lock))
		return;

	if (dynamic.governor_disabled)
		goto out;

	if (now < (dynamic.last_update_jiffies + UPDATE_PERIOD))
		goto out;

	update_dynamic_governor_cpus();

	dynamic.last_update_jiffies = now;

out:
	raw_spin_unlock(&dynamic_lock);
}

static void dynamic_stop(void)
{
	dynamic_control_governor(GOV_CTRL_BY_USER, false);
}

static const struct cpumask *dynamic_get_target_cpus(void)
{
	return &dynamic.governor_cpus;
}

static struct ecs_governor dynamic_gov = {
	.name = "dynamic",
	.update = dynamic_update,
	.enqueue_update = NULL,
	.start = dynamic_start,
	.stop = dynamic_stop,
	.get_target_cpus = dynamic_get_target_cpus,
};

int ecs_gov_dynamic_init(struct kobject *ems_kobj)
{
	if (init_dynamic_dom_list()) {
		pr_info("ECS governor will not affect scheduler\n");

		/* Reset domain list and dynamic.out_of_governing_cpus */
		INIT_LIST_HEAD(&dynamic.domain_list);
		/* disable governor */
		dynamic_control_governor(GOV_CTRL_BY_INIT, false);
	}

	dynamic_control_governor(GOV_CTRL_BY_USER, false);

	dynamic_sysfs_init(ecs_get_governor_object());
	emstune_register_notifier(&dynamic_mode_update_notifier);
	sysbusy_register_notifier(&dynamic_sysbusy_notifier);

	ecs_governor_register(&dynamic_gov, dynamic.default_gov);

	return 0;
}
