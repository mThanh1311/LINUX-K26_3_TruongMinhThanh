#include "sbuffer.h"

#include <stdlib.h>


/*
 * Find the first node that has not yet been read
 * by the requested reader.
 *
 * IMPORTANT:
 * Caller must hold buffer->lock.
 */
static sbuffer_node_t *find_unread_node(
    sbuffer_t *buffer,
    sbuffer_reader_t reader
)
{
    sbuffer_node_t *node = buffer->head;

    while (node != NULL) {

        if (reader == SBUFFER_READER_DATA &&
            !node->read_by_data) {
            return node;
        }

        if (reader == SBUFFER_READER_STORAGE &&
            !node->read_by_storage) {
            return node;
        }

        node = node->next;
    }

    return NULL;
}


/*
 * Remove fully-consumed nodes from the head.
 *
 * A node may only be removed after BOTH:
 *
 *   read_by_data    == true
 *   read_by_storage == true
 *
 * Caller must hold buffer->lock.
 */
static void remove_consumed_nodes(sbuffer_t *buffer)
{
    while (
        buffer->head != NULL &&
        buffer->head->read_by_data &&
        buffer->head->read_by_storage
    ) {

        sbuffer_node_t *old_head = buffer->head;

        buffer->head = old_head->next;

        if (buffer->head == NULL) {
            buffer->tail = NULL;
        }

        if (buffer->size > 0) {
            buffer->size--;
        }

        free(old_head);
    }
}


int sbuffer_init(sbuffer_t *buffer)
{
    if (buffer == NULL) {
        return -1;
    }

    buffer->head = NULL;
    buffer->tail = NULL;
    buffer->size = 0;
    buffer->shutdown = false;


    if (pthread_mutex_init(
            &buffer->lock,
            NULL
        ) != 0) {

        return -1;
    }


    if (pthread_cond_init(
            &buffer->not_empty,
            NULL
        ) != 0) {

        pthread_mutex_destroy(
            &buffer->lock
        );

        return -1;
    }


    return 0;
}


int sbuffer_insert(
    sbuffer_t *buffer,
    const sensor_data_t *data
)
{
    if (buffer == NULL ||
        data == NULL) {
        return -1;
    }


    /*
     * Allocate before taking the lock.
     *
     * malloc() may be relatively slow, so we avoid
     * holding the sbuffer mutex while allocating.
     */
    sbuffer_node_t *node =
        malloc(sizeof(*node));

    if (node == NULL) {
        return -1;
    }


    node->data = *data;

    node->read_by_data = false;
    node->read_by_storage = false;

    node->next = NULL;


    if (pthread_mutex_lock(
            &buffer->lock
        ) != 0) {

        free(node);
        return -1;
    }


    /*
     * Do not accept new measurements after shutdown
     * has been requested.
     */
    if (buffer->shutdown) {

        pthread_mutex_unlock(
            &buffer->lock
        );

        free(node);

        return -1;
    }


    /*
     * Append node to tail.
     */
    if (buffer->tail == NULL) {

        /*
         * Empty buffer.
         */
        buffer->head = node;
        buffer->tail = node;

    } else {

        buffer->tail->next = node;
        buffer->tail = node;
    }


    buffer->size++;


    /*
     * BOTH Data Manager and Storage Manager may
     * currently be sleeping.
     *
     * One measurement must be read by BOTH readers,
     * therefore broadcast instead of signal.
     */
    pthread_cond_broadcast(
        &buffer->not_empty
    );


    pthread_mutex_unlock(
        &buffer->lock
    );


    return 0;
}


int sbuffer_read(
    sbuffer_t *buffer,
    sbuffer_reader_t reader,
    sensor_data_t *data
)
{
    if (buffer == NULL ||
        data == NULL) {
        return -1;
    }


    if (reader != SBUFFER_READER_DATA &&
        reader != SBUFFER_READER_STORAGE) {

        return -1;
    }


    if (pthread_mutex_lock(
            &buffer->lock
        ) != 0) {

        return -1;
    }


    for (;;) {

        sbuffer_node_t *node =
            find_unread_node(
                buffer,
                reader
            );


        /*
         * There is unread data for this reader.
         */
        if (node != NULL) {

            /*
             * Copy sensor data while node is still valid
             * and protected by the mutex.
             */
            *data = node->data;


            if (reader == SBUFFER_READER_DATA) {

                node->read_by_data = true;

            } else {

                node->read_by_storage = true;
            }


            /*
             * Free any nodes at the head for which
             * both readers have completed processing.
             */
            remove_consumed_nodes(buffer);


            pthread_mutex_unlock(
                &buffer->lock
            );


            return 0;
        }


        /*
         * No unread data remains for this reader.
         *
         * If shutdown has been requested, terminate
         * the consumer cleanly.
         */
        if (buffer->shutdown) {

            pthread_mutex_unlock(
                &buffer->lock
            );

            return 1;
        }


        /*
         * No data available.
         *
         * pthread_cond_wait():
         *
         * 1. atomically releases buffer->lock
         * 2. blocks this thread
         * 3. wakes after broadcast/signal
         * 4. re-acquires buffer->lock before returning
         *
         * The surrounding loop is required because
         * condition variables may wake spuriously.
         */
        int rc = pthread_cond_wait(
            &buffer->not_empty,
            &buffer->lock
        );


        if (rc != 0) {

            pthread_mutex_unlock(
                &buffer->lock
            );

            return -1;
        }
    }
}


void sbuffer_shutdown(sbuffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }


    pthread_mutex_lock(
        &buffer->lock
    );


    buffer->shutdown = true;


    // Wake all blocked readers so that they can bserve shutdown and exit.
    pthread_cond_broadcast(
        &buffer->not_empty
    );


    pthread_mutex_unlock(
        &buffer->lock
    );
}


void sbuffer_destroy(sbuffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }


    pthread_mutex_lock(
        &buffer->lock
    );


    /*
     * Free every node still remaining.
     *
     * Normally this should only contain data that
     * had not been fully consumed before shutdown.
     */
    sbuffer_node_t *node = buffer->head;

    while (node != NULL) {

        sbuffer_node_t *next =
            node->next;

        free(node);

        node = next;
    }


    buffer->head = NULL;
    buffer->tail = NULL;
    buffer->size = 0;


    pthread_mutex_unlock(
        &buffer->lock
    );


    pthread_cond_destroy(
        &buffer->not_empty
    );

    pthread_mutex_destroy(
        &buffer->lock
    );
}