/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_http2_upstream.h"

#include <netinet/tcp.h>
#include <assert.h>
#include <cerrno>
#include <sstream>

// ADDITIONAL
#include <iostream>
// END ADDITIONAL

#include "shrpx_client_handler.h"
#include "shrpx_https_upstream.h"
#include "shrpx_downstream.h"
#include "shrpx_downstream_connection.h"
#include "shrpx_config.h"
#include "shrpx_http.h"
#include "shrpx_worker.h"
#include "shrpx_http2_session.h"
#ifdef HAVE_MRUBY
#include "shrpx_mruby.h"
#endif // HAVE_MRUBY
#include "http2.h"
#include "util.h"
#include "base64.h"
#include "app_helper.h"
#include "template.h"

using namespace nghttp2;

namespace shrpx {

namespace {
constexpr size_t MAX_BUFFER_SIZE = 32_k;
} // namespace

namespace {
int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
                             uint32_t error_code, void *user_data) {
  auto upstream = static_cast<Http2Upstream *>(user_data);
  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, upstream) << "Stream stream_id=" << stream_id
                         << " is being closed";
  }

  auto downstream = static_cast<Downstream *>(
      nghttp2_session_get_stream_user_data(session, stream_id));

  if (!downstream) {
    return 0;
  }

  auto &req = downstream->request();

  upstream->consume(stream_id, req.unconsumed_body_length);

  req.unconsumed_body_length = 0;

  if (downstream->get_request_state() == Downstream::CONNECT_FAIL) {
    upstream->remove_downstream(downstream);
    // downstream was deleted

    return 0;
  }

  if (downstream->can_detach_downstream_connection()) {
    // Keep-alive
    downstream->detach_downstream_connection();
  }

  downstream->set_request_state(Downstream::STREAM_CLOSED);

  // At this point, downstream read may be paused.

  // If shrpx_downstream::push_request_headers() failed, the
  // error is handled here.
  upstream->remove_downstream(downstream);
  // downstream was deleted

  // How to test this case? Request sufficient large download
  // and make client send RST_STREAM after it gets first DATA
  // frame chunk.

  return 0;
}
} // namespace

int Http2Upstream::upgrade_upstream(HttpsUpstream *http) {
  int rv;

  auto http2_settings = http->get_downstream()->get_http2_settings().str();
  util::to_base64(http2_settings);

  auto settings_payload =
      base64::decode(std::begin(http2_settings), std::end(http2_settings));

  rv = nghttp2_session_upgrade2(
      session_, reinterpret_cast<const uint8_t *>(settings_payload.c_str()),
      settings_payload.size(),
      http->get_downstream()->request().method == HTTP_HEAD, nullptr);
  if (rv != 0) {
    if (LOG_ENABLED(INFO)) {
      ULOG(INFO, this) << "nghttp2_session_upgrade() returned error: "
                       << nghttp2_strerror(rv);
    }
    return -1;
  }
  pre_upstream_.reset(http);
  auto downstream = http->pop_downstream();
  downstream->reset_upstream(this);
  downstream->set_stream_id(1);
  downstream->reset_upstream_rtimer();
  downstream->set_stream_id(1);

  auto ptr = downstream.get();

  nghttp2_session_set_stream_user_data(session_, 1, ptr);
  downstream_queue_.add_pending(std::move(downstream));
  downstream_queue_.mark_active(ptr);

  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, this) << "Connection upgraded to HTTP/2";
  }

  return 0;
}

void Http2Upstream::start_settings_timer() {
  ev_timer_start(handler_->get_loop(), &settings_timer_);
}

void Http2Upstream::stop_settings_timer() {
  ev_timer_stop(handler_->get_loop(), &settings_timer_);
}

namespace {
int on_header_callback2(nghttp2_session *session, const nghttp2_frame *frame,
                        nghttp2_rcbuf *name, nghttp2_rcbuf *value,
                        uint8_t flags, void *user_data) {
  auto namebuf = nghttp2_rcbuf_get_buf(name);
  auto valuebuf = nghttp2_rcbuf_get_buf(value);

  if (get_config()->http2.upstream.debug.frame_debug) {
    verbose_on_header_callback(session, frame, namebuf.base, namebuf.len,
                               valuebuf.base, valuebuf.len, flags, user_data);
  }
  if (frame->hd.type != NGHTTP2_HEADERS) {
    return 0;
  }
  auto upstream = static_cast<Http2Upstream *>(user_data);
  auto downstream = static_cast<Downstream *>(
      nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
  if (!downstream) {
    return 0;
  }

  auto &req = downstream->request();

  auto &httpconf = get_config()->http;

  if (req.fs.buffer_size() + namebuf.len + valuebuf.len >
          httpconf.request_header_field_buffer ||
      req.fs.num_fields() >= httpconf.max_request_header_fields) {
    if (downstream->get_response_state() == Downstream::MSG_COMPLETE) {
      return 0;
    }

    if (LOG_ENABLED(INFO)) {
      ULOG(INFO, upstream) << "Too large or many header field size="
                           << req.fs.buffer_size() + namebuf.len + valuebuf.len
                           << ", num=" << req.fs.num_fields() + 1;
    }

    // just ignore header fields if this is trailer part.
    if (frame->headers.cat == NGHTTP2_HCAT_HEADERS) {
      return 0;
    }

    if (upstream->error_reply(downstream, 431) != 0) {
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }

    return 0;
  }

  auto token = http2::lookup_token(namebuf.base, namebuf.len);
  auto no_index = flags & NGHTTP2_NV_FLAG_NO_INDEX;

  downstream->add_rcbuf(name);
  downstream->add_rcbuf(value);

  if (frame->headers.cat == NGHTTP2_HCAT_HEADERS) {
    // just store header fields for trailer part
    req.fs.add_trailer_token(StringRef{namebuf.base, namebuf.len},
                             StringRef{valuebuf.base, valuebuf.len}, no_index,
                             token);
    return 0;
  }

  req.fs.add_header_token(StringRef{namebuf.base, namebuf.len},
                          StringRef{valuebuf.base, valuebuf.len}, no_index,
                          token);
  return 0;
}
} // namespace

namespace {
int on_begin_headers_callback(nghttp2_session *session,
                              const nghttp2_frame *frame, void *user_data) {
  auto upstream = static_cast<Http2Upstream *>(user_data);

  if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }
  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, upstream) << "Received upstream request HEADERS stream_id="
                         << frame->hd.stream_id;
  }

  auto handler = upstream->get_client_handler();

  auto downstream = make_unique<Downstream>(upstream, handler->get_mcpool(),
                                            frame->hd.stream_id);
  nghttp2_session_set_stream_user_data(session, frame->hd.stream_id,
                                       downstream.get());

  downstream->reset_upstream_rtimer();

  auto &req = downstream->request();

  // Although, we deprecated minor version from HTTP/2, we supply
  // minor version 0 to use via header field in a conventional way.
  req.http_major = 2;
  req.http_minor = 0;

  upstream->add_pending_downstream(std::move(downstream));

  return 0;
}
} // namespace

