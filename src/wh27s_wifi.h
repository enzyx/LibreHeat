#pragma once

#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "esphome/components/wifi/wifi_component.h"
#include <string>
#ifdef USE_WEBSERVER_AUTH
#include "esphome/components/web_server_base/web_server_base.h"
#endif
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif
#ifdef USE_ESP32
#include <esp_wifi.h>
#include <cstring>
#endif

namespace wh27s {

// ESPHome has no public start-AP action. Use ESP-IDF's AP+STA mode on the
// default AP netif that ESPHome creates, retaining its station and credentials.
class WiFiPairingAccess {
 public:
  // Keep the configured credentials so recovery never permanently clears them.
  static void configure_auth(const std::string &username, const std::string &password) {
#if defined(USE_ESP32) && defined(USE_WEBSERVER_AUTH)
    auth_username_ = username;
    auth_password_ = password;
    auth_configured_ = true;
#else
    (void) username;
    (void) password;
#endif
  }

  static bool begin() {
#if defined(USE_ESP32) && defined(USE_WIFI_AP) && defined(USE_CAPTIVE_PORTAL)
    auto *wifi = esphome::wifi::global_wifi_component;
    auto *portal = esphome::captive_portal::global_captive_portal;
    if (wifi == nullptr || portal == nullptr || !wifi->has_ap() || wifi->is_disabled()) {
      ESP_LOGW("wh27s", "Wi-Fi setup requires enabled Wi-Fi, fallback AP and captive_portal");
      return false;
    }
    if (portal->is_active()) return false;
#ifdef USE_WEBSERVER_AUTH
    if (!auth_configured_) {
      ESP_LOGW("wh27s", "Configure recovery authentication before opening setup");
      return false;
    }
#endif
    const auto ap = wifi->get_ap();
    wifi_config_t config{};
    const auto ssid = ap.get_ssid();
    const auto password = ap.get_password();
    if (ssid.empty() || ssid.size() > sizeof(config.ap.ssid) ||
        password.size() > sizeof(config.ap.password)) return false;
    std::memcpy(config.ap.ssid, ssid.c_str(), ssid.size());
    config.ap.ssid_len = ssid.size();
    std::memcpy(config.ap.password, password.c_str(), password.size());
    config.ap.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.ap.pairwise_cipher = password.empty() ? WIFI_CIPHER_TYPE_NONE : WIFI_CIPHER_TYPE_CCMP;
    config.ap.channel = ap.has_channel() ? ap.get_channel() : 1;
    config.ap.max_connection = 5;
    config.ap.beacon_interval = 100;
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return false;
    if (esp_wifi_set_mode(static_cast<wifi_mode_t>(mode | WIFI_MODE_AP)) != ESP_OK) return false;
    if (esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK) {
      esp_wifi_set_mode(mode);
      ESP_LOGW("wh27s", "Could not configure Wi-Fi setup AP");
      return false;
    }
    initial_sta_ = wifi->get_sta();
    owned_ = true;
    set_recovery_auth_(true);
    portal->start();
    ESP_LOGW("wh27s", "Panel Wi-Fi button: setup AP opened for 5 minutes");
    return true;
#else
    ESP_LOGW("wh27s", "Panel Wi-Fi button: configure wifi.ap and captive_portal to enable setup");
    return false;
#endif
  }

  static void poll() {
#if defined(USE_ESP32) && defined(USE_WIFI_AP) && defined(USE_CAPTIVE_PORTAL)
    if (!owned_) return;
    auto *wifi = esphome::wifi::global_wifi_component;
    auto *portal = esphome::captive_portal::global_captive_portal;
    if (!portal->is_active()) {
      set_recovery_auth_(false);
      owned_ = false;
      return;
    }
    const auto sta = wifi->get_sta();
    // The captive portal saves credentials, but ESPHome's save method does
    // not reconnect an already-connected station. Apply an actual edit here.
    if (sta.get_ssid() != initial_sta_.get_ssid() ||
        sta.get_password() != initial_sta_.get_password()) {
      initial_sta_ = sta;
      wifi->start_connecting(sta);
    }
#endif
  }

  static void finish() {
#if defined(USE_ESP32) && defined(USE_WIFI_AP) && defined(USE_CAPTIVE_PORTAL)
    if (!owned_) return;
    owned_ = false;
    set_recovery_auth_(false);
    auto *wifi = esphome::wifi::global_wifi_component;
    auto *portal = esphome::captive_portal::global_captive_portal;
    // End even when the router is unavailable. Nothing is erased or saved here.
    if (wifi == nullptr || portal == nullptr) return;
    if (portal->is_active()) portal->end();
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK)
      esp_wifi_set_mode(static_cast<wifi_mode_t>(mode & ~WIFI_MODE_AP));
    ESP_LOGW("wh27s", "Panel Wi-Fi setup window closed");
#endif
  }

 private:
  static void set_recovery_auth_(bool recovery) {
#if defined(USE_ESP32) && defined(USE_WEBSERVER_AUTH)
    auto *base = esphome::web_server_base::global_web_server_base;
    if (base == nullptr || !auth_configured_) return;
    // ESP32's HTTP server accepts requests without authentication for an empty
    // username, including handlers already wrapped in auth middleware (OTA).
    base->set_auth_username(recovery ? "" : auth_username_.c_str());
    base->set_auth_password(auth_password_.c_str());
#else
    (void) recovery;
#endif
  }

#if defined(USE_ESP32) && defined(USE_WEBSERVER_AUTH)
  static inline std::string auth_username_;
  static inline std::string auth_password_;
  static inline bool auth_configured_{false};
#endif
  static inline bool owned_{false};
  static inline esphome::wifi::WiFiAP initial_sta_;
};

}  // namespace wh27s
