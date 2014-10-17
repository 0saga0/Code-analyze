#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <assert.h>

#include "irqbalance.h"
#include "types.h"


char *classes[] = {
	"other",
	"legacy",
	"storage",
	"video",
	"ethernet",
	"gbit-ethernet",
	"10gbit-ethernet",
	"virt-event",
	0
};

static int map_class_to_level[8] =
{ BALANCE_PACKAGE, BALANCE_CACHE, BALANCE_CORE, BALANCE_CORE, BALANCE_CORE, BALANCE_CORE, BALANCE_CORE, BALANCE_CORE };


#define MAX_CLASS 0x12
/*
 * Class codes lifted from pci spec, appendix D.
 * and mapped to irqbalance types here
 */
static short class_codes[MAX_CLASS] = {
	IRQ_OTHER,
	IRQ_SCSI,
	IRQ_ETH,
	IRQ_VIDEO,
	IRQ_OTHER,
	IRQ_OTHER,
	IRQ_LEGACY,
	IRQ_OTHER,
	IRQ_OTHER,
	IRQ_LEGACY,
	IRQ_OTHER,
	IRQ_OTHER,
	IRQ_LEGACY,
	IRQ_ETH,
	IRQ_SCSI,
	IRQ_OTHER,
	IRQ_OTHER,
	IRQ_OTHER,
};

struct user_irq_policy {
	int ban;
	int level;
	int numa_node_set;
	int numa_node;
	enum hp_e hintpolicy;
};

static GList *interrupts_db = NULL;
static GList *banned_irqs = NULL;
static GList *cl_banned_irqs = NULL;

#define SYSDEV_DIR "/sys/bus/pci/devices"

/*���ڱȽ������ж��Ƿ�һ��*/
static gint compare_ints(gconstpointer a, gconstpointer b)
{
	const struct irq_info *ai = a;
	const struct irq_info *bi = b;

	return ai->irq - bi->irq;
}

/*����һ��banned�ж�*/
static void add_banned_irq(int irq, GList **list)
{
	struct irq_info find, *new;
	GList *entry;

	find.irq = irq;
	/*�����ж�����������ָ��ж��Ѵ��ڣ�����ӣ�ֱ�ӷ���*/
	entry = g_list_find_custom(*list, &find, compare_ints);
	if (entry)
		return;
	/*���µ��жϣ�����ռ䡢����ж���Ϣ�������ж�������*/
	new = calloc(sizeof(struct irq_info), 1);
	if (!new) {
		log(TO_CONSOLE, LOG_WARNING, "No memory to ban irq %d\n", irq);
		return;
	}

	new->irq = irq;
	new->flags |= IRQ_FLAG_BANNED;
	new->hint_policy = HINT_POLICY_EXACT;

	*list = g_list_append(*list, new);
	return;
}

/*���irq�����жϣ�Ϊ��������ݽṹ���������cl_banned_irqs������*/
void add_cl_banned_irq(int irq)
{
	add_banned_irq(irq, &cl_banned_irqs);
}

/*�ж�irq�Ƿ���banned_irqs������*/
static int is_banned_irq(int irq)
{
	GList *entry;
	struct irq_info find;

	find.irq = irq;

	entry = g_list_find_custom(banned_irqs, &find, compare_ints);
	return entry ? 1:0;
}

			
/*��irq���뵽�ж����ݿ������У�������жϵ���Ϣ���������ͣ��׺ͶȲ��ԣ���һ�����ڴ���ʽڵ����Ϣ������devpathΪ
�ļ�ϵͳ��ָ���豸��·��*/
static struct irq_info *add_one_irq_to_db(const char *devpath, int irq, struct user_irq_policy *pol)
{
	int class = 0;
	int rc;
	struct irq_info *new, find;
	int numa_node;
	char path[PATH_MAX];
	FILE *fd;
	char *lcpu_mask;
	GList *entry;
	ssize_t ret;
	size_t blen;

