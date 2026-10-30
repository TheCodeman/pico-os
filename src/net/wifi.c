/**
 * @file wifi.c
 * @brief The wifi command, boot-time auto-connect, and link status messages.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "hardware/clocks.h"
#include "net.h"
#include "pico/cyw43_arch.h"
#include "pico/cyw43_driver.h"
#include "pico/stdlib.h"
#include "serial_console.h"
#include "shell.h"

/// The settings file.
#define CONFIG_PATH     "0:/WIFI.CFG"
/// How long to wait for a connection.
#define CONNECT_WAIT_MS 30000
#define RETRY_MS        30000  ///< how often a lost saved network is tried again
/// The most networks a scan lists.
#define SCAN_MAX        24

static bool radio;              ///< the chip started
static bool want;               ///< keep connected to `ssid` (retry when lost)
/// The network and its password.
///@{
static char ssid[33], password[64];
///@}
static int reported;            ///< link status last reported
/// When to try connecting again.
static absolute_time_t retry_at;

/// The station interface.
/// @return it
static struct netif *sta(void) {
    return &cyw43_state.netif[CYW43_ITF_STA];
}

/// The link's status.
/// @return CYW43_LINK_*
static int link_status(void) {
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
}

/// Why connecting failed.
/// @param status the link status
/// @return the reason
static const char *why(int status) {
    switch (status) {
    case CYW43_LINK_BADAUTH: return "wrong password";
    case CYW43_LINK_NONET: return "network not found";
    case CYW43_LINK_FAIL: return "failed";
    default: return "no answer";
    }
}

/// Starts connecting.
static void start_connect(void) {
    cyw43_arch_wifi_connect_async(ssid, password[0] ? password : NULL,
                                  password[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN);
    retry_at = make_timeout_time_ms(RETRY_MS);
}

/// gSPI clock dividers to try (the PIO runs at clk_sys / div): the SDK's
/// default first, then slower ones in case the bus is marginal.
static const uint8_t clock_divs[] = {2, 3, 4, 6};
static int start_attempts;  ///< how many tries the radio needed (for debugging)

/// Powers the radio up and loads its firmware. The driver otherwise does this
/// lazily on the first command and only prints a warning if it fails.
/// @return started
static bool start_radio(void) {
    for (unsigned i = 0; i < sizeof clock_divs; ++i) {
        ++start_attempts;
        // The dividers are for 126 MHz; scale them so the bus clock stays
        // the same at a faster system clock.
        unsigned mhz = (clock_get_hz(clk_sys) + MHZ / 2) / MHZ;
        cyw43_set_pio_clkdiv_int_frac8((clock_divs[i] * mhz + 125) / 126, 0);
        uint32_t pm;
        cyw43_arch_lwip_begin();
        int r = cyw43_wifi_get_pm(&cyw43_state, &pm);  // runs the power-up
        cyw43_arch_lwip_end();
        if (r == 0) {
            if (i)
                debug_printf("WiFi: radio started at the slower bus clock (div %u)\n", clock_divs[i]);
            return true;
        }
        debug_printf("WiFi: radio didn't answer (div %u, error %d)%s\n", clock_divs[i], r,
               i + 1 < sizeof clock_divs ? ", retrying" : "");
    }
    return false;
}

static bool arch_up;  ///< cyw43_arch_init done

void net_init(void) {
    if (!arch_up) {
        if (cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE) != 0) {
            debug_printf("WiFi: driver failed to start\n");
            return;
        }
        arch_up = true;
    }
    if (!start_radio()) {
        debug_printf("WiFi: radio not responding (wifi on tries again)\n");
        return;
    }
    radio = true;
    cyw43_arch_enable_sta_mode();
    uint8_t mac[6];
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    debug_printf("WiFi: ready, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---- Saved network ----------------------------------------------------------

/// WIFI.CFG holds two lines, "ssid=..." and "password=...".
/// @return loaded
static bool load_config(void) {
    FIL f;
    if (f_open(&f, CONFIG_PATH, FA_READ) != FR_OK)
        return false;
    char line[100];
    ssid[0] = password[0] = 0;
    while (f_gets(line, sizeof line, &f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "ssid=", 5) == 0)
            snprintf(ssid, sizeof ssid, "%.32s", line + 5);
        else if (strncmp(line, "password=", 9) == 0)
            snprintf(password, sizeof password, "%.63s", line + 9);
    }
    f_close(&f);
    return ssid[0] != 0;
}

/// Saves WIFI.CFG.
/// @return saved
static bool save_config(void) {
    FIL f;
    if (f_open(&f, CONFIG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    bool ok = f_printf(&f, "ssid=%s\npassword=%s\n", ssid, password) > 0;
    return f_close(&f) == FR_OK && ok;
}

void net_drive_ready(void) {
    if (!radio || want || !load_config())
        return;
    debug_printf("WiFi: connecting to %s\n", ssid);
    want = true;
    start_connect();
}

// ---- Status messages --------------------------------------------------------

/// Prints the IP address, gateway and DNS server.
static void print_ip(void) {
    printf("IP %s", ip4addr_ntoa(netif_ip4_addr(sta())));
    printf(", gateway %s", ip4addr_ntoa(netif_ip4_gw(sta())));
    printf(", DNS %s", ipaddr_ntoa(dns_getserver(0)));
}

void net_task(void) {
    if (!radio)
        return;
    int s = link_status();
    if (s != reported) {
        bool up = s == CYW43_LINK_UP, was_up = reported == CYW43_LINK_UP;
        bool failed = s < 0, badauth = s == CYW43_LINK_BADAUTH;
        bool report = up || was_up || (failed && want);
        if (badauth)
            want = false;  // retrying won't help
        reported = s;
        if (up) {
            net_clock_start();
            debug_printf("WiFi: connected, gateway %s, DNS %s\n",
                         ip4addr_ntoa(netif_ip4_gw(sta())), ipaddr_ntoa(dns_getserver(0)));
        }
        if (failed)
            debug_printf("WiFi: can't connect to %s: %s\n", ssid, why(s));
        // Right after start-up the screen's WiFi line is updated in place;
        // otherwise a one-line message.
        if (report && (was_up || !shell_update_wifi_line())) {
            shell_async_begin();
            if (up)
                printf("[WiFi: connected to %s, %s]\n", ssid,
                       ip4addr_ntoa(netif_ip4_addr(sta())));
            else if (was_up)
                printf("[WiFi: lost %s%s]\n", ssid, want ? ", will retry" : "");
            else
                printf("[WiFi: can't connect to %s: %s%s]\n", ssid, why(s),
                       badauth ? "" : ", will retry");
            if (was_up)
                retry_at = make_timeout_time_ms(5000);
            shell_async_end();
        }
    }
    if (want && s != CYW43_LINK_UP && s != CYW43_LINK_JOIN && s != CYW43_LINK_NOIP &&
        time_reached(retry_at))
        start_connect();
    if (s == CYW43_LINK_UP)
        net_clock_task();
}

bool net_need_link(void) {
    if (!radio) {
        printf("No WiFi radio\n");
        return false;
    }
    if (link_status() != CYW43_LINK_UP) {
        printf("WiFi isn't connected (wifi connect SSID [password])\n");
        return false;
    }
    return true;
}

// ---- The wifi command -------------------------------------------------------

static struct {
    char ssid[33];
    int16_t rssi;  ///< its signal (dBm)
    uint8_t auth;  ///< its security
    uint8_t channel;  ///< its channel
} found[SCAN_MAX];  ///< the networks the scan found
/// Their count.
static volatile int found_count;

/// The scan's callback: adds or updates a network.
/// @param env unused
/// @param r the result
/// @return 0
static int on_scan(void *env, const cyw43_ev_scan_result_t *r) {
    if (!r->ssid_len)
        return 0;  // hidden network
    char name[33];
    memcpy(name, r->ssid, r->ssid_len);
    name[r->ssid_len] = 0;
    int i;
    for (i = 0; i < found_count; ++i)
        if (strcmp(found[i].ssid, name) == 0)
            break;
    if (i == found_count) {
        if (found_count == SCAN_MAX)
            return 0;
        strcpy(found[i].ssid, name);
        found[i].rssi = -999;
        found_count = i + 1;
    }
    if (r->rssi > found[i].rssi) {  // keep the strongest access point's details
        found[i].rssi = r->rssi;
        found[i].auth = r->auth_mode;
        found[i].channel = r->channel;
    }
    return 0;
}

/// qsort's comparison: the strongest signal first.
/// @param a a network
/// @param b another
/// @return less, equal or more than 0
static int by_signal(const void *a, const void *b) {
    return ((const typeof(found[0]) *)b)->rssi - ((const typeof(found[0]) *)a)->rssi;
}

/// wifi scan: lists the networks around.
static void scan(void) {
    found_count = 0;
    // Passive: listen for beacons instead of sending probe requests. The
    // radio transmitting in bursts made the USB drive drop off the hub.
    cyw43_wifi_scan_options_t opts = {.scan_type = 1};
    cyw43_arch_lwip_begin();
    int e = cyw43_wifi_scan(&cyw43_state, &opts, NULL, on_scan);
    cyw43_arch_lwip_end();
    if (e) {
        printf("Scan failed (%d)\n", e);
        return;
    }
    printf("Scanning...");
    fflush(stdout);
    while (cyw43_wifi_scan_active(&cyw43_state)) {
        if (net_stop_key()) {
            printf(" stopped\n");
            return;
        }
        sleep_ms(50);
    }
    printf("\r");
    qsort(found, found_count, sizeof found[0], by_signal);
    printf("%-32s  Signal  Ch  Security\n", "Network");
    for (int i = 0; i < found_count; ++i) {
        uint8_t a = found[i].auth;
        printf("%-32s  %4d dBm %2u  %s\n", found[i].ssid, found[i].rssi, found[i].channel,
               a == 0 ? "open" : a & 4 ? "WPA2" : a & 2 ? "WPA" : "WEP");
    }
    if (!found_count)
        printf("No networks found\n");
}

/// wifi: the connection's status.
static void status(void) {
    int s = link_status();
    if (s == CYW43_LINK_UP) {
        int32_t rssi = 0;
        cyw43_wifi_get_rssi(&cyw43_state, &rssi);
        printf("Connected to %s, signal %ld dBm\n", ssid, (long)rssi);
        print_ip();
        printf("\n");
    } else if (want && s >= CYW43_LINK_JOIN) {
        printf("Connecting to %s...\n", ssid);
    } else if (want) {
        printf("Not connected (%s: %s, retrying)\n", ssid, why(s));
    } else {
        printf("Not connected\n");
    }
}

/// Connects to a network, waiting (Esc stops) and saving it once connected.
/// @param name the network
/// @param pass its password, or NULL
static void connect(const char *name, const char *pass) {
    snprintf(ssid, sizeof ssid, "%s", name);
    snprintf(password, sizeof password, "%s", pass ? pass : "");
    if (link_status() != CYW43_LINK_DOWN)
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    want = true;
    start_connect();
    printf("Connecting to %s (Esc to stop)...", ssid);
    fflush(stdout);
    absolute_time_t until = make_timeout_time_ms(CONNECT_WAIT_MS);
    int s;
    // Give the old link time to report down before looking for the new one.
    sleep_ms(200);
    while ((s = link_status()) != CYW43_LINK_UP && s >= 0 && !time_reached(until)) {
        if (net_stop_key()) {
            printf(" stopped\n");
            want = false;
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            reported = link_status();
            return;
        }
        sleep_ms(50);
    }
    reported = s;  // net_task needn't say it again
    if (s != CYW43_LINK_UP) {
        printf(" %s\n", s < 0 ? why(s) : "timed out");
        if (s == CYW43_LINK_BADAUTH) {
            want = false;
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            reported = link_status();
        } else {
            printf("Will keep trying in the background (wifi off to stop)\n");
        }
        return;
    }
    printf(" connected\n");
    net_clock_start();
    print_ip();
    printf("\n");
    if (!shell_drive_mounted())
        printf("No USB drive, so this network isn't saved\n");
    else if (save_config())
        printf("Saved to WIFI.CFG (password in plain text); it connects at start-up\n");
    else
        printf("Couldn't save WIFI.CFG\n");
}

void net_cmd_wifi(int argc, char **argv) {
    if (!radio) {
        if (argc > 1 && strcmp(argv[1], "on") == 0) {
            net_init();
            if (radio && shell_drive_mounted())
                net_drive_ready();
        } else {
            printf("The WiFi radio isn't running (wifi on tries to start it)\n");
        }
        return;
    }
    const char *sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        status();
    } else if (strcmp(sub, "scan") == 0) {
        scan();
    } else if (strcmp(sub, "connect") == 0 && argc >= 3) {
        connect(argv[2], argc > 3 ? argv[3] : NULL);
    } else if (strcmp(sub, "off") == 0 || strcmp(sub, "disconnect") == 0) {
        want = false;
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        reported = link_status();
        printf("Disconnected\n");
    } else if (strcmp(sub, "on") == 0) {
        if (want || (shell_drive_mounted() && load_config())) {
            want = true;
            start_connect();
            printf("Connecting to %s\n", ssid);
        } else {
            printf("No saved network (wifi connect SSID [password])\n");
        }
    } else if (strcmp(sub, "wait") == 0) {
        // For scripts (/AUTORUN): until WiFi is up, at most this long.
        int secs = argc > 2 ? atoi(argv[2]) : 20;
        absolute_time_t until = make_timeout_time_ms((secs > 0 ? secs : 20) * 1000);
        int s;
        while ((s = link_status()) != CYW43_LINK_UP && !time_reached(until) && !net_stop_key())
            sleep_ms(50);
        reported = s;
        if (s == CYW43_LINK_UP)
            status();
        else
            printf("WiFi isn't connected (waited %d s)\n", secs > 0 ? secs : 20);
    } else if (strcmp(sub, "forget") == 0) {
        want = false;
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        reported = link_status();
        if (shell_drive_mounted())
            f_unlink(CONFIG_PATH);
        printf("Disconnected; WIFI.CFG removed\n");
    } else {
        printf("usage: wifi [status]           show the connection\n"
               "       wifi scan               list networks in range\n"
               "       wifi connect SSID [pw]  join and save (use \"quotes\" for spaces)\n"
               "       wifi off | on           disconnect / reconnect the saved network\n"
               "       wifi wait [seconds]     wait until connected (default 20 s)\n"
               "       wifi forget             disconnect and delete WIFI.CFG\n");
    }
}

void net_shutdown(void) {
    want = false;
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
}

void net_print_status(void) {
    int s = radio ? link_status() : 0;
    if (!radio)
        printf("\x1b[91mno radio found\x1b[0m");
    else if (s < 0 && ssid[0])
        printf("\x1b[91mcan't connect\x1b[0m to %s (%s)%s", ssid, why(s),
               want ? ", retrying" : "");
    else if (s == CYW43_LINK_UP)
        printf("\x1b[92mconnected\x1b[0m to %s, %s", ssid, ip4addr_ntoa(netif_ip4_addr(sta())));
    else if (want)
        printf("\x1b[93mconnecting\x1b[0m to %s", ssid);
    else
        printf("\x1b[90mnot set up (type wifi)\x1b[0m");
}
