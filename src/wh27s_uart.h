#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace wh27s {

using Bytes = std::vector<uint8_t>;

inline uint16_t crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0;
  for (size_t index = 0; index < length; ++index) {
    crc ^= uint16_t(data[index]) << 8;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

inline bool extended(uint8_t service) { return service >= 0xF0 && service <= 0xF9; }

inline Bytes request(uint8_t service, uint8_t opcode, uint8_t sequence,
                     const Bytes &payload) {
  Bytes frame{0x04, 0, 0, 0, 0x65, service, opcode, sequence};
  if (extended(service)) frame.push_back(payload.size() >> 8);
  frame.push_back(payload.size());
  frame.insert(frame.end(), payload.begin(), payload.end());
  const auto crc = crc16(frame.data(), frame.size());
  frame.push_back(crc >> 8);
  frame.push_back(crc & 0xFF);
  return frame;
}

struct Field {
  uint16_t type;
  uint32_t value;
};

struct Payload {
  bool valid{true};
  std::vector<Field> fields;
  Bytes diagnosis;
};

inline Payload decode_payload(const Bytes &frame) {
  Payload result;
  if (frame.size() < 11 || (frame[5] != 1 && frame[5] != 2 && frame[5] != 0xFC) ||
      (frame[6] != 1 && frame[6] != 4)) return result;
  const size_t end = frame.size() - 2;
  size_t cursor = 9;
  while (cursor < end) {
    if (end - cursor < 2) { result.valid = false; break; }
    const uint16_t tag = (uint16_t(frame[cursor]) << 8) | frame[cursor + 1];
    cursor += 2;
    const size_t width = (tag >> 4) & 3;
    if (width > end - cursor) { result.valid = false; break; }
    uint32_t value = tag & 15;
    if (width != 0) {
      value = 0;
      for (size_t index = 0; index < width; ++index) value = (value << 8) | frame[cursor++];
    }
    const uint16_t type = tag >> 6;
    result.fields.push_back({type, value});
    if (type == 0x399) {
      if (value != end - cursor) { result.valid = false; break; }
      result.diagnosis.assign(frame.begin() + cursor, frame.begin() + end);
      break;
    }
  }
  return result;
}

// Captured on 2026-11-05 after each three-second panel Wi-Fi hold.
inline bool is_wifi_pairing_request(const Bytes &frame, const Payload &payload) {
  return frame.size() >= 11 && frame[4] == 0x87 && frame[5] == 1 && frame[6] == 1 &&
         payload.valid && std::any_of(payload.fields.begin(), payload.fields.end(),
             [](const Field &field) { return field.type == 681 && field.value == 1; });
}

struct FirmwareRecord {
  uint32_t type{0};
  uint16_t high{0};
  uint16_t low{0};
  uint16_t checksum{0};

  std::string part_number() const {
    char text[12];
    std::snprintf(text, sizeof(text), "SAA%04X%04X", unsigned(high), unsigned(low));
    return text;
  }

  std::string checksum_hex() const {
    char text[5];
    std::snprintf(text, sizeof(text), "%04X", unsigned(checksum));
    return text;
  }
};

inline std::vector<FirmwareRecord> firmware_records(const Payload &payload) {
  std::vector<FirmwareRecord> records;
  if (!payload.valid) return records;
  FirmwareRecord current;
  unsigned present = 0;
  for (const auto &field : payload.fields) {
    if (field.type == 916) {
      current = FirmwareRecord{};
      current.type = field.value;
      present = 1;
    } else if (present != 0 && field.type >= 917 && field.type <= 919) {
      if (field.value > 0xFFFF) { present = 0; continue; }
      if (field.type == 917) { current.high = field.value; present |= 2; }
      if (field.type == 918) { current.low = field.value; present |= 4; }
      if (field.type == 919) { current.checksum = field.value; present |= 8; }
      if (present == 15) {
        records.push_back(current);
        present = 0;
      }
    }
  }
  return records;
}

inline std::string field_list(const std::vector<Field> &fields) {
  std::string text;
  for (const auto &field : fields) {
    if (!text.empty()) text += "; ";
    text += std::to_string(field.type) + "=" + std::to_string(field.value);
  }
  return text;
}

class Telemetry {
 public:
  void receive(const Payload &payload) {
    if (!payload.valid) return;
    for (const auto &field : payload.fields) {
      auto existing = std::find_if(values_.begin(), values_.end(),
          [&](const Field &entry) { return entry.type == field.type; });
      if (existing == values_.end()) values_.push_back(field);
      else existing->value = field.value;
    }
  }

  float value(uint16_t type) const {
    for (const auto &field : values_)
      if (field.type == type) return static_cast<float>(field.value);
    return NAN;
  }

 private:
  std::vector<Field> values_;
};

class ReadSession {
 public:
  enum class Stage { IDLE, DISCOVERY, CAPABILITIES, STATUS, READY, FAILED };
  static constexpr uint32_t RESPONSE_TIMEOUT_MS = 2000;
  static constexpr uint32_t POLL_INTERVAL_MS = 30000;
  static constexpr unsigned MAX_ATTEMPTS = 3;
  std::function<void(const Bytes &)> on_send;
  Stage stage() const { return stage_; }

  void start(uint32_t now) { enter_(Stage::DISCOVERY, now); }
  void stop() { stage_ = Stage::IDLE; }

  void receive(const Bytes &frame, uint32_t now) {
    if (frame.size() < 11) return;
    if (stage_ == Stage::DISCOVERY && frame[4] == 0x87 && frame[5] == 1 && frame[6] == 0x10) {
      enter_(Stage::CAPABILITIES, now);
      return;
    }
    if ((frame[4] != 0xA7 && frame[4] != 0xA8) || frame[5] != 2) return;
    const auto payload = decode_payload(frame);
    if (!payload.valid) return;
    for (const auto &field : payload.fields) {
      if (stage_ == Stage::CAPABILITIES && frame[6] == 1 && field.type == 704) {
        enter_(Stage::STATUS, now);
        return;
      }
      if (stage_ == Stage::STATUS && frame[6] == 4 &&
          (field.type == 503 || field.type == 505 || field.type == 597 || field.type == 598)) {
        stage_ = Stage::READY;
        last_action_ = now;
        return;
      }
    }
  }

  void tick(uint32_t now) {
    if (stage_ == Stage::READY) {
      if (uint32_t(now - last_action_) >= POLL_INTERVAL_MS) enter_(Stage::STATUS, now);
      return;
    }
    if (stage_ == Stage::IDLE || stage_ == Stage::FAILED ||
        uint32_t(now - last_action_) < RESPONSE_TIMEOUT_MS) return;
    if (attempts_ >= MAX_ATTEMPTS) {
      stage_ = Stage::FAILED;
      return;
    }
    send_(now);
  }

 private:
  Stage stage_{Stage::IDLE};
  uint32_t last_action_{0};
  unsigned attempts_{0};

  void enter_(Stage stage, uint32_t now) {
    stage_ = stage;
    attempts_ = 0;
    send_(now);
  }

  void send_(uint32_t now) {
    last_action_ = now;
    ++attempts_;
    if (!on_send) return;
    if (stage_ == Stage::DISCOVERY) on_send(request(1, 1, 1, {0xAA, 0x43}));
    else if (stage_ == Stage::CAPABILITIES) on_send(request(2, 2, 1, {0x7D, 0x41}));
    else if (stage_ == Stage::STATUS) on_send(request(2, 2, 2, {0x7D, 0x42}));
  }
};

inline const char *mode_name(uint32_t mode) {
  switch (mode) {
    case 25: return "Heat Pump (Eco)";
    case 26: return "Auto";
    case 27: return "Turbo";
    case 28: return "Vacation";
    default: return nullptr;
  }
}

// App model codes 9/11-17 (support.airState.notification.detailList / Push categories).
inline std::string notification_name(uint32_t code) {
  switch (code) {
    case 0: return "None";
    case 9: return "Filter cleaning";
    case 11: return "Drain check";
    case 12: return "T&P valve check";
    case 13: return "System error";
    case 14: return "Power saving";
    case 15: return "Water leak";
    case 16: return "Temperature sensor error";
    case 17: return "Water not enough";
    default: return "Unknown (" + std::to_string(code) + ")";
  }
}

struct CodeLabel {
  uint32_t code;
  const char *label;
};

// Field 545 shown as the panel's numeric code with its fault label (value==display code is assumed).
inline std::string error_code_name(uint32_t code) {
  static const CodeLabel labels[] = {
      {1, "Ambient air temperature sensor"}, {4, "Float switch open"},
      {5, "Communication error"}, {6, "Lower tank temperature sensor"},
      {9, "EEPROM error"}, {12, "Mid-pipe temperature sensor"},
      {19, "Upper heating element"}, {20, "Lower heating element"},
      {21, "DC peak / IPM fault"}, {22, "CT2 maximum current"},
      {23, "DC-link voltage"}, {26, "Compressor position detection"},
      {27, "PSC fault"}, {29, "Compressor phase overcurrent"},
      {32, "Discharge pipe overheat"}, {41, "Discharge pipe temperature sensor"},
      {46, "Suction pipe temperature sensor"}, {61, "Condenser pipe temperature high"},
      {65, "Heat sink temperature sensor"}, {67, "Fan motor locked"}};
  if (code == 0) return "None";
  char number[12];
  std::snprintf(number, sizeof(number), "%02u", static_cast<unsigned>(code));
  for (const auto &entry : labels)
    if (entry.code == code) return std::string(number) + " " + entry.label;
  return std::string(number) + " Unknown";
}

// Capability fields carry model enum code N as bit N-1 (e.g. 756=0x1FD00 -> codes 9,11..17).
inline std::string capability_labels(float raw, std::initializer_list<CodeLabel> labels) {
  if (!std::isfinite(raw) || raw < 0) return {};
  const uint32_t mask = static_cast<uint32_t>(raw);
  std::string text;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    if (!(mask & (1UL << bit))) continue;
    const char *label = nullptr;
    for (const auto &entry : labels)
      if (entry.code == bit + 1) label = entry.label;
    if (!text.empty()) text += "; ";
    text += label != nullptr ? std::string(label) : "Code " + std::to_string(bit + 1);
  }
  return text.empty() ? "None" : text;
}

