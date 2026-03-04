
#pragma once

struct netif * dust_arp_filter(struct pbuf *p, struct netif *netif, u16_t eth_type);

#define LWIP_ARP_FILTER_NETIF_FN    dust_arp_filter

