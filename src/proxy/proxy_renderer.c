/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_common.h"

#include "virtgpu_drm.h"

#include "proxy_client.h"
#include "proxy_context.h"
#include <time.h>
#include <unistd.h>
#include "proxy_renderer.h"
#include "proxy_server.h"
#include "vkr_renderer.h"

int
proxy_renderer_init(const struct proxy_renderer_cbs *cbs, uint32_t flags)
{
   assert(flags & VIRGL_RENDERER_NO_VIRGL);

   proxy_renderer.cbs = cbs;
   proxy_renderer.flags = flags;

   proxy_renderer.server = proxy_server_create();
   if (!proxy_renderer.server)
      goto fail;

   proxy_renderer.client =
      proxy_client_create(proxy_renderer.server, proxy_renderer.flags);
   if (!proxy_renderer.client)
      goto fail;

   return 0;

fail:
   proxy_renderer_fini();
   return -1;
}

int
proxy_renderer_cleanup_check(void)
{
   if (!proxy_renderer.server_reap_attempted)
      return 0;
   if (proxy_renderer.server_reap_result)
      return proxy_renderer.server_reap_result;
   if (!proxy_renderer.server || !proxy_renderer.server->reaped)
      return -EPROTO;
   return 0;
}

void
proxy_renderer_fini(void)
{
   if (proxy_renderer_cleanup_check()) {
      proxy_log("cleanup refused: checked parent termination incomplete");
      abort();
   }
   if (proxy_renderer.client)
      proxy_client_destroy(proxy_renderer.client);

   if (proxy_renderer.server)
      proxy_server_destroy(proxy_renderer.server);

   memset(&proxy_renderer, 0, sizeof(struct proxy_renderer));
}

void
proxy_renderer_reset(void)
{
   proxy_client_reset(proxy_renderer.client);
}

bool
proxy_renderer_reap_reply_valid(void)
{
   return proxy_renderer.client && proxy_renderer.client->reset_checked_reply_valid;
}

struct proxy_reset_reap_count {
   uint64_t generation;
   uint32_t count;
   int error;
};

static bool
proxy_renderer_count_reap_context(struct virgl_context *ctx, void *data)
{
   struct proxy_reset_reap_count *args = data;
   args->error = proxy_context_reset_reap_ready(ctx, args->generation);
   if (args->error)
      return false;
   if (args->count == UINT32_MAX) {
      args->error = -EOVERFLOW;
      return false;
   }
   args->count++;
   return true;
}

/* Context topology is retained by the exclusive renderer owner throughout
 * validation, counting and the client RPC. This is process-record evidence.
 */
