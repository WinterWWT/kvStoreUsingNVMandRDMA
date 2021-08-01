#include "client.h"

struct rdma_cm_id * conn = NULL;

void stop_manually(int signal);

//start after retrieving an event.
int on_event(struct rdma_cm_event * event);

//after retrieving an event whose type is RDMA_CM_EVENT_ADDR_RESOLVED.
//rdma_resolve_route()
int on_addr_resolved(struct rdma_cm_id * id);

//set up qp attribute.
void build_qp_attr(struct ibv_qp_init_attr * qp_attr,struct rdma_cm_id * id);

//poll send wce and handle it
void * poll_send_cq(void * id);

//poll recv wce and handle it
void * poll_recv_cq(void * id);

//handle completion queue, process WCE according to wc->opcode
int on_completion(struct ibv_wc *wc);

//after retrieving an event whose type is RDMA_CM_ROUTE_RESOLVED.
//rdma_connect();
int on_route_resolved(struct rdma_cm_id * id);

//after retrieving an event whose type is RDMA_CM_EVENT_ESTABLISHED.
//ibv_post_send() + ibv_post_recv()
int on_connection(struct rdma_cm_id * id);

//ibv_post_send()
int send_msg(struct rdma_cm_id * id);

//ibv_post_recv()
int recv_msg(struct rdma_cm_id * id);

//read
int read_remote(struct rdma_cm_id * id);

//write
int write_remote(struct rdma_cm_id * id);

//free all resources: cq_channel, cq, qp, id, event_channel
int on_disconnect(struct rdma_cm_id * id);

//malloc and register send and recv buffer.
void register_memory(struct rdma_cm_id * id);

int main(int argc,char ** argv)
{
	if (argc != 3)
	{
		printf("Usage: ./client ip port.\n");
	}
		
	struct addrinfo * addr;
	int ret = 0;

	//get ip and port from argv
	getaddrinfo(argv[1],argv[2],NULL,&addr);
	
	struct rdma_event_channel * channel = NULL;
	channel = rdma_create_event_channel();
	
	if (!channel)
	{
		printf("rdma_create_event_channel error.\n");
		return -1;
	}

	//struct rdma_cm_id * conn = NULL;
	ret = rdma_create_id(channel,&conn,NULL,RDMA_PS_TCP);
	
	if (ret)
	{
		printf("rdma_create_id error.\n");
	}

	//resolve destination address and optional source address.
	//produce a RDMA_CM_EVENT_ADDR_RESOLVED event.
	rdma_resolve_addr(conn,NULL,addr->ai_addr,TIMEOUT_IN_MS);

	freeaddrinfo(addr);

	signal(SIGINT,stop_manually);

	struct rdma_cm_event * event = NULL;
	//int i_c =0;
	while(rdma_get_cm_event(channel,&event) == 0)
	{
		struct rdma_cm_event event_copy;
		
		memcpy(&event_copy,event,sizeof(event_copy));

		rdma_ack_cm_event(event);

		if (on_event(&event_copy))
		{
			break;
		}
		
		//printf("%d.\n",++i_c);
	}
	
	rdma_destroy_event_channel(channel);

	return ret;
}

void stop_manually(int signal)
{
	int ret = 0;
	ret = rdma_disconnect(conn);
	
	if (ret)
	{
		printf("rdma_disconnect(conn).\n");
	}

	_exit(0);
}

int on_event(struct rdma_cm_event * event)
{
        int r = 0;

        if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED)
        {
                printf("addr resolved.\n");
             
                r = on_addr_resolved(event->id);
        }
        else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED)
        {
        	printf("route resolved.\n");

		r = on_route_resolved(event->id);
        }
	else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
	{
		printf("connect established.\n");
		
		r = on_connection(event->id);
	}
	else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
	{
		printf("disconnected.\n");

		r = on_disconnect(event->id);
	}
	else
	{
		printf("undefined event_type: %s. error.\n",rdma_event_str(event->event));
		r = -1;
	}

	return r;
}

