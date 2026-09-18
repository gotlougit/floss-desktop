#include "os/system_properties.h"
#include <cassert>
#include <thread>
#include <vector>
using namespace bluetooth::os;
const bool initialized = [] {
  assert(GetSystemProperty("bluetooth.gd.start_timeout") == "12000");
  assert(!GetSystemProperty("not-present"));
  return SetSystemProperty("early", "ready");
}();
int main() {
  assert(initialized);
  assert(GetSystemProperty("early") == "ready");
  std::vector<std::thread> writers;
  for (int i = 0; i < 8; ++i) writers.emplace_back([i] {
    auto key = std::to_string(i);
    for (int n = 0; n < 1000; ++n) {
      SetSystemProperty(key, key);
      assert(GetSystemProperty(key) == key);
    }
  });
  for (auto& writer : writers) writer.join();
  assert(ClearSystemPropertiesForHost());
  assert(!GetSystemProperty("early"));
}
