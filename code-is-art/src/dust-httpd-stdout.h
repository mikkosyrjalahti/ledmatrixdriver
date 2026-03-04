/*
 * Copyright (c) 2024 Pihlaja Voipio
 *
 * SPDX-License-Identifier: CC0-1.0
 *
*/
#pragma once

/*
 * Example GET-handler: STDOUT
 *
 * This provides a simple stdio driver that logs stdout into a ring-buffer
 * and serves the contents of this buffer with a HTTP GET as a text document.
 *
*/

#define HTTPD_GET_STDOUT    // include GET URL

// This is used as a ring-buffer - should not make it larger than 1kB
// because we want to copy a snapshot into client buffer
static uint8_t httpd_stdout_buffer[1024];
static uint32_t httpd_stdout_index = 0;

// The output function for the stdio driver
static void httpd_stdout_chars(const char * buf, int n)
{
    for(int i = 0; i < n; ++i)
    {
        httpd_stdout_buffer[httpd_stdout_index++] = buf[i];
        if(httpd_stdout_index == sizeof(httpd_stdout_buffer))
            httpd_stdout_index = 0;
    }
}

// The stdio driver
stdio_driver_t httpd_stdio =
{
    .out_chars = httpd_stdout_chars,
    .out_flush = 0,
    .in_chars = 0,
    .set_chars_available_callback = 0,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = false
#endif
};

// init ..
void httpd_stdio_init()
{
    memset(httpd_stdout_buffer, ' ', sizeof(httpd_stdout_buffer));
    stdio_set_driver_enabled(&httpd_stdio, true);
}

// actual get-handler
static int get_stdout(httpd_client * con, bool head_only, const char * query_string)
{
    http_response_nolog(con, 200);
    http_write_string(con, "Content-Type: text/plain\r\n");
    http_write_string(con, "Refresh: 1\r\n");
    http_write_string(con, "Content-Length: ");
    http_write_uint(con, sizeof(httpd_stdout_buffer));
    http_end_headers(con);

    if(head_only) return -1;

    http_write_raw(con, httpd_stdout_buffer + httpd_stdout_index,
        sizeof(httpd_stdout_buffer) - httpd_stdout_index);
    if(httpd_stdout_index)
        http_write_raw(con, httpd_stdout_buffer, httpd_stdout_index);
    return -1;
}
