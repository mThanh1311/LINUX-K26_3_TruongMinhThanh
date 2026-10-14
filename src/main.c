#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gateway.h"
#include "logger.h"


/*
 * Signal handler only sets this flag.
 *
 * Do not call pthread functions, logger functions,
 * malloc(), printf(), etc. directly from the handler.
 */
static volatile sig_atomic_t g_signal_received = 0;


static void signal_handler(int signo)
{
    (void)signo;

    g_signal_received = 1;
}


/*
 * Parse TCP port from command-line argument.
 *
 * Valid range:
 *      1 .. 65535
 */
static int parse_port(
    const char *text,
    uint16_t *port
)
{
    if (text == NULL ||
        port == NULL) {

        return -1;
    }


    char *end = NULL;

    errno = 0;


    long value = strtol(
        text,
        &end,
        10
    );


    if (
        errno != 0 ||
        end == text ||
        *end != '\0' ||
        value < 1 ||
        value > 65535
    ) {

        return -1;
    }


    *port =
        (uint16_t)value;


    return 0;
}


int main(
    int argc,
    char *argv[]
)
{
    gateway_t gateway;

    uint16_t base_port;
    uint16_t http_port;

    struct sigaction sa;
    sigset_t shutdown_signals;


    /*
     * --------------------------------------------------
     * 1. Parse command-line arguments
     * --------------------------------------------------
     */
    if (argc != 3) {

        fprintf(
            stderr,
            "Usage: %s <base_port> <http_port>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }


    if (
        parse_port(
            argv[1],
            &base_port
        ) != 0 ||

        parse_port(
            argv[2],
            &http_port
        ) != 0
    ) {

        fprintf(
            stderr,
            "Invalid port number\n"
        );

        return EXIT_FAILURE;
    }


    /*
     * Later steps use:
     *
     * P
     * P + 1
     * P + 2
     * P + 3
     *
     * therefore P must be <= 65532.
     */
    if (base_port > 65532) {

        fprintf(
            stderr,
            "Base port must be <= 65532\n"
        );

        return EXIT_FAILURE;
    }


    /*
     * --------------------------------------------------
     * 2. Install signal handlers
     * --------------------------------------------------
     */
    memset(
        &sa,
        0,
        sizeof(sa)
    );

    sa.sa_handler =
        signal_handler;

    sigemptyset(
        &sa.sa_mask
    );


    if (
        sigaction(
            SIGINT,
            &sa,
            NULL
        ) == -1
    ) {

        perror(
            "sigaction SIGINT"
        );

        return EXIT_FAILURE;
    }


    if (
        sigaction(
            SIGTERM,
            &sa,
            NULL
        ) == -1
    ) {

        perror(
            "sigaction SIGTERM"
        );

        return EXIT_FAILURE;
    }


    /*
     * --------------------------------------------------
     * 3. Block shutdown signals temporarily
     * --------------------------------------------------
     *
     * Threads created later inherit this blocked mask.
     *
     * After startup, only the main thread will unblock
     * SIGINT/SIGTERM.
     */
    sigemptyset(
        &shutdown_signals
    );

    sigaddset(
        &shutdown_signals,
        SIGINT
    );

    sigaddset(
        &shutdown_signals,
        SIGTERM
    );


    if (
        pthread_sigmask(
            SIG_BLOCK,
            &shutdown_signals,
            NULL
        ) != 0
    ) {

        fprintf(
            stderr,
            "Failed to block shutdown signals\n"
        );

        return EXIT_FAILURE;
    }


    /*
     * --------------------------------------------------
     * 4. Initialize gateway
     * --------------------------------------------------
     */
    if (
        gateway_init(
            &gateway,
            base_port,
            http_port
        ) != 0
    ) {

        fprintf(
            stderr,
            "Failed to initialize gateway\n"
        );

        return EXIT_FAILURE;
    }


    /*
     * --------------------------------------------------
     * 5. Start Log Process
     * --------------------------------------------------
     *
     * fork() happens before worker pthreads are created.
     */
    if (
        logger_start(
            &gateway.log_pid
        ) != 0
    ) {

        fprintf(
            stderr,
            "Failed to start logger\n"
        );

        gateway_destroy(
            &gateway
        );

        return EXIT_FAILURE;
    }


    /*
     * --------------------------------------------------
     * 6. Start worker threads
     * --------------------------------------------------
     *
     * Connection Manager
     * Data Manager
     * Storage Manager
     */
    if (
        gateway_start(
            &gateway
        ) != 0
    ) {

        fprintf(
            stderr,
            "Failed to start gateway\n"
        );


        /*
         * Logger process has already started,
         * so shut it down before destroying gateway.
         */
        logger_shutdown(
            gateway.log_pid
        );


        gateway_destroy(
            &gateway
        );


        return EXIT_FAILURE;
    }


    /*
     * IMPORTANT:
     *
     * There is NO fake sensor_data_t here anymore.
     *
     * From M3.4e onward, all sensor measurements
     * must enter through:
     *
     * TCP
     *  ↓
     * Connection Manager
     *  ↓
     * TBP decode
     *  ↓
     * sbuffer
     */


    logger_write(
        LOG_MAIN,
        "Gateway started (TCP base port %u, HTTP port %u)",
        gateway.base_port,
        gateway.http_port
    );


    /*
     * --------------------------------------------------
     * 7. Unblock SIGINT/SIGTERM in main thread
     * --------------------------------------------------
     *
     * Worker threads inherited the blocked mask.
     *
     * Therefore shutdown signals are handled
     * by the main thread.
     */
    if (
        pthread_sigmask(
            SIG_UNBLOCK,
            &shutdown_signals,
            NULL
        ) != 0
    ) {

        fprintf(
            stderr,
            "Failed to unblock shutdown signals\n"
        );


        gateway_request_stop(
            &gateway
        );


        /*
         * Wake Data Manager / Storage Manager if they
         * are blocked in pthread_cond_wait().
         */
        sbuffer_shutdown(
            &gateway.sbuf
        );


        gateway_join(
            &gateway
        );


        logger_shutdown(
            gateway.log_pid
        );


        gateway_destroy(
            &gateway
        );


        return EXIT_FAILURE;
    }


#ifdef DEBUG
    printf(
        "[MAIN] Gateway started: "
        "base=%u http=%u\n",
        gateway.base_port,
        gateway.http_port
    );
#endif


    /*
     * --------------------------------------------------
     * 8. Main runtime loop
     * --------------------------------------------------
     *
     * Main thread waits until SIGINT or SIGTERM.
     */
    while (
        !g_signal_received &&
        !gateway_should_stop(&gateway)
    ) {
        sleep(1);
    }

    /*
     * --------------------------------------------------
     * 9. Graceful shutdown
     * --------------------------------------------------
     */

#ifdef DEBUG
    if (g_signal_received) {
        printf(
            "[MAIN] Shutdown requested by signal\n"
        );
    } else {
        printf(
            "[MAIN] Internal shutdown requested\n"
        );
    }
#endif

    if (g_signal_received) {

        logger_write(
            LOG_MAIN,
            "Gateway shutting down (signal)"
        );

    } else {

        logger_write(
            LOG_MAIN,
            "Gateway shutting down (internal request)"
        );
    }


    /*
     * Ask worker threads to stop.
     */
    gateway_request_stop(
        &gateway
    );


    /*
     * Data Manager and Storage Manager may currently
     * be blocked inside sbuffer_read().
     *
     * Broadcast shutdown so they wake up and exit.
     */
    sbuffer_shutdown(
        &gateway.sbuf
    );


    /*
     * Wait for:
     *
     * - Connection Manager
     * - Data Manager
     * - Storage Manager
     */
    gateway_join(
        &gateway
    );


    /*
     * Worker threads can no longer write logs.
     *
     * Close FIFO writer and wait for Log Process.
     */
    if (
        logger_shutdown(
            gateway.log_pid
        ) != 0
    ) {

        fprintf(
            stderr,
            "Failed to shutdown logger cleanly\n"
        );
    }


    /*
     * Destroy mutexes, condition variables,
     * sbuffer resources, etc.
     */
    gateway_destroy(
        &gateway
    );


#ifdef DEBUG
    printf(
        "[MAIN] Gateway stopped cleanly\n"
    );
#endif


    return EXIT_SUCCESS;
}