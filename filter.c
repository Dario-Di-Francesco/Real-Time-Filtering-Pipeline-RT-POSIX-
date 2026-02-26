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
#define SIG_HZ 1
#define T_SAMPLE 20000 //microsecondi
#define T_SAMPLE_PEAK 1000000 //microsecondi

#define GENERATOR_QUEUE_NAME   "/generator_queue"
#define FILTER_QUEUE_NAME   "/filter_queue"
#define PEAK_COUNTER_QUEUE_NAME   "/peak_counter_queue"
#define BUTTERFILTER_QUEUE_NAME		"/butterfilter_queue"
#define CONF_PEAK_QUEUE_NAME     "/conf_peak_queue"
#define QUEUE_PERMISSIONS 0660
#define MAX_MESSAGES 10
#define MAX_MESSAGES_P 1
#define MAX_MSG_SIZE 64
#define DIM_BUFFER 50


// 2nd-order Butterw. filter, cutoff at 2Hz @ fc = 50Hz
#define BUTTERFILT_ORD 2
double b [3] = {0.0134,    0.0267,    0.0134};
double a [3] = {1.0000,   -1.6475,    0.7009};

 

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

struct shared_double {
	double sample[3]; //sample[0] = segnale ; sample[1] = segnale+rumore; sample[2] =  tempo; 
	pthread_mutex_t lock;
};
static struct shared_double sig_noise;

struct shared_vect {
	double vect_value[DIM_BUFFER];	//array filter
	double vect_value_1[DIM_BUFFER]; //array butterfilter
	pthread_mutex_t lock;
};
static struct shared_vect sig_filtered;


void * generator(void * parameter){

	struct periodic_thread *th = (struct periodic_thread *) parameter;
	start_periodic_timer(th,T_SAMPLE);

	static struct timespec glob_time;
	float t_old_usec, t_new_usec, t, sig_val, sig_sin;



	clock_gettime(CLOCK_MONOTONIC, &glob_time);
	t_old_usec=(glob_time.tv_sec*1000000)+glob_time.tv_nsec/1000;
	
	while(1){
		
		wait_next_activation(th);

		//prelevo e calcolo tempo trascorso rispetto a t_old_usec
		clock_gettime(CLOCK_MONOTONIC, &glob_time);
		t_new_usec =(glob_time.tv_sec*1000000)+glob_time.tv_nsec/1000;
		t =(t_new_usec - t_old_usec)/1000000;

		// Generate signal
		sig_val = sin(2*M_PI*SIG_HZ*t); 
		
		// Add noise to signal
		sig_sin = sig_val + 0.5*cos(2*M_PI*10*t);
		sig_sin += 0.9*cos(2*M_PI*4*t);
		sig_sin += 0.9*cos(2*M_PI*12*t);
		sig_sin += 0.8*cos(2*M_PI*15*t);
		sig_sin += 0.7*cos(2*M_PI*18*t);

		//sezione critica 
		pthread_mutex_lock(&sig_noise.lock);
			sig_noise.sample[0] = sig_val;
			sig_noise.sample[1] = sig_sin;
			sig_noise.sample[2] = t;
		pthread_mutex_unlock(&sig_noise.lock);
	}

}


