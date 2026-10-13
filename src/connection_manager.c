#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "connection_manager.h"
#include "gateway.h"
#include "logger.h"
#include "sbuffer.h"


#define TBP_FRAME_SIZE 18U


/*
 * TBP:
 *
 * byte 0..1   : uint16_t sensor id, little-endian
 * byte 2..9   : IEEE-754 double, little-endian
 * byte 10..17 : int64_t timestamp, little-endian
 */
_Static_assert(
    sizeof(double) == 8,
    "TBP requires an 8-byte double"
);


/*
 * Read uint16_t from little-endian byte stream.
 */
static uint16_t read_u16_le(const uint8_t *p)
{
    return
        ((uint16_t)p[0]) |
        ((uint16_t)p[1] << 8);
}


/*
 * Read uint64_t from little-endian byte stream.
 */
static uint64_t read_u64_le(const uint8_t *p)
{
    return
        ((uint64_t)p[0])       |
        ((uint64_t)p[1] << 8)  |
        ((uint64_t)p[2] << 16) |
        ((uint64_t)p[3] << 24) |
        ((uint64_t)p[4] << 32) |
        ((uint64_t)p[5] << 40) |
        ((uint64_t)p[6] << 48) |
        ((uint64_t)p[7] << 56);
}


/*
 * Decode one complete 18-byte TBP frame.
 */
static int decode_tbp_frame(
    const uint8_t *frame,
    sensor_data_t *data
)
{
    if (frame == NULL ||
        data == NULL) {

        return -1;
    }


    /*
     * Sensor ID.
     */
    data->id =
        read_u16_le(
            frame
        );


    /*
     * Temperature value.
     */
    uint64_t value_bits =
        read_u64_le(
            frame + 2
        );

    memcpy(
        &data->value,
        &value_bits,
        sizeof(data->value)
    );


    /*
     * Timestamp.
     */
    uint64_t ts_bits =
        read_u64_le(
            frame + 10
        );

    int64_t timestamp =
        (int64_t)ts_bits;


    data->type =
        SENSOR_TEMPERATURE;

    data->ts =
        (time_t)timestamp;


    return 0;
}


/*
 * Create TCP listening socket.
 */
static int create_listen_socket(uint16_t port)
{
    int fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (fd < 0) {
        perror("socket");
        return -1;
    }


    int opt = 1;

    if (setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &opt,
            sizeof(opt)
        ) < 0) {

        int saved_errno =
            errno;

        perror("setsockopt");

        close(fd);

        errno = saved_errno;

        return -1;
    }


    struct sockaddr_in addr;

    memset(
        &addr,
        0,
        sizeof(addr)
    );


    addr.sin_family =
        AF_INET;

    addr.sin_addr.s_addr =
        htonl(INADDR_ANY);

    addr.sin_port =
        htons(port);


    if (bind(
            fd,
            (struct sockaddr *)&addr,
            sizeof(addr)
        ) < 0) {

        int saved_errno =
            errno;

        perror("bind");

        close(fd);

        errno = saved_errno;

        return -1;
    }


    if (listen(
            fd,
            SOMAXCONN
        ) < 0) {

        int saved_errno =
            errno;

        perror("listen");

        close(fd);

        errno = saved_errno;

        return -1;
    }


    return fd;
}


/*
 * Close one sensor connection and reset its slot.
 */
static void close_connection(
    connection_t *connection,
    struct pollfd *pfd
)
{
    if (connection == NULL ||
        pfd == NULL) {

        return;
    }


    /*
     * Sensor ID is only known after
     * the first valid TBP frame.
     */
    if (connection->identified) {

        logger_write(
            LOG_CONN,
            "The sensor node with %u has closed the connection",
            connection->sensor_id
        );
    }


    if (connection->fd != -1) {

        close(
            connection->fd
        );
    }


    connection->fd = -1;

    connection->rx_len = 0;

    connection->identified = false;

    connection->sensor_id = 0;


    pfd->fd = -1;

    pfd->events = 0;

    pfd->revents = 0;
}


/*
 * Process every complete TBP frame currently
 * available in one connection receive buffer.
 */
