#include "client.h"

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

//free all resources: cq_channel, cq, qp, id, event_channel
int on_disconnect(struct rdma_cm_id * id);

//malloc and register send and recv buffer.
void register_memory(struct rdma_cm_id * id);

//

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

	struct rdma_cm_id * conn = NULL;
	ret = rdma_create_id(channel,&conn,NULL,RDMA_PS_TCP);
	if (ret)
	{
		printf("rdma_create_id error.\n");
	}

	//resolve destination address and optional source address.
	//produce a RDMA_CM_EVENT_ADDR_RESOLVED event.
	rdma_resolve_addr(conn,NULL,addr->ai_addr,TIMEOUT_IN_MS);

	freeaddrinfo(addr);
	
	struct rdma_cm_event * event = NULL;
	while(rdma_get_cm_event(channel,&event) == 0)
	{
		struct rdma_cm_event event_copy;
		memcpy(&event_copy,event,sizeof(event_copy));

		rdma_ack_cm_event(event);
		
		if (on_event(&event_copy))
		{
			break;
		}
	}

	sleep(10);
	ret = on_disconnect(conn);

	//rdma_destroy_id(conn);

	//rdma_destroy_event_channel(channel);

	return ret;
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
	
	id->send_cq = ibv_create_cq(id->verbs,16000,NULL,id->send_cq_channel,0);
	id->recv_cq = ibv_create_cq(id->verbs,16000,NULL,id->recv_cq_channel,0);

	ibv_req_notify_cq(id->send_cq,0);
	ibv_req_notify_cq(id->recv_cq,0);

	pthread_t poll_send_thread;
	pthread_t poll_recv_thread;

	pthread_create(&poll_send_thread,NULL,poll_send_cq,id);
	pthread_create(&poll_recv_thread,NULL,poll_recv_cq,id);

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
	
	struct ibv_wc wc;

	while(true)
	{
		printf("%s: 1.\n",__func__);
		ibv_get_cq_event(id->send_cq_channel,&id->send_cq,NULL);

		ibv_ack_cq_events(id->send_cq,1);

		ibv_req_notify_cq(id->send_cq,0);
		
		int num;
		printf("%s: 2.\n",__func__);

		while(num = ibv_poll_cq(id->send_cq,1,&wc))
		{
			printf("in %s: wc->wr_id address is %p.\n",__func__,(void *)wc.wr_id);

			int ret = on_completion(&wc);
			
			printf("%s: 2.\n",__func__);

			if (ret)
			{
				break;

				printf("%s error.\n",__func__);
			}
		}
	}
}

void * poll_recv_cq(void * cm_id)
{
        printf("polling recv cq...\n");

	struct rdma_cm_id * id = (struct rdma_cm_id *)cm_id;

        struct ibv_wc wc;

        while(true)
        {
                printf("%s: 1.\n",__func__);

		ibv_get_cq_event(id->recv_cq_channel,&id->recv_cq,NULL);

                ibv_ack_cq_events(id->recv_cq,1);

                ibv_req_notify_cq(id->recv_cq,0);

                int num;
		printf("%s: 2.\n",__func__);

		while(num = ibv_poll_cq(id->recv_cq,1,&wc))
                {
                        printf("in %s: wc->wr_id address is %p.\n",__func__,(void *)wc.wr_id);

			int ret = on_completion(&wc);

                        if (ret)
                        {
                                break;

				printf("%s error.\n",__func__);
                        }
                }
        }
}

int on_completion(struct ibv_wc *wc)
{
	printf("here.\n");
	struct rdma_cm_id * id = (struct rdma_cm_id *)(wc->wr_id);
	struct context * ctx = (struct context *)(id->context);

	printf("in %s: context address is %p.\n",__func__,(void *)id->context);
	
	printf("%s: 1.\n",__func__);
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
		}
		else if (wc->opcode == IBV_WC_RECV)
		{
			printf("recv is completed.\n");

			struct message * msg_recv = (struct message *)ctx->recv_buffer;

                        printf("recv: %s.\n",msg_recv->key);

                        if (msg_recv->type == TESTOK)
                        {
                                printf("recv TESTOK is ok.\n");

                                //struct message msg_send;
                                //msg_send.type = TESTOK;
                                //char * key2 = "I recved your msg!";
                                //strcpy(msg_send.key,key2);

                                //memcpy(ctx->send_buffer,&msg_send,MSG_SIZE);

                                //send_msg(id);
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
	
	printf("in %s: context address is %p.\n",__func__,(void *)ctx);	
	
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
	free(id->context);

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

	ibv_destroy_comp_channel(id->send_cq_channel);
	if (ret)
        {
                printf("ibv_destroy_comp_channel(id->send_cq_channel) error.\n");

		return ret;
        }

	ibv_destroy_comp_channel(id->recv_cq_channel);
	if (ret)
        {
                printf("ibv_destroy_comp_channel(id->recv_cq_channel) error.\n");

                return ret;
        }

	ibv_destroy_cq(id->send_cq);
	if (ret)
	{
		printf("ibv_destroy_cq(id->send_cq) error.\n");

		return ret;
	}
	
	ibv_destroy_cq(id->recv_cq);	
	if (ret)
        {
                printf("ibv_destroy_cq(id->recv_cq) error.\n");
		return ret;
        }

	rdma_destroy_qp(id);

	rdma_destroy_id(id);
	if (ret)
	{
		printf("rdma_destroy_id(id) error.\n)");

		return ret;
	}

	rdma_destroy_event_channel(id->channel);
	
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

	printf("in %s: context address is %p.\n",__func__,(void *)ctx);

	memcpy(ctx->send_buffer,&msg_send,sizeof(struct message));
	
	int ret;

	printf("on_connection1.\n");
	ret = send_msg(id);
	printf("on_connection2.\n");

	usleep(100000);
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


	printf("send1.\n");
        sge.lkey = ctx->send_mr->lkey;
        //sge.addr = (uint64_t)ctx->send_buffer;
	sge.addr = (uintptr_t)ctx->send_buffer;
        sge.length = MSG_SIZE;
	
	printf("send2.\n");
        int rc;

	rc = ibv_post_send(id->qp,&wr,&bad_wr);
	if (rc)
	{
		printf("ibv_post_send error.\n");
	}

	printf("send3.\n");
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

	printf("recv1.\n");
        //sge.addr = (uint64_t)ctx->recv_buffer;
	sge.addr = (uintptr_t)ctx->recv_buffer;
        sge.length = MSG_SIZE;
        sge.lkey = ctx->recv_mr->lkey;
	
	printf("recv2.\n");

	int rc;

        rc = ibv_post_recv(id->qp,&wr,&bad_wr);
	if (rc)
	{
		printf("ibv_post_recv error.\n");
	}
	printf("recv3.\n");
	return rc;

}
