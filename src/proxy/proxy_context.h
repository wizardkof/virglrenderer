/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_CONTEXT_H
#define PROXY_CONTEXT_H

#include "proxy_common.h"
#include "vkr_reset_wait.h"

#include "c11/threads.h"
#include "virgl_context.h"

/* matches virtio-gpu */
#define PROXY_CONTEXT_TIMELINE_COUNT 64

static_assert(ATOMIC_INT_LOCK_FREE == 2, "proxy renderer requires lock-free atomic_uint");

struct proxy_timeline {
   uint32_t cur_seqno;
   uint32_t next_seqno;
   struct list_head fences;

   int cur_seqno_stall_count;
};

struct proxy_context {
   struct virgl_context base;

   struct proxy_client *client;
   struct proxy_socket socket;
   /* Serializes complete request/reply operations; owner retains ctx lifetime. */
   mtx_t channel_mutex;
   /* Irreversible quarantine: no completion inferred from disconnect. */
   atomic_bool quiesce_fences_held;
   bool producers_quiesced;
   uint64_t quiesce_generation;

   /* this tracks resources early attached in get_blob */
   struct hash_table *resource_table;

   /* this is shared with the render worker */
   struct {
      int fd;
      size_t size;
      void *ptr;
   } shmem;

   mtx_t timeline_mutex;
   struct proxy_timeline timelines[PROXY_CONTEXT_TIMELINE_COUNT];
   /* which timelines have fences */
   uint64_t timeline_busy_mask;
   /* this points a region of shmem updated by the render worker */
   const volatile atomic_uint *timeline_seqnos;

   mtx_t free_fences_mutex;
   struct list_head free_fences;

   struct {
      /* when VIRGL_RENDERER_THREAD_SYNC is set */
      int fence_eventfd;

      /* when VIRGL_RENDERER_ASYNC_FENCE_CB is also set */
      thrd_t thread;
      bool created;
      bool join_attempted;
      bool joined;
      atomic_bool stop;
   } sync_thread;
};

/* Read-only exact-generation witness for the serialized context-table owner. */
int
proxy_context_reset_reap_ready(struct virgl_context *base, uint64_t generation);

/* Exclusive channel owner only; true means valid reply, inspect status.
 * No caller may treat this as reset-ready or free resources on timeout.
 */
bool
proxy_context_quiesce(struct proxy_context *ctx, uint64_t generation,
                       int timeout_ms, struct vkr_reset_wait_result *out);

#endif /* PROXY_CONTEXT_H */