int Http2Upstream::on_request_headers(Downstream *downstream,
                                      const nghttp2_frame *frame) {
  if (downstream->get_response_state() == Downstream::MSG_COMPLETE) {
    return 0;
  }

  auto &req = downstream->request();
  auto &nva = req.fs.headers();

  if (LOG_ENABLED(INFO)) {
    std::stringstream ss;
    for (auto &nv : nva) {
      ss << TTY_HTTP_HD << nv.name << TTY_RST << ": " << nv.value << "\n";
    }
    ULOG(INFO, this) << "HTTP request headers. stream_id="
                     << downstream->get_stream_id() << "\n" << ss.str();
  }

  auto &dump = get_config()->http2.upstream.debug.dump;

  if (dump.request_header) {
    http2::dump_nv(dump.request_header, nva);
  }

  auto content_length = req.fs.header(http2::HD_CONTENT_LENGTH);
  if (content_length) {
    // libnghttp2 guarantees this can be parsed
    req.fs.content_length = util::parse_uint(content_length->value);
  }

  // presence of mandatory header fields are guaranteed by libnghttp2.
  auto authority = req.fs.header(http2::HD__AUTHORITY);
  auto path = req.fs.header(http2::HD__PATH);
  auto method = req.fs.header(http2::HD__METHOD);
  auto scheme = req.fs.header(http2::HD__SCHEME);

  auto method_token = http2::lookup_method_token(method->value);
  if (method_token == -1) {
    if (error_reply(downstream, 501) != 0) {
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    return 0;
  }

  // For HTTP/2 proxy, we request :authority.
  if (method_token != HTTP_CONNECT && get_config()->http2_proxy && !authority) {
    rst_stream(downstream, NGHTTP2_PROTOCOL_ERROR);
    return 0;
  }

  req.method = method_token;
  if (scheme) {
    req.scheme = scheme->value;
  }

  // TODO: hack
  if (req.scheme.str().empty()) {
    req.scheme = StringRef("https");
  }

  // nghttp2 library guarantees either :authority or host exist
  if (!authority) {
    req.no_authority = true;
    authority = req.fs.header(http2::HD_HOST);
  }

  if (authority) {
    req.authority = authority->value;
  }

  if (path) {
    if (method_token == HTTP_OPTIONS &&
        path->value == StringRef::from_lit("*")) {
      // Server-wide OPTIONS request.  Path is empty.
    } else if (get_config()->http2_proxy) {
      req.path = path->value;
    } else {
      req.path = http2::rewrite_clean_path(downstream->get_block_allocator(),
                                           path->value);
    }
  }

  if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
    req.http2_expect_body = true;
  }

  downstream->inspect_http2_request();

  downstream->set_request_state(Downstream::HEADER_COMPLETE);

#ifdef HAVE_MRUBY
  auto upstream = downstream->get_upstream();
  auto handler = upstream->get_client_handler();
  auto worker = handler->get_worker();
  auto mruby_ctx = worker->get_mruby_context();

  if (mruby_ctx->run_on_request_proc(downstream) != 0) {
    if (error_reply(downstream, 500) != 0) {
      return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    return 0;
  }
#endif // HAVE_MRUBY

  if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
    downstream->disable_upstream_rtimer();

    downstream->set_request_state(Downstream::MSG_COMPLETE);
  }

  if (downstream->get_response_state() == Downstream::MSG_COMPLETE) {
    return 0;
  }

  // ADDITIONAL
  // dep_reader_.set_url(req.path);
  std::cout << "url: " << construct_url(req) << " requesting dependencies: " << (frame->hd.flags & NGHTTP2_FLAG_REQUESTING_DEPENDENCIES) << " on stream " << frame->hd.stream_id << std::endl;
  if (req.path == "/") { // TODO: This is incorrect. There are cases where the first page will be "http://foo.com/index.html".
    dep_reader_.Start(req.authority.str());
  }
  // if (req.path == "/") {
  //   did_sent_dependency_ = false;
  // } else { 
  //   nghttp2_session_set_still_have_dependencies(session_, frame->hd.stream_id, 0);
  // }
  if (frame->hd.flags & NGHTTP2_FLAG_REQUESTING_DEPENDENCIES) {
    start_resolving_dependencies(construct_url(req), downstream->get_stream_id());
  }
  // END ADDITIONAL

  start_downstream(downstream);

  return 0;
}

void Http2Upstream::start_downstream(Downstream *downstream) {
  if (downstream_queue_.can_activate(downstream->request().authority)) {
    initiate_downstream(downstream);
    return;
  }
  std::cout << "[http2_upstream.cc] blocking downstream" << std::endl;
  downstream_queue_.mark_blocked(downstream);
}

void Http2Upstream::initiate_downstream(Downstream *downstream) {
  int rv;

  auto dconn = handler_->get_downstream_connection(downstream);
  if (!dconn ||
      (rv = downstream->attach_downstream_connection(std::move(dconn))) != 0) {
    // downstream connection fails, send error page
    if (error_reply(downstream, 503) != 0) {
      rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
    }

    downstream->set_request_state(Downstream::CONNECT_FAIL);

    downstream_queue_.mark_failure(downstream);

    return;
  }
  rv = downstream->push_request_headers();
  if (rv != 0) {

    if (error_reply(downstream, 503) != 0) {
      rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
    }

    downstream_queue_.mark_failure(downstream);

    return;
  }

  downstream_queue_.mark_active(downstream);

  // ADDITIONAL
  // std::cout << "[shrpx_http2_upstream] Connected to downstream" << std::endl;
  // END ADDITIONAL

  return;
}

namespace {
int on_frame_recv_callback(nghttp2_session *session, const nghttp2_frame *frame,
                           void *user_data) {
  if (get_config()->http2.upstream.debug.frame_debug) {
    verbose_on_frame_recv_callback(session, frame, user_data);
  }
  auto upstream = static_cast<Http2Upstream *>(user_data);
  auto handler = upstream->get_client_handler();

  handler->signal_reset_upstream_conn_rtimer();

  switch (frame->hd.type) {
  case NGHTTP2_DATA: {
    auto downstream = static_cast<Downstream *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!downstream) {
      return 0;
    }

    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
      downstream->disable_upstream_rtimer();

      downstream->end_upload_data();
      downstream->set_request_state(Downstream::MSG_COMPLETE);
    }

    return 0;
  }
  case NGHTTP2_HEADERS: {
    std::cout << "[shrpx_http2_upstream.cc] received HEADERS" << std::endl;
    auto downstream = static_cast<Downstream *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!downstream) {
      return 0;
    }

    if (frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
      downstream->reset_upstream_rtimer();

      return upstream->on_request_headers(downstream, frame);
    }

    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
      downstream->disable_upstream_rtimer();

      downstream->end_upload_data();
      downstream->set_request_state(Downstream::MSG_COMPLETE);
    }

    return 0;
  }
  case NGHTTP2_SETTINGS:
    if ((frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
      return 0;
    }
    upstream->stop_settings_timer();
    return 0;
  case NGHTTP2_GOAWAY:
    if (LOG_ENABLED(INFO)) {
      auto debug_data = util::ascii_dump(frame->goaway.opaque_data,
                                         frame->goaway.opaque_data_len);

      ULOG(INFO, upstream) << "GOAWAY received: last-stream-id="
                           << frame->goaway.last_stream_id
                           << ", error_code=" << frame->goaway.error_code
                           << ", debug_data=" << debug_data;
    }
    return 0;
  // ADDTITIONAL
  case EXT_DEPENDENCY:
    std::cout << "[shrpx_http2_upstream.cc] Received Dependency" << std::endl;
    // We have received a dependency frame from the client. This indicates that
    // the client would like to initiate a dependency stream with us.
    // Ensure that we create a dependency stream.
    return 0;
  // END ADDTITIONAL
  default:
    return 0;
  }
}
} // namespace

