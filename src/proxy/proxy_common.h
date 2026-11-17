/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_COMMON_H
#define PROXY_COMMON_H

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/hash_table.h"
#include "util/list.h"
#include "util/macros.h"
#include "virgl_util.h"
#include "virglrenderer.h"

#include "proxy_renderer.h"
#include "proxy_socket.h"

struct proxy_client;
struct proxy_context;
struct proxy_server;
struct proxy_socket;

struct proxy_renderer {
   const struct proxy_renderer_cbs *cbs;
   uint32_t flags;

   struct proxy_server *server;
   struct proxy_client *client;
   /* Populated only by a validated server-jail receipt, not by
    * RECORDS_REAPED (which covers current client records only). */
   uint64_t server_jail_empty_generation;
   bool server_reap_attempted;
   uint64_t server_reap_generation;
   int server_reap_result;
};

extern struct proxy_renderer proxy_renderer;

void
proxy_log(const char *fmt, ...);

#endif /* PROXY_COMMON_H */