void * filter(void * parameter){

	struct periodic_thread *th = (struct periodic_thread *) parameter;
	start_periodic_timer(th,T_SAMPLE);

	// Messaggio da inviare a store
	char message [MAX_MSG_SIZE];

	// Coda 
	struct mq_attr attr;
	attr.mq_flags = 0;				
	attr.mq_maxmsg = MAX_MESSAGES;	
	attr.mq_msgsize = MAX_MSG_SIZE; 
	attr.mq_curmsgs = 0;

	// Apriamo la coda filter in scrittura 
	mqd_t filter_qd;
	if ((filter_qd = mq_open (FILTER_QUEUE_NAME, O_WRONLY | O_CREAT | O_NONBLOCK , QUEUE_PERMISSIONS, &attr)) == -1) {
		perror (" filter queue: mq_open (filter)");
		exit (1);
	}
	
	static double vec_mean[2];
	vec_mean[0] = 0;
	float first_mean, cur, cur_noise, t = 0;
	float retval;
	int j=0, index=0;
	float buffer_temp[DIM_BUFFER];

	while(1){
		wait_next_activation(th);

		//sezione critica 
		pthread_mutex_lock(&sig_noise.lock);
			cur = sig_noise.sample[0];
			cur_noise = sig_noise.sample[1];
			t = sig_noise.sample[2];
		pthread_mutex_unlock(&sig_noise.lock);

		// Perform sample shift
		vec_mean[1] = vec_mean[0];
		vec_mean[0] = cur_noise;


		// Compute filtered value
		if (first_mean == 0){
			retval = vec_mean[0];
			first_mean ++;
		}
		else{
			retval = (vec_mean[0] + vec_mean[1])/2;	
		}
		buffer_temp[index]=retval;
		index++;

		if(index==DIM_BUFFER){
			pthread_mutex_lock(&sig_filtered.lock);
				for(j=0;j<DIM_BUFFER;j++){
					sig_filtered.vect_value[j]=buffer_temp[j];
				}
				index=0;
			pthread_mutex_unlock(&sig_filtered.lock);
		}
		
		sprintf (message, "%f,%f,%f,%f", t, cur, cur_noise,retval);
		//printf("message : %s\n",message);
		if (mq_send (filter_qd, message, strlen (message) + 1, 0) == -1) {
		    perror ("Filter driver: Not able to send message to store");
		}
		
	}
	if (mq_close(filter_qd) == -1) {
    perror("store: mq_close (filter)");
    exit(1);
	}

	return 0;
}


void * butterfilter(void * parameter){

	struct periodic_thread *th = (struct periodic_thread *) parameter;
	start_periodic_timer(th,T_SAMPLE);

	// Messaggio da inviare al store
	char message [MAX_MSG_SIZE];

	// Coda 
	struct mq_attr attr;
	attr.mq_flags = 0;				
	attr.mq_maxmsg = MAX_MESSAGES;	
	attr.mq_msgsize = MAX_MSG_SIZE; 
	attr.mq_curmsgs = 0;

	// Apriamo la coda filter in scrittura 
	mqd_t butterfilter_qd;
	if ((butterfilter_qd = mq_open (BUTTERFILTER_QUEUE_NAME, O_WRONLY | O_CREAT | O_NONBLOCK , QUEUE_PERMISSIONS, &attr)) == -1) {
		perror (" butterfilter queue: mq_open (butterfilter)");
		exit (1);
	}
	
	static double vec_mean[2];
	vec_mean[0] = 0;
	float first_mean, cur, cur_noise, t = 0;
	double retval1;
	int j=0,i=0, index=0;
	float buffer_temp[DIM_BUFFER];

	while(1){
		wait_next_activation(th);

		//sezione critica 
		pthread_mutex_lock(&sig_noise.lock);
			cur = sig_noise.sample[0];
			cur_noise = sig_noise.sample[1];
			t = sig_noise.sample[2];
		pthread_mutex_unlock(&sig_noise.lock);

		static double in[BUTTERFILT_ORD+1];
		static double out[BUTTERFILT_ORD+1];
		// Perform sample shift
		for (i = BUTTERFILT_ORD; i > 0; --i) {
			in[i] = in[i-1];
			out[i] = out[i-1];
		}
		in[0] = cur;	

		// Compute filtered value
		retval1 = 0;
		for (i = 0; i < BUTTERFILT_ORD+1; ++i) {
			retval1 += in[i] * b[i];
		if (i > 0)
			retval1 -= out[i] * a[i];
		}
		out[0] = retval1;
		buffer_temp[index]=retval1;
		index++;

		if(index==DIM_BUFFER){
			pthread_mutex_lock(&sig_filtered.lock);
				for(j=0;j<DIM_BUFFER;j++){
					sig_filtered.vect_value_1[j]=buffer_temp[j];
				}
			pthread_mutex_unlock(&sig_filtered.lock);
			j=0;
			index=0;
		}
		sprintf (message, "%f,%f,%f,%f", t, cur, cur_noise,retval1);
		if (mq_send (butterfilter_qd, message, strlen (message) + 1, 0) == -1) {
		    perror ("ButterFilter driver: Not able to send message to store");
		}
		
	}

	if (mq_close(butterfilter_qd) == -1) {
    	perror("store: mq_close (butterfilter)");
    	exit(1);
	}
	return NULL;

}


