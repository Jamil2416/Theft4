#include "theft4_crash_evidence.h"
#include <cassert>
#include <cstring>
#include <string_view>

int main() {
  constexpr uint64_t MiB = 1024 * 1024;
  theft4::crash::MemoryState m{};
  m.headroom = 3152 * MiB;
  m.footprint = 1968 * MiB + 123;  // Truncated to whole MiB.
  m.virtual_size = 8909 * MiB;
  m.largest_free = 148 * MiB;
  m.address_span = 61440 * MiB;
  m.regions = 5778;
  char line[512];
  const size_t n = theft4::crash::FormatEvidence(line, sizeof(line), 1793939888,
      "operator-new-failed", "size=unknown", "Theft4 native render", 3, m);
  assert(n == std::strlen(line) && line[n - 1] == '\n');
  assert(std::string_view(line) ==
         "t=1793939888 seq=3 event=operator-new-failed thread=Theft4 native render "
         "headroom_mib=3152 footprint_mib=1968 virtual_mib=8909 largest_free_va_mib=148 "
         "va_span_mib=61440 regions=5778 detail=size=unknown\n");
  // Unnamed threads and missing text stay well-formed.
  assert(theft4::crash::FormatEvidence(line, sizeof(line), 1, "terminate", nullptr, "", 1, {}));
  assert(std::string_view(line) ==
         "t=1 seq=1 event=terminate thread=? headroom_mib=0 footprint_mib=0 virtual_mib=0 "
         "largest_free_va_mib=0 va_span_mib=0 regions=0 detail=\n");
  // A line that does not fit is dropped, never written truncated.
  char small[32];
  assert(theft4::crash::FormatEvidence(small, sizeof(small), 1, "terminate", "x", "t", 1, m) == 0);
  return 0;
}
