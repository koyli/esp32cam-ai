#include "wifi_extender.h"
#include "config.h"

#include <Arduino.h>

#ifndef EXTENDER_SSID

bool extenderEnabled() { return false; }
void extenderBegin() {}
int  extenderClients() { return 0; }

#else

#include <WiFi.h>
#include <esp_netif.h>
#include <lwip/opt.h>
#include <dhcpserver/dhcpserver.h>

#if ESP_ARDUINO_VERSION_MAJOR < 3
#include <lwip/lwip_napt.h>
#if !IP_NAPT
#error "This Arduino-ESP32 build has no lwIP NAPT (CONFIG_LWIP_IPV4_NAPT); the Wi-Fi extender needs it. Remove EXTENDER_SSID from config.h."
#endif
#endif

#ifndef EXTENDER_IP
#define EXTENDER_IP 192, 168, 71, 1
#endif
#ifndef EXTENDER_MAX_CLIENTS
#define EXTENDER_MAX_CLIENTS 4
#endif

static_assert(sizeof(EXTENDER_PASS) - 1 >= 8, "EXTENDER_PASS must be at least 8 characters (WPA2)");

bool extenderEnabled() { return true; }

int extenderClients() { return WiFi.softAPgetStationNum(); }

// The soft AP's DHCP server would otherwise hand out the AP's own address as
// the DNS server, and nothing answers DNS there. Offer the upstream resolver.
static void offerDns(IPAddress dns) {
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return;

    esp_netif_dns_info_t info = {};
    info.ip.type = ESP_IPADDR_TYPE_V4;
    info.ip.u_addr.ip4.addr = (uint32_t)dns;  // IPAddress and lwIP both hold network byte order

    dhcps_offer_t offer = OFFER_DNS;
    esp_netif_dhcps_stop(ap);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &info);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_dhcps_start(ap);
}

void extenderBegin() {
    const bool    up = WiFi.status() == WL_CONNECTED;
    const uint8_t ch = up ? WiFi.channel() : 1;  // the AP must share the station's channel
    const IPAddress ap_ip(EXTENDER_IP);
    const IPAddress mask(255, 255, 255, 0);

    if (!WiFi.softAP(EXTENDER_SSID, EXTENDER_PASS, ch, 0, EXTENDER_MAX_CLIENTS)) {
        Serial.println("extender: soft AP start failed");
        return;
    }
    WiFi.softAPConfig(ap_ip, ap_ip, mask);

    IPAddress dns = up ? WiFi.dnsIP(0) : IPAddress(0, 0, 0, 0);
    if ((uint32_t)dns == 0) dns = IPAddress(1, 1, 1, 1);
    offerDns(dns);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    bool nat = WiFi.AP.enableNAPT(true);
#else
    ip_napt_enable((uint32_t)WiFi.softAPIP(), 1);
    bool nat = true;
#endif

    Serial.printf("extender: AP \"%s\" ch%u %s, dns %s, nat %s%s\n", EXTENDER_SSID, ch,
                  WiFi.softAPIP().toString().c_str(), dns.toString().c_str(),
                  nat ? "on" : "FAILED", up ? "" : " (no upstream yet)");

    if (up && ((uint32_t)WiFi.localIP() & (uint32_t)mask) == ((uint32_t)ap_ip & (uint32_t)mask)) {
        Serial.println("extender: WARNING upstream network uses the same subnet as EXTENDER_IP; "
                       "pick a different EXTENDER_IP or clients won't be routed");
    }
}

#endif
