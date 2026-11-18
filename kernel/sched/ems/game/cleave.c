#include <soc/samsung/game.h>

#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

#define DEFAULT_HEAVY_TASK_RATIO	20
#define DEFAULT_IMPORTANT_THRES		100

struct cleave_base_env {
	int			prev_task_idx;	/* prev cleave task idx */
	unsigned long		avg_util;	/* 1 period avg util */
	unsigned long		avg_util_wo;	/* util without reserved tasks */
};

static struct task_cleave {
	raw_spinlock_t		lock;

	struct cpumask		policy_cpus;
	struct cpumask		fast_cpus;

	int			nr_tasks;
	/* Descending by task heavyness */
	struct cleave_task	clv_tasks[MAX_NR_CLEAVE];	/* NOT CPU ORDER */

	/* cleave snapshot for cpu stat */
	struct cleave_base_env	env[VENDOR_NR_CPUS];	/* base environment */
	struct cleave_snapshot	cs[VENDOR_NR_CPUS];	/* snapshot for energy computing */

	/* heavy task ratio within the sum of total utilization */
	int			heavy_task_ratio;
	/* Threshold at which an important task can be cleaved */
	int			important_threshold;
} clv;

enum cleave_reason {
	CLEAVE_ABNORMAL,
	CLEAVE_NORMAL,
	CLEAVE_TRAINING,
	CLEAVE_EXECUTION_INVALID,
	CLEAVE_EXECUTION_SUCCESS,
	CLEAVE_BIG_BOOST,
	END_OF_CLEAVE_REASON,
};

static const char *cleave_reason_name[] = {
	"abnormal",
	"normal",
	"training",
	"execution_invliad",
	"execution_success",
	"big_boost",
};

void cleave_get_policy_cpus(struct cpumask *cpus)
{
	if (egg_is_normal_game())
		cpumask_copy(cpus, &clv.fast_cpus);
	else
		cpumask_copy(cpus, &clv.policy_cpus);
}

/********************************************************************************
 *				Task Cleave					*
 ********************************************************************************/
#define add_positive(_ptr, _val) do {                           \
	typeof(_ptr) ptr = (_ptr);                              \
	typeof(_val) val = (_val);                              \
	typeof(*ptr) res, var = READ_ONCE(*ptr);                \
								\
	res = var + val;                                        \
								\
	if (val < 0 && res > var)                               \
		res = 0;                                        \
								\
	WRITE_ONCE(*ptr, res);                                  \
} while (0)

#define lsub_positive(_ptr, _val) do {				\
	typeof(_ptr) ptr = (_ptr);				\
	*ptr -= min_t(typeof(*ptr), *ptr, _val);		\
} while (0)

static inline int cpu_overutilized_with_task(int cpu, int util)
{
	return (ems_get_actual_cpu_capacity(cpu) << 10) < (util * 1280);
}

static unsigned long find_next_cap(const struct cpumask *cluster_cpus)
{
	unsigned long next_cap = 0;
	int cpu;

	for_each_cpu_and(cpu, cluster_cpus, cpu_active_mask) {
		unsigned long util = clv.env[cpu].avg_util;

		if (util > next_cap)
			next_cap = util;
	}

	return et_find_next_cap(cpumask_first(cluster_cpus), next_cap);
}

static void migrate_util(unsigned long util_sum, struct cpumask *reserved_cpus)
{
	struct cpumask dct_cpus, policy_cpus;
	unsigned long capacity, spare, add;
	int pcpu, cpu;

	if (!util_sum)
		return;

	cleave_get_policy_cpus(&policy_cpus);

	dct_get_available_cpus(&dct_cpus);
	if (egg_throttle_fastest_cpu())
		cpumask_clear_cpu(cpumask_first(cpu_fastest_mask()), &dct_cpus);

	/* 1. migrate util within cleave policy cpus that are NOT SPARED by dct */
	for_each_cpu(pcpu, &policy_cpus) {
		if (pcpu != cpumask_first(cpu_clustergroup_mask(pcpu)))
			continue;

		capacity = find_next_cap(cpu_clustergroup_mask(pcpu));

		for_each_cpu(cpu, cpu_clustergroup_mask(pcpu)) {
			if (!cpu_active(cpu) || !ecs_cpu_available(cpu, NULL))
				continue;

			if (cpumask_test_cpu(cpu, reserved_cpus))
				continue;

			if (!cpumask_test_cpu(cpu, &dct_cpus))
				continue;

			spare = capacity - clv.env[cpu].avg_util;
			add = (util_sum >= spare) ? spare : util_sum;

			add_positive(&clv.env[cpu].avg_util, add);
			add_positive(&clv.env[cpu].avg_util_wo, add);
			util_sum -= add;
		}
	}
	if (!util_sum)
		return;

	/* 2. migrate util within cleave policy cpus that are SPARED by dct */
	for_each_cpu(pcpu, &policy_cpus) {
		if (pcpu != cpumask_first(cpu_clustergroup_mask(pcpu)))
			continue;

		capacity = find_next_cap(cpu_clustergroup_mask(pcpu));

		for_each_cpu(cpu, cpu_clustergroup_mask(pcpu)) {
			if (!cpu_active(cpu) || !ecs_cpu_available(cpu, NULL))
				continue;

			if (cpumask_test_cpu(cpu, reserved_cpus))
				continue;

			if (cpumask_test_cpu(cpu, &dct_cpus))
				continue;

			spare = capacity - clv.env[cpu].avg_util;
			add = (util_sum >= spare) ? spare : util_sum;

			add_positive(&clv.env[cpu].avg_util, add);
			add_positive(&clv.env[cpu].avg_util_wo, add);
			util_sum -= add;
		}
	}
	if (!util_sum)
		return;

	/* 3. migrate util to all active cpus except reserved cpus */
	for_each_cpu(cpu, cpu_active_mask) {
		if (!ecs_cpu_available(cpu, NULL))
			continue;

		if (cpumask_test_cpu(cpu, reserved_cpus))
			continue;

		capacity = ems_get_origin_cpu_capacity(cpu);
		spare = capacity - clv.env[cpu].avg_util;
		add = (util_sum >= spare) ? spare : util_sum;

		add_positive(&clv.env[cpu].avg_util, add);
		add_positive(&clv.env[cpu].avg_util_wo, add);
		util_sum -= add;
	}

	/* FIX. 4. reserved cpus reclaim the remaining util */
	add = util_sum / cpumask_weight(reserved_cpus);
	for_each_cpu(cpu, reserved_cpus) {
		add_positive(&clv.env[cpu].avg_util, add);
		add_positive(&clv.env[cpu].avg_util_wo, add);
	}
}

