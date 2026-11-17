/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_CLIENT_H
#define PROXY_CLIENT_H

#include "proxy_common.h"
#include "server/render_protocol.h"

struct proxy_client {
   struct proxy_socket socket;
   bool reset_checked_attempted;
   bool reset_checked_reply_valid;
   uint64_t reset_checked_generation;
   uint32_t reset_checked_expected_records;
   int reset_checked_result;
   struct render_client_op_reset_checked_reply reset_checked_reply;
   bool jail_checked_attempted;
   bool jail_checked_reply_valid;
   uint64_t jail_checked_generation;
   int jail_checked_result;
   struct render_client_op_jail_checked_reply jail_checked_reply;
};

struct proxy_client *
proxy_client_create(struct proxy_server *srv, uint32_t flags);

void
proxy_client_destroy(struct proxy_client *client);

/* Exclusive serialized client-channel owner; freezes legacy topology ops.
 * One attempt per client/generation, total send/receive budget. A validated
 * server failure retains the channel; transport/identity failure invalidates.
 * reply_valid distinguishes remote evidence from local failure bookkeeping.
 */
int
proxy_client_reset_checked(struct proxy_client *client, uint64_t generation,
                            uint32_t expected_records, int timeout_ms,
                            struct render_client_op_reset_checked_reply *out);

/* Same exclusive channel owner, after exact successful record reset. */
int
proxy_client_jail_checked(struct proxy_client *client, uint64_t generation,
                           int timeout_ms,
                           struct render_client_op_jail_checked_reply *out);

bool
proxy_client_reset(struct proxy_client *client);

bool
proxy_client_create_context(struct proxy_client *client,
                            uint32_t ctx_id,
                            size_t ctx_name_len,
                            const char *ctx_name,
                            int *out_ctx_fd);

bool
proxy_client_destroy_context(struct proxy_client *client, uint32_t ctx_id);

#endif /* PROXY_CLIENT_H */
