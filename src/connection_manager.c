#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection_manager.h"
#include "gateway.h"
#include "logger.h"
#include "protocols.h"
#include "sbuffer.h"


#define POLL_FD_COUNT \
    (PROTOCOL_COUNT + MAX_CONN)


/* ============================================================
 * TCP helpers
 * ============================================================ */

static int create_listen_socket(
    uint16_t port
)
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


    if (
        setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &opt,
            sizeof(opt)
        ) < 0
    ) {

        perror(
            "setsockopt"
        );

        close(fd);

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


    if (
        bind(
            fd,
            (struct sockaddr *)&addr,
            sizeof(addr)
        ) < 0
    ) {

        perror(
            "bind"
        );

        close(fd);

        return -1;
    }


    if (
        listen(
            fd,
            SOMAXCONN
        ) < 0
    ) {

        perror(
            "listen"
        );

        close(fd);

        return -1;
    }


    return fd;
}


/* ============================================================
 * RX buffer helpers
 * ============================================================ */

static void drop_rx_bytes(
    connection_t *conn,
    size_t count
)
{
    if (
        conn == NULL ||
        count == 0
    ) {

        return;
    }


    if (count >= conn->rx_len) {

        conn->rx_len = 0;

        return;
    }


    size_t remaining =
        conn->rx_len -
        count;


    memmove(
        conn->rx,
        conn->rx + count,
        remaining
    );


    conn->rx_len =
        remaining;
}


/* ============================================================
 * Reply helper
 * ============================================================ */

