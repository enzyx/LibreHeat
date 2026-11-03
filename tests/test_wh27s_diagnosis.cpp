#include "../src/wh27s_diagnosis.h"
#include <cassert>
#include <iostream>

int main(int argc, char **) {
  std::vector<uint8_t> data(69, 0);
  data[9] = 121;
  data[0] = 4;
  auto result = wh27s::decode_smart_diagnosis(data);
  assert(result.valid && result.index == 4 && result.rule != nullptr);
  assert(std::string(result.rule->component) == "M9");
  assert(result.summary() == "CH 04: Condensate drain is blocked.");
  data[0] = 250;
  result = wh27s::decode_smart_diagnosis(data);
  assert(result.rule != nullptr && std::string(result.rule->component) == "M51");
  data[0] = 0;
  data[8] = 160;
  result = wh27s::decode_smart_diagnosis(data);
  assert(result.valid && result.index == 256 && result.rule == nullptr);
  assert(result.summary() == "No problem found");
  data[9] = 0;
  assert(!wh27s::decode_smart_diagnosis(data).valid);
  data[9] = 121;
  data.pop_back();
  assert(!wh27s::decode_smart_diagnosis(data).valid);
  data.resize(70);
  assert(!wh27s::decode_smart_diagnosis(data).valid);
  assert(!wh27s::decode_smart_diagnosis({}).valid);

  // Optional cross-language vectors checked against the Python decoder.
  if (argc > 1) {
    data.assign(69, 0);
    data[9] = 121;
    for (unsigned index = 0; index < 512; ++index) {
      data[0] = index & 255;
      data[8] = index >= 256 ? 128 : 0;
      result = wh27s::decode_smart_diagnosis(data);
      assert(result.valid && result.index == index);
      std::cout << index << '\t' << result.summary() << '\n';
    }
  }
}
