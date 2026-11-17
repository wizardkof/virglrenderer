/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_SERVER_H
#define PROXY_SERVER_H

#include "proxy_common.h"

#include <sys/types.h>

#ifdef ENABLE_SAME_PROCESS_RENDER_SERVER
#include "c11/threads.h"
#endif

struct proxy_server {
   pid_t pid;
   int client_fd;
   bool reaped;
#ifdef ENABLE_SAME_PROCESS_RENDER_SERVER
   thrd_t thread;
#endif
};

struct proxy_server *
proxy_server_create(void);

/* Serialized owner only. Reaps our child without freeing server/channel.
 * External servers and thread backend have no owned-process witness. */
int
proxy_server_reap_checked(struct proxy_server *srv, int timeout_ms);

void
proxy_server_destroy(struct proxy_server *srv);

int
proxy_server_connect(struct proxy_server *srv);

#endif /* PROXY_SERVER_H */
