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
 * Real-Time Filtering Pipeline (RT-POSIX)
 * ===========================
 *
 * This program implements a real-time pipeline composed of multiple periodic threads:
 *
 *  - generator      @ 50 Hz (period = 20 ms)  -> generates a sinusoid + noise and writes into a shared struct
 *  - filter         @ 50 Hz (period = 20 ms)  -> moving-average (simple) filter, sends data to store via MQ
 *  - butterfilter   @ 50 Hz (period = 20 ms)  -> 2nd-order Butterworth filter, sends data to store via MQ
 *  - peak_counter   @ 1  Hz (period = 1 s)    -> counts peaks in last 50 filtered samples; sends count via MQ
 *
 * Shared data are protected using mutexes with priority inheritance (PTHREAD_PRIO_INHERIT)
 * to mitigate priority inversion.
 *
 * Communication:
 *  - generator -> (shared memory) -> filters
 *  - filters -> store via POSIX message queues (store is in a separate process/file in your project)
 *  - conf process (separate program) -> peak_counter via MQ to select which filtered signal to analyze
 */

#define NSEC_PER_SEC 1000000000ULL

/* Signal parameters */
#define SIG_HZ 1

/* Periods (microseconds) */
#define T_SAMPLE 20000          /* 20 ms -> 50 Hz */
#define T_SAMPLE_PEAK 1000000   /* 1 s  -> 1 Hz */

/* POSIX message queues names */
#define GENERATOR_QUEUE_NAME         "/generator_queue"      /* (not used in this file) */
#define FILTER_QUEUE_NAME            "/filter_queue"
#define PEAK_COUNTER_QUEUE_NAME      "/peak_counter_queue"
#define BUTTERFILTER_QUEUE_NAME      "/butterfilter_queue"
#define CONF_PEAK_QUEUE_NAME         "/conf_peak_queue"

/* MQ configuration */
#define QUEUE_PERMISSIONS 0660
#define MAX_MESSAGES 10            /* for filter/butterfilter -> store */
#define MAX_MESSAGES_P 1           /* for peak_counter -> store */
#define MAX_MSG_SIZE 64
#define DIM_BUFFER 50              /* window size used by peak_counter */

/*
 * 2nd-order Butterworth filter coefficients
 * cutoff at 2 Hz, sampling frequency 50 Hz
 */
#define BUTTERFILT_ORD 2
double b[3] = {0.0134, 0.0267, 0.0134};
double a[3] = {1.0000, -1.6475, 0.7009};

/* ===========================
 * Periodic thread utilities
 * ===========================
 *
 * We implement a minimal periodic-task helper:
 * - start_periodic_timer(): sets the first activation time
 * - wait_next_activation(): sleeps until absolute time, then updates next release
 */
struct periodic_thread {
    int index;              /* optional thread id (not used) */
    struct timespec r;      /* next release time (absolute) */
    int period;             /* period in microseconds */
    int wcet;               /* worst-case exec time (not used here) */
    int priority;           /* SCHED_FIFO priority */
};

/* Add microseconds to a timespec (helper) */
static inline void timespec_add_us(struct timespec *t, uint64_t d)
{
    d *= 1000; /* us -> ns */
    t->tv_nsec += d;
    t->tv_sec += t->tv_nsec / NSEC_PER_SEC;
    t->tv_nsec %= NSEC_PER_SEC;
}

/* Sleep until the next absolute release time, then schedule next one */
void wait_next_activation(struct periodic_thread *thd)
{
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &(thd->r), NULL);
    timespec_add_us(&(thd->r), thd->period);
}

/* Initialize release time to "now + offs" */
void start_periodic_timer(struct periodic_thread *thd, uint64_t offs)
{
    clock_gettime(CLOCK_REALTIME, &(thd->r));
    timespec_add_us(&(thd->r), offs);
}

/* ===========================
 * Shared data structures
 * ===========================
 */

/* Shared signal samples produced by generator.
 * sample[0] = clean signal
 * sample[1] = noisy signal
 * sample[2] = elapsed time (seconds)
 */
struct shared_double {
    double sample[3];
    pthread_mutex_t lock;
};
static struct shared_double sig_noise;

/* Shared buffers containing filtered values for peak_counter:
 * vect_value    = output of moving-average filter
 * vect_value_1  = output of Butterworth filter
 *
 * They are updated in chunks of DIM_BUFFER samples.
 */
