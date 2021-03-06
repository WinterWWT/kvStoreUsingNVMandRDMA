#include "server.h"

struct rdma_cm_id * listener = NULL;
struct rdma_cm_event * event = NULL;
struct rdma_event_channel * channel = NULL;

void stop_manually(int signal);

//register hashtable
void register_hashTable();

int on_event(struct rdma_cm_event * event);

int on_connect_request(struct rdma_cm_id * id);

void build_qp_attr(struct ibv_qp_init_attr * qp_attr,struct rdma_cm_id * id);

void * poll_send_cq(void * cm_id);
void * poll_recv_cq(void * cm_id);

void register_memory(struct rdma_cm_id * id);

int on_completion(struct ibv_wc *wc);
int on_completion2(struct ibv_wc *wc);

int send_msg(struct rdma_cm_id * id);

int recv_msg(struct rdma_cm_id * id);

int on_connection(struct rdma_cm_id * id);

int on_disconnect(struct rdma_cm_id * id);

int main()
{
	register_hashTable();
	printf("hashtable address is %p. arrar address is %p. bucketDocker1 address is %p.\n",hashtable1,hashtable1->array,bucketDocker1);
	
	//init some variable which are needed by create connection between server and client
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
	//when port is set to 0, port is seleted randomly.
	struct sockaddr_in addr;
	memset(&addr,0,sizeof(addr));
	addr.sin_family = AF_INET;
	//addr.sin_addr.s_addr = htonl(INADDR_ANY);
	//Y7000 192.168.229.128 xiaoxin 192.168.253.164
	addr.sin_addr.s_addr = inet_addr("192.168.229.128");
	addr.sin_port = htons(45678);

	//bind an RDMA identifier to a source address
	rdma_bind_addr(listener,(struct sockaddr *)&addr);
	
	//listen for incoming connection request
	rdma_listen(listener,10);

	//get local ip address and pot
	struct sockaddr_in * serverAddr = (struct sockaddr_in *)rdma_get_local_addr(listener);
	//ip is already set to "192.168.229.128" using addr.sin_addr.s_addr
	char * ip = inet_ntoa(serverAddr->sin_addr);
	//port is already set to "45678" using addr.sin_port
	uint16_t port = ntohs(rdma_get_src_port(listener));
	printf("server's ip address is %s.\n",ip);
	printf("listening on port: %d.\n",port);

	//register target signal
        signal(SIGINT,stop_manually);
	
	//retrieves the next pending communication event.
	while(rdma_get_cm_event(channel,&event) == 0)
	{
		//successful get and ack should be one-to-one correspondence.
		struct rdma_cm_event event_copy;
		memcpy(&event_copy,event,sizeof(*event));
		rdma_ack_cm_event(event);
		
		//on_event hold on the retrieved event.
		if(on_event(&event_copy))
		{
			break;
		}
	}
	
	//deallocate a communication identifier
	rdma_destroy_id(listener);

	//destroy a event channel
	rdma_destroy_event_channel(channel);

	printf("here.\n");
	
	return 0;
}

void stop_manually(int signal)
{
	free(hashtable1);
	free(bucketDocker1);

	printf("\nfree hashtable.\n");	
	//deallocate a communication identifier
	rdma_destroy_id(listener);

        //destroy a event channel
        rdma_destroy_event_channel(channel);
	printf("destroy resource.\n");

	exit(0);
}

void register_hashTable()
{
	bucketDocker1 = (struct bucket *)calloc(HASHTABLESIZE,sizeof(struct bucket));
	hashtable1 = (struct hashTable *)malloc(sizeof(struct hashTable));
	//ibv_reg_mr();
        hashtable1->size = 10;
        hashtable1->capacity = HASHTABLESIZE;
        hashtable1->array = bucketDocker1;
}

int on_event(struct rdma_cm_event * event)
{
	//printf("event type: %s.\n",rdma_event_str(event->event));

	int ret = 0;

	if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
	{
		printf("connect request.\n");

		ret = on_connect_request(event->id);
	}
	else if(event->event == RDMA_CM_EVENT_ESTABLISHED)
	{
		printf("connect established.\n");

		ret = on_connection(event->id);
	}
	else if(event->event == RDMA_CM_EVENT_DISCONNECTED)
	{
		printf("disconnected.\n");

		ret = on_disconnect(event->id);
	}
	else
	{
		printf("on_event: unknown event.\n");
	}

	return ret;
}

