
#pragma once

#define MAX_SSID    32  // standard maximum, not nullterminated
#define MAX_PASS    64  // 63 characters or 64 hexadecimal digits? whatever

#define WIFI_MAGIC ('W' | ('i'<<8) | ('F'<<16) | ('i'<<24))

typedef struct wificonfig_t
{
    uint32_t    magic0; // set to WIFI_MAGIC when flash contains valid config

    uint8_t     ssid[MAX_SSID + 1];
    uint8_t     pass[MAX_SSID + 1];

    uint32_t    magic1; // set to WIFI_MAGIC when flash contains valid config
} wificonfig;

#include "pico/flash.h"
#include "hardware/flash.h"

// place flash storage at the very end.. however on RP2350
// the very end is rewritten to workaround a bug.. so subtract 16kB
#define WIFI_CONFIG_LOCATION (PICO_FLASH_SIZE_BYTES - 16*1024);

static const uint32_t _flash_end = WIFI_CONFIG_LOCATION;  // 2MB on pico
static const uint32_t _flash_offset = (_flash_end - sizeof(wificonfig)) & ~0xfff;

static const wificonfig * const flash_wificonfig
    = ((wificonfig*)(XIP_BASE + _flash_offset));

static void _wificonfig_save_flash(void * ptr)
{
    flash_range_erase(_flash_offset, _flash_end - _flash_offset);
    flash_range_program(_flash_offset,
        (uint8_t*)ptr, _flash_end - _flash_offset);
}

// only returns if writing to flash failed
static void wificonfig_save_flash(wificonfig * config)
{
    // set timeout to 500ms
    if(PICO_OK == flash_safe_execute(_wificonfig_save_flash, config, 500))
    {
        // fail if the data in the flash does not agree with what we wrote
        if(memcmp(config, flash_wificonfig, sizeof(wificonfig))) return;
        
        while(1) {} // let watchdog reboot us
    }
}
