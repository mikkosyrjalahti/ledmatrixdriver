/*
 * Copyright (c) 2024 Pihlaja Voipio
 *
 * SPDX-License-Identifier: CC0-1.0
 *
*/
#pragma once

#include "roe-pico.h"

/*
 * Example POST-handler: set display framebuffer image
 *
 * This copies uploaded data into a framebuffer array (image) and
 * calls DISP_present() to send this to a screen when upload completes.
 *
*/

#define HTTPD_POST_SETIMG   // include '/setimg' URL

// buffer for POST request to fill
static uint32_t post_fb_image[88*88];

// This is for avoiding upload conflicts..
// We set this to the connection pointer on Content-Length validation
// and then fail the response if it does not match.
//
// This way it's always the "most recently started" upload that is allowed,
// without having to track whether previous requests are still stalling.
static httpd_client * post_fb_client = 0;

// This validates the request length and sets our conflict pointer
// to the current client if the request is non-zero length.
static int post_fb_begin(httpd_client *con, uint32_t len)
{
    if(!len) return 0;  // allow zero-length without doing anything
    
    if(len > sizeof(post_fb_image)) return 413;

    // set conflict prevention pointer - most recent request wins
    post_fb_client = con;
    return 0;
}

// Actual data upload, just copies .. this does NOT need to check
// bounds, because _begin() refuses oversized requests.
static int post_fb_data(httpd_client * con,
    uint8_t * data, uint32_t len, uint32_t offset)
{
    if(!len) return 0;  // allow zero-length without doing anything

    if(post_fb_client != con) return 409;  // conflict

    for(int i = 0; i < len; ++i)
    {
        ((uint8_t*)post_fb_image)[i+offset] = data[i];
    }

    return 0;
}

// This is called when the upload completes successfully.
static int post_fb_end(httpd_client * con, uint32_t len)
{
    if(post_fb_client != con) return 409;  // conflict
    
    if(len)
    {
        roe_queue_image(post_fb_image);
    }
    return 0;
}

static const httpd_post_handler post_fb =
{
    .begin = post_fb_begin,
    .data = post_fb_data,
    .end = post_fb_end
};

// Magic mode triggers
static int get_magic(httpd_client *con, bool head_only, const char * query)
{
    if(!head_only) roe_enable_magic(1);
    return 204;
}
static int get_spoon(httpd_client *con, bool head_only, const char * query)
{
    if(!head_only) roe_enable_magic(2);
    return 204;
}
static int get_fire(httpd_client *con, bool head_only, const char * query)
{
    if(!head_only) roe_enable_magic(3);
    return 204;
}
