/*
 * Batch Scheduling Class (SCHED_BT)
 */
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/profile.h>
#include <linux/interrupt.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/task_work.h>
#include <uapi/linux/sched/types.h>

#include <trace/events/sched.h>
#include "sched.h"
#include "fair.h"
#include "ht_isolate.h"

unsigned int bt_sysctl_sched_latency = 6000000ULL;
unsigned int bt_sysctl_sched_wakeup_granularity = 4000000ULL;
unsigned int bt_sysctl_sched_min_granularity = 3000000ULL;

unsigned int sysctl_cpu_qos_disable_kill;

const struct sched_class bt_sched_class;
unsigned int sysctl_cpu_qos;
unsigned int sysctl_idle_balance_bt_cost = 300000UL;
unsigned int sysctl_sched_bt_nr_migrate = 32;
unsigned int sysctl_rue_reserved0;
unsigned int sysctl_rue_reserved1;
unsigned int sysctl_rue_reserved2;
unsigned int sysctl_rue_reserved3;
unsigned int sysctl_rue_reserved4;
unsigned int sysctl_rue_reserved5;

unsigned int sysctl_sched_bt_load_balance_interval_min_ms = 64;
unsigned int sysctl_sched_bt_load_balance_interval_max_ms = 1024;
#ifdef CONFIG_BT_BANDWIDTH

unsigned int sysctl_sched_bt_percpu_suppress_percent = 100;
unsigned int sysctl_sched_bt_percpu_max_throttle_time_sec = 10; /* 10s */

DEFINE_PER_CPU(struct bt_bandwidth *, bt_bandwidth);
DEFINE_PER_CPU(struct bt_bandwidth_stat *, bt_bandwidth_stat);

#endif


int sched_bt_percpu_suppress_percent_handler(struct ctl_table *table,
		int write, void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	return -EINVAL;
}

void defer_to_kick_bt_task(struct task_struct *p)
{
}
void do_sched_bt_slack_timer(struct cfs_bandwidth *cfs_b)
{

}
int do_sched_bt_period_timer_share(struct cfs_bandwidth *cfs_b, int overrun, unsigned long flags)
{
	return 0;
}
#ifdef CONFIG_SCHED_DEBUG
void print_bt_stats(struct seq_file *m, int cpu)
{
}
#endif

int sched_bt_disable_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	return -EINVAL;
}

#ifdef CONFIG_BT_GROUP_SCHED
#ifdef CONFIG_BT_SHARE_CFS_BANDWIDTH
void unthrottle_bt_rq_share(struct bt_rq *bt_rq)
{
}
extern unsigned int sysctl_sched_cfs_bandwidth_slice;

#ifdef CONFIG_BT_BANDWIDTH
void __refill_cfs_bandwidth_runtime_bt(struct cfs_bandwidth *cfs_b)
{

}

void init_bt_bandwidth(struct bt_bandwidth *bt_b, int cpu, u64 period, u64 runtime, u64 runtime_dynamic)
{
}

void init_bt_bandwidth_stat(struct bt_bandwidth_stat *bt_bstat, int cpu)
{
}

int sched_bt_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	return -EINVAL;
}

#endif


#endif /* CONFIG_BT_GROUP_SCHED */


#endif

static void update_curr_cb_bt(struct rq *rq)
{
}

static bool dequeue_task_bt(struct rq *rq, struct task_struct *p, int flags)
{
	return true;
}

static void enqueue_task_bt(struct rq *rq, struct task_struct *p, int flags)
{
}

#ifdef CONFIG_SMP

static int
select_task_rq_bt(struct task_struct *p, int task_cpu, int flag)
{
	return 0;
}

#ifdef CONFIG_BT_GROUP_SCHED
static void
migrate_task_rq_bt(struct task_struct *p, int new_cpu)
{
}
#endif

static void task_dead_bt(struct task_struct *p)
{
}
#endif

static void check_preempt_wakeup_bt(struct rq *rq, struct task_struct *p, int wake_flags)
{
}

static struct task_struct *pick_task_bt(struct rq *rq)
{
	return NULL;
}

struct task_struct *pick_next_task_bt(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	return NULL;
}

struct task_struct *__pick_next_task_bt(struct rq *rq, struct task_struct *prev)
{
	return pick_next_task_bt(rq, prev, NULL);
}

static void put_prev_task_bt(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
}

void post_init_bt_entity_util_avg(struct sched_bt_entity *se)
{
}
void init_bt_entity_runnable_average(struct sched_bt_entity *se)
{

}
#ifdef CONFIG_SMP

static void rq_online_bt(struct rq *rq)
{
}

static void rq_offline_bt(struct rq *rq)
{
}
#endif /* CONFIG_SMP */

static void task_tick_bt(struct rq *rq, struct task_struct *curr, int queued)
{
}

static void task_fork_bt(struct task_struct *p)
{
}

static void
prio_changed_bt(struct rq *rq, struct task_struct *p, int oldprio)
{
}

