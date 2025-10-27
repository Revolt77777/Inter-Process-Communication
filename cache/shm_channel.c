#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include "shm_channel.h"

shm_segment_t* shm_init(int nsegments, size_t segsize) {
    shm_segment_t *seg_ids = malloc(sizeof(shm_segment_t) * nsegments);
    if (seg_ids == NULL) {
        fprintf(stderr, "Unable to allocate memory for segments\n");
        return NULL;
    }

    for (int i = 0; i < nsegments; i++) {
        // Initialize segment name
        snprintf(seg_ids[i].name, MAX_SEG_NAME_LEN, "/shm_segment_%u", i);

        // Open shared memory
        int fd = shm_open(seg_ids[i].name, O_RDWR | O_CREAT, 0666);
        if (fd == -1) {
            fprintf(stderr, "Unable to open shared segment %u: %s (errno:%d)\n",
                    i, strerror(errno), errno);
            // Clean up previously created segments
            for (int j = 0; j < i; j++) {
                munmap(seg_ids[j].proxy_ptr, seg_ids[j].segsize);
                close(seg_ids[j].proxy_fd);
                shm_unlink(seg_ids[j].name);
            }
            free(seg_ids);
            return NULL;
        }
        seg_ids[i].proxy_fd = fd;

        // Resize the segment
        if (ftruncate(fd, segsize) == -1) {
            fprintf(stderr, "Unable to resize segment %u: %s (errno:%d)\n",
                    i, strerror(errno), errno);
            close(fd);
            // Clean up previously created segments
            for (int j = 0; j < i; j++) {
                munmap(seg_ids[j].proxy_ptr, seg_ids[j].segsize);
                close(seg_ids[j].proxy_fd);
                shm_unlink(seg_ids[j].name);
            }
            shm_unlink(seg_ids[i].name);
            free(seg_ids);
            return NULL;
        }

        // Map the segment
        seg_ids[i].proxy_ptr = mmap(0, segsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (seg_ids[i].proxy_ptr == MAP_FAILED) {
            fprintf(stderr, "Unable to map segment %u: %s (errno:%d)\n",
                    i, strerror(errno), errno);
            close(fd);
            // Clean up previously created segments
            for (int j = 0; j < i; j++) {
                munmap(seg_ids[j].proxy_ptr, seg_ids[j].segsize);
                close(seg_ids[j].proxy_fd);
                shm_unlink(seg_ids[j].name);
            }
            shm_unlink(seg_ids[i].name);
            free(seg_ids);
            return NULL;
        }

        seg_ids[i].segsize = segsize;

        // Initialize sem_data_ready (cache posts when data ready, proxy waits)
        sem_t *sem_data_ready = SHM_SEM_DATA_READY(seg_ids[i].proxy_ptr);
        if (sem_init(sem_data_ready, 1, 0) == -1) {
            fprintf(stderr, "Unable to initialize sem_data_ready %u: %s (errno:%d)\n",
                    i, strerror(errno), errno);
            munmap(seg_ids[i].proxy_ptr, segsize);
            close(fd);
            // Clean up previously created segments
            for (int j = 0; j < i; j++) {
                sem_t *prev_data = SHM_SEM_DATA_READY(seg_ids[j].proxy_ptr);
                sem_t *prev_done = SHM_SEM_READ_DONE(seg_ids[j].proxy_ptr);
                sem_destroy(prev_data);
                sem_destroy(prev_done);
                munmap(seg_ids[j].proxy_ptr, seg_ids[j].segsize);
                close(seg_ids[j].proxy_fd);
                shm_unlink(seg_ids[j].name);
            }
            shm_unlink(seg_ids[i].name);
            free(seg_ids);
            return NULL;
        }

        // Initialize sem_read_done (proxy posts when done reading, cache waits)
        sem_t *sem_read_done = SHM_SEM_READ_DONE(seg_ids[i].proxy_ptr);
        if (sem_init(sem_read_done, 1, 1) == -1) {
            fprintf(stderr, "Unable to initialize sem_read_done %u: %s (errno:%d)\n",
                    i, strerror(errno), errno);
            sem_destroy(sem_data_ready);
            munmap(seg_ids[i].proxy_ptr, segsize);
            close(fd);
            // Clean up previously created segments
            for (int j = 0; j < i; j++) {
                sem_t *prev_data = SHM_SEM_DATA_READY(seg_ids[j].proxy_ptr);
                sem_t *prev_done = SHM_SEM_READ_DONE(seg_ids[j].proxy_ptr);
                sem_destroy(prev_data);
                sem_destroy(prev_done);
                munmap(seg_ids[j].proxy_ptr, seg_ids[j].segsize);
                close(seg_ids[j].proxy_fd);
                shm_unlink(seg_ids[j].name);
            }
            shm_unlink(seg_ids[i].name);
            free(seg_ids);
            return NULL;
        }

        // Initialize header
        shm_header_t *header = SHM_HEADER(seg_ids[i].proxy_ptr);
        header->status = SEG_STATUS_EMPTY;
        header->file_len = 0;
        header->chunk_len = 0;
    }

    return seg_ids;
}

void shm_cleanup(shm_segment_t *seg_ids, int nsegments) {
    if (seg_ids == NULL) {
        return;
    }

    for (int i = 0; i < nsegments; i++) {
        // Destroy both semaphores before unmapping
        if (seg_ids[i].proxy_ptr != NULL && seg_ids[i].proxy_ptr != MAP_FAILED) {
            sem_t *sem_data_ready = SHM_SEM_DATA_READY(seg_ids[i].proxy_ptr);
            sem_t *sem_read_done = SHM_SEM_READ_DONE(seg_ids[i].proxy_ptr);
            sem_destroy(sem_data_ready);
            sem_destroy(sem_read_done);
            munmap(seg_ids[i].proxy_ptr, seg_ids[i].segsize);
        }

        // Close the file descriptor
        if (seg_ids[i].proxy_fd >= 0) {
            close(seg_ids[i].proxy_fd);
        }

        // Unlink the shared memory object
        if (seg_ids[i].name[0] != '\0') {
            shm_unlink(seg_ids[i].name);
        }
    }

    // Free the array
    free(seg_ids);
}

int shm_reset_segment(shm_segment_t *seg) {
    if (seg == NULL || seg->proxy_ptr == NULL) {
        return -1;
    }

    sem_t *sem_data_ready = SHM_SEM_DATA_READY(seg->proxy_ptr);
    sem_t *sem_read_done = SHM_SEM_READ_DONE(seg->proxy_ptr);

    // Destroy both semaphores
    sem_destroy(sem_data_ready);
    sem_destroy(sem_read_done);

    // Reinitialize to initial values
    sem_init(sem_data_ready, 1, 0);
    sem_init(sem_read_done, 1, 1);

    // Reset header
    shm_header_t *header = SHM_HEADER(seg->proxy_ptr);
    header->status = SEG_STATUS_EMPTY;
    header->file_len = 0;
    header->chunk_len = 0;

    return 0;
}