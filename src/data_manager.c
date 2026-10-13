#include <stdio.h>

#include "data_manager.h"
#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"


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

    sensor_data_t data;

    while (sbuffer_read(
               &gateway->sbuf,
               SBUFFER_READER_DATA,
               &data
           ) == 0) {

#ifdef DEBUG
        printf(
            "[DATA] id=%u type=%d value=%.2f ts=%ld\n",
            data.id,
            data.type,
            data.value,
            (long)data.ts
        );
#endif
    }


#ifdef DEBUG
    printf("[DATA] Data Manager stopped\n");
#endif

    return NULL;
}