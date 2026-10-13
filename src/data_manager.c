#include <stdio.h>
#include <unistd.h>

#include "data_manager.h"
#include "gateway.h"
#include "logger.h"

void *data_manager_thread(void *arg)
{
    gateway_t *gateway = (gateway_t *)arg;

#ifdef DEBUG
    printf("[DATA] Data Manager started\n");
#endif

    logger_write(
        LOG_DATA,
        "Data Manager started"
    );


    while (!gateway_should_stop(gateway)) {
        usleep(100000);
    }

#ifdef DEBUG
    printf("[DATA] Data Manager stopped\n");
#endif

    return NULL;
}