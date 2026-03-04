/*
 * Copyright (c) 2024 Pihlaja Voipio
 *
 * SPDX-License-Identifier: CC0-1.0
 *
*/

#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/cyw43_arch.h"

#include "hardware/sync.h"  // __wfi
#include "hardware/watchdog.h"
#include "hardware/flash.h"

#include "dnsserver.h"
#include "dhcpserver.h"

#include "dust-httpd.h"

#include "roe-pico.h"

#include "wificonfig.h"

// only works after wifi init
static void set_led(bool on)
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

extern cyw43_t cyw43_state;
static bool wifi_act = false;

// we don't REALLY filter stuff.. we just use passthru to track activity
// this gives us activity LED blink
struct netif * dust_arp_filter(struct pbuf *p, struct netif *netif, u16_t eth_type)
{
    wifi_act = true;
    return netif;
}

// bootsel.c - do NOT call this once core1 has been launched
bool get_bootsel_button();

static char apname[32] = {};

int main()
{
    stdio_init_all();
    httpd_stdio_init();
    
    printf("Pico booting...\n");

    // init this first, so we can use LED
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_FINLAND))
    {
        printf("ERROR: Network init failed.\n");
        return -1;
    }

    set_led(1);

    // merged reset and gclock_init into this..
    roe_init();

    // draw splash
    roe_draw_splash();

    // sleep just a bit, so splash is always visible for a moment
    sleep_ms(2000);

    set_led(0);

    bool enableAP = false;
    
    // if there's no wifi-config in flash, then enable AP
    if(flash_wificonfig->magic0 != WIFI_MAGIC
    || flash_wificonfig->magic1 != WIFI_MAGIC)
    {
        enableAP = true;
        printf("WiFi configuration not found.\n");
    }
    else
    {
        if(!flash_wificonfig->ssid[0])
        {
            enableAP = true;
            printf("WiFi configured as AP.\n");
        }
    }

    // if we have a WiFi config, but BOOTSEL is pressed then
    // enable AP again.. note that checking BOOTSEL here might
    // glitch the gclock.. but we just have to live with that..
    if(!enableAP && get_bootsel_button())
    {
        enableAP = true;
        printf("WiFi AP enabled by BOOTSEL.\n");
    }

#if defined(CYW43_USE_OTP_MAC) && (0 == CYW43_USE_OTP_MAC)
    {
        // generate a random MAC every boot
        // NOTE: build must define CYW43_USE_OTP_MAC=0
        // or this will get overwritten by hardware MAC
        uint64_t rand_mac = get_rand_64();
        memcpy(cyw43_state.mac, &rand_mac, 6);
        cyw43_state.mac[0] &= (uint8_t)~0x1; // unicast
        cyw43_state.mac[0] |= 0x2; // locally administered
    }
#endif

    printf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        cyw43_state.mac[0],cyw43_state.mac[1],cyw43_state.mac[2],
        cyw43_state.mac[3],cyw43_state.mac[4],cyw43_state.mac[5]);

    cyw43_wifi_pm(&cyw43_state, 0xa11140);

    // this is set to either AP or STA IP based on whether mode
    ip4_addr_t myip;

    if(!enableAP)
    {

        //
        // Try to join another network
        //
        cyw43_arch_enable_sta_mode();
    
        {
            struct netif * ni = &cyw43_state.netif[CYW43_ITF_STA];
            cyw43_arch_lwip_begin();
            netif_set_hostname(ni, "ledpanel");
            cyw43_arch_lwip_end();
        }
    
        const char * join_ssid = (char*)flash_wificonfig->ssid;
        const char * join_pass = (char*)flash_wificonfig->pass;
        // if password is empty, set nullptr
        if(!join_pass[0]) join_pass = 0;
    
        // if this fails, just keep trying again..
        // use mixed for better chance we'll manage to connect
        while(cyw43_arch_wifi_connect_timeout_ms(
            join_ssid, join_pass, CYW43_AUTH_WPA2_MIXED_PSK, 10000))
        {
                // blink LED "slowly" so state is clear we're failing
                set_led(1);
                sleep_ms(250);
                set_led(0);
                sleep_ms(250);

                // keep checking bootsel if we're not connecting
                // .. again this might glitch gclock but .. hmmh yeah
                if(get_bootsel_button())
                {
                    printf("WiFi connect failed, AP enabled by BOOTSEL.\n");
                    enableAP = true;
                    cyw43_arch_disable_sta_mode();
                    break;
                }
        }
    }

    // if we didn't break out, we should be in WiFi now..
    if(!enableAP)
    {
        // NOTE: the interface .ip_addr is defined as ip_addr_t which seems
        // to be an union of ip4, ip6 in LWIP headers... but trying to access
        // the union field gives error with "ip_addr_t (aka. struct ip_addr4_t"
        // so .. there might be some funny business going on with LwIP config
        // ..
        // either way this SHOULD work .. even though we need IP4 specifically
        // because there's just no way we can fit IPv6 address on the panel
        memcpy(&myip,
            &cyw43_state.netif[CYW43_ITF_STA].ip_addr, sizeof (ip4_addr_t));
        
        printf("Got IP addr: %s\n", ip4addr_ntoa(&myip));
    }
    else
    {
        //
        // Access point setup
        //
        uint8_t id[8] = {};
        flash_get_unique_id(id);
        sprintf(apname, "LEDPANEL-%02x%02x%02x%02x%02x%02x%02x%02x",
            id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
 
        cyw43_arch_enable_ap_mode(apname, "ledpanel", CYW43_AUTH_WPA2_AES_PSK);
        printf("SSID: %s\n", apname);
    
        // use carrier grade NAT range to try to convince
        // Android devices that this isn't a private network
        // because captive won't work with 192.168.x.x or 10.x.x.x
        ip4_addr_t mask;
        IP4_ADDR(&myip, 100, 69, 42, 1);
        IP4_ADDR(&mask, 255, 255, 255, 0);
    
        // we need to poke into internals a bit
        {
            struct netif * ni = &cyw43_state.netif[CYW43_ITF_AP];
            cyw43_arch_lwip_begin();
            netif_set_hostname(ni, "ledpanel");
            netif_set_addr(ni, &myip, &mask, &myip);
            cyw43_arch_lwip_end();
        }
    
        // captive DNS server
        dns_server_t dns_server;
        dns_server_init(&dns_server, &myip);
    
        // captive DHCP server
        dhcp_server_t dhcp_server;
        dhcp_server_init(&dhcp_server, &myip, &mask);
    }
    
    // start HTTPD and tell it to skip config interface
    // unless we're running in access-point mode ..
    httpd_init(enableAP ? 0 : 1, 0, enableAP ? 0 : 1);

    // if we don't poll in 5 seconds, we've probably crashed
    watchdog_enable(5000, true);

    // image fader core1 .. pass IP
    //
    // this starts the image fading routine on second core
    roe_init_fader(ip4addr_ntoa(&myip));
    
    while(1)
    {
        __wfi();
        if(enableAP
        || CYW43_LINK_UP == cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA))
            watchdog_update();

        if(wifi_act)
        {
            set_led(1);
            wifi_act = false;
            set_led(0);
        }

    }
    
}