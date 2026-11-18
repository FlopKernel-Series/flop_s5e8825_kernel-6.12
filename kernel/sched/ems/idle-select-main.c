/*
 * Core Exynos Mobile Scheduler
 *
 * Copyright (C) 2022 Samsung Electronics Co., Ltd
 */

#include <soc/samsung/ems.h>

void ems_idle_select_init(struct platform_device *pdev)
{
	struct kobject *ems_kobj = &pdev->dev.kobj;

	ecs_gov_dynamic_init(ems_kobj);
	fv_init(ems_kobj);
	halo_governor_init(ems_kobj);
}