int on_connect_request(struct rdma_cm_id * id)
{
	id->pd = ibv_alloc_pd(id->verbs);

	id->context = (void *)malloc(sizeof(struct context));
	register_memory(id);

	id->send_cq_channel = ibv_create_comp_channel(id->verbs);
        id->recv_cq_channel = ibv_create_comp_channel(id->verbs);

        id->send_cq = ibv_create_cq(id->verbs,16000,id,id->send_cq_channel,0);
        id->recv_cq = ibv_create_cq(id->verbs,16000,id,id->recv_cq_channel,0);

        ibv_req_notify_cq(id->send_cq,0);
        ibv_req_notify_cq(id->recv_cq,0);

	struct context * ctx = (struct context *)id->context;
        pthread_create(&ctx->poll_send_thread,NULL,poll_send_cq,id);
        pthread_create(&ctx->poll_recv_thread,NULL,poll_recv_cq,id);

	struct ibv_qp_init_attr qp_attr;
        build_qp_attr(&qp_attr,id);

        //allocate a qp
        rdma_create_qp(id,id->pd,&qp_attr);

	struct rdma_conn_param params;

	memset(&params,0,sizeof(params));

	recv_msg(id);
	rdma_accept(id,&params);
}

void build_qp_attr(struct ibv_qp_init_attr * qp_attr,struct rdma_cm_id * id)
{
        memset(qp_attr, 0, sizeof(*qp_attr));

        qp_attr->send_cq = id->send_cq;
        qp_attr->recv_cq = id->recv_cq;
        qp_attr->qp_type = IBV_QPT_RC;

        qp_attr->cap.max_send_wr = 16000;
        qp_attr->cap.max_recv_wr = 16000;
        qp_attr->cap.max_send_sge = 1;
        qp_attr->cap.max_recv_sge = 1;

        qp_attr->sq_sig_all = 0;
}

void * poll_send_cq(void * cm_id)
{
        printf("pollind send cq...\n");

        struct rdma_cm_id * id = (struct rdma_cm_id *)cm_id;

	struct ibv_cq * cq;
        struct ibv_wc wc;
	void * tar;

        while(true)
        {
                ibv_get_cq_event(id->send_cq_channel,&cq,&tar);

                ibv_ack_cq_events(cq,1);

                ibv_req_notify_cq(cq,0);

               	while(ibv_poll_cq(cq,1,&wc))
                {
                        int ret = on_completion(&wc);

                        if (ret)
                        {
                                break;
                        }
                }
        }

	return NULL;
}

void * poll_recv_cq(void * cm_id)
{
        printf("polling recv cq...\n");

        struct rdma_cm_id * id = (struct rdma_cm_id *)cm_id;

	struct ibv_cq * cq;
        struct ibv_wc wc;
	void * tar;

        while(true)
        {
                ibv_get_cq_event(id->recv_cq_channel,&cq,&tar);

                ibv_ack_cq_events(cq,1);

                ibv_req_notify_cq(cq,0);

                while(ibv_poll_cq(cq,1,&wc))
                {
                        int ret = on_completion(&wc);

                        if (ret)
                        {
                                break;
                        }
                }
        }

	return NULL;
}

