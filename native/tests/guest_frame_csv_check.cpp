#include <rex/graphics/guest_frame_csv.h>
#include <iostream>
#include <sstream>

int main() {
  using namespace std::chrono_literals;
  std::ostringstream output;
  rex::graphics::GuestFrameCsv capture(output, 3);
  const auto origin = rex::graphics::GuestFrameCsv::Clock::time_point{};
  capture.Record(origin);
  capture.Record(origin + 16ms);
  capture.Record(origin + 49ms);
  capture.Record(origin + 48ms);  // Invalid samples must not corrupt the origin.
  capture.Record(origin + 49ms);
  capture.Record(origin + 99ms);
  capture.Record(origin + 199ms);  // Capture is bounded.
  const std::string expected =
      "frame_index,elapsed_seconds,guest_frame_ms\n"
      "1,0.016000000,16.000000\n"
      "2,0.049000000,33.000000\n"
      "3,0.099000000,50.000000\n";
  if (output.str() != expected || capture.count() != 3) {
    std::cerr << "FAIL: raw intervals/origin/limit\n" << output.str();
    return 1;
  }
  std::ostringstream broken;
  rex::graphics::GuestFrameCsv failed(broken);
  broken.setstate(std::ios::badbit);
  failed.Record(origin);
  failed.Record(origin + 1ms);
  if (failed.count() != 0) return 2;
  std::ostringstream empty;
  rex::graphics::GuestFrameCsv disabled(empty, 0);
  disabled.Record(origin);
  disabled.Record(origin + 1ms);
  if (disabled.count() != 0) return 3;
  std::cout << "PASS: guest submission CSV preserves intervals, rejects invalid samples, bounds capture and tolerates output failure\n";
}