class TemperatureControl {
 public:
  enum class State { IDLE, PENDING, CONFIRMED, INVALID, INVALID_MODE, STALE, UNAVAILABLE, UNCONFIRMED, CANCELLED };
  enum class Kind { TEMPERATURE, MODE, SCHEDULE, OPERATION, UNIT };
  static constexpr uint32_t MAX_STATUS_AGE_MS = 60000;
  static constexpr uint32_t READBACK_DELAY_MS = 1000;
  static constexpr uint32_t CONFIRM_TIMEOUT_MS = 5000;
  std::function<void(const Bytes &)> on_send;

  State state() const { return state_; }
  bool pending() const { return state_ == State::PENDING; }
  bool is_mode_request() const { return kind_ == Kind::MODE; }
  Kind kind() const { return kind_; }
  float requested_temperature() const { return requested_raw_ / 2.0f; }

  const char *state_name() const { return state_name_(state_); }
  const char *temperature_state_name() const { return state_name_(temperature_state_); }
  const char *mode_state_name() const { return state_name_(mode_state_); }
  const char *schedule_state_name() const { return state_name_(schedule_state_); }
  const char *operation_state_name() const { return state_name_(operation_state_); }
  const char *unit_state_name() const { return state_name_(unit_state_); }

  static const char *state_name_(State state) {
    switch (state) {
      case State::IDLE: return "Idle";
      case State::PENDING: return "Pending";
      case State::CONFIRMED: return "Confirmed";
      case State::INVALID: return "Rejected: invalid temperature";
      case State::INVALID_MODE: return "Rejected: unsupported mode";
      case State::STALE: return "Rejected: no fresh status";
      case State::UNAVAILABLE: return "Unavailable";
      case State::UNCONFIRMED: return "Not confirmed";
      case State::CANCELLED: return "Cancelled";
    }
    return "Unknown";
  }

