#include "../src/shader_capture.h"
#include <array>
#include <cstdio>

int main() {
  using namespace mc2::capture;
  std::array<uint8_t, 12> header{0x10, 0x2A, 0x11, 1, 0, 0, 0, 36, 0, 0, 0, 12};
  ContainerSize size{};
  int failures = 0;
  const auto check = [&failures](bool condition) { if (!condition) ++failures; };
  check(ReadContainerSize(header.data(), header.size(), 0x82001000, size));
  check(size.virtual_bytes == 36 && size.physical_bytes == 12 && size.total_bytes == 48);
  check(!ReadContainerSize(header.data(), 11, 0x82001000, size));
  check(!ReadContainerSize(nullptr, 12, 0x82001000, size));
  check(!ReadContainerSize(header.data(), 12, 0, size));
  check(!ReadContainerSize(header.data(), 12, 0xDFFFFFF0, size));
  check(ReadContainerSize(header.data(), 12, 0xDFFFFFD0, size));
  check(ReadContainerSize(header.data(), 12, 0xE0000000, size));
  check(ReadContainerSize(header.data(), 12, 0xFF5F6000, size));
  check(!ReadContainerSize(header.data(), 12, 0xFFFFFFF0, size));
  check(GuestRange(0xFF5F6000, 96));
  check(!GuestRange(0xDFFFFFF0, 32));
  check(!GuestRange(0x82001000, UINT64_MAX));
  check(!ReadContainerSize(header.data(), 12, 0xDFFFFFDC, size));
  check(ReadContainerSize(header.data(), 12, 0xDFFFFFDC, size, false));
  header[7] = 35;
  check(!ReadContainerSize(header.data(), 12, 0x82001000, size));
  header[7] = 36;
  header[11] = 0;
  check(!ReadContainerSize(header.data(), 12, 0x82001000, size));
  header[8] = 0xFF;
  header[9] = header[10] = header[11] = 0xFF;
  check(!ReadContainerSize(header.data(), 12, 0x82001000, size));
  header[4] = 1; header[5] = header[6] = header[7] = 0;
  header[8] = header[9] = header[10] = 0; header[11] = 1;
  check(!ReadContainerSize(header.data(), 12, 0x82001000, size));
  const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
  check(ContentHash(hello, sizeof(hello)) == 0xA430D84680AABD0Bull);
  std::printf("Shader capture checks: %d failures\n", failures);
  return failures ? 1 : 0;
}