struct shared_vect {
    double vect_value[DIM_BUFFER];
    double vect_value_1[DIM_BUFFER];
    pthread_mutex_t lock;
};
static struct shared_vect sig_filtered;

/* ===========================
 * generator thread (50 Hz)
 * ===========================
 *
 * Generates:
 *  - base sinusoid: sin(2*pi*SIG_HZ*t)
 *  - adds multiple cosine noise components
 * Writes (clean, noisy, time) into shared struct sig_noise (protected by mutex).
 */
void *generator(void *parameter)
{
    struct periodic_thread *th = (struct periodic_thread *)parameter;
    start_periodic_timer(th, T_SAMPLE);

    static struct timespec glob_time;
    float t_old_usec, t_new_usec, t, sig_val, sig_sin;

    /* We use CLOCK_MONOTONIC for measuring elapsed time (robust against time jumps) */
    clock_gettime(CLOCK_MONOTONIC, &glob_time);
    t_old_usec = (glob_time.tv_sec * 1000000) + glob_time.tv_nsec / 1000;

    while (1) {
        wait_next_activation(th);

        /* Update elapsed time t (seconds) */
        clock_gettime(CLOCK_MONOTONIC, &glob_time);
        t_new_usec = (glob_time.tv_sec * 1000000) + glob_time.tv_nsec / 1000;
        t = (t_new_usec - t_old_usec) / 1000000;

        /* Generate clean signal */
        sig_val = sin(2 * M_PI * SIG_HZ * t);

        /* Add deterministic noise components */
        sig_sin = sig_val + 0.5 * cos(2 * M_PI * 10 * t);
        sig_sin += 0.9 * cos(2 * M_PI * 4 * t);
        sig_sin += 0.9 * cos(2 * M_PI * 12 * t);
        sig_sin += 0.8 * cos(2 * M_PI * 15 * t);
        sig_sin += 0.7 * cos(2 * M_PI * 18 * t);

        /* Critical section: write shared samples */
        pthread_mutex_lock(&sig_noise.lock);
            sig_noise.sample[0] = sig_val;
            sig_noise.sample[1] = sig_sin;
            sig_noise.sample[2] = t;
        pthread_mutex_unlock(&sig_noise.lock);
    }
}

/* ===========================
 * filter thread (50 Hz)
 * ===========================
 *
 * Implements a simple moving average of order 2:
 *   y[k] = (x[k] + x[k-1]) / 2   (after the first sample)
 *
 * Sends each sample as CSV string to store through FILTER_QUEUE_NAME:
 *   "t,clean,noisy,filtered"
 *
 * Also fills a temporary buffer of DIM_BUFFER samples; when full,
 * it copies the buffer into sig_filtered.vect_value (for peak_counter).
 */
void *filter(void *parameter)
{
    struct periodic_thread *th = (struct periodic_thread *)parameter;
    start_periodic_timer(th, T_SAMPLE);

    /* Message to be sent to store (CSV) */
    char message[MAX_MSG_SIZE];

    /* Message queue attributes */
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_MESSAGES;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    /* Open output queue in write mode (non-blocking) */
    mqd_t filter_qd;
    if ((filter_qd = mq_open(FILTER_QUEUE_NAME,
                             O_WRONLY | O_CREAT | O_NONBLOCK,
                             QUEUE_PERMISSIONS, &attr)) == -1) {
        perror("filter queue: mq_open (filter)");
        exit(1);
    }

    /* State for moving-average filter (2-sample window) */
    static double vec_mean[2];
    vec_mean[0] = 0;

    /* NOTE: first_mean is used as a flag for the first sample.
     * It should be initialized to 0 to avoid undefined behavior.
     */
    float first_mean, cur, cur_noise, t = 0;
    float retval;
    int j = 0, index = 0;

    /* Temporary buffer to build windows of DIM_BUFFER samples */
    float buffer_temp[DIM_BUFFER];

    while (1) {
        wait_next_activation(th);

        /* Read shared signal (critical section) */
        pthread_mutex_lock(&sig_noise.lock);
            cur       = sig_noise.sample[0];
            cur_noise = sig_noise.sample[1];
            t         = sig_noise.sample[2];
        pthread_mutex_unlock(&sig_noise.lock);

        /* Shift previous sample */
        vec_mean[1] = vec_mean[0];
        vec_mean[0] = cur_noise;

        /* Compute filtered output */
        if (first_mean == 0) {
            retval = vec_mean[0];
            first_mean++;
        } else {
            retval = (vec_mean[0] + vec_mean[1]) / 2;
        }

        /* Fill local window buffer */
        buffer_temp[index] = retval;
        index++;

        /* When window is full, copy it to shared filtered buffer */
        if (index == DIM_BUFFER) {
            pthread_mutex_lock(&sig_filtered.lock);
                for (j = 0; j < DIM_BUFFER; j++) {
                    sig_filtered.vect_value[j] = buffer_temp[j];
                }
                index = 0;
            pthread_mutex_unlock(&sig_filtered.lock);
        }

        /* Send CSV line to store via MQ */
        sprintf(message, "%f,%f,%f,%f", t, cur, cur_noise, retval);

        if (mq_send(filter_qd, message, strlen(message) + 1, 0) == -1) {
            perror("Filter driver: Not able to send message to store");
        }
    }

    /* Unreachable in current code (infinite loop), but kept for completeness */
    if (mq_close(filter_qd) == -1) {
        perror("store: mq_close (filter)");
        exit(1);
    }
    return 0;
}

