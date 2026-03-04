#ifndef ROEPICO_H
#define ROEPICO_H

#include "pico/stdlib.h"

// initialize panel and glock handler
void roe_init();

// draw an 8-bit RGBA image (nominally sRGB),
// one uin32_t per pixel must be 88*88
void roe_draw_image(uint32_t *image);

// image fader init (img-fader.c)
// "iptxt" should only contain digits and dots
void roe_init_fader(const char * iptxt);

// draw splash logo
void roe_draw_splash();

// queue an image for the image fader
void roe_queue_image(uint32_t *image);

// enable magic
void roe_enable_magic(int x);

#endif /* ROEPICO_H */
