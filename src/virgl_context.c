/**************************************************************************
 *
 * Copyright (C) 2020 Chromium.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "virgl_context.h"

#include <errno.h>
#include <time.h>

#ifndef WIN32
#include "util/libsync.h"
#endif
#include "util/os_misc.h"
#include "util/u_hash_table.h"
#include "util/u_pointer.h"
#include "virgl_util.h"

static struct util_hash_table *virgl_context_table;

static void
virgl_context_destroy_func(void *val)
{
   struct virgl_context *ctx = val;

   if (ctx->in_fence_fd >= 0)
      close(ctx->in_fence_fd);

   ctx->destroy(ctx);
}

int
virgl_context_table_init(void)
{
   virgl_context_table = util_hash_table_create(hash_func_u32,
                                                equal_func,
                                                virgl_context_destroy_func);
   return virgl_context_table ? 0 : ENOMEM;
}

void
virgl_context_table_cleanup(void)
{
   util_hash_table_destroy(virgl_context_table);
   virgl_context_table = NULL;
}

void
virgl_context_table_reset(void)
{
   util_hash_table_clear(virgl_context_table);
}

int
virgl_context_add(struct virgl_context *ctx)
{
   const enum pipe_error err = util_hash_table_set(
         virgl_context_table, uintptr_to_pointer(ctx->ctx_id), ctx);
   return err == PIPE_OK ? 0 : ENOMEM;
}

struct virgl_context_quiesce_args {
   uint64_t generation;
   int64_t deadline_ns;
   int error;
};

static enum pipe_error
virgl_context_quiesce_entry(UNUSED void *key, void *value, void *data)
{
   struct virgl_context *ctx = value;
   struct virgl_context_quiesce_args *args = data;
   if (!ctx->quiesce_producers) {
      args->error = -EOPNOTSUPP;
      return PIPE_ERROR;
   }
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now)) {
      args->error = -EIO;
      return PIPE_ERROR;
   }
   int64_t now_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
   int64_t remaining_ms = (args->deadline_ns - now_ns) / 1000000;
   if (remaining_ms <= 0) {
      args->error = -ETIMEDOUT;
      return PIPE_ERROR;
   }
   if (ctx->prepare_destroy || ctx->prepare_destroy_bounded) {
      if (!ctx->prepare_destroy_bounded) {
         args->error = -EOPNOTSUPP;
         return PIPE_ERROR;
      }
      const bool prepared = ctx->prepare_destroy_bounded(ctx, (int)remaining_ms);
      if (clock_gettime(CLOCK_MONOTONIC, &now)) {
         args->error = -EIO;
         return PIPE_ERROR;
      }
      now_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
      remaining_ms = (args->deadline_ns - now_ns) / 1000000;
      if (remaining_ms <= 0) {
         args->error = -ETIMEDOUT;
         return PIPE_ERROR;
      }
      if (!prepared) {
         args->error = -EBUSY;
         return PIPE_ERROR;
      }
   }
   args->error = ctx->quiesce_producers(ctx, args->generation, (int)remaining_ms);
   if (args->error)
      return PIPE_ERROR;
   if (clock_gettime(CLOCK_MONOTONIC, &now)) {
      args->error = -EIO;
      return PIPE_ERROR;
   }
   now_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
   if (now_ns > args->deadline_ns) {
      args->error = -ETIMEDOUT;
      return PIPE_ERROR;
   }
   return PIPE_OK;
}

int
virgl_context_quiesce_all(uint64_t generation, int timeout_ms)
{
   if (!generation || timeout_ms <= 0)
      return -EINVAL;
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now))
      return -EIO;
   const int64_t start_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
   const int64_t budget_ns = (int64_t)timeout_ms * 1000000;
   if (start_ns < 0 || start_ns > INT64_MAX - budget_ns)
      return -EINVAL;
   struct virgl_context_quiesce_args args = {
      .generation = generation, .deadline_ns = start_ns + budget_ns,
   };
   if (util_hash_table_foreach(virgl_context_table, virgl_context_quiesce_entry,
                               &args) != PIPE_OK)
      return args.error ? args.error : -EIO;
   return 0;
}

struct virgl_context_reset_release_args {
   struct virgl_resource *res;
   uint64_t generation;
   bool validate_only;
   int error;
};

static enum pipe_error
virgl_context_reset_release_entry(UNUSED void *key, void *value, void *data)
{
   struct virgl_context *ctx = value;
   struct virgl_context_reset_release_args *args = data;
   if (!ctx->release_resource_for_reset) {
      args->error = -EOPNOTSUPP;
      return PIPE_ERROR;
   }
   args->error = ctx->release_resource_for_reset(ctx, args->res,
                                                 args->generation,
                                                 args->validate_only);
   if (args->error > 0)
      args->error = -EPROTO;
   return args->error ? PIPE_ERROR : PIPE_OK;
}

int
virgl_context_release_resource_for_reset(struct virgl_resource *res,
                                         uint64_t generation)
{
   if (!res || !generation)
      return -EINVAL;
   struct virgl_context_reset_release_args args = {
      .res = res, .generation = generation, .validate_only = true,
   };
   if (util_hash_table_foreach(virgl_context_table, virgl_context_reset_release_entry,
                               &args) != PIPE_OK)
      return args.error ? args.error : -EIO;
   /* All context owners are serialized/quiesced. An unexpected commit refusal
    * can leave earlier local markers withdrawn, but the root resource remains
    * owned by the caller; never infer complete release from that failure.
    */
   args.validate_only = false;
   if (util_hash_table_foreach(virgl_context_table, virgl_context_reset_release_entry,
                               &args) != PIPE_OK)
      return args.error ? args.error : -EIO;
   return 0;
}

