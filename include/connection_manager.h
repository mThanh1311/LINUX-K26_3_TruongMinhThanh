#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensor_types.h"

#define RX_BUF_SIZE 256
#define MAX_CONN 64

typedef struct {
    int fd;

    uint8_t rx[RX_BUF_SIZE];
    size_t rx_len;

    bool identified;
    sensor_id_t sensor_id;
} connection_t;

void *connection_manager_thread(void *arg);

#endif