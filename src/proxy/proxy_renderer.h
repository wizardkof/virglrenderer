/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_RENDERER_H
#define PROXY_RENDERER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct iovec;
struct virgl_context;
struct render_client_op_reset_checked_reply;

struct proxy_renderer_cbs {
   int (*get_server_fd)(uint32_t version);
};

#ifdef ENABLE_RENDER_SERVER

int
proxy_renderer_init(const struct proxy_renderer_cbs *cbs, uint32_t flags);

void
proxy_renderer_fini(void);

void
proxy_renderer_reset(void);

bool
proxy_renderer_reap_reply_valid(void);

/* Refuse cleanup while a checked parent transaction is pending/failed. */
int
proxy_renderer_cleanup_check(void);

/* Publish a generation-bound jail witness only after a validated RPC. */
int
proxy_renderer_drain_jail_checked(uint64_t generation, int timeout_ms);

/* Same exclusive owner; consumes a successful exact worker-record receipt.
 * Does not release context/client/server owners or grant reset readiness. */
int
proxy_renderer_reap_server_checked(uint64_t generation, int timeout_ms);

/* Initialized context table and exclusive retained topology required. */
int
proxy_renderer_reap_checked(uint64_t generation, int timeout_ms,
                             struct render_client_op_reset_checked_reply *out);

size_t
proxy_get_capset(uint32_t set, void *caps);

struct virgl_context *
proxy_context_create(uint32_t ctx_id,
                     uint32_t ctx_flags,
                     size_t debug_len,
                     const char *debug_name);

#else /* ENABLE_RENDER_SERVER */

static inline int
proxy_renderer_init(UNUSED const struct proxy_renderer_cbs *cbs, UNUSED uint32_t flags)
{
   virgl_error("Render server support was not enabled in virglrenderer\n");
   return -1;
}

static inline void
proxy_renderer_fini(void)
{
}

static inline void
proxy_renderer_reset(void)
{
}

static inline bool
proxy_renderer_reap_reply_valid(void)
{
   return false;
}

static inline int
proxy_renderer_reap_checked(UNUSED uint64_t generation, UNUSED int timeout_ms,
                             UNUSED struct render_client_op_reset_checked_reply *out)
{
   return -EOPNOTSUPP;
}

static inline int
proxy_renderer_cleanup_check(void)
{
   return 0;
}

static inline int
proxy_renderer_drain_jail_checked(UNUSED uint64_t generation, UNUSED int timeout_ms)
{
   return -EOPNOTSUPP;
}

static inline int
proxy_renderer_reap_server_checked(UNUSED uint64_t generation, UNUSED int timeout_ms)
{
   return -EOPNOTSUPP;
}

static inline size_t
proxy_get_capset(UNUSED uint32_t set, UNUSED void *caps)
{
   return 0;
}

static inline struct virgl_context *
proxy_context_create(UNUSED uint32_t ctx_id,
                     UNUSED uint32_t ctx_flags,
                     UNUSED size_t debug_len,
                     UNUSED const char *debug_name)
{
   return NULL;
}

#endif /* ENABLE_RENDER_SERVER */

#endif /* PROXY_RENDERER_H */
