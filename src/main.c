#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gateway.h"


static volatile sig_atomic_t g_signal_received = 0;


static void signal_handler(int signo)
{
    (void)signo;
    g_signal_received = 1;
}


static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;

    errno = 0;

    long value = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value < 1 ||
        value > 65535) {
        return -1;
    }

    *port = (uint16_t)value;

    return 0;
}


int main(int argc, char *argv[])
{
    gateway_t gateway;

    uint16_t base_port;
    uint16_t http_port;

    struct sigaction sa;
    sigset_t shutdown_signals;


    if (argc != 3) {
        fprintf(
            stderr,
            "Usage: %s <base_port> <http_port>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    if (parse_port(argv[1], &base_port) != 0 ||
        parse_port(argv[2], &http_port) != 0) {

        fprintf(stderr, "Invalid port number\n");
        return EXIT_FAILURE;
    }

    if (base_port > 65532) {
        fprintf(stderr, "Base port must be <= 65532\n");
        return EXIT_FAILURE;
    }


    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        return EXIT_FAILURE;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
        return EXIT_FAILURE;
    }

    sigemptyset(&shutdown_signals);
    sigaddset(&shutdown_signals, SIGINT);
    sigaddset(&shutdown_signals, SIGTERM);

    if (pthread_sigmask(
            SIG_BLOCK,
            &shutdown_signals,
            NULL
        ) != 0) {

        fprintf(stderr, "Failed to block shutdown signals\n");
        return EXIT_FAILURE;
    }


    if (gateway_init(
            &gateway,
            base_port,
            http_port
        ) != 0) {

        fprintf(stderr, "Failed to initialize gateway\n");
        return EXIT_FAILURE;
    }

    if (gateway_start(&gateway) != 0) {
        fprintf(stderr, "Failed to start gateway\n");

        gateway_destroy(&gateway);

        return EXIT_FAILURE;
    }

    if (pthread_sigmask(
            SIG_UNBLOCK,
            &shutdown_signals,
            NULL
        ) != 0) {

        fprintf(stderr, "Failed to unblock shutdown signals\n");

        gateway_request_stop(&gateway);
        gateway_join(&gateway);
        gateway_destroy(&gateway);

        return EXIT_FAILURE;
    }


#ifdef DEBUG
    printf(
        "[MAIN] Gateway started: base=%u http=%u\n",
        gateway.base_port,
        gateway.http_port
    );
#endif

    while (!g_signal_received) {
        sleep(1);
    }


#ifdef DEBUG
    printf("[MAIN] Shutdown requested\n");
#endif

    gateway_request_stop(&gateway);

    gateway_join(&gateway);

    gateway_destroy(&gateway);


#ifdef DEBUG
    printf("[MAIN] Gateway stopped cleanly\n");
#endif

    return EXIT_SUCCESS;
}