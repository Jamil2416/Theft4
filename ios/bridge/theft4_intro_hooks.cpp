#include <atomic>
#include <cstdint>
#include <limits>
#include <span>

#include <rex/logging.h>
#include <rex/runtime.h>

#include "gta4_init.h"
#include "gta4_presentation_policy.h"

namespace {
namespace policy = gta4::presentation::policy;
constexpr uint32_t kActive = 0x831D5335;
constexpr uint32_t kScreenCount = 0x831D5344;
constexpr uint32_t kDefinitions = 0x831D5498;
std::atomic<bool> skip_intro{true};
thread_local bool cold_parser_scope = false;

bool GuestSpan(uint8_t* base, uint32_t address, size_t size, bool writable = false) {
  if (!base || !address || !size || size > std::numeric_limits<uint32_t>::max()) return false;
  const uint64_t last = uint64_t(address) + size - 1;
  if (last > std::numeric_limits<uint32_t>::max()) return false;
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = kernel ? kernel->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  if (!heap || heap != memory->LookupHeap(uint32_t(last))) return false;
  const auto access = heap->QueryRangeAccess(address, uint32_t(last));
  using rex::memory::PageAccess;
  return access == PageAccess::kReadWrite || access == PageAccess::kExecuteReadWrite ||
      (!writable && (access == PageAccess::kReadOnly || access == PageAccess::kExecuteReadOnly));
}

struct ParserScope {
  bool previous = cold_parser_scope;
  explicit ParserScope(bool enabled) { cold_parser_scope = enabled; }
  ~ParserScope() { cold_parser_scope = previous; }
};
}  // namespace

// Referencing this function also retains the strong wrappers in the iOS archive.
extern "C" void theft4_intro_configure(bool enabled) {
  skip_intro.store(enabled, std::memory_order_relaxed);
}

extern "C" void sub_82145420(PPCContext& ctx, uint8_t* base) {
  const bool eligible = skip_intro.load(std::memory_order_relaxed) &&
      policy::IsColdStart(uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32) &&
      GuestSpan(base, kActive, 1) && !REX_LOAD_U8(kActive);
  const ParserScope scope(eligible);
  // Preserve initialization, asset loading, audio readiness and screen markers.
  __imp__sub_82145420(ctx, base);
}

extern "C" void sub_82145968(PPCContext& ctx, uint8_t* base) {
  const bool apply = cold_parser_scope && ctx.lr == policy::kParserCaller;
  __imp__sub_82145968(ctx, base);
  if (!apply || !GuestSpan(base, kScreenCount, sizeof(uint32_t))) return;
  const uint32_t count = REX_LOAD_U32(kScreenCount);
  if (!count || count > policy::kMaxScreens ||
      !GuestSpan(base, kDefinitions, size_t(count) * policy::kScreenStride, true)) return;
  const auto plan = policy::CollapseIntro(
      std::span<uint8_t>(base + kDefinitions, size_t(count) * policy::kScreenStride), count, true);
  REXLOG_INFO("Theft4 intro: skip-applied={} records={} prefix={} initialization=retained",
      plan.status == policy::IntroStatus::kReady, count, plan.prefix_count);
}

// Launcher episode choice. sub_82241428 returns the highest installed content
// id the profile has not yet been told about (profile setting 500), or -1,
// and records every installed id as told. Its caller maps a positive id to
// that content's episode and opens the retail "play the new episode" screen
// (startup state 3); -1 continues to the normal title (state 1). The ids come
// from each episode's setup2.xml: TBoGT's episode entry is <id>4</id>.
namespace {
constexpr uint32_t kContentManagerSlot = 0x831E4DD4;  // lis -31970 + 19924
constexpr int32_t kNoEpisode = -1;
// -2: keep the retail result; -1: always the normal title; >0: that content id.
std::atomic<int32_t> requested_content{-2};

uint32_t InstalledContentMask(PPCContext& ctx, uint8_t* base) {
  if (!GuestSpan(base, kContentManagerSlot, sizeof(uint32_t))) return 0;
  const uint32_t manager = REX_LOAD_U32(kContentManagerSlot);
  if (!manager) return 0;
  // The same query the retail function makes; called at its return, where
  // the caller expects volatile registers to be clobbered.
  const uint64_t lr = ctx.lr;
  ctx.r3.u64 = manager;
  sub_82219F98(ctx, base);
  ctx.lr = lr;
  return ctx.r3.u32;
}
}  // namespace

extern "C" void theft4_episode_start_configure(int32_t content_id) {
  requested_content.store(content_id, std::memory_order_relaxed);
}

extern "C" void sub_82241428(PPCContext& ctx, uint8_t* base) {
  // Run the retail check first so the profile's "told" mask stays current.
  __imp__sub_82241428(ctx, base);
  const int32_t retail = ctx.r3.s32;
  // One decision per launch; later title visits keep retail behaviour.
  const int32_t requested = requested_content.exchange(-2, std::memory_order_relaxed);
  if (requested == -2) return;
  int32_t result = kNoEpisode;
  if (requested > 0 && requested < 16) {
    const uint32_t installed = InstalledContentMask(ctx, base);
    if (installed & (1u << requested)) result = requested;
    REXLOG_INFO("Theft4 episode start: requested={} installed-mask={:04X} result={}",
                requested, installed, result);
  } else {
    REXLOG_INFO("Theft4 episode start: normal title (retail result {} not shown)", retail);
  }
  ctx.r3.s64 = result;
}
