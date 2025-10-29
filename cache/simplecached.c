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
int shutdown_flag = 0;

static void _sig_handler(int signo){
	if (signo == SIGTERM || signo == SIGINT){
		// Signal all threads to shut down
		pthread_mutex_lock(&mutex);
		shutdown_flag = 1;
		pthread_cond_broadcast(&cond);  // Wake all waiting threads
		pthread_mutex_unlock(&mutex);

		// Join all threads
		if (tids != NULL) {
			for (int i = 0; i < nthreads; i++) {
				pthread_join(tids[i], NULL);
			}
			free(tids);
		}

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

		// Check shutdown flag
		if (shutdown_flag) {
			pthread_mutex_unlock(&mutex);
			return NULL;
		}

		while (steque_isempty(&queue)) {
			pthread_cond_wait(&cond, &mutex);
			// Check again after waking up
			if (shutdown_flag) {
				pthread_mutex_unlock(&mutex);
				return NULL;
			}
		}
		request_t *req = steque_pop(&queue);
		pthread_mutex_unlock(&mutex);
		// fprintf(stdout, "Cache Server Received Request for: %s\n",req->path);
		// fprintf(stdout, "shm IPC segment name: %s\n", req->segname);
		// fprintf(stdout, "shm IPC segment size: %lu\n", req->segsize);

		int shm_fd = shm_open(req->segname, O_RDWR, 0666);
		if (shm_fd == -1) {
			fprintf(stderr, "Unable to open shm IPC segment.\n");
			free(req);
			continue;
		}
		void *ptr = mmap(0, req->segsize, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		close(shm_fd);
		if (ptr == MAP_FAILED) {
			fprintf(stderr, "Unable to map shm IPC segment.\n");
			free(req);
			continue;
		}

		// Get section pointers
		sem_t *sem_write = SHM_SEM_WRITE(ptr);
		sem_t *sem_read = SHM_SEM_READ(ptr);
		shm_header_t *header = SHM_HEADER(ptr);
		void *data = SHM_DATA(ptr);
		size_t data_size = SHM_DATA_SIZE(req->segsize);

		// Try to open the target file fd
		int file_fd = simplecache_get(req->path);

		if (file_fd < 0) {
			// File not found
			header->status = SEG_STATUS_NOT_FOUND;
			header->file_len = 0;
			header->chunk_len = 0;
			sem_post(sem_write);
			continue;
		}

		// Get file size
		struct stat st;
		fstat(file_fd, &st);

		// Set initial header
		header->file_len = st.st_size;

		// Wait for initial permission (sem_read starts at 1)
		sem_wait(sem_read);

		// Send the file chunk by chunk
		size_t byteTransfered = 0;
		while (byteTransfered < st.st_size) {
			ssize_t read_len = pread(file_fd, data, data_size, byteTransfered);
			if (read_len <= 0) {
				fprintf(stderr, "Read file error.\n");
				break;
			}

			// fprintf(stdout, "%lu bytes transfered, %lu/%lu.\n", read_len, byteTransfered, st.st_size);
			byteTransfered += read_len;

			// Update header
			header->chunk_len = read_len;
			if (byteTransfered >= st.st_size) {
				header->status = SEG_STATUS_COMPLETED;
			} else {
				header->status = SEG_STATUS_IN_PROGRESS;
			}

			// Signal data ready
			sem_post(sem_write);

			// Wait for proxy to finish reading (but not after last chunk)
			if (byteTransfered < st.st_size) {
				sem_wait(sem_read);
			}
		}
		// fprintf(stdout, "File transfer completed with %zu bytes.\n", byteTransfered);
		munmap(ptr, req->segsize);
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

	// fprintf(stdout, "Cache Server Initialized.\nWaiting for message.\n");
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