static void setup_base_util(void)
{
	struct cpumask reserved_cpus;
	int i = 0, cpu;
	unsigned long migratable_util = 0;

	cpumask_clear(&reserved_cpus);

	/* set up 100msec avg cpu util */
	for_each_cpu(cpu, cpu_active_mask) {
		clv.env[cpu].prev_task_idx = -1;
		clv.env[cpu].avg_util = clv.env[cpu].avg_util_wo = hist_util_cpu(cpu);
	}

	/* sub reserved task util from avg cpu util */
	for (i = 0; i < clv.nr_tasks; i++) {
		struct cleave_task *ct = &clv.clv_tasks[i];
		int prev_cpu = ct->ets->reserved_cpu;
		unsigned long others_util = 0;

		if (invalid_cpu(prev_cpu))
			continue;

		if (clv.env[prev_cpu].avg_util >= ct->util)
			others_util = clv.env[prev_cpu].avg_util - ct->util;

		/* sub others util from reserved cpu */
		lsub_positive(&clv.env[prev_cpu].avg_util, others_util);
		lsub_positive(&clv.env[prev_cpu].avg_util_wo, others_util);
		migratable_util += others_util;

		lsub_positive(&clv.env[prev_cpu].avg_util_wo, ct->util);
		clv.env[prev_cpu].prev_task_idx = i;

		cpumask_set_cpu(prev_cpu, &reserved_cpus);
	}

	migrate_util(migratable_util, &reserved_cpus);

	if (trace_cleave_setup_util_env_enabled()) {
		for_each_cpu(cpu, cpu_active_mask) {
			struct cleave_task *ct = NULL;
			struct cleave_base_env *env = &clv.env[cpu];

			if (env->prev_task_idx >= 0)
				ct = &clv.clv_tasks[env->prev_task_idx];

			if (ct)
				trace_cleave_setup_util_env(cpu, env->avg_util, env->avg_util_wo, ct->ets, ct->util);
			else
				trace_cleave_setup_util_env(cpu, env->avg_util, env->avg_util_wo, NULL, 0);
		}
	}
}

#define MIN_SPARE_CAP_THR	50
static void set_best_perf_cpu(struct cleave_task *ct, struct cpumask *reserved)
{
	int cpu;
	int max_spare_cpu = -1, max_spare_cap = INT_MIN;
	int max_cap_cpu = -1, max_cap= INT_MIN;

	for_each_cpu_and(cpu, cpu_active_mask, &ct->ets->cpus_ptr) {
		int cap = ems_get_actual_cpu_capacity(cpu);
		int spare_cap = max((cap - clv.env[cpu].avg_util_wo), (int) 0);

		if (egg_throttle_fastest_cpu() && !egg_is_normal_game() &&
			cpumask_test_cpu(cpu, cpu_fastest_mask()))
			continue;

		if (cpumask_test_cpu(cpu, reserved))
			continue;

		if (cap >= max_cap) {
			max_cap = cap;
			max_cap_cpu = cpu;
		}

		if ((spare_cap >= max_spare_cap) && (spare_cap > MIN_SPARE_CAP_THR)) {
			max_spare_cap = spare_cap;
			max_spare_cpu = cpu;
		}
	}

	if (!invalid_cpu(max_spare_cpu)) {
		cpumask_set_cpu(max_spare_cpu, &ct->available_cpus);
		cpumask_set_cpu(max_spare_cpu, reserved);
		return;
	}

	if (!invalid_cpu(max_cap_cpu)) {
		cpumask_set_cpu(max_cap_cpu, &ct->available_cpus);
		cpumask_set_cpu(max_cap_cpu, reserved);
		return;
	}
}

static void correct_available_cpus(struct cpumask *only_available_cpu, int end_idx)
{
	int i;
	struct cpumask temp;

	/* check available_cpus of heavier tasks without only_avilable_cpu */
	for (i = 0; i < end_idx; i++) {
		struct cleave_task *ct = &clv.clv_tasks[i];

		cpumask_andnot(&temp, &ct->available_cpus, only_available_cpu);
		if (cpumask_empty(&temp))
			return;
	}

	for (i = 0; i < end_idx; i++) {
		struct cleave_task *ct = &clv.clv_tasks[i];

		cpumask_andnot(&ct->available_cpus, &ct->available_cpus, only_available_cpu);
		cpumask_copy(&ct->available_cpus_org, &ct->available_cpus);
	}
}