namespace {
int on_data_chunk_recv_callback(nghttp2_session *session, uint8_t flags,
                                int32_t stream_id, const uint8_t *data,
                                size_t len, void *user_data) {
  auto upstream = static_cast<Http2Upstream *>(user_data);
  auto downstream = static_cast<Downstream *>(
      nghttp2_session_get_stream_user_data(session, stream_id));

  if (!downstream || !downstream->get_downstream_connection()) {
    if (upstream->consume(stream_id, len) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
  }

  downstream->reset_upstream_rtimer();

  if (downstream->push_upload_data_chunk(data, len) != 0) {
    upstream->rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);

    if (upstream->consume(stream_id, len) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
  }

  return 0;
}
} // namespace

namespace {
int on_frame_send_callback(nghttp2_session *session, const nghttp2_frame *frame,
                           void *user_data) {
  if (get_config()->http2.upstream.debug.frame_debug) {
    verbose_on_frame_send_callback(session, frame, user_data);
  }
  auto upstream = static_cast<Http2Upstream *>(user_data);
  auto handler = upstream->get_client_handler();

  switch (frame->hd.type) {
  case NGHTTP2_DATA:
  case NGHTTP2_HEADERS: {
    if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0) {
      return 0;
    }
    // RST_STREAM if request is still incomplete.
    auto stream_id = frame->hd.stream_id;
    auto downstream = static_cast<Downstream *>(
        nghttp2_session_get_stream_user_data(session, stream_id));

    if (!downstream) {
      return 0;
    }

    // For tunneling, issue RST_STREAM to finish the stream.
    if (downstream->get_upgraded() ||
        nghttp2_session_get_stream_remote_close(session, stream_id) == 0) {
      if (LOG_ENABLED(INFO)) {
        ULOG(INFO, upstream)
            << "Send RST_STREAM to "
            << (downstream->get_upgraded() ? "tunneled " : "")
            << "stream stream_id=" << downstream->get_stream_id()
            << " to finish off incomplete request";
      }

      upstream->rst_stream(downstream, NGHTTP2_NO_ERROR);
    }

    return 0;
  }
  case NGHTTP2_SETTINGS:
    if ((frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
      upstream->start_settings_timer();
    }
    return 0;
  case NGHTTP2_PUSH_PROMISE: {
    auto promised_stream_id = frame->push_promise.promised_stream_id;

    if (nghttp2_session_get_stream_user_data(session, promised_stream_id)) {
      // In case of push from backend, downstream object was already
      // created.
      return 0;
    }

    auto promised_downstream = make_unique<Downstream>(
        upstream, handler->get_mcpool(), promised_stream_id);
    auto &req = promised_downstream->request();

    // As long as we use nghttp2_session_mem_send(), setting stream
    // user data here should not fail.  This is because this callback
    // is called just after frame was serialized.  So no worries about
    // hanging Downstream.
    nghttp2_session_set_stream_user_data(session, promised_stream_id,
                                         promised_downstream.get());

    promised_downstream->set_assoc_stream_id(frame->hd.stream_id);
    promised_downstream->disable_upstream_rtimer();

    req.http_major = 2;
    req.http_minor = 0;

    auto &promised_balloc = promised_downstream->get_block_allocator();

    for (size_t i = 0; i < frame->push_promise.nvlen; ++i) {
      auto &nv = frame->push_promise.nva[i];

      auto name =
          make_string_ref(promised_balloc, StringRef{nv.name, nv.namelen});
      auto value =
          make_string_ref(promised_balloc, StringRef{nv.value, nv.valuelen});

      auto token = http2::lookup_token(nv.name, nv.namelen);
      switch (token) {
      case http2::HD__METHOD:
        req.method = http2::lookup_method_token(value);
        break;
      case http2::HD__SCHEME:
        req.scheme = value;
        break;
      case http2::HD__AUTHORITY:
        req.authority = value;
        break;
      case http2::HD__PATH:
        req.path = http2::rewrite_clean_path(promised_balloc, value);
        break;
      }
      req.fs.add_header_token(name, value, nv.flags & NGHTTP2_NV_FLAG_NO_INDEX,
                              token);
    }

    promised_downstream->inspect_http2_request();

    promised_downstream->set_request_state(Downstream::MSG_COMPLETE);

    // a bit weird but start_downstream() expects that given
    // downstream is in pending queue.
    auto ptr = promised_downstream.get();
    upstream->add_pending_downstream(std::move(promised_downstream));

#ifdef HAVE_MRUBY
    auto worker = handler->get_worker();
    auto mruby_ctx = worker->get_mruby_context();

    if (mruby_ctx->run_on_request_proc(ptr) != 0) {
      if (upstream->error_reply(ptr, 500) != 0) {
        upstream->rst_stream(ptr, NGHTTP2_INTERNAL_ERROR);
        return 0;
      }
      return 0;
    }
#endif // HAVE_MRUBY

    upstream->start_downstream(ptr);

    return 0;
  }
  case NGHTTP2_GOAWAY:
    if (LOG_ENABLED(INFO)) {
      auto debug_data = util::ascii_dump(frame->goaway.opaque_data,
                                         frame->goaway.opaque_data_len);

      ULOG(INFO, upstream) << "Sending GOAWAY: last-stream-id="
                           << frame->goaway.last_stream_id
                           << ", error_code=" << frame->goaway.error_code
                           << ", debug_data=" << debug_data;
    }
    return 0;
  default:
    return 0;
  }
}
} // namespace

namespace {
int on_frame_not_send_callback(nghttp2_session *session,
                               const nghttp2_frame *frame, int lib_error_code,
                               void *user_data) {
  auto upstream = static_cast<Http2Upstream *>(user_data);
  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, upstream) << "Failed to send control frame type="
                         << static_cast<uint32_t>(frame->hd.type)
                         << ", lib_error_code=" << lib_error_code << ":"
                         << nghttp2_strerror(lib_error_code);
  }
  if (frame->hd.type == NGHTTP2_HEADERS &&
      lib_error_code != NGHTTP2_ERR_STREAM_CLOSED &&
      lib_error_code != NGHTTP2_ERR_STREAM_CLOSING) {
    // To avoid stream hanging around, issue RST_STREAM.
    auto downstream = static_cast<Downstream *>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (downstream) {
      upstream->rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
    }
  }
  return 0;
}
} // namespace

namespace {
constexpr auto PADDING = std::array<uint8_t, 256>{};
} // namespace

namespace {
int send_data_callback(nghttp2_session *session, nghttp2_frame *frame,
                       const uint8_t *framehd, size_t length,
                       nghttp2_data_source *source, void *user_data) {
  auto downstream = static_cast<Downstream *>(source->ptr);
  auto upstream = static_cast<Http2Upstream *>(downstream->get_upstream());
  auto body = downstream->get_response_buf();

  auto wb = upstream->get_response_buf();

  size_t padlen = 0;

  wb->append(framehd, 9);
  if (frame->data.padlen > 0) {
    padlen = frame->data.padlen - 1;
    wb->append(static_cast<uint8_t>(padlen));
  }

  body->remove(*wb, length);

  wb->append(PADDING.data(), padlen);

  downstream->reset_upstream_wtimer();

  if (length > 0 && downstream->resume_read(SHRPX_NO_BUFFER, length) != 0) {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  // We have to add length here, so that we can log this amount of
  // data transferred.
  downstream->response_sent_body_length += length;

  return wb->rleft() >= MAX_BUFFER_SIZE ? NGHTTP2_ERR_PAUSE : 0;
}
} // namespace

namespace {
uint32_t infer_upstream_rst_stream_error_code(uint32_t downstream_error_code) {
  // NGHTTP2_REFUSED_STREAM is important because it tells upstream
  // client to retry.
  switch (downstream_error_code) {
  case NGHTTP2_NO_ERROR:
  case NGHTTP2_REFUSED_STREAM:
    return downstream_error_code;
  default:
    return NGHTTP2_INTERNAL_ERROR;
  }
}
} // namespace

namespace {
void settings_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto upstream = static_cast<Http2Upstream *>(w->data);
  auto handler = upstream->get_client_handler();
  ULOG(INFO, upstream) << "SETTINGS timeout";
  if (upstream->terminate_session(NGHTTP2_SETTINGS_TIMEOUT) != 0) {
    delete handler;
    return;
  }
  handler->signal_write();
}
} // namespace

namespace {
void shutdown_timeout_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto upstream = static_cast<Http2Upstream *>(w->data);
  auto handler = upstream->get_client_handler();
  upstream->submit_goaway();
  handler->signal_write();
}
} // namespace

namespace {
void prepare_cb(struct ev_loop *loop, ev_prepare *w, int revents) {
  auto upstream = static_cast<Http2Upstream *>(w->data);
  upstream->check_shutdown();
}
} // namespace

void Http2Upstream::submit_goaway() {
  auto last_stream_id = nghttp2_session_get_last_proc_stream_id(session_);
  nghttp2_submit_goaway(session_, NGHTTP2_FLAG_NONE, last_stream_id,
                        NGHTTP2_NO_ERROR, nullptr, 0);
}

void Http2Upstream::check_shutdown() {
  int rv;
  if (shutdown_handled_) {
    return;
  }

  auto worker = handler_->get_worker();

  if (worker->get_graceful_shutdown()) {
    shutdown_handled_ = true;
    rv = nghttp2_submit_shutdown_notice(session_);
    if (rv != 0) {
      ULOG(FATAL, this) << "nghttp2_submit_shutdown_notice() failed: "
                        << nghttp2_strerror(rv);
      return;
    }
    handler_->signal_write();
    ev_timer_start(handler_->get_loop(), &shutdown_timer_);
  }
}

