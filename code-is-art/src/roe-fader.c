/*
 * Copyright (c) 2024 Pihlaja Voipio
 *
 * SPDX-License-Identifier: CC0-1.0
 *
*/

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "pico/sync.h"
#include "pico/multicore.h"
#include "pico/flash.h"     // for lockout support

#include <string.h>
#include <stdlib.h>     // rand()

#include "manda.h"
#include "hacklab.h"
#include "tunnel.h"
#include "tunnel-data.h"

#include "matrix.h"
#include "palettes.h"

#include "roe-pico.h"

static const uint8_t digits[] =
{
    /* 0 */
    0b0010,
    0b0101,
    0b0101,
    0b0101,
    0b0010,
    /* 1 */
    0b0010,
    0b0110,
    0b0010,
    0b0010,
    0b0111,
    /* 2 */
    0b0110,
    0b0001,
    0b0010,
    0b0100,
    0b0111,
    /* 3 */
    0b0110,
    0b0001,
    0b0110,
    0b0001,
    0b0110,
    /* 4 */
    0b0010,
    0b0100,
    0b0111,
    0b0010,
    0b0010,
    /* 5 */
    0b0111,
    0b0100,
    0b0110,
    0b0001,
    0b0110,
    /* 6 */
    0b0011,
    0b0100,
    0b0110,
    0b0101,
    0b0010,
    /* 7 */
    0b0111,
    0b0001,
    0b0001,
    0b0010,
    0b0100,
    /* 8 */
    0b0010,
    0b0101,
    0b0010,
    0b0101,
    0b0010,
    /* 9 */
    0b0010,
    0b0101,
    0b0011,
    0b0001,
    0b0110,
    /* . */
    0b0000,
    0b0000,
    0b0000,
    0b0000,
    0b0010,
};

static int magic_mode = 0;

/*

    This mess randomly fades from one image to another.
    - imageNew is target image
    - frameBuf is the frame buffer
    - imageQueue is used to queue images from HTTPD
*/

// Can't use mutex with background LwIP and critical sections kinda suck
// so we'll just use a pointer as a queue and spin when non-null..
static uint32_t     *queue_img = 0;

// we need 2 framebuffers (current and target image)
static uint32_t     imageNew[88*88] = {};
static uint32_t     frameBuf[88*88] = {};

// use a third one where we copy the image on queue, so that we don't need
// to stall until it's actually picked up by the update code..
static uint32_t     imageQueue[88*88] = {};

// Sadly the fire really doesn't work well if this is 8 bit
// so even though we are RAM starved, we'll do what we have to do
// .. we could theoretically union this on top of imageNew .. but
// that's arguably kinda ugly ..
uint16_t fireBuf[88*90] = {};

static uint8_t      trType = 4;
static uint8_t      trTime = 0;

// this is an ordered dither pattern
// doesn't matter too much as long as it's "random"
const uint8_t pattern[16] =
{
    0, 8, 2, 10,
    12, 4, 14, 6,
    3, 11, 1, 9,
    15, 7, 13, 5
};

static void copy_random_lines(int dir)
{
    int t = trTime;
    if(t > 44) return;
    
    switch(dir)
    {
    case 0:
        for(int x = 0; x < 88; ++x)
        {
            int d = 4 + (pattern[x%16] >> 1);
            int oldState = (t > 1) ? 0 : d*(t-1);
            int newState = d*t;
            if(newState > 88) newState = 88;
            for(int y = oldState; y < newState; ++y)
            {
                frameBuf[y*88+x] = imageNew[y*88+x];
            }
        }
        break;
    case 1:
        for(int x = 0; x < 88; ++x)
        {
            int d = 4 + (pattern[x%16] >> 1);
            int oldState = (t > 1) ? 0 : d*(t-1);
            int newState = d*t;
            if(newState > 88) newState = 88;
            for(int y = oldState; y < newState; ++y)
            {
                frameBuf[(87-y)*88+x] = imageNew[(87-y)*88+x];
            }
        }
        break;
    case 2:
        for(int y = 0; y < 88; ++y)
        {
            int d = 4 + (pattern[y%16] >> 1);
            int oldState = (t > 1) ? 0 : d*(t-1);
            int newState = d*t;
            if(newState > 88) newState = 88;
            for(int x = oldState; x < newState; ++x)
            {
                frameBuf[y*88+x] = imageNew[y*88+x];
            }
        }
        break;
    case 3:
        for(int y = 0; y < 88; ++y)
        {
            int d = 4 + (pattern[y%16] >> 1);
            int oldState = (t > 1) ? 0 : d*(t-1);
            int newState = d*t;
            if(newState > 88) newState = 88;
            for(int x = oldState; x < newState; ++x)
            {
                frameBuf[y*88+(87-x)] = imageNew[y*88+(87-x)];
            }
        }
        break;
    }
}

