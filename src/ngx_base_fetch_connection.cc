/*
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: oschaaf@we-amp.com (Otto van der Schaaf)
extern "C" {
#include <ngx_channel.h>
}

#include "ngx_base_fetch_connection.h"
#include "ngx_base_fetch.h"

#include "ngx_pagespeed.h"

#include "net/instaweb/rewriter/public/rewrite_stats.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

callbackPtr NgxBaseFetchConnection::event_handler;
// TODO(oschaaf): properly shut down / check restarts / config reloads
int NgxBaseFetchConnection::pagespeed_pipe_write_fd;
int NgxBaseFetchConnection::pagespeed_pipe_read_fd;
ngx_connection_t* NgxBaseFetchConnection::pagespeed_connection;

namespace ngx_base_fetch_connection {

void connection_read_handler(ngx_event_t* ev) {
  ngx_connection_t* c = static_cast<ngx_connection_t*>(ev->data);

  if (ev->timedout) {
    ev->timedout = 0;
    return;
  }

  NgxBaseFetchConnection::ReadHandler(c);
  // TODO(oschaaf): call ngx_handle_read_event() == NGX_OK
}

}  // namespace ngx_base_fetch

bool NgxBaseFetchConnection::Init(ngx_cycle_t* cycle, callbackPtr callback) {
  int file_descriptors[2];

  // TODO(oschaaf): test the unhappy flows here.
  // TODO(oschaaf): fix up log messages.
  if (pipe(file_descriptors) != 0) {
    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0, "pipe() failed");
    return false;
  }

  if (ngx_nonblocking(file_descriptors[0]) == -1) {
    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                  ngx_nonblocking_n " pipe[0] failed");
    close(file_descriptors[0]);
    close(file_descriptors[1]);
    return false;
  }

  if (ngx_nonblocking(file_descriptors[1]) == -1) {
    ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                  ngx_nonblocking_n " pipe[1] failed");
    close(file_descriptors[0]);
    close(file_descriptors[1]);
    return false;
  }

  if (!CreateNgxConnection(cycle, file_descriptors[0])) {
    close(file_descriptors[0]);
    close(file_descriptors[1]);
    // TODO(oschaaf): this has one arg less then the calls above.
    // figure out if that is that a problem?
    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                  "pagespeed: failed to create connection.");
    return false;
  }

  NgxBaseFetchConnection::event_handler = callback;
  pagespeed_pipe_read_fd = file_descriptors[0];
  pagespeed_pipe_write_fd = file_descriptors[1];
  return true;
}

bool NgxBaseFetchConnection::CreateNgxConnection(ngx_cycle_t* cycle,
                                                 int pipe_fd) {
  ngx_int_t rc = ngx_add_channel_event(
      cycle, pipe_fd, NGX_READ_EVENT,
      ngx_base_fetch_connection::connection_read_handler);
  return rc  == NGX_OK;
}

bool NgxBaseFetchConnection::WriteEvent(char type, NgxBaseFetch* sender) {
  ssize_t size = 0;
  ps_event_data data;

  ngx_memzero(&data, sizeof(data));
  data.type = type;
  data.sender = sender;

  // TODO(oschaaf): end loop on shutdown.
  while (true) {
    size = write(pagespeed_pipe_write_fd,
                 static_cast<void*>(&data), sizeof(data));
    if (size == sizeof(data)) {
      return true;
    } else if (size == -1) {
      // TODO(oschaaf): eagain/ewouldblock, could they
      // make us spin forever? Can we delegate the retry to
      // another thread for those?
      if (ngx_errno == EINTR || ngx_errno == EAGAIN
          || ngx_errno == EWOULDBLOCK) {
        continue;
      } else {
        ngx_log_error(NGX_LOG_DEBUG, pagespeed_connection->log, 0,
                      "pagespeed NgxBaseFetchConnection::Write failed (%d)",
                      errno);
        return false;
      }
    } else {
      CHECK(false) << "pagespeed: unexpected return value from write(): "
                   << size;
    }
  }
  CHECK(false) << "Should not get here";
  return false;
}

// Deserialize ps_event_data's from the pipe as they become available.
// Subsequently do some bookkeeping, cleanup, and error checking to keep
// the mess out of ps_base_fetch_handler.
void NgxBaseFetchConnection::ReadHandler(ngx_connection_t* c) {
  for (;;) {
    // TODO(oschaaf): ps_run_events() can end up calling itself when it calls
    // base_fetch_handler(). If that happens in the middle of looping over
    // multiple ps_event_data's we have obtained with read(), the recursion
    // could cause us to process events out of order.
    // To avoid that, read only one ps_event_data at a time for now. 
    ps_event_data data;
    ngx_int_t size = read(pagespeed_pipe_read_fd, static_cast<void*>(&data),
                        sizeof(data));

    if (size == -1) {
      if (errno == EINTR) {
        continue;
      } else if (ngx_errno == EAGAIN || ngx_errno == EWOULDBLOCK) {
        return;
      }
    }

    if (size <= 0) {
        ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, 0,
                    "pagespeed [%p] NgxBaseFetchConnection::Read() del con (%d)",
                      c, size == 0 ? 0 : errno);
        // TODO(oschaaf): CHECK & test. Should we do this in more places?
        if (c != NULL) {
          if (ngx_event_flags & NGX_USE_EPOLL_EVENT) {
            ngx_del_conn(c, 0);
          }
          ngx_close_connection(c);
          close(pagespeed_pipe_read_fd);
        }
      break;
    }

    NgxBaseFetchConnection::event_handler(data);
  }
}

void NgxBaseFetchConnection::Shutdown() {
  ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, 0,
                "pagespeed NgxBaseFetchConnection shutting down");
  close(pagespeed_pipe_write_fd);
  // Drain the pipe, process final events, and shut down.
  NgxBaseFetchConnection::ReadHandler(pagespeed_connection);  
}

}  // namespace net_instaweb