/* ===========================
 * butterfilter thread (50 Hz)
 * ===========================
 *
 * Implements a 2nd-order IIR Butterworth filter:
 *   y[k] = sum(b[i]*x[k-i]) - sum(a[i]*y[k-i]) (for i>0)
 *
 * Sends each sample as CSV to store through BUTTERFILTER_QUEUE_NAME:
 *   "t,clean,noisy,butter_filtered"
 *
 * Also fills sig_filtered.vect_value_1 with windows of DIM_BUFFER samples,
 * used by peak_counter depending on configuration.
 */
void *butterfilter(void *parameter)
{
    struct periodic_thread *th = (struct periodic_thread *)parameter;
    start_periodic_timer(th, T_SAMPLE);

    char message[MAX_MSG_SIZE];

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_MESSAGES;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    mqd_t butterfilter_qd;
    if ((butterfilter_qd = mq_open(BUTTERFILTER_QUEUE_NAME,
                                   O_WRONLY | O_CREAT | O_NONBLOCK,
                                   QUEUE_PERMISSIONS, &attr)) == -1) {
        perror("butterfilter queue: mq_open (butterfilter)");
        exit(1);
    }

    /* NOTE: first_mean is declared but unused here; it can be removed. */
    static double vec_mean[2];
    vec_mean[0] = 0;

    float first_mean, cur, cur_noise, t = 0;
    double retval1;
    int j = 0, i = 0, index = 0;
    float buffer_temp[DIM_BUFFER];

    while (1) {
        wait_next_activation(th);

        /* Read shared signal */
        pthread_mutex_lock(&sig_noise.lock);
            cur       = sig_noise.sample[0];
            cur_noise = sig_noise.sample[1];
            t         = sig_noise.sample[2];
        pthread_mutex_unlock(&sig_noise.lock);

        /* Filter internal state (kept static across activations) */
        static double in[BUTTERFILT_ORD + 1];
        static double out[BUTTERFILT_ORD + 1];

        /* Shift delay lines */
        for (i = BUTTERFILT_ORD; i > 0; --i) {
            in[i] = in[i - 1];
            out[i] = out[i - 1];
        }

        /* Input sample for IIR filter
         * NOTE: you are using 'cur' (clean). If you want to filter the noisy signal,
         * you likely want 'cur_noise' here instead.
         */
        in[0] = cur;

        /* Compute IIR output */
        retval1 = 0;
        for (i = 0; i < BUTTERFILT_ORD + 1; ++i) {
            retval1 += in[i] * b[i];
            if (i > 0)
                retval1 -= out[i] * a[i];
        }
        out[0] = retval1;

        /* Fill local window buffer */
        buffer_temp[index] = retval1;
        index++;

        /* When window is full, publish it for peak_counter */
        if (index == DIM_BUFFER) {
            pthread_mutex_lock(&sig_filtered.lock);
                for (j = 0; j < DIM_BUFFER; j++) {
                    sig_filtered.vect_value_1[j] = buffer_temp[j];
                }
            pthread_mutex_unlock(&sig_filtered.lock);

            j = 0;
            index = 0;
        }

        /* Send CSV line to store */
        sprintf(message, "%f,%f,%f,%f", t, cur, cur_noise, retval1);

        if (mq_send(butterfilter_qd, message, strlen(message) + 1, 0) == -1) {
            perror("ButterFilter driver: Not able to send message to store");
        }
    }

    if (mq_close(butterfilter_qd) == -1) {
        perror("store: mq_close (butterfilter)");
        exit(1);
    }
    return NULL;
}