static enum pipe_error
virgl_context_prepare_destroy_entry(UNUSED void *key, void *value, UNUSED void *data)
{
   struct virgl_context *ctx = value;
   return !ctx->prepare_destroy || ctx->prepare_destroy(ctx) ? PIPE_OK : PIPE_ERROR;
}

bool
virgl_context_prepare_destroy_all(void)
{
   return util_hash_table_foreach(virgl_context_table,
                                  virgl_context_prepare_destroy_entry, NULL) == PIPE_OK;
}

bool
virgl_context_remove_checked(uint32_t ctx_id)
{
   struct virgl_context *ctx = virgl_context_lookup(ctx_id);
   if (!ctx)
      return true;
   if (ctx->prepare_destroy && !ctx->prepare_destroy(ctx))
      return false;
   util_hash_table_remove(virgl_context_table, uintptr_to_pointer(ctx_id));
   return true;
}

void
virgl_context_remove(uint32_t ctx_id)
{
   util_hash_table_remove(virgl_context_table, uintptr_to_pointer(ctx_id));
}

struct virgl_context *
virgl_context_lookup(uint32_t ctx_id)
{
   return util_hash_table_get(virgl_context_table,
                              uintptr_to_pointer(ctx_id));
}

static enum pipe_error
virgl_context_foreach_func(UNUSED void *key, void *val, void *data)
{
   const struct virgl_context_foreach_args *args = data;
   struct virgl_context *ctx = val;

   return args->callback(ctx, args->data) ? PIPE_OK : PIPE_ERROR;
}

void
virgl_context_foreach(const struct virgl_context_foreach_args *args)
{
   util_hash_table_foreach(virgl_context_table,
                           virgl_context_foreach_func,
                           (void *)args);
}

int virgl_context_take_in_fence_fd(struct virgl_context *ctx)
{
   TRACE_FUNC();

   if (ctx->in_fence_fd < 0)
      return -1;

   int ret = ctx->in_fence_fd;
   ctx->in_fence_fd = -1;

   return ret;
}
