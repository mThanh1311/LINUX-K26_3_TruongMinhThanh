#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>


#define TBP_FRAME_SIZE 18U


static volatile sig_atomic_t g_stop = 0;


static void signal_handler(int signo)
{
    (void)signo;
    g_stop = 1;
}


static int parse_u16(
    const char *text,
    uint16_t *value
)
{
    if (text == NULL ||
        value == NULL) {
        return -1;
    }

    char *end = NULL;

    errno = 0;

    unsigned long v =
        strtoul(text, &end, 10);

    if (
        errno != 0 ||
        end == text ||
        *end != '\0' ||
        v > UINT16_MAX
    ) {
        return -1;
    }

    *value = (uint16_t)v;

    return 0;
}


static int parse_period(
    const char *text,
    unsigned int *period_ms
)
{
    if (text == NULL ||
        period_ms == NULL) {
        return -1;
    }

    char *end = NULL;

    errno = 0;

    unsigned long v =
        strtoul(text, &end, 10);

    if (
        errno != 0 ||
        end == text ||
        *end != '\0' ||
        v == 0 ||
        v > 3600000UL
    ) {
        return -1;
    }

    *period_ms =
        (unsigned int)v;

    return 0;
}


static void write_u16_le(
    uint8_t *p,
    uint16_t value
)
{
    p[0] =
        (uint8_t)(value & 0xFFU);

    p[1] =
        (uint8_t)(
            (value >> 8) & 0xFFU
        );
}


static void write_u64_le(
    uint8_t *p,
    uint64_t value
)
{
    for (unsigned int i = 0; i < 8; i++) {

        p[i] =
            (uint8_t)(
                (value >> (8U * i)) &
                0xFFU
            );
    }
}


static void build_tbp_frame(
    uint8_t frame[TBP_FRAME_SIZE],
    uint16_t sensor_id,
    double temperature,
    int64_t timestamp
)
{
    write_u16_le(
        frame,
        sensor_id
    );


    uint64_t value_bits;

    memcpy(
        &value_bits,
        &temperature,
        sizeof(value_bits)
    );


    write_u64_le(
        frame + 2,
        value_bits
    );


    write_u64_le(
        frame + 10,
        (uint64_t)timestamp
    );
}


static int send_all(
    int fd,
    const uint8_t *buffer,
    size_t length
)
{
    size_t sent_total = 0;


    while (sent_total < length) {

        ssize_t sent =
            send(
                fd,
                buffer + sent_total,
                length - sent_total,
                0
            );


        if (sent < 0) {

            if (errno == EINTR) {

                if (g_stop) {
                    return -1;
                }

                continue;
            }

            perror("send");

            return -1;
        }


        if (sent == 0) {
            return -1;
        }


        sent_total +=
            (size_t)sent;
    }


    return 0;
}


static int sleep_ms(
    unsigned int period_ms
)
{
    struct timespec req;

    req.tv_sec =
        period_ms / 1000U;

    req.tv_nsec =
        (long)(
            period_ms % 1000U
        ) * 1000000L;


    while (
        !g_stop &&
        nanosleep(
            &req,
            &req
        ) == -1
    ) {

        if (errno != EINTR) {
            return -1;
        }
    }


    return 0;
}


static int connect_to_gateway(
    const char *host,
    uint16_t port
)
{
    char port_text[16];

    snprintf(
        port_text,
        sizeof(port_text),
        "%u",
        port
    );


    struct addrinfo hints;

    memset(
        &hints,
        0,
        sizeof(hints)
    );

    hints.ai_family =
        AF_UNSPEC;

    hints.ai_socktype =
        SOCK_STREAM;


    struct addrinfo *result = NULL;


    int rc =
        getaddrinfo(
            host,
            port_text,
            &hints,
            &result
        );


    if (rc != 0) {

        fprintf(
            stderr,
            "getaddrinfo: %s\n",
            gai_strerror(rc)
        );

        return -1;
    }


    int fd = -1;


    for (
        struct addrinfo *rp = result;
        rp != NULL;
        rp = rp->ai_next
    ) {

        fd = socket(
            rp->ai_family,
            rp->ai_socktype,
            rp->ai_protocol
        );


        if (fd < 0) {
            continue;
        }


        if (
            connect(
                fd,
                rp->ai_addr,
                rp->ai_addrlen
            ) == 0
        ) {

            break;
        }


        close(fd);

        fd = -1;
    }


    freeaddrinfo(result);


    if (fd < 0) {

        fprintf(
            stderr,
            "Unable to connect to %s:%u\n",
            host,
            port
        );

        return -1;
    }


    return fd;
}