  void unavailable(Kind kind = Kind::TEMPERATURE) {
    if (!pending()) {
      kind_ = kind;
      set_state_(State::UNAVAILABLE);
    }
  }

  void cancel() {
    if (pending()) set_state_(State::CANCELLED);
  }

  void invalidate_status() {
    mode_known_ = false;
    target_known_ = false;
    schedule_known_ = false;
    operation_known_ = false;
    unit_known_ = false;
    cancel();
  }

  bool set_temperature(float celsius, uint32_t now) {
    if (pending()) return false;
    kind_ = Kind::TEMPERATURE;
    const float scaled = celsius * 2.0f;
    if (!std::isfinite(celsius) || celsius < 35.0f || celsius > 60.0f ||
        scaled != std::round(scaled)) {
      set_state_(State::INVALID);
      return false;
    }
    if (!mode_known_ || !target_known_ || mode_ < 25 || mode_ > 28 ||
        uint32_t(now - mode_at_) > MAX_STATUS_AGE_MS ||
        uint32_t(now - target_at_) > MAX_STATUS_AGE_MS) {
      set_state_(State::STALE);
      return false;
    }
    requested_raw_ = static_cast<uint8_t>(scaled);
    return begin_(request(2, 1, 0, {0x7E, 0x50, mode_, 0x95, 0x90, requested_raw_}), now);
  }