// this takes blend range [0, 0xff]
static inline uint32_t blend(uint32_t c, uint8_t blend)
{
    if(!blend) return 0;
    if(blend==0xff) return c;

    // two components at a time
    uint32_t ag = (((c>>8)& 0xff00ff)*blend) + 0x800080;
    uint32_t rb = ((c     & 0xff00ff)*blend) + 0x800080;

    ag = ((ag + ((ag&0xff00ff00)>>8))&0xff00ff00);
    rb = ((rb + ((rb&0xff00ff00)>>8))&0xff00ff00)>>8;
    return ag|rb;
}

// lerp the colors using an alpha value as parameter
// gives c0 when a = 0, c1 when a=0xff
static inline uint32_t alphaMask(uint32_t c0, uint32_t c1, uint8_t a) {
    return blend(c0, ~a) + blend(c1, a);
}

static void do_blend()
{
    int t = 4*trTime;
    if(t > 256) return;
    
    for(int y = 0; y < 88; ++y)
    {
        for(int x = 0; x < 88; ++x)
        {
            frameBuf[y*88+x] = alphaMask(frameBuf[y*88+x], imageNew[y*88+x], t-1);
        }
    }
}

static void do_middle()
{
    int t = 2*trTime;
    if(t > 44) return;
    
    for(int y = 44-t; y < 44+t; ++y)
    {
        for(int x = 44-t; x < 44+t; ++x)
        {
            frameBuf[y*88+x] = imageNew[y*88+x];
        }
    }
}

static void do_close(int axis)
{
    int t = 2*trTime;
    if(t > 44) return;

    switch(axis)
    {
    case 0:
        for(int y = 0; y < 88; ++y)
        {
            for(int x = t-2; x < t; ++x)
            {
                frameBuf[y*88+x] = imageNew[y*88+x];
            }
            for(int x = 88-t; x < 90-t; ++x)
            {
                frameBuf[y*88+x] = imageNew[y*88+x];
            }
        }
        break;
    case 1:
        for(int x = 0; x < 88; ++x)
        {
            for(int y = t-2; y < t; ++y)
            {
                frameBuf[y*88+x] = imageNew[y*88+x];
            }
            for(int y = 88-t; y < 90-t; ++y)
            {
                frameBuf[y*88+x] = imageNew[y*88+x];
            }
        }
        break;
    }
}

static void do_scroll(int dir)
{
    int t = 4*(trTime-1);
    if(t > 84) return;

    switch(dir)
    {
    case 0:
        for(int y = 0; y < 88; ++y)
        {
            for(int x = 0; x < 84; ++x)
            {
                frameBuf[y*88+x] = frameBuf[y*88+(x+4)];
            }
            for(int x = 84; x < 88; ++x)
            {
                frameBuf[y*88+x] = imageNew[y*88+(t+x-84)];
            }
        }
        break;
    case 1:
        for(int y = 0; y < 88; ++y)
        {
            for(int x = 88; x > 4; --x)
            {
                frameBuf[y*88+(x-1)] = frameBuf[y*88+(x-5)];
            }
            for(int x = 0; x < 4; ++x)
            {
                frameBuf[y*88+x] = imageNew[y*88+(x-t+84)];
            }
        }
        break;
    case 2:
        for(int x = 0; x < 88; ++x)
        {
            for(int y = 0; y < 84; ++y)
            {
                frameBuf[y*88+x] = frameBuf[(y+4)*88+x];
            }
            for(int y = 84; y < 88; ++y)
            {
                frameBuf[y*88+x] = imageNew[(t+y-84)*88+x];
            }
        }
        break;
    case 3:
        for(int x = 0; x < 88; ++x)
        {
            for(int y = 88; y > 4; --y)
            {
                frameBuf[(y-1)*88+x] = frameBuf[(y-5)*88+x];
            }
            for(int y = 0; y < 4; ++y)
            {
                frameBuf[y*88+x] = imageNew[(y-t+84)*88+x];
            }
        }
        break;
    }
}