	/*�������ж��Ƿ��Ѵ��ڣ��Ѵ����򷵻ؿ� */
	find.irq = irq;
	entry = g_list_find_custom(interrupts_db, &find, compare_ints);
	if (entry) {
		log(TO_CONSOLE, LOG_INFO, "DROPPING DUPLICATE ENTRY FOR IRQ %d on path %s\n", irq, devpath);
		return NULL;
	}

	/*�����ж��Ƿ���banned�жϣ����򷵻ؿ�*/
	if (is_banned_irq(irq)) {
		log(TO_ALL, LOG_INFO, "SKIPPING BANNED IRQ %d\n", irq);
		return NULL;
	}

	/*�����ж���Ϣ�ռ䲢���*/
	new = calloc(sizeof(struct irq_info), 1);
	if (!new)
		return NULL;

	new->irq = irq;
	new->class = IRQ_OTHER;
	new->hint_policy = pol->hintpolicy; 

	/*�����жϼ����ж�������*/
	interrupts_db = g_list_append(interrupts_db, new);

	sprintf(path, "%s/class", devpath);

	fd = fopen(path, "r");

	if (!fd) {
		perror("Can't open class file: ");
		goto get_numa_node;
	}

	rc = fscanf(fd, "%x", &class);
	fclose(fd);

	if (!rc)
		goto get_numa_node;

	/*���÷�һ�����ڴ���ʽڵ�Ͳ��*/
	class >>= 16;

	if (class >= MAX_CLASS)
		goto get_numa_node;

	new->class = class_codes[class];
	if (pol->level >= 0)
		new->level = pol->level;
	else
		new->level = map_class_to_level[class_codes[class]];

get_numa_node:
	numa_node = -1;
	if (numa_avail) {
		sprintf(path, "%s/numa_node", devpath);
		fd = fopen(path, "r");
		if (fd) {
			rc = fscanf(fd, "%d", &numa_node);
			fclose(fd);
		}
	}

	if (pol->numa_node_set == 1)
		new->numa_node = get_numa_node(pol->numa_node);
	else
		new->numa_node = get_numa_node(numa_node);

	sprintf(path, "%s/local_cpus", devpath);
	fd = fopen(path, "r");
	if (!fd) {
		cpus_setall(new->cpumask);
		goto assign_affinity_hint;
	}
	lcpu_mask = NULL;
	ret = getline(&lcpu_mask, &blen, fd);
	fclose(fd);
	if (ret <= 0) {
		cpus_setall(new->cpumask);
	} else {
		cpumask_parse_user(lcpu_mask, ret, new->cpumask);
	}
	free(lcpu_mask);

/*����irq��affinity_hint*/
assign_affinity_hint:
	cpus_clear(new->affinity_hint);
	sprintf(path, "/proc/irq/%d/affinity_hint", irq);
	fd = fopen(path, "r");
	if (!fd)
		goto out;
	lcpu_mask = NULL;
	ret = getline(&lcpu_mask, &blen, fd);
	fclose(fd);
	if (ret <= 0)
		goto out;
	/*���ַ���ת����λͼ����ֵ��affinity_hint*/
	cpumask_parse_user(lcpu_mask, ret, new->affinity_hint);
	free(lcpu_mask);
out:
	log(TO_CONSOLE, LOG_INFO, "Adding IRQ %d to database\n", irq);
	return new;
}