  // Field 649 lights the panel's "Schedule" indicator; live-verified 2026-11-03.
  bool set_schedule(bool on, uint32_t now) {
    if (pending()) return false;
    kind_ = Kind::SCHEDULE;
    if (!schedule_known_ || uint32_t(now - schedule_at_) > MAX_STATUS_AGE_MS) {
      set_state_(State::STALE);
      return false;
    }
    requested_schedule_ = on ? 1 : 0;
    return begin_(request(2, 1, 0, {0xA2, uint8_t(0x40 | requested_schedule_)}), now);
  }

  // Stock capture 2026-11-03: the heater honours 503=0 only while 649=1; on = 503+505+510 (cloud frame).
  bool set_operation(bool on, uint32_t now) {
    if (pending()) return false;
    kind_ = Kind::OPERATION;
    if (!operation_known_ || !schedule_known_ || !mode_known_ || !target_known_ ||
        mode_name(mode_) == nullptr ||
        uint32_t(now - operation_at_) > MAX_STATUS_AGE_MS ||
        uint32_t(now - schedule_at_) > MAX_STATUS_AGE_MS ||
        uint32_t(now - mode_at_) > MAX_STATUS_AGE_MS ||
        uint32_t(now - target_at_) > MAX_STATUS_AGE_MS) {
      set_state_(State::STALE);
      return false;
    }
    if (target_raw_ < 70 || target_raw_ > 120) {
      set_state_(State::INVALID);
      return false;
    }
    requested_operation_ = on ? 1 : 0;
    if (on)
      return begin_(request(2, 1, 0, {0x7D, 0xC1, 0x7E, 0x50, mode_, 0x7F, 0x90, target_raw_}), now);
    off_needs_schedule_ = schedule_ != 1;
    return begin_(off_needs_schedule_ ? request(2, 1, 0, {0xA2, 0x41}) : request(2, 1, 0, {0x7D, 0xC0}), now);
  }

