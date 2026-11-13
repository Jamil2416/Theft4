#pragma once
#include <stddef.h>
#include <stdint.h>

// Crash-time memory evidence. Nothing is written unless an allocation fails or
// the process is about to terminate, so Retail Mode still writes no files in
// normal play. The 2026-11-06 crash (bad_alloc from a ~148 KB vector resize
// in PrepareConvertedVertexPayload) left only the .ips: no headroom, footprint
// or address-space data. Each event appends one line to the installed path.
#ifdef __cplusplus
extern "C" {
#endif
// Copies the path; call once at launch. Installs the operator-new failure and
// std::terminate hooks.
void theft4_crash_evidence_install(const char* path);
// Appends an event with the current memory state (Objective-C exceptions).
void theft4_crash_evidence_record(const char* event, const char* detail);
// Starts a low-priority thread that appends one address-space sample to the
// path every 15 seconds for the whole session (the 9-10 Nov crashes ran out of
// address space with RAM to spare, 8-35 minutes in, past any capture). A file
// over 512 KB is first renamed to "<path>.1". Call once at launch.
void theft4_crash_evidence_start_trail(const char* path);
// Diagnostic: each trail sample appends " caches=<values>" read from `values`
// (and the launch sample " cache_fields=<names>"). Call before starting the trail.
void theft4_crash_evidence_set_cache_probe(const char* (*names)(void),
                                           uint32_t (*values)(uint64_t* out, uint32_t capacity));
#ifdef __cplusplus
}

#include <cstdio>

namespace theft4::crash {
struct MemoryState {
  uint64_t headroom = 0;      // os_proc_available_memory
  uint64_t footprint = 0;     // task_vm_info.phys_footprint
  uint64_t virtual_size = 0;  // task_vm_info.virtual_size
  uint64_t largest_free = 0;  // largest unmapped range in [min_address, max_address)
  uint64_t address_span = 0;  // max_address - min_address
  uint64_t regions = 0;       // VM regions walked
};

// One line, integer conversions only so it does not allocate. Returns the
// length written, or 0 when the line does not fit.
inline size_t FormatEvidence(char* out, size_t capacity, uint64_t unix_seconds, const char* event,
                             const char* detail, const char* thread, uint64_t sequence,
                             const MemoryState& m) {
  constexpr uint64_t MiB = 1024 * 1024;
  const int n = std::snprintf(out, capacity,
      "t=%llu seq=%llu event=%s thread=%s headroom_mib=%llu footprint_mib=%llu "
      "virtual_mib=%llu largest_free_va_mib=%llu va_span_mib=%llu regions=%llu detail=%s\n",
      (unsigned long long)unix_seconds, (unsigned long long)sequence, event ? event : "",
      thread && *thread ? thread : "?", (unsigned long long)(m.headroom / MiB),
      (unsigned long long)(m.footprint / MiB), (unsigned long long)(m.virtual_size / MiB),
      (unsigned long long)(m.largest_free / MiB), (unsigned long long)(m.address_span / MiB),
      (unsigned long long)m.regions, detail ? detail : "");
  return n > 0 && size_t(n) < capacity ? size_t(n) : 0;
}

// Address space by owner. A bucket is one VM tag (the number the allocating
// library gave its mappings: malloc zones, stacks, IOSurface, ...) and one
// kind: 'a' anonymous memory, 'f' mapped file, 'n' reserved with no access,
// 's' shared-library submap.
struct VaBucket {
  uint32_t tag = 0;
  char kind = 'a';
  uint64_t bytes = 0, count = 0, largest = 0;
};
struct VaMap {
  static constexpr unsigned kBuckets = 64;
  VaBucket buckets[kBuckets];
  unsigned used = 0;
  uint64_t free_total = 0;  // unmapped bytes in [min_address, max_address)
  void Add(uint32_t tag, char kind, uint64_t size) {
    unsigned i = 0;
    while (i < used && (buckets[i].tag != tag || buckets[i].kind != kind)) ++i;
    if (i == used) {
      // More than 64 owners: the rest share the last bucket.
      if (used == kBuckets) i = kBuckets - 1;
      else buckets[used++] = VaBucket{tag, kind};
    }
    VaBucket& b = buckets[i];
    b.bytes += size;
    ++b.count;
    if (size > b.largest) b.largest = size;
  }
};

// "free_va_mib=N owners=N" then the largest `top` buckets as
// " <tag><kind>=<MiB>M/<regions>/<largest region KiB>K". Does not allocate.
// Buckets that do not fit are left off; returns the length written.
inline size_t FormatVaMap(char* out, size_t capacity, const VaMap& map, unsigned top) {
  constexpr uint64_t MiB = 1024 * 1024;
  int n = std::snprintf(out, capacity, "free_va_mib=%llu owners=%u",
      (unsigned long long)(map.free_total / MiB), map.used);
  if (n <= 0 || size_t(n) >= capacity) return 0;
  size_t length = size_t(n);
  bool printed[VaMap::kBuckets] = {};
  for (unsigned rank = 0; rank < top && rank < map.used; ++rank) {
    unsigned best = map.used;
    for (unsigned i = 0; i < map.used; ++i)
      if (!printed[i] && (best == map.used || map.buckets[i].bytes > map.buckets[best].bytes)) best = i;
    printed[best] = true;
    const VaBucket& b = map.buckets[best];
    n = std::snprintf(out + length, capacity - length, " %u%c=%lluM/%llu/%lluK", b.tag, b.kind,
        (unsigned long long)(b.bytes / MiB), (unsigned long long)b.count,
        (unsigned long long)(b.largest / 1024));
    if (n <= 0 || size_t(n) >= capacity - length) { out[length] = 0; break; }
    length += size_t(n);
  }
  return length;
}
}  // namespace theft4::crash
#endif