static int process_tbp_frames(
    gateway_t *gateway,
    connection_t *conn
)
{
    if (gateway == NULL ||
        conn == NULL) {

        return -1;
    }


    while (
        conn->rx_len >=
        TBP_FRAME_SIZE
    ) {

        sensor_data_t data;


        /*
         * Decode first 18 bytes.
         */
        if (decode_tbp_frame(
                conn->rx,
                &data
            ) != 0) {

            return -1;
        }


        /*
         * First valid frame identifies
         * this TCP connection.
         */
        if (!conn->identified) {

            conn->identified = true;

            conn->sensor_id =
                data.id;


            logger_write(
                LOG_CONN,
                "A sensor node with %u has opened a new connection",
                conn->sensor_id
            );


#ifdef DEBUG
            printf(
                "[CONN] Sensor identified: "
                "id=%u (fd=%d)\n",
                conn->sensor_id,
                conn->fd
            );
#endif

        } else if (
            data.id !=
            conn->sensor_id
        ) {

            /*
             * One TCP connection represents
             * one sensor node.
             */
#ifdef DEBUG
            printf(
                "[CONN] Sensor ID mismatch: "
                "expected=%u received=%u\n",
                conn->sensor_id,
                data.id
            );
#endif

            return -1;
        }


#ifdef DEBUG
        printf(
            "[CONN] TBP frame: "
            "id=%u value=%.2f ts=%ld\n",
            data.id,
            data.value,
            (long)data.ts
        );
#endif


        /*
         * Put normalized sensor data into
         * the single shared sbuffer.
         *
         * Only consume the TCP frame after
         * the insert succeeds.
         */
        if (sbuffer_insert(
                &gateway->sbuf,
                &data
            ) != 0) {

            if (!gateway_should_stop(
                    gateway
                )) {

                fprintf(
                    stderr,
                    "[CONN] Failed to insert sensor data into sbuffer\n"
                );
            }

            return -1;
        }


        /*
         * Remove exactly one consumed
         * 18-byte frame from rx[].
         */
        size_t remaining =
            conn->rx_len -
            TBP_FRAME_SIZE;


        if (remaining > 0) {

            memmove(
                conn->rx,
                conn->rx +
                    TBP_FRAME_SIZE,
                remaining
            );
        }


        conn->rx_len =
            remaining;
    }


    return 0;
}


