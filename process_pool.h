#ifndef _PROCESS_POOL_H
#define _PROCESS_POOL_H
#include<arpa/inet.h>
#include<assert.h>
#include<sys/socket.h>
#include<sys/types.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<sys/epoll.h>
#include<errno.h>
#include<fcntl.h>
#include<stdio.h>
#include<sys/wait.h>
#include"time_heap.h"
#include<time.h>
#define TIME_OUT 5
/*描述一个子进程的类,pid是目标子进程的pid,pipefd是父子进程之间通信的管道*/
class process{
	public:
		pid_t pid;		//子进程pid
		int pipefd[2];	//子进程与父进程之间的管道
	public:
		process():pid(-1){}
};

/*进程池类，定义为模板方便代码复用，模板参数是处理逻辑任务的类*/
template<typename T>
class process_pool{
	private:
		/*进程池允许的最大子进程数量*/
		static const int MAX_PROCESS_NUMBER = 16;
		/*一个子进程最多能处理多少个客户端*/
		static const int MAX_CLIENT_NUMBER = 65535;
		/*epoll的监听上限*/
		static const int MAX_EVENT_NUMBER = 10000;
		/*进程池中子进程的个数*/
		int process_number;
		/*进程在进程池中的编号*/
		int process_id;
		/*监听的socket*/
		int listenfd;
		/*进程的epollfd*/
		int epollfd;
		/*是否停止运行的标志*/
		int stop;
		/*进程池中所有子进程的描述信息*/
		process* sub_process;
		/*进程池静态实例*/
		static process_pool<T>* instance;
		/*时间堆*/
		time_heap<T> *min_heap;
	private:
		/*单例模式中的懒汉式，将构造函数私有化，通过下面的getinstance静态函数来获取process_pool实例，保证全局只有唯一一个实例*/
		process_pool(int listenfd,int process_number = 0):listenfd(listenfd),process_number(process_number),process_id(-1)
		{
			min_heap = new time_heap<T>(MAX_CLIENT_NUMBER);
			assert(min_heap != nullptr);

			sub_process =  new process[process_number];
			assert(sub_process != nullptr);

			for(int i = 0;i < process_number;i++)
			{
				int ret = socketpair(AF_UNIX,SOCK_STREAM,0,sub_process[i].pipefd);
				assert(ret == 0);

				sub_process[i].pid = fork();
				assert(sub_process[i].pid >= 0);

				if(sub_process[i].pid > 0)
				{
					close(sub_process[i].pipefd[0]);
				}
				else{
					close(sub_process[i].pipefd[1]);
					process_id = i;
					break;
				}
			}
		}
		void timer_handler();
		void set_sig_pipe(bool isparent);
		void run_child();
		void run_parent();
	public:
		static process_pool<T>* getinstance(int listenfd,int process_number = 8)
		{
			if(instance == nullptr)
			{
				instance = new process_pool<T>(listenfd,process_number);
			}
			return instance;
		}
		~process_pool()
		{
			delete []sub_process;
		}
		/*启动进程池*/
		void run();
};
template<typename T>
process_pool<T>* process_pool<T>::instance = nullptr;

/*处理信号的管道，以实现统一信号源*/
static int sig_pipefd[2];

static void set_nonblock(int fd)
{
	int flag = fcntl(fd,F_GETFL);
	flag |= O_NONBLOCK;
	fcntl(fd,F_SETFL,flag);
}

static void addfd(int epollfd,int fd)
{
	epoll_event event;
	event.data.fd =  fd;
	event.events = EPOLLIN | EPOLLET;
	epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
	set_nonblock(fd);
}

static void sig_handler(int sig)
{
	send(sig_pipefd[1],(char*)&sig,1,0);
}

static void addsig(int sig,void(handler)(int))
{
	struct sigaction sa;
	sa.sa_handler = handler;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(sig,&sa,NULL);
}

