#ifndef NETWORK_INITIATOR_H
#define NETWORK_INITIATOR_H

#include "transaction.h"
#include <pthread.h>

typedef struct TransactionNode_t {
    TransactionHeader header;
    struct TransactionNode_t *next;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    int completed;
} TransactionNode;

typedef struct {
    TransactionNode *head;
    TransactionNode *tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} TransactionList;

typedef struct NetworkInitiator_t {
    int read_fd;
    int write_fd;
    int is_connected;
    int should_exit;
    pthread_t read_thread;
    pthread_t write_thread;
    TransactionList read_list;
    TransactionList write_list;
} NetworkInitiator;

NetworkInitiator* network_initiator_new(void);
void network_initiator_free(NetworkInitiator* initiator);
int network_initiator_initialize(NetworkInitiator* initiator, const char* ip, int port);
int network_initiator_send_transaction(NetworkInitiator* initiator, TransactionNode* node);
int network_initiator_send_read_transaction(NetworkInitiator* initiator, TransactionNode* node);
int network_initiator_send_write_transaction(NetworkInitiator* initiator, TransactionNode* node);
void network_initiator_finish(NetworkInitiator* initiator);

void *read_thread_func(void *arg);
void *write_thread_func(void *arg);

#endif
