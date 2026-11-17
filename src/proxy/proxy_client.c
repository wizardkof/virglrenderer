/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_client.h"

#include <unistd.h>
#include <time.h>

#include "server/render_protocol.h"

#include "proxy_server.h"

bool
proxy_client_destroy_context(struct proxy_client *client, uint32_t ctx_id)
{
   if (client->reset_checked_attempted)
      return false;
   const struct render_client_op_destroy_context_request req = {
      .header.op = RENDER_CLIENT_OP_DESTROY_CONTEXT,
      .ctx_id = ctx_id,
   };

   return proxy_socket_send_request(&client->socket, &req, sizeof(req));
}

bool
proxy_client_create_context(struct proxy_client *client,
                            uint32_t ctx_id,
                            size_t ctx_name_len,
                            const char *ctx_name,
                            int *out_ctx_fd)
{
   if (client->reset_checked_attempted)
      return false;
   struct render_client_op_create_context_request req = {
      .header.op = RENDER_CLIENT_OP_CREATE_CONTEXT,
      .ctx_id = ctx_id,
   };

   const size_t len = MIN2(ctx_name_len, sizeof(req.ctx_name) - 1);
   memcpy(req.ctx_name, ctx_name, len);

   if (!proxy_socket_send_request(&client->socket, &req, sizeof(req)))
      return false;

   struct render_client_op_create_context_reply reply;
   int fd_count;
   int ctx_fd;
   if (!proxy_socket_receive_reply_with_fds(&client->socket, &reply, sizeof(reply),
                                            &ctx_fd, 1, &fd_count))
      return false;

   if (reply.ok != fd_count) {
      if (fd_count)
         close(ctx_fd);
      return false;
   } else if (!reply.ok) {
      return false;
   }

   if (!proxy_socket_is_valid(ctx_fd)) {
      close(ctx_fd);
      return false;
   }

   *out_ctx_fd = ctx_fd;
   return true;
}

int
proxy_client_reset_checked(struct proxy_client *client, uint64_t generation,
                            uint32_t expected_records, int timeout_ms,
                            struct render_client_op_reset_checked_reply *out)
{
   struct timespec start, now;
   int ret = -EIO;
   if (!client || !out || !generation || timeout_ms <= 0)
      return -EINVAL;
   if (client->reset_checked_attempted) {
      if (client->reset_checked_generation != generation)
         return -EBUSY;
      if (client->reset_checked_expected_records != expected_records)
         return -ESTALE;
      *out = client->reset_checked_reply;
      return client->reset_checked_result;
   }
   client->reset_checked_attempted = true;
   client->reset_checked_reply_valid = false;
   client->reset_checked_generation = generation;
   client->reset_checked_expected_records = expected_records;
   client->reset_checked_result = -EINPROGRESS;
   client->reset_checked_reply = (struct render_client_op_reset_checked_reply){
      .generation = generation, .protocol_version = RENDER_RESET_CHECKED_VERSION,
      .status = -EINPROGRESS,
   };
   const struct render_client_op_reset_checked_request request = {
      .header.op = RENDER_CLIENT_OP_RESET_CHECKED,
      .protocol_version = RENDER_RESET_CHECKED_VERSION,
      .generation = generation, .timeout_ms = timeout_ms,
   };
   if (clock_gettime(CLOCK_MONOTONIC, &start) ||
       !proxy_socket_send_request_bounded(&client->socket, &request,
                                          sizeof(request), timeout_ms))
      goto fail;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      goto fail;
   int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 +
                     now.tv_nsec - start.tv_nsec;
   int64_t remaining = ((int64_t)timeout_ms * 1000000 - elapsed) / 1000000;
   if (elapsed < 0 || remaining <= 0 || remaining > timeout_ms) {
      ret = -ETIMEDOUT;
      goto fail;
   }
   struct render_client_op_reset_checked_reply reply;
   if (!proxy_socket_receive_reply_bounded(&client->socket, &reply, sizeof(reply),
                                            remaining))
      goto fail;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      goto fail;
   elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 +
             now.tv_nsec - start.tv_nsec;
   if (elapsed < 0 || elapsed >= (int64_t)timeout_ms * 1000000) {
      ret = -ETIMEDOUT;
      goto fail;
   }
   if (reply.generation != generation ||
       reply.protocol_version != RENDER_RESET_CHECKED_VERSION || reply.status > 0 ||
       (uint64_t)reply.reaped_count + reply.remaining_count != expected_records ||
       (!reply.status && (reply.phase != RENDER_RESET_RECORDS_REAPED ||
                          reply.remaining_count || reply.failed_ctx_id)) ||
       (reply.status && reply.phase)) {
      ret = -EPROTO;
      goto fail;
   }
   client->reset_checked_reply_valid = true;
   client->reset_checked_reply = reply;
   client->reset_checked_result = reply.status;
   *out = reply;
   return reply.status;
fail:
   if (client->socket.fd >= 0) {
      close(client->socket.fd);
      client->socket.fd = -1;
   }
   client->reset_checked_result = ret;
   client->reset_checked_reply.status = ret;
   *out = client->reset_checked_reply;
   return ret;
}

