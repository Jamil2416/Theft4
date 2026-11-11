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
  // Address space by owner: largest first, same tag split by kind.
  theft4::crash::VaMap map;
  map.free_total = 3 * MiB;
  map.Add(1, 'a', 2 * MiB);
  map.Add(0, 'f', 4096 * MiB);
  map.Add(1, 'a', 6 * MiB);
  map.Add(1, 'n', 1 * MiB);
  char owners[256];
  size_t length = theft4::crash::FormatVaMap(owners, sizeof(owners), map, 2);
  assert(length == std::strlen(owners));
  assert(std::string_view(owners) == "free_va_mib=3 owners=3 0f=4096M/1/4194304K 1a=8M/2/6144K");
  // Buckets that do not fit are left off whole.
  char tight[40];
  length = theft4::crash::FormatVaMap(tight, sizeof(tight), map, 3);
  assert(std::string_view(tight) == "free_va_mib=3 owners=3" && length == std::strlen(tight));
  // Past 64 owners the rest share the last bucket.
  for (uint32_t tag = 10; tag < 100; ++tag) map.Add(tag, 'a', MiB);
  assert(map.used == theft4::crash::VaMap::kBuckets);
  return 0;
}
