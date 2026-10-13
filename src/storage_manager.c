#include "storage_manager.h"
#include "gateway.h"

#include <stdio.h>
#include <unistd.h>

void *storage_manager_thread(void *arg)
{
    gateway_t *gateway = (gateway_t *)arg;

#ifdef DEBUG
    printf("[STOR] Storage Manager started\n");
#endif

    while (!gateway_should_stop(gateway)) {
        usleep(100000);
    }

#ifdef DEBUG
    printf("[STOR] Storage Manager stopped\n");
#endif

    return NULL;
}