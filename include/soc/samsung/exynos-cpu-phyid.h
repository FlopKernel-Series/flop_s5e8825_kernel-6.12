#ifndef __EXYNOS_CPU_PHYID_H
#define __EXYNOS_CPU_PHYID_H __FILE__

extern int get_cpu_phy_id(int cpu);
extern int get_cpu_log_id(int cpu);
extern const struct cpumask *get_possible_phy_cpus(void);
extern int phycluster_of_logcpu(int lcpu);
extern int phycluster_of_phycpu(int cpu);

#endif

