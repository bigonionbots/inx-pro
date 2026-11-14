/**
 * @file DnsFallback.cpp
 * @brief Public DNS servers behind the ones Wi-Fi networks hand out (see DnsFallback.h).
 */
#include "DnsFallback.h"

#ifndef SIMULATOR

#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>

namespace DnsFallback {
namespace {

constexpr uint32_t kCloudflare = ESP_IP4TOADDR(1, 1, 1, 1);
constexpr uint32_t kGoogle = ESP_IP4TOADDR(8, 8, 8, 8);

bool isEmpty(const esp_netif_dns_info_t& dns) {
  return dns.ip.type == ESP_IPADDR_TYPE_V4 ? dns.ip.u_addr.ip4.addr == 0 : false;
}

void setServer(esp_netif_t* netif, const esp_netif_dns_type_t slot, const uint32_t address) {
  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = address;
  esp_netif_set_dns_info(netif, slot, &dns);
}

esp_netif_dns_info_t server(esp_netif_t* netif, const esp_netif_dns_type_t slot) {
  esp_netif_dns_info_t dns = {};
  if (esp_netif_get_dns_info(netif, slot, &dns) != ESP_OK) {
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = 0;
  }
  return dns;
}

void onGotIp(arduino_event_id_t, arduino_event_info_t) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) return;
  // An empty main server makes lwIP give up at once instead of trying the next one, so fill it.
  if (isEmpty(server(netif, ESP_NETIF_DNS_MAIN))) setServer(netif, ESP_NETIF_DNS_MAIN, kCloudflare);
  if (isEmpty(server(netif, ESP_NETIF_DNS_BACKUP))) setServer(netif, ESP_NETIF_DNS_BACKUP, kCloudflare);
  setServer(netif, ESP_NETIF_DNS_FALLBACK, kGoogle);
  INX_SERIAL.printf("[%lu] [DNS] servers %s, %s, %s\n", millis(), WiFi.dnsIP(0).toString().c_str(),
                    WiFi.dnsIP(1).toString().c_str(), WiFi.dnsIP(2).toString().c_str());
}

}  // namespace

void begin() { WiFi.onEvent(onGotIp, ARDUINO_EVENT_WIFI_STA_GOT_IP); }

}  // namespace DnsFallback

#else

namespace DnsFallback {
void begin() {}
}  // namespace DnsFallback

#endif  // SIMULATOR