static int send_reply(
    connection_t *conn,
    parse_result_t result
)
{
    if (
        conn == NULL ||
        conn->adapter == NULL
    ) {

        return -1;
    }


    uint8_t reply[8];


    size_t reply_len =
        conn->adapter->build_reply(
            result,
            reply,
            sizeof(reply)
        );


    if (reply_len == 0) {
        return 0;
    }


    size_t sent_total = 0;


    while (
        sent_total <
        reply_len
    ) {

        ssize_t sent =
            send(
                conn->fd,
                reply + sent_total,
                reply_len - sent_total,
                0
            );


        if (sent < 0) {

            if (errno == EINTR) {
                continue;
            }

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


/* ============================================================
 * Parse error reason
 *
 * Logger's allowed reasons:
 *
 * accept error
 * too many connections
 * malformed first frame
 * unknown sensor
 * bad CRC
 * bad length
 * ============================================================ */

static const char *parse_error_reason(
    const connection_t *conn
)
{
    if (
        conn == NULL ||
        conn->adapter == NULL
    ) {

        return "bad length";
    }


    /*
     * Detect SGP CRC specifically.
     */
    if (
        strcmp(
            conn->adapter->name,
            "SGP"
        ) == 0 &&
        conn->rx_len >= 11 &&
        conn->rx[0] == 0x7E
    ) {

        uint16_t received_crc =
            ((uint16_t)conn->rx[9] << 8) |
            ((uint16_t)conn->rx[10]);


        uint16_t calculated_crc =
            sgp_crc16_ccitt_false(
                conn->rx + 1,
                8
            );


        if (
            received_crc !=
            calculated_crc
        ) {

            return "bad CRC";
        }
    }


    if (!conn->identified) {

        return
            "malformed first frame";
    }


    return
        "bad length";
}


/* ============================================================
 * Connection cleanup
 * ============================================================ */

static void close_connection(
    connmgr_t *manager,
    connection_t *conn,
    struct pollfd *pfd
)
{
    if (
        manager == NULL ||
        conn == NULL ||
        pfd == NULL
    ) {

        return;
    }


    if (conn->identified) {

        logger_write(
            LOG_CONN,
            "The sensor node with %u has closed the connection",
            conn->sensor_id
        );
    }


    if (conn->fd != -1) {

        close(
            conn->fd
        );


        if (manager->n_conns > 0) {

            manager->n_conns--;
        }
    }


    conn->fd =
        -1;

    conn->adapter =
        NULL;

    conn->rx_len =
        0;

    conn->identified =
        false;

    conn->sensor_id =
        0;

    conn->parse_errors =
        0;


    pfd->fd =
        -1;

    pfd->events =
        0;

    pfd->revents =
        0;
}


/* ============================================================
 * Adapter parse loop
 * ============================================================ */

static int process_connection_buffer(
    gateway_t *gateway,
    connection_t *conn
)
{
    if (
        gateway == NULL ||
        conn == NULL ||
        conn->adapter == NULL
    ) {

        return -1;
    }


    while (
        conn->rx_len > 0
    ) {

        sensor_data_t data;

        size_t consumed = 0;


        parse_result_t result =
            conn->adapter->parse(
                conn->rx,
                conn->rx_len,
                &data,
                &consumed
            );


        /*
         * TCP fragment:
         * keep bytes and wait for recv().
         */
        if (
            result ==
            PARSE_NEED_MORE
        ) {

            return 0;
        }


        /*
         * Malformed frame.
         */
        if (
            result ==
            PARSE_ERROR
        ) {

            const char *reason =
                parse_error_reason(
                    conn
                );


            logger_write(
                LOG_CONN,
                "Received malformed frame on %s port (%s)",
                conn->adapter->name,
                reason
            );


#ifdef DEBUG
            printf(
                "[CONN] %s parse error: %s "
                "(fd=%d consumed=%zu)\n",
                conn->adapter->name,
                reason,
                conn->fd,
                consumed
            );
#endif


            /*
             * SGP sends NAK here.
             * Other adapters return reply_len = 0.
             */
            if (
                send_reply(
                    conn,
                    PARSE_ERROR
                ) != 0
            ) {

                return -1;
            }


            /*
             * Defensive recovery:
             * adapter must always consume at least
             * one byte on PARSE_ERROR.
             */
            if (
                consumed == 0 ||
                consumed > conn->rx_len
            ) {

                consumed = 1;
            }


            drop_rx_bytes(
                conn,
                consumed
            );


            conn->parse_errors++;


            /*
             * Policy:
             *
             * 3 consecutive malformed frames
             * -> close connection.
             */
            if (
                conn->parse_errors >=
                MAX_PARSE_ERRORS
            ) {

#ifdef DEBUG
                printf(
                    "[CONN] Closing %s connection "
                    "after %u parse errors\n",
                    conn->adapter->name,
                    conn->parse_errors
                );
#endif

                return -1;
            }


            continue;
        }


        /*
         * PARSE_OK must consume bytes.
         */
        if (
            consumed == 0 ||
            consumed > conn->rx_len
        ) {

            return -1;
        }


        /*
         * First valid frame identifies
         * this TCP connection.
         */
        if (!conn->identified) {

            conn->identified =
                true;

            conn->sensor_id =
                data.id;


            logger_write(
                LOG_CONN,
                "A sensor node with %u has opened a new connection",
                conn->sensor_id
            );


#ifdef DEBUG
            printf(
                "[CONN] %s sensor identified: "
                "id=%u (fd=%d)\n",
                conn->adapter->name,
                conn->sensor_id,
                conn->fd
            );
#endif

        } else if (
            data.id !=
            conn->sensor_id
        ) {

            /*
             * One TCP connection belongs
             * to exactly one sensor ID.
             */
            logger_write(
                LOG_CONN,
                "Received malformed frame on %s port (unknown sensor)",
                conn->adapter->name
            );


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
            "[CONN] %s frame: "
            "id=%u type=%d value=%.2f ts=%ld\n",
            conn->adapter->name,
            data.id,
            data.type,
            data.value,
            (long)data.ts
        );
#endif


        /*
         * Normalize all protocols into the
         * same shared sensor_data_t pipeline.
         */
        if (
            sbuffer_insert(
                &gateway->sbuf,
                &data
            ) != 0
        ) {

            if (
                !gateway_should_stop(
                    gateway
                )
            ) {

                fprintf(
                    stderr,
                    "[CONN] Failed to insert sensor data\n"
                );
            }


            return -1;
        }


        /*
         * SGP sends ACK after successful
         * parsing + insertion.
         */
        if (
            send_reply(
                conn,
                PARSE_OK
            ) != 0
        ) {

            return -1;
        }


        drop_rx_bytes(
            conn,
            consumed
        );


        /*
         * Successful frame resets
         * consecutive-error counter.
         */
        conn->parse_errors =
            0;
    }


    return 0;
}


/* ============================================================
 * Connection Manager thread
 * ============================================================ */

void *connection_manager_thread(
    void *arg
)
{
    gateway_t *gateway =
        (gateway_t *)arg;


    connmgr_t *manager =
        &gateway->conn;


#ifdef DEBUG
    printf(
        "[CONN] Connection Manager started\n"
    );
#endif


    /*
     * Initialize manager.
     */
    manager->n_conns =
        0;


    for (
        size_t i = 0;
        i < PROTOCOL_COUNT;
        i++
    ) {

        manager->listen_fd[i] =
            -1;
    }


    for (
        size_t i = 0;
        i < MAX_CONN;
        i++
    ) {

        manager->conns[i].fd =
            -1;

        manager->conns[i].adapter =
            NULL;

        manager->conns[i].rx_len =
            0;

        manager->conns[i].identified =
            false;

        manager->conns[i].sensor_id =
            0;

        manager->conns[i].parse_errors =
            0;
    }


    /*
     * ------------------------------------------------
     * Open P, P+1, P+2, P+3
     * ------------------------------------------------
     */
    for (
        size_t i = 0;
        i < PROTOCOL_COUNT;
        i++
    ) {

        const protocol_adapter_t *adapter =
            ADAPTERS[i];


        uint16_t port =
            (uint16_t)(
                gateway->base_port +
                adapter->port_offset
            );


        manager->listen_fd[i] =
            create_listen_socket(
                port
            );


        if (
            manager->listen_fd[i] <
            0
        ) {

            logger_write(
                LOG_CONN,
                "Connection attempt failed on %s port (accept error)",
                adapter->name
            );


            for (
                size_t j = 0;
                j < i;
                j++
            ) {

                close(
                    manager->listen_fd[j]
                );

                manager->listen_fd[j] =
                    -1;
            }


            return NULL;
        }


#ifdef DEBUG
        printf(
            "[CONN] %s listening on TCP port %u\n",
            adapter->name,
            port
        );
#endif
    }


    /*
     * poll slots:
     *
     * [0..3]       listening sockets
     * [4..67]      sensor connections
     */
    struct pollfd
        pfds[POLL_FD_COUNT];


    for (
        size_t i = 0;
        i < POLL_FD_COUNT;
        i++
    ) {

        pfds[i].fd =
            -1;

        pfds[i].events =
            0;

        pfds[i].revents =
            0;
    }


    for (
        size_t i = 0;
        i < PROTOCOL_COUNT;
        i++
    ) {

        pfds[i].fd =
            manager->listen_fd[i];

        pfds[i].events =
            POLLIN;
    }


    /*
     * ------------------------------------------------
     * Main poll loop
     * ------------------------------------------------
     */
    while (
        !gateway_should_stop(
            gateway
        )
    ) {

        int ret =
            poll(
                pfds,
                POLL_FD_COUNT,
                500
            );


        if (ret < 0) {

            if (errno == EINTR) {
                continue;
            }


            perror(
                "poll"
            );

            break;
        }


        if (ret == 0) {
            continue;
        }


        /*
         * ============================================
         * Listening sockets
         * ============================================
         */
        for (
            size_t listener = 0;
            listener < PROTOCOL_COUNT;
            listener++
        ) {

            if (
                !(
                    pfds[listener].revents &
                    POLLIN
                )
            ) {

                continue;
            }


            const protocol_adapter_t *adapter =
                ADAPTERS[listener];


            struct sockaddr_in
                client_addr;


            socklen_t client_len =
                sizeof(client_addr);


            int client_fd =
                accept(
                    manager->listen_fd[listener],
                    (struct sockaddr *)&client_addr,
                    &client_len
                );


            if (client_fd < 0) {

                if (errno != EINTR) {

                    logger_write(
                        LOG_CONN,
                        "Connection attempt failed on %s port (accept error)",
                        adapter->name
                    );
                }


                continue;
            }


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
                    manager->conns[slot].fd ==
                    -1
                ) {

                    break;
                }
            }


            if (
                slot ==
                MAX_CONN
            ) {

                logger_write(
                    LOG_CONN,
                    "Connection attempt failed on %s port (too many connections)",
                    adapter->name
                );


                close(
                    client_fd
                );


                continue;
            }


            connection_t *conn =
                &manager->conns[slot];


            conn->fd =
                client_fd;

            conn->adapter =
                adapter;

            conn->rx_len =
                0;

            conn->identified =
                false;

            conn->sensor_id =
                0;

            conn->parse_errors =
                0;


            size_t poll_index =
                PROTOCOL_COUNT +
                slot;


            pfds[poll_index].fd =
                client_fd;

            pfds[poll_index].events =
                POLLIN;

            pfds[poll_index].revents =
                0;


            manager->n_conns++;


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
                    "[CONN] Accepted %s connection "
                    "from %s:%u "
                    "(fd=%d slot=%zu)\n",
                    adapter->name,
                    client_ip,
                    ntohs(
                        client_addr.sin_port
                    ),
                    client_fd,
                    slot
                );

            } else {

                printf(
                    "[CONN] Accepted %s connection "
                    "(fd=%d slot=%zu)\n",
                    adapter->name,
                    client_fd,
                    slot
                );
            }
#endif
        }


        /*
         * ============================================
         * Existing sensor connections
         * ============================================
         */
        for (
            size_t i = 0;
            i < MAX_CONN;
            i++
        ) {

            connection_t *conn =
                &manager->conns[i];


            if (
                conn->fd ==
                -1
            ) {

                continue;
            }


            size_t poll_index =
                PROTOCOL_COUNT +
                i;


            struct pollfd *pfd =
                &pfds[poll_index];


            short revents =
                pfd->revents;


            if (revents == 0) {
                continue;
            }


            /*
             * ----------------------------------------
             * Incoming bytes
             * ----------------------------------------
             */
            if (
                revents &
                POLLIN
            ) {

                size_t free_space =
                    RX_BUF_SIZE -
                    conn->rx_len;


                if (free_space == 0) {

                    logger_write(
                        LOG_CONN,
                        "Received malformed frame on %s port (bad length)",
                        conn->adapter->name
                    );


                    close_connection(
                        manager,
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


                if (received == 0) {

#ifdef DEBUG
                    printf(
                        "[CONN] %s sensor closed "
                        "(fd=%d)\n",
                        conn->adapter->name,
                        conn->fd
                    );
#endif


                    close_connection(
                        manager,
                        conn,
                        pfd
                    );


                    continue;
                }


                if (received < 0) {

                    if (errno == EINTR) {
                        continue;
                    }


                    close_connection(
                        manager,
                        conn,
                        pfd
                    );


                    continue;
                }


                conn->rx_len +=
                    (size_t)received;


#ifdef DEBUG
                printf(
                    "[CONN] %s received %zd bytes "
                    "(fd=%d buffered=%zu)\n",
                    conn->adapter->name,
                    received,
                    conn->fd,
                    conn->rx_len
                );
#endif


                if (
                    process_connection_buffer(
                        gateway,
                        conn
                    ) != 0
                ) {

                    close_connection(
                        manager,
                        conn,
                        pfd
                    );


                    continue;
                }
            }


            /*
             * ----------------------------------------
             * Socket error / hangup
             * ----------------------------------------
             */
            if (
                conn->fd != -1 &&
                (
                    (revents & POLLHUP) ||
                    (revents & POLLERR) ||
                    (revents & POLLNVAL)
                )
            ) {

                close_connection(
                    manager,
                    conn,
                    pfd
                );
            }
        }
    }


    /*
     * ------------------------------------------------
     * Shutdown connections
     * ------------------------------------------------
     */
    for (
        size_t i = 0;
        i < MAX_CONN;
        i++
    ) {

        if (
            manager->conns[i].fd !=
            -1
        ) {

            close_connection(
                manager,
                &manager->conns[i],
                &pfds[
                    PROTOCOL_COUNT +
                    i
                ]
            );
        }
    }


    /*
     * Shutdown listening sockets.
     */
    for (
        size_t i = 0;
        i < PROTOCOL_COUNT;
        i++
    ) {

        if (
            manager->listen_fd[i] !=
            -1
        ) {

            close(
                manager->listen_fd[i]
            );

            manager->listen_fd[i] =
                -1;
        }
    }


#ifdef DEBUG
    printf(
        "[CONN] Connection Manager stopped\n"
    );
#endif


    return NULL;
}