void * peak_counter(void * parameter){

	struct periodic_thread *th = (struct periodic_thread *) parameter;
	start_periodic_timer(th,T_SAMPLE_PEAK);

	float signal_f[DIM_BUFFER];
	int i=0, j=0, a=0;
	float curr;
	int count=0, flag=0;
	char message[MAX_MSG_SIZE];
	char choice[MAX_MSG_SIZE];

	// Coda 
	struct mq_attr attr;
	attr.mq_flags = 0;				
	attr.mq_maxmsg = MAX_MESSAGES_P;	
	attr.mq_msgsize = MAX_MSG_SIZE; 
	attr.mq_curmsgs = 0;

	// Apriamo la coda filter in scrittura 
	mqd_t peak_counter_qd;
	if ((peak_counter_qd = mq_open (PEAK_COUNTER_QUEUE_NAME, O_WRONLY | O_CREAT | O_NONBLOCK , QUEUE_PERMISSIONS, &attr)) == -1) {
		perror (" Peak counter queue: mq_open (filter)");
		exit (1);
	}

	mqd_t conf_peak_qd;	

	while(1){

		wait_next_activation(th);
		if ((conf_peak_qd = mq_open (CONF_PEAK_QUEUE_NAME, O_RDONLY | O_NONBLOCK  , QUEUE_PERMISSIONS, &attr)) == -1) {
			perror (" Conf peak queue: mq_open (conf peak)");
		}
		if (mq_receive (conf_peak_qd, choice,MAX_MSG_SIZE , NULL) == -1) {
            perror ("Conf peak driver: Not able to recieve message to conf peak");  
    	}
		else{
			a = atoi(choice);
		}
		switch (a) {
			case 0 :	
			default :
					pthread_mutex_lock(&sig_filtered.lock);
						for(j=0;j<DIM_BUFFER;j++){
							signal_f[j]=sig_filtered.vect_value[j];
						}
					pthread_mutex_unlock(&sig_filtered.lock);
				break;
			case 1 :
					pthread_mutex_lock(&sig_filtered.lock);
						for(j=0;j<DIM_BUFFER;j++){
							signal_f[j]=sig_filtered.vect_value_1[j];
						}
					pthread_mutex_unlock(&sig_filtered.lock);
				break;
		}

		if(j==DIM_BUFFER){
			j=0;	
			for(i=0; i<DIM_BUFFER; i++){
				if(i==0){
					curr=signal_f[0];
				}
				else{
					if(signal_f[i]>curr){
						if(flag<0){
							count++;
						}
						flag=1;				
					}
					else {
						if(signal_f[i]<curr){
							if(flag>0){
								count++;
							}
							flag=-1;
						}				
					}
					curr=signal_f[i];
				}
			}
			i=0;	
			sprintf (message,"%d",count);
			if (mq_send (peak_counter_qd, message, strlen (message) + 1, 0) == -1) {
				perror ("Filter driver: Not able to send message to store");
			}
			count=0;
		}	
		if (mq_close(conf_peak_qd) == -1) {
        perror("conf: mq_close (conf_peak)");
    	}
	}	
	
	if (mq_close(peak_counter_qd) == -1) {
    	perror("store: mq_close (filter)");
    	exit(1);
	}
	return 0;
}


