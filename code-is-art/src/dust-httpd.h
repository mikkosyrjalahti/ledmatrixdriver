/*
 * Copyright (c) 2024 Pihlaja Voipio
 *
 * SPDX-License-Identifier: CC0-1.0
 *
*/

#pragma once

#include "lwip/tcp.h"

/*

    This is small HTTPD for Pico-W, designed for AJAX control surfaces.

    Supports:
     - static files (provide ptr + len, eg. from xxd-generated headers)
     - simple GET handlers for dynamic state queries (eg. AJAX poll)
     - simple POST handlers for large uploads
     - captive portal redirects

    The server should not crash on invalid requests, but it might accept
    some that violate the HTTP specification.
    
    In the interest of code-size and speed, client headers are not parsed
    fully, but rather assumed to be well-formed as long as we can extract
    the actual fields we want.

    GET/POST handler's maximum response size with headers included is
    limited to one TCP packet (ie. a bit over 1kB plus HTTP headers).

    Multiple concurrent connections are supported, but the server does not
    buffer POST requests (where content might be larger than Pico memory),
    so care should be taken where conflicts need to be avoided.

*/

// Client state-machine enumeration
//
enum
{
    httpd_CS_begin = 0, // pre-HEAD/GET/POST
    httpd_CS_HEAD,      // got HEAD request
    httpd_CS_GET,       // got GET request, sending file
    httpd_CS_POST,      // receiving content for POST
};

// forward declare
typedef struct httpd_client_t httpd_client;

/*
 * Single function GET handler:
 *
 * This gets a pointer to a query string, or nullptr if none.
 * If head_only is true, then client sent a HEAD request.
 *
 * return 0 to response 204 Empty, HTTP status code for stock response
 * or write custom response to con->buffer and return -1
 *
*/
typedef int (*httpd_get_fn)
    (httpd_client * con, bool head_only, const char * query_string);

/* POST handlers:
 *
 *  - begin is called after parsing "Content-Length" from headers
 *  - data is called for every received chunk of data
 *      buf contains len bytes of data after offset previous bytes
 *  - end is called when the request completes
 *
 * For any of the functions:
 *
 * return 0 to keep going, HTTP status code for stock response
 * or write custom response to con->buffer and return -1
 *
 * if end() returns 0, we responsd with 204 Empty
*/
typedef struct httpd_post_handler_t
{
    int (*begin)(httpd_client *con, uint32_t content_length);
    int (*data)(httpd_client *con, uint8_t *buf, uint32_t len, uint32_t offset);
    int (*end)(httpd_client *con, uint32_t size);
} httpd_post_handler;

struct httpd_client_t
{
    // this points to matched URL (static string in tables) for logging
    const char *url;
    
    // allocate one full TCP packet
    uint8_t     buffer[TCP_MSS];
    uint32_t    header_len;     // headers + dynamic data (ie. "not file")
    uint32_t    len_pending;    // sent bytes pending to be ACKed
    
    union
    {
        struct {
            const uint8_t   *ptr;   // pointer to static file contents
            
            uint32_t    len;        // length of the file
            uint32_t    len_wrote;  // bytes written
        } file;

        struct {
            httpd_post_handler handler;
                
            uint32_t    len;        // total client "Content-Length"
            uint32_t    len_recv;   // bytes received
        } post;
    };

    uint8_t     timeout;    // counts coarse ticks for timeouts
    uint8_t     state;


};

// This installs the stdio driver for our /stdout GET handler
// This can be called before httpd_init() to start logging early
void httpd_stdio_init();

// Start the actual HTTPD ..
//
// the offsets are used to set the initial indexes
// to scan in static/get/post to allow for hiding config
//
bool httpd_init(
    uint32_t static_file_offset,
    uint32_t get_handler_offset,
    uint32_t post_handler_offset);

// These are used to write response headers.
//
// Call httpd_response with status to being.
// Call httpd_write_string/httpd_write_uint to append extra headers.
// Call httpd_end_headers to finish headers.
void http_response(httpd_client * con, int status);
void http_response_nolog(httpd_client * con, int status);
void http_write_string(httpd_client * con, const char * str);
void http_write_uint(httpd_client * con, uint32_t number);
void http_write_raw(httpd_client * con, uint8_t * data, uint32_t len);
void http_end_headers(httpd_client * con);
