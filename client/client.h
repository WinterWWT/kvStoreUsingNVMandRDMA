#ifndef _CLIENT_H
#define _CLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include "murmurhash.h"
#include "crc32.h"
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define TIMEOUT_IN_MS 500
#define MSG_SIZE 8192

struct context
{
	struct ibv_mr * send_mr;
	struct ibv_mr * recv_mr;
	//struct ibv_mr *
	
	char * send_buffer;
	char * recv_buffer;

	pthread_t poll_send_thread;
        pthread_t poll_recv_thread;
};

enum MSG_TYPE
{
	PUT,
	GET,

	GETHT1,
	GETHT2,
	HADDR1,
	HADDR2,

	TEST,
	TESTOK
};

struct message
{
	uint8_t type;
       	char key[32];
		
};

#endif