static void setup_training_clv_task(struct cpumask *reserved_cpus)
{
	struct cpumask candidates;
	struct cleave_task *candi_tasks[NUM_OF_TRAINING_CPU] = { NULL, };
	int training_cpu[NUM_OF_TRAINING_CPU] = { -1, };
	int fast_cnt = INT_MAX, slow_cnt = INT_MAX;
	int idx = 0, cpu;

	cpumask_and(&candidates, cpu_active_mask, ecs_max_cpus());

	/* if we can't use fastest cpu, give up training */
	if (!cpumask_intersects(&candidates, cpu_fastest_mask()))
		return;

	/* set up training cpu */
	while (!cpumask_empty(&candidates)) {
		cpu = cpumask_last(&candidates);

		training_cpu[idx++] = cpu;

		cpumask_andnot(&candidates, &candidates, cpu_clustergroup_mask(cpu));

		if (idx == NUM_OF_TRAINING_CPU)
			break;
	}

	if (unlikely(idx != NUM_OF_TRAINING_CPU))
		return;

	/* find the least execution task */
	for (idx = 0; idx < clv.nr_tasks; idx++) {
		struct cleave_task *ct = &clv.clv_tasks[idx];
		struct egg_training_info *eti = &ct->ets->training;

		if (!eti->active)
			continue;

		if (eti->count[FAST_CPU] < fast_cnt) {
			candi_tasks[FAST_CPU] = ct;
			fast_cnt = eti->count[FAST_CPU];
			continue;
		}
	}

	/* find the least execution task */
	for (idx = 0; idx < clv.nr_tasks; idx++) {
		struct cleave_task *ct = &clv.clv_tasks[idx];
		struct egg_training_info *eti = &ct->ets->training;

		if (ct == candi_tasks[FAST_CPU])
			continue;

		if (!eti->active)
			continue;

		if (eti->count[SLOW_CPU] < slow_cnt) {
			candi_tasks[SLOW_CPU] = ct;
			slow_cnt = eti->count[SLOW_CPU];
		}
	}

	if (unlikely(fast_cnt == INT_MAX || slow_cnt == INT_MAX))
		return;

	for (idx = 0; idx < NUM_OF_TRAINING_CPU; idx++) {
		struct cleave_task *ct = candi_tasks[idx];

		candi_tasks[idx]->ets->training.count[idx] += 1;

		cpumask_set_cpu(training_cpu[idx], &ct->available_cpus);
		cpumask_copy(&ct->available_cpus_org, &ct->available_cpus);

		cpumask_set_cpu(training_cpu[idx], reserved_cpus);

		ct->reason = CLEAVE_TRAINING;
	}
}

static void set_execution_task_state(struct cleave_task *ct, int dst_cpu,
		struct cpumask *reserved_cpus, enum ep_execution_state state)
{
	if (unlikely(!ct || !ct->ets))
		return;

	if (ct && ct->ets) {
		egg_set_execution_task(ct->ets->task);
		cpumask_set_cpu(dst_cpu, reserved_cpus);
		cpumask_set_cpu(dst_cpu, &ct->available_cpus);
		cpumask_copy(&ct->available_cpus_org, &ct->available_cpus);
		ct->reason = state;
	}

	egg_set_execution_state(state);
	trace_cleave_set_execution_state(ep_execution_state_name[state]);
}

#define REDUCTION_RATIO_THR	5
static void setup_execution_clv_task_try(struct cpumask *reserved_cpus)
{
	/* find best efficiency task on fastest cpu */
	struct cleave_task *best = NULL;
	int max_util_reduction = 0, sec_util_reduction = 0;
	int fastest = cpumask_last(cpu_fastest_mask());
	int idx;

	for (idx = 0; idx < clv.nr_tasks; idx++) {
		struct cleave_task *ct = &clv.clv_tasks[idx];
		struct egg_training_info *eti = &ct->ets->training;
		int util_reduction;

		if (!eti->active)
			continue;

		util_reduction = (eti->hist_util[exec_slow_cl_idx]
				* eti->hist_uarch[exec_slow_cl_idx][UARCH_IPC])
				/ eti->hist_uarch[exec_fast_cl_idx][UARCH_IPC];
		util_reduction = eti->hist_util[exec_slow_cl_idx] - util_reduction;

		if (util_reduction < 0)
			continue;

		if (max_util_reduction < util_reduction) {
			if (max_util_reduction && sec_util_reduction < max_util_reduction)
				sec_util_reduction = max_util_reduction;

			max_util_reduction = util_reduction;
			best = ct;
		} else {
			if (sec_util_reduction < util_reduction)
				sec_util_reduction = util_reduction;
		}

		trace_cleave_setup_execution_clv_task(ct, util_reduction);
	}

	/* check redcution delta ratio */
	if (max_util_reduction) {
		int delta = max_util_reduction - sec_util_reduction;
		int delta_ratio = (delta * 100) / max_util_reduction;

		if (delta_ratio < REDUCTION_RATIO_THR)
			best = NULL;
	}

	if (!best) {
		set_execution_task_state(NULL, -1, reserved_cpus, EP_EXECUTION_FAILED);
		return;
	}

	set_execution_task_state(best, fastest, reserved_cpus, EP_EXECUTION_SUCCESS);
}

