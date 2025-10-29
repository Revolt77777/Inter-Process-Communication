#include "gfserver.h"
#include "cache-student.h"
#include "shm_channel.h"

#define BUFSIZE (840)

void return_segment(seg_queue_args_t *arg, shm_segment_t *seg) {
	shm_reset_segment(seg);
	pthread_mutex_lock(arg->mutex);
	steque_push(arg->queue, seg);
	pthread_cond_signal(arg->cond);
	pthread_mutex_unlock(arg->mutex);
}

ssize_t handle_with_cache(gfcontext_t *ctx, const char *path, seg_queue_args_t *arg) {
	// fprintf(stdout, "Retrieving file %s from cache server\n", path);

	// Retrieve shm_segment from pool in proxy server
	pthread_mutex_lock(arg->mutex);
	while (steque_isempty(arg->queue)) {
		pthread_cond_wait(arg->cond, arg->mutex);
	}
	shm_segment_t *seg = steque_pop(arg->queue);
	pthread_mutex_unlock(arg->mutex);
	// fprintf(stdout, "Segment retrieved: %s\n",seg->name);

	// Generate request message
	request_t req;
	strcpy(req.path, path);
	strcpy(req.segname, seg->name);
	req.segsize = seg->segsize;

	// Connect with cache server request message queue
	mqd_t req_mq;
	int retries = 0;
	const int MAX_RETRIES = 10;

	// Retry logic in case cache server hasn't set up yet
	while ((req_mq = mq_open(REQUEST_QUEUE_NAME, O_WRONLY)) == -1) {
		if (retries >= MAX_RETRIES) {
			fprintf(stderr, "Unable to open request queue after %d retries: %s (errno:%d)\n",
					MAX_RETRIES, strerror(errno), errno);
			return -1;
		}

		fprintf(stderr, "Unable to open request queue (retry %d/%d): %s (errno:%d)\n",
				retries + 1, MAX_RETRIES, strerror(errno), errno);

		sleep(1);
		retries++;
	}

	// Sending request to request mq of cache server
	int send = mq_send(req_mq, (char*)&req, sizeof(request_t), 0);
	mq_close(req_mq);
	if (send == -1) {
		fprintf(stderr, "Unable to send request: %s (errno:%d)\n", strerror(errno), errno);
		return -1;
	}

	// Calculate the section pointer
	sem_t *read_sem = SHM_SEM_READ(seg->proxy_ptr);
	sem_t *write_sem = SHM_SEM_WRITE(seg->proxy_ptr);
	shm_header_t *header = SHM_HEADER(seg->proxy_ptr);
	void *data = SHM_DATA(seg->proxy_ptr);

	// Wait for first response
	sem_wait(write_sem);

	// Parse the first header
	// fprintf(stdout, "Received header %d, %lu, %lu.\n", header->status, header->file_len, header->chunk_len);
	if (header->status == SEG_STATUS_NOT_FOUND) {
		return_segment(arg, seg);
		return gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
	}

	gfs_sendheader(ctx, GF_OK, header->file_len);

	// Sending the file chunk by chunk
	size_t byteTransfered = 0;
	while (byteTransfered < header->file_len) {
		size_t write_len = gfs_send(ctx, data, header->chunk_len);

		if (write_len != header->chunk_len){
			fprintf(stderr, "server write error with %lu chunk_len and %lu write_len and %lu bytestransfered.\n", header->chunk_len, write_len, byteTransfered);
		}
		byteTransfered += write_len;
		// fprintf(stdout, "%lu bytes transfered with %lu chunk_len, %lu/%lu.\n", write_len, header->chunk_len, byteTransfered, header->file_len);

		// Signal done reading and wait for next chunk (but not after last)
		if (byteTransfered < header->file_len) {
			sem_post(read_sem);
			sem_wait(write_sem);
		}
	}

	// Verify completion
	if (header->status != SEG_STATUS_COMPLETED) {
		fprintf(stderr, "Warning: transferred %zu but expected %zu, status is incompleted.\n", byteTransfered, header->file_len);
	}

	// Reset and return the segment to the pool
	return_segment(arg, seg);
	// fprintf(stdout, "File transfer completed with %zu bytes.\n", byteTransfered);
	return byteTransfered;
}

/*ssize_t handle_with_cache(gfcontext_t *ctx, const char *path, void* arg){
	size_t file_len;
    size_t bytes_transferred;
	char *data_dir = arg;
	ssize_t read_len;
    ssize_t write_len;
	char buffer[BUFSIZE];
	int fildes;
	struct stat statbuf;

	strncpy(buffer,data_dir, BUFSIZE);
	strncat(buffer,path, BUFSIZE);

	if( 0 > (fildes = open(buffer, O_RDONLY))){
		if (errno == ENOENT)
			//If the file just wasn't found, then send FILE_NOT_FOUND code
			return gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
		else
			//Otherwise, it must have been a server error. gfserver library will handle
			return SERVER_FAILURE;
	}

	//Calculating the file size
	if (fstat(fildes, &statbuf) < 0) {
		return SERVER_FAILURE;
	}
	file_len = (size_t) statbuf.st_size;
	///

	gfs_sendheader(ctx, GF_OK, file_len);

	//Sending the file contents chunk by chunk

	bytes_transferred = 0;
	while(bytes_transferred < file_len){
		read_len = read(fildes, buffer, BUFSIZE);
		if (read_len <= 0){
			fprintf(stderr, "handle_with_file read error, %zd, %zu, %zu", read_len, bytes_transferred, file_len );
			return SERVER_FAILURE;
		}
		write_len = gfs_send(ctx, buffer, read_len);
		if (write_len != read_len){
			fprintf(stderr, "handle_with_file write error");
			return SERVER_FAILURE;
		}
		bytes_transferred += write_len;
	}

	return bytes_transferred;


}
*/