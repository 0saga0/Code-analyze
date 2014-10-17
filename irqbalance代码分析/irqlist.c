#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>

#include "types.h"
#include "irqbalance.h"

struct load_balance_info {
	unsigned long long int total_load;  //ϵͳ���жϸ���
	unsigned long long avg_load;	//ϵͳƽ���жϸ���
	unsigned long long min_load;	//ϵͳ�е��жϸ�����Сֵ
	unsigned long long adjustment_load; //��¼Ǩ���жϵ�����ܸ���
	int load_sources;	//�����������
	unsigned long long int deviations;	//��ֵ
	long double std_deviation;	
	unsigned int num_over;	//����ƽ�����ص��������
	unsigned int num_under;	//����ƽ�����ص��������
	unsigned int num_powersave;	//ϵͳ�н���ģʽ���������
	struct topo_obj *powersave;
};

/*���¸��ؾ���ṹ�е���С���أ��ܸ����Լ�����Դ��Ŀ*/
static void gather_load_stats(struct topo_obj *obj, void *data)
{
	struct load_balance_info *info = data;

	if (info->min_load == 0 || obj->load < info->min_load)
		info->min_load = obj->load;
	info->total_load += obj->load;
	info->load_sources += 1;
}

/*���¸��ؾ���ṹ�еĸ��ز�ֵ��Ϣ*/
static void compute_deviations(struct topo_obj *obj, void *data)
{
	struct load_balance_info *info = data;
	unsigned long long int deviation;

	deviation = (obj->load > info->avg_load) ?
		obj->load - info->avg_load :
		info->avg_load - obj->load;

	info->deviations += (deviation * deviation);
}

/*�ж�����ж��Ƿ����Ǩ��Ҫ��������������ԭ�����ж�������ɾ��*/
static void move_candidate_irqs(struct irq_info *info, void *data)
{
	struct load_balance_info *lb_info = data;

	/* never move an irq that has an afinity hint when 
 	 * hint_policy is HINT_POLICY_EXACT 
 	 */
	if (info->hint_policy == HINT_POLICY_EXACT)
		if (!cpus_empty(info->affinity_hint))
			return;

	/* Don't rebalance irqs that don't want it */
	if (info->level == BALANCE_NONE)
		return;

	/*�������ж����󶨵�CPUֻ����һ���жϣ���������жϴ����ĸ��ض��أ�����Ǩ�Ƹ��ж� */
	if (g_list_length(info->assigned_obj->interrupts) <= 1)
		return;

	/* IRQs with a load of 1 have most likely not had any interrupts and
	 * aren't worth migrating
	 */
	if (info->load <= 1)
		return;

	/*Ǩ���ж�Ҫ��֤�жϸ��ز��ܳ������ڸ�������С���ز�ֵ��һ�� */
	if ((lb_info->adjustment_load - info->load) > (lb_info->min_load + info->load)) {
		lb_info->adjustment_load -= info->load;
		lb_info->min_load += info->load;
	} else
		return;

	log(TO_CONSOLE, LOG_INFO, "Selecting irq %d for rebalancing\n", info->irq);

	/*���жϴ�ԭ�����ж�������ɾ��������rebalance_irq_list������*/
	migrate_irq(&info->assigned_obj->interrupts, &rebalance_irq_list, info);

	info->assigned_obj = NULL;
}

