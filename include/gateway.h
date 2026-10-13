#ifndef GATEWAY_H
#define GATEWAY_H

#include <pthread.h>
#include <signal.h>
#include <stdint.h>

typedef struct {
    uint16_t base_port;
    uint16_t http_port;

    volatile sig_atomic_t stop;

    pthread_mutex_t stop_mutex;

    pthread_t conn_thread;
    pthread_t data_thread;
    pthread_t storage_thread;
} gateway_t;

int gateway_init(
    gateway_t *gateway,
    uint16_t base_port,
    uint16_t http_port
);

int gateway_start(gateway_t *gateway);

int gateway_should_stop(gateway_t *gateway);

void gateway_request_stop(gateway_t *gateway);

void gateway_join(gateway_t *gateway);

void gateway_destroy(gateway_t *gateway);

#endif