static void do_dither()
{
    int t = trTime - 1;
    if(t >= 16) return;

    int d = pattern[t];
    int xo = 0x3 & d;
    int yo = 0x3 & (d >> 2);

    for(int y = 0; y < 88; y += 4)
    {
        for(int x = 0; x < 88; x += 4)
        {
            frameBuf[(yo+y)*88+(xo+x)] = imageNew[(yo+y)*88+(xo+x)];
        }
    }
}

static void do_dither_blocks()
{
    int t = trTime - 1;
    if(t >= 16) return;

    int d = pattern[t];
    int xo = 0x3 & d;
    int yo = 0x3 & (d >> 2);

    xo *= 22;
    yo *= 22;

    for(int y = 0; y < 22; ++y)
    {
        for(int x = 0; x < 22; ++x)
        {
            frameBuf[(yo+y)*88+(xo+x)] = imageNew[(yo+y)*88+(xo+x)];
        }
    }
}

static void do_dither_lines(int axis)
{
    int t = trTime - 1;
    if(t >= 16) return;

    int d = pattern[t>>1] >> 1;

    switch(axis)
    {
    case 0:
        for(int y = 0; y < 88; y += 8)
        {
            for(int x = 0; x < 88; ++x)
            {
                frameBuf[(d+y)*88+x] = imageNew[(d+y)*88+x];
            }
        }
        break;
    case 1:
        for(int y = 0; y < 88; ++y)
        {
            for(int x = 0; x < 88; x += 8)
            {
                frameBuf[y*88+(d+x)] = imageNew[y*88+(d+x)];
            }
        }
        break;
    }
}

static void check_queue()
{
    __dmb();
    if(queue_img)
    {
        memcpy(imageNew, queue_img, sizeof(frameBuf));
        queue_img = 0;
        __dmb();
    
        trTime = 0;
        trType = get_rand_32();
    }
}

static void img_update()
{
    if(trTime == 255) return;

    // for reasons of "I'm not going to rewrite them all right now"
    // we increment before calling the functions..
    ++trTime;
    
    switch(trType % 16)
    {
    case 0: copy_random_lines(0); break;
    case 1: copy_random_lines(1); break;
    case 2: copy_random_lines(2); break;
    case 3: copy_random_lines(3); break;

    case 4: do_blend(); break;
    case 5: do_middle(); break;

    case 6: do_scroll(0); break;
    case 7: do_scroll(1); break;
    case 8: do_scroll(2); break;
    case 9: do_scroll(3); break;
    
    case 10: do_close(0); break;
    case 11: do_close(1); break;

    case 12: do_dither(); break;
    case 13: do_dither_blocks(); break;

    case 14: do_dither_lines(0); break;
    case 15: do_dither_lines(1); break;
    }

    // release mutex before we actually send to display

    roe_draw_image(frameBuf);
}

void roe_queue_image(uint32_t *img)
{
    magic_mode = 0;
    
    // simply spin until queue_img is null
    while(queue_img) { __dmb(); }

    memcpy(imageQueue, img, sizeof(frameBuf));
    queue_img = imageQueue;
    
    __dmb();
}

void roe_draw_splash()
{
    // initialize framebuffer with lab logo
    for(int i = 0; i < 88*88; ++i)
    {
        frameBuf[i]
            = (hacklab_rgb[3*i]) + (hacklab_rgb[3*i+1]<<8) + (hacklab_rgb[3*i+2]<<16);
    }

    roe_draw_image(frameBuf);
}

