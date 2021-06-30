#include "server.h"

int main()
{
	//init some variable which are needed by create connection between server and client
	struct rdma_event_channel * channel = NULL;
	struct rdma_cm_id * listener = NULL;
	int rl = 0;
	
	//create a event channel
	channel = rdma_create_event_channel();
	if (channel == NULL)
	{
		printf("rdma_create_event_channel error!.\n");
		
		return -1;
	}

	//allocate a communication identifier 
	//rdma_port_space: RDMA_PS_IPOIB,RDMA_PS_TCP,RDMA_PS_UDP,RDMA_PS_IB
	rl = rdma_create_id(channel,&listener,NULL,RDMA_PS_TCP);
	if (rl)
	{
		printf("rdma_create_id error!.\n");

		return -1;
	}
	
	//set server'address which is expressed as struct sockaddr_in.
	struct sockaddr_in addr;
	memset(&addr,0,sizeof(addr));
	addr.sin_family = AF_INET;
	//addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_addr.s_addr = inet_addr("192.168.229.128");

	//bind an RDMA identifier to a source address
	rdma_bind_addr(listener,(struct sockaddr *)&addr);
	
	//listen for incoming connection request
	rdma_listen(listener,10);

	//get local ip address and pot
	struct sockaddr_in * serverAddr = (struct sockaddr_in *)rdma_get_local_addr(listener);
	char * ip = inet_ntoa(serverAddr->sin_addr);
	uint16_t port = ntohs(rdma_get_src_port(listener));

	printf("server's ip address is %s.\n",ip);
	printf("listening on port: %d.\n",port);

	printf("test.\n");

	//deallocate a communication identifier
	rdma_destroy_id(listener);

	//destroy a event channel
	rdma_destroy_event_channel(channel);
	return 0;
}