nghttp2_session_callbacks *create_http2_upstream_callbacks() {
  int rv;
  nghttp2_session_callbacks *callbacks;

  rv = nghttp2_session_callbacks_new(&callbacks);

  if (rv != 0) {
    return nullptr;
  }

  nghttp2_session_callbacks_set_on_stream_close_callback(
      callbacks, on_stream_close_callback);

  nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
                                                       on_frame_recv_callback);

  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      callbacks, on_data_chunk_recv_callback);

  nghttp2_session_callbacks_set_on_frame_send_callback(callbacks,
                                                       on_frame_send_callback);

  nghttp2_session_callbacks_set_on_frame_not_send_callback(
      callbacks, on_frame_not_send_callback);

  nghttp2_session_callbacks_set_on_header_callback2(callbacks,
                                                    on_header_callback2);

  nghttp2_session_callbacks_set_on_begin_headers_callback(
      callbacks, on_begin_headers_callback);

  nghttp2_session_callbacks_set_send_data_callback(callbacks,
                                                   send_data_callback);

  if (get_config()->padding) {
    nghttp2_session_callbacks_set_select_padding_callback(
        callbacks, http::select_padding_callback);
  }

  if (get_config()->http2.upstream.debug.frame_debug) {
    nghttp2_session_callbacks_set_error_callback(callbacks,
                                                 verbose_error_callback);
  }

  return callbacks;
}

Http2Upstream::Http2Upstream(ClientHandler *handler)
    : wb_(handler->get_worker()->get_mcpool()),
      downstream_queue_(
          get_config()->http2_proxy
              ? get_config()->conn.downstream.connections_per_host
              : get_config()->conn.downstream.connections_per_frontend,
          !get_config()->http2_proxy),
      handler_(handler),
      session_(nullptr),
      shutdown_handled_(false) {

  int rv;

  auto &http2conf = get_config()->http2;

  rv = nghttp2_session_server_new2(&session_, http2conf.upstream.callbacks,
                                   this, http2conf.upstream.option);

  std::cout << "Creating Http2Upstream" << std::endl;
  assert(rv == 0);

  flow_control_ = true;

  // TODO Maybe call from outside?
  std::array<nghttp2_settings_entry, 2> entry;
  entry[0].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
  entry[0].value = http2conf.upstream.max_concurrent_streams;

  entry[1].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
  entry[1].value = (1 << http2conf.upstream.window_bits) - 1;

  rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, entry.data(),
                               entry.size());
  if (rv != 0) {
    ULOG(ERROR, this) << "nghttp2_submit_settings() returned error: "
                      << nghttp2_strerror(rv);
  }

  if (http2conf.upstream.connection_window_bits > 16) {
    int32_t delta = (1 << http2conf.upstream.connection_window_bits) - 1 -
                    NGHTTP2_INITIAL_CONNECTION_WINDOW_SIZE;
    rv = nghttp2_submit_window_update(session_, NGHTTP2_FLAG_NONE, 0, delta);

    if (rv != 0) {
      ULOG(ERROR, this) << "nghttp2_submit_window_update() returned error: "
                        << nghttp2_strerror(rv);
    }
  }

  // We wait for SETTINGS ACK at least 10 seconds.
  ev_timer_init(&settings_timer_, settings_timeout_cb, 10., 0.);

  settings_timer_.data = this;

  // timer for 2nd GOAWAY.  HTTP/2 spec recommend 1 RTT.  We wait for
  // 2 seconds.
  ev_timer_init(&shutdown_timer_, shutdown_timeout_cb, 2., 0);
  shutdown_timer_.data = this;

  ev_prepare_init(&prep_, prepare_cb);
  prep_.data = this;
  ev_prepare_start(handler_->get_loop(), &prep_);

  handler_->reset_upstream_read_timeout(
      get_config()->conn.upstream.timeout.http2_read);

  handler_->signal_write();

  // ADDITIONAL
  dep_reader_.set_on_new_dependency_callback(
      std::bind(&Http2Upstream::on_new_dependency_callback, 
                this,
                std::placeholders::_1,
                std::placeholders::_2));
  // END ADDITIONAL
}

Http2Upstream::~Http2Upstream() {
  nghttp2_session_del(session_);
  ev_prepare_stop(handler_->get_loop(), &prep_);
  ev_timer_stop(handler_->get_loop(), &shutdown_timer_);
  ev_timer_stop(handler_->get_loop(), &settings_timer_);
}

int Http2Upstream::on_read() {
  ssize_t rv = 0;
  auto rb = handler_->get_rb();
  auto rlimit = handler_->get_rlimit();

  if (rb->rleft()) {
    rv = nghttp2_session_mem_recv(session_, rb->pos, rb->rleft());
    if (rv < 0) {
      if (rv != NGHTTP2_ERR_BAD_CLIENT_MAGIC) {
        ULOG(ERROR, this) << "nghttp2_session_recv() returned error: "
                          << nghttp2_strerror(rv);
      }
      return -1;
    }

    // nghttp2_session_mem_recv should consume all input bytes on
    // success.
    assert(static_cast<size_t>(rv) == rb->rleft());
    rb->reset();
    rlimit->startw();
  }

  if (nghttp2_session_want_read(session_) == 0 &&
      nghttp2_session_want_write(session_) == 0 && wb_.rleft() == 0) {
    if (LOG_ENABLED(INFO)) {
      ULOG(INFO, this) << "No more read/write for this HTTP2 session";
    }
    return -1;
  }

  handler_->signal_write();
  return 0;
}

// After this function call, downstream may be deleted.
int Http2Upstream::on_write() {
  for (;;) {
    if (wb_.rleft() >= MAX_BUFFER_SIZE) {
      return 0;
    }

    const uint8_t *data;
    auto datalen = nghttp2_session_mem_send(session_, &data);

    if (datalen < 0) {
      ULOG(ERROR, this) << "nghttp2_session_mem_send() returned error: "
                        << nghttp2_strerror(datalen);
      return -1;
    }
    if (datalen == 0) {
      break;
    }
    wb_.append(data, datalen);
  }

  if (nghttp2_session_want_read(session_) == 0 &&
      nghttp2_session_want_write(session_) == 0 && wb_.rleft() == 0) {
    if (LOG_ENABLED(INFO)) {
      ULOG(INFO, this) << "No more read/write for this HTTP2 session";
    }
    return -1;
  }

  return 0;
}

ClientHandler *Http2Upstream::get_client_handler() const { return handler_; }

int Http2Upstream::downstream_read(DownstreamConnection *dconn) {
  auto downstream = dconn->get_downstream();

  if (downstream->get_response_state() == Downstream::MSG_RESET) {
    // The downstream stream was reset (canceled). In this case,
    // RST_STREAM to the upstream and delete downstream connection
    // here. Deleting downstream will be taken place at
    // on_stream_close_callback.
    rst_stream(downstream,
               infer_upstream_rst_stream_error_code(
                   downstream->get_response_rst_stream_error_code()));
    downstream->pop_downstream_connection();
    // dconn was deleted
    dconn = nullptr;
  } else if (downstream->get_response_state() == Downstream::MSG_BAD_HEADER) {
    if (error_reply(downstream, 502) != 0) {
      return -1;
    }
    downstream->pop_downstream_connection();
    // dconn was deleted
    dconn = nullptr;
  } else {
    auto rv = downstream->on_read();
    if (rv == SHRPX_ERR_EOF) {
      return downstream_eof(dconn);
    }
    if (rv == SHRPX_ERR_DCONN_CANCELED) {
      downstream->pop_downstream_connection();
      handler_->signal_write();
      return 0;
    }
    if (rv != 0) {
      if (rv != SHRPX_ERR_NETWORK) {
        if (LOG_ENABLED(INFO)) {
          DCLOG(INFO, dconn) << "HTTP parser failure";
        }
      }
      return downstream_error(dconn, Downstream::EVENT_ERROR);
    }

    if (downstream->can_detach_downstream_connection()) {
      // Keep-alive
      downstream->detach_downstream_connection();
    }
  }

  handler_->signal_write();

  // At this point, downstream may be deleted.

  return 0;
}

int Http2Upstream::downstream_write(DownstreamConnection *dconn) {
  int rv;
  rv = dconn->on_write();
  if (rv == SHRPX_ERR_NETWORK) {
    return downstream_error(dconn, Downstream::EVENT_ERROR);
  }
  if (rv != 0) {
    return -1;
  }
  return 0;
}

