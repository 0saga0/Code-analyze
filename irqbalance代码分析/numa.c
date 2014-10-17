#include "config.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>

#include "irqbalance.h"

#define SYSFS_NODE_PATH "/sys/devices/system/node"

GList *numa_nodes = NULL;

static struct topo_obj unspecified_node_template = {
	.load = 0,
	.number = -1,
	.obj_type = OBJ_TYPE_NODE,
	.mask = CPU_MASK_ALL,
	.interrupts = NULL,
	.children = NULL,
	.parent = NULL,
	.obj_type_list = &numa_nodes,
};

static struct topo_obj unspecified_node;

/*����һ����һ�����ڴ���ʽڵ���ṹ*/
static void add_one_node(const char *nodename)
{
	char path[PATH_MAX];
	struct topo_obj *new;
	char *cpustr = NULL;
	FILE *f;
	ssize_t ret;
	size_t blen;

	new = calloc(1, sizeof(struct topo_obj));
	if (!new)
		return;
	/*�ڵ�Ӧ���п����е�CPU*/
	sprintf(path, "%s/%s/cpumap", SYSFS_NODE_PATH, nodename);
	f = fopen(path, "r");
	if (!f) {
		free(new);
		return;
	}
	if (ferror(f)) {
		cpus_clear(new->mask);
	} else {
		/*���ݽڵ�Ŀ�����CPUλͼ�����ýڵ����λͼ����*/
		ret = getline(&cpustr, &blen, f);
		if (ret <= 0) {
			cpus_clear(new->mask);
		} else {
			cpumask_parse_user(cpustr, ret, new->mask);
			free(cpustr);
		}
	}
	fclose(f);
	/*���ڵ����������Ϣ*/
	new->obj_type = OBJ_TYPE_NODE;	
	new->number = strtoul(&nodename[4], NULL, 10);
	new->obj_type_list = &numa_nodes;
	/*���½��Ľڵ������ڵ���������*/
	numa_nodes = g_list_append(numa_nodes, new);
}

/*����һ��NUMA���������ϵͳ֧��NUMA�������е�NUMA�ڵ���뵽������*/
void build_numa_node_list(void)
{
	DIR *dir;
	struct dirent *entry;

	/*����ģ��ṹ����һ��NUMA�ڵ���ṹ*/
	memcpy(&unspecified_node, &unspecified_node_template, sizeof (struct topo_obj));

	/*���ýṹ���뵽NUMA�������У���Ϊһ����ʵ�������ͷ��� */
	numa_nodes = g_list_append(numa_nodes, &unspecified_node);

	/*�鿴�Ƿ�֧��NUMA�ܹ������֧�֣������еĽڵ���뵽NUMA��������*/
	if (!numa_avail)
		return;

	dir = opendir(SYSFS_NODE_PATH);
	if (!dir)
		return;

	do {
		entry = readdir(dir);
		if (!entry)
			break;
		if ((entry->d_type == DT_DIR) && (strstr(entry->d_name, "node"))) {
			add_one_node(entry->d_name);
		}
	} while (entry);
	closedir(dir);
}

/*�ͷ�NUMA�ڵ���ռ䣬������������ռ��Լ��ж�����ռ�*/
static void free_numa_node(gpointer data)
{
	struct topo_obj *obj = data;
	g_list_free(obj->children);
	g_list_free(obj->interrupts);

	if (data != &unspecified_node)
		free(data);
}

/*�ͷ������ڵ��������Լ������ϵĽڵ���ռ�*/
void free_numa_node_list(void)
{
	g_list_free_full(numa_nodes, free_numa_node);
	numa_nodes = NULL;
}

/*�Ƚ������ڵ����Ƿ�һ�£����򷵻�0*/
static gint compare_node(gconstpointer a, gconstpointer b)
{
	const struct topo_obj *ai = a;
	const struct topo_obj *bi = b;

	return (ai->number == bi->number) ? 0 : 1;
}

/*��package�����ָ���ڵ������������*/
void add_package_to_node(struct topo_obj *p, int nodeid)
{
	struct topo_obj *node;
	/*���ݽڵ����ҵ�ָ��NUMA�ڵ�*/
	node = get_numa_node(nodeid);

	if (!node) {
		log(TO_CONSOLE, LOG_INFO, "Could not find numa node for node id %d\n", nodeid);
		return;
	}
	/*�����package��û���뵽�κ�NUMA�ڵ����������У�������뵽ָ���ڵ������������*/
	if (!p->parent) {
		node->children = g_list_append(node->children, p);
		p->parent = node;
	}
}

void dump_numa_node_info(struct topo_obj *d, void *unused __attribute__((unused)))
{
	char buffer[4096];

	log(TO_CONSOLE, LOG_INFO, "NUMA NODE NUMBER: %d\n", d->number);
	cpumask_scnprintf(buffer, 4096, d->mask); 
	log(TO_CONSOLE, LOG_INFO, "LOCAL CPU MASK: %s\n", buffer);
	log(TO_CONSOLE, LOG_INFO, "\n");
}

/*���ݽڵ�ID�ҵ���Ӧ��NMUA�ڵ���*/
struct topo_obj *get_numa_node(int nodeid)
{
	struct topo_obj find;
	GList *entry;

	if (!numa_avail)
		return &unspecified_node;

	if (nodeid == -1)
		return &unspecified_node;

	find.number = nodeid;

	entry = g_list_find_custom(numa_nodes, &find, compare_node);
	return entry ? entry->data : NULL;
}