/*����buf�е��û����ã����浽pol��*/
static void parse_user_policy_key(char *buf, int irq, struct user_irq_policy *pol)
{
	char *key, *value, *end;
	char *levelvals[] = { "none", "package", "cache", "core" };
	int idx;
	int key_set = 1;

	key = buf;
	/*�����״γ���'='��λ�õ�ָ��*/
	value = strchr(buf, '=');

	if (!value) {
		log(TO_SYSLOG, LOG_WARNING, "Bad format for policy, ignoring: %s\n", buf);
		return;
	}

	/*�ս����buf�ַ����г��ֵȺ�֮ǰ�Ĳ��֣���value֮ǰ */
	*value = '\0';

	/*endΪvalue�ַ���β��������valueָ��=���롮/n��֮����ַ���*/
	value++;
	end = strchr(value, '\n');
	if (end)
		*end = '\0';

	/*strcasecmp���ں��Դ�Сд�Ƚ��ַ���*/

	/*�����û��Ľ�ֹ����һ���Խڵ��Լ��׺ͶȲ�������*/
	if (!strcasecmp("ban", key)) {
		if (!strcasecmp("false", value))
			pol->ban = 0;
		else if (!strcasecmp("true", value))
			pol->ban = 1;
		else {
			key_set = 0;
			log(TO_ALL, LOG_WARNING, "Unknown value for ban poilcy: %s\n", value);
		}
	} else if (!strcasecmp("balance_level", key)) {
		for (idx=0; idx<4; idx++) {
			if (!strcasecmp(levelvals[idx], value))
				break;
		}

		if (idx>3) {
			key_set = 0;
			log(TO_ALL, LOG_WARNING, "Bad value for balance_level policy: %s\n", value);
		} else
			pol->level = idx;
	} else if (!strcasecmp("numa_node", key)) {
		idx = strtoul(value, NULL, 10);	
		if (!get_numa_node(idx)) {
			log(TO_ALL, LOG_WARNING, "NUMA node %d doesn't exist\n",
				idx);
			return;
		}
		pol->numa_node = idx;
		pol->numa_node_set = 1;
	} else if (!strcasecmp("hintpolicy", key)) {
		if (!strcasecmp("exact", value))
			pol->hintpolicy = HINT_POLICY_EXACT;
		else if (!strcasecmp("subset", value))
			pol->hintpolicy = HINT_POLICY_SUBSET;
		else if (!strcasecmp("ignore", value))
			pol->hintpolicy = HINT_POLICY_IGNORE;
		else {
			key_set = 0;
			log(TO_ALL, LOG_WARNING, "Unknown value for hitpolicy: %s\n", value);
		}
	} else {
		key_set = 0;
		log(TO_ALL, LOG_WARNING, "Unknown key returned, ignoring: %s\n", key);
	}

	if (key_set)
		log(TO_ALL, LOG_INFO, "IRQ %d: Override %s to %s\n", irq, key, value);

	
}

/*�����û��Ĳ��Խű��������жϲ��� */
static void get_irq_user_policy(char *path, int irq, struct user_irq_policy *pol)
{
	char *cmd;
	FILE *output;
	char buffer[128];
	char *brc;

	/* ��ʼ�����ݽṹpolȫΪ-1���������׺ͶȲ���ΪHINT_POLICY_IGNORE*/
	memset(pol, -1, sizeof(struct user_irq_policy));
	pol->hintpolicy = global_hint_policy;

	/* ���û�����ò��Խű���ֱ�ӷ��� */
	if (!polscript)
		return;

	/*Ϊ�����������ռ䲢��ֵ*/
	cmd = alloca(strlen(path)+strlen(polscript)+64);
	if (!cmd)
		return;
	sprintf(cmd, "exec %s %s %d", polscript, path, irq);

    /*popen() ����ͨ������һ���ܵ�������fork ����һ���ӽ��̣�ִ��һ��shell ����������������һ�����̡�
    ������̱����� pclose() �����ر�*/
	output = popen(cmd, "r");
	if (!output) {
		log(TO_ALL, LOG_WARNING, "Unable to execute user policy script %s\n", polscript);
		return;
	}

	while(!feof(output)) {
		/*��output�ж�ȡ�ַ������ɹ��򷵻�buffer��ַ*/
		brc = fgets(buffer, 128, output);
		/* ���ݶ�ȡ����Ϣ����pol*/
		if (brc)
			parse_user_policy_key(brc, irq, pol);
	}
	pclose(output);
}

