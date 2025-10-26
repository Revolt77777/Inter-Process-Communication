#ifndef __CACHE_STUDENT_H__844
#define __CACHE_STUDENT_H__844

#include "steque.h"
#include <sys/stat.h>
#include <mqueue.h>

static const char REQUEST_QUEUE_NAME[] = "/cache_request_q";
static const char REPLY_QUEUE_NAME[] = "/cache_reply_q";

typedef struct {
    char path[256];
} request_t;

#endif // __CACHE_STUDENT_H__844