/*�жϸ���ĸ��������������ع��أ������ж������Ǩ�ƣ�ֱ�����ز�����Ҫ�����ж�Ǩ��*/
static void migrate_overloaded_irqs(struct topo_obj *obj, void *data)
{
	struct load_balance_info *info = data;

	/*����������˽���ģʽ�����¸��ؾ���ṹ�еĽ����������*/
	if (obj->powersave_mode)
		info->num_powersave++;

	/*�������ĸ�������С��ƽ�����أ����¸��ؾ���ṹ�еĵ͸������������
	�������ĸ������Դ���ƽ�����أ����¸��ؾ���ṹ�еĸ߸����������*/
	if ((obj->load + info->std_deviation) <= info->avg_load) {
		info->num_under++;
		if (power_thresh != ULONG_MAX && !info->powersave)
			if (!obj->powersave_mode)
				info->powersave = obj;
	} else if ((obj->load - info->std_deviation) >=info->avg_load) {
		info->num_over++;
	}

	if ((obj->load > info->min_load) &&
	    (g_list_length(obj->interrupts) > 1)) {
		/* ��������ж������ո��ش�С�������� */
		sort_irq_list(&obj->interrupts);

		/*����������ж�������������жϴ��������Ƴ���ֱ������ĸ����Ѿ��޷�����Ǩ�Ƶ����� */
		info->adjustment_load = obj->load;
		for_each_irq(obj->interrupts, move_candidate_irqs, info);
	}
}

/*���������жϴ�һ������Ǩ�Ƴ�ȥ*/
static void force_irq_migration(struct irq_info *info, void *data __attribute__((unused)))
{
	migrate_irq(&info->assigned_obj->interrupts, &rebalance_irq_list, info);
	info->assigned_obj = NULL;
}

/*ȡ��һ����Ľ���ģʽ*/
static void clear_powersave_mode(struct topo_obj *obj, void *data __attribute__((unused)))
{
	obj->powersave_mode = 0;
}

/*�ҵ�һ���������и����ص���Ȼ��Ǩ�����жϣ�ֱ�������Ѿ�������ҪǨ���ж�*/
static void find_overloaded_objs(GList *name, struct load_balance_info *info) 
{
	/*�Ƚ����ؾ���ṹ��0���ٸ����������и��������������㸺�ؾ������ݽṹ�и���������ֵ*/
	memset(info, 0, sizeof(struct load_balance_info));
	for_each_object(name, gather_load_stats, info);
	info->load_sources = (info->load_sources == 0) ? 1 : (info->load_sources);
	info->avg_load = info->total_load / info->load_sources;
	for_each_object(name, compute_deviations, info);
	/* Don't divide by zero if there is a single load source */
	if (info->load_sources == 1)
		info->std_deviation = 0;
	else {
		info->std_deviation = (long double)(info->deviations / (info->load_sources - 1));
		info->std_deviation = sqrt(info->std_deviation);
	}
	/*���������򣬸������غ͸��ؾ������ݽṹ��Ǩ�����ж�*/
	for_each_object(name, migrate_overloaded_irqs, info);
}

/*�ӵײ��CPU��ʼ���и��ص�Ǩ�ƣ��������ε�cache��package�򣬽ڵ��򣬸�������CPU���˽ṹ���жϸ��ؾ���״̬*/
void update_migration_status(void)
{
	struct load_balance_info info;
	find_overloaded_objs(cpus, &info);
	if (power_thresh != ULONG_MAX && cycle_count > 5) {
		if (!info.num_over && (info.num_under >= power_thresh) && info.powersave) {
			log(TO_ALL, LOG_INFO, "cpu %d entering powersave mode\n", info.powersave->number);
			info.powersave->powersave_mode = 1;
			if (g_list_length(info.powersave->interrupts) > 0)
				for_each_irq(info.powersave->interrupts, force_irq_migration, NULL);
		} else if ((info.num_over) && (info.num_powersave)) {
			log(TO_ALL, LOG_INFO, "Load average increasing, re-enabling all cpus for irq balancing\n");
			for_each_object(cpus, clear_powersave_mode, NULL);
		}
	}
	find_overloaded_objs(cache_domains, &info);
	find_overloaded_objs(packages, &info);
	find_overloaded_objs(numa_nodes, &info);
}

static void dump_workload(struct irq_info *info, void *unused __attribute__((unused)))
{
	log(TO_CONSOLE, LOG_INFO, "Interrupt %i node_num %d (class %s) has workload %lu \n",
	    info->irq, irq_numa_node(info)->number, classes[info->class], (unsigned long)info->load);
}

void dump_workloads(void)
{
	for_each_irq(NULL, dump_workload, NULL);
}