void register_memory(struct rdma_cm_id * id)
{
	struct context * ctx = (struct context *)id->context;
	             
	ctx->send_buffer = (char *)malloc(MSG_SIZE);
        ctx->recv_buffer = (char *)malloc(MSG_SIZE);

        ctx->send_mr = ibv_reg_mr(id->pd,ctx->send_buffer,MSG_SIZE,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
        ctx->recv_mr = ibv_reg_mr(id->pd,ctx->recv_buffer,MSG_SIZE,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);

	ctx->hTable = hashtable1;
	ctx->bDocker = bucketDocker1;

	ctx->read_mr = ibv_reg_mr(id->pd,ctx->hTable,sizeof(struct hashTable),IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
        ctx->write_mr = ibv_reg_mr(id->pd,ctx->bDocker,HASHTABLESIZE * sizeof(struct bucket),IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
}

int on_completion(struct ibv_wc *wc)
{
        struct rdma_cm_id * id = (struct rdma_cm_id *)wc->wr_id;
	struct context * ctx = (struct context *)(id->context);

	if (wc->status != IBV_WC_SUCCESS)
        {
                printf("opcode: %d.\n",wc->opcode);
                return -1;
        }
        else
        {
                if(wc->opcode == IBV_WC_SEND)
                {
                        printf("send is completed.\n");
			
			struct message * msg_send = (struct message *)ctx->send_buffer;

			printf("send: %s.\n",msg_send->key);
			if (msg_send->type == HADDR1)
			{
				printf("hashtable address is %p.\n",(void *)(msg_send->hTable));
				printf("hTable_rkey is %u.\n",msg_send->hTable_rkey);
				
				printf("bucketDocker address is %p.\n",(void *)(msg_send->bDocker));
				printf("bDocker_rkey is %u.\n",msg_send->bDocker_rkey);
			}
                }
                else if (wc->opcode == IBV_WC_RECV)
                {
                        printf("recv is completed.\n");
			
			struct message * msg_recv = (struct message *)ctx->recv_buffer;
			
			printf("recv: %s.\n",msg_recv->key);
			
			if (msg_recv->type == TEST)
			{
				printf("recv TEST is ok.\n");

				struct message msg_send;
				msg_send.type = TESTOK;
				char * key = "I recved your msg!";
				strcpy(msg_send.key,key);

				memcpy(ctx->send_buffer,&msg_send,sizeof(struct message));
				
				send_msg(id);
			}
			else if (msg_recv->type == GETHT1)
			{
				struct message msg_send;
                                
				msg_send.type = HADDR1;
                                char * key = "give you address of hashtable.";
                                strcpy(msg_send.key,key);
				
				msg_send.hTable = (uintptr_t)(ctx->hTable);
			       	msg_send.bDocker = (uintptr_t)(ctx->bDocker);
				msg_send.hTable_rkey = ctx->read_mr->rkey;
				msg_send.bDocker_rkey = ctx->write_mr->rkey;

                                memcpy(ctx->send_buffer,&msg_send,sizeof(struct message));

                                send_msg(id);
			}
                }

                else if (wc->opcode == IBV_WC_RDMA_READ)
                {
                        printf("read is completed.\n");
                }

                else if (wc->opcode == IBV_WC_RDMA_WRITE)
                {
                        printf("write is completed.\n");
                }

        }

        return 0;
}

int send_msg(struct rdma_cm_id * id)
{
        struct ibv_send_wr wr;
        struct ibv_send_wr * bad_wr = NULL;
        struct ibv_sge sge;
        struct context * ctx = (struct context *)(id->context);

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t)id;
        wr.opcode = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_SIGNALED;

	sge.lkey = ctx->send_mr->lkey;
	sge.addr = (uintptr_t)ctx->send_buffer;
        //sge.length = MSG_SIZE;
	sge.length = sizeof(struct message);

        int rc;

        rc = ibv_post_send(id->qp,&wr,&bad_wr);
        if (rc)
        {
                printf("ibv_post_send error.\n");
        }

        recv_msg(id);

        return rc;
}

int recv_msg(struct rdma_cm_id * id)
{
        struct ibv_recv_wr wr;
        struct ibv_recv_wr * bad_wr = NULL;
        struct ibv_sge sge;
        struct context * ctx = (struct context *)(id->context);

        memset(&wr,0,sizeof(wr));
        wr.wr_id = (uintptr_t)id;
	wr.next = NULL;
        wr.sg_list = &sge;
        wr.num_sge = 1;

	sge.addr = (uintptr_t)ctx->recv_buffer;
        //sge.length = MSG_SIZE;
	sge.length = sizeof(struct message);
        sge.lkey = ctx->recv_mr->lkey;

        int rc;

        rc = ibv_post_recv(id->qp,&wr,&bad_wr);
        if (rc)
        {
                printf("ibv_post_recv error.\n");
        }

        return rc;
}

int on_connection(struct rdma_cm_id * id)
{
	int ret = 0;

	return ret;	
}

int on_disconnect(struct rdma_cm_id * id)
{
	int ret = 0;

	struct context * ctx = (struct context *)id->context;
        free(ctx->send_buffer);
        free(ctx->recv_buffer);
        ctx->send_buffer = NULL;
        ctx->recv_buffer = NULL;

	ret = ibv_dereg_mr(ctx->send_mr);
        if (ret)
        {
                printf("ibv_dereg_mr(ctx->send_mr) error");
                return ret;
        }

        ibv_dereg_mr(ctx->recv_mr);
        if (ret)
        {
                printf("ibv_dereg_mr(ctx->send_mr) error");
                return ret;
        }

	free(id->context);

        rdma_destroy_qp(id);
	rdma_destroy_id(id);

	return ret;
}	
