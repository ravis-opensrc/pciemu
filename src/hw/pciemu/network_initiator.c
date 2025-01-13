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
    initiator->read_fd = -1;
    initiator->write_fd = -1;
    initiator->is_connected = 0;
    initiator->should_exit = 0;
    initiator->read_list.head = NULL;
    initiator->read_list.tail = NULL;
    initiator->write_list.head = NULL;
    initiator->write_list.tail = NULL;
    pthread_mutex_init(&initiator->read_list.lock, NULL);
    pthread_cond_init(&initiator->read_list.cond, NULL);
    pthread_mutex_init(&initiator->write_list.lock, NULL);
    pthread_cond_init(&initiator->write_list.cond, NULL);
    pthread_create(&initiator->read_thread, NULL, read_thread_func, initiator);
    pthread_create(&initiator->write_thread, NULL, write_thread_func, initiator);
    return initiator;
}

void network_initiator_free(NetworkInitiator* initiator) {
    if (initiator) {
        network_initiator_finish(initiator);
        pthread_mutex_destroy(&initiator->read_list.lock);
        pthread_cond_destroy(&initiator->read_list.cond);
        pthread_mutex_destroy(&initiator->write_list.lock);
        pthread_cond_destroy(&initiator->write_list.cond);
        free(initiator);
    }
}

int network_initiator_initialize(NetworkInitiator* initiator, const char* ip, int port) {

    initiator->read_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (initiator->read_fd < 0) {
        perror("Failed to create read socket");
        return 0;
    }

    initiator->write_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (initiator->write_fd < 0) {
        perror("Failed to create write socket");
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons(port);

    if (connect(initiator->read_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Failed to connect read socket to target");
        return 0;
    }

    if (connect(initiator->write_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Failed to connect write socket to target");
        return 0;
    }

    initiator->is_connected = 1;
    return 1;
}

int network_initiator_send_transaction(NetworkInitiator* initiator, TransactionNode* node) {
    if (node->header.type == READ) {
        return network_initiator_send_read_transaction(initiator, node);
    } else if (node->header.type == WRITE) {
        return network_initiator_send_write_transaction(initiator, node);
    }

    return -1;
}

int network_initiator_send_read_transaction(NetworkInitiator* initiator, TransactionNode* node) {
    pthread_mutex_lock(&initiator->read_list.lock);

    if (initiator->read_list.tail) {
        initiator->read_list.tail->next = node;
    } else {
        initiator->read_list.head = node;
    }
    initiator->read_list.tail = node;

    pthread_cond_signal(&initiator->read_list.cond);
    pthread_mutex_unlock(&initiator->read_list.lock);

    return 0;
}

int network_initiator_send_write_transaction(NetworkInitiator* initiator, TransactionNode* node) {
    pthread_mutex_lock(&initiator->write_list.lock);

    if (initiator->write_list.tail) {
        initiator->write_list.tail->next = node;
    } else {
        initiator->write_list.head = node;
    }
    initiator->write_list.tail = node;

    pthread_cond_signal(&initiator->write_list.cond);
    pthread_mutex_unlock(&initiator->write_list.lock);

    return 0;
}

void *read_thread_func(void *arg) {
    NetworkInitiator *initiator = (NetworkInitiator *)arg;
    TransactionList *list = &initiator->read_list;

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

        if (write(initiator->read_fd, &node->header, sizeof(TransactionHeader)) < 0) {
            perror("Failed to send read transaction header");
            goto end_dispatch;
        }

        if (read(initiator->read_fd, &node->header, sizeof(TransactionHeader)) < 0) {
            perror("Failed to receive read data");
            goto end_dispatch;
        }

end_dispatch:
        pthread_mutex_lock(&node->mutex);
        node->completed = 1;
        pthread_cond_signal(&node->cond);
        pthread_mutex_unlock(&node->mutex);
    }

    return NULL;
}

void *write_thread_func(void *arg) {
    NetworkInitiator *initiator = (NetworkInitiator *)arg;
    TransactionList *list = &initiator->write_list;

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

        if (write(initiator->write_fd, &node->header, sizeof(TransactionHeader)) < 0) {
            perror("Failed to send write transaction header");
            goto end_dispatch;
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
    if (initiator->read_fd >= 0) {
        close(initiator->read_fd);
        initiator->read_fd = -1;
    }
    if (initiator->write_fd >= 0) {
        close(initiator->write_fd);
        initiator->write_fd = -1;
    }
    initiator->is_connected = 0;

    pthread_mutex_lock(&initiator->read_list.lock);
    initiator->should_exit = 1;
    pthread_cond_signal(&initiator->read_list.cond);
    pthread_mutex_unlock(&initiator->read_list.lock);

    pthread_mutex_lock(&initiator->write_list.lock);
    initiator->should_exit = 1;
    pthread_cond_signal(&initiator->write_list.cond);
    pthread_mutex_unlock(&initiator->write_list.lock);

    pthread_join(initiator->read_thread, NULL);
    pthread_join(initiator->write_thread, NULL);
}
