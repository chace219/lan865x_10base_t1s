/*
 *  This file is part of the Arduino_10BASE_T1S library.
 *
 *  Copyright (c) 2024 Arduino SA
 *
 *  This Source Code Form is subject to the terms of the Mozilla Public
 *  License, v. 2.0. If a copy of the MPL was not distributed with this
 *  file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

/**************************************************************************************
 * INCLUDES
 **************************************************************************************/

#include <stdint.h>
#include <stddef.h>

#if defined(ARDUINO_ARCH_RP2040)
#include <lwip/tcp.h>
#include <lwip/pbuf.h>
#include <lwip/err.h>
#else
#include "lib/liblwip/include/lwip/tcp.h"
#include "lib/liblwip/include/lwip/pbuf.h"
#include "lib/liblwip/include/lwip/err.h"
#endif

/**************************************************************************************
 * CONSTANTS
 **************************************************************************************/

/** Maximum number of registered route handlers.  Override from the build to fit
 *  more endpoints — the consumer registers 16 (web UI + REST API + OTA), so it
 *  sets -DHTTP_SERVER_MAX_ROUTES=24.  Excess registrations are dropped with a
 *  "[HTTP] Route table full" message. */
#ifndef HTTP_SERVER_MAX_ROUTES
#define HTTP_SERVER_MAX_ROUTES      8
#endif

/** Maximum number of registered upload handlers (POST streaming). */
#define HTTP_SERVER_MAX_UPLOAD_ROUTES 4

/** Per-request tracing ("Connection accepted", "RX data bytes", "GET /path",
 *  "Peer closed").  Off by default: these run inside the lwIP recv callback and
 *  each line is a blocking USB-CDC write, which adds milliseconds of latency to
 *  the very loop that must drain the MAC-PHY RX FIFO — a direct contributor to
 *  [TC6 EVT] RX_Buffer_Overflow.  Build with -DHTTP_SERVER_DEBUG=1 to restore
 *  them.  Error/lifecycle messages (bind/listen failures, 431, route table
 *  full) are always printed. */
#ifndef HTTP_SERVER_DEBUG
#define HTTP_SERVER_DEBUG           0
#endif

#if HTTP_SERVER_DEBUG
#define HTTP_TRACE_PRINT(x)         Serial.print(x)
#define HTTP_TRACE_PRINTLN(x)       Serial.println(x)
#else
#define HTTP_TRACE_PRINT(x)         do { } while (0)
#define HTTP_TRACE_PRINTLN(x)       do { } while (0)
#endif

/** TODO: It needs to adjust buffer size as fit 
 * to the actual request size and response, microcontroller platform */

/** Receive buffer for one HTTP request (bytes).
 *  Modern browsers (Chrome/Edge) send Sec-Fetch-*, Sec-Ch-Ua-* and other
 *  headers that raise a typical POST to 600-750 bytes.  2 KB covers even
 *  the most verbose browser without wasting significant heap space (ConnState
 *  is heap-allocated, not on the stack). */
#define HTTP_SERVER_REQ_BUF_SIZE    2048

/** Response body buffer (bytes).
 *  Must be large enough for the longest handler response.  Override from the
 *  build to fit larger pages — e.g. the consumer's embedded monitor/config
 *  pages need -DHTTP_SERVER_RESP_BUF_SIZE=16384.  The buffer is static inside
 *  handleRequest, not on the stack. */
#ifndef HTTP_SERVER_RESP_BUF_SIZE
#define HTTP_SERVER_RESP_BUF_SIZE   1536
#endif

/** Max response-body bytes queued to lwIP per send cycle.  The body is paced in
 *  pieces this size, draining between each via the onSent() ACK callback, so a
 *  large page is transmitted as many small SPI chunk bursts instead of one long
 *  burst — the long burst overruns the MAC-PHY SPI link on marginal (jumper)
 *  wiring and causes [TC6 ERR] BadChecksum.  Override from the build to tune. */
#ifndef HTTP_SERVER_TX_CHUNK_MAX
#define HTTP_SERVER_TX_CHUNK_MAX    1460
#endif

/**************************************************************************************
 * CLASS DECLARATION
 **************************************************************************************/

/**
 * @class Arduino_10BASE_T1S_HTTP
 * @brief Lightweight HTTP/1.0 server built on the lwIP raw-TCP API.
 *
 * Usage
 * -----
 * 1. Instantiate (optionally pass a port, default 80).
 * 2. Register route handlers with on().
 * 3. Call begin() after the network interface is up.
 * 4. No poll() call needed — the server is driven by lwIP callbacks.
 *
 * Handler signature
 * -----------------
 * @code
 * uint16_t myHandler(const char* method,
 *                    const char* path,
 *                    const char* query,
 *                    char*       resp_body,
 *                    size_t      resp_body_len)
 * {
 *     snprintf(resp_body, resp_body_len, "<h1>Hello</h1>");
 *     return 200;   // HTTP status code
 * }
 * @endcode
 *
 * @param method      "GET" or "POST"
 * @param path        URL path, e.g. "/sensors"
 * @param query       Query string after '?', empty string if none
 * @param resp_body   Output buffer — write your HTML/JSON response here
 * @param resp_body_len Size of resp_body buffer
 * @return HTTP status code to send (200, 404, 500, …)
 *
 * Content-Type is always "text/html; charset=utf-8".
 * For JSON, start the body with a recognisable prefix and the client can
 * detect it, or extend the class to carry a content-type out-param.
 */
class Arduino_10BASE_T1S_HTTP
{
public:

  enum UploadStatus {
    UPLOAD_START = 0,
    UPLOAD_FILE_WRITE,
    UPLOAD_FILE_END,
    UPLOAD_FILE_ABORTED
  };

