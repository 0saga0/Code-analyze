#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include "types.h"
#include "irqbalance.h"


GList *rebalance_irq_list;

/*����Ǩ��Ŀ�����ݽṹ*/
struct obj_placement {
		struct topo_obj *best;	//�������ٵ���
		struct topo_obj *least_irqs;	//�������ٲ����ж���ĿҲ���ٵ���
		uint64_t best_cost;	 //�������ٵ���ĸ���ֵ
		struct irq_info *info;
};

/*�ж���d�Ƿ��ʺ���ΪǨ���жϵ�����Ŀ��ѡ��*/
static void find_best_object(struct topo_obj *d, void *data)
{
	struct obj_placement *best = (struct obj_placement *)data;
	uint64_t newload;
	cpumask_t subset;

	/*���������õ�NUMAͷ��� */
	if (numa_avail && (d->obj_type == OBJ_TYPE_NODE) && (d->number == -1))
		return;

	/*�������޿���CPU��NUMA�ڵ� */
	if ((d->obj_type == OBJ_TYPE_NODE) &&
	    (!cpus_intersects(d->mask, unbanned_cpus)))
		return;

	/*��֤���������жϵ��׺Ͷ�����Ҫ�� */
	if (best->info->hint_policy == HINT_POLICY_SUBSET) {
		if (!cpus_empty(best->info->affinity_hint)) {
			cpus_and(subset, best->info->affinity_hint, d->mask);
			if (cpus_empty(subset))
				return;
		}
	}

	if (d->powersave_mode)
		return;

	newload = d->load;

	/*�����d�и���ֵС�ڼ�¼������ֵ��������Ϊ������*/
	if (newload < best->best_cost) {
		best->best = d;
		best->best_cost = newload;
		best->least_irqs = NULL;
	}

	/*���g��ĸ������¼����ֵһ�������ж���Ŀ���٣�������*/
	if (newload == best->best_cost) {
		if (g_list_length(d->interrupts) < g_list_length(best->best->interrupts))
			best->least_irqs = d;
	}
}

/*Ϊ��Ҫ��Ǩ��Ŀ����ж���������*/
static void find_best_object_for_irq(struct irq_info *info, void *data)
{
	struct obj_placement place;
	struct topo_obj *d = data;
	struct topo_obj *asign;

	if (!info->moved)
		return;
	/*�ӽڵ���ʼ��*/
	switch (d->obj_type) {
	case OBJ_TYPE_NODE:
		if (info->level == BALANCE_NONE)
			return;
		break;

	case OBJ_TYPE_PACKAGE:
		if (info->level == BALANCE_PACKAGE)
			return;
		break;

	case OBJ_TYPE_CACHE:
		if (info->level == BALANCE_CACHE)
			return;
		break;

	case OBJ_TYPE_CPU:
		if (info->level == BALANCE_CORE)
			return;
		break;
	}

	/*�ȳ�ʼ������ѡ�����ݽṹ*/
	place.info = info;
	place.best = NULL;
	place.least_irqs = NULL;
	place.best_cost = ULLONG_MAX;

	/*���������ڵ�����������ҵ����ŵ��������Ϣ���浽����ѡ�����ݽṹ��*/
	for_each_object(d->children, find_best_object, &place);

	/*����и������ٲ����ж���ĿҲ���ٵģ�������ΪǨ��Ŀ�꣬����ѡ�������ٵ���ΪǨ��Ŀ��*/
	asign = place.least_irqs ? place.least_irqs : place.best;

	/*���ж�Ǩ�Ƶ�Ŀ�����ϣ�������ĸ���*/
	if (asign) {
		migrate_irq(&d->interrupts, &asign->interrupts, info);
		info->assigned_obj = asign;
		asign->load += info->load;
	}
}

/*Ϊ��d���ж��ҵ�����ʵ�����*/
static void place_irq_in_object(struct topo_obj *d, void *data __attribute__((unused)))
{
	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, find_best_object_for_irq, d);
}

/*���жϲ�����ʵ�����*/
static void place_irq_in_node(struct irq_info *info, void *data __attribute__((unused)))
{
	struct obj_placement place;
	struct topo_obj *asign;

	if ((info->level == BALANCE_NONE) && cpus_empty(banned_cpus))
		return;
	/*����ýڵ��й����Ľڵ������ȿ�����ڵ����Ƿ��Ƿ�Ϸ�*/
	if (irq_numa_node(info)->number != -1) 
	{
		/*����ýڵ��򲻿���ʱ����תȥѰ�����ŵ�������ýڵ�����ã���ֱ�ӽ��жϲ���ڵ�����ж�������*/
		if (!cpus_intersects(irq_numa_node(info)->mask, unbanned_cpus))
			goto find_placement;

		migrate_irq(&rebalance_irq_list, &irq_numa_node(info)->interrupts, info);
		info->assigned_obj = irq_numa_node(info);
		irq_numa_node(info)->load += info->load + 1;
		return;
	}
	
/*ͨ���������ݽṹ�Լ�����������ķ�ʽ��Ѱ�����ŵ���*/
find_placement:
	place.best_cost = ULLONG_MAX;
	place.best = NULL;
	place.least_irqs = NULL;
	place.info = info;

	for_each_object(numa_nodes, find_best_object, &place);

	asign = place.least_irqs ? place.least_irqs : place.best;

	if (asign) {
		migrate_irq(&rebalance_irq_list, &asign->interrupts, info);
		info->assigned_obj = asign;
		asign->load += info->load;
	}
}

/*����жϹ������Ƿ���ȷ*/
static void validate_irq(struct irq_info *info, void *data)
{
	if (info->assigned_obj != data)
		log(TO_CONSOLE, LOG_INFO, "object validation error: irq %d is wrong, points to %p, should be %p\n",
			info->irq, info->assigned_obj, data);
}

/*�����������е��ж��Ƿ�ȷʵ���ڸ���*/
static void validate_object(struct topo_obj *d, void *data __attribute__((unused)))
{
	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, validate_irq, d);
}

/*����������˽ṹ���жϹ����Ƿ���ȷ*/
static void validate_object_tree_placement(void)
{
	for_each_object(packages, validate_object, NULL);	
	for_each_object(cache_domains, validate_object, NULL);
	for_each_object(cpus, validate_object, NULL);
}

/*ȫ�ֵ��жϵ�������Ǩ�Ƶ��жϲ������У������������˽ṹ�Ż��жϵķ���*/
void calculate_placement(void)
{
	sort_irq_list(&rebalance_irq_list);
	if (g_list_length(rebalance_irq_list) > 0) {
		for_each_irq(rebalance_irq_list, place_irq_in_node, NULL);
		for_each_object(numa_nodes, place_irq_in_object, NULL);
		for_each_object(packages, place_irq_in_object, NULL);
		for_each_object(cache_domains, place_irq_in_object, NULL);
	}
	if (debug_mode)
		validate_object_tree_placement();
}
