#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <stdint.h>

#define SERVER_PORT 5555

typedef enum TransactionType_t {
    READ,
    WRITE
}TransactionType;

typedef struct TransactionHeader_t {
    TransactionType type;
    uint64_t address;
    uint32_t data;
}TransactionHeader;

#endif
