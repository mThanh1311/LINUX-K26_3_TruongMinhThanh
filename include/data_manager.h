#ifndef DATA_MANAGER_H
#define DATA_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensor_config.h"
#include "sensor_types.h"


#define MAX_SENSORS 64

#define ROOM_SENSOR_CONFIG_PATH \
    "config/room_sensor.conf"


typedef enum {
    STATE_COLLECTING,
    STATE_NORMAL,
    STATE_ALERT
} sensor_state_t;


typedef struct {
    uint16_t room;
    sensor_id_t id;
    sensor_type_t type;
} sensor_info_t;


typedef enum {
    ALERT_NONE,

    ALERT_TEMP_COLD,
    ALERT_TEMP_HOT,

    ALERT_HUMIDITY_DRY,
    ALERT_HUMIDITY_HUMID,

    ALERT_SMOKE_GAS
} alert_kind_t;


typedef struct {
    sensor_info_t info;

    /*
     * Temperature / Humidity:
     * circular window for last 5 samples.
     */
    double window[RUN_AVG_LENGTH];

    size_t count;
    size_t next;

    double sum;
    double running_avg;

    sensor_state_t state;
    alert_kind_t alert_kind;


    /*
     * Door event state.
     */
    bool door_state_known;
    bool door_open;

} datamgr_sensor_t;


typedef struct {
    datamgr_sensor_t sensors[MAX_SENSORS];

    size_t n;
} datamgr_t;


typedef void (*sensor_handler_t)(
    datamgr_t *manager,
    datamgr_sensor_t *sensor,
    const sensor_data_t *data
);


void *data_manager_thread(void *arg);


#endif