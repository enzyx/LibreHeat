"""Exercise the recovery adapter with fake ESPHome/ESP-IDF services."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADERS = [
    "esphome/core/defines.h", "esphome/core/log.h",
    "esphome/components/wifi/wifi_component.h",
    "esphome/components/web_server_base/web_server_base.h",
    "esphome/components/captive_portal/captive_portal.h", "esp_wifi.h",
]
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <string>
#define USE_ESP32 1
#define USE_WIFI_AP
#define USE_CAPTIVE_PORTAL
#ifndef TEST_NO_AUTH
#define USE_WEBSERVER_AUTH
#endif
#define ESP_LOGW(...) ((void)0)
namespace esphome {
namespace wifi {
struct WiFiAP {
  std::string ssid{"recovery"}, password;
  std::string get_ssid() const { return ssid; }
  std::string get_password() const { return password; }
  bool has_channel() const { return false; }
  int get_channel() const { return 1; }
};
struct WiFiComponent {
  WiFiAP ap, sta{"original-router", "original-password"};
  bool disabled{false}, connected{true};
  int connects{0};
  bool has_ap() const { return true; }
  bool is_disabled() const { return disabled; }
  bool is_connected() const { return connected; }
  WiFiAP get_ap() const { return ap; }
  WiFiAP get_sta() const { return sta; }
  void start_connecting(const WiFiAP &) { ++connects; }
};
inline WiFiComponent instance;
inline auto *global_wifi_component = &instance;
}
namespace web_server_base {
struct WebServerBase {
  const char *username{"admin"}, *password{"secret"};
  void set_auth_username(const char *value) { username = value; }
  void set_auth_password(const char *value) { password = value; }
};
inline WebServerBase instance;
inline auto *global_web_server_base = &instance;
}
namespace captive_portal {
struct CaptivePortal {
  bool active{false};
  bool is_active() const { return active; }
  void start() { active = true; }
  void end() { active = false; }
};
inline CaptivePortal instance;
inline auto *global_captive_portal = &instance;
}
}
using wifi_mode_t = int;
constexpr int WIFI_MODE_AP = 2, WIFI_IF_AP = 1, ESP_OK = 0;
constexpr int WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 1;
constexpr int WIFI_CIPHER_TYPE_NONE = 0, WIFI_CIPHER_TYPE_CCMP = 1;
struct wifi_config_t {
  struct {
    unsigned char ssid[32]{}, password[64]{};
    int ssid_len{}, authmode{}, pairwise_cipher{}, channel{}, max_connection{}, beacon_interval{};
  } ap;
};
int radio_mode = 1;
bool fail_config = false;
int esp_wifi_get_mode(wifi_mode_t *mode) { *mode = radio_mode; return ESP_OK; }
int esp_wifi_set_mode(wifi_mode_t mode) { radio_mode = mode; return ESP_OK; }
int esp_wifi_set_config(int, wifi_config_t *config) {
  assert(config->ap.authmode == WIFI_AUTH_OPEN);
  return fail_config ? -1 : ESP_OK;
}
#include "src/wh27s_wifi.h"
int main() {
  using Recovery = wh27s::WiFiPairingAccess;
  auto &wifi = esphome::wifi::instance;
  auto &portal = esphome::captive_portal::instance;
  auto &web = esphome::web_server_base::instance;
#ifdef USE_WEBSERVER_AUTH
  assert(!Recovery::begin()); // Never bypass auth without restoration credentials.
#endif
  Recovery::configure_auth("admin", "secret");
  assert(Recovery::begin());
  assert(portal.active && radio_mode == 3);
#ifdef USE_WEBSERVER_AUTH
  assert(std::string(web.username).empty());
#endif
  assert(!Recovery::begin()); // Repeated packets do not restart recovery.
  Recovery::poll();
  assert(wifi.connects == 0);
  Recovery::finish(); // Same call the script makes after its five-minute delay.
  assert(!portal.active && radio_mode == 1);
  assert(wifi.sta.ssid == "original-router" && wifi.sta.password == "original-password");
  assert(std::string(web.username) == "admin" && std::string(web.password) == "secret");
  assert(wifi.connects == 0);

  wifi.connected = false;
  assert(Recovery::begin());
  Recovery::finish(); // Offline timeout must close too.
  assert(!portal.active && radio_mode == 1);
  assert(std::string(web.username) == "admin");
  assert(wifi.sta.ssid == "original-router" && wifi.sta.password == "original-password");

  assert(Recovery::begin());
  wifi.sta = {"new-router", "new-password"};
  Recovery::poll();
  Recovery::poll();
  assert(wifi.connects == 1); // Only actual credential edits cause reconnects.
  portal.end(); // ESPHome closes the portal on a successful reconnection.
  radio_mode = 1;
  Recovery::poll();
  assert(std::string(web.username) == "admin");
  Recovery::finish();
  assert(wifi.sta.ssid == "new-router" && wifi.sta.password == "new-password");

  fail_config = true;
  assert(!Recovery::begin());
  assert(radio_mode == 1 && !portal.active);
  assert(std::string(web.username) == "admin");
}
'''

with tempfile.TemporaryDirectory(prefix="wh27s-wifi-") as directory:
    temporary = Path(directory)
    for header in HEADERS:
        path = temporary / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#pragma once\n")
    source = temporary / "test.cpp"
    source.write_text(HARNESS)
    for flags in ([], ["-DTEST_NO_AUTH"]):
        executable = temporary / "test"
        subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            *flags, "-I", str(temporary), "-I", str(ROOT), str(source),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
print("Recovery lifecycle tests passed (with and without web authentication)")
