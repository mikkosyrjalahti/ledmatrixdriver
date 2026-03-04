
/*
 * Copyright (c) 2024 Pihlaja Voipio
 *
 * SPDX-License-Identifier: CC0-1.0
 *
*/
#pragma once

#include "wificonfig.h"

// we put this here, so we can set con->url for proper logging
static const char post_wifi_url[] = "/set_wificonfig";

static int post_wifi_begin(httpd_client *con, uint32_t len)
{
    if(!len) return 0;

    // we have ~1.4kB space in TCP_MSS so accept 1kB
    if(len > 1024) return 413;

    // zero out the connection buffer for temp storage
    memset(con->buffer, 0, sizeof(con->buffer));
    
    return 0;
}

static int post_wifi_data(httpd_client *con,
    uint8_t * data, uint32_t len, uint32_t offset)
{
    for(int i = 0; i < len; ++i)
    {
        con->buffer[i+offset] = data[i];
    }
    return 0;    
}

static int post_wifi_end(httpd_client *con, uint32_t len)
{
    // we should now have regular form-data in con->buffer
    //
    // ie: ssid=network&pass=password
    // theoretically could also be the other way :P
    wificonfig  config;
    memset(&config, 0, sizeof(wificonfig));

    int gotSSID = false;
    int gotPASS = false;

    char * ptr = (char*) con->buffer;
    while(*ptr)
    {
        if(!gotSSID && !memcmp(ptr, "ssid=", 5))
        {
            gotSSID = true;
            ptr += 5;
            for(int i = 0; i < MAX_SSID; ++i)
            {
                // done with parameter?
                if(!*ptr || *ptr == '&') break;
                if(*ptr == '%') // need to parse
                {
                    int x = 0;
                    ++ptr;
                    if(*ptr >= '0' && *ptr <= '9') x += ((*ptr)-'0');
                    else if(*ptr >= 'A' && *ptr <= 'F') x += (10+((*ptr)-'A'));
                    else return 400;
                    x <<= 4;
                    ++ptr;
                    if(*ptr >= '0' && *ptr <= '9') x += ((*ptr)-'0');
                    else if(*ptr >= 'A' && *ptr <= 'F') x += (10+((*ptr)-'A'));
                    else return 400;
                    ++ptr;
                    config.ssid[i] = x;
                    
                }
                else if(*ptr == '+')
                {
                    config.ssid[i] = ' ';
                    ++ptr;
                }
                else
                {
                    config.ssid[i] = *ptr;
                    ++ptr;
                }
            }
            if(*ptr && *ptr != '&') return 400; // bad request
            if(*ptr == '&') ++ptr;
            continue;
        }
        
        if(!gotPASS && !memcmp(ptr, "pass=", 5))
        {
            gotPASS = true;
            ptr += 5;
            for(int i = 0; i < MAX_PASS; ++i)
            {
                // done with parameter?
                if(!*ptr || *ptr == '&') break;
                if(*ptr == '%') // need to parse
                {
                    int x = 0;
                    ++ptr;
                    if(*ptr >= '0' && *ptr <= '9') x += ((*ptr)-'0');
                    else if(*ptr >= 'A' && *ptr <= 'F') x += (10+((*ptr)-'A'));
                    else return 400;
                    x <<= 4;
                    ++ptr;
                    if(*ptr >= '0' && *ptr <= '9') x += ((*ptr)-'0');
                    else if(*ptr >= 'A' && *ptr <= 'F') x += (10+((*ptr)-'A'));
                    else return 400;
                    ++ptr;
                    config.pass[i] = x;
                }
                else if(*ptr == '+')
                {
                    config.pass[i] = ' ';
                    ++ptr;
                }
                else
                {
                    config.pass[i] = *ptr;
                    ++ptr;
                }
            }
            if(*ptr && *ptr != '&') return 400; // bad request
            if(*ptr == '&') ++ptr;
            continue;
        }
        
        return 400; // bad request
    }
    
    if(gotSSID && gotPASS)
    {
        config.magic0 = WIFI_MAGIC;
        config.magic1 = WIFI_MAGIC;

        // if this returns, then writing to flash failed
        wificonfig_save_flash(&config);

        return 500; // internal error
    }
    else return 400; // bad request

}

static const httpd_post_handler post_wificonfig =
{
    .begin = post_wifi_begin,
    .data = post_wifi_data,
    .end = post_wifi_end
};