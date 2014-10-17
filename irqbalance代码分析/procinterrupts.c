#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <ctype.h>

#include "cpumask.h"
#include "irqbalance.h"

#define LINESIZE 4096

static int proc_int_has_msi = 0;
static int msi_found_in_sysfs = 0;

/*�����ж�Ŀ¼����ȡ�ж���Ϣ������ж���Ϣ�ṹ������Ϣ�������ж�������*/
GList* collect_full_irq_list()
{
	GList *tmp_list = NULL;
	FILE *file;
	char *line = NULL;
	size_t size = 0;
	char *irq_name, *savedptr, *last_token, *p;

	file = fopen("/proc/interrupts", "r");
	if (!file)
		return NULL;

	/*��һ����CPU��ţ�����Ҫ�� */
	if (getline(&line, &size, file)==0) {
		free(line);
		fclose(file);
		return NULL;
	}

	while (!feof(file)) {
		int	 number;
		struct irq_info *info;
		char *c;
		char savedline[1024];

		if (getline(&line, &size, file)==0)
			break;

		/*��ʼ��ȡ�жϺŻ������� */
		c = line;
		while (isblank(*(c)))
			c++;
		/*ʹ���жϺ���Ϣ���жϲ���Ҫ�ܣ����Ҹ����ж�����ǰ�棬һ�������ж����Ʊ�ע���жϣ�����NMI��LOC��ֱ�Ӻ���*/	
		if (!(*c>='0' && *c<='9'))
			break;

		/*ð�Ž������жϺź���*/
		c = strchr(line, ':');
		if (!c)
			continue;

		/*�����ж���Ϣ��������Ϣ�����ݽ��зָ�*/
		strncpy(savedline, line, sizeof(savedline));
		irq_name = strtok_r(savedline, " ", &savedptr);
		last_token = strtok_r(NULL, " ", &savedptr);
		while ((p = strtok_r(NULL, " ", &savedptr))) {
			irq_name = last_token;
			last_token = p;
		}

		/*���жϺŰ�ʮ���Ʊ��棬��C��ֵΪ0�ͽ��жϺ���������Ϣ�ָ�����*/
		*c = 0;
		c++;
		number = strtoul(line, NULL, 10);

		/*�����ж���Ϣ�ṹ��������жϺ��Լ�����������Ϣ*/
		info = calloc(sizeof(struct irq_info), 1);
		if (info) {
			info->irq = number;
			if (strstr(irq_name, "xen-dyn-event") != NULL) {
				info->type = IRQ_TYPE_VIRT_EVENT;
				info->class = IRQ_VIRT_EVENT;
			} else {
				info->type = IRQ_TYPE_LEGACY;
				info->class = IRQ_OTHER;
			} 
			info->hint_policy = global_hint_policy;

		/*��ʶ������жϼ���������*/
			tmp_list = g_list_append(tmp_list, info);
		}

	}
	fclose(file);
	free(line);
	return tmp_list;
}

