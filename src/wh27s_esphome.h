#pragma once

#include "wh27s_uart.h"
#include "wh27s_diagnosis.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <string>

namespace wh27s {

inline std::string hex(const Bytes &bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (auto byte : bytes) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 15]);
  }
  return result;
}

class ResearchLink {
 public:
  StreamDecoder decoder;
  ReadSession session;
  TemperatureControl temperature;
  Telemetry telemetry;
  bool transmit_enabled{false};
  bool tx_available{true};
  const char *label{"UART"};
  uint32_t transmitted_frames{0};
  std::function<void(const std::string &)> on_raw;
  std::function<void(const std::string &)> on_diagnosis;
  std::function<void(const DiagnosisResult &)> on_diagnosis_decoded;
  std::function<void(uint16_t, uint32_t)> on_field;
  std::function<void(const std::vector<FirmwareRecord> &)> on_firmware;
  std::function<void(const std::vector<Field> &)> on_capabilities;
  std::function<void()> on_bridge_closed;
  std::function<void()> on_wifi_pairing;

  explicit ResearchLink(esphome::uart::UARTComponent *uart) : uart_(uart) {
    decoder.on_frame = [this](const Bytes &frame) { receive_(frame); };
    session.on_send = [this](const Bytes &frame) { send_(frame); };
    temperature.on_send = [this](const Bytes &frame) { send_(frame); };
  }

  void poll() {
    if (bridge_enabled()) {
      poll_bridge_();
      return;
    }
    unsigned budget = 256;
    while (budget-- != 0 && uart_->available() > 0) {
      uint8_t byte;
      if (!uart_->read_byte(&byte)) break;
      decoder.push(byte, esphome::millis());
    }
    if (uart_->available() == 0) decoder.tick(esphome::millis());
    if (tx_available && transmit_enabled) {
      temperature.tick(esphome::millis());
      if (!temperature.pending()) {
        session.tick(esphome::millis());
        if (session.stage() == ReadSession::Stage::READY && !firmware_requested_) {
          firmware_requested_ = true;
          send_(request(2, 2, 1, {0xE4, 0xC0}));
        } else if (session.stage() == ReadSession::Stage::READY &&
                   uint32_t(esphome::millis() - last_diagnosis_) >= DIAGNOSIS_INTERVAL_MS &&
                   uint32_t(esphome::millis() - last_tx_) >= 1500) {
          last_diagnosis_ = esphome::millis();
          send_(request(2, 2, 1, {0xE6, 0x00}));
        }
      }
    } else {
      temperature.cancel();
      session.stop();
    }
  }

  bool set_temperature(float celsius) {
    if (bridge_enabled() || !tx_available || !transmit_enabled || session.stage() != ReadSession::Stage::READY) {
      temperature.unavailable();
      ESP_LOGW("wh27s", "Temperature request refused: read session unavailable or busy");
      return false;
    }
    if (!temperature.set_temperature(celsius, esphome::millis())) {
      ESP_LOGW("wh27s", "Temperature request refused: %s", temperature.state_name());
      return false;
    }
    ESP_LOGI("wh27s", "Requested target %.1f C; preserving reported mode; awaiting heater report", celsius);
    return true;
  }

  bool set_mode(uint32_t mode) {
    if (bridge_enabled() || !tx_available || !transmit_enabled || session.stage() != ReadSession::Stage::READY) {
      temperature.unavailable(TemperatureControl::Kind::MODE);
      ESP_LOGW("wh27s", "Mode request refused: read session unavailable or busy");
      return false;
    }
    if (!temperature.set_mode(mode, esphome::millis())) {
      ESP_LOGW("wh27s", "Mode request refused: %s", temperature.state_name());
      return false;
    }
    ESP_LOGI("wh27s", "Requested mode %s; preserving reported target; awaiting heater report", mode_name(mode));
    return true;
  }

  bool set_schedule(bool on) {
    if (bridge_enabled() || !tx_available || !transmit_enabled || session.stage() != ReadSession::Stage::READY) {
      temperature.unavailable(TemperatureControl::Kind::SCHEDULE);
      ESP_LOGW("wh27s", "Schedule request refused: read session unavailable or busy");
      return false;
    }
    if (!temperature.set_schedule(on, esphome::millis())) {
      ESP_LOGW("wh27s", "Schedule request refused: %s", temperature.state_name());
      return false;
    }
    ESP_LOGI("wh27s", "Requested schedule indicator %s; awaiting heater report", on ? "on" : "off");
    return true;
  }

