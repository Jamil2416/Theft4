#include "theft4_crash_evidence.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <exception>
#include <fcntl.h>
#include <mach/mach.h>
#include <new>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/stat.h>
#include <typeinfo>
#include <unistd.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

namespace {
// Everything here runs after malloc has failed or during termination: fixed
// buffers and syscalls only.
char g_path[1024];
char g_trail_path[1024];
std::atomic<uint64_t> g_sequence{0};
std::atomic<unsigned> g_new_failures{0};
std::terminate_handler g_previous_terminate = nullptr;
constexpr unsigned kMaxNewFailureLines = 8;
constexpr off_t kMaxFileBytes = 64 * 1024;
constexpr off_t kMaxTrailBytes = 512 * 1024;
constexpr unsigned kMaxTrailSamples = 12 * 60 * 4;

// One walk of the top-level VM map: shared-library submaps count as one
// region each. `map`, when given, receives the address space by owner.
theft4::crash::MemoryState Sample(theft4::crash::VaMap* map) {
  theft4::crash::MemoryState m{};
#if TARGET_OS_IPHONE
  m.headroom = os_proc_available_memory();
#endif
  task_vm_info_data_t vm{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&vm), &count) != KERN_SUCCESS) return m;
  const size_t have = size_t(count) * sizeof(natural_t);
  m.virtual_size = vm.virtual_size;
  if (have >= offsetof(task_vm_info_data_t, phys_footprint) + sizeof(vm.phys_footprint))
    m.footprint = vm.phys_footprint;
  if (have < offsetof(task_vm_info_data_t, max_address) + sizeof(vm.max_address)) return m;
  const uint64_t low = vm.min_address, high = vm.max_address;
  uint64_t cursor = low;
  vm_address_t address = vm_address_t(low);
  const auto gap = [&](uint64_t bytes) {
    m.largest_free = std::max(m.largest_free, bytes);
    if (map) map->free_total += bytes;
  };
  for (;;) {
    vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info{};
    mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (vm_region_recurse_64(mach_task_self(), &address, &size, &depth,
                             vm_region_recurse_info_t(&info), &info_count) != KERN_SUCCESS || !size)
      break;
    if (address >= high) break;
    ++m.regions;
    if (address > cursor) gap(address - cursor);
    if (map)
      map->Add(info.user_tag, info.is_submap ? 's' : info.protection == VM_PROT_NONE ? 'n'
               : info.external_pager ? 'f' : 'a', size);
    cursor = std::max<uint64_t>(cursor, uint64_t(address) + size);
    address += size;
  }
  if (high > cursor) gap(high - cursor);
  m.address_span = high - low;
  return m;
}

void Append(const char* path, const char* line, size_t length) {
  if (!path[0] || !length) return;
  const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return;
  (void)write(fd, line, length);
  close(fd);
}

// The event line, then a "va-map" line saying who holds the address space.
void Write(const char* event, const char* detail) {
  if (!g_path[0]) return;
  char thread[64] = {};
  pthread_getname_np(pthread_self(), thread, sizeof(thread));
  theft4::crash::VaMap map;
  const theft4::crash::MemoryState state = Sample(&map);
  const uint64_t now = uint64_t(time(nullptr));
  char line[2048];
  Append(g_path, line, theft4::crash::FormatEvidence(line, sizeof(line), now, event, detail, thread,
      ++g_sequence, state));
  char owners[1536];
  if (!theft4::crash::FormatVaMap(owners, sizeof(owners), map, 32)) return;
  Append(g_path, line, theft4::crash::FormatEvidence(line, sizeof(line), now, "va-map", owners, thread,
      ++g_sequence, state));
}

void* TrailMain(void*) {
  pthread_setname_np("Theft4 memory trail");
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
  // 15 s apart: about 110 KB an hour, and the walk is a few thousand cheap
  // kernel calls. Stops after 12 hours so one session cannot fill the disk.
  for (unsigned sample = 0; sample < kMaxTrailSamples; ++sample) {
    theft4::crash::VaMap map;
    const theft4::crash::MemoryState state = Sample(&map);
    char owners[768], line[1280];
    if (theft4::crash::FormatVaMap(owners, sizeof(owners), map, 12))
      Append(g_trail_path, line, theft4::crash::FormatEvidence(line, sizeof(line),
          uint64_t(time(nullptr)), sample ? "sample" : "launch", owners, "trail", sample + 1, state));
    sleep(15);
  }
  return nullptr;
}

// operator new reports no size to its handler; the .ips backtrace names the
// allocation site. Behaviour is unchanged: the failure still throws.
void NewHandler() {
  if (g_new_failures.fetch_add(1) < kMaxNewFailureLines)
    Write("operator-new-failed", "size=unknown");
  throw std::bad_alloc();
}

[[noreturn]] void Terminate() {
  char detail[512] = "no active exception";
  if (const std::type_info* type = abi::__cxa_current_exception_type()) {
    std::snprintf(detail, sizeof(detail), "uncaught=%s", type->name());
    if (auto active = std::current_exception()) {
      try {
        std::rethrow_exception(active);
      } catch (const std::exception& e) {
        std::snprintf(detail, sizeof(detail), "uncaught=%s what=%s", type->name(), e.what());
      } catch (...) {
      }
    }
  }
  Write("terminate", detail);
  // Objective-C exceptions continue into libobjc's handler, which runs the
  // uncaught-exception handler (recorded by the app) and then aborts.
  if (g_previous_terminate) g_previous_terminate();
  std::abort();
}
}  // namespace

extern "C" void theft4_crash_evidence_install(const char* path) {
  if (!path || g_path[0]) return;
  const size_t length = std::strlen(path);
  if (length >= sizeof(g_path)) return;
  std::memcpy(g_path, path, length + 1);
  struct stat existing {};
  if (stat(g_path, &existing) == 0 && existing.st_size > kMaxFileBytes) truncate(g_path, 0);
  std::set_new_handler(NewHandler);
  g_previous_terminate = std::set_terminate(Terminate);
}

extern "C" void theft4_crash_evidence_record(const char* event, const char* detail) {
  Write(event, detail);
}

extern "C" void theft4_crash_evidence_start_trail(const char* path) {
  if (!path || g_trail_path[0]) return;
  const size_t length = std::strlen(path);
  if (length + 3 > sizeof(g_trail_path)) return;
  std::memcpy(g_trail_path, path, length + 1);
  struct stat existing {};
  if (stat(g_trail_path, &existing) == 0 && existing.st_size > kMaxTrailBytes) {
    // Keep the session that just ended: it may be the one that crashed.
    char previous[sizeof(g_trail_path) + 2];
    std::snprintf(previous, sizeof(previous), "%s.1", g_trail_path);
    rename(g_trail_path, previous);
  }
  pthread_t thread;
  if (pthread_create(&thread, nullptr, TrailMain, nullptr) == 0) pthread_detach(thread);
}
#else
extern "C" void theft4_crash_evidence_install(const char*) {}
extern "C" void theft4_crash_evidence_record(const char*, const char*) {}
extern "C" void theft4_crash_evidence_start_trail(const char*) {}
#endif