int Http2Upstream::downstream_eof(DownstreamConnection *dconn) {
  auto downstream = dconn->get_downstream();

  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, dconn) << "EOF. stream_id=" << downstream->get_stream_id();
  }

  // Delete downstream connection. If we don't delete it here, it will
  // be pooled in on_stream_close_callback.
  downstream->pop_downstream_connection();
  // dconn was deleted
  dconn = nullptr;
  // downstream wil be deleted in on_stream_close_callback.
  if (downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
    // Server may indicate the end of the request by EOF
    if (LOG_ENABLED(INFO)) {
      ULOG(INFO, this) << "Downstream body was ended by EOF";
    }
    downstream->set_response_state(Downstream::MSG_COMPLETE);

    // For tunneled connection, MSG_COMPLETE signals
    // downstream_data_read_callback to send RST_STREAM after pending
    // response body is sent. This is needed to ensure that RST_STREAM
    // is sent after all pending data are sent.
    on_downstream_body_complete(downstream);
  } else if (downstream->get_response_state() != Downstream::MSG_COMPLETE) {
    // If stream was not closed, then we set MSG_COMPLETE and let
    // on_stream_close_callback delete downstream.
    if (error_reply(downstream, 502) != 0) {
      return -1;
    }
  }
  handler_->signal_write();
  // At this point, downstream may be deleted.
  return 0;
}

int Http2Upstream::downstream_error(DownstreamConnection *dconn, int events) {
  auto downstream = dconn->get_downstream();

  if (LOG_ENABLED(INFO)) {
    if (events & Downstream::EVENT_ERROR) {
      DCLOG(INFO, dconn) << "Downstream network/general error";
    } else {
      DCLOG(INFO, dconn) << "Timeout";
    }
    if (downstream->get_upgraded()) {
      DCLOG(INFO, dconn) << "Note: this is tunnel connection";
    }
  }

  // Delete downstream connection. If we don't delete it here, it will
  // be pooled in on_stream_close_callback.
  downstream->pop_downstream_connection();
  // dconn was deleted
  dconn = nullptr;

  if (downstream->get_response_state() == Downstream::MSG_COMPLETE) {
    // For SSL tunneling, we issue RST_STREAM. For other types of
    // stream, we don't have to do anything since response was
    // complete.
    if (downstream->get_upgraded()) {
      rst_stream(downstream, NGHTTP2_NO_ERROR);
    }
  } else {
    if (downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
      if (downstream->get_upgraded()) {
        on_downstream_body_complete(downstream);
      } else {
        rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
      }
    } else {
      unsigned int status;
      if (events & Downstream::EVENT_TIMEOUT) {
        status = 504;
      } else {
        status = 502;
      }
      if (error_reply(downstream, status) != 0) {
        return -1;
      }
    }
    downstream->set_response_state(Downstream::MSG_COMPLETE);
  }
  handler_->signal_write();
  // At this point, downstream may be deleted.
  return 0;
}


int Http2Upstream::rst_stream(Downstream *downstream, uint32_t error_code) {
  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, this) << "RST_STREAM stream_id=" << downstream->get_stream_id()
                     << " with error_code=" << error_code;
  }
  int rv;
  rv = nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE,
                                 downstream->get_stream_id(), error_code);
  if (rv < NGHTTP2_ERR_FATAL) {
    ULOG(FATAL, this) << "nghttp2_submit_rst_stream() failed: "
                      << nghttp2_strerror(rv);
    DIE();
  }
  return 0;
}

int Http2Upstream::terminate_session(uint32_t error_code) {
  int rv;
  rv = nghttp2_session_terminate_session(session_, error_code);
  if (rv != 0) {
    return -1;
  }
  return 0;
}