static void switched_from_bt(struct rq *rq, struct task_struct *p)
{
}

static void switched_to_bt(struct rq *rq, struct task_struct *p)
{
}

static int
balance_bt(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	return 0;
}

static void set_next_task_bt(struct rq *rq, struct task_struct *p, bool first)
{
}

void init_bt_rq(struct bt_rq *bt_rq)
{
}

#ifdef CONFIG_BT_GROUP_SCHED
static void task_change_group_bt(struct task_struct *p)
{
}

void init_tg_bt_entry(struct task_group *tg, struct bt_rq *bt_rq,
		struct sched_bt_entity *se, int cpu,
		struct sched_bt_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	bt_rq->tg = tg;
	bt_rq->rq = rq;
	tg->bt_rq[cpu] = bt_rq;
	tg->bt[cpu] = se;

	/* se could be NULL for root_task_group */
	if (!se)
		return;

	if (!parent) {
		se->bt_rq = &rq->bt;
		se->depth = 0;
	} else {
		se->bt_rq = parent->bt_my_q;
		se->depth = parent->depth + 1;
	}

	se->bt_my_q = bt_rq;
	se->parent = parent;
}


void free_bt_sched_group(struct task_group *tg)
{
	int i;

	for_each_possible_cpu(i) {
		if (tg->bt_rq)
			kfree(tg->bt_rq[i]);
		if (tg->bt)
			kfree(tg->bt[i]);
	}

	kfree(tg->bt_rq);
	kfree(tg->bt);
}

int alloc_bt_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct bt_rq *bt_rq;
	struct sched_bt_entity *se;
	int i;

	tg->bt_rq = kzalloc(sizeof(bt_rq) * nr_cpu_ids, GFP_KERNEL);
	if (!tg->bt_rq)
		goto err;
	tg->bt = kzalloc(sizeof(se) * nr_cpu_ids, GFP_KERNEL);
	if (!tg->bt)
		goto err;

	tg->bt_shares = NICE_0_LOAD;

	for_each_possible_cpu(i) {
		bt_rq = kzalloc_node(sizeof(struct bt_rq),
				GFP_KERNEL, cpu_to_node(i));
		if (!bt_rq)
			goto err;

		se = kzalloc_node(sizeof(struct sched_bt_entity),
				GFP_KERNEL, cpu_to_node(i));
		if (!se)
			goto err_free_rq;
		init_tg_bt_entry(tg, bt_rq, se, i, parent->bt[i]);
	}

	return 1;

err_free_rq:
	kfree(bt_rq);
err:
	return 0;
}

void unregister_bt_sched_group(struct task_group *tg)
{
}

static DEFINE_MUTEX(bt_shares_mutex);

int sched_group_set_bt_shares(struct task_group *tg, unsigned long shares)
{
	return 0;
}
#else /* !CONFIG_BT_GROUP_SCHED */

void free_bt_sched_group(struct task_group *tg) { }

int alloc_bt_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}

void unregister_bt_sched_group(struct task_group *tg) { }

#endif /* CONFIG_BT_GROUP_SCHED */

static unsigned int get_rr_interval_bt(struct rq *rq, struct task_struct *task)
{
	return 0;
}

static void yield_task_bt(struct rq *rq)
{
}

void trigger_load_balance_bt(struct rq *rq)
{
}

DEFINE_SCHED_CLASS(bt) = {

	.enqueue_task		= enqueue_task_bt,
	.dequeue_task		= dequeue_task_bt,
	.yield_task		= yield_task_bt,

	.wakeup_preempt		= check_preempt_wakeup_bt,
	.pick_next_task		= __pick_next_task_bt,
	.put_prev_task		= put_prev_task_bt,
	.set_next_task		= set_next_task_bt,

#ifdef CONFIG_SMP
	.balance		= balance_bt,
	.pick_task		= pick_task_bt,
	.select_task_rq		= select_task_rq_bt,
#ifdef CONFIG_BT_GROUP_SCHED
	.migrate_task_rq	= migrate_task_rq_bt,
#endif
	.rq_online		= rq_online_bt,
	.rq_offline		= rq_offline_bt,
	.task_dead              = task_dead_bt,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif
	.task_tick		= task_tick_bt,
	.task_fork		= task_fork_bt,

	.prio_changed		= prio_changed_bt,
	.switched_from		= switched_from_bt,
	.switched_to		= switched_to_bt,

	.get_rr_interval	= get_rr_interval_bt,
	.update_curr		= update_curr_cb_bt,
#ifdef CONFIG_BT_GROUP_SCHED
	.task_change_group      = task_change_group_bt,
#endif
};

int bt_ignore_cpubind_handler(struct ctl_table *table, int write,
					  void *buffer, size_t *lenp,
					  loff_t *ppos)
{
	return proc_dointvec_minmax(table, write, buffer, lenp, ppos);
}

void sched_bt_killall(void)
{

}

void init_offline_cpu_control(void)
{
}
__init void init_sched_bt_class(void)
{

}
