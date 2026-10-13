#include <stdio.h>

#include "storage_manager.h"
#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"


void *storage_manager_thread(void *arg)
{
    gateway_t *gateway = (gateway_t *)arg;

#ifdef DEBUG
    printf("[STOR] Storage Manager started\n");
#endif

    logger_write(
        LOG_STOR,
        "Storage Manager started"
    );

    sensor_data_t data;

    while (sbuffer_read(
               &gateway->sbuf,
               SBUFFER_READER_STORAGE,
               &data
           ) == 0) {

#ifdef DEBUG
        printf(
            "[STOR] id=%u type=%d value=%.2f ts=%ld\n",
            data.id,
            data.type,
            data.value,
            (long)data.ts
        );
#endif
    }

#ifdef DEBUG
    printf("[STOR] Storage Manager stopped\n");
#endif

    return NULL;
}