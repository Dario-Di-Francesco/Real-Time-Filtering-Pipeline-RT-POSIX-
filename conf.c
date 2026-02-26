
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <sys/time.h>
#include <stdint.h>
#include <mqueue.h>


#define CONF_PEAK_QUEUE_NAME     "/conf_peak_queue"
#define CONF_STORE_QUEUE_NAME     "/conf_store_queue"
#define QUEUE_PERMISSIONS 0660
#define MAX_MESSAGES 1
#define MAX_MSG_SIZE 32


int main(int argc, char ** argv)
{
    // Scelta da mandare a peak_counter e a store
	char choice[MAX_MSG_SIZE];

	/* Coda */
	struct mq_attr attr;

	attr.mq_flags = 0;				
	attr.mq_maxmsg = MAX_MESSAGES;	
	attr.mq_msgsize = MAX_MSG_SIZE; 
	attr.mq_curmsgs = 0;
	
	// Apriamo la coda conf peak in scrittura
	mqd_t conf_peak_qd, conf_store_qd;
	if ((conf_peak_qd = mq_open (CONF_PEAK_QUEUE_NAME, O_WRONLY | O_CREAT | O_NONBLOCK, QUEUE_PERMISSIONS,&attr)) == -1) {
		perror ("conf : mq_open (conf_peak)");
		exit (1);
	}
    // Apriamo la coda conf peak in scrittura
    if ((conf_store_qd = mq_open (CONF_STORE_QUEUE_NAME, O_WRONLY | O_CREAT | O_NONBLOCK, QUEUE_PERMISSIONS,&attr)) == -1) {
		perror ("conf : mq_open (conf_store)");
		exit (1);
	}

    printf("Inserisci la scelta del filtro tra 0, 1 ('q' to exit): ");
    scanf("%s",choice);
   
    while(strcmp("q", choice) != 0) {
        if((strcmp("0", choice) == 0) || (strcmp("1", choice) == 0)) {
            if (mq_send (conf_peak_qd, choice, strlen(choice)+1, 0) == -1) {
                perror ("Conf driver: Not able to send message to peak_counter");
     
            }
            if (mq_send (conf_store_qd, choice, strlen(choice)+1, 0) == -1) {
                perror ("Filter driver: Not able to send message to store");
                
            }
        }
        else {
            printf("Scelta non valida! Riprova.\n\n");
        }
        
        printf("Inserisci la scelta del filtro tra 0, 1 ) ('q' to exit): ");
        scanf("%s",choice);
        
    }


    //chiusura risorse
    if (mq_close(conf_peak_qd) == -1) {
        perror("conf: mq_close (conf_peak)");
        exit(1);
    }
    if (mq_close(conf_store_qd) == -1) {
        perror("conf: mq_close (conf_store)");
        exit(1);
    }
    
    if(mq_unlink(CONF_PEAK_QUEUE_NAME)==-1) {
		perror("Main: mq_unlink conf_peak queue");
		exit(1);
	}

	if(mq_unlink(CONF_STORE_QUEUE_NAME)==-1) {
		perror("Main: mq_unlink conf_store queue");
		exit(1);
	}

    printf("The conf is stopped. Bye!\n");
	return 0;
}
