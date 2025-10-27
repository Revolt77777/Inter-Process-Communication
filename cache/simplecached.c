#include <stdio.h>
#include <unistd.h>
#include <printf.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <sys/signal.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include "cache-student.h"
#include "shm_channel.h"
#include "simplecache.h"
#include "gfserver.h"

// CACHE_FAILURE
#if !defined(CACHE_FAILURE)
    #define CACHE_FAILURE (-1)
#endif 

#define MAX_CACHE_REQUEST_LEN 6112
#define MAX_SIMPLE_CACHE_QUEUE_SIZE 10

unsigned long int cache_delay;
mqd_t req_mq;
steque_t queue;
pthread_mutex_t mutex;
pthread_cond_t cond;
pthread_t *tids = NULL;
int nthreads;

static void _sig_handler(int signo){
	if (signo == SIGTERM || signo == SIGINT){
		// Clean up synchronization primitives
		pthread_mutex_destroy(&mutex);
		pthread_cond_destroy(&cond);
		steque_destroy(&queue);

		// Clean up cache
		simplecache_destroy();

		// Clean up message queue
		mq_close(req_mq);
		mq_unlink(REQUEST_QUEUE_NAME);

		exit(signo);
	}
}

#define USAGE                                                                 \
"usage:\n"                                                                    \
"  simplecached [options]\n"                                                  \
"options:\n"                                                                  \
"  -c [cachedir]       Path to static files (Default: ./)\n"                  \
"  -t [thread_count]   Thread count for work queue (Default is 8, Range is 1-100)\n"      \
"  -d [delay]          Delay in simplecache_get (Default is 0, Range is 0-2500000 (microseconds)\n "	\
"  -h                  Show this help message\n"

//OPTIONS
static struct option gLongOptions[] = {
  {"cachedir",           required_argument,      NULL,           'c'},
  {"nthreads",           required_argument,      NULL,           't'},
  {"help",               no_argument,            NULL,           'h'},
  {"hidden",			 no_argument,			 NULL,			 'i'}, /* server side */
  {"delay", 			 required_argument,		 NULL, 			 'd'}, // delay.
  {NULL,                 0,                      NULL,             0}
};

void Usage() {
  fprintf(stdout, "%s", USAGE);
}

void * worker_fn() {
	while(1) {
		pthread_mutex_lock(&mutex);
		while (steque_isempty(&queue)) {
			pthread_cond_wait(&cond, &mutex);
		}
		request_t *req = steque_pop(&queue);
		pthread_mutex_unlock(&mutex);
		fprintf(stdout, "Cache Server Received Request for: %s\n",req->path);
		fprintf(stdout, "shm IPC segment name: %s\n", req->segname);
		fprintf(stdout, "shm IPC segment size: %lu\n", req->segsize);

		int fd = simplecache_get(req->path);
		fprintf(stdout, "File Descriptor for the target file: %d\n",fd);


		free(req);
	}
	return NULL;
}

int main(int argc, char **argv) {
	nthreads = 6;
	char *cachedir = "locals.txt";
	char option_char;

	/* disable buffering to stdout */
	setbuf(stdout, NULL);

	while ((option_char = getopt_long(argc, argv, "d:ic:hlt:x", gLongOptions, NULL)) != -1) {
		switch (option_char) {
			default:
				Usage();
				exit(1);
			case 't': // thread-count
				nthreads = atoi(optarg);
				break;				
			case 'h': // help
				Usage();
				exit(0);
				break;    
            case 'c': //cache directory
				cachedir = optarg;
				break;
            case 'd':
				cache_delay = (unsigned long int) atoi(optarg);
				break;
			case 'i': // server side usage
			case 'o': // do not modify
			case 'a': // experimental
				break;
		}
	}

	if (cache_delay > 2500000) {
		fprintf(stderr, "Cache delay must be less than 2500000 (us)\n");
		exit(__LINE__);
	}

	if ((nthreads>100) || (nthreads < 1)) {
		fprintf(stderr, "Invalid number of threads must be in between 1-100\n");
		exit(__LINE__);
	}
	if (SIG_ERR == signal(SIGINT, _sig_handler)){
		fprintf(stderr,"Unable to catch SIGINT...exiting.\n");
		exit(CACHE_FAILURE);
	}
	if (SIG_ERR == signal(SIGTERM, _sig_handler)){
		fprintf(stderr,"Unable to catch SIGTERM...exiting.\n");
		exit(CACHE_FAILURE);
	}
	/*Initialize cache*/
	simplecache_init(cachedir);

	// Set up multi-threading management
	steque_init(&queue);
	pthread_mutex_init(&mutex, NULL);
	pthread_cond_init(&cond, NULL);

	tids = malloc(sizeof(pthread_t) * nthreads);

	for (int i = 0; i < nthreads; i++) {
		pthread_create(&tids[i], NULL, worker_fn, NULL);
	}

	// Set up the POSIX message queue to handle incoming requests
	struct mq_attr attr;
	attr.mq_maxmsg = MAX_SIMPLE_CACHE_QUEUE_SIZE;
	attr.mq_msgsize = MAX_CACHE_REQUEST_LEN;

	req_mq = mq_open(REQUEST_QUEUE_NAME, O_CREAT | O_RDONLY, 0666, &attr);
	if (req_mq == -1) {
		fprintf(stderr, "Unable to open request queue: %s (errno:%d)\n", strerror(errno), errno);
		_sig_handler(SIGTERM);
	}

	fprintf(stdout, "Cache Server Initialized.\nWaiting for message.\n");
	while(1) {
		char buffer[MAX_CACHE_REQUEST_LEN];
		ssize_t recv = mq_receive(req_mq, buffer, MAX_CACHE_REQUEST_LEN, NULL);

		if (recv == -1) {
			fprintf(stderr, "Unable to receive message: %s (errno:%d)\n", strerror(errno), errno);
			continue;
		}

		if (recv != sizeof(request_t)) {
			fprintf(stderr, "Received incorrect request message, skip.\n");
			continue;
		}

		// Create copy of received request message to put on queue
		request_t *req = malloc(sizeof(request_t));
		memcpy(req, buffer, sizeof(request_t));

		pthread_mutex_lock(&mutex);
		steque_push(&queue, req);
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&mutex);
	}

	// Line never reached
	fprintf(stderr, "req_mq closed, exiting...\n");
	simplecache_destroy();
	exit(CACHE_FAILURE);
	return -1;
}
