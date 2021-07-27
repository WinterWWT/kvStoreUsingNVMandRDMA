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
#define KEYSIZE 20
#define HASHTABLESIZE 20000

struct bucket
{
        char key[KEYSIZE];
        void * valuePtr;
        int valueLen;
};

struct bucket * bucketDocker1 = NULL;

struct hashTable
{
        int size;
        int capacity;
        struct bucket * array;
};

struct hashTable * hashtable1 = NULL;

struct context
{
	struct ibv_mr * send_mr;
	struct ibv_mr * recv_mr;
	//struct ibv_mr *
	
	char * send_buffer;
	char * recv_buffer;

	struct hashTable * hTable;

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
	char * address;
};

#endif