void *connection_manager_thread(void *arg)
{
    gateway_t *gateway =
        (gateway_t *)arg;


#ifdef DEBUG
    printf(
        "[CONN] Connection Manager started\n"
    );
#endif


    logger_write(
        LOG_CONN,
        "Connection Manager started"
    );


    /*
     * Step 3:
     * Temperature/TBP only.
     */
    int listen_fd =
        create_listen_socket(
            gateway->base_port
        );


    if (listen_fd < 0) {

        int saved_errno =
            errno;

        logger_write(
            LOG_CONN,
            "Connection attempt failed on TBP port (%s)",
            strerror(saved_errno)
        );

        return NULL;
    }


#ifdef DEBUG
    printf(
        "[CONN] Listening on TCP port %u\n",
        gateway->base_port
    );
#endif


    /*
     * pfds[0] = listening socket
     *
     * pfds[1..MAX_CONN]
     * = sensor client sockets
     */
    struct pollfd pfds[
        MAX_CONN + 1
    ];


    connection_t connections[
        MAX_CONN
    ];


    /*
     * Initialize poll descriptors.
     */
    for (
        size_t i = 0;
        i < MAX_CONN + 1;
        i++
    ) {

        pfds[i].fd = -1;

        pfds[i].events = 0;

        pfds[i].revents = 0;
    }


    /*
     * Initialize connection slots.
     */
    for (
        size_t i = 0;
        i < MAX_CONN;
        i++
    ) {

        connections[i].fd = -1;

        connections[i].rx_len = 0;

        connections[i].identified =
            false;

        connections[i].sensor_id = 0;
    }


    /*
     * Poll slot 0 is always
     * the listening socket.
     */
    pfds[0].fd =
        listen_fd;

    pfds[0].events =
        POLLIN;


    while (!gateway_should_stop(
        gateway
    )) {

        int ret = poll(
            pfds,
            MAX_CONN + 1,
            500
        );


        if (ret < 0) {

            if (errno == EINTR) {
                continue;
            }

            perror("poll");

            break;
        }


        if (ret == 0) {
            continue;
        }


        /*
         * Listening socket failed.
         */
        if (
            pfds[0].revents &
            (
                POLLERR |
                POLLHUP |
                POLLNVAL
            )
        ) {

            fprintf(
                stderr,
                "[CONN] Listening socket error\n"
            );

            break;
        }


        /*
         * =================================================
         * New incoming connection
         * =================================================
         */
        if (
            pfds[0].revents &
            POLLIN
        ) {

            struct sockaddr_in
                client_addr;


            socklen_t client_len =
                sizeof(client_addr);


            int client_fd = accept(
                listen_fd,
                (struct sockaddr *)
                    &client_addr,
                &client_len
            );


            if (client_fd < 0) {

                if (errno != EINTR) {
                    perror("accept");
                }

            } else {

                /*
                 * Find free connection slot.
                 */
                size_t slot;


                for (
                    slot = 0;
                    slot < MAX_CONN;
                    slot++
                ) {

                    if (
                        connections[slot].fd ==
                        -1
                    ) {

                        break;
                    }
                }


                /*
                 * No free slot.
                 */
                if (slot == MAX_CONN) {

#ifdef DEBUG
                    printf(
                        "[CONN] Too many sensor connections\n"
                    );
#endif

                    close(
                        client_fd
                    );

                } else {

                    connection_t *conn =
                        &connections[slot];


                    conn->fd =
                        client_fd;

                    conn->rx_len =
                        0;

                    conn->identified =
                        false;

                    conn->sensor_id =
                        0;


                    pfds[slot + 1].fd =
                        client_fd;

                    pfds[slot + 1].events =
                        POLLIN;

                    pfds[slot + 1].revents =
                        0;


#ifdef DEBUG
                    char client_ip[
                        INET_ADDRSTRLEN
                    ];


                    const char *ip_result =
                        inet_ntop(
                            AF_INET,
                            &client_addr.sin_addr,
                            client_ip,
                            sizeof(client_ip)
                        );


                    if (ip_result != NULL) {

                        printf(
                            "[CONN] Accepted connection "
                            "from %s:%u "
                            "(fd=%d, slot=%zu)\n",
                            client_ip,
                            ntohs(
                                client_addr.sin_port
                            ),
                            client_fd,
                            slot
                        );

                    } else {

                        printf(
                            "[CONN] Accepted connection "
                            "(fd=%d, slot=%zu)\n",
                            client_fd,
                            slot
                        );
                    }
#endif
                }
            }
        }


        /*
         * =================================================
         * Existing sensor connections
         * =================================================
         */
        for (
            size_t i = 0;
            i < MAX_CONN;
            i++
        ) {

            connection_t *conn =
                &connections[i];


            if (conn->fd == -1) {
                continue;
            }


            struct pollfd *pfd =
                &pfds[i + 1];


            short revents =
                pfd->revents;


            if (revents == 0) {
                continue;
            }


            /*
             * ---------------------------------------------
             * Sensor has TCP bytes available
             * ---------------------------------------------
             */
            if (revents & POLLIN) {

                size_t free_space =
                    RX_BUF_SIZE -
                    conn->rx_len;


                /*
                 * Buffer cannot accept more bytes.
                 */
                if (free_space == 0) {

#ifdef DEBUG
                    printf(
                        "[CONN] RX buffer full "
                        "(fd=%d, slot=%zu)\n",
                        conn->fd,
                        i
                    );
#endif

                    close_connection(
                        conn,
                        pfd
                    );

                    continue;
                }


                ssize_t received =
                    recv(
                        conn->fd,
                        conn->rx +
                            conn->rx_len,
                        free_space,
                        0
                    );


                /*
                 * Peer closed connection.
                 */
                if (received == 0) {

#ifdef DEBUG
                    printf(
                        "[CONN] Sensor closed connection "
                        "(fd=%d, slot=%zu)\n",
                        conn->fd,
                        i
                    );
#endif


                    close_connection(
                        conn,
                        pfd
                    );

                    continue;
                }


                /*
                 * recv() error.
                 */
                if (received < 0) {

                    if (errno == EINTR) {
                        continue;
                    }


                    perror(
                        "recv"
                    );


                    close_connection(
                        conn,
                        pfd
                    );

                    continue;
                }


                /*
                 * Append new bytes to this
                 * connection's private rx buffer.
                 */
                conn->rx_len +=
                    (size_t)received;


#ifdef DEBUG
                printf(
                    "[CONN] Received %zd bytes "
                    "(fd=%d, slot=%zu, buffered=%zu)\n",
                    received,
                    conn->fd,
                    i,
                    conn->rx_len
                );
#endif


                /*
                 * Decode every complete TBP frame.
                 */
                if (process_tbp_frames(
                        gateway,
                        conn
                    ) != 0) {

                    if (!gateway_should_stop(
                            gateway
                        )) {

                        fprintf(
                            stderr,
                            "[CONN] Invalid or failed TBP processing "
                            "(fd=%d, slot=%zu)\n",
                            conn->fd,
                            i
                        );
                    }


                    // Protocol failure: discard this connection.
                    close_connection(
                        conn,
                        pfd
                    );

                    continue;
                }
            }


            // Hangup / socket error
            if (
                conn->fd != -1 &&
                (
                    (revents & POLLHUP) ||
                    (revents & POLLERR) ||
                    (revents & POLLNVAL)
                )
            ) {

#ifdef DEBUG
                printf(
                    "[CONN] Sensor disconnected "
                    "(fd=%d, slot=%zu)\n",
                    conn->fd,
                    i
                );
#endif


                close_connection(
                    conn,
                    pfd
                );
            }
        }
    }

    // Gateway shutdown
    for (
        size_t i = 0;
        i < MAX_CONN;
        i++
    ) {

        if (
            connections[i].fd !=
            -1
        ) {

            close_connection(
                &connections[i],
                &pfds[i + 1]
            );
        }
    }


    close(
        listen_fd
    );


#ifdef DEBUG
    printf(
        "[CONN] Connection Manager stopped\n"
    );
#endif


    return NULL;
}