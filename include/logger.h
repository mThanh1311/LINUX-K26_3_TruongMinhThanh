#ifndef LOGGER_H
#define LOGGER_H

#include <sys/types.h>

#define LOG_FIFO_PATH "logFifo"
#define LOG_FILE_PATH "gateway.log"

#define LOG_MESSAGE_MAX 200
#define LOG_EVENT_MAX   256

typedef enum {
    LOG_MAIN,
    LOG_CONN,
    LOG_DATA,
    LOG_STOR,
    LOG_WEB
} log_source_t;


/*
 * Create FIFO and fork the log process.
 *
 * On success:
 * - child_pid contains PID of log process
 * - parent has FIFO writer ready
 */
int logger_start(pid_t *child_pid);



// Send one complete log event into logFifo.
int logger_write(
    log_source_t source,
    const char *fmt,
    ...
);



// Close FIFO writer, wait for log process, then remove logFifo.
int logger_shutdown(pid_t child_pid);

#endif