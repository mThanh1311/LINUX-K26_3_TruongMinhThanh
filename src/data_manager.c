#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_manager.h"
#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"


/* ============================================================
 * Sensor type conversion
 * ============================================================ */

static int sensor_type_from_string(
    const char *text,
    sensor_type_t *type
)
{
    if (
        text == NULL ||
        type == NULL
    ) {
        return -1;
    }


    if (
        strcmp(
            text,
            "temperature"
        ) == 0
    ) {

        *type =
            SENSOR_TEMPERATURE;

        return 0;
    }


    if (
        strcmp(
            text,
            "humidity"
        ) == 0
    ) {

        *type =
            SENSOR_HUMIDITY;

        return 0;
    }


    if (
        strcmp(
            text,
            "motion"
        ) == 0
    ) {

        *type =
            SENSOR_MOTION;

        return 0;
    }


    if (
        strcmp(
            text,
            "door"
        ) == 0
    ) {

        *type =
            SENSOR_DOOR;

        return 0;
    }


    if (
        strcmp(
            text,
            "smoke_gas"
        ) == 0
    ) {

        *type =
            SENSOR_SMOKE_GAS;

        return 0;
    }


    return -1;
}


/* ============================================================
 * Configuration lookup
 * ============================================================ */

static datamgr_sensor_t *find_sensor(
    datamgr_t *manager,
    sensor_id_t id
)
{
    if (manager == NULL) {
        return NULL;
    }


    for (
        size_t i = 0;
        i < manager->n;
        i++
    ) {

        if (
            manager->sensors[i].info.id ==
            id
        ) {

            return
                &manager->sensors[i];
        }
    }


    return NULL;
}


/* ============================================================
 * Load room-sensor configuration
 * ============================================================ */

static int load_room_sensor_config(
    datamgr_t *manager,
    const char *path
)
{
    if (
        manager == NULL ||
        path == NULL
    ) {

        return -1;
    }


    FILE *file =
        fopen(
            path,
            "r"
        );


    if (file == NULL) {

        perror(
            "fopen room-sensor config"
        );

        return -1;
    }


    manager->n =
        0;


    char line[256];


    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {

        char *p =
            line;


        while (
            isspace(
                (unsigned char)*p
            )
        ) {
            p++;
        }


        /*
         * Ignore blank lines
         * and comments.
         */
        if (
            *p == '\0' ||
            *p == '\n' ||
            *p == '#'
        ) {

            continue;
        }


        unsigned int room;
        unsigned int id;

        char type_text[32];
        char extra;


        int fields =
            sscanf(
                p,
                "%u %u %31s %c",
                &room,
                &id,
                type_text,
                &extra
            );


        /*
         * Exactly three fields required.
         */
        if (fields != 3) {

            fprintf(
                stderr,
                "Invalid room-sensor config line: %s",
                line
            );

            fclose(file);

            return -1;
        }


        if (
            room > UINT16_MAX ||
            id > UINT16_MAX
        ) {

            fprintf(
                stderr,
                "Room or sensor ID out of range\n"
            );

            fclose(file);

            return -1;
        }


        sensor_type_t type;


        if (
            sensor_type_from_string(
                type_text,
                &type
            ) != 0
        ) {

            fprintf(
                stderr,
                "Invalid sensor type in config: %s\n",
                type_text
            );

            fclose(file);

            return -1;
        }


        /*
         * Sensor IDs are unique.
         */
        if (
            find_sensor(
                manager,
                (sensor_id_t)id
            ) != NULL
        ) {

            fprintf(
                stderr,
                "Duplicate sensor ID in config: %u\n",
                id
            );

            fclose(file);

            return -1;
        }


        if (
            manager->n >=
            MAX_SENSORS
        ) {

            fprintf(
                stderr,
                "Too many sensors in room-sensor config\n"
            );

            fclose(file);

            return -1;
        }


        datamgr_sensor_t *sensor =
            &manager->sensors[
                manager->n
            ];


        memset(
            sensor,
            0,
            sizeof(*sensor)
        );


        sensor->info.room =
            (uint16_t)room;

        sensor->info.id =
            (sensor_id_t)id;

        sensor->info.type =
            type;


        sensor->state =
            STATE_COLLECTING;

        sensor->alert_kind =
            ALERT_NONE;


        manager->n++;
    }


    fclose(file);


#ifdef DEBUG
    printf(
        "[DATA] Loaded %zu sensors from %s\n",
        manager->n,
        path
    );
#endif


    return 0;
}


/* ============================================================
 * Running average
 * ============================================================ */

