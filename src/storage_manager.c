#include <sqlite3.h>
#include <stdio.h>
#include <unistd.h>

#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"
#include "storage_manager.h"


#define STORAGE_DB_PATH "gateway.db"

#define SQL_MAX_ATTEMPTS 3
#define SQL_RETRY_DELAY_SEC 1


static const char *CREATE_TABLE_SQL =
    "CREATE TABLE IF NOT EXISTS SensorData ("
    "id INTEGER NOT NULL,"
    "type INTEGER NOT NULL,"
    "value REAL NOT NULL,"
    "ts INTEGER NOT NULL"
    ");";


static const char *INSERT_SQL =
    "INSERT INTO SensorData "
    "(id, type, value, ts) "
    "VALUES (?, ?, ?, ?);";


static void storage_disconnect(
    storagemgr_t *manager
)
{
    if (manager == NULL) {
        return;
    }

    if (manager->insert != NULL) {

        sqlite3_finalize(
            manager->insert
        );

        manager->insert = NULL;
    }

    if (manager->db != NULL) {

        sqlite3_close(
            manager->db
        );

        manager->db = NULL;
    }
}


static int table_exists(
    sqlite3 *db
)
{
    static const char *sql =
        "SELECT 1 "
        "FROM sqlite_master "
        "WHERE type='table' "
        "AND name='SensorData' "
        "LIMIT 1;";

    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(
            db,
            sql,
            -1,
            &stmt,
            NULL
        ) != SQLITE_OK) {

        return -1;
    }

    int rc =
        sqlite3_step(stmt);

    int exists =
        (rc == SQLITE_ROW);

    sqlite3_finalize(stmt);

    return exists;
}


static int storage_connect(
    storagemgr_t *manager
)
{
    if (manager == NULL ||
        manager->db_path == NULL) {

        return -1;
    }

    storage_disconnect(manager);

    int rc =
        sqlite3_open(
            manager->db_path,
            &manager->db
        );

    if (rc != SQLITE_OK) {

        storage_disconnect(manager);

        return -1;
    }

    /*
     * Do not wait forever when another process
     * keeps the database locked.
     */
    sqlite3_busy_timeout(
        manager->db,
        500
    );

    int existed =
        table_exists(
            manager->db
        );

    if (existed < 0) {

        storage_disconnect(manager);

        return -1;
    }

    char *error_message = NULL;

    rc =
        sqlite3_exec(
            manager->db,
            CREATE_TABLE_SQL,
            NULL,
            NULL,
            &error_message
        );

    if (rc != SQLITE_OK) {

#ifdef DEBUG
        fprintf(
            stderr,
            "[STOR] CREATE TABLE failed: %s\n",
            error_message != NULL ?
                error_message :
                "unknown error"
        );
#endif

        sqlite3_free(
            error_message
        );

        storage_disconnect(
            manager
        );

        return -1;
    }

    rc =
        sqlite3_prepare_v2(
            manager->db,
            INSERT_SQL,
            -1,
            &manager->insert,
            NULL
        );

    if (rc != SQLITE_OK) {

        storage_disconnect(manager);

        return -1;
    }

    manager->state =
        SQL_CONNECTED;

    logger_write(
        LOG_STOR,
        "Connection to SQL server established."
    );

    if (!existed) {

        logger_write(
            LOG_STOR,
            "New table SensorData created."
        );
    }

#ifdef DEBUG
    printf(
        "[STOR] SQLite connected: %s\n",
        manager->db_path
    );
#endif

    return 0;
}