int
proxy_client_jail_checked(struct proxy_client *client, uint64_t generation,
                            int timeout_ms,
                            struct render_client_op_jail_checked_reply *out)
{
   struct timespec start, now;
   int ret = -EIO;
   if (!client || !out || !generation || timeout_ms <= 0)
      return -EINVAL;
   if (client->jail_checked_attempted) {
      if (client->jail_checked_generation != generation)
         return -EBUSY;
      *out = client->jail_checked_reply;
      return client->jail_checked_result;
   }
   if (!client->reset_checked_attempted || !client->reset_checked_reply_valid ||
       client->reset_checked_result)
      return -EBUSY;
   if (client->reset_checked_generation != generation)
      return -ESTALE;
   client->jail_checked_attempted = true;
   client->jail_checked_reply_valid = false;
   client->jail_checked_generation = generation;
   client->jail_checked_result = -EINPROGRESS;
   client->jail_checked_reply = (struct render_client_op_jail_checked_reply){
      .generation = generation, .protocol_version = RENDER_JAIL_CHECKED_VERSION,
      .status = -EINPROGRESS,
   };
   const struct render_client_op_jail_checked_request request = {
      .header.op = RENDER_CLIENT_OP_JAIL_CHECKED,
      .protocol_version = RENDER_JAIL_CHECKED_VERSION,
      .generation = generation, .timeout_ms = timeout_ms,
   };
   if (clock_gettime(CLOCK_MONOTONIC, &start) ||
       !proxy_socket_send_request_bounded(&client->socket, &request,
                                          sizeof(request), timeout_ms))
      goto fail;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      goto fail;
   int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 +
                     now.tv_nsec - start.tv_nsec;
   int64_t remaining = ((int64_t)timeout_ms * 1000000 - elapsed) / 1000000;
   if (elapsed < 0 || remaining <= 0 || remaining > timeout_ms) {
      ret = -ETIMEDOUT;
      goto fail;
   }
   struct render_client_op_jail_checked_reply reply;
   if (!proxy_socket_receive_reply_bounded(&client->socket, &reply, sizeof(reply),
                                            remaining))
      goto fail;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      goto fail;
   elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 +
             now.tv_nsec - start.tv_nsec;
   if (elapsed < 0 || elapsed >= (int64_t)timeout_ms * 1000000) {
      ret = -ETIMEDOUT;
      goto fail;
   }
   if (reply.generation != generation ||
       reply.protocol_version != RENDER_JAIL_CHECKED_VERSION || reply.status > 0 ||
       (uint64_t)reply.reaped_count + reply.remaining_count > UINT32_MAX ||
       (!reply.status && (reply.phase != RENDER_JAIL_EMPTY ||
                          reply.remaining_count)) ||
       (reply.status && reply.phase)) {
      ret = -EPROTO;
      goto fail;
   }
   client->jail_checked_reply_valid = true;
   client->jail_checked_reply = reply;
   client->jail_checked_result = reply.status;
   *out = reply;
   return reply.status;
fail:
   if (client->socket.fd >= 0) {
      close(client->socket.fd);
      client->socket.fd = -1;
   }
   client->jail_checked_result = ret;
   client->jail_checked_reply.status = ret;
   *out = client->jail_checked_reply;
   return ret;
}

bool
proxy_client_reset(struct proxy_client *client)
{
   if (client->reset_checked_attempted)
      return false;
   const struct render_client_op_reset_request req = {
      .header.op = RENDER_CLIENT_OP_RESET,
   };
   return proxy_socket_send_request(&client->socket, &req, sizeof(req));
}

void
proxy_client_destroy(struct proxy_client *client)
{
   proxy_socket_fini(&client->socket);
   free(client);
}

static bool
proxy_client_init(struct proxy_client *client, uint32_t flags)
{
   const struct render_client_op_init_request req = {
      .header.op = RENDER_CLIENT_OP_INIT,
      .flags = flags,
   };
   return proxy_socket_send_request(&client->socket, &req, sizeof(req));
}

struct proxy_client *
proxy_client_create(struct proxy_server *srv, uint32_t flags)
{
   struct proxy_client *client = calloc(1, sizeof(*client));
   if (!client)
      return NULL;

   const int client_fd = proxy_server_connect(srv);
   if (client_fd < 0) {
      free(client);
      return NULL;
   }

   proxy_socket_init(&client->socket, client_fd);

   if (!proxy_client_init(client, flags)) {
      proxy_socket_fini(&client->socket);
      free(client);
      return NULL;
   }

   return client;
}