namespace {
ssize_t downstream_data_read_callback(nghttp2_session *session,
                                      int32_t stream_id, uint8_t *buf,
                                      size_t length, uint32_t *data_flags,
                                      nghttp2_data_source *source,
                                      void *user_data) {
  int rv;
  auto downstream = static_cast<Downstream *>(source->ptr);
  auto body = downstream->get_response_buf();
  assert(body);

  const auto &resp = downstream->response();

  auto nread = std::min(body->rleft(), length);
  auto body_empty = body->rleft() == nread;

  *data_flags |= NGHTTP2_DATA_FLAG_NO_COPY;

  if (body_empty &&
      downstream->get_response_state() == Downstream::MSG_COMPLETE) {

    *data_flags |= NGHTTP2_DATA_FLAG_EOF;

    if (!downstream->get_upgraded()) {
      const auto &trailers = resp.fs.trailers();
      if (!trailers.empty()) {
        std::vector<nghttp2_nv> nva;
        nva.reserve(trailers.size());
        http2::copy_headers_to_nva_nocopy(nva, trailers);
        if (!nva.empty()) {
          rv = nghttp2_submit_trailer(session, stream_id, nva.data(),
                                      nva.size());
          if (rv != 0) {
            if (nghttp2_is_fatal(rv)) {
              return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
          } else {
            *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
          }
        }
      }
    }
  }

  if (nread == 0 && ((*data_flags) & NGHTTP2_DATA_FLAG_EOF) == 0) {
    return NGHTTP2_ERR_DEFERRED;
  }

  return nread;
}
} // namespace

int Http2Upstream::send_reply(Downstream *downstream, const uint8_t *body,
                              size_t bodylen) {
  int rv;

  nghttp2_data_provider data_prd, *data_prd_ptr = nullptr;

  if (bodylen) {
    data_prd.source.ptr = downstream;
    data_prd.read_callback = downstream_data_read_callback;
    data_prd_ptr = &data_prd;
  }

  const auto &resp = downstream->response();
  auto &httpconf = get_config()->http;

  auto &balloc = downstream->get_block_allocator();

  const auto &headers = resp.fs.headers();
  auto nva = std::vector<nghttp2_nv>();
  // 2 for :status and server
  nva.reserve(2 + headers.size() + httpconf.add_response_headers.size());

  auto response_status = http2::stringify_status(balloc, resp.http_status);

  nva.push_back(http2::make_nv_ls_nocopy(":status", response_status));

  for (auto &kv : headers) {
    if (kv.name.empty() || kv.name[0] == ':') {
      continue;
    }
    switch (kv.token) {
    case http2::HD_CONNECTION:
    case http2::HD_KEEP_ALIVE:
    case http2::HD_PROXY_CONNECTION:
    case http2::HD_TE:
    case http2::HD_TRANSFER_ENCODING:
    case http2::HD_UPGRADE:
      continue;
    }
    nva.push_back(http2::make_nv_nocopy(kv.name, kv.value, kv.no_index));
  }

  if (!resp.fs.header(http2::HD_SERVER)) {
    nva.push_back(
        http2::make_nv_ls_nocopy("server", get_config()->http.server_name));
  }

  for (auto &p : httpconf.add_response_headers) {
    nva.push_back(http2::make_nv_nocopy(p.name, p.value));
  }

  rv = nghttp2_submit_response(session_, downstream->get_stream_id(),
                               nva.data(), nva.size(), data_prd_ptr);
  if (nghttp2_is_fatal(rv)) {
    ULOG(FATAL, this) << "nghttp2_submit_response() failed: "
                      << nghttp2_strerror(rv);
    return -1;
  }

  auto buf = downstream->get_response_buf();

  buf->append(body, bodylen);

  downstream->set_response_state(Downstream::MSG_COMPLETE);

  return 0;
}

int Http2Upstream::error_reply(Downstream *downstream,
                               unsigned int status_code) {
  int rv;
  auto &resp = downstream->response();

  auto &balloc = downstream->get_block_allocator();

  auto html = http::create_error_html(balloc, status_code);
  resp.http_status = status_code;
  auto body = downstream->get_response_buf();
  body->append(html);
  downstream->set_response_state(Downstream::MSG_COMPLETE);

  nghttp2_data_provider data_prd;
  data_prd.source.ptr = downstream;
  data_prd.read_callback = downstream_data_read_callback;

  auto lgconf = log_config();
  lgconf->update_tstamp(std::chrono::system_clock::now());

  auto response_status = http2::stringify_status(balloc, status_code);
  auto content_length = util::make_string_ref_uint(balloc, html.size());
  auto date = make_string_ref(balloc, StringRef{lgconf->time_http_str});

  auto nva = std::array<nghttp2_nv, 5>{
      {http2::make_nv_ls_nocopy(":status", response_status),
       http2::make_nv_ll("content-type", "text/html; charset=UTF-8"),
       http2::make_nv_ls_nocopy("server", get_config()->http.server_name),
       http2::make_nv_ls_nocopy("content-length", content_length),
       http2::make_nv_ls_nocopy("date", date)}};

  rv = nghttp2_submit_response(session_, downstream->get_stream_id(),
                               nva.data(), nva.size(), &data_prd);
  if (rv < NGHTTP2_ERR_FATAL) {
    ULOG(FATAL, this) << "nghttp2_submit_response() failed: "
                      << nghttp2_strerror(rv);
    return -1;
  }

  return 0;
}

void Http2Upstream::add_pending_downstream(
    std::unique_ptr<Downstream> downstream) {
  downstream_queue_.add_pending(std::move(downstream));
}

void Http2Upstream::remove_downstream(Downstream *downstream) {
  if (downstream->accesslog_ready()) {
    handler_->write_accesslog(downstream);
  }

  nghttp2_session_set_stream_user_data(session_, downstream->get_stream_id(),
                                       nullptr);

  auto next_downstream = downstream_queue_.remove_and_get_blocked(downstream);

  if (next_downstream) {
    initiate_downstream(next_downstream);
  }
}

// WARNING: Never call directly or indirectly nghttp2_session_send or
// nghttp2_session_recv. These calls may delete downstream.
int Http2Upstream::on_downstream_header_complete(Downstream *downstream) {
  int rv;

  const auto &req = downstream->request();
  auto &resp = downstream->response();

  auto &balloc = downstream->get_block_allocator();

  if (LOG_ENABLED(INFO)) {
    if (downstream->get_non_final_response()) {
      DLOG(INFO, downstream) << "HTTP non-final response header";
    } else {
      DLOG(INFO, downstream) << "HTTP response header completed";
    }
  }

  auto &httpconf = get_config()->http;

  if (!get_config()->http2_proxy && !httpconf.no_location_rewrite) {
    downstream->rewrite_location_response_header(req.scheme);
  }

#ifdef HAVE_MRUBY
  if (!downstream->get_non_final_response()) {
    auto worker = handler_->get_worker();
    auto mruby_ctx = worker->get_mruby_context();

    if (mruby_ctx->run_on_response_proc(downstream) != 0) {
      if (error_reply(downstream, 500) != 0) {
        return -1;
      }
      // Returning -1 will signal deletion of dconn.
      return -1;
    }

    if (downstream->get_response_state() == Downstream::MSG_COMPLETE) {
      return -1;
    }
  }
#endif // HAVE_MRUBY

  auto &http2conf = get_config()->http2;

  // We need some conditions that must be fulfilled to initiate server
  // push.
  //
  // * Server push is disabled for http2 proxy or client proxy, since
  //   incoming headers are mixed origins.  We don't know how to
  //   reliably determine the authority yet.
  //
  // * We need non-final response or 200 response code for associated
  //   resource.  This is too restrictive, we will review this later.
  //
  // * We requires GET or POST for associated resource.  Probably we
  //   don't want to push for HEAD request.  Not sure other methods
  //   are also eligible for push.
  if (!http2conf.no_server_push &&
      nghttp2_session_get_remote_settings(session_,
                                          NGHTTP2_SETTINGS_ENABLE_PUSH) == 1 &&
      !get_config()->http2_proxy && (downstream->get_stream_id() % 2) &&
      resp.fs.header(http2::HD_LINK) &&
      (downstream->get_non_final_response() || resp.http_status == 200) &&
      (req.method == HTTP_GET || req.method == HTTP_POST)) {

    if (prepare_push_promise(downstream) != 0) {
      // Continue to send response even if push was failed.
    }
  }

  auto nva = std::vector<nghttp2_nv>();
  // 4 means :status and possible server, via and x-http2-push header
  // field.
  nva.reserve(resp.fs.headers().size() + 4 +
              httpconf.add_response_headers.size());

  auto response_status = http2::stringify_status(balloc, resp.http_status);

  nva.push_back(http2::make_nv_ls_nocopy(":status", response_status));

  if (downstream->get_non_final_response()) {
    http2::copy_headers_to_nva(nva, resp.fs.headers());

    if (LOG_ENABLED(INFO)) {
      log_response_headers(downstream, nva);
    }

    rv = nghttp2_submit_headers(session_, NGHTTP2_FLAG_NONE,
                                downstream->get_stream_id(), nullptr,
                                nva.data(), nva.size(), nullptr);

    resp.fs.clear_headers();

    if (rv != 0) {
      ULOG(FATAL, this) << "nghttp2_submit_headers() failed";
      return -1;
    }

    return 0;
  }

  if (downstream->get_assoc_stream_id() != -1) {
    rv = adjust_pushed_stream_priority(downstream);
    if (rv != 0) {
      return -1;
    }
  }

  http2::copy_headers_to_nva_nocopy(nva, resp.fs.headers());

  if (!get_config()->http2_proxy) {
    nva.push_back(http2::make_nv_ls_nocopy("server", httpconf.server_name));
  } else {
    auto server = resp.fs.header(http2::HD_SERVER);
    if (server) {
      nva.push_back(http2::make_nv_ls_nocopy("server", (*server).value));
    }
  }

  auto via = resp.fs.header(http2::HD_VIA);
  if (httpconf.no_via) {
    if (via) {
      nva.push_back(http2::make_nv_ls_nocopy("via", (*via).value));
    }
  } else {
    // we don't create more than 16 bytes in
    // http::create_via_header_value.
    size_t len = 16;
    if (via) {
      len += via->value.size() + 2;
    }

    auto iov = make_byte_ref(balloc, len + 1);
    auto p = iov.base;
    if (via) {
      p = std::copy(std::begin(via->value), std::end(via->value), p);
      p = util::copy_lit(p, ", ");
    }
    p = http::create_via_header_value(p, resp.http_major, resp.http_minor);
    *p = '\0';

    nva.push_back(http2::make_nv_ls_nocopy("via", StringRef{iov.base, p}));
  }

  for (auto &p : httpconf.add_response_headers) {
    nva.push_back(http2::make_nv_nocopy(p.name, p.value));
  }

  if (downstream->get_stream_id() % 2 == 0) {
    // This header field is basically for human on client side to
    // figure out that the resource is pushed.
    nva.push_back(http2::make_nv_ll("x-http2-push", "1"));
  }

  if (LOG_ENABLED(INFO)) {
    log_response_headers(downstream, nva);
  }

  if (http2conf.upstream.debug.dump.response_header) {
    http2::dump_nv(http2conf.upstream.debug.dump.response_header, nva.data(),
                   nva.size());
  }

  nghttp2_data_provider data_prd;
  data_prd.source.ptr = downstream;
  data_prd.read_callback = downstream_data_read_callback;

  nghttp2_data_provider *data_prdptr;

  if (downstream->expect_response_body()) {
    data_prdptr = &data_prd;
  } else {
    data_prdptr = nullptr;
  }

  auto stream_id = downstream->get_stream_id();
  auto url = construct_url(req);
  if (nghttp2_session_should_resolve_dependency_for_stream(session_, stream_id) &&
      dep_reader_.still_have_dependencies(url)) {
    nghttp2_data_provider dependency_data_provider = 
      dep_reader_.GetDependenciesDataProvider(construct_url(req));
    nghttp2_data_provider *dependencies_prdptr = &dependency_data_provider;

    rv = nghttp2_submit_response_with_dependencies(
                                 session_, downstream->get_stream_id(),
                                 nva.data(), nva.size(), data_prdptr,
                                 dependencies_prdptr);
  } else {
    rv = nghttp2_submit_response(session_, downstream->get_stream_id(),
                                 nva.data(), nva.size(), data_prdptr);

  }
  if (rv != 0) {
    ULOG(FATAL, this) << "nghttp2_submit_response() failed";
    return -1;
  }

  // ADDITIONAL
  // std::cout << "[Http2Upstream.cc] submitted response for " << req.path << " with status: " << resp.http_status << " on stream: " << stream_id << " requesting deps: " << nghttp2_session_should_resolve_dependency_for_stream(session_, stream_id) << std::endl;
  // if (nghttp2_session_should_resolve_dependency_for_stream(session_, stream_id)) {
  //   if (!dep_reader_.StartReturningDependencies(construct_url(req))) {
  //     // Set the stream state to be finished with dependencies.
  //     on_all_dependencies_discovered_callback(construct_url(req), stream_id);
  //   }
  // }
  // END ADDITIONAL

  return 0;
}

// WARNING: Never call directly or indirectly nghttp2_session_send or
// nghttp2_session_recv. These calls may delete downstream.
int Http2Upstream::on_downstream_body(Downstream *downstream,
                                      const uint8_t *data, size_t len,
                                      bool flush) {
  auto body = downstream->get_response_buf();
  body->append(data, len);

  if (flush) {
    nghttp2_session_resume_data(session_, downstream->get_stream_id());

    downstream->ensure_upstream_wtimer();
  }

  return 0;
}

int Http2Upstream::adjust_pushed_stream_priority(Downstream *downstream) {
  int rv;

  // We only change pushed stream.  The pushed stream has
  // assoc_stream_id which is not -1.
  auto assoc_stream_id = downstream->get_assoc_stream_id();
  auto stream_id = downstream->get_stream_id();

  auto assoc_stream = nghttp2_session_find_stream(session_, assoc_stream_id);
  auto stream = nghttp2_session_find_stream(session_, stream_id);

  // By default, downstream depends on assoc_stream.  If its
  // relationship is changed, then we don't change priority.
  if (!assoc_stream || assoc_stream != nghttp2_stream_get_parent(stream)) {
    return 0;
  }

  // We are going to make stream depend on dep_stream which is the
  // parent stream of assoc_stream, if the content-type of stream
  // indicates javascript or css.
  auto dep_stream = nghttp2_stream_get_parent(assoc_stream);
  if (!dep_stream) {
    return 0;
  }

  const auto &resp = downstream->response();
  auto ct = resp.fs.header(http2::HD_CONTENT_TYPE);
  if (!ct) {
    return 0;
  }

  if (!util::istarts_with_l(ct->value, "application/javascript") &&
      !util::istarts_with_l(ct->value, "text/css") &&
      // for polymer...
      !util::istarts_with_l(ct->value, "text/html")) {
    return 0;
  }

  auto dep_stream_id = nghttp2_stream_get_stream_id(dep_stream);
  auto weight = nghttp2_stream_get_weight(assoc_stream);

  nghttp2_priority_spec pri_spec;
  nghttp2_priority_spec_init(&pri_spec, dep_stream_id, weight, 0);

  rv = nghttp2_session_change_stream_priority(session_, stream_id, &pri_spec);
  if (nghttp2_is_fatal(rv)) {
    ULOG(FATAL, this) << "nghttp2_session_change_stream_priority() failed: "
                      << nghttp2_strerror(rv);
    return -1;
  }

  if (rv == 0) {
    if (LOG_ENABLED(INFO)) {
      ULOG(INFO, this) << "Changed pushed stream priority: pushed stream("
                       << stream_id << ") now depends on stream("
                       << dep_stream_id << ") with weight " << weight;
    }
  }

  return 0;
}

// WARNING: Never call directly or indirectly nghttp2_session_send or
// nghttp2_session_recv. These calls may delete downstream.
int Http2Upstream::on_downstream_body_complete(Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DLOG(INFO, downstream) << "HTTP response completed";
  }

