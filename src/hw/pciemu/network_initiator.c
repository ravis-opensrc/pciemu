#include "network_initiator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

NetworkInitiator* network_initiator_new(void) {
    NetworkInitiator* initiator = (NetworkInitiator*)malloc(sizeof(NetworkInitiator));
    if (!initiator) {
        perror("Failed to allocate NetworkInitiator");
        return NULL;
    }
    initiator->fd = -1;
    initiator->is_connected = 0;
    initiator->should_exit = 0;
    initiator->transaction_list.head = NULL;
    initiator->transaction_list.tail = NULL;
    pthread_mutex_init(&initiator->transaction_list.lock, NULL);
    pthread_cond_init(&initiator->transaction_list.cond, NULL);
    pthread_create(&initiator->dispatch_thread, NULL, dispatch_thread_func, initiator);
    return initiator;
}

void network_initiator_free(NetworkInitiator* initiator) {
    if (initiator) {
        network_initiator_finish(initiator);
        pthread_mutex_destroy(&initiator->transaction_list.lock);
        pthread_cond_destroy(&initiator->transaction_list.cond);
        free(initiator);
    }
}

int network_initiator_initialize(NetworkInitiator* initiator, const char* ip, int port) {
    printf("Opening TCP socket: %s:%d\n", ip, port);

    initiator->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (initiator->fd < 0) {
        perror("Failed to create socket");
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons(port);

    if (connect(initiator->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Failed to connect to target");
        return 0;
    }

    printf("Connected to target\n");
    initiator->is_connected = 1;
    return 1;
}

int network_initiator_send_transaction(NetworkInitiator* initiator, TransactionNode* node) {
    pthread_mutex_lock(&initiator->transaction_list.lock);

    if (initiator->transaction_list.tail) {
        initiator->transaction_list.tail->next = node;
    } else {
        initiator->transaction_list.head = node;
    }
    initiator->transaction_list.tail = node;

    pthread_cond_signal(&initiator->transaction_list.cond);
    pthread_mutex_unlock(&initiator->transaction_list.lock);

    return 0;
}

void *dispatch_thread_func(void *arg) {
    NetworkInitiator *initiator = (NetworkInitiator *)arg;
    TransactionList *list = &initiator->transaction_list;

    while (1) {
        pthread_mutex_lock(&list->lock);

        while (list->head == NULL && !initiator->should_exit) {
            pthread_cond_wait(&list->cond, &list->lock);
        }

        if (initiator->should_exit) {
            pthread_mutex_unlock(&list->lock);
            break;
        }

        TransactionNode *node = list->head;
        list->head = node->next;
        if (list->head == NULL) {
            list->tail = NULL;
        }

        pthread_mutex_unlock(&list->lock);

        if (!initiator->is_connected) {
            fprintf(stderr, "Not connected to target\n");
            goto end_dispatch;
        }

        if (write(initiator->fd, &node->header, sizeof(TransactionHeader)) < 0) {
            perror("Failed to send transaction header");
            goto end_dispatch;
         }   

        if (node->header.type == READ) {
            if (read(initiator->fd, &node->header, sizeof(TransactionHeader)) < 0) {
                perror("Failed to receive read data");
                goto end_dispatch;
            }
         }

end_dispatch:
        pthread_mutex_lock(&node->mutex);
        node->completed = 1;
        pthread_cond_signal(&node->cond);
        pthread_mutex_unlock(&node->mutex);
    }

    return NULL;
}

void network_initiator_finish(NetworkInitiator* initiator) {
    if (initiator->fd >= 0) {
        close(initiator->fd);
        initiator->fd = -1;
    }
    initiator->is_connected = 0;

    pthread_mutex_lock(&initiator->transaction_list.lock);
    initiator->should_exit = 1;
    pthread_cond_signal(&initiator->transaction_list.cond);
    pthread_mutex_unlock(&initiator->transaction_list.lock);

    pthread_join(initiator->dispatch_thread, NULL);
}
