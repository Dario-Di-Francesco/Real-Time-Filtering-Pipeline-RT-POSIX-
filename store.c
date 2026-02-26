#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdint.h>
#include <mqueue.h>

/*
 * ===========================
 * Store task (RT-POSIX)
 * ===========================
 *
 * This program implements the "store" stage of a real-time pipeline.
 * It periodically:
 *  1) (optionally) reads a configuration message to select which filter stream to log
 *  2) reads one CSV message from the selected filter queue
 *  3) writes the CSV fields to a text file (signal.txt)
 *  4) reads the latest peak count from the peak_counter queue (if available) and prints it
 *
 * Termination: press 'q' on stdin to stop the thread.
 */

#define NSEC_PER_SEC 1000000000ULL

/* Output file where samples are logged (one line per received message) */
#define OUTFILE "signal.txt"

/* Period for the store task (microseconds) */
#define T_SAMPLE 200000  /* 200 ms -> 5 Hz */

/* Message queue names used by this process */
#define FILTER_QUEUE_NAME          "/filter_queue"
#define PEAK_COUNTER_QUEUE_NAME    "/peak_counter_queue"
#define BUTTERFILTER_QUEUE_NAME    "/butterfilter_queue"
#define CONF_STORE_QUEUE_NAME      "/conf_store_queue"

/* Message queue configuration */
#define QUEUE_PERMISSIONS 0660
#define MAX_MESSAGES 10          /* data queues capacity (filter streams) */
#define MAX_CONF_MESSAGES 1      /* (not used here, but intended for config queue) */
#define MAX_MSG_SIZE 64

/* ===========================
 * Periodic thread utilities
 * ===========================
 *
 * Minimal periodic scheduling helper based on absolute sleep:
 * - start_periodic_timer(): initializes the first activation time
 * - wait_next_activation(): sleeps until release time and schedules the next one
 */
struct periodic_thread {
    int index;              /* optional thread id (unused) */
    struct timespec r;      /* next release time */
    int period;             /* microseconds */
    int wcet;               /* worst-case exec time (unused) */
    int priority;           /* SCHED_FIFO priority */
};

/* Add microseconds to a timespec */
static inline void timespec_add_us(struct timespec *t, uint64_t d)
{
    d *= 1000; /* us -> ns */
    t->tv_nsec += d;
    t->tv_sec += t->tv_nsec / NSEC_PER_SEC;
    t->tv_nsec %= NSEC_PER_SEC;
}

/* Sleep until the next absolute activation, then compute the next release time */
void wait_next_activation(struct periodic_thread *thd)
{
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &(thd->r), NULL);
    timespec_add_us(&(thd->r), thd->period);
}

/* Initialize next release to "now + offs" */
void start_periodic_timer(struct periodic_thread *thd, uint64_t offs)
{
    clock_gettime(CLOCK_REALTIME, &(thd->r));
    timespec_add_us(&(thd->r), offs);
}

/* ===========================
 * store thread (5 Hz)
 * ===========================
 *
 * - Opens/creates the output file and writes logs (CSV) to it.
 * - Reads from two possible filter queues:
 *      * /filter_queue (moving-average)
 *      * /butterfilter_queue (Butterworth)
 * - Reads peak counts from /peak_counter_queue (non-blocking).
 * - Reads configuration /conf_store_queue to select which queue to read:
 *      0 -> /filter_queue
 *      1 -> /butterfilter_queue
 */