/* ===========================
 * peak_counter thread (1 Hz)
 * ===========================
 *
 * Every second:
 *  1) reads configuration (0/1) from CONF_PEAK_QUEUE_NAME (non-blocking)
 *  2) copies the last DIM_BUFFER samples from the chosen filtered buffer:
 *       - 0 -> sig_filtered.vect_value
 *       - 1 -> sig_filtered.vect_value_1
 *  3) counts the number of "direction changes" (rough peak count)
 *  4) sends the count as string to PEAK_COUNTER_QUEUE_NAME
 *
 * The peak counting logic detects transitions from increasing to decreasing
 * (or vice versa) using 'flag'.
 */
void *peak_counter(void *parameter)
{
    struct periodic_thread *th = (struct periodic_thread *)parameter;
    start_periodic_timer(th, T_SAMPLE_PEAK);

    float signal_f[DIM_BUFFER];
    int i = 0, j = 0, a = 0;
    float curr;
    int count = 0, flag = 0;
    char message[MAX_MSG_SIZE];
    char choice[MAX_MSG_SIZE];

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = MAX_MESSAGES_P;
    attr.mq_msgsize = MAX_MSG_SIZE;
    attr.mq_curmsgs = 0;

    /* Output queue to store */
    mqd_t peak_counter_qd;
    if ((peak_counter_qd = mq_open(PEAK_COUNTER_QUEUE_NAME,
                                   O_WRONLY | O_CREAT | O_NONBLOCK,
                                   QUEUE_PERMISSIONS, &attr)) == -1) {
        perror("Peak counter queue: mq_open (peak_counter)");
        exit(1);
    }

    mqd_t conf_peak_qd;

    while (1) {
        wait_next_activation(th);

        /* Read configuration (which filter to analyze) */
        if ((conf_peak_qd = mq_open(CONF_PEAK_QUEUE_NAME,
                                    O_RDONLY | O_NONBLOCK,
                                    QUEUE_PERMISSIONS, &attr)) == -1) {
            perror("Conf peak queue: mq_open (conf peak)");
        }

        /* Non-blocking receive:
         * if no message is available, mq_receive() fails and prints perror.
         * In that case, 'a' remains the previous selection (or default 0).
         */
        if (mq_receive(conf_peak_qd, choice, MAX_MSG_SIZE, NULL) == -1) {
            perror("Conf peak driver: Not able to recieve message to conf peak");
        } else {
            a = atoi(choice);
        }

        /* Select which filtered signal buffer to copy */
        switch (a) {
            case 0:
            default:
                pthread_mutex_lock(&sig_filtered.lock);
                    for (j = 0; j < DIM_BUFFER; j++) {
                        signal_f[j] = sig_filtered.vect_value[j];
                    }
                pthread_mutex_unlock(&sig_filtered.lock);
                break;

            case 1:
                pthread_mutex_lock(&sig_filtered.lock);
                    for (j = 0; j < DIM_BUFFER; j++) {
                        signal_f[j] = sig_filtered.vect_value_1[j];
                    }
                pthread_mutex_unlock(&sig_filtered.lock);
                break;
        }

        /* Count peaks:
         * We count changes of slope sign using 'flag':
         *  flag =  1 -> increasing
         *  flag = -1 -> decreasing
         * Whenever we switch from increasing->decreasing or decreasing->increasing, we increment count.
         */
        if (j == DIM_BUFFER) {
            j = 0;

            for (i = 0; i < DIM_BUFFER; i++) {
                if (i == 0) {
                    curr = signal_f[0];
                } else {
                    if (signal_f[i] > curr) {
                        if (flag < 0) {
                            count++;
                        }
                        flag = 1;
                    } else {
                        if (signal_f[i] < curr) {
                            if (flag > 0) {
                                count++;
                            }
                            flag = -1;
                        }
                    }
                    curr = signal_f[i];
                }
            }
            i = 0;

            /* Send peak count to store */
            sprintf(message, "%d", count);

            if (mq_send(peak_counter_qd, message, strlen(message) + 1, 0) == -1) {
                perror("Peak counter: Not able to send message to store");
            }
            count = 0;
        }

        /* Close config queue descriptor each cycle (functional but not optimal).
         * It would be more efficient to open it once outside the loop and keep it open.
         */
        if (mq_close(conf_peak_qd) == -1) {
            perror("conf: mq_close (conf_peak)");
        }
    }

    if (mq_close(peak_counter_qd) == -1) {
        perror("store: mq_close (peak_counter)");
        exit(1);
    }
    return 0;
}

