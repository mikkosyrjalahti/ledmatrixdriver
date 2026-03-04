/*
 * Copyright (c) 2024 Pihlaja Voipio
 *
 * SPDX-License-Identifier: CC0-1.0
 *
*/

#include <string.h>
#include <pico/stdio.h>
#include <pico/stdio/driver.h>

#include "dust-httpd.h"

// Set 1 to include the URLs for captive portal
#define CAPTIVE_PORTAL  1

// Set 1 to output simple request logging to stdout
// Also enables reporting some important errors
#define PRINT_LOG 1

// Set 1 to output so extra debugs to stdout
// Reports connection errors and similar "normally safe to ignore" stuff
#define PRINT_DEBUG 0

// The HTTP port
#define HTTPD_PORT  80

// this is in "poll ticks" which is like half a second
// should be at least 2 or clients will timeout randomly
#define HTTPD_TIMEOUT   20

// Example GET/POST handlers
#include "dust-httpd-stdout.h"  // GET example
#include "dust-httpd-setimg.h"  // POST example

#include "dust-httpd-config.h"

// different content types
#define content_text    "text/plain"
#define content_html    "text/html; charset=utf-8"
#define content_png     "image/png"
#define content_jpeg    "image/jpeg"
#define content_zip     "application/zip"

// xxd-generated "files as arrays"
#include "index.html.h"
#include "config.html.h"
#include "default.png.h"
#include "nyan.png.h"
#include "sources.zip.h"

// These are regular static files
static struct static_files_t
{
    const char      *url;
    const char      *content_type;
    
    const uint8_t   *data;
    uint32_t        len;
} static_files[] = {
    // index.html is redirected here - if we're in AP mode, serve the config page
    {   "/",            content_html,   config_html,     config_html_len  },
    // index.html is redirected here
    {   "/",            content_html,   index_html,     index_html_len  },
    // always serve the normal index.html as panel.html
    {   "/panel.html",  content_html,   index_html,     index_html_len  },
    {   "/default.png", content_png,    default_png,    default_png_len },
    {   "/other.png",   content_png,    nyan_png,       nyan_png_len },
    {   "/sources.zip", content_zip,    sources_zip,    sources_zip_len },
    
#if CAPTIVE_PORTAL
    // captive portal stuff
    {   "/connectivity-check.html", content_html, index_html, index_html_len },
    {   "/ncsi.txt",                content_text, (uint8_t*)"OK",   2 },
    {   "/connecttest.txt",         content_text, (uint8_t*)"OK",   2 },
#endif
};
static const unsigned n_static_files
    = sizeof(static_files) / sizeof(struct static_files_t);
    
// this is set on HTTPD init
static uint32_t static_file_offset = 0;


// GET handler callbacks
static struct get_handlers_t
{
    const char      *url;
    httpd_get_fn    handler;
} get_handlers[] = {
#ifdef HTTPD_GET_STDOUT
    {   "/stdout",  get_stdout  },
#endif
    {   "/magic",   get_magic   },
    {   "/spoon",   get_spoon   },
    {   "/science", get_fire    },
};
static const unsigned n_get_handlers
    = sizeof(get_handlers) / sizeof(struct get_handlers_t);

// this is set on HTTPD init
static uint32_t get_handler_offset = 0;

// POST handler callbacks
static struct post_handlers_t
{
    const char          *url;
    httpd_post_handler  handler;
} post_handlers[] = {
    {   post_wifi_url,  post_wificonfig  }, // must be first, skipped if not AP mode
#ifdef HTTPD_POST_SETIMG
    {   "/setimg",  post_fb },
#endif
};
static const unsigned n_post_handlers
    = sizeof(post_handlers) / sizeof(struct post_handlers_t);

// this is set on HTTPD init
static uint32_t post_handler_offset = 0;

// all these do 302 Redirect to root
// mostly for captive portal, but also handle 'index.html'
static const char *   _redirect[] = {
    "/index.html",
    
#if CAPTIVE_PORTAL
    "/redirect",    // We'll leave this 
    "/get_204",
    "/generate_204",
    "/hotspot-detect.html",
#endif
};
static const unsigned n_redirect = sizeof(_redirect) / sizeof(char*);

// The more spammy debugs go through DEBUG_printf
#if PRINT_DEBUG
# define DEBUG_printf    printf
#else
# define DEBUG_printf(...) do{}while(0)
#endif

// Simple logging goes through LOG_printf
#if PRINT_LOG
# define LOG_printf    printf
#else
# define LOG_printf(...) do{}while(0)
#endif