  auto &resp = downstream->response();

  if (!downstream->validate_response_recv_body_length()) {
    rst_stream(downstream, NGHTTP2_PROTOCOL_ERROR);
    resp.connection_close = true;
    return 0;
  }

  nghttp2_session_resume_data(session_, downstream->get_stream_id());
  downstream->ensure_upstream_wtimer();

  return 0;
}

bool Http2Upstream::get_flow_control() const { return flow_control_; }

void Http2Upstream::pause_read(IOCtrlReason reason) {}

int Http2Upstream::resume_read(IOCtrlReason reason, Downstream *downstream,
                               size_t consumed) {
  if (get_flow_control()) {
    if (consume(downstream->get_stream_id(), consumed) != 0) {
      return -1;
    }

    auto &req = downstream->request();

    req.consume(consumed);
  }

  handler_->signal_write();
  return 0;
}

int Http2Upstream::on_downstream_abort_request(Downstream *downstream,
                                               unsigned int status_code) {
  int rv;

  rv = error_reply(downstream, status_code);

  if (rv != 0) {
    return -1;
  }

  handler_->signal_write();
  return 0;
}

int Http2Upstream::consume(int32_t stream_id, size_t len) {
  int rv;

  rv = nghttp2_session_consume(session_, stream_id, len);

  if (rv != 0) {
    ULOG(WARN, this) << "nghttp2_session_consume() returned error: "
                     << nghttp2_strerror(rv);
    return -1;
  }

  return 0;
}

void Http2Upstream::log_response_headers(
    Downstream *downstream, const std::vector<nghttp2_nv> &nva) const {
  std::stringstream ss;
  for (auto &nv : nva) {
    ss << TTY_HTTP_HD << StringRef{nv.name, nv.namelen} << TTY_RST << ": "
       << StringRef{nv.value, nv.valuelen} << "\n";
  }
  ULOG(INFO, this) << "HTTP response headers. stream_id="
                   << downstream->get_stream_id() << "\n" << ss.str();
}

int Http2Upstream::on_timeout(Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, this) << "Stream timeout stream_id="
                     << downstream->get_stream_id();
  }

  rst_stream(downstream, NGHTTP2_NO_ERROR);

  return 0;
}

void Http2Upstream::on_handler_delete() {
  for (auto d = downstream_queue_.get_downstreams(); d; d = d->dlnext) {
    if (d->get_dispatch_state() == Downstream::DISPATCH_ACTIVE &&
        d->accesslog_ready()) {
      handler_->write_accesslog(d);
    }
  }
}

int Http2Upstream::on_downstream_reset(bool no_retry) {
  int rv;

  for (auto downstream = downstream_queue_.get_downstreams(); downstream;
       downstream = downstream->dlnext) {
    if (downstream->get_dispatch_state() != Downstream::DISPATCH_ACTIVE) {
      // This is error condition when we failed push_request_headers()
      // in initiate_downstream().  Otherwise, we have
      // Downstream::DISPATCH_ACTIVE state, or we did not set
      // DownstreamConnection.
      downstream->pop_downstream_connection();
      continue;
    }

    if (!downstream->request_submission_ready()) {
      // pushed stream is handled here
      rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
      downstream->pop_downstream_connection();
      continue;
    }

    downstream->pop_downstream_connection();

    downstream->add_retry();

    std::unique_ptr<DownstreamConnection> dconn;

    if (no_retry || downstream->no_more_retry()) {
      goto fail;
    }

    // downstream connection is clean; we can retry with new
    // downstream connection.

    dconn = handler_->get_downstream_connection(downstream);
    if (!dconn) {
      goto fail;
    }

    rv = downstream->attach_downstream_connection(std::move(dconn));
    if (rv != 0) {
      goto fail;
    }

    continue;

  fail:
    if (on_downstream_abort_request(downstream, 503) != 0) {
      return -1;
    }
    downstream->pop_downstream_connection();
  }

  handler_->signal_write();

  return 0;
}

int Http2Upstream::prepare_push_promise(Downstream *downstream) {
  int rv;

  const auto &req = downstream->request();
  const auto &resp = downstream->response();

  auto base = http2::get_pure_path_component(req.path);
  if (base.empty()) {
    return 0;
  }

  auto &balloc = downstream->get_block_allocator();

  for (auto &kv : resp.fs.headers()) {
    if (kv.token != http2::HD_LINK) {
      continue;
    }
    for (auto &link : http2::parse_link_header(kv.value)) {
      StringRef scheme, authority, path;

      rv = http2::construct_push_component(balloc, scheme, authority, path,
                                           base, link.uri);
      if (rv != 0) {
        continue;
      }

      if (scheme.empty()) {
        scheme = req.scheme;
      }

      if (authority.empty()) {
        authority = req.authority;
      }

      rv = submit_push_promise(scheme, authority, path, downstream);
      if (rv != 0) {
        return -1;
      }
    }
  }
  return 0;
}

