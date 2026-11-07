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
}  // namespace theft4::crash
#endif