static uint32_t rotate = 0;

static void do_magic()
{
    ++rotate;
    for(int i = 0; i < 88*88; ++i)
    {
        uint8_t x = 0x3f & (rotate+((sine[(rotate)&0xff] + tunnel_map[3*i])>>2));
        uint8_t y = 0x3f & ((rotate + (tunnel_map[3*i+1]>>2)));
        uint8_t k = tunnel_tex[x+64*y];

        uint8_t z = tunnel_map[3*i+2];

        uint32_t color = blend(palette[0x3f&(((-z+rotate)>>3))], k);
        uint8_t zz = z < 0x80 ? z<<1 : 0xff;
        frameBuf[i] = blend(color, zz);
    }
    roe_draw_image(frameBuf);
}

void do_matrix()
{
    ++rotate;
    for(int i = 0; i < 88*88; ++i)
    {
        frameBuf[i] = matrix_palette[0xff & (rotate-matrix_gray[i])];
    }
    roe_draw_image(frameBuf);
}

void do_fire()
{
    for(int x = 0; x < 88; ++x)
    {
        fireBuf[88*89 + x] = (fireBuf[88*89 + x] + (0xffff&get_rand_32()))>>1;
    }

    for(int y = 89; --y;)
    {
        for(int x = 0; x < 88; ++x)
        {
            int32_t fire = (((int32_t)(fireBuf[88*y + x])
                +(int32_t)(fireBuf[88*(y+1) + (x+87)%88])
                +(int32_t)(fireBuf[88*(y+1) + (x+1)%88])
                +(int32_t)fireBuf[88*(y+1) + x]) >> 2) - (150 - 2*y);
            if(fire < 0) fire = 0;
            fireBuf[88*y + x] = fire;
        }
    }
    
    for(int i = 0; i < 88*88; ++i)
    {
        frameBuf[i] = fire_palette[0xff&(fireBuf[i]>>8)];
    }
    
    roe_draw_image(frameBuf);
}


static void core1_main()
{
    // we need this thing..
    flash_safe_execute_core_init();

    while(1)
    {
        // sleep 10ms, but check queued image every 1ms
        for(int i = 0; i < 10; ++i)
        {
            sleep_ms(1);
            check_queue();
        }
        switch(magic_mode)
        {
        case 1: do_magic(); break;
        case 2: do_matrix(); break;
        case 3: do_fire(); break;
        default: img_update();
        }
    }
}

void roe_init_fader(const char * txt)
{
    // 16 is just enough for 123.123.123.123 + '\0'
    //
    // the copy here is mostly "just in case"
    char ip_txt[16] = {};
    strncpy(ip_txt, txt, 15);   // don't use sizeof, txt decays to pointer..

    // initialize Manda as target image
    for(int i = 0; i < 88*88; ++i)
    {
        imageNew[i]
            = (manda_bgr[3*i]<<16) + (manda_bgr[3*i+1]<<8) + manda_bgr[3*i+2];
    }

    // draw IP at the top, where there's some white area
    {
        int x = 50 - 2*strlen(ip_txt);

        uint32_t fontColor = 0x808080;

        for(char * c = ip_txt; *c; ++c)
        {
            const uint8_t *g = 0;
            if(*c >= '0' && *c <= '9')
            {
                g = &digits[5*(*c - '0')];
            }
            else if(*c == '.')
            {
                g = &digits[50];
            }
            else continue;  // skip not digits or dot
            
            // font is 5 pixels high
            for(int y = 0; y < 5; ++y)
            {
                if(1&((*g)>>2)) imageNew[(y+1)*88+x+0] = fontColor;
                if(1&((*g)>>1)) imageNew[(y+1)*88+x+1] = fontColor;
                if(1&((*g)>>0)) imageNew[(y+1)*88+x+2] = fontColor;
                ++g;
            }
            x += 4;
        }
    }
    
    trType = 4; // blend
    trTime = 0;
    
    multicore_launch_core1(core1_main);
}

void roe_enable_magic(int x)
{
    magic_mode = x;
    __dmb();
}