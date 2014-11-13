#include "nf10iface.h"
#include "nf10driver.h"
#include "nf10priv.h"

#include <linux/interrupt.h>
#include <linux/pci.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>

//���ڵ��жϴ��������������豸���жϺŰ�
irqreturn_t int_handler(int irq, void *dev_id)
{
    struct pci_dev *pdev = dev_id;
/*��nf10_probe������ʹ�ú���platform_set_drvdata()��ndev�����ƽ̨�����豸��˽�����ݡ�
ʹ����ʱֻ�����platform_get_drvdata()�Ϳ�����*/
	struct nf10_card *card = (struct nf10_card*)pci_get_drvdata(pdev);

/*����ִ��һ��ָ��workqueue�е����񣬵�һ������Ϊָ����workqueueָ�룬�ڶ�������Ϊ�������������ָ�� */
    queue_work(card->wq, (struct work_struct*)&card->work);

    return IRQ_HANDLED;   //��ʾ�жϴ�����ɣ���֮��Ӧ����IRQ_NONE
}

static netdev_tx_t nf10i_tx(struct sk_buff *skb, struct net_device *dev)
{
//	ͨ��netdev_priv���ʵ���˽�����ݣ� ��ͨ��struct net_device *dev�׵�ַ�Ӷ�����ƫ�����͵õ���˽�����ݵ��׵�ַ
	struct nf10_card* card = ((struct nf10_ndev_priv*)netdev_priv(dev))->card;

//Ϊ�˺���İ����䣬�Ȼ�ȡ�������ṹ�Ķ˿ںţ���Ҫע�����������4��
    int port = ((struct nf10_ndev_priv*)netdev_priv(dev))->port_num;

	//����֮ǰҪ�ȼ�����С�Ƿ����Ҫ��
    // meet minimum size requirement
    if(skb->len < 60)
    {
    //������С�ﲻ����СҪ��ʱ���������β����0��������һ������Ϊ����pad��skb��ַ������Ϊpad�ĳ���
        skb_pad(skb, 60 - skb->len);
	//skb_put() �����������ĳ�����Ϊmemcpy׼���ռ�. �������������Ҫ����һЩ��ͷ
        skb_put(skb, 60 - skb->len);
    }

    if(skb->len > 1514)
    {
        printk(KERN_ERR "nf10: packet too big, dropping");
/*�����������ֵʱ���ͷ�sk_buff�ṹ
Linux�ں�ʹ��kfree_skb(),dev_kfree_skb()�������ڷ��ж������ģ�
dev_kfree_skb_irq(),�����ж������ġ�
dev_kfree_skb_any()��ʾ�ж�����жϽԿ���*/
		dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    // transmit packet�����ڴ�С����Ҫ��İ������÷��ͺ��������ݰ����ͳ�ȥ
    if(nf10priv_xmit(card, skb, port))
    {
	//������ط�0������ʾ���ͳ���
		printk(KERN_ERR "nf10: dropping packet at port %d", port);
        dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }

    // update stats����������󣬽���Ӧ����ṹ���Ͱ�����Ŀ�Լ����͵����ֽ���ˢ��
    card->ndev[port]->stats.tx_packets++;
    card->ndev[port]->stats.tx_bytes += skb->len;

    return NETDEV_TX_OK;
}

static int nf10i_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
    return 0;
}

static int nf10i_open(struct net_device *dev)
{
//����netdev_priv��ȡ������˽�����ݵ�ַ��ֱ�ӷ�����net_device�ṹĩ�˵�ַ����Ϊpriv��Ա������dev�ṹ����棬
//���ص�Ҳ����priv���׵�ַ��
//��port�����жϱ�����1��ʾ������ṹ����ʹ��
	((struct nf10_ndev_priv*)netdev_priv(dev))->port_up = 1;
    return 0;
}

static int nf10i_stop(struct net_device *dev)
{
//��port�����жϱ�����0��ʾ������ṹ�ѱ�������
	((struct nf10_ndev_priv*)netdev_priv(dev))->port_up = 0;
    return 0;
}

//Ϊ�����豸����mac��ַ
static int nf10i_set_mac(struct net_device *dev, void *a)
{
/*
sockaddr�䶨�����£�
struct sockaddr {
����unsigned short sa_family; 
����char sa_data[14]; 
����};
˵����sa_family ����2�ֽڵĵ�ַ���壬һ�㶼�ǡ�AF_xxx������ʽ��ͨ���õĶ���AF_INET��
����  sa_data �� ��14�ֽڵ�Э���ַ��*/
	struct sockaddr *addr = (struct sockaddr *) a;

//�жϵ�ַ�Ƿ�Ϸ�
    if (!is_valid_ether_addr(addr->sa_data))
        return -EADDRNOTAVAIL;

//��Դsrc��ָ���ڴ��ַ����ʼλ�ÿ�ʼ����n���ֽڵ�Ŀ��dest��ָ���ڴ��ַ����ʼλ����
//����ԭ��void *memcpy(void *dest, const void *src, size_t n);
	memcpy(dev->dev_addr, addr->sa_data, ETH_ALEN);

    return 0;
}