int Http2Upstream::submit_push_promise(const StringRef &scheme,
                                       const StringRef &authority,
                                       const StringRef &path,
                                       Downstream *downstream) {
  const auto &req = downstream->request();

  std::vector<nghttp2_nv> nva;
  // 4 for :method, :scheme, :path and :authority
  nva.reserve(4 + req.fs.headers().size());

  // juse use "GET" for now
  nva.push_back(http2::make_nv_ll(":method", "GET"));
  nva.push_back(http2::make_nv_ls_nocopy(":scheme", scheme));
  nva.push_back(http2::make_nv_ls_nocopy(":path", path));
  nva.push_back(http2::make_nv_ls_nocopy(":authority", authority));

  for (auto &kv : req.fs.headers()) {
    switch (kv.token) {
    // TODO generate referer
    case http2::HD__AUTHORITY:
    case http2::HD__SCHEME:
    case http2::HD__METHOD:
    case http2::HD__PATH:
      continue;
    case http2::HD_ACCEPT_ENCODING:
    case http2::HD_ACCEPT_LANGUAGE:
    case http2::HD_CACHE_CONTROL:
    case http2::HD_HOST:
    case http2::HD_USER_AGENT:
      nva.push_back(http2::make_nv_nocopy(kv.name, kv.value, kv.no_index));
      break;
    }
  }

  auto promised_stream_id = nghttp2_submit_push_promise(
      session_, NGHTTP2_FLAG_NONE, downstream->get_stream_id(), nva.data(),
      nva.size(), nullptr);

  if (promised_stream_id < 0) {
    if (LOG_ENABLED(INFO)) {
      ULOG(INFO, this) << "nghttp2_submit_push_promise() failed: "
                       << nghttp2_strerror(promised_stream_id);
    }
    if (nghttp2_is_fatal(promised_stream_id)) {
      return -1;
    }
    return 0;
  }

  if (LOG_ENABLED(INFO)) {
    std::stringstream ss;
    for (auto &nv : nva) {
      ss << TTY_HTTP_HD << StringRef{nv.name, nv.namelen} << TTY_RST << ": "
         << StringRef{nv.value, nv.valuelen} << "\n";
    }
    ULOG(INFO, this) << "HTTP push request headers. promised_stream_id="
                     << promised_stream_id << "\n" << ss.str();
  }

  return 0;
}

bool Http2Upstream::push_enabled() const {
  return !(get_config()->http2.no_server_push ||
           nghttp2_session_get_remote_settings(
               session_, NGHTTP2_SETTINGS_ENABLE_PUSH) == 0 ||
           get_config()->http2_proxy);
}

int Http2Upstream::initiate_push(Downstream *downstream, const StringRef &uri) {
  int rv;

  if (uri.empty() || !push_enabled() || (downstream->get_stream_id() % 2)) {
    return 0;
  }

  const auto &req = downstream->request();

  auto base = http2::get_pure_path_component(req.path);
  if (base.empty()) {
    return -1;
  }

  auto &balloc = downstream->get_block_allocator();

  StringRef scheme, authority, path;

  rv = http2::construct_push_component(balloc, scheme, authority, path, base,
                                       uri);
  if (rv != 0) {
    return -1;
  }

  if (scheme.empty()) {
    scheme = req.scheme;
  }

  if (authority.empty()) {
    authority = req.authority;
  }

  rv = submit_push_promise(scheme, authority, path, downstream);

  if (rv != 0) {
    return -1;
  }

  return 0;
}

int Http2Upstream::response_riovec(struct iovec *iov, int iovcnt) const {
  if (iovcnt == 0 || wb_.rleft() == 0) {
    return 0;
  }

  return wb_.riovec(iov, iovcnt);
}

void Http2Upstream::response_drain(size_t n) { wb_.drain(n); }

bool Http2Upstream::response_empty() const { return wb_.rleft() == 0; }

DefaultMemchunks *Http2Upstream::get_response_buf() { return &wb_; }

Downstream *
Http2Upstream::on_downstream_push_promise(Downstream *downstream,
                                          int32_t promised_stream_id) {
  // promised_stream_id is for backend HTTP/2 session, not for
  // frontend.
  auto promised_downstream =
      make_unique<Downstream>(this, handler_->get_mcpool(), 0);
  auto &promised_req = promised_downstream->request();

  promised_downstream->set_downstream_stream_id(promised_stream_id);
  // Set associated stream in frontend
  promised_downstream->set_assoc_stream_id(downstream->get_stream_id());

  promised_downstream->disable_upstream_rtimer();

  promised_req.http_major = 2;
  promised_req.http_minor = 0;

  auto ptr = promised_downstream.get();
  add_pending_downstream(std::move(promised_downstream));
  downstream_queue_.mark_active(ptr);

  return ptr;
}

int Http2Upstream::on_downstream_push_promise_complete(
    Downstream *downstream, Downstream *promised_downstream) {
  std::vector<nghttp2_nv> nva;

  const auto &promised_req = promised_downstream->request();
  const auto &headers = promised_req.fs.headers();

  nva.reserve(headers.size());

  for (auto &kv : headers) {
    nva.push_back(http2::make_nv(kv.name, kv.value, kv.no_index));
  }

  auto promised_stream_id = nghttp2_submit_push_promise(
      session_, NGHTTP2_FLAG_NONE, downstream->get_stream_id(), nva.data(),
      nva.size(), promised_downstream);
  if (promised_stream_id < 0) {
    return -1;
  }

  promised_downstream->set_stream_id(promised_stream_id);

  return 0;
}

void Http2Upstream::cancel_premature_downstream(
    Downstream *promised_downstream) {
  if (LOG_ENABLED(INFO)) {
    ULOG(INFO, this) << "Remove premature promised stream "
                     << promised_downstream;
  }
  downstream_queue_.remove_and_get_blocked(promised_downstream, false);
}

// ADDITIONAL
/*
 * Called when at least one dependency is found.
 */
int Http2Upstream::on_dependency_received() {
  std::cout << "[shrpx_http2_upstream.cc] OnDependency" << std::endl;
  return -1;
}

void Http2Upstream::on_new_dependency_callback(const std::string url, 
                                               int32_t stream_id) {
  std::cout << "[shrpx_http2_upstream.cc] new dependency callback for url: " << url << std::endl;
  // Here is where we would like to submit dependency frames.
  while (dep_reader_.still_have_dependencies(url)) {
    std::cout << "[shrpx_http2_upstream.cc] submitting dependency with remaining: " << dep_reader_.num_dependencies_remaining(url) << std::endl;
    nghttp2_data_provider dependency_data_provider = 
      dep_reader_.GetDependenciesDataProvider(url);
    nghttp2_data_provider *dependency_data_provider_ptr = &dependency_data_provider;
    uint8_t flag = NGHTTP2_FLAG_NONE;
    if (!dep_reader_.still_have_dependencies(url)) {
      // When the last dependency is about to be sent.
      flag |= NGHTTP2_FLAG_END_STREAM;
    }
    if (nghttp2_submit_dependency(session_, flag,
        stream_id, dependency_data_provider_ptr) == 0) {
      std::cout << "submitted dependencies" << std::endl;
    } else {
      std::cout << "submitted dependencies failed" << std::endl;
    }
  }
  on_all_dependencies_discovered_callback(url, stream_id);
}

void Http2Upstream::on_all_dependencies_discovered_callback(std::string url,
                                                            int32_t stream_id) {
  std::cout << "[shrpx_http2_upstream.cc] all dependencies discovered callback with stream id: " << stream_id << std::endl;
  // Allow the stream to sent END_STREAM flag.
  nghttp2_session_set_still_have_dependencies(session_, stream_id, 0);
}

void Http2Upstream::start_resolving_dependencies(std::string url, int32_t stream_id) {
  // TODO: Remove this
  // if (did_sent_dependency_) {
  //   return;
  // }

  // Check whether the stream is expecting dependencies or not.
  if (nghttp2_session_should_resolve_dependency_for_stream(session_, stream_id)) {
    std::cout << "Start resolving dependencies for: " << stream_id << std::endl;
    dep_reader_.RegisterForGettingDependencies(url, stream_id);
  }
}

std::string Http2Upstream::construct_url(const struct Request& request) {
  return request.scheme.str() + "://" +
         request.authority.str() + 
         request.path.str();
}
// END ADDITIONAL


} // namespace shrpx