  bool set_operation(bool on) {
    if (bridge_enabled() || !tx_available || !transmit_enabled || session.stage() != ReadSession::Stage::READY) {
      temperature.unavailable(TemperatureControl::Kind::OPERATION);
      ESP_LOGW("wh27s", "Operation request refused: read session unavailable or busy");
      return false;
    }
    if (!temperature.set_operation(on, esphome::millis())) {
      ESP_LOGW("wh27s", "Operation request refused: %s", temperature.state_name());
      return false;
    }
    ESP_LOGI("wh27s", "Requested operation %s; awaiting heater report", on ? "on" : "off");
    return true;
  }

  void start_reads() {
    if (bridge_enabled() || !tx_available || !transmit_enabled || temperature.pending()) {
      ESP_LOGW("wh27s", "Read session refused: UART transmission is disabled");
      return;
    }
    ESP_LOGI("wh27s", "Starting captured discovery/capability/status read sequence");
    temperature.invalidate_status();
    session.start(esphome::millis());
  }

  void probe(uint8_t service, const Bytes &payload) {
    const uint32_t now = esphome::millis();
    if (bridge_enabled() || !tx_available || !transmit_enabled || temperature.pending()) {
      ESP_LOGW("wh27s", "Probe refused: UART transmission is disabled");
      return;
    }
    if (has_probed_ && uint32_t(now - last_probe_) < 1000) {
      ESP_LOGW("wh27s", "Probe refused: wait one second between requests");
      return;
    }
    has_probed_ = true;
    last_probe_ = now;
    send_(request(service, payload == Bytes{0xAA, 0x43} ? 1 : 2, 1, payload));
  }

  bool bridge_enabled() const { return bridge_listener_ != nullptr; }

  bool start_bridge() {
    if (bridge_enabled()) return true;
    if (!tx_available || !transmit_enabled || temperature.pending()) return false;
    auto listener = esphome::socket::socket_listen(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!listener) return false;
    int reuse = 1;
    listener->setsockopt(SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_storage address{};
    const auto length = esphome::socket::set_sockaddr(
        reinterpret_cast<struct sockaddr *>(&address), sizeof(address), "0.0.0.0", 6638);
    if (listener->setblocking(false) != 0 || length == 0 ||
        listener->bind(reinterpret_cast<struct sockaddr *>(&address), length) != 0 ||
        listener->listen(1) != 0) return false;
    session.stop();
    temperature.invalidate_status();
    bridge_started_ = bridge_activity_ = esphome::millis();
    bridge_pending_ = bridge_offset_ = 0;
    bridge_listener_ = std::move(listener);
    ESP_LOGI("wh27s", "Exclusive raw UART bridge enabled on TCP 6638; host owns ACKs; maximum 5 minutes");
    return true;
  }

  void stop_bridge() {
    if (!bridge_enabled()) return;
    bridge_client_.reset();
    bridge_listener_.reset();
    bridge_pending_ = bridge_offset_ = 0;
    uint8_t discarded[256];
    for (unsigned count = 0; count < 16 && uart_->available() > 0; ++count) {
      const auto length = std::min<size_t>(uart_->available(), sizeof(discarded));
      if (!uart_->read_array(discarded, length)) break;
    }
    decoder.tick(esphome::millis());
    ESP_LOGI("wh27s", "UART bridge closed; restoring normal read session");
    if (transmit_enabled) start_reads();
    if (on_bridge_closed) on_bridge_closed();
  }

 private:
  static constexpr uint32_t DIAGNOSIS_INTERVAL_MS = 60000;
  esphome::uart::UARTComponent *uart_;
  uint32_t last_probe_{0};
  uint32_t last_diagnosis_{0};
  uint32_t last_tx_{0};
  bool has_probed_{false};
  bool firmware_requested_{false};
  std::unique_ptr<esphome::socket::ListenSocket> bridge_listener_;
  std::unique_ptr<esphome::socket::Socket> bridge_client_;
  std::array<uint8_t, 256> bridge_buffer_{};
  size_t bridge_pending_{0};
  size_t bridge_offset_{0};
  uint32_t bridge_started_{0};
  uint32_t bridge_activity_{0};

  void poll_bridge_() {
    const auto now = esphome::millis();
    if (!transmit_enabled || uint32_t(now - bridge_started_) >= 300000 ||
        uint32_t(now - bridge_activity_) >= (bridge_client_ ? 15000U : 30000U)) {
      stop_bridge();
      return;
    }
    auto candidate = bridge_listener_->accept(nullptr, nullptr);
    if (candidate) {
      if (bridge_client_) {
        candidate->close();
      } else if (candidate->setblocking(false) == 0) {
        bridge_client_ = std::move(candidate);
        bridge_activity_ = now;
      }
    }
    if (!bridge_client_) return;
    uint8_t incoming[128];
    const auto received = bridge_client_->read(incoming, sizeof(incoming));
    if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      stop_bridge();
      return;
    }
    if (received > 0) {
      uart_->write_array(incoming, received);
      bridge_activity_ = now;
    }
    if (bridge_pending_ == bridge_offset_) {
      bridge_offset_ = bridge_pending_ = 0;
      const auto available = std::min<size_t>(uart_->available(), bridge_buffer_.size());
      if (available > 0 && uart_->read_array(bridge_buffer_.data(), available))
        bridge_pending_ = available;
    }
    if (bridge_pending_ == 0) return;
    const auto written = bridge_client_->write(bridge_buffer_.data() + bridge_offset_,
                                               bridge_pending_ - bridge_offset_);
    if (written > 0) {
      bridge_offset_ += written;
      bridge_activity_ = now;
    } else if (written == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      stop_bridge();
    }
  }