// This simply lists all the status code we support in one place
// so that GET/POST handlers an return a numeric code.
//
// This is not a full list of all possible HTTP responses
// but rather only the ones that we actually use..
//
// Continue is special cased
static const char http_continue[] = "HTTP/1.1 100 Please\r\n\r\n";
static const uint32_t http_continue_len = sizeof(http_continue)-1;

static const char * http_status_text(int status)
{
    switch(status)
    {
    case 200: return "200 OK";
    case 204: return "204 Done";            // No content
    case 302: return "302 That Way";        // Redirect
    case 303: return "303 Acid Party";      // See other (action result redirect)
    case 400: return "400 Bad Apple";       // bad request
    case 404: return "404 Porn Not Found";
    case 405: return "405 Screwhammer";     // method not allowed
    case 409: return "409 Missed Train";    // conflict
    case 410: return "410 Only Sporks Left";// gone
    case 411: return "411 Size Matters";    // length required
    case 413: return "413 Stack Overflow";  // content too large
    case 414: return "414 TLDR";            // URI too long
    case 431: return "431 Too Complicated"; // Request Header Fields Too Large
    case 418: return "418 There Is No spoon";   // I'm a teapot (yes!)
    case 451: return "451 Forbidden By Physics";    // unavailable for legal reasons
    
    default: return "500 Kernel Panic";    // Internal error
    }
}

// Append string (don't want to use sprintf)
void http_write_string(httpd_client * con, const char * str)
{
    while(*str) con->buffer[con->header_len++] = *str++;
}

// Append unsigned integer (don't want to use sprintf)
void http_write_uint(httpd_client * con, uint32_t x)
{
    uint32_t off = con->header_len;

    if(!x) { con->buffer[con->header_len++] = '0'; return; }

    // write backwards
    while(x) { con->buffer[off++] = '0' + (x % 10); x /= 10; }

    // then reverse in place
    for(int i = con->header_len, j = off - 1; i < j; ++i, --j)
    {
        uint8_t t = con->buffer[i];
        con->buffer[i] = con->buffer[j];
        con->buffer[j] = t;
    }
    
    con->header_len = off;
}

void http_write_raw(httpd_client * con, uint8_t * data, uint32_t len)
{
    for(int i = 0; i < len; ++i) con->buffer[con->header_len++] = data[i];
}


// This beings a HTTP response
void http_response(httpd_client * con, int status)
{
    LOG_printf("HTTP %d %s\n", status, con->url);
    http_response_nolog(con, status);
}
// version that doesn't log the request (eg. for /stdout)
void http_response_nolog(httpd_client * con, int status)
{
    con->header_len = 0;
    http_write_string(con, "HTTP/1.1 ");
    http_write_string(con, http_status_text(status));
    http_write_string(con, "\r\n");
}

// This ends headers
void http_end_headers(httpd_client * con)
{
    http_write_string(con, "Connection: close\r\n\r\n");
}