static double update_running_average(
    datamgr_sensor_t *sensor,
    double value
)
{
    if (
        sensor->count <
        RUN_AVG_LENGTH
    ) {

        sensor->window[
            sensor->next
        ] = value;


        sensor->sum +=
            value;


        sensor->count++;

    } else {

        sensor->sum -=
            sensor->window[
                sensor->next
            ];


        sensor->window[
            sensor->next
        ] = value;


        sensor->sum +=
            value;
    }


    sensor->next =
        (
            sensor->next + 1
        ) %
        RUN_AVG_LENGTH;


    sensor->running_avg =
        sensor->sum /
        (double)sensor->count;


    return
        sensor->running_avg;
}


/* ============================================================
 * Temperature handler
 * ============================================================ */

static void handle_temperature(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;


    double avg =
        update_running_average(
            sensor,
            data->value
        );


#ifdef DEBUG
    printf(
        "[DATA] TEMP id=%u room=%u "
        "value=%.2f avg=%.2f samples=%zu\n",
        data->id,
        sensor->info.room,
        data->value,
        avg,
        sensor->count
    );
#endif


    /*
     * Need exactly 5 measurements
     * before making a running-average decision.
     */
    if (
        sensor->count <
        RUN_AVG_LENGTH
    ) {

        sensor->state =
            STATE_COLLECTING;

        return;
    }


    if (
        avg <
        TEMP_MIN_C
    ) {

        if (
            sensor->state !=
                STATE_ALERT ||
            sensor->alert_kind !=
                ALERT_TEMP_COLD
        ) {

            logger_write(
                LOG_DATA,
                "The sensor node with %u reports it's too cold "
                "(running avg temperature = %.1f)",
                data->id,
                avg
            );
        }


        sensor->state =
            STATE_ALERT;

        sensor->alert_kind =
            ALERT_TEMP_COLD;


        return;
    }


    if (
        avg >
        TEMP_MAX_C
    ) {

        if (
            sensor->state !=
                STATE_ALERT ||
            sensor->alert_kind !=
                ALERT_TEMP_HOT
        ) {

            logger_write(
                LOG_DATA,
                "The sensor node with %u reports it's too hot "
                "(running avg temperature = %.1f)",
                data->id,
                avg
            );
        }


        sensor->state =
            STATE_ALERT;

        sensor->alert_kind =
            ALERT_TEMP_HOT;


        return;
    }


    sensor->state =
        STATE_NORMAL;

    sensor->alert_kind =
        ALERT_NONE;
}


/* ============================================================
 * Humidity handler
 * ============================================================ */

static void handle_humidity(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;


    double avg =
        update_running_average(
            sensor,
            data->value
        );


#ifdef DEBUG
    printf(
        "[DATA] HUM id=%u room=%u "
        "value=%.2f avg=%.2f samples=%zu\n",
        data->id,
        sensor->info.room,
        data->value,
        avg,
        sensor->count
    );
#endif


    if (
        sensor->count <
        RUN_AVG_LENGTH
    ) {

        sensor->state =
            STATE_COLLECTING;

        return;
    }


    if (
        avg <
        HUMIDITY_MIN
    ) {

        if (
            sensor->state !=
                STATE_ALERT ||
            sensor->alert_kind !=
                ALERT_HUMIDITY_DRY
        ) {

            logger_write(
                LOG_DATA,
                "The sensor node with %u reports it's too dry "
                "(running avg humidity = %.1f)",
                data->id,
                avg
            );
        }


        sensor->state =
            STATE_ALERT;

        sensor->alert_kind =
            ALERT_HUMIDITY_DRY;


        return;
    }


    if (
        avg >
        HUMIDITY_MAX
    ) {

        if (
            sensor->state !=
                STATE_ALERT ||
            sensor->alert_kind !=
                ALERT_HUMIDITY_HUMID
        ) {

            logger_write(
                LOG_DATA,
                "The sensor node with %u reports it's too humid "
                "(running avg humidity = %.1f)",
                data->id,
                avg
            );
        }


        sensor->state =
            STATE_ALERT;

        sensor->alert_kind =
            ALERT_HUMIDITY_HUMID;


        return;
    }


    sensor->state =
        STATE_NORMAL;

    sensor->alert_kind =
        ALERT_NONE;
}


/* ============================================================
 * Motion handler
 * ============================================================ */

static void handle_motion(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;
    (void)sensor;


    /*
     * Motion frames represent events.
     */
    if (
        data->value ==
        1.0
    ) {

        logger_write(
            LOG_DATA,
            "The sensor node with %u detected motion",
            data->id
        );


#ifdef DEBUG
        printf(
            "[DATA] MOTION id=%u detected\n",
            data->id
        );
#endif
    }
}


/* ============================================================
 * Door handler
 * ============================================================ */