  // Panel reports: 508=0 Celsius, 508=1 Fahrenheit (2026-11-05).
  // Write uses the ordinary service-02 SET shape; require report readback.
  bool toggle_unit(uint32_t now) {
    if (pending()) return false;
    kind_ = Kind::UNIT;
    if (!unit_known_ || uint32_t(now - unit_at_) > MAX_STATUS_AGE_MS) {
      set_state_(State::STALE);
      return false;
    }
    requested_unit_ = unit_ ^ 1;
    return begin_(request(2, 1, 0, {0x7F, uint8_t(requested_unit_ ? 0x01 : 0x00)}), now);
  }

  bool set_mode(uint32_t mode, uint32_t now) {
    if (pending()) return false;
    kind_ = Kind::MODE;
    if (mode_name(mode) == nullptr) {
      set_state_(State::INVALID_MODE);
      return false;
    }
    if (!mode_known_ || !target_known_ ||
        uint32_t(now - mode_at_) > MAX_STATUS_AGE_MS ||
        uint32_t(now - target_at_) > MAX_STATUS_AGE_MS) {
      set_state_(State::STALE);
      return false;
    }
    if (target_raw_ < 70 || target_raw_ > 120) {
      set_state_(State::INVALID);
      return false;
    }
    requested_mode_ = static_cast<uint8_t>(mode);
    return begin_(request(2, 1, 0, {0x7E, 0x50, requested_mode_, 0x95, 0x90, target_raw_}), now);
  }

  void receive(const Bytes &frame, uint32_t now) {
    if (frame.size() < 11 || (frame[4] != 0xA7 && frame[4] != 0xA8) ||
        frame[5] != 2 || frame[6] != 4) return;
    const auto payload = decode_payload(frame);
    if (!payload.valid) return;
    for (const auto &field : payload.fields) {
      if (field.type == 649) {
        schedule_known_ = field.value <= 1;
        schedule_ = static_cast<uint8_t>(field.value);
        schedule_at_ = now;
        if (pending() && kind_ == Kind::SCHEDULE && field.value == requested_schedule_)
          set_state_(State::CONFIRMED);
        if (pending() && kind_ == Kind::OPERATION && off_needs_schedule_ && field.value == 1) {
          off_needs_schedule_ = false;
          begin_(request(2, 1, 0, {0x7D, 0xC0}), now);
        }
      } else if (field.type == 503) {
        operation_known_ = field.value <= 1;
        operation_at_ = now;
        if (pending() && kind_ == Kind::OPERATION && !off_needs_schedule_ &&
            field.value == requested_operation_)
          set_state_(State::CONFIRMED);
      } else if (field.type == 505) {
        mode_known_ = field.value <= 255;
        mode_ = static_cast<uint8_t>(field.value);
        mode_at_ = now;
        if (pending() && kind_ == Kind::MODE && field.value == requested_mode_)
          set_state_(State::CONFIRMED);
      } else if (field.type == 508) {
        unit_known_ = field.value <= 1;
        unit_ = static_cast<uint8_t>(field.value);
        unit_at_ = now;
        if (pending() && kind_ == Kind::UNIT && field.value == requested_unit_)
          set_state_(State::CONFIRMED);
      } else if (field.type == 598) {
        target_known_ = field.value <= 255;
        target_raw_ = static_cast<uint8_t>(field.value);
        target_at_ = now;
        if (pending() && kind_ == Kind::TEMPERATURE && field.value == requested_raw_)
          set_state_(State::CONFIRMED);
      }
    }
  }

  void tick(uint32_t now) {
    if (!pending()) return;
    const uint32_t elapsed = now - requested_at_;
    if (elapsed >= CONFIRM_TIMEOUT_MS) {
      set_state_(State::UNCONFIRMED);
    } else if (!readback_sent_ && elapsed >= READBACK_DELAY_MS) {
      readback_sent_ = true;
      if (on_send) on_send(request(2, 2, 2, {0x7D, 0x42}));
    }
  }

