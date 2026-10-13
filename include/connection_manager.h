#ifndef CONNECTION_MANAGER_H
#define CONNECTION_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol_adapter.h"
#include "sensor_types.h"


#define RX_BUF_SIZE 256
#define MAX_CONN 64

#define MAX_PARSE_ERRORS 5


typedef struct {
    int fd;

    const protocol_adapter_t *adapter;

    uint8_t rx[RX_BUF_SIZE];
    size_t rx_len;

    bool identified;
    sensor_id_t sensor_id;

    unsigned int parse_errors;

} connection_t;


typedef struct {
    int listen_fd[4];

    connection_t conns[MAX_CONN];

    size_t n_conns;

} connmgr_t;


void *connection_manager_thread(
    void *arg
);


#endif