/*����Ƿ�Ҫbanһ��irq�������ban���Ի����Ѿ���ban������1�����򷵻�0*/
static int check_for_irq_ban(char *path, int irq)
{
	char *cmd;
	int rc;
	struct irq_info find;
	GList *entry;

	/* �����ж��Ƿ�����cl_banned_irqs������ */
	find.irq = irq;
	entry = g_list_find_custom(cl_banned_irqs, &find, compare_ints);
	if (entry)
		return 1;

	/*û��ban�Ĳ��Խű�*/
	if (!banscript)
		return 0;

	/*·�����Ϸ�*/
	if (!path)
		return 0;

	/*ִ��ban���Խű�*/
	cmd = alloca(strlen(path)+strlen(banscript)+32);
	if (!cmd)
		return 0;
	
	sprintf(cmd, "%s %s %d > /dev/null",banscript, path, irq);
	rc = system(cmd);

	/*
 	 * The system command itself failed
 	 */
	if (rc == -1) {
		log(TO_ALL, LOG_WARNING, "%s failed, please check the --banscript option\n", cmd);
		return 0;
	}

	if (WEXITSTATUS(rc)) {
		log(TO_ALL, LOG_INFO, "irq %d is baned by %s\n", irq, banscript);
		return 1;
	}
	return 0;

}

/*Ϊ��·���µ��豸�����ж���ڣ�����msi-x�Լ�int�ж� */
static void build_one_dev_entry(const char *dirname)
{
	struct dirent *entry;
	DIR *msidir;
	FILE *fd;
	int irqnum;
	struct irq_info *new;
	char path[PATH_MAX];
	char devpath[PATH_MAX];
	struct user_irq_policy pol;

	sprintf(path, "%s/%s/msi_irqs", SYSDEV_DIR, dirname);
	sprintf(devpath, "%s/%s", SYSDEV_DIR, dirname);

/*�����msi-x�жϵĻ����������е��ж�������ڣ�Ϊ��Щû�������жϲ��Ե���������жϲ��ԣ����Ҽ����ж�������*/	
	msidir = opendir(path);
	if (msidir) {
		do {
			entry = readdir(msidir);
			if (!entry)
				break;
			/*strtol()��ɨ�����d_name�ַ���������ǰ��Ŀո��ַ���ֱ���������ֻ��������Ųſ�ʼ��ת��������������Ϊ
10����ת��Ϊʮ���ƣ������������ֻ��ַ�������ʱ('\0')����ת��������������ء�*/
			irqnum = strtol(entry->d_name, NULL, 10);
			if (irqnum) {
				new = get_irq_info(irqnum);
				if (new)
					continue;
				get_irq_user_policy(devpath, irqnum, &pol);
				if ((pol.ban == 1) || (check_for_irq_ban(devpath, irqnum))) {
					add_banned_irq(irqnum, &banned_irqs);
					continue;
				}
				new = add_one_irq_to_db(devpath, irqnum, &pol);
				if (!new)
					continue;
				/*�����ж�����*/
				new->type = IRQ_TYPE_MSIX;
			}
		} while (entry != NULL);
		closedir(msidir);
		return;
	}

	sprintf(path, "%s/%s/irq", SYSDEV_DIR, dirname);
	fd = fopen(path, "r");
	if (!fd)
		return;
	if (fscanf(fd, "%d", &irqnum) < 0)
		goto done;

	/*���ڴ�ͳ�ж϶��ԣ�һ���豸ֻ��һ��int�жϺţ�������ж�δ�����жϲ��ԣ������ò������ж������� */
	if (irqnum) {
		new = get_irq_info(irqnum);
		if (new)
			goto done;
		get_irq_user_policy(devpath, irqnum, &pol);
		if ((pol.ban == 1) || (check_for_irq_ban(path, irqnum))) {
			add_banned_irq(irqnum, &banned_irqs);
			goto done;
		}

		new = add_one_irq_to_db(devpath, irqnum, &pol);
		if (!new)
			goto done;
		new->type = IRQ_TYPE_LEGACY;
	}

done:
	fclose(fd);
	return;
}

/*�ͷ�һ���ж�(�ж���Ϣ�ṹ) */
static void free_irq(struct irq_info *info, void *data __attribute__((unused)))
{
	free(info);
}

