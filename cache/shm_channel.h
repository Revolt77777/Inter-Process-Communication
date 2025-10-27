#ifndef SHM_CHANNEL_H
#define SHM_CHANNEL_H

#define MAX_SEG_NAME_LEN 32

#include <sys/mman.h>
#include <stddef.h>
#include <semaphore.h>
#include "steque.h"

// Segment status
typedef enum {
    SEG_STATUS_EMPTY,
    SEG_STATUS_IN_PROGRESS,
    SEG_STATUS_COMPLETED,
    SEG_STATUS_NOT_FOUND
} seg_status_t;

// Header stored at the beginning of each shared memory segment
typedef struct {
    seg_status_t status;
    size_t file_len;      // Total file length
    size_t chunk_len;     // Length of data in this segment
} shm_header_t;

typedef struct {
    char name[MAX_SEG_NAME_LEN];
    size_t segsize;
    int proxy_fd;
    void* proxy_ptr;  // Points to segment start: [sem_t][shm_header_t][data...]
} shm_segment_t;

typedef struct {
    steque_t *queue;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
} seg_queue_args_t;

// Helper macros to access components within shared memory
// Memory layout: [sem_data_ready][sem_read_done][shm_header_t][data...]
// Works with raw pointer from mmap (for both proxy and cache)
#define SHM_SEM_DATA_READY(ptr) ((sem_t*)(ptr))
#define SHM_SEM_READ_DONE(ptr)  ((sem_t*)((char*)(ptr) + sizeof(sem_t)))
#define SHM_HEADER(ptr)         ((shm_header_t*)((char*)(ptr) + 2*sizeof(sem_t)))
#define SHM_DATA(ptr)           ((void*)((char*)(ptr) + 2*sizeof(sem_t) + sizeof(shm_header_t)))
#define SHM_DATA_SIZE(segsize)  ((segsize) - 2*sizeof(sem_t) - sizeof(shm_header_t))

// Initializes shared memory segments
// Returns: Pointer to array of shm_segment_t, or NULL on failure
shm_segment_t* shm_init(int nsegments, size_t segsize);

// Cleans up shared memory segments
void shm_cleanup(shm_segment_t *seg_ids, int nsegments);

// Resets a segment for reuse (semaphore and header)
// Returns: 0 on success, -1 on failure
int shm_reset_segment(shm_segment_t *seg);

#endif
