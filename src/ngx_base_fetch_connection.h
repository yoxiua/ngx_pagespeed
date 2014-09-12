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
//
// TODO(oschaaf): Doc

#ifndef NGX_BASE_FETCH_CONNECTION_H_
#define NGX_BASE_FETCH_CONNECTION_H_

extern "C" {
#include <ngx_http.h>
}

#include <pthread.h>

#include "ngx_pagespeed.h"

#include "ngx_server_context.h"

#include "net/instaweb/http/public/async_fetch.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/http/headers.h"

namespace net_instaweb {

const char kHeadersComplete = 'H';
const char kFlush = 'F';
const char kDone = 'D';

typedef struct {
  ngx_http_request_t* r;
  char type;
  NgxBaseFetch* sender;
} ps_event_data;

class NgxBaseFetch;

// TODO(oschaaf): callbackPtr -> baseFetchCallbackPtr
typedef void (*callbackPtr)(const ps_event_data&);

class NgxBaseFetchConnection {
 public:  
  // Creates the file descriptors and ngx_connection_t required for event
  // messaging between pagespeed and nginx.
  static bool Init(ngx_cycle_t* cycle, callbackPtr handler);
  // Shuts down the pagespeed connection created in Init()
  static void Shutdown();
  static void ReadHandler(ngx_connection_t* c);
  static bool WriteEvent(char type, NgxBaseFetch* sender);

 private:
  NgxBaseFetchConnection() {}

  static bool CreateNgxConnection(ngx_cycle_t* cycle, int pipe_fd);

  static callbackPtr event_handler;
  // We own the underlying fd's and connection.
  static int pagespeed_pipe_write_fd;
  static int pagespeed_pipe_read_fd;
  static ngx_connection_t* pagespeed_connection;
};

}  // namespace net_instaweb

#endif  // NGX_BASE_FETCH_CONNECTION_H_