int main(int argc, char ** argv)
{
	pthread_t generator_thread;
	pthread_t filter_thread;
	pthread_t butterfilter_thread;
	pthread_t peak_counter_thread;
	
	pthread_mutexattr_t mymutexattr;
	pthread_mutexattr_init(&mymutexattr);
	pthread_mutexattr_setprotocol(&mymutexattr, PTHREAD_PRIO_INHERIT);

	// Inizializzazione dei mutex
	pthread_mutex_init(&sig_noise.lock,&mymutexattr);  
    pthread_mutex_init(&sig_filtered.lock,&mymutexattr);

	// Pulizia degli attributi del mutex dopo l'uso
    pthread_mutexattr_destroy(&mymutexattr);

	// Definizione degli attributi per i thread
	struct sched_param myparam;
	pthread_attr_t myattr;
	pthread_attr_init(&myattr);
    pthread_attr_setschedpolicy(&myattr, SCHED_FIFO);
    pthread_attr_setinheritsched(&myattr, PTHREAD_EXPLICIT_SCHED);

	// GENERATOR THREAD
	struct periodic_thread generator_th;
	generator_th.period = T_SAMPLE;
	generator_th.priority = 50;

	myparam.sched_priority = generator_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam); 
    pthread_create(&generator_thread, &myattr, generator, (void*)&generator_th);

	// FILTER THREAD
	struct periodic_thread filter_th;
	filter_th.period = T_SAMPLE;
	filter_th.priority = 50;

	myparam.sched_priority = filter_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam); 
    pthread_create(&filter_thread, &myattr, filter, (void*)&filter_th);

	// BUTTERFILTER THREAD
	struct periodic_thread butterfilter_th;
	butterfilter_th.period = T_SAMPLE;
	butterfilter_th.priority = 50;

	myparam.sched_priority = butterfilter_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam); 
    pthread_create(&butterfilter_thread, &myattr, butterfilter, (void*)&butterfilter_th);

	// PEAK COUNTER THREAD
	struct periodic_thread peak_counter_th;
	peak_counter_th.period = T_SAMPLE_PEAK;
	peak_counter_th.priority = 45;

	myparam.sched_priority = peak_counter_th.priority;
    pthread_attr_setschedparam(&myattr, &myparam); 
    pthread_create(&peak_counter_thread, &myattr, peak_counter, (void*)&peak_counter_th);

	pthread_attr_destroy(&myattr);

	while (1) {
        if (getchar() == 'q') break;
    }


	//DEALLOCAZIONE THREAD E PULIZIA RISORSE
	pthread_cancel(generator_thread);
	pthread_join(generator_thread,0);

	pthread_cancel(filter_thread);
	pthread_join(filter_thread,0);

	pthread_cancel(butterfilter_thread);
	pthread_join(butterfilter_thread,0);

	pthread_cancel(peak_counter_thread);
	pthread_join(peak_counter_thread,0);
	
	pthread_mutex_destroy(&sig_noise.lock);
	pthread_mutex_destroy(&sig_filtered.lock);

	if(mq_unlink(FILTER_QUEUE_NAME)==-1) {
		perror("Main: mq_unlink filter queue");
		exit(1);
	}

	if(mq_unlink(BUTTERFILTER_QUEUE_NAME)==-1) {
		perror("Main: mq_unlink butterfilter queue");
		exit(1);
	}

	if(mq_unlink(PEAK_COUNTER_QUEUE_NAME)==-1) {
		perror("Main: mq_unlink counter peak queue");
		exit(1);
	}

	if(mq_unlink(CONF_PEAK_QUEUE_NAME)==-1) {
		perror("Main: mq_unlink conf peak queue");
		exit(1);
	}

	printf("The filter is stopped. Bye!\n");
	return 0;
}

