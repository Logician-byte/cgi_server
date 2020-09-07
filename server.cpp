#include"process_pool.h"
#include<arpa/inet.h>
/*用于处理客户CGI请求的类*/
class cgi_conn
{
	private:
		int sockfd;
		struct sockaddr_in address;
		static int epollfd;
		char buf[BUFSIZ];
		heap_timer<cgi_conn> *timer;
		time_heap<cgi_conn> *min_heap;
	public:
		void removefd()
		{
			epoll_ctl(epollfd,EPOLL_CTL_DEL,sockfd,NULL);
			close(sockfd);
			printf("close fd %d\n",sockfd);
		}
		void init(int sockfd,struct sockaddr_in &address,int epollfd,heap_timer<cgi_conn> *timer,time_heap<cgi_conn> *min_heap)
		{
			this->sockfd = sockfd;
			this->address = address;
			this->epollfd = epollfd;
			this->timer = timer;
			this->min_heap = min_heap;
			memset(buf,'\0',sizeof(buf));
		}
		void process()
		{
			while(true)
			{
				memset(buf,'\0',sizeof(buf));
				int ret = recv(sockfd,buf,sizeof(buf),0);
				if(ret < 0)
				{
					if(errno != EAGAIN)
					{
						perror("client recv error:");
						/*删除定时器*/
						min_heap->del_timer(timer);
						/*关闭连接*/
						removefd();
						timer = nullptr;
					}
					break;
				}
				/*若对端关闭，则关闭连接*/
				else if(ret == 0)
				{
					printf("conn socket close\n ");
					/*删除定时器*/
					min_heap->del_timer(timer);
					removefd();
					timer = nullptr;
					break;
				}

				int num = send(sockfd,buf,strlen(buf),0);
				if(num == -1)
				{
					perror("send error:");
					removefd();
					break;
				}
			}
			if(timer)
			{
				time_t time_out = time(NULL) + 2*TIME_OUT;
				min_heap->adjust_timer(timer,time_out);
			}
		}
};
int cgi_conn::epollfd = -1;

int main(int argc,const char* argv[])
{
	if(argc < 3)
	{
		printf("usage: %s ip_adress port_number\n",argv[0]);
		exit(1);
	}

	const char *ip = argv[1];
	int port = atoi(argv[2]);

	struct sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	inet_pton(AF_INET,ip,&address.sin_addr);

	int listenfd = socket(AF_INET,SOCK_STREAM,0);

	bind(listenfd,(struct sockaddr*)&address,sizeof(address));

	listen(listenfd,3);

	process_pool<cgi_conn>* pool = process_pool<cgi_conn>::getinstance(listenfd);
	if(pool)
	{
		pool->run();
		delete pool;
	}
	close(listenfd);
	return 0;
}