  void log_frame_(const char *direction, const Bytes &frame) {
    ESP_LOGI("wh27s", "%s %s t=%lu route=%02X service=%02X opcode=%02X seq=%02X bytes=%u",
         label, direction, static_cast<unsigned long>(esphome::millis()), frame[4], frame[5],
             frame[6], frame[7], static_cast<unsigned>(frame.size()));
    const auto raw = hex(frame);
    for (size_t offset = 0; offset < raw.size(); offset += 192)
      ESP_LOGI("wh27s", "%s %s hex[%u]=%s", label, direction, static_cast<unsigned>(offset / 2),
               raw.substr(offset, 192).c_str());
  }

  void send_(const Bytes &frame) {
    if (bridge_enabled() || !tx_available || !transmit_enabled) return;
    uart_->write_array(frame);
    ++transmitted_frames;
    last_tx_ = esphome::millis();
    log_frame_("TX", frame);
  }

  void receive_(const Bytes &frame) {
    log_frame_("RX", frame);
    if (on_raw) on_raw(hex(frame));
    if (frame[4] == 0x65) return;
    const auto payload = decode_payload(frame);
    if (!payload.valid) {
      ESP_LOGW("wh27s", "Malformed compact payload retained as raw frame");
      return;
    }
    telemetry.receive(payload);
    if (frame[5] == 2 && frame[6] == 1) {
      const auto records = firmware_records(payload);
      if (!records.empty() && on_firmware) on_firmware(records);
      if (on_capabilities && std::any_of(payload.fields.begin(), payload.fields.end(),
              [](const Field &field) { return field.type == 704; }))
        on_capabilities(payload.fields);
    }
    const bool eo_special = !payload.fields.empty() && payload.fields.front().type == 0x385;
    if ((frame[5] == 1 || frame[5] == 2 || frame[5] == 0xFC) &&
        (frame[6] == 1 || frame[6] == 4) && !eo_special)
      send_(request(frame[5], 0x10, frame[7], {}));
    const auto previous_command_state = temperature.state();
    temperature.receive(frame, esphome::millis());
    if (previous_command_state != temperature.state())
      ESP_LOGI("wh27s", "Control command: %s", temperature.state_name());
    for (const auto &field : payload.fields) {
      ESP_LOGI("wh27s", "TLV type=%u raw=%lu", field.type,
               static_cast<unsigned long>(field.value));
      if (on_field) on_field(field.type, field.value);
    }
    if (!payload.diagnosis.empty() && on_diagnosis) on_diagnosis(hex(payload.diagnosis));
    if (!payload.diagnosis.empty() && on_diagnosis_decoded)
      on_diagnosis_decoded(decode_smart_diagnosis(payload.diagnosis));
    if (tx_available && transmit_enabled) session.receive(frame, esphome::millis());
    // ACK above before scheduling Wi-Fi work; repeats are coalesced by the script.
    if (on_wifi_pairing && is_wifi_pairing_request(frame, payload)) on_wifi_pairing();
  }
};

inline ResearchLink *&research_link() {
  static ResearchLink *link = nullptr;
  return link;
}

inline ResearchLink *&monitor_link() {
  static ResearchLink *link = nullptr;
  return link;
}

}
