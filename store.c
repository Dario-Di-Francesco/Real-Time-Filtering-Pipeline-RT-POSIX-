
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


#define NSEC_PER_SEC 1000000000ULL
#define OUTFILE "signal.txt"
#define T_SAMPLE 200000 //microsecondi

#define FILTER_QUEUE_NAME   "/filter_queue"
#define PEAK_COUNTER_QUEUE_NAME   "/peak_counter_queue"
#define BUTTERFILTER_QUEUE_NAME   "/butterfilter_queue"
#define CONF_STORE_QUEUE_NAME     "/conf_store_queue"
#define QUEUE_PERMISSIONS 0660
#define MAX_MESSAGES 10
#define MAX_CONF_MESSAGES 1
#define MAX_MSG_SIZE 64


struct periodic_thread {
    int index;
    struct timespec r;
    int period;
    int wcet;
    int priority;
};

 
static inline void timespec_add_us(struct timespec *t, uint64_t d)
{
    d *= 1000;
    t->tv_nsec += d;
    t->tv_sec += t->tv_nsec / NSEC_PER_SEC;
    t->tv_nsec %= NSEC_PER_SEC;
}

void wait_next_activation(struct periodic_thread * thd)
{
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &(thd->r), NULL);
    timespec_add_us(&(thd->r), thd->period);
}
 
void start_periodic_timer(struct periodic_thread * thd, uint64_t offs)
{
    clock_gettime(CLOCK_REALTIME, &(thd->r));
    timespec_add_us(&(thd->r), offs);
}



void * store(void * parameter){

	struct periodic_thread *th = (struct periodic_thread *) parameter;
	start_periodic_timer(th,T_SAMPLE);

	// Messaggio da prelevare dal filter
	char message [MAX_MSG_SIZE];
	char message_peak [MAX_MSG_SIZE];
	char choice[MAX_MSG_SIZE];

	int outfile;
	FILE * outfd;
	outfile = open(OUTFILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	outfd = fdopen(outfile, "w");

	if (outfd == NULL) {
  		perror("Unable to associate FILE stream with file descriptor");
    	return 0;
	}

	/* Coda */
	struct mq_attr attr;
	attr.mq_flags = 0;				
	attr.mq_maxmsg = MAX_MESSAGES;	
	attr.mq_msgsize = MAX_MSG_SIZE; 
	attr.mq_curmsgs = 0;

	float buffer=0;
	int head = 0, a=0, error=0;
	char delimiters[] = ",";
    char *token;

	// Apriamo la coda peak counter in lettura 
	mqd_t peak_counter_qd;
	if ((peak_counter_qd = mq_open (PEAK_COUNTER_QUEUE_NAME, O_RDONLY | O_NONBLOCK , QUEUE_PERMISSIONS,&attr)) == -1) {
		perror ("peak counter : mq_open (peak counter)");
		exit (1);
	}

	// Apriamo la coda filter in lettura 
	mqd_t filter_qd;
	if ((filter_qd = mq_open (FILTER_QUEUE_NAME, O_RDONLY , QUEUE_PERMISSIONS,&attr)) == -1) {
		perror ("filter : mq_open (filter)");
		exit (1);
	}
	// Apriamo la coda filter in lettura 
	mqd_t butterfilter_qd;
	if ((butterfilter_qd = mq_open (BUTTERFILTER_QUEUE_NAME, O_RDONLY  , QUEUE_PERMISSIONS,&attr)) == -1) {
		perror ("butterfilter : mq_open (butterfilter)");
		exit (1);
	}

	
	mqd_t conf_store_qd;
		
	while(1){
		wait_next_activation(th);
		// Apriamo la coda conf store in lettura 
		if ((conf_store_qd = mq_open (CONF_STORE_QUEUE_NAME, O_RDONLY | O_NONBLOCK, QUEUE_PERMISSIONS,&attr)) == -1) {
			perror ("conf_store_qd : mq_open (conf_store)");
		}
		//Riceviamo la scelta da conf 
		if (mq_receive(conf_store_qd, choice,MAX_MSG_SIZE,NULL) == -1){
			perror ("conf_store : mq_receive (conf_store)");
		}
		a = atoi(choice);
		switch (a) {
			case 0 :	
			default :
				error=0;
				if (mq_receive(filter_qd, message,MAX_MSG_SIZE,NULL) == -1){
				perror ("filter loop: mq_receive (filter)");
				error=1;
				}		
				break;
			case 1 :
				error=0;
				if (mq_receive(butterfilter_qd, message,MAX_MSG_SIZE,NULL) == -1){
				perror ("butterfilter loop: mq_receive (butterfilter)");
				error=1;					
				}		
				break;
			
		}
		if(error!=1){
			token = strtok(message, delimiters);
			while (token != NULL) {
				buffer = atof(token);
				fprintf(outfd, "%lf,", buffer);
				token = strtok(NULL, delimiters);
			}
			fprintf(outfd, "\n");
			fflush(outfd);			
			if (mq_receive(peak_counter_qd, message_peak,MAX_MSG_SIZE,NULL) == -1){
				perror ("peak counter: mq_receive (peak counter)");	
			}
			else{
				printf("Last Peaks : %s\n", message_peak); 
			}
		}

	if (mq_close(conf_store_qd) == -1) {
    	perror("conf store: mq_close (conf store)");
	}

	}

	if (mq_close(filter_qd) == -1) {
    	perror("filter: mq_close (filter)");
    	exit(1);
	}

	if (mq_close(butterfilter_qd) == -1) {
		perror("butterfilter: mq_close (filter)");
		exit(1);
	}

	return 0;
}

int main(int argc, char ** argv)
{
	pthread_t store_thread;
	// Definizione degli attributi per i thread
	struct sched_param myparam;
	pthread_attr_t myattr;
	pthread_attr_init(&myattr);
    pthread_attr_setschedpolicy(&myattr, SCHED_FIFO);
    pthread_attr_setinheritsched(&myattr, PTHREAD_EXPLICIT_SCHED);
	// PERIODIC THREAD
	struct periodic_thread store_th;
	store_th.period = T_SAMPLE;
	store_th.priority = 48;

	myparam.sched_priority = store_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam); 
    pthread_create(&store_thread, &myattr, store, (void*)&store_th);

	pthread_attr_destroy(&myattr);

	while (1) {
        if (getchar() == 'q') break;
    }
	pthread_cancel(store_thread);
	pthread_join(store_thread,0);

	printf("The store is stopped. Bye!\n");
	
	return 0;
}