static void check_execution_clv_task_validation(struct cpumask *reserved_cpus)
{
	/* find best efficiency task on fastest cpu */
	struct cleave_task *execution_ct = NULL, *best = NULL;
	int fastest = cpumask_last(cpu_fastest_mask());
	int max_util_reduction = INT_MIN;
	int idx;

	for (idx = 0; idx < clv.nr_tasks; idx++) {
		struct cleave_task *ct = &clv.clv_tasks[idx];
		struct egg_training_info *eti;
		int util_reduction, util;

		if (unlikely(!ct->ets))
			continue;

		eti = &ct->ets->training;
		if (!eti->active)
			continue;

		util = ct->util;

		/* check execution task reduction */
		if (egg_get_execution_task() == ct->ets->task) {
			util_reduction = (util * eti->hist_uarch[exec_fast_cl_idx][UARCH_IPC])
						/ eti->hist_uarch[exec_slow_cl_idx][UARCH_IPC];
			util_reduction = util_reduction - util;
			execution_ct = ct;
		} else {
			util_reduction = (util * eti->hist_uarch[exec_slow_cl_idx][UARCH_IPC])
						/ eti->hist_uarch[exec_fast_cl_idx][UARCH_IPC];
			util_reduction = util - util_reduction;
		}

		if (max_util_reduction < util_reduction) {
			max_util_reduction = util_reduction;
			best = ct;
		}

		trace_cleave_setup_execution_clv_task(ct, util_reduction);
	}

	/* noti execution state is not invalid */
	if (!execution_ct) {
		set_execution_task_state(NULL, -1, reserved_cpus, EP_EXECUTION_FAILED);
		return;
	}

	if (best == execution_ct) {
		set_execution_task_state(execution_ct, fastest, reserved_cpus, EP_EXECUTION_SUCCESS);
	} else {
		set_execution_task_state(execution_ct, fastest, reserved_cpus, EP_EXECUTION_INVALID);
	}
}

static void setup_execution_clv_task(struct cpumask *reserved_cpus)
{
	if (egg_get_execution_state() == EP_EXECUTION_TRY)
		setup_execution_clv_task_try(reserved_cpus);
	else
		check_execution_clv_task_validation(reserved_cpus);
}

static int set_boosted_task_state(struct cleave_task *ct, struct cpumask *reserved_cpus)
{
	int boost_cpu = get_boost_cpu();

	if (boost_cpu != ct->ets->reserved_cpu)
		return -1;

	if (unlikely(!cpumask_test_cpu(boost_cpu, cpu_active_mask)))
		return -1;

	if (!cpumask_test_cpu(boost_cpu, &ct->ets->cpus_ptr))
		return -1;

	if (cpumask_test_cpu(boost_cpu, reserved_cpus))
		return -1;

	cpumask_set_cpu(boost_cpu, reserved_cpus);
	cpumask_set_cpu(boost_cpu, &ct->available_cpus);
	cpumask_copy(&ct->available_cpus_org, &ct->available_cpus);

	ct->reason = CLEAVE_BIG_BOOST;

	return 0;
}

static void setup_normal_clv_task(struct cpumask *reserved_cpus)
{
	struct cpumask fastmask[MAX_NR_CLEAVE];
	struct cpumask slowmask[MAX_NR_CLEAVE];
	struct cpumask policy_cpus;
	int idx = 0, cpu;

	cleave_get_policy_cpus(&policy_cpus);

	for (idx = 0; idx < clv.nr_tasks; idx++) {
		cpumask_clear(&fastmask[idx]);
		cpumask_clear(&slowmask[idx]);
	}

	/* find fastest available cpu */
	idx = 0;
	for (cpu = cpumask_last(cpu_fastest_mask()); cpu >= 0; cpu--) {
		if (idx >= clv.nr_tasks)
			break;

		if (!cpumask_test_cpu(cpu, ecs_max_cpus()))
			continue;

		if (egg_throttle_fastest_cpu() &&
			cpumask_test_cpu(cpu, cpu_fastest_mask()))
			continue;

		if (!cpu_active(cpu))
			continue;

		cpumask_copy(&fastmask[idx], cpu_clustergroup_mask(cpu));
		idx++;
	}

	/* find slowest available cpu */
	idx = clv.nr_tasks - 1;
	for_each_cpu(cpu, &policy_cpus) {
		if (idx < 0)
			break;

		if (!cpumask_test_cpu(cpu, ecs_max_cpus()))
			continue;

		if (egg_throttle_fastest_cpu() &&
			cpumask_test_cpu(cpu, cpu_fastest_mask()))
			continue;

		if (!cpu_active(cpu))
			continue;

		cpumask_copy(&slowmask[idx], cpu_clustergroup_mask(cpu));
		idx--;
	}

	/*
	 * setup available_cpus from fastmask and slowmask
	 * fastmask is maximum cpu bound and slowmask is minimum cpu bound
	 */
	for (idx = 0; idx < clv.nr_tasks; idx++) {
		int fast_cpu, slow_cpu;
		struct cleave_task *ct = &clv.clv_tasks[idx];

		if (unlikely(!ct->ets))
			continue;

		if (ct->ets->pid == get_boosted_task()) {
			if (!set_boosted_task_state(ct, reserved_cpus))
				continue;
		}

		/* skip selecting available_cpus */
		if (ct->ets->training.active && !cpumask_empty(&ct->available_cpus))
			continue;

		fast_cpu = cpumask_last(&fastmask[idx]);
		slow_cpu = cpumask_first(&slowmask[idx]);
		if (unlikely(invalid_cpu(fast_cpu) || invalid_cpu(slow_cpu))) {
			struct cpumask active_cpus;

			cpumask_and(&active_cpus, cpu_active_mask, ecs_max_cpus());
			slow_cpu = 0;
			fast_cpu = cpumask_last(&active_cpus);
		}

		for (cpu = slow_cpu; cpu <= fast_cpu; cpu++) {
			int util_with = ct->util + clv.env[cpu].avg_util_wo;

			if (cpumask_test_cpu(cpu, reserved_cpus))
				continue;

			if (unlikely(!cpumask_test_cpu(cpu, cpu_active_mask)))
				continue;

			if (!cpumask_test_cpu(cpu, &ct->ets->cpus_ptr))
				continue;

			if (cpu_overutilized_with_task(cpu, util_with))
				continue;

			cpumask_set_cpu(cpu, &ct->available_cpus);
		}

		/*
		 * if there is no available cpus by overutil,
		 * use best perf cpu
		 */
		if (cpumask_empty(&ct->available_cpus)) {
			set_best_perf_cpu(ct, reserved_cpus);

			if (egg_get_sub_mode() == EP_NORMAL)
				ct->misfit = true;
		}

		/* correct available_cpus of heavier tasks than this */
		if (cpumask_weight(&ct->available_cpus) == 1)
			correct_available_cpus(&ct->available_cpus, idx);

		ct->reason = CLEAVE_NORMAL;
		cpumask_copy(&ct->available_cpus_org, &ct->available_cpus);
	}
}

