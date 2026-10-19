#pragma once
#include "store.h"
void notify_start(void);
void notify_push(const dbevent_t *e);   /* non-blocking; dropped if the queue is full (hub backfills via /events) */