/*�����жϳ�����������Ϣ�������ϵͳ�ж��Ƿ���ȷ*/
void parse_proc_interrupts(void)
{
	FILE *file;
	char *line = NULL;
	size_t size = 0;

	file = fopen("/proc/interrupts", "r");
	if (!file)
		return;

	/*��һ����CPU��ţ�����Ҫ�� */
	if (getline(&line, &size, file)==0) {
		free(line);
		fclose(file);
		return;
	}

	while (!feof(file)) {
		int cpunr;
		int	 number;
		uint64_t count;
		char *c, *c2;
		struct irq_info *info;
		char savedline[1024];

		if (getline(&line, &size, file)==0)
			break;

		/*�ж��Ƿ���msi�ж�*/
		if (!proc_int_has_msi)
			if (strstr(line, "MSI") != NULL)
				proc_int_has_msi = 1;

		/*ʹ���жϺ���Ϣ���жϲ���Ҫ�ܣ����Ҹ����ж�����ǰ�棬һ�������ж����Ʊ�ע���жϣ�����NMI��LOC��ֱ�Ӻ���*/	
		c = line;
		while (isblank(*(c)))
			c++;	
		if (!(*c>='0' && *c<='9'))
			break;

		/*ð�Ž������жϺź���*/
		c = strchr(line, ':');
		if (!c)
			continue;

		strncpy(savedline, line, sizeof(savedline));

		*c = 0;
		c++;
		/*��ʮ���Ʒ�ʽ�����жϺ���Ϣ*/
		number = strtoul(line, NULL, 10);

		/*ͨ���жϺŻ�ȡ�ж���Ϣ�ṹ������õ��սṹ��˵����Ҫ���½����ж���Ϣ*/
		info = get_irq_info(number);
		if (!info) {
			need_rescan = 1;
			break;
		}
		/*���ڼ�¼һ���жϴ������ܴ����Լ�ϵͳ�е�CPU��Ŀ*/
		count = 0;
		cpunr = 0;

		/*��ȡ�ж���ÿ��CPU�ϴ����Ĵ������ۼ�*/
		c2=NULL;
		while (1) {
			uint64_t C;
			C = strtoull(c, &c2, 10);
			if (c==c2) /* end of numbers */
				break;
			count += C;
			c=c2;
			cpunr++;
		}

		/*ͳ�Ƶõ���CPU����Ŀ��CPU����������Ŀ��ƥ�䣬��ʱӦ�����½��������˽ṹ*/
		if (cpunr != core_count) {
			need_rescan = 1;
			break;
		}

		/* ��Ϊ�ж��Ƴ��Ͳ������ֵ��������ʱӦ�����½��������˽ṹ */
		if (count < info->irq_count) {
			need_rescan = 1;
			break;
		}

		/*�����жϴ�����������Ϣ*/
		info->last_irq_count = info->irq_count;		
		info->irq_count = count;

		/* �����MSI/MSI-X�жϣ����б��*/
		if ((info->type == IRQ_TYPE_MSI) || (info->type == IRQ_TYPE_MSIX))
			msi_found_in_sysfs = 1;
	}		
	if ((proc_int_has_msi) && (!msi_found_in_sysfs) && (!need_rescan)) {
		log(TO_ALL, LOG_WARNING, "WARNING: MSI interrupts found in /proc/interrupts\n");
		log(TO_ALL, LOG_WARNING, "But none found in sysfs, you need to update your kernel\n");
		log(TO_ALL, LOG_WARNING, "Until then, IRQs will be improperly classified\n");
		/*
 		 * Set msi_foun_in_sysfs, so we don't get this error constantly
 		 */
		msi_found_in_sysfs = 1;
	}
	fclose(file);
	free(line);
}

/*������һ�������ڡ������жϴ����ĸ�����Ϊ���ؽ��м�¼*/
static void accumulate_irq_count(struct irq_info *info, void *data)
{
	uint64_t *acc = data;

	*acc += (info->irq_count - info->last_irq_count);
}

/**/
static void assign_load_slice(struct irq_info *info, void *data)
{
	uint64_t *load_slice = data;
	info->load = (info->irq_count - info->last_irq_count) * *load_slice;

	/*ÿһ���жϵĸ��ض�����ҪΪ����*/
	if (!info->load)
		info->load++;
}

/*
 * Recursive helper to estimate the number of irqs shared between 
 * multiple topology objects that was handled by this particular object
 */
static uint64_t get_parent_branch_irq_count_share(struct topo_obj *d)
{
	uint64_t total_irq_count = 0;

	if (d->parent) {
		total_irq_count = get_parent_branch_irq_count_share(d->parent);
		total_irq_count /= g_list_length((d->parent)->children);
	}

	if (g_list_length(d->interrupts) > 0)
		for_each_irq(d->interrupts, accumulate_irq_count, &total_irq_count);

	return total_irq_count;
}