static void setup_available_cpus(int sub_mode)
{
	struct cpumask reserved_cpus;
	int idx;

	cpumask_clear(&reserved_cpus);

	/* setup training task first */
	if (egg_get_op_mode() == OP_EXTREME_PERFORMANCE) {
		if (sub_mode == EP_TRAINING)
			setup_training_clv_task(&reserved_cpus);
		else if (sub_mode == EP_EXECUTION)
			setup_execution_clv_task(&reserved_cpus);
	}

	/* setup normal task */
	setup_normal_clv_task(&reserved_cpus);

	if (trace_cleave_setup_available_cpus_env_enabled()) {
		for (idx = 0; idx < clv.nr_tasks; idx++) {
			struct cleave_task *ct = &clv.clv_tasks[idx];
			trace_cleave_setup_available_cpus_env(ct,
				cleave_reason_name[ct->reason]);
		}
	}
}

/*
 * set up base util and available mask for cleave tasks
 * base util is avg cpu util for a period (100msec) without reserved task util
 */
static void cleave_setup_base_env(int sub_mode)
{
	/* setup base util for cpu and task */
	setup_base_util();

	/*
	 * Find available cpus
	 * To find fastest available cpu for cleave task list
	 * locate cleave_task list from fastest cpus to slowest cpus in cleave policy cpus
	 * To find slowest available cpu for cleave task list
	 * locate cleave_task list from slowest cpus to fastest cpus in cleave policy cpus
	 * And merge fastest available cpu and slowest available cpu.
	 * these are final available cpus for cleave task
	 */
	setup_available_cpus(sub_mode);
}

static int default_get_next_cap(struct cpumask *cpus)
{
	unsigned long next_cap = 0;
	int cpu;

	for_each_cpu_and(cpu, cpus, cpu_active_mask) {
		unsigned long util;

		util = clv.cs[cpu].util;

		/*
		 * The cpu in the coregroup has same capacity and the
		 * capacity depends on the cpu with biggest utilization.
		 * Find biggest utilization in the coregroup and use it
		 * as max floor to know what capacity the cpu will have.
		 */
		if (util > next_cap)
			next_cap = util;
	}

	return min(next_cap, ems_get_actual_cpu_capacity(cpumask_first(cpus)));
}

static unsigned int compute_system_energy(struct cleave_snapshot *cs,
					struct energy_backup *backup)
{
	struct energy_state states[VENDOR_NR_CPUS] = { 0, };
	struct energy_domain *energy_domains = get_energy_domains();
	int cluster, nr_energy_domains = get_nr_energy_domains();

	for (cluster = 0; cluster < nr_energy_domains; cluster++) {
		struct cpumask *cpus = &energy_domains[cluster].cpus;
		int capacity = default_get_next_cap(cpus);

		et_fill_energy_state_cleave(cs, cpus, states, capacity);
	}

	return et_compute_system_energy(states, -1, backup);
}

static int find_min_util_cpu(struct cleave_task *ct, struct cpumask *candidates_cpus)
{
	int cpu, util, min_cpu = -1, min_util = INT_MAX;

	for_each_cpu(cpu, candidates_cpus) {
		util = clv.env[cpu].avg_util_wo + ct->util;

		/* give prev advantage when find min util cpu in a cluster */
		if (cpu == ct->ets->reserved_cpu)
			util -= (ct->util >> 4);

		if (util < min_util) {
			min_util = util;
			min_cpu = cpu;
		}
	}
	return min_cpu;
}

static void copy_cpu_combination(struct cleave_snapshot *cs)
{
	int idx;

	/* copy candidates */
	for (idx = 0; idx < clv.nr_tasks; idx++) {
		struct cleave_task *ct = &clv.clv_tasks[idx];

		ct->cpu = ct->visiting_cpu;
	}
}

static void visit_min_util_cpu(struct cleave_task *ct,
			struct cpumask *reserved_cpus)
{
	struct cpumask candidate, prev_candidate;
	struct cpumask policy_cpus;

	cleave_get_policy_cpus(&policy_cpus);

	cpumask_and(&candidate, &ct->ets->cpus_ptr, cpu_active_mask);
	cpumask_copy(&prev_candidate, &candidate);
	if (cpumask_weight(&candidate) == 1)
		goto only_one_option;
	else if (cpumask_empty(&candidate))
		return;

	cpumask_and(&candidate, &candidate, &policy_cpus);
	cpumask_copy(&prev_candidate, &candidate);
	if (cpumask_weight(&candidate) == 1)
		goto only_one_option;
	else if (cpumask_empty(&candidate))
		goto only_prev_option;

	cpumask_andnot(&candidate, &candidate, reserved_cpus);
	cpumask_copy(&prev_candidate, &candidate);
	if (cpumask_weight(&candidate) == 1)
		goto only_one_option;
	else if (cpumask_empty(&candidate))
		goto only_prev_option;