int on_addr_resolved(struct rdma_cm_id * id)
{
	//alloc a protection domin
	//ibv_context * verbs is produced by ibv_open_devices().
	id->pd = ibv_alloc_pd(id->verbs);	

	id->context = (void *)malloc(sizeof(struct context));
        register_memory(id);	

	id->send_cq_channel = ibv_create_comp_channel(id->verbs);
	id->recv_cq_channel = ibv_create_comp_channel(id->verbs);
	
	id->send_cq = ibv_create_cq(id->verbs,16,NULL,id->send_cq_channel,0);	//16000
	id->recv_cq = ibv_create_cq(id->verbs,16,NULL,id->recv_cq_channel,0);	//16000

	ibv_req_notify_cq(id->send_cq,0);
	ibv_req_notify_cq(id->recv_cq,0);

	struct context * ctx = (struct context *)id->context;
	pthread_create(&ctx->poll_send_thread,NULL,poll_send_cq,id);
	pthread_create(&ctx->poll_recv_thread,NULL,poll_recv_cq,id);

	struct ibv_qp_init_attr qp_attr;
	build_qp_attr(&qp_attr,id);

	//allocate a qp
	rdma_create_qp(id,id->pd,&qp_attr);
	
	recv_msg(id);
	
	//resolve the route information needed to establish a connection.
	//produce a RDMA_CM_EVENT_ROUTE_RESOLVED event.
	rdma_resolve_route(id,TIMEOUT_IN_MS);
	

	return 0;
}

void build_qp_attr(struct ibv_qp_init_attr * qp_attr,struct rdma_cm_id * id)
{
	memset(qp_attr, 0, sizeof(*qp_attr));
	
	qp_attr->send_cq = id->send_cq;
        qp_attr->recv_cq = id->recv_cq;
        qp_attr->qp_type = IBV_QPT_RC;
	
	qp_attr->cap.max_send_wr = 16;	//16000
	qp_attr->cap.max_recv_wr = 16;	//16000
        qp_attr->cap.max_send_sge = 1;
        qp_attr->cap.max_recv_sge = 1;
        
	qp_attr->sq_sig_all = 0;
}

void * poll_send_cq(void * cm_id)
{
	printf("pollind send cq...\n");

	struct rdma_cm_id * id = (struct rdma_cm_id *)cm_id;
	
	struct ibv_wc wc;
	struct ibv_cq * cq;
	void * tar;

	while(true)
	{
		//ibv_get_cq_event(id->send_cq_channel,&id->send_cq,&cm_id);
		ibv_get_cq_event(id->send_cq_channel,&cq,&tar);

		//ibv_ack_cq_events(id->send_cq,1);
		ibv_ack_cq_events(cq,1);

		//ibv_req_notify_cq(id->send_cq,0);
		ibv_req_notify_cq(cq,0);
		
		int num;

		//int i_send;
		//while(num = ibv_poll_cq(id->send_cq,1,&wc))
		while(num = ibv_poll_cq(cq,1,&wc))
		{
			int ret = on_completion(&wc);
			//printf("num is %d.\n",num);
			//printf("i_send: %d.ret is %d.\n",++i_send,ret);
			
			if (ret)
			{
				break;
			}
		}
	}
	printf("never.\n");

	return NULL;
}

void * poll_recv_cq(void * cm_id)
{
        printf("polling recv cq...\n");

	struct rdma_cm_id * id = (struct rdma_cm_id *)cm_id;

        struct ibv_wc wc;
	struct ibv_cq * cq;
	void * tar;

        while(true)
        {
		//ibv_get_cq_event(id->recv_cq_channel,&id->recv_cq,&cm_id);
		ibv_get_cq_event(id->recv_cq_channel,&cq,&tar);

                //ibv_ack_cq_events(id->recv_cq,1);
		ibv_ack_cq_events(cq,1);

                //ibv_req_notify_cq(id->recv_cq,0);
		ibv_req_notify_cq(cq,0);

                int num;

		//int i_recv;
		//while(num = ibv_poll_cq(id->recv_cq,1,&wc))
		while(num = ibv_poll_cq(cq,1,&wc))
                {
			int ret = on_completion(&wc);
			//printf("i_recv: %d.ret is %d.\n",++i_recv,ret);

                        if (ret)
                        {
                                break;
                        }
                }
        }

	return NULL;
}