static double generate_temperature(void)
{
    /*
     * Generate approximately:
     *
     * 18.00 °C ... 27.99 °C
     */
    int random_part =
        rand() % 1000;

    return
        18.0 +
        ((double)random_part / 100.0);
}


int main(
    int argc,
    char *argv[]
)
{
    /*
     * M3 interface:
     *
     * ./sensor_node temperature <id> <host> <P> <period_ms>
     */
    if (argc != 6) {

        fprintf(
            stderr,
            "Usage: %s "
            "temperature "
            "<id> "
            "<host> "
            "<base_port> "
            "<period_ms>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }


    /*
     * Step 3 supports Temperature/TBP only.
     *
     * Step 4 will extend this program to:
     * humidity, motion, door, smoke_gas.
     */
    if (
        strcmp(
            argv[1],
            "temperature"
        ) != 0
    ) {

        fprintf(
            stderr,
            "M3 sensor_node currently supports "
            "temperature only\n"
        );

        return EXIT_FAILURE;
    }


    uint16_t sensor_id;
    uint16_t base_port;
    unsigned int period_ms;


    if (
        parse_u16(
            argv[2],
            &sensor_id
        ) != 0
    ) {

        fprintf(
            stderr,
            "Invalid sensor ID\n"
        );

        return EXIT_FAILURE;
    }


    if (
        parse_u16(
            argv[4],
            &base_port
        ) != 0 ||
        base_port == 0
    ) {

        fprintf(
            stderr,
            "Invalid base port\n"
        );

        return EXIT_FAILURE;
    }


    if (
        parse_period(
            argv[5],
            &period_ms
        ) != 0
    ) {

        fprintf(
            stderr,
            "Invalid period\n"
        );

        return EXIT_FAILURE;
    }


    /*
     * SIGPIPE must not terminate the process
     * if the gateway disappears.
     */
    signal(
        SIGPIPE,
        SIG_IGN
    );


    struct sigaction sa;

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


    sigaction(
        SIGINT,
        &sa,
        NULL
    );

    sigaction(
        SIGTERM,
        &sa,
        NULL
    );


    int fd =
        connect_to_gateway(
            argv[3],
            base_port
        );


    if (fd < 0) {
        return EXIT_FAILURE;
    }


    srand(
        (unsigned int)(
            time(NULL) ^
            getpid() ^
            sensor_id
        )
    );


    printf(
        "[SENSOR] temperature sensor %u "
        "connected to %s:%u "
        "(period=%u ms)\n",
        sensor_id,
        argv[3],
        base_port,
        period_ms
    );


    while (!g_stop) {

        double temperature =
            generate_temperature();


        int64_t timestamp =
            (int64_t)time(NULL);


        uint8_t frame[
            TBP_FRAME_SIZE
        ];


        build_tbp_frame(
            frame,
            sensor_id,
            temperature,
            timestamp
        );


        if (
            send_all(
                fd,
                frame,
                sizeof(frame)
            ) != 0
        ) {

            if (!g_stop) {

                fprintf(
                    stderr,
                    "[SENSOR] connection lost\n"
                );
            }

            break;
        }


        printf(
            "[SENSOR] id=%u "
            "value=%.2f "
            "ts=%ld\n",
            sensor_id,
            temperature,
            (long)timestamp
        );


        if (
            sleep_ms(
                period_ms
            ) != 0
        ) {

            break;
        }
    }


    /*
     * Graceful TCP close.
     */
    shutdown(
        fd,
        SHUT_RDWR
    );

    close(fd);


    printf(
        "[SENSOR] sensor %u stopped\n",
        sensor_id
    );


    return EXIT_SUCCESS;
}