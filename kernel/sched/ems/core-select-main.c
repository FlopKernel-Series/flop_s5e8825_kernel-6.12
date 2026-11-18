/*
 * Core Exynos Mobile Scheduler
 *
 * Copyright (C) 2022 Samsung Electronics Co., Ltd
 */

#include <soc/samsung/ems.h>

static void core_select_rq_init(void)
{
	int cpu;

	for_each_cpu(cpu, cpu_possible_mask) {
		struct rq *rq = cpu_rq(cpu);

		INIT_LIST_HEAD(ems_rq_tex_list(rq));
		ems_rq_nr_tex(rq) = 0;
		ems_rq_nr_high_prio(rq) = 0;
		ems_rq_nr_launch(rq) = 0;
		ems_rq_nr_reserved_task(rq) = 0;
	}
}

void ems_core_select_init(struct platform_device *pdev)
{
	struct kobject *ems_kobj = &pdev->dev.kobj;

	core_select_rq_init();
	core_init(pdev->dev.of_node);
	ntu_init(ems_kobj);
	frt_init();
	sysbusy_init(ems_kobj);
	gsc_init(ems_kobj);
	emstune_init(ems_kobj, pdev->dev.of_node, &pdev->dev);

	lb_init();
	mhdvfs_init(ems_kobj);

	high_prio_init();
}