int
proxy_renderer_reap_checked(uint64_t generation, int timeout_ms,
                             struct render_client_op_reset_checked_reply *out)
{
   struct timespec start, now;
   if (!generation || timeout_ms <= 0 || !out || !proxy_renderer.client)
      return -EINVAL;
   if (clock_gettime(CLOCK_MONOTONIC, &start))
      return -errno;
   struct proxy_reset_reap_count count = { .generation = generation };
   const struct virgl_context_foreach_args args = {
      .callback = proxy_renderer_count_reap_context, .data = &count,
   };
   virgl_context_foreach(&args);
   if (count.error)
      return count.error;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      return -errno;
   int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 +
                     now.tv_nsec - start.tv_nsec;
   int64_t remaining = ((int64_t)timeout_ms * 1000000 - elapsed) / 1000000;
   if (elapsed < 0 || remaining <= 0 || remaining > timeout_ms)
      return -ETIMEDOUT;
   int ret = proxy_client_reset_checked(proxy_renderer.client, generation,
                                         count.count, remaining, out);
   if (ret)
      return ret;
   bool clock_failed = clock_gettime(CLOCK_MONOTONIC, &now) != 0;
   elapsed = clock_failed ? -1 :
      (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec;
   if (elapsed < 0 || elapsed >= (int64_t)timeout_ms * 1000000) {
      /* Include validation and any scheduling gap before RPC entry. Do not
       * allow its cached success to bypass an expired enclosing budget.
       */
      ret = elapsed < 0 ? -EIO : -ETIMEDOUT;
      proxy_renderer.client->reset_checked_reply_valid = false;
      proxy_renderer.client->reset_checked_result = ret;
      proxy_renderer.client->reset_checked_reply.status = ret;
      proxy_renderer.client->reset_checked_reply.phase = 0;
      if (proxy_renderer.client->socket.fd >= 0) {
         close(proxy_renderer.client->socket.fd);
         proxy_renderer.client->socket.fd = -1;
      }
      *out = proxy_renderer.client->reset_checked_reply;
      return ret;
   }
   return 0;
}

int
proxy_renderer_drain_jail_checked(uint64_t generation, int timeout_ms)
{
   struct proxy_client *client = proxy_renderer.client;
   struct timespec start, now;
   if (!client || !generation || timeout_ms <= 0)
      return -EINVAL;
   if (proxy_renderer.server_jail_empty_generation)
      return proxy_renderer.server_jail_empty_generation == generation ? 0 : -EBUSY;
   if (clock_gettime(CLOCK_MONOTONIC, &start))
      return -errno;
   struct render_client_op_jail_checked_reply reply = { 0 };
   int ret = proxy_client_jail_checked(client, generation, timeout_ms, &reply);
   if (ret)
      return ret;
   if (!client->jail_checked_reply_valid || reply.generation != generation ||
       reply.protocol_version != RENDER_JAIL_CHECKED_VERSION ||
       reply.phase != RENDER_JAIL_EMPTY || reply.status || reply.remaining_count)
      ret = -EPROTO;
   if (!ret) {
      if (clock_gettime(CLOCK_MONOTONIC, &now)) {
         ret = -errno;
      } else {
         int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 +
                           now.tv_nsec - start.tv_nsec;
         if (elapsed < 0 || elapsed >= (int64_t)timeout_ms * 1000000)
            ret = elapsed < 0 ? -EIO : -ETIMEDOUT;
      }
   }
   if (ret) {
      client->jail_checked_reply_valid = false;
      client->jail_checked_result = ret;
      client->jail_checked_reply.status = ret;
      client->jail_checked_reply.phase = 0;
      if (client->socket.fd >= 0) {
         close(client->socket.fd);
         client->socket.fd = -1;
      }
      return ret;
   }
   /* Current server owns one client, already frozen by RESET_CHECKED.
    * No producer can add a new worker after this exact jail-empty receipt. */
   proxy_renderer.server_jail_empty_generation = generation;
   return 0;
}

/* Exclusive owner retains the frozen topology and remote receipt. A timeout
 * is terminal for this transaction even if the child was actually reaped. */
int
proxy_renderer_reap_server_checked(uint64_t generation, int timeout_ms)
{
   struct proxy_client *client = proxy_renderer.client;
   struct timespec start, now;
   if (!generation || timeout_ms <= 0 || !client || !proxy_renderer.server)
      return -EINVAL;
   if (proxy_renderer.server_reap_attempted)
      return proxy_renderer.server_reap_generation == generation ?
             proxy_renderer.server_reap_result : -EBUSY;
   if (!client->reset_checked_attempted || !client->reset_checked_reply_valid ||
       client->reset_checked_result)
      return -EBUSY;
   if (client->reset_checked_generation != generation)
      return -ESTALE;
   const struct render_client_op_reset_checked_reply *reply =
      &client->reset_checked_reply;
   if (reply->generation != generation ||
       reply->protocol_version != RENDER_RESET_CHECKED_VERSION ||
       reply->phase != RENDER_RESET_RECORDS_REAPED || reply->status ||
       reply->remaining_count || reply->failed_ctx_id ||
       reply->reaped_count != client->reset_checked_expected_records)
      return -EPROTO;
   /* A client can have no records while previously destroyed workers still
    * remain in the jail. Do not orphan them by killing their parent. The
    * jail RPC must establish this additional witness first. */
   if (proxy_renderer.server_jail_empty_generation != generation)
      return -EOPNOTSUPP;
   if (clock_gettime(CLOCK_MONOTONIC, &start))
      return -errno;
   proxy_renderer.server_reap_attempted = true;
   proxy_renderer.server_reap_generation = generation;
   proxy_renderer.server_reap_result = -EINPROGRESS;
   int ret = proxy_server_reap_checked(proxy_renderer.server, timeout_ms);
   if (!ret) {
      if (clock_gettime(CLOCK_MONOTONIC, &now)) {
         ret = -errno;
      } else {
         int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 +
                           now.tv_nsec - start.tv_nsec;
         if (elapsed < 0)
            ret = -EIO;
         else if (elapsed >= (int64_t)timeout_ms * 1000000)
            ret = -ETIMEDOUT;
      }
   }
   proxy_renderer.server_reap_result = ret;
   return ret;
}

size_t
proxy_get_capset(uint32_t set, void *caps)
{
   switch (set) {
   case VIRTGPU_DRM_CAPSET_VENUS:
      return vkr_get_capset(caps, proxy_renderer.flags);
   default:
      break;
   }

   return 0;
}