	ct->visiting_cpu = find_min_util_cpu(ct, &candidate);
	return;

only_one_option:
	ct->visiting_cpu = cpumask_last(&candidate);
	return;

only_prev_option:
	ct->visiting_cpu = cpumask_last(&candidate);
	return;
}

/* visit fastcpus first in available_cpus */
static void visit_available_cpus(struct cleave_task *ct,
			struct cpumask *reserved_cpus, bool force)
{
	int cpu;
	struct cpumask candidates_cpus;
	struct cpumask fastest_cpus;

	while (!cpumask_empty(&ct->available_cpus)) {
		cpu = cpumask_last(&ct->available_cpus);

		cpumask_copy(&fastest_cpus, cpu_clustergroup_mask(cpu));
		cpumask_and(&fastest_cpus, &fastest_cpus, &ct->available_cpus);
		cpumask_andnot(&ct->available_cpus, &ct->available_cpus, &fastest_cpus);
		cpumask_andnot(&candidates_cpus, &fastest_cpus, reserved_cpus);
		if (cpumask_empty(&candidates_cpus))
			continue;

		cpu = find_min_util_cpu(ct, &candidates_cpus);
		if (unlikely(invalid_cpu(cpu)))
			continue;

		ct->visiting_cpu = cpu;
		break;
	}

	/*
	 * if force flag is set, it means that there is no candidates,
	 * so, select min util cpu evenif it is overutilized
	 */
	if (invalid_cpu(ct->visiting_cpu) && force)
		visit_min_util_cpu(ct, reserved_cpus);
}

static int find_next_start_idx(void)
{
	int idx = -1;

	for (idx = clv.nr_tasks - 1; idx >= 0; idx--) {
		struct cleave_task *ct = &clv.clv_tasks[idx];
		if (!cpumask_empty(&ct->available_cpus))
			return idx;
	}
	return idx;
}

static void restore_available_cpus(int task_idx, bool clv_task_ordered)
{
	int idx, cpu;
	struct cpumask exclude_cpus;
	struct cleave_task *ct = &clv.clv_tasks[task_idx];

	if (!clv_task_ordered) {
		for (idx = task_idx + 1; idx < clv.nr_tasks; idx++) {
			ct = &clv.clv_tasks[idx];

			/* reset available cpus under the start task */
			cpumask_copy(&ct->available_cpus, &ct->available_cpus_org);
		}
		return;
	}

	cpumask_clear(&exclude_cpus);
	if (likely(!invalid_cpu(ct->visiting_cpu))) {
		for (cpu = cpumask_last(cpu_possible_mask); cpu >= 0; cpu--) {

			if (cpumask_test_cpu(cpu, &exclude_cpus))
				continue;

			if (cpumask_intersects(cpu_clustergroup_mask(cpu), &ct->available_cpus))
				break;

			cpumask_or(&exclude_cpus, &exclude_cpus, cpu_clustergroup_mask(cpu));
		}
	}

	for (idx = task_idx + 1; idx < clv.nr_tasks; idx++) {
		ct = &clv.clv_tasks[idx];

		/* reset available cpus under the start task */
		cpumask_copy(&ct->available_cpus, &ct->available_cpus_org);

		/* align fastcpus level with start_task and below tasks of it */
		cpumask_andnot(&ct->available_cpus, &ct->available_cpus, &exclude_cpus);
	}
}

static bool make_snapshot(int start_idx,
		struct energy_backup *backup, bool force)
{
	int idx, cpu;
	struct cpumask reserved_cpus;;

	/* clear snapshot for clv_task_idx */
	for_each_cpu(cpu, cpu_possible_mask)
		clv.cs[cpu].clv_task_idx = -1;

	/* visit available_cpus */
	cpumask_clear(&reserved_cpus);
	for (idx = 0; idx < clv.nr_tasks; idx++) {
		struct cleave_task *ct = &clv.clv_tasks[idx];

		/*
		 * task_idx is low than start_idx means that
		 * tasks under start_idx use previous vsisiting cpus and cpu.
		 */
		if (idx < start_idx) {
			if (unlikely(invalid_cpu(ct->visiting_cpu)))
				return false;

			clv.cs[ct->visiting_cpu].clv_task_idx = idx;
			cpumask_set_cpu(ct->visiting_cpu, &reserved_cpus);
			continue;
		} else {
			ct->visiting_cpu = -1;
		}

		/*
		 * tasks_idx is high than start_idx means that
		 * this turn will visit another available_cpus of tasks over start_idx
		 * it means task over start_idx will change reserved cpu.
		 */
		visit_available_cpus(ct, &reserved_cpus, force);
		if (unlikely(invalid_cpu(ct->visiting_cpu)))
			return false;

		clv.cs[ct->visiting_cpu].clv_task_idx = idx;
		cpumask_set_cpu(ct->visiting_cpu, &reserved_cpus);
	}

	/* setup util snapshot for energy computing */
	for_each_cpu(cpu, cpu_possible_mask) {
		struct cleave_snapshot *cs = &clv.cs[cpu];
		cs->util = clv.env[cpu].avg_util_wo;

		if (cs->clv_task_idx != -1) {
			struct cleave_task *ct = &clv.clv_tasks[cs->clv_task_idx];
			int task_util = ct->util;

			/* give prev advantage */
			if (ct->ets->reserved_cpu == ct->visiting_cpu)
				task_util = max(task_util - (task_util >> 5), (int) 0);

			cs->util += task_util;

			trace_cleave_snapshot(cpu, cs->util, ct->ets);
		} else {
			trace_cleave_snapshot(cpu, cs->util, NULL);
		}

		/* remove backup data */
		if (cs->util != cs->prev_util)
			backup[cpu].voltage = 0;
		cs->prev_util = cs->util;
	}

	return true;
}