int on_completion(struct ibv_wc *wc)
{
	struct rdma_cm_id * id = (struct rdma_cm_id *)(wc->wr_id);
	struct context * ctx = (struct context *)(id->context);

	//printf("opcode: %d.\n",wc->opcode);
	if (wc->status != IBV_WC_SUCCESS)
	{
		printf("failed, opcode: %d.\n",wc->opcode);
		return -1;
	}
	else
	{
		if(wc->opcode == IBV_WC_SEND)
		{
			printf("send is completed. opcode is %d.\n",wc->opcode);
			
			struct message * msg_send = (struct message *)ctx->send_buffer;
			
			printf("send: %s.\n",msg_send->key);
		}
		else if (wc->opcode == IBV_WC_RECV)
		{
			printf("recv is completed. opcode is %d.\n",wc->opcode);

			struct message * msg_recv = (struct message *)ctx->recv_buffer;

                        printf("recv: %s.\n",msg_recv->key);

                        if (msg_recv->type == TESTOK)
                        {
                                printf("recv TESTOK is ok.\n");

				struct message msg_send;
                                msg_send.type = GETHT1;
                                char * key = "give me hashtable address.";
                        	strcpy(msg_send.key,key);

                        	memcpy(ctx->send_buffer,&msg_send,sizeof(struct message));

                        	send_msg(id);
			}

			if (msg_recv->type == HADDR1)
			{
				hashtable1 = (struct hashTable *)(msg_recv->hTable);
				ctx->hTable = hashtable1;
				printf("hashtable address is %p.\n",(void *)ctx->hTable);

				ctx->hTable_rkey = msg_recv->hTable_rkey;

				printf("hTable_rkey is %u.\n",ctx->hTable_rkey);

				bucketDocker1 = (struct bucket *)(msg_recv->bDocker);
				ctx->bDocker = bucketDocker1;
				printf("bucketDocker address is %p.\n",(void *)ctx->bDocker);
				
				ctx->bDocker_rkey = msg_recv->bDocker_rkey;

				printf("bDocker_rkey is %u.\n",ctx->bDocker_rkey);
				
				char * key = "user6284781860667377211";
				ctx->offset = (murmurhash(key,strlen(key),SEED) % HASHTABLESIZE) * sizeof(struct bucket);
				ctx->len = sizeof(struct bucket);
				struct bucket writeBuck;
				strcpy(writeBuck.key,key);
				writeBuck.valuePtr = NULL;
				writeBuck.valueLen = 100;
				memcpy(ctx->send_buffer,&writeBuck,sizeof(struct bucket));
				
				write_remote(id);

				//printf("write 1.\n");
				
				//rdma_disconnect(conn);
			}

		}
		else if (wc->opcode == IBV_WC_RDMA_READ)
		{  
			printf("read is completed. opcode is %d.\n",wc->opcode);

			struct bucket * readBucket = (struct bucket *)ctx->recv_buffer;
			printf("read: %s. valuesize is %d.\n",readBucket->key,readBucket->valueLen);

			rdma_disconnect(conn);
		}
		else if (wc->opcode == IBV_WC_RDMA_WRITE)
		{
			printf("write is completed. opcode is %d.\n",wc->opcode);

			struct bucket * writeBucket = (struct bucket *)ctx->send_buffer;
			printf("write: %s.\n",writeBucket->key);

			if(writeBucket->valueLen == 200)
                        {
				//printf("111.\n");
                                char * key = "user8517097267634966620";
                                ctx->offset = (murmurhash(key,strlen(key),SEED) % HASHTABLESIZE) * sizeof(struct bucket);
                                ctx->len = sizeof(struct bucket);

                                read_remote(id);
                        }
			
			if (writeBucket->valueLen == 100)
			{
				//printf("write 2.\n");
				char * key2 = "user8517097267634966620";
                        	ctx->offset = (murmurhash(key2,strlen(key2),SEED) % HASHTABLESIZE) * sizeof(struct bucket);
                        	ctx->len = sizeof(struct bucket);
                        	struct bucket writeBuck2;
                        	strcpy(writeBuck2.key,key2);
                        	writeBuck2.valuePtr = NULL;
                        	writeBuck2.valueLen = 200;
                       		memcpy(ctx->send_buffer,&writeBuck2,sizeof(struct bucket));

                        	write_remote(id);
			}
		}
	
	}
	
	return 0;
}

int on_route_resolved(struct rdma_cm_id * id)
{
	struct rdma_conn_param param;
	memset(&param,0,sizeof(param));

	param.initiator_depth = param.responder_resources = 1;
	param.rnr_retry_count = 7;

	//initiate an connection request.
	rdma_connect(id,&param);

	return 0;
}

void register_memory(struct rdma_cm_id * id)
{
	struct context * ctx = (struct context *)id->context;
	
	ctx->send_buffer = (char *)malloc(MSG_SIZE);
	ctx->recv_buffer = (char *)malloc(MSG_SIZE);

	ctx->send_mr = ibv_reg_mr(id->pd,ctx->send_buffer,MSG_SIZE,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!ctx->send_mr)
	{
		printf("ibv_reg_mr send error.\n");
	}

	ctx->recv_mr = ibv_reg_mr(id->pd,ctx->recv_buffer,MSG_SIZE,IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!ctx->send_mr)
        {
                printf("ibv_reg_mr send error.\n");
        }

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

	ret = ibv_dereg_mr(ctx->recv_mr);
	if (ret)
   	{
		printf("ibv_dereg_mr(ctx->send_mr) error");
		return ret;
	}
	
	rdma_destroy_qp(id); //maybe conn not id	
	
	//doesn't need ibv_destroy_cq();
	//doesn't need ibv_destroy_comp_channel();

	rdma_destroy_id(id);
	if (ret)
	{
		printf("rdma_destroy_id(id) error.\n)");

		return ret;
	}
	
	ret = 1;
	return ret;
}

