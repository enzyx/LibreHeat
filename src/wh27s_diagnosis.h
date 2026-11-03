// Recovered audio-endpoint rules. Physical UART fault encoding is unverified.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wh27s {
struct DiagnosisRule {
  uint16_t index;
  const char *title;
  const char *component;
  const char *description;
};

inline constexpr DiagnosisRule DIAGNOSIS_RULES[] = {
  {1, "CH 01", "M20", "Ambient Temp sensor is not working."},
  {2, "CH 02", "M20", "Upper tank Temp sensor is not working."},
  {4, "CH 04", "M9", "Condensate drain is blocked."},
  {5, "CH 05", "M34", "PCB Communication error."},
  {6, "CH 06", "M20", "Lower tank Temp sensor is not working."},
  {8, "CH 08", "M18", "Water Leakage is detected."},
  {9, "CH 09", "M34", "EEPROM error."},
  {12, "CH 12", "M20", "Mid pipe Temp sensor is not working."},
  {19, "CH 19", "M33", "Upper element is not working."},
  {20, "CH 20", "M33", "Lower element is not working."},
  {21, "CH 21", "M24", "DC Peak (IPM Fault)"},
  {22, "CH 22", "M24", "22 CT 2 (Max, Current)"},
  {23, "CH 23", "M24", "DC Link Low/High Volt."},
  {26, "CH 26", "M27", "DC Comp Position Error, LOCKIG"},
  {27, "CH 27", "M24", "PSC Fault"},
  {29, "CH 29", "M27", "Comp Phase is Over-Current"},
  {32, "CH 32", "M20", "Discharge pipe is overheated"},
  {41, "CH 41", "M20", "Discharge pipe Temp sensor is not working."},
  {46, "CH 46", "M20", "Suction Pipe Temp sensor is not working."},
  {61, "CH 61", "M20", "Cond. Pipe Temp is High"},
  {65, "CH 65", "M29", "Heat sink Temp senor is not working."},
  {67, "CH 67", "M31", "BLDC motor fan is locked"},
  {250, "CH 250", "M51", "No or not enough water in tank."},
  {251, "CH 251", "M50", "Water Temp is too Low"},
  {252, "CH 252", "M50", "Water Temp is too High"},
  {253, "CH 253", "M50", "Water Temp is too Low"},
  {254, "CH 254", "M50", "ECO is tripped."},
  {255, "CH 255", "M50", "Water Temp is too High"},
};

struct DiagnosisResult {
  bool valid{false};
  uint16_t index{0};
  const DiagnosisRule *rule{nullptr};

  std::string summary() const {
    if (!valid) return "Unsupported diagnosis block";
    if (rule != nullptr) return std::string(rule->title) + ": " + rule->description;
    // All 512 indices were tested: LG uses its normal verdict for non-fault rules.
    return "No problem found";
  }
};

inline DiagnosisResult decode_smart_diagnosis(const std::vector<uint8_t> &data) {
  if (data.size() != 69 || data[9] != 121) return {};
  DiagnosisResult result;
  result.valid = true;
  result.index = data[0] | ((data[8] & 0x80U) << 1);
  for (const auto &rule : DIAGNOSIS_RULES) {
    if (rule.index == result.index) {
      result.rule = &rule;
      break;
    }
  }
  return result;
}
}  // namespace wh27s
