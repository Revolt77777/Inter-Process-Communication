#ifndef __CACHE_STUDENT_H__844
#define __CACHE_STUDENT_H__844

#include "steque.h"
#include "shm_channel.h"
#include <sys/stat.h>
#include <mqueue.h>

static const char REQUEST_QUEUE_NAME[] = "/cache_request_q";

typedef struct {
    char path[256];
    char segname[MAX_SEG_NAME_LEN];
    size_t segsize;
} request_t;

#endif // __CACHE_STUDENT_H__844
