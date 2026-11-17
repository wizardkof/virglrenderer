/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_server.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#include "server/render_protocol.h"

#ifdef ENABLE_SAME_PROCESS_RENDER_SERVER
#include "server/render_context.h"
#include "server/render_server.h"
#endif

int
proxy_server_connect(struct proxy_server *srv)
{
   int client_fd = srv->client_fd;
   /* transfer ownership */
   srv->client_fd = -1;
   return client_fd;
}

int
proxy_server_reap_checked(struct proxy_server *srv, int timeout_ms)
{
   if (!srv || timeout_ms <= 0)
      return -EINVAL;
#ifdef ENABLE_SAME_PROCESS_RENDER_SERVER
   /* No bounded thread-join witness exists here. */
   return -EOPNOTSUPP;
#else
   struct timespec start, now;
   const int64_t budget = (int64_t)timeout_ms * 1000000;
   if (srv->pid <= 0)
      return -EOPNOTSUPP;
   if (clock_gettime(CLOCK_MONOTONIC, &start))
      return -errno;
   if (!srv->reaped && kill(srv->pid, SIGKILL) && errno != ESRCH)
      return -errno;
   for (;;) {
      if (!srv->reaped) {
         siginfo_t info = { 0 };
         int ret = waitid(P_PID, srv->pid, &info, WEXITED | WNOHANG);
         if (ret && errno != EINTR)
            return -errno;
         if (!ret && info.si_pid == srv->pid) {
            if (info.si_code != CLD_EXITED && info.si_code != CLD_KILLED &&
                info.si_code != CLD_DUMPED)
               return -EPROTO;
            /* Persist even if the budget expires immediately afterwards:
             * a retry must never signal a PID which may have been reused. */
            srv->reaped = true;
         }
      }
      if (clock_gettime(CLOCK_MONOTONIC, &now))
         return -errno;
      int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000 +
                        now.tv_nsec - start.tv_nsec;
      if (elapsed < 0)
         return -EIO;
      if (elapsed >= budget)
         return -ETIMEDOUT;
      if (srv->reaped)
         return 0;
      int64_t remaining = budget - elapsed;
      struct timespec pause = {
         .tv_sec = 0,
         .tv_nsec = remaining < 1000000 ? remaining : 1000000,
      };
      if (nanosleep(&pause, NULL) && errno != EINTR)
         return -errno;
   }
#endif
}

void
proxy_server_destroy(struct proxy_server *srv)
{
   if (srv->pid > 0 && !srv->reaped) {
      kill(srv->pid, SIGKILL);

      siginfo_t siginfo = { 0 };
      waitid(P_PID, srv->pid, &siginfo, WEXITED);
   }

   if (srv->client_fd >= 0)
      close(srv->client_fd);

#ifdef ENABLE_SAME_PROCESS_RENDER_SERVER
   thrd_join(srv->thread, NULL);
#endif

   free(srv);
}

#ifndef ENABLE_SAME_PROCESS_RENDER_SERVER
static bool
proxy_server_fork(struct proxy_server *srv)
{
   int socket_fds[2];
   if (!proxy_socket_pair(socket_fds))
      return false;
   const int client_fd = socket_fds[0];
   const int remote_fd = socket_fds[1];

   pid_t pid = fork();
   if (pid < 0) {
      proxy_log("failed to fork proxy server");
      close(client_fd);
      close(remote_fd);
      return false;
   }

   if (pid > 0) {
      srv->pid = pid;
      srv->client_fd = client_fd;
      close(remote_fd);
   } else {
      close(client_fd);

      /* do not receive signals from terminal */
      setpgid(0, 0);

      char fd_str[16];
      snprintf(fd_str, sizeof(fd_str), "%d", remote_fd);

      /* for devenv without installing server */
      char *const server_path = getenv("RENDER_SERVER_EXEC_PATH");
      char *const argv[] = {
         server_path ? server_path : RENDER_SERVER_EXEC_PATH,
         "--socket-fd",
         fd_str,
         NULL,
      };
      execv(argv[0], argv);

      proxy_log("failed to exec %s: %s", argv[0], strerror(errno));
      close(remote_fd);
      exit(-1);
   }

   return true;
}

static bool
proxy_server_init_fd(struct proxy_server *srv)
{
   /* the fd represents a connection to the server */
   srv->client_fd = proxy_renderer.cbs->get_server_fd(RENDER_SERVER_VERSION);
   if (srv->client_fd < 0)
      return false;

   return true;
}

#else  /* ENABLE_SAME_PROCESS_RENDER_SERVER */

static int
proxy_server_start_thread(void *args)
{
   int remote_fd = (int)(uintptr_t)args;
   char fd_str[16];
   snprintf(fd_str, sizeof(fd_str), "%d", remote_fd);
   char *argv[] = {
      RENDER_SERVER_EXEC_PATH,
      "--socket-fd",
      fd_str,
      NULL,
   };
   struct render_context_args ctx_args;

   /* must reset the global getopt index */
   optind = 1;

   bool ok = render_server_main(ARRAY_SIZE(argv) - 1, argv, &ctx_args);

   return ok ? 0 : -1;
}

static bool
proxy_server_init_thread(struct proxy_server *srv)
{
   int socket_fds[2];

   if (!proxy_socket_pair(socket_fds))
      return false;

   const int client_fd = socket_fds[0];
   const int remote_fd = socket_fds[1];

   bool ok = thrd_create(&srv->thread, proxy_server_start_thread,
                         (void *)(uintptr_t)remote_fd) == thrd_success;

   if (ok) {
      srv->client_fd = client_fd;
   } else {
      close(client_fd);
      close(remote_fd);
   }

   return ok;
}
#endif /* ENABLE_SAME_PROCESS_RENDER_SERVER */

struct proxy_server *
proxy_server_create(void)
{
   struct proxy_server *srv = calloc(1, sizeof(*srv));
   if (!srv)
      return NULL;

   srv->pid = -1;

#ifdef ENABLE_SAME_PROCESS_RENDER_SERVER
   if (!proxy_server_init_thread(srv)) {
      free(srv);
      return NULL;
   }
#else
   if (!proxy_server_init_fd(srv)) {
      /* start the render server on demand when the client does not provide a
       * server fd
       */
      if (!proxy_server_fork(srv)) {
         free(srv);
         return NULL;
      }
   }
#endif

   if (!proxy_socket_is_valid(srv->client_fd)) {
      proxy_log("invalid client fd type");
      close(srv->client_fd);
      free(srv);
      return NULL;
   }

   proxy_log("proxy server with pid %d", srv->pid);

   return srv;
}