#define MAX_CANDIDATES_CNT	10
static void cleave_tasks(void)
{
	struct energy_backup backup[VENDOR_NR_CPUS];
	int min_energy = INT_MAX, task_idx = 0, idx, cnt = 0;
	int sub_mode = egg_get_sub_mode();
	bool force = true, valid;
	bool clv_tasks_ordered;

	clv_tasks_ordered = sub_mode == EP_NORMAL;

	cleave_setup_base_env(sub_mode);

	while (task_idx != -1) {
		int energy;

		if (unlikely(cnt++ > MAX_CANDIDATES_CNT))
			break;

		/* make cpu util snapshot to energy computing about new cpu combination */
		valid = make_snapshot(task_idx, backup, force);
		/*
		 * This function determines the task idx
		 * to which reserved CPU will be switched next loop.
		 */
		task_idx = find_next_start_idx();

		/*
		 * task_idx is -1 means that there is no available_cpus
		 * it means that we search all candidate cpu combination.
		 * if task_idx isn't -1, it means that there are candidate cpu combination.
		 * so we need to restore available_cpus for exixt cpu combinatons.
		 */
		if (task_idx != -1)
			restore_available_cpus(task_idx, clv_tasks_ordered);

		/* if snapshot is not valid, remove from candidates */
		if (!valid)
			continue;

		/* compute energy for this cpu combination */
		energy = compute_system_energy(&clv.cs[0], backup);
		trace_cleave_compute_system_energy(cnt, &clv.clv_tasks[0], MAX_NR_CLEAVE, energy);
		if (energy > min_energy)
			continue;

		min_energy = energy;
		copy_cpu_combination(&clv.cs[0]);
		force = false;
	}

	if (trace_cleave_energy_cpu_enabled()) {
		for (idx = 0; idx < clv.nr_tasks; idx++)
			trace_cleave_energy_cpu(&clv.clv_tasks[idx]);
	}
}

static bool cleave_heavy_task(unsigned long task_util, unsigned long total_util)
{
	int threshold = (total_util * clv.heavy_task_ratio) / 100;

	return task_util >= threshold ? true : false;
}

static bool cleave_important_task(struct egg_task_struct *ets)
{
	return ets->important && (ets->hist_util > clv.important_threshold);
}

static bool task_need_cleave(struct egg_task_struct *ets, unsigned long total_util)
{
	if (cleave_heavy_task(ets->hist_util, total_util))
		return true;

	if (cleave_important_task(ets))
		return true;

	return false;
}

static void __cleave_update_task(struct egg_task_struct **major_tasks,
				int total_hist_util, bool update_boost_task)
{
	int i, boosted_task = get_boosted_task();

	for (i = 0; i < MAX_NR_MAJOR; i++) {
		struct egg_task_struct *ets = major_tasks[i];
		struct cleave_task *ct = &clv.clv_tasks[clv.nr_tasks];

		if (update_boost_task && boosted_task != -1 && boosted_task != ets->pid)
			continue;

		if (!update_boost_task && boosted_task == ets->pid)
			continue;

		if (ets->task && task_need_cleave(ets, total_hist_util)) {
			ct->ets = ets;
			ct->util = max(ets->hist_util, (unsigned long)1);
			clv.nr_tasks++;

			trace_cleave_heavy_task(ct, total_hist_util, clv.nr_tasks);
		} else {
			ct->ets = NULL;
			ct->util = 0;
		}

		ct->cpu = -1;
		ct->misfit = false;
		ct->reason = CLEAVE_ABNORMAL;
		ct->visiting_cpu = -1;
		cpumask_clear(&ct->available_cpus);
		cpumask_clear(&ct->available_cpus_org);

		if (clv.nr_tasks >= MAX_NR_CLEAVE)
			break;
	}
}

static void cleave_update_tasks(void)
{
	struct egg_task_struct *major_tasks[MAX_NR_MAJOR];
	unsigned long total_hist_util = 0;

	/* get top heavy tasks */
	egg_get_major_tasks(&major_tasks[0], &total_hist_util);

	clv.nr_tasks = 0;

	/* to re-order cleave task list for boosted task */
	if (get_boosted_task() != -1)
		__cleave_update_task(major_tasks, total_hist_util, true);

	__cleave_update_task(major_tasks, total_hist_util, false);
}

/********************************************************************************
 *				External APIs					*
 ********************************************************************************/
void cleave_update_boosted_mode(int boosted_task)
{
	unsigned long flags;
	int prev_boosted_task;

	raw_spin_lock_irqsave(&clv.lock, flags);

	prev_boosted_task = get_boosted_task();

	if (boosted_task != prev_boosted_task) {
		egg_update_reserved_cpu(boosted_task, prev_boosted_task);
		set_boosted_task(boosted_task);
	}

	raw_spin_unlock_irqrestore(&clv.lock, flags);


}
void cleave_update(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&clv.lock, flags);

	if (!egg_enabled())
		goto unlock;

	cleave_update_tasks();

	if (clv.nr_tasks)
		cleave_tasks();

	egg_update_reserved_cpus(clv.clv_tasks, clv.nr_tasks);

unlock:
	raw_spin_unlock_irqrestore(&clv.lock, flags);
}

void reset_cleave(void)
{
	unsigned long flags;
	int i;

	if (egg_enabled())
		return;

	raw_spin_lock_irqsave(&clv.lock, flags);
	for (i = 0; i < MAX_NR_CLEAVE; i++) {
		clv.clv_tasks[i].ets = NULL;
		clv.clv_tasks[i].util = 0;
		clv.clv_tasks[i].cpu = -1;
		clv.clv_tasks[i].misfit = false;
	}
	clv.nr_tasks = 0;
	raw_spin_unlock_irqrestore(&clv.lock, flags);
}