 private:
  State state_{State::IDLE};
  State temperature_state_{State::IDLE};
  State mode_state_{State::IDLE};
  State schedule_state_{State::IDLE};
  State operation_state_{State::IDLE};
  State unit_state_{State::IDLE};
  Kind kind_{Kind::TEMPERATURE};
  bool mode_known_{false};
  bool target_known_{false};
  bool schedule_known_{false};
  bool operation_known_{false};
  bool unit_known_{false};
  bool off_needs_schedule_{false};
  bool readback_sent_{false};
  uint8_t mode_{0};
  uint8_t target_raw_{0};
  uint8_t schedule_{0};
  uint8_t requested_mode_{0};
  uint8_t requested_raw_{0};
  uint8_t requested_schedule_{0};
  uint8_t requested_operation_{0};
  uint8_t unit_{0};
  uint8_t requested_unit_{0};
  uint32_t mode_at_{0};
  uint32_t target_at_{0};
  uint32_t schedule_at_{0};
  uint32_t operation_at_{0};
  uint32_t unit_at_{0};
  uint32_t requested_at_{0};

  void set_state_(State state) {
    state_ = state;
    if (kind_ == Kind::MODE) mode_state_ = state;
    else if (kind_ == Kind::SCHEDULE) schedule_state_ = state;
    else if (kind_ == Kind::OPERATION) operation_state_ = state;
    else if (kind_ == Kind::UNIT) unit_state_ = state;
    else temperature_state_ = state;
  }

  bool begin_(const Bytes &frame, uint32_t now) {
    if (!on_send) {
      set_state_(State::UNAVAILABLE);
      return false;
    }
    requested_at_ = now;
    readback_sent_ = false;
    set_state_(State::PENDING);
    on_send(frame);
    return true;
  }
};

class StreamDecoder {
 public:
  static constexpr size_t MAX_FRAME = 2048;
  static constexpr uint32_t IDLE_TIMEOUT_MS = 250;
  std::function<void(const Bytes &)> on_frame;
  uint32_t frames{0};
  uint32_t crc_errors{0};
  uint32_t length_errors{0};
  uint32_t timeouts{0};
  uint32_t received_bytes{0};

  StreamDecoder() { buffer_.reserve(MAX_FRAME); }

  void tick(uint32_t now) {
    if (!buffer_.empty() && uint32_t(now - last_byte_) > IDLE_TIMEOUT_MS) {
      ++timeouts;
      buffer_.clear();
    }
  }

  void push(uint8_t byte, uint32_t now) {
    tick(now);
    last_byte_ = now;
    ++received_bytes;
    buffer_.push_back(byte);
    drain_();
  }

 private:
  Bytes buffer_;
  uint32_t last_byte_{0};

  void drain_() {
    while (!buffer_.empty()) {
      const auto start = std::find(buffer_.begin(), buffer_.end(), uint8_t(4));
      buffer_.erase(buffer_.begin(), start);
      if (buffer_.size() < 9) return;
      const bool wide = extended(buffer_[5]);
      if (wide && buffer_.size() < 10) return;
      const size_t payload_size = wide ? (size_t(buffer_[8]) << 8) | buffer_[9] : buffer_[8];
      const size_t total = payload_size + (wide ? 12 : 11);
      if (total > MAX_FRAME) {
        ++length_errors;
        buffer_.erase(buffer_.begin());
        continue;
      }
      if (buffer_.size() < total) return;
      const uint16_t expected = (uint16_t(buffer_[total - 2]) << 8) | buffer_[total - 1];
      if (crc16(buffer_.data(), total - 2) != expected) {
        ++crc_errors;
        buffer_.erase(buffer_.begin());
        continue;
      }
      const Bytes frame(buffer_.begin(), buffer_.begin() + total);
      buffer_.erase(buffer_.begin(), buffer_.begin() + total);
      ++frames;
      if (on_frame) on_frame(frame);
    }
  }
};

}
