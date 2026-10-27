/**
 * @file lwipopts.h
 * @brief lwIP settings for the WiFi stack (pico_cyw43_arch_lwip_threadsafe_background: lwIP runs in a low-priority interrupt on core 0, no RTOS).
 *
 * All buffers are static SRAM; see the RAM notes in README.md.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0  ///< malloc isn't safe from the lwIP interrupt
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    8000   ///< lwIP's own heap: outgoing segments
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)
#define PBUF_POOL_SIZE              16     ///< incoming packets, 1.5 KB each

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_TCP_KEEPALIVE          1

/// SNTP sets the clock (clock.c) once WiFi is up; HTTPS needs the date.
#define SNTP_SERVER_DNS             1
#define SNTP_STARTUP_DELAY          0
void net_set_time(uint32_t sec, uint32_t us);
#define SNTP_SET_SYSTEM_TIME_US(sec, us) net_set_time(sec, us)

#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)  ///< must fit net_conn's receive ring
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define LWIP_CHKSUM_ALGORITHM       3

#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
