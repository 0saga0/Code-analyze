#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include "irqbalance.h"

/*���irq_info���ж��׺Ͷ�������Ϣ���ж�applied_mask�Ƿ�����һ��*/
static int check_affinity(struct irq_info *info, cpumask_t applied_mask)
{
	cpumask_t current_mask;
	char buf[PATH_MAX];
	char *line = NULL;
	size_t size = 0;
	FILE *file;
    /*buf������жϵ��׺Ͷ�Ŀ¼·��*/
	sprintf(buf, "/proc/irq/%i/smp_affinity", info->irq);
	file = fopen(buf, "r");
	if (!file)
		return 1;
	/*getline���ڶ�ȡһ���ַ�ֱ�����з������ļ�β���ɹ�ʱ���ض�ȡ���ֽ�����line�����ȡ���ַ���*/
	if (getline(&line, &size, file)==0) {
		free(line);
		fclose(file);
		return 1;
	}
	/*���õ����׺Ͷ���Ϣת����λͼ��Ϣ*/
	cpumask_parse_user(line, strlen(line), current_mask);
	fclose(file);
	free(line);
	/*�ж�applied_mask�Ƿ�����жϵ��׺Ͷ�����һ��*/
	return cpus_equal(applied_mask, current_mask);
}

/*�Խ�����Ǩ�Ƶ��ж����������׺Ͷ�ӳ��*/
static void activate_mapping(struct irq_info *info, void *data __attribute__((unused)))
{
	char buf[PATH_MAX];
	FILE *file;
	cpumask_t applied_mask;
	int valid_mask = 0;

	/*ֻ�н�����Ǩ�Ʋ���û�м���ӳ����ж���Ҫ����ӳ�伤��*/
	if (!info->moved)
		return;
	
	/*����û����õĲ�����HINT_POLICY_EXACT����ô�����/proc/irq/N/affinity_hint�����׺Ͷ�,
�����HINT_POLICY_SUBSET, ��ô�����/proc/irq/N/affinity_hint & applied_mask ����*/
	if ((info->hint_policy == HINT_POLICY_EXACT) &&
	    (!cpus_empty(info->affinity_hint))) 
	{
	    
	    /*����û����õ��׺Ͷ�λͼ���ֹǨ�Ƶ�CPUλͼ�Ƿ��н���*/
		if (cpus_intersects(info->affinity_hint, banned_cpus))
			log(TO_ALL, LOG_WARNING,
			    "irq %d affinity_hint and banned cpus confict\n",
			    info->irq);
		else {
		/*�׺Ͷ�λͼ����ͻ������applied_mask������ʾλͼ����*/
			applied_mask = info->affinity_hint;
			valid_mask = 1;
		}
	} 
	else if (info->assigned_obj) 
	{
		applied_mask = info->assigned_obj->mask;
		if ((info->hint_policy == HINT_POLICY_SUBSET) &&
		    (!cpus_empty(info->affinity_hint))) 
		{
			cpus_and(applied_mask, applied_mask, info->affinity_hint);
			if (!cpus_intersects(applied_mask, unbanned_cpus))
				log(TO_ALL, LOG_WARNING,
				    "irq %d affinity_hint subset empty\n",
				   info->irq);
			else
				valid_mask = 1;
		}
		else 
		{
			valid_mask = 1;
		}
	}

	/*����׺Ͷ����ò��ɹ��������õ��׺Ͷ�λͼ��irq_info��ԭ�������һ�£�ֱ�ӷ���*/
	if (!valid_mask || check_affinity(info, applied_mask))
		return;

	if (!info->assigned_obj)
		return;

	sprintf(buf, "/proc/irq/%i/smp_affinity", info->irq);
	file = fopen(buf, "w");
	if (!file)
		return;

	/*�����úõ��׺Ͷ�λͼ���浽���жϵ��׺Ͷ�·����*/
	cpumask_scnprintf(buf, PATH_MAX, applied_mask);
	fprintf(file, "%s", buf);
	fclose(file);

	/*Ǩ�Ƶ��ж��Ѿ�������׺Ͷ�ӳ��*/
	info->moved = 0;
}

/*����ϵͳ�жϣ��Խ�����Ǩ�Ʋ���δ���������׺Ͷ���Ϣ���ж������׺Ͷ���Ϣ*/
void activate_mappings(void)
{
	for_each_irq(NULL, activate_mapping, NULL);
}