int on_connection(struct rdma_cm_id * id)
{
	//TEST
	struct message msg_send;
	msg_send.type = TEST;
	char * key2 = "Are you ready to recv msg?";
	strcpy(msg_send.key,key2);
	
	struct context * ctx = (struct context *)id->context;

	memcpy(ctx->send_buffer,&msg_send,sizeof(struct message));
	
	int ret;

	ret = send_msg(id);

	return ret;
}

int send_msg(struct rdma_cm_id * id)
{
	struct ibv_send_wr wr;
	struct ibv_send_wr * bad_wr = NULL;
	struct ibv_sge sge;
	struct context * ctx = (struct context *)id->context;
	
	memset(&wr, 0, sizeof(wr));
        //wr.wr_id = (uint64_t)id;
	wr.wr_id = (uintptr_t)id;
        wr.opcode = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_SIGNALED;

        sge.lkey = ctx->send_mr->lkey;
        //sge.addr = (uint64_t)ctx->send_buffer;
	sge.addr = (uintptr_t)ctx->send_buffer;
        sge.length = MSG_SIZE;
	
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
        //wr.wr_id = (uint64_t)id;
	wr.wr_id = (uintptr_t)id;
        wr.next = NULL;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        //sge.addr = (uint64_t)ctx->recv_buffer;
	sge.addr = (uintptr_t)ctx->recv_buffer;
        sge.length = MSG_SIZE;
        sge.lkey = ctx->recv_mr->lkey;
	
	int rc;

        rc = ibv_post_recv(id->qp,&wr,&bad_wr);
	if (rc)
	{
		printf("ibv_post_recv recv() error.\n");
	}

	return rc;
}

int read_remote(struct rdma_cm_id * id)
{
        struct ibv_send_wr wr;
        struct ibv_send_wr *bad_wr = NULL;
        struct ibv_sge sge;
        struct context *ctx = (struct context *)(id->context);
	//memset(ctx->recv_buffer,0,sizeof(ctx->recv_buffer));
	
	printf("in %s, 1.\n",__func__);
        memset(&wr,0,sizeof(wr));
        wr.wr_id = (uintptr_t)id;
        wr.opcode = IBV_WR_RDMA_READ;
        wr.send_flags = IBV_SEND_SIGNALED;

        wr.wr.rdma.remote_addr = (uintptr_t)(ctx->bDocker) + ctx->offset;
        wr.wr.rdma.rkey = ctx->bDocker_rkey;

	printf("in %s, 2.\n",__func__);
	wr.sg_list = &sge;
	wr.num_sge = 1;
	sge.addr = (uintptr_t)ctx->recv_buffer;
	sge.length = ctx->len;
	sge.lkey = ctx->recv_mr->lkey;

	int rc;

	printf("in %s, 3.\n",__func__);
	rc = ibv_post_send(id->qp,&wr,&bad_wr);
	//printf("rc is %d.\n",rc);
	if (rc)
        {
                printf("ibv_post_send read() error.\n");
        }

	printf("in %s, 4.\n",__func__);

	return rc;
}

int write_remote(struct rdma_cm_id * id)
{
        struct ibv_send_wr wr;
        struct ibv_send_wr *bad_wr = NULL;
        struct ibv_sge sge;
        struct context *ctx = (struct context *)(id->context);

        memset(&wr,0,sizeof(wr));
        wr.wr_id = (uintptr_t)id;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.send_flags = IBV_SEND_SIGNALED;

        wr.wr.rdma.remote_addr = (uintptr_t)(ctx->bDocker) + ctx->offset;
        wr.wr.rdma.rkey = ctx->bDocker_rkey;

        wr.sg_list = &sge;
        wr.num_sge = 1;
        sge.addr = (uintptr_t)ctx->send_buffer;
        sge.length = ctx->len;
        sge.lkey = ctx->send_mr->lkey;

        int rc;

        rc = ibv_post_send(id->qp,&wr,&bad_wr);
        if (rc)
        {
                printf("ibv_post_send write() error.\n");
        }

        return rc;
}