static void handle_door(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;


    bool open =
        data->value != 0.0;


    /*
     * Log initial known state and then
     * only state changes.
     */
    if (
        !sensor->door_state_known ||
        sensor->door_open != open
    ) {

        logger_write(
            LOG_DATA,
            "The sensor node with %u reports the door is %s",
            data->id,
            open ?
                "open" :
                "closed"
        );


        sensor->door_state_known =
            true;

        sensor->door_open =
            open;
    }


#ifdef DEBUG
    printf(
        "[DATA] DOOR id=%u state=%s\n",
        data->id,
        open ?
            "open" :
            "closed"
    );
#endif
}


/* ============================================================
 * Smoke/Gas handler
 * ============================================================ */

static void handle_smoke_gas(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
)
{
    (void)manager;


#ifdef DEBUG
    printf(
        "[DATA] GAS id=%u room=%u ppm=%.2f\n",
        data->id,
        sensor->info.room,
        data->value
    );
#endif


    if (
        data->value >=
        GAS_ALARM_PPM
    ) {

        /*
         * Log only when entering alert state.
         */
        if (
            sensor->state !=
                STATE_ALERT ||
            sensor->alert_kind !=
                ALERT_SMOKE_GAS
        ) {

            logger_write(
                LOG_DATA,
                "The sensor node with %u reports a smoke/gas alarm "
                "(concentration = %.1f ppm)",
                data->id,
                data->value
            );
        }


        sensor->state =
            STATE_ALERT;

        sensor->alert_kind =
            ALERT_SMOKE_GAS;


        return;
    }


    /*
     * Back below alarm threshold:
     * arm alarm again for next crossing.
     */
    sensor->state =
        STATE_NORMAL;

    sensor->alert_kind =
        ALERT_NONE;
}


/* ============================================================
 * Handler table
 * ============================================================ */

static sensor_handler_t
    HANDLERS[6] = {

    [SENSOR_TEMPERATURE] =
        handle_temperature,

    [SENSOR_HUMIDITY] =
        handle_humidity,

    [SENSOR_MOTION] =
        handle_motion,

    [SENSOR_DOOR] =
        handle_door,

    [SENSOR_SMOKE_GAS] =
        handle_smoke_gas
};


/* ============================================================
 * Dispatch one normalized measurement
 * ============================================================ */

static void process_sensor_data(
    datamgr_t *manager,
    const sensor_data_t *data
)
{
    datamgr_sensor_t *sensor =
        find_sensor(
            manager,
            data->id
        );


    /*
     * Unknown ID OR type mismatch:
     * same required log event.
     */
    if (
        sensor == NULL ||
        sensor->info.type !=
            data->type
    ) {

        logger_write(
            LOG_DATA,
            "Received sensor data with invalid sensor node ID %u",
            data->id
        );


#ifdef DEBUG
        printf(
            "[DATA] Invalid sensor: id=%u type=%d\n",
            data->id,
            data->type
        );
#endif

        return;
    }


    if (
        data->type <
            SENSOR_TEMPERATURE ||
        data->type >
            SENSOR_SMOKE_GAS ||
        HANDLERS[
            data->type
        ] == NULL
    ) {

        logger_write(
            LOG_DATA,
            "Received sensor data with invalid sensor node ID %u",
            data->id
        );

        return;
    }


    HANDLERS[
        data->type
    ](
        manager,
        sensor,
        data
    );
}


/* ============================================================
 * Data Manager thread
 * ============================================================ */

void *data_manager_thread(void *arg)
{
    gateway_t *gateway =
        (gateway_t *)arg;


#ifdef DEBUG
    printf(
        "[DATA] Data Manager started\n"
    );
#endif


    logger_write(
        LOG_DATA,
        "Data Manager started"
    );


    if (
        load_room_sensor_config(
            &gateway->data,
            ROOM_SENSOR_CONFIG_PATH
        ) != 0
    ) {

        fprintf(
            stderr,
            "[DATA] Failed to load room-sensor configuration\n"
        );


        gateway_request_stop(
            gateway
        );


        sbuffer_shutdown(
            &gateway->sbuf
        );


        return NULL;
    }


    sensor_data_t data;


    while (
        sbuffer_read(
            &gateway->sbuf,
            SBUFFER_READER_DATA,
            &data
        ) == 0
    ) {

#ifdef DEBUG
        printf(
            "[DATA] id=%u type=%d "
            "value=%.2f ts=%ld\n",
            data.id,
            data.type,
            data.value,
            (long)data.ts
        );
#endif


        process_sensor_data(
            &gateway->data,
            &data
        );
    }


#ifdef DEBUG
    printf(
        "[DATA] Data Manager stopped\n"
    );
#endif


    return NULL;
}