static void compute_irq_branch_load_share(struct topo_obj *d, void *data __attribute__((unused)))
{
	uint64_t local_irq_counts = 0;
	uint64_t load_slice;
	int	load_divisor = g_list_length(d->children);

	d->load /= (load_divisor ? load_divisor : 1);

	if (g_list_length(d->interrupts) > 0) 
	{
		local_irq_counts = get_parent_branch_irq_count_share(d);
		load_slice = local_irq_counts ? (d->load / local_irq_counts) : 1;
		for_each_irq(d->interrupts, assign_load_slice, &load_slice);
	}

	if (d->parent)
		d->parent->load += d->load;
}

/*�����ؽ�����0*/
static void reset_load(struct topo_obj *d, void *data __attribute__((unused)))
{
	if (d->parent)
		reset_load(d->parent, NULL);

	d->load = 0;
}

void parse_proc_stat(void)
{
	FILE *file;
	char *line = NULL;
	size_t size = 0;
	int cpunr, rc, cpucount;
	struct topo_obj *cpu;
	unsigned long long irq_load, softirq_load;

/*��Ŀ¼����ÿһ��CPU�ĸ��ؼ�¼*/
	file = fopen("/proc/stat", "r");
	if (!file) {
		log(TO_ALL, LOG_WARNING, "WARNING cant open /proc/stat.  balacing is broken\n");
		return;
	}

	/* ��һ����CPU����ͳ�ƺͣ�����Ҫ*/
	if (getline(&line, &size, file)==0) {
		free(line);
		log(TO_ALL, LOG_WARNING, "WARNING read /proc/stat. balancing is broken\n");
		fclose(file);
		return;
	}

	cpucount = 0;
	while (!feof(file)) {
		if (getline(&line, &size, file)==0)
			break;

		/*ֻҪCPU�ĸ�����Ϣ������ĺ���*/
		if (!strstr(line, "cpu"))
			break;
		/*��CPU��Ű�ʮ���Ʊ���*/
		cpunr = strtoul(&line[3], NULL, 10);

		/*������CPU�Ѿ���ban�б��У�������CPU*/
		if (cpu_isset(cpunr, banned_cpus))
			continue;

		/*��ȡCPU���жϺ����жϸ���*/
		rc = sscanf(line, "%*s %*u %*u %*u %*u %*u %llu %llu", &irq_load, &softirq_load);
		if (rc < 2)
			break;	

		/*��ȡCPU��ṹ*/
		cpu = find_cpu_core(cpunr);
		if (!cpu)
			break;

		/*CPU������*/
		cpucount++;

		/*
 		 * For each cpu add the irq and softirq load and propagate that
 		 * all the way up the device tree
 		 */
		if (cycle_count) {
			cpu->load = (irq_load + softirq_load) - (cpu->last_load);
			/*
			 * the [soft]irq_load values are in jiffies, with
			 * HZ jiffies per second.  Convert the load to nanoseconds
			 * to get a better integer resolution of nanoseconds per
			 * interrupt.
			 */
			cpu->load *= NSEC_PER_SEC/HZ;
		}
		cpu->last_load = (irq_load + softirq_load);
	}

	fclose(file);
	free(line);
	if (cpucount != get_cpu_count()) {
		log(TO_ALL, LOG_WARNING, "WARNING, didn't collect load info for all cpus, balancing is broken\n");
		return;
	}

	/*���CPU�����ϵĽṹ��ĸ���ֵ */
	for_each_object(cache_domains, reset_load, NULL);

	/*
 	 * Now that we have load for each cpu attribute a fair share of the load
 	 * to each irq on that cpu
 	 */
	for_each_object(cpus, compute_irq_branch_load_share, NULL);
	for_each_object(cache_domains, compute_irq_branch_load_share, NULL);
	for_each_object(packages, compute_irq_branch_load_share, NULL);
	for_each_object(numa_nodes, compute_irq_branch_load_share, NULL);

}
