#include "gateway.h"

#include "connection_manager.h"
#include "data_manager.h"
#include "storage_manager.h"

#include <stdio.h>
#include <string.h>

int gateway_init(
    gateway_t *gateway,
    uint16_t base_port,
    uint16_t http_port
)
{
    if (gateway == NULL) {
        return -1;
    }

    memset(gateway, 0, sizeof(*gateway));

    gateway->base_port = base_port;
    gateway->http_port = http_port;
    gateway->stop = 0;

    if (pthread_mutex_init(
            &gateway->stop_mutex,
            NULL
        ) != 0) {

        return -1;
    }

    if (sbuffer_init(
            &gateway->sbuf
        ) != 0) {

        pthread_mutex_destroy(
            &gateway->stop_mutex
        );

        return -1;
    }

    return 0;
}

int gateway_start(gateway_t *gateway)
{
    int ret;

    ret = pthread_create(
        &gateway->conn_thread,
        NULL,
        connection_manager_thread,
        gateway
    );

    if (ret != 0) {
        fprintf(stderr, "Failed to create Connection Manager thread\n");
        return -1;
    }

    ret = pthread_create(
        &gateway->data_thread,
        NULL,
        data_manager_thread,
        gateway
    );

    if (ret != 0) {
        fprintf(stderr, "Failed to create Data Manager thread\n");
        return -1;
    }

    ret = pthread_create(
        &gateway->storage_thread,
        NULL,
        storage_manager_thread,
        gateway
    );

    if (ret != 0) {
        fprintf(stderr, "Failed to create Storage Manager thread\n");
        return -1;
    }

    return 0;
}

int gateway_should_stop(gateway_t *gateway)
{
    int stop;

    pthread_mutex_lock(&gateway->stop_mutex);

    stop = gateway->stop;

    pthread_mutex_unlock(&gateway->stop_mutex);

    return stop;
}

void gateway_request_stop(gateway_t *gateway)
{
    if (gateway == NULL) {
        return;
    }

    pthread_mutex_lock(&gateway->stop_mutex);

    gateway->stop = 1;

    pthread_mutex_unlock(&gateway->stop_mutex);
}

void gateway_join(gateway_t *gateway)
{
    if (gateway == NULL) {
        return;
    }

    pthread_join(gateway->conn_thread, NULL);
    pthread_join(gateway->data_thread, NULL);
    pthread_join(gateway->storage_thread, NULL);
}

void gateway_destroy(gateway_t *gateway)
{
    if (gateway == NULL) {
        return;
    }

    sbuffer_destroy(&gateway->sbuf);

    pthread_mutex_destroy(
        &gateway->stop_mutex
    );
}