  struct UploadInfo {
    UploadStatus    status;
    const uint8_t*  buf;
    size_t          currentSize;
    size_t          totalSize;
    size_t          contentLength;
    const char*     path;
    const char*     query;
  };

  /**
   * @brief Handler function pointer type.
   *
   * Write the response body into resp_body (null-terminated).
   * Return the HTTP status code (200, 404, 500, …).
   */
  typedef uint16_t (*RouteHandler)(const char* method,
                                   const char* path,
                                   const char* query,
                                   char*       resp_body,
                                   size_t      resp_body_len);

  typedef void (*UploadHandler)(const UploadInfo& upload_info);

           Arduino_10BASE_T1S_HTTP(uint16_t port = 80);
  virtual ~Arduino_10BASE_T1S_HTTP();

  /**
   * @brief Start the HTTP server.
   * Call this after the 10BASE-T1S network interface is up (i.e., after
   * Arduino_10BASE_T1S_PHY_Interface::begin() returns successfully).
   * @return true  Server is listening.
   * @return false Failed to create / bind the TCP PCB.
   */
  bool begin();

  /** @brief Stop the server and release all TCP resources. */
  void stop();

  /**
   * @brief Register a URL path handler.
   *
   * @param path     URL path to match, e.g. "/", "/data", "/led"
   *                 Exact match only (no wildcards).
   * @param handler  Function called when the path is requested.
   * @return true    Handler registered.
   * @return false   Route table full (HTTP_SERVER_MAX_ROUTES).
   */
  bool on(const char* path, RouteHandler handler);

  bool onUpload(const char* path, UploadHandler handler);

  /**
   * @brief Set a catch-all handler for paths with no registered route.
   * If not set, unmatched paths receive a plain 404 response.
   */
  void setNotFoundHandler(RouteHandler handler);

  /** @brief Returns true if the server is currently listening. */
  bool isRunning() const { return _listen_pcb != nullptr; }

  /**
   * @brief Send an HTTP response with application/json Content-Type.
   * Used by REST route handlers that need to override the default text/html type.
   * After calling this, do NOT write into resp_body — instead set resp_body[0]=0
   * and return the status code so handleRequest skips the normal sendResponse().
   * This method writes directly to the TCP PCB so it must be called from within
   * a route handler invocation (where tpcb is available via the internal state).
   * NOTE: The recommended pattern for JSON handlers is simply to write the JSON
   * into resp_body and return the status code; the client-side fetch() code
   * should use Content-Type detection. A proper per-handler content-type is a
   * future enhancement.
   */

private:

  /* ------------------------------------------------------------------ */
  /* Internal types                                                       */
  /* ------------------------------------------------------------------ */

  struct Route {
    const char*  path;
    RouteHandler handler;
  };

  struct UploadRoute {
    const char*   path;
    UploadHandler handler;
  };

  /** Per-connection state allocated on the heap when a client connects. */
  struct ConnState {
    Arduino_10BASE_T1S_HTTP* server;
    char     req_buf[HTTP_SERVER_REQ_BUF_SIZE];
    uint16_t req_len;
    bool     headers_done;
    char     method[8];
    char     path[128];
    /* Holds the URL query string for GET, and for POST/PUT the URL query
     * string followed by '&' and the request body (so handlers can find
     * both the session token and the JSON payload in one strstr scan). */
    char     query[512];
    bool     close_after_send;
    bool     upload_mode;
    bool     upload_started;
    uint32_t content_length;
    uint32_t body_received;
    UploadHandler upload_handler;
    /* Outgoing-response state: bodies larger than tcp_sndbuf() are queued in
     * chunks across successive onSent() callbacks (see pumpTx). */
    const char* tx_body;       // remaining unsent body bytes (points into the static resp buffer)
    uint32_t    tx_remaining;  // bytes of tx_body still to queue
    bool        tx_half_close; // half-close TX once the whole body is queued
  };

  /* ------------------------------------------------------------------ */
  /* Data members                                                         */
  /* ------------------------------------------------------------------ */

  uint16_t     _port;
  tcp_pcb*     _listen_pcb;
  Route        _routes[HTTP_SERVER_MAX_ROUTES];
  UploadRoute  _upload_routes[HTTP_SERVER_MAX_UPLOAD_ROUTES];
  uint8_t      _route_count;
  uint8_t      _upload_route_count;
  RouteHandler _not_found_handler;

  /* ------------------------------------------------------------------ */
  /* Private helpers                                                      */
  /* ------------------------------------------------------------------ */

  RouteHandler findHandler(const char* path) const;
  UploadHandler findUploadHandler(const char* path) const;
  void         handleRequest(struct tcp_pcb* tpcb, ConnState* state);

  static void  sendResponse(struct tcp_pcb* tpcb,
                            ConnState*      state,
                            uint16_t        status_code,
                            const char*     status_text,
                            const char*     body);
  /* Queue as much of state->tx_body as tcp_sndbuf() allows; resumed by onSent. */
  static void  pumpTx(struct tcp_pcb* tpcb, ConnState* state);
  static void  closeConn(struct tcp_pcb* tpcb, ConnState* state);

  /* ------------------------------------------------------------------ */
  /* lwIP raw-TCP callbacks (must be static)                              */
  /* ------------------------------------------------------------------ */

  static err_t onAccept(void* arg, struct tcp_pcb* new_pcb, err_t err);
  static err_t onRecv  (void* arg, struct tcp_pcb* tpcb,
                        struct pbuf* p, err_t err);
  static err_t onSent  (void* arg, struct tcp_pcb* tpcb, u16_t len);
  static void  onError (void* arg, err_t err);
  static err_t onPoll  (void* arg, struct tcp_pcb* tpcb);
};