/* ===========================
 * main
 * ===========================
 *
 * Creates periodic threads with SCHED_FIFO priorities.
 * Uses mutexes with priority inheritance to mitigate priority inversion.
 * Stops when user presses 'q' on stdin, then cancels threads and unlinks MQs.
 */
int main(int argc, char **argv)
{
    pthread_t generator_thread;
    pthread_t filter_thread;
    pthread_t butterfilter_thread;
    pthread_t peak_counter_thread;

    /* Mutex attributes: enable priority inheritance protocol */
    pthread_mutexattr_t mymutexattr;
    pthread_mutexattr_init(&mymutexattr);
    pthread_mutexattr_setprotocol(&mymutexattr, PTHREAD_PRIO_INHERIT);

    /* Initialize mutexes guarding shared data */
    pthread_mutex_init(&sig_noise.lock, &mymutexattr);
    pthread_mutex_init(&sig_filtered.lock, &mymutexattr);

    /* Mutex attribute object can be destroyed after init */
    pthread_mutexattr_destroy(&mymutexattr);

    /* Thread attributes: explicit real-time scheduling */
    struct sched_param myparam;
    pthread_attr_t myattr;
    pthread_attr_init(&myattr);
    pthread_attr_setschedpolicy(&myattr, SCHED_FIFO);
    pthread_attr_setinheritsched(&myattr, PTHREAD_EXPLICIT_SCHED);

    /* GENERATOR THREAD (50 Hz, priority 50) */
    struct periodic_thread generator_th;
    generator_th.period = T_SAMPLE;
    generator_th.priority = 50;

    myparam.sched_priority = generator_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam);
    pthread_create(&generator_thread, &myattr, generator, (void *)&generator_th);

    /* FILTER THREAD (50 Hz, priority 50) */
    struct periodic_thread filter_th;
    filter_th.period = T_SAMPLE;
    filter_th.priority = 50;

    myparam.sched_priority = filter_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam);
    pthread_create(&filter_thread, &myattr, filter, (void *)&filter_th);

    /* BUTTERFILTER THREAD (50 Hz, priority 50) */
    struct periodic_thread butterfilter_th;
    butterfilter_th.period = T_SAMPLE;
    butterfilter_th.priority = 50;

    myparam.sched_priority = butterfilter_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam);
    pthread_create(&butterfilter_thread, &myattr, butterfilter, (void *)&butterfilter_th);

    /* PEAK COUNTER THREAD (1 Hz, priority 45) */
    struct periodic_thread peak_counter_th;
    peak_counter_th.period = T_SAMPLE_PEAK;
    peak_counter_th.priority = 45;

    myparam.sched_priority = peak_counter_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam);
    pthread_create(&peak_counter_thread, &myattr, peak_counter, (void *)&peak_counter_th);

    pthread_attr_destroy(&myattr);

    /* Simple stop condition: press 'q' in terminal */
    while (1) {
        if (getchar() == 'q') break;
    }

    /* Cancel and join threads */
    pthread_cancel(generator_thread);
    pthread_join(generator_thread, 0);

    pthread_cancel(filter_thread);
    pthread_join(filter_thread, 0);

    pthread_cancel(butterfilter_thread);
    pthread_join(butterfilter_thread, 0);

    pthread_cancel(peak_counter_thread);
    pthread_join(peak_counter_thread, 0);

    /* Cleanup mutexes */
    pthread_mutex_destroy(&sig_noise.lock);
    pthread_mutex_destroy(&sig_filtered.lock);

    /* Unlink message queues (remove from system namespace).
     * NOTE: Make sure no other process still needs them.
     */
    if (mq_unlink(FILTER_QUEUE_NAME) == -1) {
        perror("Main: mq_unlink filter queue");
        exit(1);
    }

    if (mq_unlink(BUTTERFILTER_QUEUE_NAME) == -1) {
        perror("Main: mq_unlink butterfilter queue");
        exit(1);
    }

    if (mq_unlink(PEAK_COUNTER_QUEUE_NAME) == -1) {
        perror("Main: mq_unlink counter peak queue");
        exit(1);
    }

    if (mq_unlink(CONF_PEAK_QUEUE_NAME) == -1) {
        perror("Main: mq_unlink conf peak queue");
        exit(1);
    }

    printf("The filter is stopped. Bye!\n");
    return 0;
}