void *store(void *parameter)
{
    struct periodic_thread *th = (struct periodic_thread *)parameter;
    start_periodic_timer(th, T_SAMPLE);

    /* Buffers used to receive messages from MQs */
    char message[MAX_MSG_SIZE];       /* CSV sample message from filter/butterfilter */
    char message_peak[MAX_MSG_SIZE];  /* peak count message */
    char choice[MAX_MSG_SIZE];        /* configuration choice (as string) */

    /* Open output file (truncate each run) */
    int outfile;
    FILE *outfd;

    outfile = open(OUTFILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    outfd = fdopen(outfile, "w");

    if (outfd == NULL) {
        perror("Unable to associate FILE stream with file descriptor");
        return 0;
    }

    /* Message queue attributes used for mq_open */
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_MESSAGES;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    /* Variables used for parsing and selection */
    float buffer = 0;                 /* holds each parsed CSV field as float */
    int a = 0;                        /* selected filter id (0 or 1) */
    int error = 0;                    /* flag to skip write if receive fails */
    char delimiters[] = ",";          /* CSV separator */
    char *token;                      /* strtok token pointer */

    /*
     * Open peak_counter queue for reading, in NON-BLOCKING mode.
     * If there is no peak message available at a given cycle, mq_receive() fails.
     */
    mqd_t peak_counter_qd;
    if ((peak_counter_qd = mq_open(PEAK_COUNTER_QUEUE_NAME,
                                   O_RDONLY | O_NONBLOCK,
                                   QUEUE_PERMISSIONS, &attr)) == -1) {
        perror("peak counter: mq_open (peak counter)");
        exit(1);
    }

    /* Open filter queues for reading (blocking by default) */
    mqd_t filter_qd;
    if ((filter_qd = mq_open(FILTER_QUEUE_NAME,
                             O_RDONLY,
                             QUEUE_PERMISSIONS, &attr)) == -1) {
        perror("filter: mq_open (filter)");
        exit(1);
    }

    mqd_t butterfilter_qd;
    if ((butterfilter_qd = mq_open(BUTTERFILTER_QUEUE_NAME,
                                   O_RDONLY,
                                   QUEUE_PERMISSIONS, &attr)) == -1) {
        perror("butterfilter: mq_open (butterfilter)");
        exit(1);
    }

    /* Configuration queue descriptor (opened inside the loop in your code) */
    mqd_t conf_store_qd;

    while (1) {
        wait_next_activation(th);

        /*
         * Read configuration from /conf_store_queue (non-blocking).
         * NOTE: Opening the queue every cycle is functional but inefficient.
         * A more efficient approach is to open it once outside the loop.
         */
        if ((conf_store_qd = mq_open(CONF_STORE_QUEUE_NAME,
                                     O_RDONLY | O_NONBLOCK,
                                     QUEUE_PERMISSIONS, &attr)) == -1) {
            perror("conf_store_qd: mq_open (conf_store)");
        }

        /*
         * Receive the latest selection.
         * If queue is empty (non-blocking), mq_receive() fails and prints perror.
         * In that case, 'choice' might contain old data unless you handle it.
         */
        if (mq_receive(conf_store_qd, choice, MAX_MSG_SIZE, NULL) == -1) {
            perror("conf_store: mq_receive (conf_store)");
        }

        /* Convert selection string to integer:
         *  a = 0 -> read from /filter_queue
         *  a = 1 -> read from /butterfilter_queue
         */
        a = atoi(choice);

        /* Read one message from the selected data queue */
        switch (a) {
            case 0:
            default:
                error = 0;
                if (mq_receive(filter_qd, message, MAX_MSG_SIZE, NULL) == -1) {
                    perror("filter loop: mq_receive (filter)");
                    error = 1;
                }
                break;

            case 1:
                error = 0;
                if (mq_receive(butterfilter_qd, message, MAX_MSG_SIZE, NULL) == -1) {
                    perror("butterfilter loop: mq_receive (butterfilter)");
                    error = 1;
                }
                break;
        }

        /*
         * If a data message was received successfully:
         * - parse CSV fields separated by commas
         * - write each field to file followed by comma
         * - write newline and flush
         */
        if (error != 1) {
            token = strtok(message, delimiters);

            while (token != NULL) {
                buffer = atof(token);
                fprintf(outfd, "%lf,", buffer);
                token = strtok(NULL, delimiters);
            }

            fprintf(outfd, "\n");
            fflush(outfd);

            /*
             * Try to read peak count (non-blocking queue).
             * If a message is available, print it; otherwise just ignore.
             */
            if (mq_receive(peak_counter_qd, message_peak, MAX_MSG_SIZE, NULL) == -1) {
                perror("peak counter: mq_receive (peak counter)");
            } else {
                printf("Last Peaks : %s\n", message_peak);
            }
        }

        /* Close configuration queue descriptor at each iteration.
         * (Works, but again: opening once would be more efficient.)
         */
        if (mq_close(conf_store_qd) == -1) {
            perror("conf store: mq_close (conf store)");
        }
    }

    /* Unreachable in current code (infinite loop), but kept for completeness */
    if (mq_close(filter_qd) == -1) {
        perror("filter: mq_close (filter)");
        exit(1);
    }

    if (mq_close(butterfilter_qd) == -1) {
        perror("butterfilter: mq_close (butterfilter)");
        exit(1);
    }

    return 0;
}

/* ===========================
 * main
 * ===========================
 *
 * Creates the store thread with SCHED_FIFO real-time policy.
 * Priority chosen: 48.
 * Stops when user presses 'q', then cancels and joins the thread.
 */
int main(int argc, char **argv)
{
    pthread_t store_thread;

    /* Configure thread attributes for real-time scheduling */
    struct sched_param myparam;
    pthread_attr_t myattr;

    pthread_attr_init(&myattr);
    pthread_attr_setschedpolicy(&myattr, SCHED_FIFO);
    pthread_attr_setinheritsched(&myattr, PTHREAD_EXPLICIT_SCHED);

    /* Store periodic thread parameters */
    struct periodic_thread store_th;
    store_th.period = T_SAMPLE;
    store_th.priority = 48;

    myparam.sched_priority = store_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam);

    /* Create store thread */
    pthread_create(&store_thread, &myattr, store, (void *)&store_th);

    pthread_attr_destroy(&myattr);

    /* Stop condition: press 'q' on stdin */
    while (1) {
        if (getchar() == 'q') break;
    }

    /* Cancel and join */
    pthread_cancel(store_thread);
    pthread_join(store_thread, 0);

    printf("The store is stopped. Bye!\n");
    return 0;
}