static struct net_device_stats *nf10i_stats(struct net_device *dev)
{
//����ͳ����Ϣ���緢�͵İ�����Ŀ�Լ����͵��ܵ��ֽ���Ŀ
	return &dev->stats;
}

//������������ӿ�
static const struct net_device_ops nf10_ops =
{
    .ndo_open            = nf10i_open,
    .ndo_stop            = nf10i_stop,
    .ndo_do_ioctl        = nf10i_ioctl,
    .ndo_get_stats       = nf10i_stats,
    .ndo_start_xmit      = nf10i_tx,
    .ndo_set_mac_address = nf10i_set_mac
};

// init called by alloc_netdev������ӿڵĳ�ʼ����Ϊ��ָ�������ṹ������䵥Ԫ��
static void nf10iface_init(struct net_device *dev)
{
    ether_setup(dev); /* assign some of the fields */

    dev->netdev_ops      = &nf10_ops;
    dev->watchdog_timeo  = msecs_to_jiffies(5000);
    dev->mtu             = MTU;

}

//��ʹ������֮ǰ��Ҫ�������жϲ���ʼ����4�������豸�ṹ
int nf10iface_probe(struct pci_dev *pdev, struct nf10_card *card)
{
    int ret = -ENODEV;
    int i;

    struct net_device *netdev;

    char *devname = "nf%d";

    // request IRQ��ע���жϣ������жϴ�����
    if(request_irq(pdev->irq, int_handler, 0, DEVICE_NAME, pdev) != 0)
    {
        printk(KERN_ERR "nf10: request_irq failed\n");
        goto err_out_free_none;
    }

    // Set up the network device
    //����4�������豸�ṹ��Ϊ�����ռ䲢��ʼ��...
    for (i = 0; i < 4; i++)
    {
/*alloc_netdev()��������һ��net_device�ṹ�壬�����Ա��ֵ�����ظýṹ���ָ�롣
��һ���������豸˽�г�Ա�Ĵ�С���ڶ�������Ϊ�豸��������������Ϊnet_device��setup()����ָ��
setup()�������յĲ���Ϊstruct net_deviceָ�룬����Ԥ��net_device��Ա��ֵ��*/
		netdev = card->ndev[i] = alloc_netdev(sizeof(struct nf10_ndev_priv),
                                              devname, nf10iface_init);
        if(netdev == NULL)
        {
            printk(KERN_ERR "nf10: Could not allocate ethernet device.\n");
            ret = -ENOMEM;
            goto err_out_free_dev;
        }

		//���е�4�������豸�ṹ�����������豸���жϺ�
        netdev->irq = pdev->irq;

        ((struct nf10_ndev_priv*)netdev_priv(netdev))->card     = card;
		//���õ������豸�ṹ��Ӧ���õ�port
        ((struct nf10_ndev_priv*)netdev_priv(netdev))->port_num = i;
        ((struct nf10_ndev_priv*)netdev_priv(netdev))->port_up  = 0;

        // assign some made up MAC adddr
        memcpy(netdev->dev_addr, "\0NF10C0", ETH_ALEN);
        netdev->dev_addr[ETH_ALEN - 1] = i;
		//ע�������豸�ṹ
        if(register_netdev(netdev))
        {
            printk(KERN_ERR "nf10: register_netdev failed\n");
        }
	//���������ϲ�����Э��������������пյĻ��������ã������һ������ͽ�����
        netif_start_queue(netdev);
    }

    // give some descriptors to the card������ָ�������Ľ���������
    for(i = 0; i < card->mem_rx_dsc.cl_size-2; i++)
    {
		nf10priv_send_rx_dsc(card);
    }

    // yay
    return 0;

    // fail
err_out_free_dev:
    for (i = 0; i < 4; i++)
    {
        if(card->ndev[i])
        {
            unregister_netdev(card->ndev[i]);
            free_netdev(card->ndev[i]);
        }
    }

err_out_free_none:
    return ret;
}

//�Ƴ�����ӿ�ʱ��Ҫ��4�������豸�ṹȫ���ͷ�
int nf10iface_remove(struct pci_dev *pdev, struct nf10_card *card)
{
    int i;

    for (i = 0; i < 4; i++)
    {
        if(card->ndev[i])
        {
            unregister_netdev(card->ndev[i]);
            free_netdev(card->ndev[i]);
        }
    }
    free_irq(pdev->irq, pdev);
    return 0;
}
