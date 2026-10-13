#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


#define MAX_FRAME_SIZE 96U
#define MAX_PAYLOAD_SIZE (MAX_FRAME_SIZE * 2U)

#define TBP_FRAME_SIZE        18U
#define EVP_MOTION_FRAME_SIZE 8U
#define EVP_DOOR_FRAME_SIZE   9U
#define SGP_FRAME_SIZE        11U

#define SGP_MAGIC 0x7EU
#define SGP_ACK   0x06U
#define SGP_NAK   0x15U

#define SGP_MAX_RETRIES 3U


typedef enum {
    NODE_TEMPERATURE,
    NODE_HUMIDITY,
    NODE_MOTION,
    NODE_DOOR,
    NODE_SMOKE_GAS
} node_type_t;


typedef struct {
    uint8_t data[MAX_FRAME_SIZE];
    size_t len;

    double value;
    int64_t timestamp;

    bool corrupt;
} encoded_frame_t;


static volatile sig_atomic_t g_stop = 0;


/* ============================================================
 * Signal
 * ============================================================ */

static void signal_handler(int signo)
{
    (void)signo;
    g_stop = 1;
}


/* ============================================================
 * Argument parsing
 * ============================================================ */

static int parse_u16(
    const char *text,
    uint16_t *value
)
{
    if (text == NULL || value == NULL) {
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
    if (text == NULL || period_ms == NULL) {
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

    *period_ms = (unsigned int)v;

    return 0;
}


static int parse_node_type(
    const char *text,
    node_type_t *type
)
{
    if (text == NULL || type == NULL) {
        return -1;
    }

    if (strcmp(text, "temperature") == 0) {
        *type = NODE_TEMPERATURE;
        return 0;
    }

    if (strcmp(text, "humidity") == 0) {
        *type = NODE_HUMIDITY;
        return 0;
    }

    if (strcmp(text, "motion") == 0) {
        *type = NODE_MOTION;
        return 0;
    }

    if (strcmp(text, "door") == 0) {
        *type = NODE_DOOR;
        return 0;
    }

    if (strcmp(text, "smoke_gas") == 0) {
        *type = NODE_SMOKE_GAS;
        return 0;
    }

    return -1;
}


static const char *node_type_name(node_type_t type)
{
    switch (type) {

        case NODE_TEMPERATURE:
            return "temperature";

        case NODE_HUMIDITY:
            return "humidity";

        case NODE_MOTION:
            return "motion";

        case NODE_DOOR:
            return "door";

        case NODE_SMOKE_GAS:
            return "smoke_gas";
    }

    return "unknown";
}


/* ============================================================
 * Port mapping
 * ============================================================ */

static uint16_t target_port(
    node_type_t type,
    uint16_t base_port
)
{
    switch (type) {

        case NODE_TEMPERATURE:
            return base_port;

        case NODE_HUMIDITY:
            return (uint16_t)(base_port + 1);

        case NODE_MOTION:
        case NODE_DOOR:
            return (uint16_t)(base_port + 2);

        case NODE_SMOKE_GAS:
            return (uint16_t)(base_port + 3);
    }

    return base_port;
}


/* ============================================================
 * Encoding helpers
 * ============================================================ */

static void write_u16_le(
    uint8_t *p,
    uint16_t value
)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
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


static void write_u16_be(
    uint8_t *p,
    uint16_t value
)
{
    p[0] = (uint8_t)((value >> 8) & 0xFFU);
    p[1] = (uint8_t)(value & 0xFFU);
}


static void write_u32_be(
    uint8_t *p,
    uint32_t value
)
{
    p[0] = (uint8_t)((value >> 24) & 0xFFU);
    p[1] = (uint8_t)((value >> 16) & 0xFFU);
    p[2] = (uint8_t)((value >> 8) & 0xFFU);
    p[3] = (uint8_t)(value & 0xFFU);
}


/* ============================================================
 * CRC-16/CCITT-FALSE
 * ============================================================ */

static uint16_t crc16_ccitt_false(
    const uint8_t *data,
    size_t len
)
{
    uint16_t crc = 0xFFFFU;

    for (size_t i = 0; i < len; i++) {

        crc ^=
            (uint16_t)data[i] << 8;

        for (unsigned int bit = 0; bit < 8; bit++) {

            if (crc & 0x8000U) {

                crc =
                    (uint16_t)(
                        (crc << 1) ^
                        0x1021U
                    );

            } else {

                crc =
                    (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}


/* ============================================================
 * Random generators
 * ============================================================ */

static double random_double(
    double min_value,
    double max_value
)
{
    double ratio =
        (double)rand() /
        (double)RAND_MAX;

    return
        min_value +
        ratio *
        (max_value - min_value);
}


/*
 * Motion and Door:
 *
 * period_ms is average interval,
 * not a fixed interval.
 *
 * Uniform around approximately:
 * 0.5 * mean ... 1.5 * mean
 */
static unsigned int random_event_delay(
    unsigned int mean_ms
)
{
    unsigned int half =
        mean_ms / 2U;

    if (half == 0) {
        half = 1;
    }

    unsigned int span =
        mean_ms + 1U;

    return
        half +
        (unsigned int)(
            rand() % span
        );
}


/* ============================================================
 * Frame encoders
 * ============================================================ */

static int build_temperature(
    uint16_t id,
    bool corrupt,
    encoded_frame_t *frame
)
{
    double value =
        corrupt ?
            200.0 :
            random_double(
                18.0,
                28.0
            );

    int64_t ts =
        (int64_t)time(NULL);


    write_u16_le(
        frame->data,
        id
    );


    uint64_t bits;

    memcpy(
        &bits,
        &value,
        sizeof(bits)
    );


    write_u64_le(
        frame->data + 2,
        bits
    );


    write_u64_le(
        frame->data + 10,
        (uint64_t)ts
    );


    frame->len =
        TBP_FRAME_SIZE;

    frame->value =
        value;

    frame->timestamp =
        ts;

    frame->corrupt =
        corrupt;


    return 0;
}


static int build_humidity(
    uint16_t id,
    bool corrupt,
    encoded_frame_t *frame
)
{
    double value =
        random_double(
            30.0,
            80.0
        );

    int64_t ts =
        (int64_t)time(NULL);


    int written;


    if (corrupt) {
        written =
            snprintf(
                (char *)frame->data,
                sizeof(frame->data),
                "BAD %u %.2f %ld\n",
                id,
                value,
                (long)ts
            );

    } else {

        written =
            snprintf(
                (char *)frame->data,
                sizeof(frame->data),
                "HUM %u %.2f %ld\n",
                id,
                value,
                (long)ts
            );
    }


    if (
        written < 0 ||
        (size_t)written >=
            sizeof(frame->data)
    ) {
        return -1;
    }


    frame->len =
        (size_t)written;

    frame->value =
        value;

    frame->timestamp =
        ts;

    frame->corrupt =
        corrupt;


    return 0;
}


static int build_motion(
    uint16_t id,
    bool corrupt,
    encoded_frame_t *frame
)
{
    uint32_t ts =
        (uint32_t)time(NULL);


    if (corrupt) {

        /*
         * Unknown TLV type.
         *
         * One byte is enough; parser will keep it
         * until more bytes arrive and then resync.
         */
        frame->data[0] =
            0xFFU;

        frame->len =
            1;

    } else {

        frame->data[0] =
            0x01U;

        frame->data[1] =
            0x06U;

        write_u16_be(
            frame->data + 2,
            id
        );

        write_u32_be(
            frame->data + 4,
            ts
        );

        frame->len =
            EVP_MOTION_FRAME_SIZE;
    }


    frame->value =
        1.0;

    frame->timestamp =
        (int64_t)ts;

    frame->corrupt =
        corrupt;


    return 0;
}


static int build_door(
    uint16_t id,
    uint8_t state,
    bool corrupt,
    encoded_frame_t *frame
)
{
    uint32_t ts =
        (uint32_t)time(NULL);


    if (corrupt) {

        frame->data[0] =
            0xFFU;

        frame->len =
            1;

    } else {

        frame->data[0] =
            0x02U;

        frame->data[1] =
            0x07U;

        write_u16_be(
            frame->data + 2,
            id
        );

        frame->data[4] =
            state;

        write_u32_be(
            frame->data + 5,
            ts
        );

        frame->len =
            EVP_DOOR_FRAME_SIZE;
    }


    frame->value =
        (double)state;

    frame->timestamp =
        (int64_t)ts;

    frame->corrupt =
        corrupt;


    return 0;
}


static int build_smoke_gas(
    uint16_t id,
    bool corrupt,
    encoded_frame_t *frame
)
{
    uint16_t ppm =
        (uint16_t)(
            rand() %
            501
        );

    uint32_t ts =
        (uint32_t)time(NULL);


    frame->data[0] =
        SGP_MAGIC;


    write_u16_be(
        frame->data + 1,
        id
    );


    write_u16_be(
        frame->data + 3,
        ppm
    );


    write_u32_be(
        frame->data + 5,
        ts
    );


    uint16_t crc =
        crc16_ccitt_false(
            frame->data + 1,
            8
        );


    write_u16_be(
        frame->data + 9,
        crc
    );


    if (corrupt) {

        /*
         * Break CRC deliberately.
         */
        frame->data[10] ^=
            0xFFU;
    }


    frame->len =
        SGP_FRAME_SIZE;

    frame->value =
        (double)ppm;

    frame->timestamp =
        (int64_t)ts;

    frame->corrupt =
        corrupt;


    return 0;
}


static int build_frame(
    node_type_t type,
    uint16_t id,
    uint8_t door_state,
    bool corrupt,
    encoded_frame_t *frame
)
{
    memset(
        frame,
        0,
        sizeof(*frame)
    );


    switch (type) {

        case NODE_TEMPERATURE:

            return
                build_temperature(
                    id,
                    corrupt,
                    frame
                );


        case NODE_HUMIDITY:

            return
                build_humidity(
                    id,
                    corrupt,
                    frame
                );


        case NODE_MOTION:

            return
                build_motion(
                    id,
                    corrupt,
                    frame
                );


        case NODE_DOOR:

            return
                build_door(
                    id,
                    door_state,
                    corrupt,
                    frame
                );


        case NODE_SMOKE_GAS:

            return
                build_smoke_gas(
                    id,
                    corrupt,
                    frame
                );
    }


    return -1;
}


/* ============================================================
 * Networking
 * ============================================================ */

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


    struct addrinfo *result =
        NULL;


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

        fd =
            socket(
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


    freeaddrinfo(
        result
    );


    if (fd < 0) {

        fprintf(
            stderr,
            "Unable to connect to %s:%u\n",
            host,
            port
        );

        return -1;
    }


    /*
     * SGP:
     * ACK must arrive within 1 second.
     */
    struct timeval timeout;

    timeout.tv_sec =
        1;

    timeout.tv_usec =
        0;


    if (
        setsockopt(
            fd,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)
        ) < 0
    ) {

        perror(
            "setsockopt SO_RCVTIMEO"
        );

        close(fd);

        return -1;
    }


    return fd;
}


static int send_all(
    int fd,
    const uint8_t *data,
    size_t len
)
{
    size_t total = 0;


    while (total < len) {

        ssize_t sent =
            send(
                fd,
                data + total,
                len - total,
                0
            );


        if (sent < 0) {

            if (errno == EINTR) {

                if (g_stop) {
                    return -1;
                }

                continue;
            }


            perror(
                "send"
            );

            return -1;
        }


        if (sent == 0) {
            return -1;
        }


        total +=
            (size_t)sent;
    }


    return 0;
}


/*
 * --fragment:
 *
 * deliberately split one application payload
 * into two send() calls.
 */
static int send_payload(
    int fd,
    const uint8_t *payload,
    size_t len,
    bool fragment
)
{
    if (
        !fragment ||
        len < 2
    ) {

        return
            send_all(
                fd,
                payload,
                len
            );
    }


    size_t split =
        len / 2U;


    if (
        send_all(
            fd,
            payload,
            split
        ) != 0
    ) {

        return -1;
    }


    usleep(
        100000
    );


    return
        send_all(
            fd,
            payload + split,
            len - split
        );
}


/*
 * Returns:
 *
 *  0 -> received byte
 *  1 -> timeout
 * -1 -> socket failure / closed
 */
static int recv_sgp_reply(
    int fd,
    uint8_t *reply
)
{
    for (;;) {

        ssize_t received =
            recv(
                fd,
                reply,
                1,
                0
            );


        if (received == 1) {
            return 0;
        }


        if (received == 0) {
            return -1;
        }


        if (errno == EINTR) {

            if (g_stop) {
                return -1;
            }

            continue;
        }


        if (
            errno == EAGAIN ||
            errno == EWOULDBLOCK
        ) {

            return 1;
        }


        perror(
            "recv"
        );

        return -1;
    }
}


/*
 * Send one or more SGP frames.
 *
 * If any ACK is missing or NAK is received,
 * retransmit the same payload.
 *
 * Initial transmission + maximum 3 retries.
 */
static int send_sgp_with_retry(
    int fd,
    const uint8_t *payload,
    size_t payload_len,
    size_t frame_count,
    bool fragment
)
{
    for (
        unsigned int attempt = 0;
        attempt <= SGP_MAX_RETRIES;
        attempt++
    ) {

        if (
            send_payload(
                fd,
                payload,
                payload_len,
                fragment
            ) != 0
        ) {

            return -1;
        }


        bool all_ack =
            true;


        for (
            size_t i = 0;
            i < frame_count;
            i++
        ) {

            uint8_t reply = 0;

            int rc =
                recv_sgp_reply(
                    fd,
                    &reply
                );


            if (rc == 1) {

                printf(
                    "[SENSOR] SGP ACK timeout\n"
                );

                all_ack =
                    false;

                continue;
            }


            if (rc < 0) {

                return -1;
            }


            if (reply == SGP_ACK) {

                printf(
                    "[SENSOR] SGP reply=ACK(0x06)\n"
                );

            } else if (
                reply ==
                SGP_NAK
            ) {

                printf(
                    "[SENSOR] SGP reply=NAK(0x15)\n"
                );

                all_ack =
                    false;

            } else {

                printf(
                    "[SENSOR] SGP unexpected reply=0x%02X\n",
                    reply
                );

                all_ack =
                    false;
            }
        }


        if (all_ack) {
            return 0;
        }


        if (
            attempt <
            SGP_MAX_RETRIES
        ) {

            printf(
                "[SENSOR] Retrying SGP frame "
                "(retry %u/%u)\n",
                attempt + 1,
                SGP_MAX_RETRIES
            );
        }
    }


    /*
     * Retry budget exhausted,
     * but connection may still be valid.
     */
    return 1;
}


/* ============================================================
 * Timing
 * ============================================================ */

static int sleep_ms(
    unsigned int period_ms
)
{
    struct timespec req;

    req.tv_sec =
        period_ms /
        1000U;

    req.tv_nsec =
        (long)(
            period_ms %
            1000U
        ) *
        1000000L;


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


/* ============================================================
 * Main
 * ============================================================ */

int main(
    int argc,
    char *argv[]
)
{
    if (argc < 6) {

        fprintf(
            stderr,
            "Usage:\n"
            "  %s <type> <id> <host> <base_port> <period_ms> "
            "[--fragment] [--glue] [--corrupt]\n\n"
            "Types:\n"
            "  temperature\n"
            "  humidity\n"
            "  motion\n"
            "  door\n"
            "  smoke_gas\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }


    node_type_t type;

    uint16_t sensor_id;
    uint16_t base_port;

    unsigned int period_ms;


    if (
        parse_node_type(
            argv[1],
            &type
        ) != 0
    ) {

        fprintf(
            stderr,
            "Invalid sensor type\n"
        );

        return EXIT_FAILURE;
    }


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
        base_port == 0 ||
        base_port > 65532
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


    bool fragment =
        false;

    bool glue =
        false;

    bool corrupt_option =
        false;


    for (
        int i = 6;
        i < argc;
        i++
    ) {

        if (
            strcmp(
                argv[i],
                "--fragment"
            ) == 0
        ) {

            fragment = true;

        } else if (
            strcmp(
                argv[i],
                "--glue"
            ) == 0
        ) {

            glue = true;

        } else if (
            strcmp(
                argv[i],
                "--corrupt"
            ) == 0
        ) {

            corrupt_option = true;

        } else {

            fprintf(
                stderr,
                "Unknown option: %s\n",
                argv[i]
            );

            return EXIT_FAILURE;
        }
    }


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


    if (
        sigaction(
            SIGINT,
            &sa,
            NULL
        ) == -1 ||
        sigaction(
            SIGTERM,
            &sa,
            NULL
        ) == -1
    ) {

        perror(
            "sigaction"
        );

        return EXIT_FAILURE;
    }


    uint16_t port =
        target_port(
            type,
            base_port
        );


    int fd =
        connect_to_gateway(
            argv[3],
            port
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
        "[SENSOR] %s sensor %u connected "
        "to %s:%u "
        "(period=%u ms",
        node_type_name(type),
        sensor_id,
        argv[3],
        port,
        period_ms
    );


    if (fragment) {
        printf(" fragment");
    }

    if (glue) {
        printf(" glue");
    }

    if (corrupt_option) {
        printf(" corrupt");
    }

    printf(")\n");


    uint64_t frame_sequence =
        0;

    uint8_t door_state =
        0;


    while (!g_stop) {

        encoded_frame_t frames[2];

        size_t frame_count =
            glue ?
                2U :
                1U;


        uint8_t payload[
            MAX_PAYLOAD_SIZE
        ];

        size_t payload_len =
            0;


        for (
            size_t i = 0;
            i < frame_count;
            i++
        ) {

            frame_sequence++;


            /*
             * --corrupt:
             * one malformed frame every 4 frames.
             */
            bool corrupt_this =
                corrupt_option &&
                (
                    frame_sequence %
                    4U ==
                    0U
                );


            if (
                type ==
                NODE_DOOR
            ) {

                /*
                 * Door only emits when state changes.
                 */
                door_state =
                    door_state ?
                        0U :
                        1U;
            }


            if (
                build_frame(
                    type,
                    sensor_id,
                    door_state,
                    corrupt_this,
                    &frames[i]
                ) != 0
            ) {

                fprintf(
                    stderr,
                    "Failed to build frame\n"
                );

                close(fd);

                return EXIT_FAILURE;
            }


            if (
                payload_len +
                    frames[i].len >
                sizeof(payload)
            ) {

                close(fd);

                return EXIT_FAILURE;
            }


            memcpy(
                payload +
                    payload_len,
                frames[i].data,
                frames[i].len
            );


            payload_len +=
                frames[i].len;
        }


        int send_rc;


        if (
            type ==
            NODE_SMOKE_GAS
        ) {

            send_rc =
                send_sgp_with_retry(
                    fd,
                    payload,
                    payload_len,
                    frame_count,
                    fragment
                );

        } else {

            send_rc =
                send_payload(
                    fd,
                    payload,
                    payload_len,
                    fragment
                );
        }


        if (send_rc < 0) {

            if (!g_stop) {

                fprintf(
                    stderr,
                    "[SENSOR] connection lost\n"
                );
            }

            break;
        }


        for (
            size_t i = 0;
            i < frame_count;
            i++
        ) {

            if (frames[i].corrupt) {

                printf(
                    "[SENSOR] %s id=%u "
                    "sent malformed frame ts=%ld\n",
                    node_type_name(type),
                    sensor_id,
                    (long)frames[i].timestamp
                );

            } else {

                printf(
                    "[SENSOR] %s id=%u "
                    "value=%.2f ts=%ld\n",
                    node_type_name(type),
                    sensor_id,
                    frames[i].value,
                    (long)frames[i].timestamp
                );
            }
        }


        unsigned int delay =
            period_ms;


        if (
            type ==
                NODE_MOTION ||
            type ==
                NODE_DOOR
        ) {

            delay =
                random_event_delay(
                    period_ms
                );
        }


        if (
            sleep_ms(
                delay
            ) != 0
        ) {
            break;
        }
    }


    shutdown(
        fd,
        SHUT_RDWR
    );

    close(fd);


    printf(
        "[SENSOR] %s sensor %u stopped\n",
        node_type_name(type),
        sensor_id
    );


    return EXIT_SUCCESS;
}