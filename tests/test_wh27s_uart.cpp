#include "../src/wh27s_uart.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <string>

static wh27s::Bytes unhex(const std::string &text) {
  wh27s::Bytes result;
  assert(text.size() % 2 == 0);
  for (size_t offset = 0; offset < text.size(); offset += 2)
    result.push_back(std::stoul(text.substr(offset, 2), nullptr, 16));
  return result;
}

static wh27s::Bytes synthetic_report(const wh27s::Bytes &payload) {
  auto frame = wh27s::request(2, 4, 0x33, payload);
  frame[4] = 0xA7;
  const auto crc = wh27s::crc16(frame.data(), frame.size() - 2);
  frame[frame.size() - 2] = crc >> 8;
  frame.back() = crc & 0xFF;
  return frame;
}

int main(int argc, char **) {
  using namespace wh27s;
  const std::string check = "123456789";
  assert(crc16(reinterpret_cast<const uint8_t *>(check.data()), check.size()) == 0x31C3);
  assert(request(1, 1, 1, {0xAA, 0x43}) == unhex("040000006501010102aa43e6cd"));
  assert(request(2, 2, 1, {0xE6, 0}) == unhex("040000006502020102e600f6f9"));
  assert(request(2, 0x10, 0x2C, {}) == unhex("040000006502102c004be4"));
  // Real panel-button frame, including repeated identical packets in the capture.
  const auto pairing = unhex("040000008701010102aa41be8f");
  assert(is_wifi_pairing_request(pairing, decode_payload(pairing)));
  assert(!is_wifi_pairing_request(request(1, 1, 1, {0xAA, 0x43}),
                                 decode_payload(request(1, 1, 1, {0xAA, 0x43}))));
  auto other_pairing = pairing;
  other_pairing[4] = 0xA7;
  assert(!is_wifi_pairing_request(other_pairing, decode_payload(other_pairing)));
  other_pairing = pairing;
  other_pairing[6] = 4;
  assert(!is_wifi_pairing_request(other_pairing, decode_payload(other_pairing)));
  assert(!is_wifi_pairing_request(pairing, Payload{false, {{681, 1}}, {}}));
  assert(!is_wifi_pairing_request({}, decode_payload(pairing)));
  TemperatureControl unit_control;
  std::vector<Bytes> unit_sent;
  unit_control.on_send = [&](const Bytes &frame) { unit_sent.push_back(frame); };
  assert(!unit_control.toggle_unit(0));  // Unknown state must not guess a direction.
  unit_control.receive(synthetic_report({0x7F, 0x00}), 100);
  assert(unit_control.toggle_unit(101));
  assert(unit_sent.back() == request(2, 1, 0, {0x7F, 0x01}));
  assert(!unit_control.toggle_unit(102));  // Do not overlap writes.
  unit_control.receive(request(2, 0x10, 0, {}), 103);
  assert(unit_control.pending());  // ACK is not confirmation.
  unit_control.receive(synthetic_report({0x7F, 0x00}), 104);
  assert(unit_control.pending());
  unit_control.tick(1101);
  assert(unit_sent.back() == request(2, 2, 2, {0x7D, 0x42}));
  unit_control.receive(synthetic_report({0x7F, 0x01}), 1102);
  assert(unit_control.state() == TemperatureControl::State::CONFIRMED);
  assert(unit_control.toggle_unit(1103));
  assert(unit_sent.back() == request(2, 1, 0, {0x7F, 0x00}));
  unit_control.tick(6103);
  assert(unit_control.state() == TemperatureControl::State::UNCONFIRMED);
  assert(!unit_control.toggle_unit(70000));  // Stale state.
  unit_control.receive(synthetic_report({0x7F, 0x02}), 70001);
  assert(!unit_control.toggle_unit(70002));  // Unknown enum.
  unit_control.receive(synthetic_report({0x7F, 0x00}), 70003);
  unit_control.invalidate_status();
  assert(!unit_control.toggle_unit(70004));
  const auto report = unhex("04000000a702042c0395905c227f");
  const auto decoded = decode_payload(report);
  assert(decoded.valid && decoded.fields.size() == 1);
  assert(decoded.fields[0].type == 598 && decoded.fields[0].value == 92);
  Bytes diagnosis{0xE6, 0x50, 69};
  diagnosis.resize(72, 0x80);
  const auto diagnostic = decode_payload(request(2, 4, 1, diagnosis));
  assert(diagnostic.valid && diagnostic.fields.size() == 1 && diagnostic.diagnosis.size() == 69);
  diagnosis.pop_back();
  assert(!decode_payload(request(2, 4, 1, diagnosis)).valid);
  assert(!decode_payload(request(2, 4, 1, {0x95, 0x90})).valid);
  assert(decode_payload(request(0x66, 4, 0x11, {0x95, 0x90, 92})).fields.empty());

  const auto identity = decode_payload(unhex("04000000a70201461ce50be5604133e5a00704e5e00704e50ce5603764e5a03267e5e00fae74a5"));
  const auto records = firmware_records(identity);
  assert(records.size() == 2);
  assert(records[0].type == 11 && records[0].part_number() == "SAA41330704");
  assert(records[0].checksum_hex() == "0704");
  assert(records[1].type == 12 && records[1].part_number() == "SAA37643267");
  assert(records[1].checksum_hex() == "0FAE");
  assert(firmware_records(Payload{false, identity.fields, {}}).empty());
  assert(firmware_records(Payload{true, {{916, 11}, {917, 0x4133}, {919, 0x0704},
                                         {916, 12}, {918, 0x3267}, {919, 0x0FAE}}, {}}).empty());
  assert(firmware_records(Payload{true, {{916, 11}, {917, 0x10000}, {918, 1}, {919, 2}}, {}}).empty());
  assert(field_list({{725, 1}, {725, 3}}) == "725=1; 725=3");
  Telemetry telemetry;
  assert(std::isnan(telemetry.value(554)));
  telemetry.receive(Payload{true, {{554, 0}, {552, 3}, {553, 89}}, {}});
  telemetry.receive(Payload{true, {{554, 42}}, {}});
  assert(telemetry.value(554) == 42 && telemetry.value(552) == 3 && telemetry.value(553) == 89);
  telemetry.receive(Payload{false, {{554, 100}}, {}});
  assert(telemetry.value(554) == 42);

  ReadSession session;
  std::vector<Bytes> sent;
  session.on_send = [&](const Bytes &frame) { sent.push_back(frame); };
  session.tick(100);
  assert(sent.empty());
  session.start(100);
  assert(sent.back() == unhex("040000006501010102aa43e6cd"));
  session.receive(unhex("04000000870210010044d1"), 101);
  assert(session.stage() == ReadSession::Stage::DISCOVERY && sent.size() == 1);
  session.receive(unhex("040000008701100100df0d"), 115);
  assert(sent.back() == unhex("0400000065020201027d416a0d"));
  session.receive(unhex("04000000a70201013fb01012b4901eb0c1b101b4f0020000b541b543b6a00faeb6f0330704bd3001fd00b701bc41bd43b5d019b6105ab5d01ab6105ab5d01bb6105ab5d01cb6105a5e4b"), 326);
  assert(sent.back() == unhex("0400000065020202027d42c1b2") && sent.size() == 3);
  session.receive(unhex("0400000087021002001782"), 327);
  assert(session.stage() == ReadSession::Stage::STATUS);
  session.receive(unhex("04000000a70204432b7e50197dc195506d95905aa2407f0088408a038a50578a808c90508cd01dacd032d540d5a010e0c900ac400087"), 500);
  assert(session.stage() == ReadSession::Stage::READY);
  session.tick(30499);
  assert(sent.size() == 3);
  session.tick(30500);
  assert(sent.size() == 4 && sent.back() == unhex("0400000065020202027d42c1b2"));
  session.stop();
  session.tick(50000);
  assert(session.stage() == ReadSession::Stage::IDLE && sent.size() == 4);
  session.start(60000);
  session.tick(62000);
  session.tick(64000);
  session.tick(66000);
  assert(session.stage() == ReadSession::Stage::FAILED && sent.size() == 7);
  session.tick(100000);
  assert(sent.size() == 7);
  session.start(0xFFFFFF00);
  session.tick(0x100);
  assert(sent.size() == 8);
  session.tick(0x700);
  assert(sent.size() == 9);

  TemperatureControl temperature;
  std::vector<Bytes> writes;
  temperature.on_send = [&](const Bytes &frame) { writes.push_back(frame); };
  assert(!temperature.set_temperature(46, 0) && writes.empty());
  const auto status = unhex("04000000a70204432b7e50197dc195506d95905aa2407f0088408a038a50578a808c90508cd01dacd032d540d5a010e0c900ac400087");
  temperature.receive(status, 100);
  const char *captured_writes[] = {
      "0400000065020100067e5019959058009d",
      "0400000065020100067e501995905a20df",
      "0400000065020100067e501995905c4019",
      "0400000065020100067e501995905e605b",
      "0400000065020100067e5019959060b7c6"};
  for (unsigned index = 0; index < 5; ++index) {
    assert(temperature.set_temperature(44.0f + index, 200 + index * 20));
    assert(writes.back() == unhex(captured_writes[index]));
    assert(temperature.pending());
    temperature.receive(synthetic_report({0x95, 0x90, uint8_t(88 + index * 2)}), 210 + index * 20);
    assert(temperature.state() == TemperatureControl::State::CONFIRMED);
  }
  const auto before_invalid = writes.size();
  for (const auto invalid : {34.5f, 60.5f, 45.25f,
                            std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity()}) {
    assert(!temperature.set_temperature(invalid, 300));
  }
  assert(writes.size() == before_invalid);
  temperature.receive(synthetic_report({0x7E, 0x50, 27, 0x95, 0x90, 90}), 400);
  assert(temperature.set_temperature(45.5f, 500));
  assert(writes.back() == request(2, 1, 0, {0x7E, 0x50, 27, 0x95, 0x90, 91}));
  assert(temperature.requested_temperature() == 45.5f);
  assert(!temperature.set_temperature(46, 501));
  temperature.receive(unhex("04000000870210000077e0"), 600);
  assert(temperature.pending());
  temperature.receive(synthetic_report({0x95, 0x90, 90}), 700);
  assert(temperature.pending());
  const auto before_readback = writes.size();
  temperature.tick(1499);
  assert(writes.size() == before_readback);
  temperature.tick(1500);
  assert(writes.size() == before_readback + 1);
  assert(writes.back() == unhex("0400000065020202027d42c1b2"));
  temperature.tick(2500);
  assert(writes.size() == before_readback + 1);
  temperature.receive(synthetic_report({0x95, 0x90, 91}), 3000);
  assert(temperature.state() == TemperatureControl::State::CONFIRMED);
  assert(temperature.set_temperature(35, 3100));
  temperature.tick(8100);
  assert(temperature.state() == TemperatureControl::State::UNCONFIRMED);
  const auto after_timeout = writes.size();
  temperature.tick(10000);
  assert(writes.size() == after_timeout);
  assert(!temperature.set_temperature(46, 70000));
  temperature.receive(status, 71000);
  assert(temperature.set_temperature(60, 71001));
  temperature.cancel();
  assert(temperature.state() == TemperatureControl::State::CANCELLED);
  temperature.invalidate_status();
  assert(!temperature.set_temperature(46, 71002));
  temperature.receive(status, 0xFFFFFF00);
  assert(temperature.set_temperature(46, 0x10));
  temperature.receive(report, 0x20);
  assert(temperature.state() == TemperatureControl::State::CONFIRMED);
  temperature.receive(synthetic_report({0x7E, 0x50, 95, 0x95, 0x90, 90}), 0x30);
  assert(!temperature.set_temperature(46, 0x40));

  TemperatureControl modes;
  std::vector<Bytes> mode_writes;
  modes.on_send = [&](const Bytes &frame) { mode_writes.push_back(frame); };
  assert(!modes.set_mode(25, 0) && mode_writes.empty());
  modes.receive(status, 100);
  assert(!modes.set_mode(95, 101));
  assert(modes.state() == TemperatureControl::State::INVALID_MODE);
  assert(mode_name(25) != nullptr && mode_name(95) == nullptr);
  assert(capability_labels(130304, {{9, "F"}, {11, "D"}, {17, "W"}}) ==
         "F; D; Code 12; Code 13; Code 14; Code 15; Code 16; W");
  assert(capability_labels(3, {{1, "S"}, {2, "O"}}) == "S; O");
  assert(capability_labels(0, {}) == "None" && capability_labels(NAN, {}).empty());
  assert(notification_name(0) == "None" && notification_name(12) == "T&P valve check");
  assert(notification_name(99) == "Unknown (99)");
  assert(error_code_name(0) == "None" && error_code_name(5) == "05 Communication error");
  assert(error_code_name(99) == "99 Unknown");
  for (uint8_t mode = 25; mode <= 28; ++mode) {
    const uint32_t now = 200 + (mode - 25) * 100;
    assert(modes.set_mode(mode, now) && modes.is_mode_request());
    assert(mode_writes.back() == request(2, 1, 0, {0x7E, 0x50, mode, 0x95, 0x90, 90}));
    assert(mode_writes.back().size() == 17);
    assert(!modes.set_temperature(46, now + 1));
    modes.receive(unhex("04000000870210000077e0"), now + 2);
    modes.receive(synthetic_report({0x95, 0x90, 90}), now + 3);
    assert(modes.pending());
    modes.receive(synthetic_report({0x7E, 0x50, mode}), now + 4);
    assert(modes.state() == TemperatureControl::State::CONFIRMED);
  }
  assert(modes.set_temperature(46, 800));
  assert(std::string(modes.mode_state_name()) == "Confirmed");
  modes.receive(synthetic_report({0x95, 0x90, 92}), 810);
  assert(std::string(modes.temperature_state_name()) == "Confirmed");
  assert(modes.set_mode(25, 1000));
  assert(mode_writes.back() == unhex("0400000065020100067e501995905c4019"));
  modes.receive(synthetic_report({0x7E, 0x50, 26}), 1001);
  assert(modes.pending());
  const auto before_mode_readback = mode_writes.size();
  modes.tick(2000);
  assert(mode_writes.size() == before_mode_readback + 1);
  assert(mode_writes.back() == unhex("0400000065020202027d42c1b2"));
  modes.tick(6000);
  assert(modes.state() == TemperatureControl::State::UNCONFIRMED);
  assert(std::string(modes.mode_state_name()) == "Not confirmed");
  assert(std::string(modes.temperature_state_name()) == "Confirmed");
  modes.tick(9000);
  assert(mode_writes.size() == before_mode_readback + 1);
  assert(!modes.set_mode(25, 70000));
  modes.receive(status, 71000);
  assert(modes.set_temperature(46, 71001) && !modes.is_mode_request());
  assert(!modes.set_mode(26, 71002) && !modes.is_mode_request());
  modes.cancel();

  TemperatureControl schedule;
  std::vector<Bytes> schedule_writes;
  schedule.on_send = [&](const Bytes &frame) { schedule_writes.push_back(frame); };
  assert(!schedule.set_schedule(true, 0) && schedule_writes.empty());
  assert(std::string(schedule.schedule_state_name()) == "Rejected: no fresh status");
  schedule.receive(status, 100);
  assert(schedule.set_schedule(true, 200) && schedule.kind() == TemperatureControl::Kind::SCHEDULE);
  assert(schedule_writes.back() == request(2, 1, 0, {0xA2, 0x41}));
  assert(!schedule.set_mode(25, 201) && !schedule.set_schedule(false, 201));
  schedule.receive(synthetic_report({0xA2, 0x40}), 300);
  assert(schedule.pending());
  schedule.receive(synthetic_report({0xA2, 0x41}), 400);
  assert(std::string(schedule.schedule_state_name()) == "Confirmed");
  assert(std::string(schedule.mode_state_name()) == "Idle");
  assert(schedule.set_schedule(false, 500));
  assert(schedule_writes.back() == request(2, 1, 0, {0xA2, 0x40}));
  schedule.tick(5500);
  assert(std::string(schedule.schedule_state_name()) == "Not confirmed");
  assert(!schedule.set_schedule(false, 70000));
  schedule.receive(status, 70100);
  schedule.invalidate_status();
  assert(!schedule.set_schedule(false, 70101));

  // Captured stock sequence: off = 649=1 then 503=0 (040000006502010002a241f772, 0400000065020100027dc073c2).
  TemperatureControl power;
  std::vector<Bytes> power_writes;
  power.on_send = [&](const Bytes &frame) { power_writes.push_back(frame); };
  assert(!power.set_operation(false, 0) && power_writes.empty());
  power.receive(status, 100);
  assert(power.set_operation(false, 200));
  assert(power_writes.back() == unhex("040000006502010002a241f772"));
  power.receive(synthetic_report({0x7D, 0xC1}), 250);
  assert(power.pending() && power_writes.size() == 1);
  power.receive(synthetic_report({0xA2, 0x41}), 300);
  assert(power_writes.back() == unhex("0400000065020100027dc073c2") && power.pending());
  power.receive(synthetic_report({0x7D, 0xC0}), 400);
  assert(std::string(power.operation_state_name()) == "Confirmed");
  assert(power.set_operation(false, 500));
  assert(power_writes.back() == unhex("0400000065020100027dc073c2"));
  power.receive(synthetic_report({0x7D, 0xC0}), 600);
  assert(power.set_operation(true, 700));
  assert(power_writes.back() == unhex("0400000065020100087dc17e50197f905a840e"));
  power.receive(synthetic_report({0x7D, 0xC1}), 800);
  assert(power.state() == TemperatureControl::State::CONFIRMED);
  assert(std::string(power.schedule_state_name()) == "Idle");
  assert(power.set_operation(false, 900));
  power.tick(5900);
  assert(std::string(power.operation_state_name()) == "Not confirmed");
  power.invalidate_status();
  assert(!power.set_operation(true, 6000));

  StreamDecoder decoder;
  unsigned callbacks = 0;
  decoder.on_frame = [&](const Bytes &frame) {
    ++callbacks;
    assert(frame == report);
  };
  decoder.push(0xFF, 0);
  decoder.push(0x04, 1);
  decoder.tick(252);
  assert(decoder.timeouts == 1);
  for (auto byte : report) decoder.push(byte, 253);
  assert(decoder.frames == 1 && callbacks == 1);
  auto corrupt = report;
  corrupt.back() ^= 1;
  for (auto byte : corrupt) decoder.push(byte, 254);
  decoder.tick(505);
  for (auto byte : report) decoder.push(byte, 506);
  assert(decoder.crc_errors >= 1 && decoder.frames == 2);
  StreamDecoder wide;
  const auto extended_frame = request(0xF1, 4, 1, Bytes(300, 0xAA));
  for (auto byte : extended_frame) wide.push(byte, 0);
  assert(wide.frames == 1);
  for (auto byte : unhex("0400000065f10401ffff")) wide.push(byte, 1);
  assert(wide.length_errors == 1);
  StreamDecoder rollover;
  rollover.push(4, 0xFFFFFFF0);
  rollover.tick(0x20);
  assert(rollover.timeouts == 0);
  rollover.tick(0x100);
  assert(rollover.timeouts == 1);

  if (argc > 1) {
    std::string line;
    unsigned replayed = 0;
    while (std::getline(std::cin, line)) {
      if (line.empty()) continue;
      StreamDecoder replay;
      replay.on_frame = [&](const Bytes &frame) {
        const auto payload = decode_payload(frame);
        assert(payload.valid);
      };
      for (auto byte : unhex(line)) replay.push(byte, 0);
      assert(replay.frames == 1 && replay.crc_errors == 0 && replay.length_errors == 0);
      ++replayed;
    }
    std::cout << "Replayed " << replayed << " captured frames\n";
    assert(replayed > 0);
  }
  std::cout << "UART transport tests passed\n";
}
