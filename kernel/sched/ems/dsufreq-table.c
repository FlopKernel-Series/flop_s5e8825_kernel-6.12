// SPDX-License-Identifier: GPL-2.0
/* Kept apart from dsufreq: sharing the table would cycle the two modules. */

#include <linux/of.h>
#include <linux/slab.h>
#include <soc/samsung/cal-if.h>
#include <soc/samsung/ems.h>

#define DSUFREQ_ENTRY_INVALID	(~0u)

struct fv_table {
	unsigned int freq;
	unsigned int volt;
};

static unsigned long *dsufreq_table;
static unsigned int dsufreq_table_size;
static unsigned int dsufreq_cal_id;

static int et_init_dsufreq_table(void)
{
	struct device_node *dn;
	unsigned int max_freq = ~0u, min_freq = 0;
	unsigned long *freq_table;
	unsigned int *dt_table;
	int i, size;

	if (dsufreq_table)
		return 0;

	dn = of_find_compatible_node(NULL, NULL, "samsung,exynos-dsufreq");
	if (!dn)
		return -ENODEV;

	if (of_property_read_u32(dn, "cal-id", &dsufreq_cal_id)) {
		of_node_put(dn);
		return -EINVAL;
	}

	of_property_read_u32(dn, "min-freq", &min_freq);
	of_property_read_u32(dn, "max-freq", &max_freq);

	size = of_property_count_u32_elems(dn, "table");
	if (size > 0) {
		dt_table = kcalloc(size, sizeof(*dt_table), GFP_KERNEL);
		if (!dt_table) {
			of_node_put(dn);
			return -ENOMEM;
		}

		if (of_property_read_u32_array(dn, "table", dt_table, size)) {
			kfree(dt_table);
			of_node_put(dn);
			return -EINVAL;
		}

		freq_table = kcalloc(size, sizeof(*freq_table), GFP_KERNEL);
		if (!freq_table) {
			kfree(dt_table);
			of_node_put(dn);
			return -ENOMEM;
		}

		for (i = 0; i < size; i++)
			freq_table[i] = dt_table[i];

		kfree(dt_table);
	} else {
		size = cal_dfs_get_lv_num(dsufreq_cal_id);
		if (size <= 0) {
			of_node_put(dn);
			return -EINVAL;
		}

		freq_table = kcalloc(size, sizeof(*freq_table), GFP_KERNEL);
		if (!freq_table) {
			of_node_put(dn);
			return -ENOMEM;
		}

		if (cal_dfs_get_rate_table(dsufreq_cal_id, freq_table)) {
			kfree(freq_table);
			of_node_put(dn);
			return -EINVAL;
		}
	}

	/* Entries outside of the usable range are marked, as the driver does */
	for (i = 0; i < size; i++)
		if (freq_table[i] > max_freq || freq_table[i] < min_freq)
			freq_table[i] = DSUFREQ_ENTRY_INVALID;

	of_node_put(dn);

	dsufreq_table = freq_table;
	dsufreq_table_size = size;

	return 0;
}

unsigned long *ems_get_dsufreq_table(int *table_size)
{
	if (et_init_dsufreq_table())
		return NULL;

	*table_size = dsufreq_table_size;
	return dsufreq_table;
}

int ems_get_dsufreq_volt_table(unsigned int *volt_table, int table_size)
{
	struct fv_table *fv_table;
	int i, ret;

	if (et_init_dsufreq_table())
		return -ENODEV;

	if (table_size > dsufreq_table_size)
		return -EINVAL;

	fv_table = kcalloc(table_size, sizeof(*fv_table), GFP_KERNEL);
	if (!fv_table)
		return -ENOMEM;

	for (i = 0; i < table_size; i++)
		fv_table[i].freq = dsufreq_table[i];

	ret = cal_dfs_get_freq_volt_table(dsufreq_cal_id, fv_table, table_size);
	if (!ret)
		for (i = 0; i < table_size; i++)
			volt_table[i] = fv_table[i].volt;

	kfree(fv_table);

	return ret;
}