template<typename T>
void process_pool<T>::timer_handler()
{
	min_heap->tick();
	if(!min_heap->empty())
	{
		heap_timer<T>* timer = min_heap->top();
		int timeout = timer->expire - time(NULL);
		alarm(timeout);
	}
}
/*统一事件源*/
template<typename T>
void process_pool<T>::set_sig_pipe(bool isparent)
{
	epollfd  = epoll_create(5);
	assert(epollfd != -1);

	int ret =  socketpair(AF_UNIX,SOCK_STREAM,0,sig_pipefd);
	assert(ret != -1);

	addfd(epollfd,sig_pipefd[0]);

	addsig(SIGINT,sig_handler);
	addsig(SIGPIPE,SIG_IGN);
	/*若为父进程则设置SIGCHLD信号,用来回收子进程*/
	if(isparent)
		addsig(SIGCHLD,sig_handler);
	else //若为子进程则设置SIGALRM信号
		addsig(SIGALRM,sig_handler);
}

/*父进程的编号为-1，子进程的编号大于等于0，利用编号来判断启动父进程还是子进程*/
template<typename T>
void process_pool<T>::run()
{
	if(process_id == -1)
		run_parent();
	else run_child();
}

template<typename T>
void process_pool<T>::run_child()
{
	set_sig_pipe(false);

	addfd(epollfd,sub_process[process_id].pipefd[0]);

	epoll_event events[MAX_EVENT_NUMBER];
	T *users = new T[MAX_CLIENT_NUMBER];
	stop = false;
	bool timeout = false;
	while(!stop)
	{
		int n = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
		if(n < 0)
		{
			if(errno == EINTR)
				continue;
			perror("epoll_wait error:");
			break;
		}
		for(int i = 0;i < n;i++)
		{
			int socketfd = events[i].data.fd;
			if(socketfd == sub_process[process_id].pipefd[0])
			{
				int num = -1;
				while(1)
				{
					int ret = recv(socketfd,(char*)&num,sizeof(num),0);
					if(ret < 0)
					{
						if(errno != EAGAIN)
							perror("sub_process pipefd recv error:");
						break;
					}
					//父进程关闭与该子进程的管道一端，则跳出
					if(ret == 0)
						break;
					struct sockaddr_in conn_addr;
					socklen_t conn_addr_len = sizeof(conn_addr);
					int connfd = accept(listenfd,(struct sockaddr*)&conn_addr,&conn_addr_len);
					if(connfd < 0)
					{
						perror("accept error:");
						break;
					}
					char ip[100];
					printf("process_id is %d accept client address = %s\n",process_id,inet_ntop(AF_INET,&conn_addr.sin_addr,ip,sizeof(ip)));
					addfd(epollfd,connfd);

					heap_timer<T>* timer = new heap_timer<T>(2*TIME_OUT);
					timer->cgi_conn = &users[connfd];
					timer->function = &T::removefd;

					users[connfd].init(connfd,conn_addr,epollfd,timer,min_heap);

					/*若定时器为时间堆中的第一个定时器则设置alram信号*/
					if(min_heap->empty())
					{
						alarm(2*TIME_OUT);
					}
					/*添加定时器*/
					min_heap->add_timer(timer);
				}
			}
			/*只有SIGINT信号才会使信号管道有事件发生,直接使进程结束即可*/
			else if(socketfd  == sig_pipefd[0])
			{
				char signum[BUFSIZ];
				while(true)
				{
					memset(signum,'\0',sizeof(signum));
					int ret = recv(socketfd,signum,sizeof(signum),0);
					if(ret < 0)
					{
						if(errno != EAGAIN)
							perror("sub_process sig_pipefd recv error:");
						break;
					}
					for(int i = 0;i < ret;i++)
					{
						switch(signum[i])
						{
							case SIGALRM:
							{
								timeout = true;
								break;
							}
							case SIGINT:
							{
								stop = true;
								break;
							}
						}
					}
				}
			}
			/*其他可读事件必然是客户端请求到来,调用逻辑处理对象的process方法处理即可*/
			else if(events[i].events & EPOLLIN)
			{
				printf("users process=========\n");
				users[socketfd].process();
			}
		}
		if(timeout)
		{
			timer_handler();
			timeout = false;
		}
	}

	printf("child pid = %d exit\n",getpid());

	/*向父进程发送自己的编号，以便父进程及时释放与该进程通信的管道资源*/
	send(sub_process[process_id].pipefd[0],(char*)&process_id,1,0);

	delete[]users;
	close(epollfd);
	close(sig_pipefd[0]);
	close(sig_pipefd[1]);
	close(sub_process[process_id].pipefd[0]);
}
template<typename T>
void process_pool<T>::run_parent()
{
	set_sig_pipe(true);

	addfd(epollfd,listenfd);
	/*监听与子进程通信的所有管道*/
	for(int i = 0;i < process_number;i++)
	{
		addfd(epollfd,sub_process[i].pipefd[1]);
	}

	epoll_event events[MAX_EVENT_NUMBER];
	int cnt = 0;
	int sub_process_number = process_number; 
	stop = false;
	while(!stop)
	{
		int n = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
		if(n < 0)
		{
			if(errno != EINTR)
			{
				perror("parent process epoll_wait error:");
				break;
			}
			continue;
		}
		for(int i = 0;i < n;i++)
		{
			int socketfd = events[i].data.fd;
			/*若有客户端连接请求到来,轮流选取子进程来此处理客户端请求，若无子进程，则服务终止*/
			if(socketfd == listenfd)
			{
				int num = 0;
				int j = cnt;
				do{
					j = (j + 1) % process_number;
				}while(sub_process[j].pid == -1 && j != cnt);

				if(sub_process[j].pid == -1)
				{
					stop = true;
					break;
				}
				cnt = j;
				send(sub_process[j].pipefd[1],(char*)&num,1,0);
				printf("send request to child %d\n",j);
			}
			/*处理父进程接收到的信号*/
			else if(socketfd == sig_pipefd[0])
			{
				char signals[BUFSIZ];
				while(1)
				{
					memset(signals,'\0',sizeof(signals));
					int ret = recv(socketfd,signals,sizeof(signals),0);
					if(ret == -1)
					{
						if(errno != EAGAIN)
							perror("parent process error:");
						break;
					}
					for(int i = 0;i < ret;i++)
					{
						switch(signals[i])
						{
							case SIGCHLD:
							{
								printf("parents get SIGCHLD\n");
								int stat;
								pid_t pid;
								while((pid = waitpid(-1,&stat,WNOHANG)) > 0)
								{
									sub_process_number--;
									printf("waitpid is %d\n",pid);
									/*若所有子进程退出了,则父进程也退出*/
									if(!sub_process_number)
									{
										stop = true;
									}
								}
								break;
							}
							case SIGINT:
							{
								printf("parents get SIGINT\n");

								sleep(1);
								printf("kill all the child now\n");
								//将所有子进程杀死
								for(int i  = 0;i < process_number;i++)
								{
									pid_t pid = sub_process[i].pid; 
									if(pid != -1)
									{
										printf("send kill to process pid = %d\n",pid);
										int ret = kill(pid,SIGINT);
										if(ret == -1)
										{
											perror("kill error:");
										}
									}
								}
								break;
							}
						}
					}
				}
			}
			/*若是其他事件，则必然为子进程发送的编号*/
			else if(events[i].events & EPOLLIN)
			{
				char id;
				int ret = recv(socketfd,&id,sizeof(id),0);
				if(ret < 0)
				{
					perror("parent_process_pipefd recv error:");
				}
				printf("sub_process send id = %d\n",(int)id);

				/*将子进程的pid设置为-1，以标记子进程已经退出*/
				sub_process[(int)id].pid =  -1;

				/*将管道的文件描述符从epoll中移除*/
				ret = epoll_ctl(epollfd,EPOLL_CTL_DEL,socketfd,NULL);
				if(ret == -1)
					perror("epoll_ctl error");

				/*释放与子进程通信的管道一端*/
				close(sub_process[(int)id].pipefd[1]);
			}
		}
	}
	close(epollfd);
	close(sig_pipefd[0]);
	close(sig_pipefd[1]);
}
#endif