static int storage_insert(
    storagemgr_t *manager,
    const sensor_data_t *data
)
{
    if (manager == NULL ||
        data == NULL ||
        manager->insert == NULL) {

        return -1;
    }

    sqlite3_reset(
        manager->insert
    );

    sqlite3_clear_bindings(
        manager->insert
    );

    if (sqlite3_bind_int(
            manager->insert,
            1,
            (int)data->id
        ) != SQLITE_OK) {

        return -1;
    }

    if (sqlite3_bind_int(
            manager->insert,
            2,
            (int)data->type
        ) != SQLITE_OK) {

        return -1;
    }

    if (sqlite3_bind_double(
            manager->insert,
            3,
            data->value
        ) != SQLITE_OK) {

        return -1;
    }

    if (sqlite3_bind_int64(
            manager->insert,
            4,
            (sqlite3_int64)data->ts
        ) != SQLITE_OK) {

        return -1;
    }

    int rc =
        sqlite3_step(
            manager->insert
        );

    sqlite3_reset(
        manager->insert
    );

    if (rc != SQLITE_DONE) {

#ifdef DEBUG
        fprintf(
            stderr,
            "[STOR] INSERT failed: %s\n",
            sqlite3_errmsg(
                manager->db
            )
        );
#endif

        return -1;
    }

    return 0;
}


static void storage_fail_gateway(
    gateway_t *gateway,
    storagemgr_t *manager
)
{
    manager->state =
        SQL_DOWN;

    logger_write(
        LOG_STOR,
        "Unable to connect to SQL server."
    );

    gateway_request_stop(
        gateway
    );

    /*
     * Wake threads blocked in sbuffer.
     */
    sbuffer_shutdown(
        &gateway->sbuf
    );
}


void *storage_manager_thread(void *arg)
{
    gateway_t *gateway =
        (gateway_t *)arg;

    storagemgr_t *manager =
        &gateway->storage;


#ifdef DEBUG
    printf(
        "[STOR] Storage Manager started\n"
    );
#endif


    manager->db = NULL;
    manager->insert = NULL;

    manager->state =
        SQL_DOWN;

    manager->attempts = 0;

    manager->db_path =
        STORAGE_DB_PATH;


    /*
     * Initial connection.
     */
    while (
        storage_connect(
            manager
        ) != 0
    ) {

        manager->attempts++;

        if (
            manager->attempts >=
            SQL_MAX_ATTEMPTS
        ) {

            storage_fail_gateway(
                gateway,
                manager
            );

            storage_disconnect(
                manager
            );

            return NULL;
        }

        sleep(
            SQL_RETRY_DELAY_SEC
        );
    }


    manager->attempts = 0;


    sensor_data_t data;


    for (;;) {

        /*
         * Peek only.
         *
         * Measurement stays in sbuffer until
         * SQLite INSERT succeeds.
         */
        int read_result =
            sbuffer_peek_storage(
                &gateway->sbuf,
                &data
            );

        if (read_result != 0) {
            break;
        }


        if (
            storage_insert(
                manager,
                &data
            ) == 0
        ) {

            /*
             * SQLite owns the measurement now.
             * Storage consumer may finally mark it read.
             */
            if (
                sbuffer_commit_storage(
                    &gateway->sbuf
                ) != 0
            ) {

                gateway_request_stop(
                    gateway
                );

                sbuffer_shutdown(
                    &gateway->sbuf
                );

                break;
            }


            manager->attempts = 0;


#ifdef DEBUG
            printf(
                "[STOR] id=%u type=%d "
                "value=%.2f ts=%ld stored\n",
                data.id,
                data.type,
                data.value,
                (long)data.ts
            );
#endif

            continue;
        }


        /*
         * INSERT failed.
         *
         * Do NOT commit sbuffer node.
         */
        if (
            manager->state ==
            SQL_CONNECTED
        ) {

            logger_write(
                LOG_STOR,
                "Connection to SQL server lost."
            );
        }


        manager->state =
            SQL_LOST;

        manager->attempts++;


        storage_disconnect(
            manager
        );


        if (
            manager->attempts >=
            SQL_MAX_ATTEMPTS
        ) {

            storage_fail_gateway(
                gateway,
                manager
            );

            break;
        }


        sleep(
            SQL_RETRY_DELAY_SEC
        );


        /*
         * Try to establish DB again.
         *
         * We intentionally do not reset attempts here.
         * Only a successful INSERT proves recovery.
         */
        if (
            storage_connect(
                manager
            ) != 0
        ) {

            /*
             * Next loop iteration will retry.
             */
            continue;
        }
    }


    storage_disconnect(
        manager
    );


#ifdef DEBUG
    printf(
        "[STOR] Storage Manager stopped\n"
    );
#endif


    return NULL;
}