/********************************************************************************
 *				Sysfs node					*
 ********************************************************************************/
#define STR_LEN 7
static ssize_t policy_cpus_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int ret = 0;
	unsigned long flags;

	raw_spin_lock_irqsave(&clv.lock, flags);

	ret += sprintf(buf + ret, "policy : %*pbl fast : %*pbl\n",
			cpumask_pr_args(&clv.policy_cpus), cpumask_pr_args(&clv.fast_cpus));

	raw_spin_unlock_irqrestore(&clv.lock, flags);

	return ret;
}

static ssize_t policy_cpus_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct cpumask policy_cpus;
	char str[STR_LEN], re_str[STR_LEN];
	unsigned int cpumask_value;
	unsigned long flags;

	if (strlen(buf) >= STR_LEN)
		return -EINVAL;

	if (!sscanf(buf, "%5s", str))
		return -EINVAL;

	if (str[0] == '0' && toupper(str[1]) == 'X')
		/* Move str pointer to remove "0x" */
		cpumask_parse(str + 2, &policy_cpus);
	else {
		if (!sscanf(str, "%d", &cpumask_value))
			return -EINVAL;

		if (cpumask_value >= VENDOR_NR_CPUS)
			return -EINVAL;

		snprintf(re_str, STR_LEN - 1, "%x", cpumask_value);
		cpumask_parse(re_str, &policy_cpus);
	}

	raw_spin_lock_irqsave(&clv.lock, flags);

	cpumask_copy(&clv.policy_cpus, &policy_cpus);

	raw_spin_unlock_irqrestore(&clv.lock, flags);

	return count;

}
static struct kobj_attribute policy_cpus_attr =
__ATTR(policy_cpus, 0644, policy_cpus_show, policy_cpus_store);

static ssize_t heavy_task_ratio_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int ret = 0;
	unsigned long flags;

	raw_spin_lock_irqsave(&clv.lock, flags);

	ret += sprintf(buf + ret, "%d\n", (int)clv.heavy_task_ratio);

	raw_spin_unlock_irqrestore(&clv.lock, flags);

	return ret;
}

static ssize_t heavy_task_ratio_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;
	unsigned long flags;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	raw_spin_lock_irqsave(&clv.lock, flags);

	clv.heavy_task_ratio = val;

	raw_spin_unlock_irqrestore(&clv.lock, flags);

	return count;
}
static struct kobj_attribute heavy_task_ratio_attr =
__ATTR(heavy_task_ratio, 0644, heavy_task_ratio_show, heavy_task_ratio_store);

static ssize_t important_thres_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int ret = 0;
	unsigned long flags;

	raw_spin_lock_irqsave(&clv.lock, flags);

	ret += sprintf(buf + ret, "%d\n", clv.important_threshold);

	raw_spin_unlock_irqrestore(&clv.lock, flags);

	return ret;
}

static ssize_t important_thres_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;
	unsigned long flags;

	if (sscanf(buf, "%d", &val) != 1)
		return -EINVAL;

	raw_spin_lock_irqsave(&clv.lock, flags);

	clv.important_threshold = val;

	raw_spin_unlock_irqrestore(&clv.lock, flags);

	return count;
}
static struct kobj_attribute important_thres_attr =
__ATTR(important_thres, 0644, important_thres_show, important_thres_store);

static struct attribute *cleave_attrs[] = {
	&policy_cpus_attr.attr,
	&heavy_task_ratio_attr.attr,
	&important_thres_attr.attr,
	NULL,
};

static struct attribute_group cleave_attr_group = {
	.name		= "cleave",
	.attrs		= cleave_attrs,
};

/********************************************************************************
 *				Initialization					*
 ********************************************************************************/
static int cleave_dt_parse(void)
{
	struct device_node *dn;
	const char *buf;

	dn = of_find_node_by_path("/ems/egg/cleave");
	if (!dn) {
		pr_err("%s: failed to find device node\n", __func__);
		return -EINVAL;
	}

	if (!of_property_read_string(dn, "policy-cpus", &buf))
		ems_cpulist_parse(buf, &clv.policy_cpus);

	if (!of_property_read_string(dn, "fast-cpus", &buf))
		ems_cpulist_parse(buf, &clv.fast_cpus);

	if (of_property_read_u32(dn, "heavy-task-ratio", &clv.heavy_task_ratio))
		clv.heavy_task_ratio = DEFAULT_HEAVY_TASK_RATIO;

	return 0;
}

static void cleave_core_init(void)
{
	int i;

	cpumask_clear(&clv.policy_cpus);
	cpumask_clear(&clv.fast_cpus);

	for (i = 0; i < MAX_NR_CLEAVE; i++) {
		clv.clv_tasks[i].ets = NULL;
		clv.clv_tasks[i].util = 0;
		clv.clv_tasks[i].cpu = -1;
		clv.clv_tasks[i].misfit = false;
	}
	clv.nr_tasks = 0;
	clv.heavy_task_ratio = DEFAULT_HEAVY_TASK_RATIO;
	clv.important_threshold = DEFAULT_IMPORTANT_THRES;

	raw_spin_lock_init(&clv.lock);

	return;
}

int cleave_init(struct kobject *egg_kobj)
{
	cleave_core_init();

	if (cleave_dt_parse()) {
		pr_err("%s:failed to parse\n", __func__);
		return -EINVAL;
	}

	if (sysfs_create_group(egg_kobj, &cleave_attr_group)) {
		pr_err("%s:failed to create sysfs\n", __func__);
		return -ENOMEM;
	}

	pr_info("EMS: Cleave Initialized\n");

	return 0;
}