static err_t httpd_close_client(
    httpd_client *con, struct tcp_pcb * pcb, err_t close_err)
{
    if(pcb)
    {
        tcp_arg(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        tcp_sent(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        err_t err = tcp_close(pcb);
        if (err != ERR_OK) {
            DEBUG_printf("httpd: close failed %d, calling abort\n", err);
            tcp_abort(pcb);
            close_err = ERR_ABRT;
        }
    }
    if (con) {
        free(con);
    }
    return close_err;
}

static err_t httpd_send_data(httpd_client * con, struct tcp_pcb * pcb)
{
    // figure out how much we still want to send
    int nsend = con->file.len - con->file.len_wrote;

    // get the amount of space TCP can accept
    int maxsend = tcp_sndbuf(pcb);
    if(nsend > maxsend) nsend = maxsend;

    if(!nsend) return ERR_OK;   // this is fine

    // if we are trying to send data, we should have a file :)
    assert(con->file.ptr);

    // stupid thing always ooms if we try to send full buf
    // so write one TCP_MSS at a time.. this works :)
    do
    {
        int n = nsend;
        if(n > TCP_MSS) n = TCP_MSS;
        
        int err = tcp_write(pcb, con->file.ptr + con->file.len_wrote, n, 0);
        if (err == ERR_MEM) { break; }  // this is fine, happens
        if (err != ERR_OK) {
            DEBUG_printf("httpd: send failed: %d\n", err);
            return httpd_close_client(con, pcb, err);
        }
        con->file.len_wrote += n;
        nsend -= n;
    }
    while(nsend);
    return ERR_OK;
}

static err_t httpd_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    httpd_client *con = (httpd_client*)arg;

    // sent headers, can close
    if(con->state == httpd_CS_HEAD)
    {
        return httpd_close_client(con, pcb, ERR_OK);
    }
    
    // if we're not sending files, bail out
    // anything else should be single packet
    if(con->state != httpd_CS_GET) return ERR_OK;

    con->len_pending -= len;
    if(!con->len_pending)
    {
        // we're always "Connection: close" for now ..
        return httpd_close_client(con, pcb, ERR_OK);
    }
    else
    {
        return httpd_send_data(con, pcb);
    }
}

static err_t httpd_poll(void * arg, struct tcp_pcb * pcb)
{
    httpd_client *con = (httpd_client*)arg;

    switch(con->state)
    {
    case httpd_CS_GET:
        return httpd_send_data(con, pcb);
    default:
        if(++con->timeout >= HTTPD_TIMEOUT)
        {
            DEBUG_printf("httpd: client timeout\n");
            return httpd_close_client(con, pcb, ERR_OK);
        }
    }
    return ERR_OK;
}

static void httpd_err(void *arg, err_t err) {
    httpd_client *con = (httpd_client*)arg;
    DEBUG_printf("httpd_err %d\n", err);
    httpd_close_client(con, 0, err);
}

// buffer for one full packet
static uint8_t  httpd_buffer[TCP_MSS];

// return 0 on "keep going" or HTTP status code for stock responses
// where we need to write custom headers, we return -1
static int httpd_recv_headers(httpd_client *con, struct tcp_pcb *pcb)
{
    // do we have a full request
    uint8_t *end_of_req = (uint8_t*) strnstr(
        (char*) con->buffer, "\r\n\r\n", con->header_len);

    if(end_of_req)
    {
        // put a null-termination at the second CRLF
        // this way GET/POST handlers can rely on it
        end_of_req[2] = 0;
    
        if(!memcmp("GET ", con->buffer, 4))
        {
            con->state = httpd_CS_GET;
            con->url = (char*) con->buffer + 4;
        }
        else if(!memcmp("HEAD ", con->buffer, 5))
        {
            con->state = httpd_CS_HEAD;
            con->url = (char*) con->buffer + 5;
        }
        else if(!memcmp("POST ", con->buffer, 5))
        {
            con->state = httpd_CS_POST;
            con->url = (char*) con->buffer + 5;
        }
        
        // whatever else send I'm a teapot
        if(con->state == httpd_CS_begin)
        {
            return 418;
        }

        // find end of URL, allow HTTP/1.0 missing proto
        uint8_t * end_of_url = (uint8_t*)strpbrk(con->url, " ?");
        if(!end_of_url) end_of_url = end_of_req;

        uint8_t * end_of_query = ('?' == *end_of_url)
            ? (uint8_t*) strpbrk((char*) end_of_url+1, " ") : 0;

        // null-terminate URL
        *end_of_url = 0;

        // null-terminate query string if any
        if(end_of_query) *end_of_query = 0;

        for(int i = 0; i < n_redirect; ++i)
        {
            if(!strcmp(con->url, _redirect[i]))
            {
                http_response(con, 302);
                http_write_string(con, "Location: http://");
                http_write_string(con, ipaddr_ntoa(&pcb->local_ip));
                http_write_string(con, "/\r\n");
                http_end_headers(con);
                return -1;
            }
        }

        for(int i = static_file_offset; i < n_static_files; ++i)
        {
            if(!strcmp(con->url, static_files[i].url))
            {
                con->url = static_files[i].url;
                if(con->state == httpd_CS_POST)
                {
                    http_response(con, 405);
                    http_write_string(con, "Allow: HEAD, GET\r\n");
                    http_end_headers(con);
                    return -1;
                }
                
                if(con->state == httpd_CS_GET)
                {
                    con->file.ptr = static_files[i].data;
                    con->file.len = static_files[i].len;
                    con->len_pending += con->file.len;
                }
                
                http_response(con, 200);
                http_write_string(con, "Content-Length: ");
                http_write_uint(con, con->file.len);
                http_write_string(con, "\r\nContent-Type: ");
                http_write_string(con, static_files[i].content_type);
                http_write_string(con, "\r\n");
                http_end_headers(con);
                return -1;
            }
        }

        for(int i = get_handler_offset; i < n_get_handlers; ++i)
        {
            if(!strcmp(con->url, get_handlers[i].url))
            {
                con->url = get_handlers[i].url;
                if(con->state == httpd_CS_POST)
                {
                    http_response(con, 405);
                    http_write_string(con, "Allow: HEAD, GET\r\n");
                    http_end_headers(con);
                    return -1;
                }

                bool head_only = (con->state == httpd_CS_HEAD);
                con->state = httpd_CS_HEAD;

                // make a copy of the query string
                // this makes life easier for the GET handlers
                if(end_of_query)
                    strcpy((char*) httpd_buffer, (char*) end_of_url + 1);

                int status = get_handlers[i].handler(con, head_only,
                    end_of_query ? (char*) httpd_buffer : 0);

                if(!status) status = 204;

                return status;
            }
        }
        

        for(int i = post_handler_offset; i < n_post_handlers; ++i)
        {
            if(!strcmp(con->url, post_handlers[i].url))
            {
                con->url = post_handlers[i].url;
                if(con->state != httpd_CS_POST)
                {
                    // could perhaps error out, but whatever
                    http_response(con, 405);
                    http_write_string(con, "Allow: Post\r\n");
                    http_end_headers(con);
                    return -1;
                }
                
                con->post.handler = post_handlers[i].handler;

                uint32_t off = (end_of_url - con->buffer) + 1;
                uint8_t * clen =
                    (uint8_t*) strnstr((char*)con->buffer + off,
                        "\r\nContent-Length:", con->header_len - off);
                        
                int status = 0;
                        
                if(!clen)
                {
                    status = 411;
                }
                else
                {
                    clen += sizeof("\r\nContent-Length");
                    while(*clen == ' ') ++clen;
                    
                    while(*clen >= '0' && *clen <= '9')
                    {
                        // overflow?
                        if(con->post.len > UINT_MAX / 10) status = 413;
                        
                        con->post.len *= 10;
                        con->post.len += *clen - '0';
                        ++clen;
                    }

                    // even if we flagged 413, still flag 400
                    // if we don't get a sensible number
                    if((clen[0] != '\r' || clen[1] != '\n'))
                    {
                        status = 400;
                    }
                }

                // We need to make copy of the connection buffer
                // because we want to allow con->buffer for POST handler
                memcpy(httpd_buffer, con->buffer, con->header_len);

                // check if handler accepts the length?
                status = con->post.handler.begin(con, con->post.len);
                if(status) return status;
                
                if(!con->post.len) status = 204;
                
                if(!status)
                {
                    // compute how much header data?
                    // the data has been moved by now, but pointers
                    // are still fine to use for computation
                    uint32_t nhead = (end_of_req - con->buffer) + 4;
                    uint32_t ndata = con->header_len - nhead;

                    // don't allow more content and claimed
                    if(ndata > con->post.len) ndata = con->post.len;
                    
                    // If we don't get any data, ask "please"
                    // Sending this even if the browser doesn't "Expect:"
                    // seems to be perfectly fine, so just do it whenever
                    // don't immediately have content already pending.
                    if(!ndata)
                    {
                        // status 100 is special-cased in _recv
                        return 100;
                    }
                    else
                    {
                        status = con->post.handler.data(
                            con, httpd_buffer + nhead, ndata, 0);
                        con->post.len_recv = ndata;

                        if(!status && ndata >= con->post.len)
                        {
                            status = con->post.handler.end(con, con->post.len);
                            // default to "Done"
                            if(!status) status = 204;
                        }
                        return status;
                    }
                }

            }
        }

        // Do we have a file or a post handler?
        // The union aliases these, so just check one
        if(!con->file.ptr)
        {
            return 404;
        }
    }
    else
    {
        // not full request
        if(con->header_len == sizeof(con->buffer))
        {
            return 414;
        }
    }
    
    return 0;   // waiting more data
}

static err_t httpd_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    httpd_client *con = (httpd_client*)arg;
    if(err != ERR_OK)
    {
        return httpd_close_client(con, pcb, err);
    }

    if(p->tot_len)
    {
        // client is doing something.. so let it live
        con->timeout = 0;

        int status = 0;

        int offset = 0;
        while(offset < p->tot_len)
        {
            // do POST recv first, it's highest bandwidth
            if(con->state == httpd_CS_POST)
            {
                // don't accept more data than client claimed
                int n = con->post.len - con->post.len_recv;
                if(n > sizeof(httpd_buffer)) n = sizeof(httpd_buffer);
                
                n = pbuf_copy_partial(p, httpd_buffer, n, offset);
                if(!n) return httpd_close_client(con, pcb, ERR_BUF);
                offset += n;
                status = con->post.handler.data(con,
                    httpd_buffer, n, con->post.len_recv);
                con->post.len_recv += n;

                if(!status && con->post.len_recv >= con->post.len)
                {
                    status = con->post.handler.end(con, con->post.len);
                    // default to "Done"
                    if(!status) status = 204;
                }

                if(status)
                {
                    // is this stock response?
                    if(status > 0)
                    {
                        http_response(con, status);
                        http_end_headers(con);
                    }

                    // The only status codes where we do something
                    // other than return headers are 100 and 200
                    if(status != 100 && status != 200)
                        con->state = httpd_CS_HEAD;
                }

                continue;
            }

            // don't care about any client data at this point
            // but we must check the pbuf to see if there is an error
            if(con->state == httpd_CS_GET
            || con->state == httpd_CS_HEAD)
            {
                int n = pbuf_copy_partial(p, httpd_buffer, sizeof(httpd_buffer), offset);
                if(!n) return httpd_close_client(con, pcb, ERR_BUF);
                offset = p->tot_len;
                break;
            }

            // don't know what we are doing yet?
            if(con->state == httpd_CS_begin)
            {
                int n = sizeof(con->buffer) - con->header_len;
                n = pbuf_copy_partial(p, con->buffer+con->header_len, n, offset);
                if(!n) return httpd_close_client(con, pcb, ERR_BUF);
                con->header_len += n;
                offset += n;

                status = httpd_recv_headers(con, pcb);
                
                // status 100 is special cased below
                // status -1 means we already wrote headers
                if(status > 0 && status != 100)
                {
                    http_response(con, status);
                    http_end_headers(con);
                    con->state = httpd_CS_HEAD;
                }
            }
            
        }
        
        // We special-case 100 here for two reasons:
        //  - to preserve headers for GET/POST handlers
        //  - to make sure we can retransmit even after writing final headers
        if(status == 100)
        {
            con->len_pending += http_continue_len;
            err = tcp_write(pcb, http_continue, http_continue_len, 0);
            if (err != ERR_OK) {
                // if we free pbuf here we get PANIC
                DEBUG_printf("httpd: error sending headers %d\n", err);
            }
        }
        else if(status)
        {
            con->len_pending += con->header_len;
            err = tcp_write(pcb, con->buffer, con->header_len, 0);
            if (err != ERR_OK) {
                // if we free pbuf here we get PANIC
                DEBUG_printf("httpd: error sending headers %d\n", err);
            }
        }

        tcp_recved(pcb, p->tot_len);
    }
    
    if(err != ERR_OK)
        return httpd_close_client(con, pcb, err);
    else
    {
        return err;
    }
}

