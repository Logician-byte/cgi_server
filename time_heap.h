#ifndef _TIME_HEAP_H
#define _TIME_HEAP_H
#include<iostream>
#include<stdio.h>
#include<netinet/in.h>
#include<time.h>
using std::exception;
//定时器类
template<typename T>
class heap_timer
{
	public:
		time_t expire;	/*定时器生效的绝对时间*/
		void (T::*function) ();//任务回调函数
		T* cgi_conn;
	public:
		heap_timer(int delay)
		{
			expire = time(NULL) + delay;
		}
};
/*时间堆类*/
template<typename T>
class time_heap
{
	private:
		heap_timer<T>** heap;/*堆数组*/
		int capacity;/*堆数组的容量*/
		int cur_size;/*元素个数*/
		/*最小堆下滤操作，确保堆数组中以第node个节点作为根的子树拥有最小堆性质*/
		void down(int node)
		{
			heap_timer<T>* tmp = heap[node];
			int child;
			for(;(node*2+1) < cur_size;node = child)
			{
				child = node*2+1;
				if((child+1 < cur_size) && (heap[child+1]->expire < heap[child]->expire))
				{
					child++;
				}
				if(heap[child]->expire < tmp->expire)
				{
					heap[node] = heap[child];
				}
				else break;
			}
			heap[node] = tmp;
		}
		/*将堆数组容量扩大1倍*/
		void resize()
		{
			heap_timer<T>** temp = new heap_timer<T>*[2*capacity];
			if(temp == NULL)
				throw std::exception();
			for(int i = 0;i < 2*capacity;i++)
			{
				if(i < cur_size)
				{
					temp[i] = heap[i];
				}
				else temp[i] = NULL;
			}
			delete [] heap;
			heap = temp;
			capacity *= 2;
		}
	public:
		time_heap(int capacity):capacity(capacity),cur_size(0)
		{
			heap = new heap_timer<T>*[capacity];
			if(heap == NULL)
				throw std::exception();
			for(int i = 0;i < capacity;i++)
				heap[i] = NULL;
		}
		time_heap(heap_timer<T>** temp,int size,int capacity):capacity(capacity),cur_size(size)
		{
			if(size > capacity)
				throw std::exception();
			heap = new heap_timer<T>*[capacity];
			if(heap == NULL)
				throw std::exception();
			for(int i = 0;i < capacity;i++)
			{
				if(i < size)
					heap[i] = temp[i];
				else heap[i] = NULL;
			}
			if(size > 0)
			{
				/*对非叶子节点进行下滤操作*/
				for(int i = (cur_size - 2)/2;i >= 0;i--)
					down(i);
			}
		}
		~time_heap()
		{
			for(int i = 0;i < cur_size;i++)
				delete heap[i];
			delete[]heap;
		}
		/*添加定时器*/
		void add_timer(heap_timer<T>* timer)
		{
			if(timer == NULL)
				return;
			if(cur_size >= capacity)/*若空间不够，则扩大1倍*/
				resize();
			/*新插入一个元素，当前堆大小加1，node是新建空穴的位置*/
			int node = cur_size++;
			int parent;
			/*对从空穴到根节点的路径上的所有节点执行上虑操作*/
			for(;node > 0;node = parent)
			{
				parent = (node-1)/2;
				if(timer->expire >= heap[parent]->expire)
					break;
				else heap[node] = heap[parent];
			}
			heap[node] = timer;
		}
		/*查看堆是否为空*/
		bool empty()
		{
			return cur_size == 0;
		}
		/*获得堆顶*/
		heap_timer<T>* top()
		{
			if(empty())
				throw std::exception();
			return heap[0];
		}
		/*删除堆顶定时器*/
		void pop()
		{
			if(empty())
			{
				throw std::exception();
			}
			delete heap[0];
			heap[0] = heap[cur_size-1];
			cur_size--;
			down(0);
		}
		/*删除目标定时器*/
		void del_timer(heap_timer<T>* timer)
		{
			if(timer == NULL)
				return;
			/*将定时器对应的回调函数指向空，这样起到延迟删除的作用，节省了真正删除定时器的开销，但是会容易使堆数组膨胀*/
			timer->function = NULL;
		}
		/*调整目标定时器*/
		void adjust_timer(heap_timer<T>* timer,time_t timeout)
		{
			if(timer == NULL)
				return;
			timer->expire = timeout;
			down(0);
		}
		/*心博函数*/
		void tick()
		{
			time_t timeout = time(NULL);
			while(!empty())
			{
				/*如果堆顶没到期则退出循环*/
				if(heap[0]->expire > timeout)
					break;
				/*否则执行堆顶定时器中的任务*/
				if(heap[0]->function != NULL)
				{
					(heap[0]->cgi_conn->*heap[0]->function)();
				}
				/*将堆顶删除*/
				pop();
			}
		}
};
#endif
