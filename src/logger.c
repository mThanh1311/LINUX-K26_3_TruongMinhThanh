#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


static int g_log_fd = -1;

static pthread_mutex_t g_log_mutex =
    PTHREAD_MUTEX_INITIALIZER;


static const char *source_to_string(log_source_t source)
{
    switch (source) {
    case LOG_MAIN:
        return "MAIN";

    case LOG_CONN:
        return "CONN";

    case LOG_DATA:
        return "DATA";

    case LOG_STOR:
        return "STOR";

    case LOG_WEB:
        return "WEB";

    default:
        return "MAIN";
    }
}


static int logger_process(void)
{
    int fifo_fd;
    FILE *fifo_stream;
    FILE *log_file;

    char event[LOG_EVENT_MAX];
    char timestamp[32];

    unsigned long seq = 1;


    
    // Open FIFO for reading
    // read/fgets will block when no log data is available
    fifo_fd = open(LOG_FIFO_PATH, O_RDONLY);

    if (fifo_fd == -1) {
        perror("open logFifo");
        return -1;
    }


    fifo_stream = fdopen(fifo_fd, "r");

    if (fifo_stream == NULL) {
        perror("fdopen logFifo");
        close(fifo_fd);
        return -1;
    }


    // Start a fresh log for each gateway run
    log_file = fopen(LOG_FILE_PATH, "w");

    if (log_file == NULL) {
        perror("fopen gateway.log");
        fclose(fifo_stream);
        return -1;
    }


    // Each FIFO event ends with '\n'
    while (fgets(
        event,
        sizeof(event),
        fifo_stream
    ) != NULL) {

        size_t len = strlen(event);

        if (len > 0 &&
            event[len - 1] == '\n') {
            event[len - 1] = '\0';
        }


        time_t now = time(NULL);
        struct tm local_time;

        localtime_r(
            &now,
            &local_time
        );

        strftime(
            timestamp,
            sizeof(timestamp),
            "%Y-%m-%d %H:%M:%S",
            &local_time
        );


        fprintf(
            log_file,
            "%lu %s %s\n",
            seq++,
            timestamp,
            event
        );

        
        // Make log visible immediately.
        fflush(log_file);
    }


    fclose(log_file);
    fclose(fifo_stream);

    return 0;
}


int logger_start(pid_t *child_pid)
{
    if (child_pid == NULL) {
        return -1;
    }


    // Create named FIFO.
    if (mkfifo(LOG_FIFO_PATH, 0666) == -1) {

        if (errno != EEXIST) {
            perror("mkfifo");
            return -1;
        }
    }


    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        unlink(LOG_FIFO_PATH);
        return -1;
    }


    
    // Child process
    if (pid == 0) {

        int status = logger_process();

        if (status == 0) {
            _exit(EXIT_SUCCESS);
        }

        _exit(EXIT_FAILURE);
    }


    
    // Parent process
    // This may wait briefly until the child opens the FIFO read end
    g_log_fd = open(
        LOG_FIFO_PATH,
        O_WRONLY
    );

    if (g_log_fd == -1) {
        perror("open logFifo writer");

        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);

        unlink(LOG_FIFO_PATH);

        return -1;
    }


    *child_pid = pid;

    return 0;
}


int logger_write(
    log_source_t source,
    const char *fmt,
    ...
)
{
    if (fmt == NULL ||
        g_log_fd == -1) {
        return -1;
    }


    char message[LOG_MESSAGE_MAX + 1];
    char event[LOG_EVENT_MAX];

    va_list args;

    va_start(args, fmt);

    int message_len = vsnprintf(
        message,
        sizeof(message),
        fmt,
        args
    );

    va_end(args);


    if (message_len < 0 ||
        message_len > LOG_MESSAGE_MAX) {
        return -1;
    }


    int event_len = snprintf(
        event,
        sizeof(event),
        "%s %s\n",
        source_to_string(source),
        message
    );

    if (event_len < 0 ||
        (size_t)event_len >= sizeof(event)) {
        return -1;
    }



    // Separate logging mutex
    // One event -> one write()
    pthread_mutex_lock(&g_log_mutex);

    ssize_t written;

    do {
        written = write(
            g_log_fd,
            event,
            (size_t)event_len
        );
    } while (
        written == -1 &&
        errno == EINTR
    );

    pthread_mutex_unlock(&g_log_mutex);


    if (written != event_len) {
        return -1;
    }

    return 0;
}


int logger_shutdown(pid_t child_pid)
{
    
    // All worker threads should already be joined before calling this.
    if (g_log_fd != -1) {
        close(g_log_fd);
        g_log_fd = -1;
    }


    // Closing the last writer causes EOF in logger_process().
    if (child_pid > 0) {
        waitpid(
            child_pid,
            NULL,
            0
        );
    }


    if (unlink(LOG_FIFO_PATH) == -1 &&
        errno != ENOENT) {

        perror("unlink logFifo");
        return -1;
    }


    return 0;
}