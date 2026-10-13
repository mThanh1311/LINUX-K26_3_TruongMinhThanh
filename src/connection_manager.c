#include <stdio.h>
#include <unistd.h>

#include "connection_manager.h"
#include "gateway.h"
#include "logger.h"

void *connection_manager_thread(void *arg)
{
    gateway_t *gateway = (gateway_t *)arg;

#ifdef DEBUG
    printf("[CONN] Connection Manager started\n");
#endif

    logger_write(
        LOG_CONN,
        "Connection Manager started"
    );

    while (!gateway_should_stop(gateway)) {
        usleep(100000);
    }

#ifdef DEBUG
    printf("[CONN] Connection Manager stopped\n");
#endif

    return NULL;
}