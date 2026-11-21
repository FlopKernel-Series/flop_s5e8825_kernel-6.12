#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched/clock.h>

#define PHYSICAL_ID_SHIFT	(8)

/*
 * Physical cpus	   : O O / O O O /  O  O /  O
 * Available Physical cpus : O O / O O O /  X  X /  O
 * Logical cpus		   : O O / O O O /  O
 * cpu_physical_id	   : 0 1 / 2 3 4 /  7 -1 / -1
 * cpu_logical_id	   : 0 1 / 2 3 4 / -1 -1 /  5
 */

int cpu_physical_id[CONFIG_VENDOR_NR_CPUS];
int cpu_logical_id[CONFIG_VENDOR_NR_CPUS];
int physical_cluster_id[CONFIG_VENDOR_NR_CPUS];
struct cpumask possible_phy_cpus;
struct cpumask possible_phy_cpus;

const struct cpumask *get_possible_phy_cpus(void)
{
	return (const struct cpumask *) &possible_phy_cpus;
}
EXPORT_SYMBOL_GPL(get_possible_phy_cpus);

int get_cpu_phy_id(int cpu)
{
	return cpu_physical_id[cpu];
}
EXPORT_SYMBOL_GPL(get_cpu_phy_id);

int get_cpu_log_id(int cpu)
{
	return cpu_logical_id[cpu];
}
EXPORT_SYMBOL_GPL(get_cpu_log_id);

int phycluster_of_phycpu(int cpu)
{
	return physical_cluster_id[cpu];
}
EXPORT_SYMBOL_GPL(phycluster_of_phycpu);

int phycluster_of_logcpu(int lcpu)
{
	int pcpu = get_cpu_phy_id(lcpu);
	return physical_cluster_id[pcpu];
}
EXPORT_SYMBOL_GPL(phycluster_of_logcpu);

static void build_phycluster_id(struct device_node *dn)
{
	int num_of_cl, cpu, cl_idx;
	const char *buf[CONFIG_VENDOR_NR_CPUS];

	memset(&physical_cluster_id, -1, sizeof(int) * (int)CONFIG_VENDOR_NR_CPUS);

	num_of_cl = of_property_count_strings(dn, "cpus-map");
	if (num_of_cl == 0)
		return;

	of_property_read_string_array(dn, "cpus-map", buf, num_of_cl);
	for (cl_idx = 0; cl_idx < num_of_cl; cl_idx++) {
		struct cpumask cpus;
		bitmap_parselist(buf[cl_idx], cpumask_bits(&cpus), (CONFIG_VENDOR_NR_CPUS));
		for_each_cpu(cpu, &cpus) {
			physical_cluster_id[cpu] = cl_idx;
			pr_info("phy_cpu%d phy_cluster=%d\n",cpu, cl_idx);
		}
	}
}

static int exynos_cpu_phyid_probe(struct platform_device *pdev)
{
	struct device_node *dn;
	u64 hwid;
	const __be32 *cell;
	int cpu, ac, len;
	int cnt = 0;
	int hole_phyid;
	bool has_hole = false;

	memset(&cpu_physical_id, -1, sizeof(int) * (int)CONFIG_VENDOR_NR_CPUS);
	memset(&cpu_logical_id, -1, sizeof(int) * (int)CONFIG_VENDOR_NR_CPUS);

	if(!of_property_read_u32(pdev->dev.of_node, "cpu-hole-phyid", &hole_phyid))
		has_hole = true;

	for_each_of_cpu_node(dn) {
		int phyid;
		ac = of_n_addr_cells(dn);
		cell = of_get_property(dn, "reg", &len);
		if (!cell || !ac || ((sizeof(*cell) * ac) > len))
			return -1;

		hwid = of_read_number(cell, ac);

		if (hwid & ~MPIDR_HWID_BITMASK)
			goto next;

		cpu_physical_id[cnt] = phyid = (hwid >> PHYSICAL_ID_SHIFT);
		if(has_hole && phyid >= hole_phyid)
			phyid--;

		cpu_logical_id[phyid] = cnt;

		pr_info("%s: cpu_logid%d: hwid[0x%llx], cpu_phyid[%u]\n",
				__func__, cnt, hwid, cpu_physical_id[cnt]);
next:
		cnt++;
	}

	/* build possible physical_cpus_mask */
	cpumask_clear(&possible_phy_cpus);
	for_each_possible_cpu(cpu) {
		int phyid = get_cpu_phy_id(cpu);
		if(has_hole && phyid >= hole_phyid)
			phyid--;
		if (phyid != -1)
			cpumask_set_cpu(phyid, &possible_phy_cpus);
	}
	pr_info("%s: possible physical cpus map : %#x\n", __func__,
		*(unsigned int *)cpumask_bits(&possible_phy_cpus));

	build_phycluster_id(pdev->dev.of_node);

	return 0;
}

static const struct of_device_id of_exynos_cpu_phyid_match[] = {
	{ .compatible = "samsung,exynos-cpu-phyid", },
	{ },
};
MODULE_DEVICE_TABLE(of, of_exynos_cpu_phyid_match);

static struct platform_driver exynos_cpu_phyid_driver = {
	.driver = {
		.name = "exynos-cpu-phyid",
		.owner = THIS_MODULE,
		.of_match_table = of_exynos_cpu_phyid_match,
	},
	.probe		= exynos_cpu_phyid_probe,
};

static int __init exynos_cpu_phyid_driver_init(void)
{
	return platform_driver_register(&exynos_cpu_phyid_driver);
}
arch_initcall(exynos_cpu_phyid_driver_init);

static void __exit exynos_cpu_phyid_driver_exit(void)
{
	platform_driver_unregister(&exynos_cpu_phyid_driver);
}
module_exit(exynos_cpu_phyid_driver_exit);

MODULE_DESCRIPTION("Exynos CPU PHYID driver");
MODULE_LICENSE("GPL");

