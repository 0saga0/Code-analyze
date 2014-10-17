#include <stdlib.h>

#include "glist.h"

/*�ͷŸ�����Ŀռ� */
void
g_list_free (GList *list)
{
	GList *l = list;

	while(l) {
		GList *tmp = l->next;
		free(l);
		l = tmp;
	}
}

/*��ȡ���������һ��Ԫ�� */
GList*
g_list_last (GList *list)
{
  if (list)
    {
      while (list->next)
	  	list = list->next;
    }
  
  return list;
}

/*����һ���½ڵ㣬�����ڵ��������β��������������ڣ����ýڵ���Ϊ������ͷ�� */
GList*
g_list_append (GList *list, gpointer data)
{
  GList *new_list;
  GList *last;
  
  new_list = malloc(sizeof(*new_list));
  new_list->data = data;
  new_list->next = NULL;
  
  if (list)
    {
      last = g_list_last (list);
      last->next = new_list;
      new_list->prev = last;

      return list;
    }
  else
    {
      new_list->prev = NULL;
      return new_list;
    }
}

/*��һ��Ԫ�ش��������Ƴ�*/
static inline GList*
_g_list_remove_link (GList *list,
		     GList *link)
{
  if (link)
    {
      if (link->prev)
	link->prev->next = link->next;
      if (link->next)
	link->next->prev = link->prev;
      
      if (link == list)
	list = list->next;
      
      link->next = NULL;
      link->prev = NULL;
    }
  
  return list;
}

/*��Ԫ�ش��������Ƴ����ͷ���ռ� */
GList*
g_list_delete_link (GList *list,
		    GList *link_)
{
  list = _g_list_remove_link (list, link_);
  free (link_);

  return list;
}

/*��ȡ����ĵ�һ��Ԫ�� */
GList*
g_list_first (GList *list)
{
  if (list)
    {
      while (list->prev)
		list = list->prev;
    }
  
  return list;
}

/*����������ϲ����������ض��ıȽϷ�ʽ����*/
static GList *
g_list_sort_merge (GList     *l1, 
		   GList     *l2,
		   GFunc     compare_func,
		   gpointer  user_data)
{
  GList list, *l, *lprev;
  gint cmp;

  l = &list; 
  lprev = NULL;

  while (l1 && l2)
    {
      cmp = ((GCompareDataFunc) compare_func) (l1->data, l2->data, user_data);

      if (cmp <= 0)
      {
	 	 l->next = l1;
	 	 l1 = l1->next;
      } 
      else 
	  {
	 	 l->next = l2;
	 	 l2 = l2->next;
      }
      l = l->next;
      l->prev = lprev; 
      lprev = l;
    }
  l->next = l1 ? l1 : l2;
  l->next->prev = l;

  return list.next;
}

/*�������ղ������趨�ȽϷ�ʽ����*/
static GList* 
g_list_sort_real (GList    *list,
		  GFunc     compare_func,
		  gpointer  user_data)
{
  GList *l1, *l2;
  
  if (!list) 
    return NULL;
  if (!list->next) 
    return list;

  /*������L���ǰ�����룬�ֱ���L1��L2��ָ*/
  l1 = list; 
  l2 = list->next;

  while ((l2 = l2->next) != NULL)
  {
      if ((l2 = l2->next) == NULL) 
		break;
      l1 = l1->next;
  }
  l2 = l1->next; 
  l1->next = NULL; 

  /*�ݹ�ķ�ʽ��������������ʵ�����������ղ����еıȽϷ�ʽ����*/
  return g_list_sort_merge (g_list_sort_real (list, compare_func, user_data),
			    g_list_sort_real (l2, compare_func, user_data),
			    compare_func,
			    user_data);
}

/*�������ղ������趨�ȽϷ�ʽ����*/
GList *
g_list_sort (GList        *list,
	     GCompareFunc  compare_func)
{
  return g_list_sort_real (list, (GFunc) compare_func, NULL);
			    
}

/*��ȡ������Ԫ�صĸ��� */
guint
g_list_length (GList *list)
{
  guint length;
  
  length = 0;
  while (list)
    {
      length++;
      list = list->next;
    }
  
  return length;
}

/*���������������е�ÿһ��Ԫ�ص��ò����еĺ���*/
void
g_list_foreach (GList	 *list,
		GFunc	  func,
		gpointer  user_data)
{
  while (list)
    {
      GList *next = list->next;
      (*func) (list->data, user_data);
      list = next;
    }
}

/*�ͷ�������ÿһ��Ԫ�ص����ݽṹ�����ͷ�����ռ�*/
void
g_list_free_full (GList          *list,
		  GDestroyNotify  free_func)
{
  g_list_foreach (list, (GFunc) free_func, NULL);
  g_list_free (list);
}

/*���ݲ����еĹ��ܺ����ҵ�����������Ҫ���Ԫ��*/
GList*
g_list_find_custom (GList         *list,
		    gconstpointer  data,
		    GCompareFunc   func)
{
  g_return_val_if_fail (func != NULL, list);

  while (list)
    {
      if (! func (list->data, data))
		return list;
      list = list->next;
    }

  return NULL;
}

/*ɾ���������ض���Ԫ�أ������ͷ���ռ� */
GList*
g_list_remove (GList         *list,
               gconstpointer  data)
{
  GList *tmp;
 
  tmp = list;
  while (tmp)
    {
      if (tmp->data != data)
        tmp = tmp->next;
      else
        {
          list = _g_list_remove_link(list, tmp);
          g_list_free(tmp);

          break;
        }
    }
  return list;
}