/*�ͷ��жϺ��ж�����*/
void free_irq_db(void)
{
	for_each_irq(NULL, free_irq, NULL);
	g_list_free(interrupts_db);
	interrupts_db = NULL;
	for_each_irq(banned_irqs, free_irq, NULL);
	g_list_free(banned_irqs);
	banned_irqs = NULL;
	g_list_free(rebalance_irq_list);
	rebalance_irq_list = NULL;
}

/*Ϊһ���µ��ж������ж���Ϣ���������ж�����*/
static void add_new_irq(int irq, struct irq_info *hint)
{
	struct irq_info *new;
	struct user_irq_policy pol;

	new = get_irq_info(irq);
	if (new)
		return;

	get_irq_user_policy("/sys", irq, &pol);
	if ((pol.ban == 1) || check_for_irq_ban(NULL, irq)) {
		add_banned_irq(irq, &banned_irqs);
		new = get_irq_info(irq);
	} else
		new = add_one_irq_to_db("/sys", irq, &pol);

	if (!new) {
		log(TO_CONSOLE, LOG_WARNING, "add_new_irq: Failed to add irq %d\n", irq);
		return;
	}

	/*
	 * Override some of the new irq defaults here
	 */
	if (hint) {
		new->type = hint->type;
		new->class = hint->class;
	}

	new->level = map_class_to_level[new->class];
}

/*Ϊ���ж������ж���Ϣ��������������*/
static void add_missing_irq(struct irq_info *info, void *unused __attribute__((unused)))
{
	struct irq_info *lookup = get_irq_info(info->irq);

	if (!lookup)
		add_new_irq(info->irq, info);
	
}

/*Ϊϵͳ�豸�����ж���ڣ������жϼ����ж�������*/
void rebuild_irq_db(void)
{
	DIR *devdir;
	struct dirent *entry;
	GList *tmp_irqs = NULL;

	free_irq_db();

	/*��ȡϵͳ�ж�����*/
	tmp_irqs = collect_full_irq_list();

	devdir = opendir(SYSDEV_DIR);
	if (!devdir)
		goto free;

	do {
		entry = readdir(devdir);

		if (!entry)
			break;

		build_one_dev_entry(entry->d_name);

	} while (entry != NULL);

	closedir(devdir);


	for_each_irq(tmp_irqs, add_missing_irq, NULL);

free:
	g_list_free_full(tmp_irqs, free);

}

/*�����ж�����Ĭ���Ǳ����ж����ݿ�����*/
void for_each_irq(GList *list, void (*cb)(struct irq_info *info, void *data), void *data)
{
	GList *entry = g_list_first(list ? list : interrupts_db);
	GList *next;

	while (entry) {
		next = g_list_next(entry);
		cb(entry->data, data);
		entry = next;
	}
}

/*��ȡ�ж���Ϣ*/
struct irq_info *get_irq_info(int irq)
{
	GList *entry;
	struct irq_info find;

	find.irq = irq;
	entry = g_list_find_custom(interrupts_db, &find, compare_ints);

	if (!entry)
		entry = g_list_find_custom(banned_irqs, &find, compare_ints);

	return entry ? entry->data : NULL;
}


/*�жϴ�һ������Ǩ�Ƶ���һ�������У������Ϊ��Ǩ��*/
void migrate_irq(GList **from, GList **to, struct irq_info *info)
{
	GList *entry;
	struct irq_info find, *tmp;

	find.irq = info->irq;
	entry = g_list_find_custom(*from, &find, compare_ints);

	if (!entry)
		return;

	tmp = entry->data;
	*from = g_list_delete_link(*from, entry);


	*to = g_list_append(*to, tmp);
	info->moved = 1;
}

/*�ж��Ƿ���ȷ����*/
static gint sort_irqs(gconstpointer A, gconstpointer B)
{
        struct irq_info *a, *b;
        a = (struct irq_info*)A;
        b = (struct irq_info*)B;

	if (a->class < b->class || a->load < b->load || a < b)
		return 1;
        return -1;
}

/*����ж������Ƿ�����ȷ��ʽ�������û�У������¶���������*/
void sort_irq_list(GList **list)
{
	*list = g_list_sort(*list, sort_irqs);
}