// this wraps httpd_recv to make sure we obey the somewhat
// annoying logic with regards to whether to free the pbuf
static err_t httpd_recv_wrap(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    err = httpd_recv(arg, pcb, p, err);
    if(err == ERR_OK) { pbuf_free(p); }
    return err;
}

static err_t httpd_accept(void *arg, struct tcp_pcb *client_pcb, err_t err)
{
    if (err != ERR_OK || client_pcb == NULL) {
        DEBUG_printf("httpd: accept error\n");
        return ERR_VAL;
    }
    DEBUG_printf("httpd: client connected\n");

    // Create the state for the connection
    httpd_client *con = calloc(1, sizeof(httpd_client));
    if (!con) {
        // put this to LOG, because it should not happen
        LOG_printf("httpd: can't alloc pcb\n");
        return ERR_MEM;
    }
    client_pcb->flags |= TF_NODELAY;
    
    // setup connection to client
    tcp_arg(client_pcb, con);
    tcp_sent(client_pcb, httpd_sent);
    tcp_recv(client_pcb, httpd_recv_wrap);
    tcp_poll(client_pcb, httpd_poll, 1);  // about twice a second
    tcp_err(client_pcb, httpd_err);

    con->url = "??";
    
    return ERR_OK;
}

bool httpd_init(
    uint32_t _static_file_offset,
    uint32_t _get_handler_offset,
    uint32_t _post_handler_offset)
{
    static_file_offset = _static_file_offset;
    get_handler_offset = _get_handler_offset;
    post_handler_offset = _post_handler_offset;
    
    DEBUG_printf("starting server on port %d\n", HTTPD_PORT);

    // NOTE: we currently leak this.. but that's fine 'cos we always running
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        LOG_printf("failed to create pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, IP_ANY_TYPE, HTTPD_PORT);
    if (err) {
        LOG_printf("failed to bind to port %d\n",HTTPD_PORT);
        return false;
    }

    pcb = tcp_listen_with_backlog(pcb, 5);
    if (!pcb) {
        LOG_printf("failed to listen\n");
        if (pcb) {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(pcb, 0);    // takes state.. but like .. we don't care
    tcp_accept(pcb, httpd_accept);

    LOG_printf("HTTPD (%d) started\n", HTTPD_PORT);
    return true;
}
