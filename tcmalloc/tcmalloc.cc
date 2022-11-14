// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// tcmalloc is a fast malloc implementation.  See
// https://github.com/google/tcmalloc/tree/master/docs/design.md for a high-level description of
// how this malloc works.
//
// SYNCHRONIZATION
//  1. The thread-/cpu-specific lists are accessed without acquiring any locks.
//     This is safe because each such list is only accessed by one thread/cpu at
//     a time.
//  2. We have a lock per central free-list, and hold it while manipulating
//     the central free list for a particular size.
//  3. The central page allocator is protected by "pageheap_lock".
//  4. The pagemap (which maps from page-number to descriptor),
//     can be read without holding any locks, and written while holding
//     the "pageheap_lock".
//
//     This multi-threaded access to the pagemap is safe for fairly
//     subtle reasons.  We basically assume that when an object X is
//     allocated by thread A and deallocated by thread B, there must
//     have been appropriate synchronization in the handoff of object
//     X from thread A to thread B.
//
// PAGEMAP
// -------
// Page map contains a mapping from page id to Span.
//
// If Span s occupies pages [p..q],
//      pagemap[p] == s
//      pagemap[q] == s
//      pagemap[p+1..q-1] are undefined
//      pagemap[p-1] and pagemap[q+1] are defined:
//         NULL if the corresponding page is not yet in the address space.
//         Otherwise it points to a Span.  This span may be free
//         or allocated.  If free, it is in one of pageheap's freelist.

#include "tcmalloc/tcmalloc.h"

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/const_init.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/base/thread_annotations.h"
#include "absl/debugging/stacktrace.h"
#include "absl/memory/memory.h"
#include "absl/numeric/bits.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/strip.h"
#include "tcmalloc/allocation_sample.h"
#include "tcmalloc/central_freelist.h"
#include "tcmalloc/common.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/experiment.h"
#include "tcmalloc/global_stats.h"
#include "tcmalloc/guarded_page_allocator.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal_malloc_extension.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/malloc_tracing_extension.h"
#include "tcmalloc/new_extension.h"
#include "tcmalloc/page_allocator.h"
#include "tcmalloc/page_heap.h"
#include "tcmalloc/page_heap_allocator.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/sampled_allocation.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stack_trace_table.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/stats.h"
#include "tcmalloc/system-alloc.h"
#include "tcmalloc/tcmalloc_policy.h"
#include "tcmalloc/thread_cache.h"
#include "tcmalloc/transfer_cache.h"
#include "tcmalloc/transfer_cache_stats.h"

#if defined(TCMALLOC_HAVE_STRUCT_MALLINFO)
#include <malloc.h>
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Gets a human readable description of the current state of the malloc data
// structures. Returns the actual written size.
// [buffer, buffer+result] will contain NUL-terminated output string.
//
// REQUIRES: buffer_length > 0.
extern "C" ABSL_ATTRIBUTE_UNUSED int MallocExtension_Internal_GetStatsInPbtxt(
    char* buffer, int buffer_length) {
  ASSERT(buffer_length > 0);
  Printer printer(buffer, buffer_length);

  // Print level one stats unless lots of space is available
  if (buffer_length < 10000) {
    DumpStatsInPbtxt(&printer, 1);
  } else {
    DumpStatsInPbtxt(&printer, 2);
  }

  size_t required = printer.SpaceRequired();

  if (buffer_length > required) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    required += GetRegionFactory()->GetStatsInPbtxt(
        absl::Span<char>(buffer + required, buffer_length - required));
  }

  return required;
}

static void PrintStats(int level) {
  const int kBufferSize = 64 << 10;
  char* buffer = new char[kBufferSize];
  Printer printer(buffer, kBufferSize);
  DumpStats(&printer, level);
  (void)write(STDERR_FILENO, buffer, strlen(buffer));
  delete[] buffer;
}

// This function computes a profile that maps a live stack trace to
// the number of bytes of central-cache memory pinned by an allocation
// at that stack trace.
// In the case when span is hosting >= 1 number of small objects (t.proxy !=
// nullptr), we call span::Fragmentation() and read `span->allocated_`. It is
// safe to do so since we hold the per-sample lock while iterating over sampled
// allocations. It prevents the sampled allocation that has the proxy object to
// complete deallocation, thus `proxy` can not be returned to the span yet. It
// thus prevents the central free list to return the span to the page heap.
static std::unique_ptr<const ProfileBase> DumpFragmentationProfile() {
  auto profile = std::make_unique<StackTraceTable>(ProfileType::kFragmentation);
  tc_globals.sampled_allocation_recorder().Iterate(
      [&profile](const SampledAllocation& sampled_allocation) {
        // Compute fragmentation to charge to this sample:
        const StackTrace& t = sampled_allocation.sampled_stack;
        if (t.proxy == nullptr) {
          // There is just one object per-span, and neighboring spans
          // can be released back to the system, so we charge no
          // fragmentation to this sampled object.
          return;
        }

        // Fetch the span on which the proxy lives so we can examine its
        // co-residents.
        const PageId p = PageIdContaining(t.proxy);
        Span* span = tc_globals.pagemap().GetDescriptor(p);
        if (span == nullptr) {
          // Avoid crashes in production mode code, but report in tests.
          ASSERT(span != nullptr);
          return;
        }

        const double frag = span->Fragmentation(t.allocated_size);
        if (frag > 0) {
          // Associate the memory warmth with the actual object, not the proxy.
          // The residency information (t.span_start_address) is likely not very
          // useful, but we might as well pass it along.
          profile->AddTrace(frag, t);
        }
      });
  return profile;
}

static std::unique_ptr<const ProfileBase> DumpHeapProfile() {
  auto profile = std::make_unique<StackTraceTable>(ProfileType::kHeap);
  Residency r;
  tc_globals.sampled_allocation_recorder().Iterate(
      [&](const SampledAllocation& sampled_allocation) {
        profile->AddTrace(1.0, sampled_allocation.sampled_stack, &r);
      });
  return profile;
}

ABSL_CONST_INIT static AllocationSampleList allocation_samples_;

extern "C" void MallocExtension_Internal_GetStats(std::string* ret) {
  size_t shift = std::max<size_t>(18, absl::bit_width(ret->capacity()) - 1);
  for (; shift < 22; shift++) {
    const size_t size = 1 << shift;
    // Double ret's size until we succeed in writing the buffer without
    // truncation.
    //
    // TODO(b/142931922):  printer only writes data and does not read it.
    // Leverage https://wg21.link/P1072 when it is standardized.
    ret->resize(size - 1);

    size_t written_size = TCMalloc_Internal_GetStats(&*ret->begin(), size - 1);
    if (written_size < size - 1) {
      // We did not truncate.
      ret->resize(written_size);
      break;
    }
  }
}

extern "C" size_t TCMalloc_Internal_GetStats(char* buffer,
                                             size_t buffer_length) {
  Printer printer(buffer, buffer_length);
  if (buffer_length < 10000) {
    DumpStats(&printer, 1);
  } else {
    DumpStats(&printer, 2);
  }

  printer.printf("\nLow-level allocator stats:\n");
  printer.printf("Memory Release Failures: %d\n", SystemReleaseErrors());

  size_t n = printer.SpaceRequired();

  size_t bytes_remaining = buffer_length > n ? buffer_length - n : 0;
  if (bytes_remaining > 0) {
    n += GetRegionFactory()->GetStats(
        absl::Span<char>(buffer + n, bytes_remaining));
  }

  return n;
}

extern "C" const ProfileBase* MallocExtension_Internal_SnapshotCurrent(
    ProfileType type) {
  switch (type) {
    case ProfileType::kHeap:
      return DumpHeapProfile().release();
    case ProfileType::kFragmentation:
      return DumpFragmentationProfile().release();
    case ProfileType::kPeakHeap:
      return tc_globals.peak_heap_tracker().DumpSample().release();
    default:
      return nullptr;
  }
}

extern "C" AllocationProfilingTokenBase*
MallocExtension_Internal_StartAllocationProfiling() {
  return new AllocationSample(&allocation_samples_, absl::Now());
}

MallocExtension::Ownership GetOwnership(const void* ptr) {
  const PageId p = PageIdContaining(ptr);
  return tc_globals.pagemap().GetDescriptor(p)
             ? MallocExtension::Ownership::kOwned
             : MallocExtension::Ownership::kNotOwned;
}

extern "C" bool MallocExtension_Internal_GetNumericProperty(
    const char* name_data, size_t name_size, size_t* value) {
  return GetNumericProperty(name_data, name_size, value);
}

extern "C" void MallocExtension_Internal_GetMemoryLimit(
    MallocExtension::MemoryLimit* limit) {
  ASSERT(limit != nullptr);

  std::tie(limit->limit, limit->hard) = tc_globals.page_allocator().limit();
}

extern "C" void MallocExtension_Internal_SetMemoryLimit(
    const MallocExtension::MemoryLimit* limit) {
  ASSERT(limit != nullptr);

  if (!limit->hard) {
    Parameters::set_heap_size_hard_limit(0);
    tc_globals.page_allocator().set_limit(limit->limit, false /* !hard */);
  } else {
    Parameters::set_heap_size_hard_limit(limit->limit);
  }
}

extern "C" void MallocExtension_Internal_MarkThreadIdle() {
  ThreadCache::BecomeIdle();
}

extern "C" AddressRegionFactory* MallocExtension_Internal_GetRegionFactory() {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  return GetRegionFactory();
}

extern "C" void MallocExtension_Internal_SetRegionFactory(
    AddressRegionFactory* factory) {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  SetRegionFactory(factory);
}

// ReleaseMemoryToSystem drops the page heap lock while actually calling to
// kernel to release pages. To avoid confusing ourselves with
// extra_bytes_released handling, lets do separate lock just for release.
ABSL_CONST_INIT static absl::base_internal::SpinLock release_lock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);

extern "C" void MallocExtension_Internal_ReleaseMemoryToSystem(
    size_t num_bytes) {
  // ReleaseMemoryToSystem() might release more than the requested bytes because
  // the page heap releases at the span granularity, and spans are of wildly
  // different sizes.  This keeps track of the extra bytes bytes released so
  // that the app can periodically call ReleaseMemoryToSystem() to release
  // memory at a constant rate.
  ABSL_CONST_INIT static size_t extra_bytes_released;

  absl::base_internal::SpinLockHolder rh(&release_lock);

  absl::base_internal::SpinLockHolder h(&pageheap_lock);
  if (num_bytes <= extra_bytes_released) {
    // We released too much on a prior call, so don't release any
    // more this time.
    extra_bytes_released = extra_bytes_released - num_bytes;
    num_bytes = 0;
  } else {
    num_bytes = num_bytes - extra_bytes_released;
  }

  Length num_pages;
  if (num_bytes > 0) {
    // A sub-page size request may round down to zero.  Assume the caller wants
    // some memory released.
    num_pages = BytesToLengthCeil(num_bytes);
    ASSERT(num_pages > Length(0));
  } else {
    num_pages = Length(0);
  }
  size_t bytes_released =
      tc_globals.page_allocator().ReleaseAtLeastNPages(num_pages).in_bytes();
  if (bytes_released > num_bytes) {
    extra_bytes_released = bytes_released - num_bytes;
  } else {
    // The PageHeap wasn't able to release num_bytes.  Don't try to compensate
    // with a big release next time.
    extra_bytes_released = 0;
  }
}

// nallocx slow path.
// Moved to a separate function because size_class_with_alignment is not inlined
// which would cause nallocx to become non-leaf function with stack frame and
// stack spills. ABSL_ATTRIBUTE_ALWAYS_INLINE does not work on
// size_class_with_alignment, compiler barks that it can't inline the function
// somewhere.
static ABSL_ATTRIBUTE_NOINLINE size_t nallocx_slow(size_t size, int flags) {
  tc_globals.InitIfNecessary();
  size_t align = static_cast<size_t>(1ull << (flags & 0x3f));
  uint32_t size_class;
  if (ABSL_PREDICT_TRUE(tc_globals.sizemap().GetSizeClass(
          CppPolicy().AlignAs(align), size, &size_class))) {
    ASSERT(size_class != 0);
    return tc_globals.sizemap().class_to_size(size_class);
  } else {
    return BytesToLengthCeil(size).in_bytes();
  }
}

// The nallocx function allocates no memory, but it performs the same size
// computation as the malloc function, and returns the real size of the
// allocation that would result from the equivalent malloc function call.
// nallocx is a malloc extension originally implemented by jemalloc:
// http://www.unix.com/man-page/freebsd/3/nallocx/
extern "C" size_t nallocx(size_t size, int flags) noexcept {
#ifdef ENABLE_PROTECTION
  size++;
#endif
  if (ABSL_PREDICT_FALSE(!tc_globals.IsInited() || flags != 0)) {
    return nallocx_slow(size, flags);
  }
  uint32_t size_class;
  if (ABSL_PREDICT_TRUE(
          tc_globals.sizemap().GetSizeClass(CppPolicy(), size, &size_class))) {
    ASSERT(size_class != 0);
    return tc_globals.sizemap().class_to_size(size_class);
  } else {
    return BytesToLengthCeil(size).in_bytes();
  }
}

extern "C" MallocExtension::Ownership MallocExtension_Internal_GetOwnership(
    const void* ptr) {
  return GetOwnership(ptr);
}

extern "C" void MallocExtension_Internal_GetProperties(
    std::map<std::string, MallocExtension::Property>* result) {
  TCMallocStats stats;
  ExtractTCMallocStats(&stats, true);

  const uint64_t virtual_memory_used = VirtualMemoryUsed(stats);
  const uint64_t physical_memory_used = PhysicalMemoryUsed(stats);
  const uint64_t bytes_in_use_by_app = InUseByApp(stats);

  result->clear();
  // Virtual Memory Used
  (*result)["generic.virtual_memory_used"].value = virtual_memory_used;
  // Physical Memory used
  (*result)["generic.physical_memory_used"].value = physical_memory_used;
  // Bytes in use By App
  (*result)["generic.current_allocated_bytes"].value = bytes_in_use_by_app;
  (*result)["generic.bytes_in_use_by_app"].value = bytes_in_use_by_app;
  (*result)["generic.heap_size"].value = HeapSizeBytes(stats.pageheap);
  // Page Heap Free
  (*result)["tcmalloc.page_heap_free"].value = stats.pageheap.free_bytes;
  (*result)["tcmalloc.pageheap_free_bytes"].value = stats.pageheap.free_bytes;
  // Metadata Bytes
  (*result)["tcmalloc.metadata_bytes"].value = stats.metadata_bytes;
  // Heaps in Use
  (*result)["tcmalloc.thread_cache_count"].value = stats.tc_stats.in_use;
  // Central Cache Free List
  (*result)["tcmalloc.central_cache_free"].value = stats.central_bytes;
  // Transfer Cache Free List
  (*result)["tcmalloc.transfer_cache_free"].value = stats.transfer_bytes;
  // Per CPU Cache Free List
  (*result)["tcmalloc.cpu_free"].value = stats.per_cpu_bytes;
  (*result)["tcmalloc.sharded_transfer_cache_free"].value =
      stats.sharded_transfer_bytes;
  (*result)["tcmalloc.per_cpu_caches_active"].value =
      tc_globals.CpuCacheActive();
  // Thread Cache Free List
  (*result)["tcmalloc.current_total_thread_cache_bytes"].value =
      stats.thread_bytes;
  (*result)["tcmalloc.thread_cache_free"].value = stats.thread_bytes;
  (*result)["tcmalloc.local_bytes"].value = LocalBytes(stats);

  size_t overall_thread_cache_size;
  {
    absl::base_internal::SpinLockHolder l(&pageheap_lock);
    overall_thread_cache_size = ThreadCache::overall_thread_cache_size();
  }
  (*result)["tcmalloc.max_total_thread_cache_bytes"].value =
      overall_thread_cache_size;

  // Page Unmapped
  (*result)["tcmalloc.pageheap_unmapped_bytes"].value =
      stats.pageheap.unmapped_bytes;
  // Arena non-resident bytes aren't on the page heap, but they are unmapped.
  (*result)["tcmalloc.page_heap_unmapped"].value =
      stats.pageheap.unmapped_bytes + stats.arena.bytes_nonresident;
  (*result)["tcmalloc.sampled_internal_fragmentation"].value =
      tc_globals.sampled_internal_fragmentation_.value();

  (*result)["tcmalloc.page_algorithm"].value =
      tc_globals.page_allocator().algorithm();

  (*result)["tcmalloc.external_fragmentation_bytes"].value =
      ExternalBytes(stats);
  (*result)["tcmalloc.required_bytes"].value = RequiredBytes(stats);
  (*result)["tcmalloc.slack_bytes"].value = SlackBytes(stats.pageheap);

  size_t amount;
  bool is_hard;
  std::tie(amount, is_hard) = tc_globals.page_allocator().limit();
  if (is_hard) {
    (*result)["tcmalloc.hard_usage_limit_bytes"].value = amount;
    (*result)["tcmalloc.desired_usage_limit_bytes"].value =
        std::numeric_limits<size_t>::max();
  } else {
    (*result)["tcmalloc.hard_usage_limit_bytes"].value =
        std::numeric_limits<size_t>::max();
    (*result)["tcmalloc.desired_usage_limit_bytes"].value = amount;
  }

  WalkExperiments([&](absl::string_view name, bool active) {
    (*result)[absl::StrCat("tcmalloc.experiment.", name)].value = active;
  });
}

extern "C" size_t MallocExtension_Internal_ReleaseCpuMemory(int cpu) {
  size_t bytes = 0;
  if (tc_globals.CpuCacheActive()) {
    bytes = tc_globals.cpu_cache().Reclaim(cpu);
  }
  return bytes;
}

//-------------------------------------------------------------------
// Helpers for the exported routines below
//-------------------------------------------------------------------

ABSL_CONST_INIT static thread_local Sampler thread_sampler_
    ABSL_ATTRIBUTE_INITIAL_EXEC;

inline Sampler* GetThreadSampler() { return &thread_sampler_; }

enum class Hooks { RUN, NO };

static void FreeSmallSlow(void* ptr, size_t size_class);

namespace {

// Sets `*psize` to `size`,
inline void SetCapacity(size_t size, std::nullptr_t) {}
inline void SetCapacity(size_t size, size_t* psize) { *psize = size; }

// Sets `*psize` to the size for the size class in `size_class`,
inline void SetClassCapacity(size_t size, std::nullptr_t) {}
inline void SetClassCapacity(uint32_t size_class, size_t* psize) {
  *psize = tc_globals.sizemap().class_to_size(size_class);
}

// Sets `*psize` to the size for the size class in `size_class` if `ptr` is not
// null, else `*psize` is set to 0. This method is overloaded for `nullptr_t`
// below, allowing the compiler to optimize code between regular and size
// returning allocation operations.
inline void SetClassCapacity(const void*, uint32_t, std::nullptr_t) {}
inline void SetClassCapacity(const void* ptr, uint32_t size_class,
                             size_t* psize) {
  if (ABSL_PREDICT_TRUE(ptr != nullptr)) {
    *psize = tc_globals.sizemap().class_to_size(size_class);
  } else {
    *psize = 0;
  }
}

// Sets `*psize` to the size in pages corresponding to the requested size in
// `size` if `ptr` is not null, else `*psize` is set to 0. This method is
// overloaded for `nullptr_t` below, allowing the compiler to optimize code
// between regular and size returning allocation operations.
inline void SetPagesCapacity(const void*, Length, std::nullptr_t) {}
inline void SetPagesCapacity(const void* ptr, Length size, size_t* psize) {
  if (ABSL_PREDICT_TRUE(ptr != nullptr)) {
    *psize = size.in_bytes();
  } else {
    *psize = 0;
  }
}

}  // namespace

// In free fast-path we handle delete hooks by delegating work to slower
// function that both performs delete hooks calls and does free. This is done so
// that free fast-path only does tail calls, which allow compiler to avoid
// generating costly prologue/epilogue for fast-path.
template <void F(void*, size_t), Hooks hooks_state>
static ABSL_ATTRIBUTE_SECTION(google_malloc) void invoke_delete_hooks_and_free(
    void* ptr, size_t t) {
  // Refresh the fast path state.
  GetThreadSampler()->UpdateFastPathState();
  return F(ptr, t);
}

template <void F(void*, PageId), Hooks hooks_state>
static ABSL_ATTRIBUTE_SECTION(google_malloc) void invoke_delete_hooks_and_free(
    void* ptr, PageId p) {
  // Refresh the fast path state.
  GetThreadSampler()->UpdateFastPathState();
  return F(ptr, p);
}

// Helper for do_free_with_size_class
template <Hooks hooks_state>
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE void FreeSmall(void* ptr,
                                                          size_t size_class) {
  if (!IsExpandedSizeClass(size_class)) {
    ASSERT(IsNormalMemory(ptr));
  } else {
    ASSERT(IsColdMemory(ptr));
  }
  if (ABSL_PREDICT_FALSE(!GetThreadSampler()->IsOnFastPath())) {
    // Take the slow path.
    invoke_delete_hooks_and_free<FreeSmallSlow, hooks_state>(ptr, size_class);
    return;
  }

#ifndef TCMALLOC_DEPRECATED_PERTHREAD
  // The CPU Cache is enabled, so we're able to take the fastpath.
  ASSERT(tc_globals.CpuCacheActive());
  ASSERT(subtle::percpu::IsFastNoInit());

  tc_globals.cpu_cache().Deallocate(ptr, size_class);
#else   // TCMALLOC_DEPRECATED_PERTHREAD
  ThreadCache* cache = ThreadCache::GetCacheIfPresent();

  // IsOnFastPath does not track whether or not we have an active ThreadCache on
  // this thread, so we need to check cache for nullptr.
  if (ABSL_PREDICT_FALSE(cache == nullptr)) {
    FreeSmallSlow(ptr, size_class);
    return;
  }

  cache->Deallocate(ptr, size_class);
#endif  // TCMALLOC_DEPRECATED_PERTHREAD
}

// this helper function is used when FreeSmall (defined above) hits
// the case of thread state not being in per-cpu mode or hitting case
// of no thread cache. This happens when thread state is not yet
// properly initialized with real thread cache or with per-cpu mode,
// or when thread state is already destroyed as part of thread
// termination.
//
// We explicitly prevent inlining it to keep it out of fast-path, so
// that fast-path only has tail-call, so that fast-path doesn't need
// function prologue/epilogue.
ABSL_ATTRIBUTE_NOINLINE
static void FreeSmallSlow(void* ptr, size_t size_class) {
  if (ABSL_PREDICT_TRUE(UsePerCpuCache())) {
    tc_globals.cpu_cache().Deallocate(ptr, size_class);
  } else if (ThreadCache* cache = ThreadCache::GetCacheIfPresent()) {
    // TODO(b/134691947):  If we reach this path from the ThreadCache fastpath,
    // we've already checked that UsePerCpuCache is false and cache == nullptr.
    // Consider optimizing this.
    cache->Deallocate(ptr, size_class);
  } else {
    // This thread doesn't have thread-cache yet or already. Delete directly
    // into central cache.
    tc_globals.transfer_cache().InsertRange(size_class,
                                            absl::Span<void*>(&ptr, 1));
  }
}

namespace {

// If this allocation can be guarded, and if it's time to do a guarded sample,
// returns a guarded allocation Span.  Otherwise returns nullptr.
static void* TrySampleGuardedAllocation(size_t size, size_t alignment,
                                        Length num_pages) {
  if (num_pages == Length(1) &&
      GetThreadSampler()->ShouldSampleGuardedAllocation()) {
    // The num_pages == 1 constraint ensures that size <= kPageSize.  And since
    // alignments above kPageSize cause size_class == 0, we're also guaranteed
    // alignment <= kPageSize
    //
    // In all cases kPageSize <= GPA::page_size_, so Allocate's preconditions
    // are met.
    return tc_globals.guardedpage_allocator().Allocate(size, alignment);
  }
  return nullptr;
}

// Performs sampling for already occurred allocation of object.
//
// For very small object sizes, object is used as 'proxy' and full
// page with sampled marked is allocated instead.
//
// For medium-sized objects that have single instance per span,
// they're simply freed and fresh page span is allocated to represent
// sampling.
//
// For large objects (i.e. allocated with do_malloc_pages) they are
// also fully reused and their span is marked as sampled.
//
// Note that do_free_with_size assumes sampled objects have
// page-aligned addresses. Please change both functions if need to
// invalidate the assumption.
//
// Note that size_class might not match requested_size in case of
// memalign. I.e. when larger than requested allocation is done to
// satisfy alignment constraint.
//
// In case of out-of-memory condition when allocating span or
// stacktrace struct, this function simply cheats and returns original
// object. As if no sampling was requested.
template <typename Policy>
static void* SampleifyAllocation(Policy policy, size_t requested_size,
                                 size_t weight, size_t size_class, void* obj,
                                 Span* span, size_t* capacity) {
  CHECK_CONDITION((size_class != 0 && obj != nullptr && span == nullptr) ||
                  (size_class == 0 && obj == nullptr && span != nullptr));

  void* proxy = nullptr;
  void* guarded_alloc = nullptr;
  size_t allocated_size;
  bool allocated_cold;

  // requested_alignment = 1 means 'small size table alignment was used'
  // Historically this is reported as requested_alignment = 0
  size_t requested_alignment = policy.align();
  if (requested_alignment == 1) {
    requested_alignment = 0;
  }

  if (size_class != 0) {
    ASSERT(size_class == tc_globals.pagemap().sizeclass(PageIdContaining(obj)));

    allocated_size = tc_globals.sizemap().class_to_size(size_class);
    allocated_cold = IsExpandedSizeClass(size_class);

    // If the caller didn't provide a span, allocate one:
    Length num_pages = BytesToLengthCeil(allocated_size);
    if ((guarded_alloc = TrySampleGuardedAllocation(
             requested_size, requested_alignment, num_pages))) {
      ASSERT(IsSampledMemory(guarded_alloc));
      const PageId p = PageIdContaining(guarded_alloc);
      absl::base_internal::SpinLockHolder h(&pageheap_lock);
      span = Span::New(p, num_pages);
      for (Length i=Length(0); i<num_pages; ++i) {
        tc_globals.pagemap().Set(p+i, span);
      }

      size_t span_size =
        Length(tc_globals.sizemap().class_to_pages(size_class)).in_bytes();
      span->obj_size = allocated_size / 8;
      span->objects_per_span = span_size / allocated_size;
      // If we report capacity back from a size returning allocation, we can not
      // report the allocated_size, as we guard the size to 'requested_size',
      // and we maintain the invariant that GetAllocatedSize() must match the
      // returned size from size returning allocations. So in that case, we
      // report the requested size for both capacity and GetAllocatedSize().
      if (capacity) allocated_size = requested_size;
    } else if ((span = tc_globals.page_allocator().New(
                    num_pages, 1, MemoryTag::kSampled)) == nullptr) {
      if (capacity) *capacity = allocated_size;
      return obj;
    }

    size_t span_size =
        Length(tc_globals.sizemap().class_to_pages(size_class)).in_bytes();
    size_t objects_per_span = span_size / allocated_size;

    if (objects_per_span != 1) {
      ASSERT(objects_per_span > 1);
      proxy = obj;
      obj = nullptr;
    }
  } else {
    // Set allocated_size to the exact size for a page allocation.
    // NOTE: if we introduce gwp-asan sampling / guarded allocations
    // for page allocations, then we need to revisit do_malloc_pages as
    // the current assumption is that only class sized allocs are sampled
    // for gwp-asan.
    allocated_size = span->bytes_in_span();
    allocated_cold = IsColdMemory(span->start_address());
  }
  if (capacity) *capacity = allocated_size;

  ASSERT(span != nullptr);

  // Grab the stack trace outside the heap lock.
  StackTrace tmp;
  tmp.proxy = proxy;
  tmp.depth = absl::GetStackTrace(tmp.stack, kMaxStackDepth, 0);
  tmp.requested_size = requested_size;
  tmp.requested_alignment = requested_alignment;
  tmp.requested_size_returning = capacity != nullptr;
  tmp.allocated_size = allocated_size;
  tmp.access_hint = static_cast<uint8_t>(policy.access());
  tmp.cold_allocated = allocated_cold;
  tmp.weight = weight;
  tmp.span_start_address = span->start_address();
  tmp.allocation_time = absl::Now();

  // How many allocations does this sample represent, given the sampling
  // frequency (weight) and its size.
  const double allocation_estimate =
      static_cast<double>(weight) / (requested_size + 1);

  // Adjust our estimate of internal fragmentation.
  ASSERT(requested_size <= allocated_size);
  if (requested_size < allocated_size) {
    tc_globals.sampled_internal_fragmentation_.Add(
        allocation_estimate * (allocated_size - requested_size));
  }

  allocation_samples_.ReportMalloc(tmp);

  // The SampledAllocation object is visible to readers after this. Readers only
  // care about its various metadata (e.g. stack trace, weight) to generate the
  // heap profile, and won't need any information from Span::Sample() next.
  SampledAllocation* sampled_allocation =
      tc_globals.sampled_allocation_recorder().Register(std::move(tmp));
  // No pageheap_lock required. The span is freshly allocated and no one else
  // can access it. It is visible after we return from this allocation path.
  span->Sample(sampled_allocation);
  span->obj_size = allocated_size / 8;
  span->objects_per_span = span->bytes_in_span() / allocated_size;

  // if we register the size class here, tcmalloc crashes
  // if (size_class != 0)
  //   tc_globals.pagemap().RegisterSizeClass(span, size_class);

  tc_globals.peak_heap_tracker().MaybeSaveSample();

  if (obj != nullptr) {
    // We are not maintaining precise statistics on malloc hit/miss rates at our
    // cache tiers.  We can deallocate into our ordinary cache.
    ASSERT(size_class != 0);
    FreeSmallSlow(obj, size_class);
  }
  return guarded_alloc ? guarded_alloc : span->start_address();
}

// ShouldSampleAllocation() is called when an allocation of the given requested
// size is in progress. It returns the sampling weight of the allocation if it
// should be "sampled," and 0 otherwise. See SampleifyAllocation().
//
// Sampling is done based on requested sizes and later unskewed during profile
// generation.
inline size_t ShouldSampleAllocation(size_t size) {
  return GetThreadSampler()->RecordAllocation(size);
}

inline size_t GetSize(const void* ptr) {
  if (ptr == nullptr) return 0;
  const PageId p = PageIdContaining(ptr);
  size_t size_class = tc_globals.pagemap().sizeclass(p);
  if (size_class != 0) {
    return tc_globals.sizemap().class_to_size(size_class);
  } else {
    const Span* span = tc_globals.pagemap().GetExistingDescriptor(p);
    if (span->sampled()) {
      if (tc_globals.guardedpage_allocator().PointerIsMine(ptr)) {
        return tc_globals.guardedpage_allocator().GetRequestedSize(ptr);
      }
      return span->sampled_allocation()->sampled_stack.allocated_size;
    } else {
      return span->bytes_in_span();
    }
  }
}

static inline struct escape** alloc_escape_list() {
  struct escape **list = (struct escape **)Static::escape_list_allocator().New();
  memset(list, 0, 1024*sizeof(struct escape *));
  return list;
}

static inline void delete_escape_list(struct escape **list) {
  Static::escape_list_allocator().Delete(reinterpret_cast<EscapeList*>(list));
}

static inline struct escape* alloc_escape() {
  // no need to zero memory
  return (struct escape *)Static::escape_allocator().New();
}

static inline void delete_escape(struct escape *e) {
  Static::escape_allocator().Delete(reinterpret_cast<EscapeChunk*>(e));
}

static inline void insert_escape(void **loc, void *ptr,
    unsigned idx) {
  // todo
  return;
}

static inline void poison_escapes(Span *span, int idx,
    void *ptr, void *end) {
  struct escape **escape_list = span->escape_list;
  if (!escape_list || !escape_list[idx])
    return;

  struct escape* cur = escape_list[idx];
  while (cur) {
    struct escape *next = cur->next;
    void* cur_addr = *(reinterpret_cast<void**>(cur->loc));
    if (ptr <= cur_addr && cur_addr < end) {
      // *(reinterpret_cast<size_t*>(cur->loc)) |= (size_t) 0xdeadbeef00000000;
    }
    delete_escape(cur);
    cur = next;
  }
  escape_list[idx] = nullptr;
}

static inline void clear_old_escape(void *ptr, void *loc) {
  Span *span = tc_globals.pagemap().GetDescriptor(PageIdContaining(ptr));
  if (span != nullptr) {
    span->Prefetch();

    // It is possible that ptr points to a span in the freelist
    // for page_heap maintained span, span in the freelist still has
    // page table entries, but the escape_list should be null
    if (!span->escape_list || !span->obj_size)
      return;
    unsigned idx = ((size_t)ptr - (size_t)span->start_address()) / (span->obj_size * 8ULL);
    CHECK_CONDITION(idx < span->objects_per_span);
    if (!span->escape_list[idx])
      return;
    struct escape *cur, *pre;
    for (pre=nullptr, cur=span->escape_list[idx]; cur; cur = cur->next) {
      if (cur->loc == loc) {
        if (pre) {
          pre->next = cur->next;
        } else {
          span->escape_list[idx] = cur->next;
        }
        delete_escape(cur);
        break;
      }
      pre = cur;
    }
  }
}

template <typename Policy, typename CapacityPtr = std::nullptr_t>
inline void* do_malloc_pages(Policy policy, size_t size, int num_objects,
                             CapacityPtr capacity = nullptr) {
  // Page allocator does not deal well with num_pages = 0.
  Length num_pages = std::max<Length>(BytesToLengthCeil(size), Length(1));

  MemoryTag tag = MemoryTag::kNormal;
  if (IsColdHint(policy.access())) {
    tag = MemoryTag::kCold;
  } else if (tc_globals.numa_topology().numa_aware()) {
    tag = NumaNormalTag(policy.numa_partition());
  }
  Span* span = tc_globals.page_allocator().NewAligned(
      num_pages, BytesToLengthCeil(policy.align()), num_objects, tag);

  if (span == nullptr) {
    SetPagesCapacity(nullptr, Length(0), capacity);
    return nullptr;
  }

  void* result = span->start_address();
  ASSERT(!ColdFeatureActive() || tag == GetMemoryTag(span->start_address()));

  // Set capacity to the exact size for a page allocation.  This needs to be
  // revisited if we introduce gwp-asan sampling / guarded allocations to
  // do_malloc_pages().
  SetPagesCapacity(result, num_pages, capacity);

  if (size_t weight = ShouldSampleAllocation(size)) {
    CHECK_CONDITION(result == SampleifyAllocation(policy, size, weight, 0,
                                                  nullptr, span, capacity));
  }

  span->objects_per_span = (uint32_t)num_objects;
  span->obj_size = (uint32_t) (GetSize(result) / 8);
  return result;
}

template <typename Policy, typename CapacityPtr>
inline void* ABSL_ATTRIBUTE_ALWAYS_INLINE AllocSmall(Policy policy,
                                                     size_t size_class,
                                                     size_t size,
                                                     CapacityPtr capacity) {
  ASSERT(size_class != 0);
  void* result;

  if (UsePerCpuCache()) {
    result = tc_globals.cpu_cache().Allocate<Policy::handle_oom>(size_class);
  } else {
    result = ThreadCache::GetCache()->Allocate<Policy::handle_oom>(size_class);
  }

  if (!Policy::can_return_nullptr()) {
    ASSUME(result != nullptr);
  }

  if (ABSL_PREDICT_FALSE(result == nullptr)) {
    SetCapacity(0, capacity);
    return nullptr;
  }
  size_t weight;
  if (ABSL_PREDICT_FALSE(weight = ShouldSampleAllocation(size))) {
    return SampleifyAllocation(policy, size, weight, size_class, result,
                               nullptr, capacity);
  }
  SetClassCapacity(size_class, capacity);
  return result;
}

// Handles freeing object that doesn't have size class, i.e. which
// is either large or sampled. We explicitly prevent inlining it to
// keep it out of fast-path. This helps avoid expensive
// prologue/epilogue for fast-path freeing functions.
ABSL_ATTRIBUTE_NOINLINE
static void do_free_pages(void* ptr, const PageId p) {
  Span* span = tc_globals.pagemap().GetExistingDescriptor(p);
  CHECK_CONDITION(span != nullptr && "Possible double free detected");
  // Prefetch now to avoid a stall accessing *span while under the lock.
  span->Prefetch();

  // No pageheap_lock required. The sampled span should be unmarked and have its
  // state cleared only once. External synchronization when freeing is required;
  // otherwise, concurrent writes here would likely report a double-free.
  if (SampledAllocation* sampled_allocation = span->Unsample()) {
    void* const proxy = sampled_allocation->sampled_stack.proxy;
    const size_t weight = sampled_allocation->sampled_stack.weight;
    const size_t requested_size =
        sampled_allocation->sampled_stack.requested_size;
    const size_t allocated_size =
        sampled_allocation->sampled_stack.allocated_size;
    const size_t alignment =
        sampled_allocation->sampled_stack.requested_alignment;
    // How many allocations does this sample represent, given the sampling
    // frequency (weight) and its size.
    const double allocation_estimate =
        static_cast<double>(weight) / (requested_size + 1);
    tc_globals.sampled_allocation_recorder().Unregister(sampled_allocation);

    // Adjust our estimate of internal fragmentation.
    ASSERT(requested_size <= allocated_size);
    if (requested_size < allocated_size) {
      const size_t sampled_fragmentation =
          allocation_estimate * (allocated_size - requested_size);

      // Check against wraparound
      ASSERT(tc_globals.sampled_internal_fragmentation_.value() >=
             sampled_fragmentation);
      tc_globals.sampled_internal_fragmentation_.Add(-sampled_fragmentation);
    }

    if (proxy) {
      const auto policy = CppPolicy().InSameNumaPartitionAs(proxy);
      size_t size_class;
      if (AccessFromPointer(proxy) == AllocationAccess::kCold) {
        size_class = tc_globals.sizemap().SizeClass(
            policy.AccessAsCold().AlignAs(alignment), allocated_size);
      } else {
        size_class = tc_globals.sizemap().SizeClass(
            policy.AccessAsHot().AlignAs(alignment), allocated_size);
      }
      ASSERT(size_class ==
             tc_globals.pagemap().sizeclass(PageIdContaining(proxy)));
      FreeSmall<Hooks::NO>(proxy, size_class);
    }
  }

  {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    ASSERT(span->first_page() == p);
    if (IsSampledMemory(ptr)) {
      if (tc_globals.guardedpage_allocator().PointerIsMine(ptr)) {
        // Release lock while calling Deallocate() since it does a system call.
        pageheap_lock.Unlock();
        tc_globals.guardedpage_allocator().Deallocate(ptr);
        pageheap_lock.Lock();
        span->DestroyEscape();
        for (PageId p = span->first_page(); p <= span->last_page(); ++p) {
          tc_globals.pagemap().Set(p, nullptr);
        }
        Span::Delete(span);
      } else if (IsColdMemory(ptr)) {
        ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
        tc_globals.page_allocator().Delete(span, 1, MemoryTag::kCold);
    } else {
        ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
        tc_globals.page_allocator().Delete(span, 1, MemoryTag::kSampled);
      }
    } else if (kNumaPartitions != 1) {
      ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
      tc_globals.page_allocator().Delete(span, 1, GetMemoryTag(ptr));
    } else {
      ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
      tc_globals.page_allocator().Delete(span, 1, MemoryTag::kNormal);
    }
  }
}

#ifndef NDEBUG
static size_t GetSizeClass(void* ptr) {
  const PageId p = PageIdContaining(ptr);
  return tc_globals.pagemap().sizeclass(p);
}
#endif

// Helper for the object deletion (free, delete, etc.).  Inputs:
//   ptr is object to be freed
//   size_class is the size class of that object, or 0 if it's unknown
//   have_size_class is true iff size_class is known and is non-0.
//
// Note that since have_size_class is compile-time constant, genius compiler
// would not need it. Since it would be able to somehow infer that
// GetSizeClass never produces 0 size_class, and so it
// would know that places that call this function with explicit 0 is
// "have_size_class-case" and others are "!have_size_class-case". But we
// certainly don't have such compiler. See also do_free_with_size below.
template <bool have_size_class, Hooks hooks_state>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void do_free_with_size_class(
    void* ptr, size_t size_class) {
  // !have_size_class -> size_class == 0
  ASSERT(have_size_class || size_class == 0);

  const PageId p = PageIdContaining(ptr);

  // if we have_size_class, then we've excluded ptr == nullptr case. See
  // comment in do_free_with_size. Thus we only bother testing nullptr
  // in non-sized case.
  //
  // Thus: ptr == nullptr -> !have_size_class
  ASSERT(ptr != nullptr || !have_size_class);
  if (!have_size_class && ABSL_PREDICT_FALSE(ptr == nullptr)) {
    return;
  }

  // ptr must be a result of a previous malloc/memalign/... call, and
  // therefore static initialization must have already occurred.
  ASSERT(tc_globals.IsInited());

#ifdef ENABLE_STATISTIC
  tc_globals.free_cnt++;
#endif

#ifdef ENABLE_PROTECTION
  Span* span_ = tc_globals.pagemap().GetDescriptor(p);
  if (span_) {
    // check if is invalid free
    // if sizeclass is 0, then the span is dedicated to the page
    // the check is done in the following.
    // fixme
    size_t obj_size = GetSize(ptr);
    CHECK_CONDITION(obj_size == span_->obj_size * 8ULL);
    CHECK_CONDITION(obj_size != 0);
    size_t start_addr = (size_t)span_->start_address();
    if (((size_t)ptr - start_addr) % obj_size != 0) {
      Log(kLogWithStack, __FILE__, __LINE__,
          "double/invalid free detected");
#ifdef CRASH_ON_CORRUPTION
      fflush(stdout);
      abort();
#endif
      return;
    }
    // free all escapes to p
    int idx = ((size_t)ptr - start_addr) / obj_size;
    poison_escapes(span_, idx, ptr, (char*)ptr + obj_size);
  } else {
    if ((reinterpret_cast<uintptr_t>(ptr) & 0xdeadbeef00000000) == 0xdeadbeef00000000) {
      Log(kLogWithStack, __FILE__, __LINE__,
        "double/invalid free detected");
    } else {
      Log(kLogWithStack, __FILE__, __LINE__,
        "freeing a pointer with no span", ptr);
    }
#ifdef CRASH_ON_CORRUPTION
    fflush(stdout);
    abort();
#endif
    return;
  }
#endif

  if (!have_size_class) {
    size_class = tc_globals.pagemap().sizeclass(p);
  }
  if (have_size_class || ABSL_PREDICT_TRUE(size_class != 0)) {
    ASSERT(size_class == GetSizeClass(ptr));
    ASSERT(ptr != nullptr);
    ASSERT(!tc_globals.pagemap().GetExistingDescriptor(p)->sampled());
    FreeSmall<hooks_state>(ptr, size_class);
  } else {
    invoke_delete_hooks_and_free<do_free_pages, hooks_state>(ptr, p);
  }
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void do_free(void* ptr) {
  return do_free_with_size_class<false, Hooks::RUN>(ptr, 0);
}

void do_free_no_hooks(void* ptr) {
  return do_free_with_size_class<false, Hooks::NO>(ptr, 0);
}

template <typename AlignPolicy>
bool CorrectSize(void* ptr, size_t size, AlignPolicy align);

bool CorrectAlignment(void* ptr, std::align_val_t alignment);

inline ABSL_ATTRIBUTE_ALWAYS_INLINE void FreePages(void* ptr) {
  const PageId p = PageIdContaining(ptr);
  invoke_delete_hooks_and_free<do_free_pages, Hooks::RUN>(ptr, p);
}

template <typename AlignPolicy>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void do_free_with_size(void* ptr,
                                                           size_t size,
                                                           AlignPolicy align) {
  ASSERT(CorrectSize(ptr, size, align));
  ASSERT(CorrectAlignment(ptr, static_cast<std::align_val_t>(align.align())));

#ifdef ENABLE_STATISTIC
  tc_globals.free_cnt++;
#endif

#ifdef ENABLE_PROTECTION
  PageId p = PageIdContaining(ptr);
  Span* span_ = tc_globals.pagemap().GetDescriptor(p);
  if (span_) {
    // check if is invalid free
    // if sizeclass is 0, then the span is dedicated to the page
    // the check is done in the following.
    // fixme
    size_t obj_size = GetSize(ptr);
    CHECK_CONDITION(obj_size == span_->obj_size * 8ULL);
    CHECK_CONDITION(obj_size != 0);
    size_t start_addr = (size_t)span_->start_address();
    if (((size_t)ptr - start_addr) % obj_size != 0) {
      Log(kLogWithStack, __FILE__, __LINE__,
          "double/invalid free detected");
#ifdef CRASH_ON_CORRUPTION
      fflush(stdout);
      abort();
#endif
      return;
    }
    // free all escapes to p
    int idx = ((size_t)ptr - start_addr) / obj_size;
    poison_escapes(span_, idx, ptr, (char*)ptr + obj_size);
  } else {
    if ((reinterpret_cast<uintptr_t>(ptr) & 0xdeadbeef00000000) == 0xdeadbeef00000000) {
      Log(kLogWithStack, __FILE__, __LINE__,
        "double/invalid free detected");
    } else {
      Log(kLogWithStack, __FILE__, __LINE__,
        "freeing a pointer with no span", ptr);
    }
#ifdef CRASH_ON_CORRUPTION
    fflush(stdout);
    abort();
#endif
    return;
  }
#endif

  // This is an optimized path that may be taken if the binary is compiled
  // with -fsized-delete. We attempt to discover the size class cheaply
  // without any cache misses by doing a plain computation that
  // maps from size to size-class.
  //
  // The optimized path doesn't work with sampled objects, whose deletions
  // trigger more operations and require to visit metadata.
  if (ABSL_PREDICT_FALSE(IsSampledMemory(ptr))) {
    // IsColdMemory(ptr) implies IsSampledMemory(ptr).
    if (!IsColdMemory(ptr)) {
      // we don't know true class size of the ptr
      if (ptr == nullptr) return;
      return FreePages(ptr);
    } else {
      // TODO(b/124707070):  Dedupe this with the code below, once this path is
      // used more frequently.
      ASSERT(ptr != nullptr);

      uint32_t size_class;
      if (ABSL_PREDICT_FALSE(!tc_globals.sizemap().GetSizeClass(
              CppPolicy().AlignAs(align.align()).AccessAsCold(), size,
              &size_class))) {
        // We couldn't calculate the size class, which means size > kMaxSize.
        ASSERT(size > kMaxSize || align.align() > alignof(std::max_align_t));
        static_assert(kMaxSize >= kPageSize,
                      "kMaxSize must be at least kPageSize");
        return FreePages(ptr);
      }

      return do_free_with_size_class<true, Hooks::RUN>(ptr, size_class);
    }
  }

  // At this point, since ptr's tag bit is 1, it means that it
  // cannot be nullptr either. Thus all code below may rely on ptr !=
  // nullptr. And particularly, since we're only caller of
  // do_free_with_size_class with have_size_class == true, it means
  // have_size_class implies ptr != nullptr.
  ASSERT(ptr != nullptr);

  uint32_t size_class;
  if (ABSL_PREDICT_FALSE(!tc_globals.sizemap().GetSizeClass(
          CppPolicy().AlignAs(align.align()).InSameNumaPartitionAs(ptr), size,
          &size_class))) {
    // We couldn't calculate the size class, which means size > kMaxSize.
    ASSERT(size > kMaxSize || align.align() > alignof(std::max_align_t));
    static_assert(kMaxSize >= kPageSize, "kMaxSize must be at least kPageSize");
    return FreePages(ptr);
  }

  return do_free_with_size_class<true, Hooks::RUN>(ptr, size_class);
}

// Checks that an asserted object size for <ptr> is valid.
template <typename AlignPolicy>
bool CorrectSize(void* ptr, size_t size, AlignPolicy align) {
  // size == 0 means we got no hint from sized delete, so we certainly don't
  // have an incorrect one.
  if (size == 0) return true;
  if (ptr == nullptr) return true;
  uint32_t size_class = 0;
  // Round-up passed in size to how much tcmalloc allocates for that size.
  if (tc_globals.guardedpage_allocator().PointerIsMine(ptr)) {
    size = tc_globals.guardedpage_allocator().GetRequestedSize(ptr);
  } else if (tc_globals.sizemap().GetSizeClass(
                 CppPolicy().AlignAs(align.align()), size, &size_class)) {
    size = tc_globals.sizemap().class_to_size(size_class);
  } else {
    size = BytesToLengthCeil(size).in_bytes();
  }
  size_t actual = GetSize(ptr);
  if (ABSL_PREDICT_TRUE(actual == size)) return true;
  // We might have had a cold size class, which then sampled, so actual > size.
  // Let's check that.
  //
  // TODO(b/124707070):  When we grow a sampled allocation in this way,
  // recompute the true size at allocation time.  This allows size-feedback from
  // operator new to benefit from the bytes we are allocating.
  if (actual > size && IsSampledMemory(ptr)) {
    if (tc_globals.sizemap().GetSizeClass(
            CppPolicy().AlignAs(align.align()).AccessAsCold(), size,
            &size_class)) {
      size = tc_globals.sizemap().class_to_size(size_class);
      if (actual == size) {
        return true;
      }
    }
  }
  Log(kLog, __FILE__, __LINE__, "size check failed", actual, size, size_class);
  return false;
}

// Checks that an asserted object <ptr> has <align> alignment.
bool CorrectAlignment(void* ptr, std::align_val_t alignment) {
  size_t align = static_cast<size_t>(alignment);
  ASSERT(absl::has_single_bit(align));
  return ((reinterpret_cast<uintptr_t>(ptr) & (align - 1)) == 0);
}

// Helpers for use by exported routines below or inside debugallocation.cc:

inline void do_malloc_stats() { PrintStats(1); }

#ifdef TCMALLOC_HAVE_MALLOC_TRIM
inline int do_malloc_trim(size_t pad) {
  return 0;  // Indicate no memory released
}
#endif

inline int do_mallopt(int cmd, int value) {
  return 1;  // Indicates error
}

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
inline struct mallinfo do_mallinfo() {
  TCMallocStats stats;
  ExtractTCMallocStats(&stats, false);

  // Just some of the fields are filled in.
  struct mallinfo info;
  memset(&info, 0, sizeof(info));

  // Unfortunately, the struct contains "int" field, so some of the
  // size values will be truncated.
  info.arena = static_cast<int>(stats.pageheap.system_bytes);
  info.fsmblks = static_cast<int>(stats.thread_bytes + stats.central_bytes +
                                  stats.transfer_bytes);
  info.fordblks = static_cast<int>(stats.pageheap.free_bytes +
                                   stats.pageheap.unmapped_bytes);
  info.uordblks = static_cast<int>(InUseByApp(stats));

  return info;
}
#endif  // TCMALLOC_HAVE_STRUCT_MALLINFO

static inline size_t do_get_chunk_end(void* base) noexcept {
#ifdef ENABLE_STATISTIC
  tc_globals.get_end_cnt++;
#endif
  const PageId p = PageIdContaining(base);
  size_t start_addr, obj_size;
  Span* span;

  size_t page_info = tc_globals.pagemap().get_page_info(p);
  size_t size_class = page_info & (CompactSizeClass)(-1);
  if (size_class != 0) {
    obj_size = tc_globals.sizemap().class_to_size(size_class);
    start_addr = (size_t)PageId(page_info >> (sizeof(CompactSizeClass) * 8))
                     .start_addr();
  } else {
    span = tc_globals.pagemap().GetDescriptor(p);
    if (!span) {
      return 0x1000000000000;
    }
    obj_size = span->obj_size * 8ULL;
    start_addr = (size_t)span->start_address();
  }

  size_t chunk_start =
      (size_t)(start_addr) +
      (((size_t)base - (size_t)(start_addr)) / obj_size) * obj_size;
  size_t chunk_end = chunk_start + obj_size;

  return chunk_end;
}

static inline void* do_strncpy_check(void* _dst, void* _src, size_t maxlen) noexcept {
  char* dst_end = (char*)do_get_chunk_end(_dst);
  char* src_end = (char*)do_get_chunk_end(_src);

  char* dst = (char*)_dst;
  char* src = (char*)_src;
  size_t i = 0;

  while (*src && i++ < maxlen) {
    if (src < src_end && dst < dst_end) {
      *dst++ = *src++;
    } else {
#ifdef ENABLE_ERROR_REPORT
      Log(kLogWithStack, __FILE__, __LINE__, "OOB detected");
#endif
#ifdef CRASH_ON_CORRUPTION
      fflush(stdout);
      abort();
#endif
    }
  }

  *dst = 0;
  return _dst;
}

static inline void* do_strcpy_check(void* _dst, void* _src) noexcept {
  char* dst_end = (char*)do_get_chunk_end(_dst);
  char* src_end = (char*)do_get_chunk_end(_src);

  char* dst = (char*)_dst;
  char* src = (char*)_src;

  while (*src) {
    if (src < src_end && dst < dst_end) {
      *dst++ = *src++;
    } else {
#ifdef ENABLE_ERROR_REPORT
      Log(kLogWithStack, __FILE__, __LINE__, "OOB detected");
#endif
#ifdef CRASH_ON_CORRUPTION
      fflush(stdout);
      abort();
#endif
    }
  }

  *dst = 0;
  return _dst;
}

static inline void* do_strncat_check(void* _dst, void* _src, size_t maxlen) noexcept {
  char* dst_end = (char*)do_get_chunk_end(_dst);
  char* src_end = (char*)do_get_chunk_end(_src);

  char* dst = (char*)_dst;
  char* src = (char*)_src;
  size_t i = 0;

  while (*dst) {
    if (dst < dst_end) dst++;
    else {
#ifdef ENABLE_ERROR_REPORT
      Log(kLogWithStack, __FILE__, __LINE__, "OOB detected");
#endif
#ifdef CRASH_ON_CORRUPTION
      fflush(stdout);
      abort();
#endif
    }
  }

  while (*src && i++ < maxlen) {
    if (src < src_end && dst < dst_end) {
      *dst++ = *src++;
    } else {
#ifdef ENABLE_ERROR_REPORT
    Log(kLogWithStack, __FILE__, __LINE__, "OOB detected");
#endif
#ifdef CRASH_ON_CORRUPTION
    fflush(stdout);
    abort();
#endif
    }
  }

  *dst = 0;
  return _dst;
}

static inline void* do_strcat_check(void* _dst, void* _src) noexcept {
  char* dst_end = (char*)do_get_chunk_end(_dst);
  char* src_end = (char*)do_get_chunk_end(_src);

  char* dst = (char*)_dst;
  char* src = (char*)_src;

  while (*dst) {
    if (dst < dst_end) dst++;
    else {
#ifdef ENABLE_ERROR_REPORT
      Log(kLogWithStack, __FILE__, __LINE__, "OOB detected");
#endif
#ifdef CRASH_ON_CORRUPTION
      fflush(stdout);
      abort();
#endif
    }
  }

  while (*src) {
    if (src < src_end && dst < dst_end) {
      *dst++ = *src++;
    } else {
#ifdef ENABLE_ERROR_REPORT
    Log(kLogWithStack, __FILE__, __LINE__, "OOB detected");
#endif
#ifdef CRASH_ON_CORRUPTION
    fflush(stdout);
    abort();
#endif
    }
  }

  *dst = 0;
  return _dst;
}

// If we consult the span then retrieve the obj_size and start address, it will
// invoke 4 memory access: first find span from the map (2 accesses), then obj_size
// and start address in the span. This is expensive because the span is not hot thus
// the two access to span invokes ~50% of overhead.

// we will use sizeclass instead, with some tweaks on the page table. Now the
// sizeclass page table also contains the start page of the span.

// return 0 for valid access
// return -1 for invalid access
// return 1 for non-heap memory
static inline int do_gep_check_boundary(void *base, void *ptr, size_t size) noexcept {
  const PageId p = PageIdContaining(base);
  size_t start_addr, obj_size;
  Span *span;

// #define OBJ_SIZE_DEBUG
#ifdef OBJ_SIZE_DEBUG
  span = tc_globals.pagemap().GetExistingDescriptor(p);
  CHECK_CONDITION(span->obj_size != 0);
  CHECK_CONDITION(span->obj_size * 8ULL = GetSize(base));

  size_t raw_data = tc_globals.pagemap().get_page_info(p);
  if (raw_data) {
    CHECK_CONDITION((raw_data >> 8) == span->first_page().index());
  }
#endif

  size_t page_info = tc_globals.pagemap().get_page_info(p);
  size_t size_class = page_info & (CompactSizeClass)(-1);
  if (size_class != 0) {
    obj_size = tc_globals.sizemap().class_to_size(size_class);
    start_addr = (size_t)PageId(page_info >> (sizeof(CompactSizeClass) * 8)).start_addr();
  } else {
    span = tc_globals.pagemap().GetDescriptor(p);
    if (!span) {
      return 1;
    }
    obj_size = span->obj_size * 8ULL;
    start_addr = (size_t)span->start_address();
  }

  size_t chunk_start = (size_t)(start_addr) + (((size_t)base - (size_t)(start_addr)) / obj_size) * obj_size;
  size_t chunk_end = chunk_start + obj_size;

#ifdef PROTECTION_DEBUG
  printf("start_addr 0x%lx, objsize %ld, chunk range [%lx-%lx], base %p, access range [%p-0x%lx]\n",
          start_addr, obj_size, chunk_start, chunk_end, base, ptr, size+(size_t)ptr);
#endif

  // We need reserve eight more bytes
  if ((size_t)ptr >= chunk_start && ((size_t)ptr + size) <= chunk_end) {
    return 0;
  }

#ifdef ENABLE_ERROR_REPORT
  Log(kLogWithStack, __FILE__, __LINE__, "OOB detected");
#endif
#ifdef CRASH_ON_CORRUPTION
  fflush(stdout);
  abort();
#endif

  return -1;
}

// return 0 for valid access
// return -1 for invalid access
// return 1 for non-heap memory
static inline int do_bc_check_boundary(void *base, size_t size) noexcept {
  const PageId p = PageIdContaining(base);
  size_t start_addr, obj_size;
  Span *span;

// #define OBJ_SIZE_DEBUG
#ifdef OBJ_SIZE_DEBUG
  span = tc_globals.pagemap().GetExistingDescriptor(p);
  CHECK_CONDITION(span->obj_size != 0);
  CHECK_CONDITION(span->obj_size * 8ULL = GetSize(base));

  size_t raw_data = tc_globals.pagemap().get_page_info(p);
  if (raw_data) {
    CHECK_CONDITION((raw_data >> 8) == span->first_page().index());
  }
#endif

  size_t page_info = tc_globals.pagemap().get_page_info(p);
  size_t size_class = page_info & (CompactSizeClass)(-1);
  if (size_class != 0) {
    obj_size = tc_globals.sizemap().class_to_size(size_class);
    start_addr = (size_t)PageId(page_info >> (sizeof(CompactSizeClass) * 8)).start_addr();
  } else {
    span = tc_globals.pagemap().GetDescriptor(p);
    if (!span) {
      return 1;
    }
    obj_size = span->obj_size * 8ULL;
    start_addr = (size_t)span->start_address();
  }

  size_t chunk_start = (size_t)(start_addr) + (((size_t)base - (size_t)(start_addr)) / obj_size) * obj_size;
  size_t chunk_end = chunk_start + obj_size;

#ifdef PROTECTION_DEBUG
  printf("start_addr 0x%lx, objsize %ld, chunk range [%lx-%lx], base %p, access range [%p-0x%lx]\n",
          start_addr, obj_size, chunk_start, chunk_end, base, base, size+(size_t)base);
#endif

  if ((size_t)base >= chunk_start && ((size_t)base + size) <= chunk_end) {
    return 0;
  }

#ifdef ENABLE_ERROR_REPORT
  Log(kLogWithStack, __FILE__, __LINE__, "OOB detected");
#endif
#ifdef CRASH_ON_CORRUPTION
  fflush(stdout);
  abort();
#endif

  return -1;
}

static inline size_t do_get_chunk_start(void* base) noexcept {
  const PageId p = PageIdContaining(base);
  size_t start_addr, obj_size;
  Span *span;

  size_t page_info = tc_globals.pagemap().get_page_info(p);
  size_t size_class = page_info & (CompactSizeClass)(-1);
  if (size_class != 0) {
    obj_size = tc_globals.sizemap().class_to_size(size_class);
    start_addr = (size_t)PageId(page_info >> (sizeof(CompactSizeClass) * 8)).start_addr();
  } else {
    span = tc_globals.pagemap().GetDescriptor(p);
    if (!span) {
      return 0;
    }
    obj_size = span->obj_size * 8ULL;
    start_addr = (size_t)span->start_address();
  }

  size_t chunk_start = (size_t)(start_addr) + (((size_t)base - (size_t)(start_addr)) / obj_size) * obj_size;

  return chunk_start;
}

static inline void commit_escape(Span *span, void **loc,
    void *ptr, unsigned idx) {
  // insert escape here
  if (span->escape_list == nullptr) {
    if (span->objects_per_span <= 2) {
      span->escape_list = (struct escape **)alloc_escape();
      memset(span->escape_list, 0, 16);
    } else {
      span->escape_list = alloc_escape_list();
    }
  }

  struct escape **escape_list = span->escape_list;
  // store the loc into ptr's escapes
  struct escape *loc_e = alloc_escape();
  loc_e->loc = (void *)loc;
  loc_e->next = escape_list[idx];
  escape_list[idx] = loc_e;
}

static inline int do_escape(
    void **loc, void* ptr) noexcept {
  // store pointer new into loc
  // so loc will point to new
  // find span of new and then add to the list

  // for (size_t size_class=1; size_class <100; size_class++) {
  //   size_t span_size =
  //         Length(tc_globals.sizemap().class_to_pages(size_class)).in_bytes();
  //   size_t allocated_size = tc_globals.sizemap().class_to_size(size_class);
  //   size_t objects_per_span = span_size / allocated_size;
  //   printf("[%ld] alloc size %ld object per span %ld\n", size_class, allocated_size, objects_per_span);
  // }

  // this is cheap but optimizes a lot for perl
  Span* loc_span = tc_globals.pagemap().GetDescriptor(PageIdContaining((void*)loc));
  if (!loc_span) {
    return -1;
  }
#ifdef ENABLE_STATISTIC
  tc_globals.escape_heap_cnt++;
#endif
  Span* span = tc_globals.pagemap().GetDescriptor(PageIdContaining(ptr));
  if (!span) {
    return -1;
  }
  span->Prefetch();
#ifdef ENABLE_STATISTIC
  tc_globals.escape_valid_cnt++;
#endif
  // FIXME: obj_size shouldn't be 0
  size_t obj_size = span->obj_size * 8ULL;
  if (ABSL_PREDICT_FALSE(obj_size == 0)) {
    printf("span %p obj size is 0\n", span);
    return -1;
  }

  unsigned idx = ((size_t)ptr - (size_t)span->start_address()) / obj_size;
  size_t obj_start = (size_t)span->start_address() + obj_size * idx;

  void *old_ptr = *loc;
  if (obj_start <= (size_t)old_ptr && (size_t)old_ptr < obj_start+obj_size) {
    // same loc, optimize this
#ifdef ENABLE_STATISTIC
    tc_globals.escape_loc_optimized++;
#endif
    return 0;
  }
#ifdef ENABLE_STATISTIC
  tc_globals.escape_final_cnt++;
#endif

  // FIXME
  // CHECK_CONDITION(idx < span->objects_per_span);
  if (ABSL_PREDICT_FALSE(idx >= span->objects_per_span)) {
    // this is a bug
    printf("span %p obj_per_span %d idx %d, ptr %p start addr %p span size %lx obj size %x\n", span, span->objects_per_span, idx, ptr, span->start_address(), span->bytes_in_span(), span->obj_size);
    return -1;
  }

  if (tc_globals.escape_pos == CACHE_SIZE) {
    // do commit
    for (int i=0; i<CACHE_SIZE; i++) {
      ptr = tc_globals.escape_caches[i].ptr;
      loc = tc_globals.escape_caches[i].loc;
      if (*loc == ptr) {
        span = tc_globals.pagemap().GetDescriptor(PageIdContaining(ptr));
        if (!span || !span->obj_size)
          continue;
        
        obj_size = span->obj_size * 8ULL;
        idx = ((size_t)ptr - (size_t)span->start_address()) / obj_size;
        if (idx >= 1024)
          continue;
        commit_escape(span, loc, ptr, idx);
      } else {
        // removing old records is heavy
        // we leave it for free to do it
#ifdef ENABLE_STATISTIC
        tc_globals.escape_cache_optimized++;
#endif
      }
    }
    tc_globals.escape_pos = 0;
  }

  tc_globals.escape_caches[tc_globals.escape_pos].loc = loc;
  // tc_globals.escape_caches[tc_globals.escape_pos].old_ptr = *loc;
  tc_globals.escape_caches[tc_globals.escape_pos++].ptr = ptr;
  return 0;
}

static inline void do_report_error() noexcept {
#ifdef ENABLE_ERROR_REPORT
  Log(kLogWithStack, __FILE__, __LINE__, "OOB detected");
#endif
#ifdef CRASH_ON_CORRUPTION
  fflush(stdout);
  abort();
#endif
}

static inline size_t do_get_chunk_range(void* base, size_t* start) noexcept {
#ifdef ENABLE_STATISTIC
  tc_globals.get_end_cnt++;
#endif
  const PageId p = PageIdContaining(base);
  size_t start_addr, obj_size;
  Span *span;

  size_t page_info = tc_globals.pagemap().get_page_info(p);
  size_t size_class = page_info & (CompactSizeClass)(-1);
  if (size_class != 0) {
    obj_size = tc_globals.sizemap().class_to_size(size_class);
    start_addr = (size_t)PageId(page_info >> (sizeof(CompactSizeClass) * 8)).start_addr();
  } else {
    span = tc_globals.pagemap().GetDescriptor(p);
    if (!span) {
      *start = 0;
      return 0x1000000000000;
    }
    obj_size = span->obj_size * 8ULL;
    start_addr = (size_t)span->start_address();
  }

  size_t chunk_start = (size_t)(start_addr) + (((size_t)base - (size_t)(start_addr)) / obj_size) * obj_size;
  size_t chunk_end = chunk_start + obj_size;

  *start = chunk_start;
  return chunk_end;
}

static inline void do_report_statistic() {
#ifdef ENABLE_STATISTIC
  fprintf(stderr, "\nmalloc count\t\t: %ld\n", tc_globals.malloc_cnt);
  fprintf(stderr, "free count\t\t: %ld\n", tc_globals.free_cnt);
  fprintf(stderr, "escape count\t\t: %ld\n", tc_globals.escape_cnt);
  fprintf(stderr, "escape valid count\t: %ld\n", tc_globals.escape_valid_cnt);
  fprintf(stderr, "escape heap count\t: %ld\n", tc_globals.escape_heap_cnt);
  fprintf(stderr, "escape optimized count\t: %ld\n", tc_globals.escape_loc_optimized);
  fprintf(stderr, "escape final count\t: %ld\n", tc_globals.escape_final_cnt);
  fprintf(stderr, "escape cache optimized\t: %ld\n", tc_globals.escape_cache_optimized);
  fprintf(stderr, "get end count\t: %ld\n", tc_globals.get_end_cnt);
  fprintf(stderr, "gep check count\t: %ld\n", tc_globals.gep_check_cnt);
  fprintf(stderr, "bc check count\t: %ld\n", tc_globals.bc_check_cnt);
#endif
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

using tcmalloc::tcmalloc_internal::AllocSmall;
using tcmalloc::tcmalloc_internal::CppPolicy;
using tcmalloc::tcmalloc_internal::do_free_no_hooks;
#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
using tcmalloc::tcmalloc_internal::do_mallinfo;
#endif
using tcmalloc::tcmalloc_internal::do_malloc_pages;
using tcmalloc::tcmalloc_internal::do_malloc_stats;
#ifdef TCMALLOC_HAVE_MALLOC_TRIM
using tcmalloc::tcmalloc_internal::do_malloc_trim;
#endif
using tcmalloc::tcmalloc_internal::do_mallopt;
using tcmalloc::tcmalloc_internal::GetThreadSampler;
using tcmalloc::tcmalloc_internal::MallocPolicy;
using tcmalloc::tcmalloc_internal::SetClassCapacity;
using tcmalloc::tcmalloc_internal::SetPagesCapacity;
using tcmalloc::tcmalloc_internal::tc_globals;
using tcmalloc::tcmalloc_internal::UsePerCpuCache;

// export safe function
using tcmalloc::tcmalloc_internal::do_gep_check_boundary;
using tcmalloc::tcmalloc_internal::do_bc_check_boundary;
using tcmalloc::tcmalloc_internal::do_escape;
using tcmalloc::tcmalloc_internal::do_get_chunk_range;
using tcmalloc::tcmalloc_internal::do_report_error;
using tcmalloc::tcmalloc_internal::do_report_statistic;
using tcmalloc::tcmalloc_internal::do_strcat_check;
using tcmalloc::tcmalloc_internal::do_strncat_check;
using tcmalloc::tcmalloc_internal::do_strcpy_check;
using tcmalloc::tcmalloc_internal::do_strncpy_check;

#ifdef TCMALLOC_DEPRECATED_PERTHREAD
using tcmalloc::tcmalloc_internal::ThreadCache;
#endif  // TCMALLOC_DEPRECATED_PERTHREAD

// Slow path implementation.
// This function is used by `fast_alloc` if the allocation requires page sized
// allocations or some complex logic is required such as initialization,
// invoking new/delete hooks, sampling, etc.
//
// TODO(b/130771275):  This function is marked as static, rather than appearing
// in the anonymous namespace, to workaround incomplete heapz filtering.
template <typename Policy, typename CapacityPtr = std::nullptr_t>
static void* ABSL_ATTRIBUTE_SECTION(google_malloc)
    slow_alloc(Policy policy, size_t size, CapacityPtr capacity = nullptr) {
  tc_globals.InitIfNecessary();
  GetThreadSampler()->UpdateFastPathState();
  void* p;
  uint32_t size_class;
  bool is_small = tc_globals.sizemap().GetSizeClass(policy, size, &size_class);
  if (ABSL_PREDICT_TRUE(is_small)) {
    p = AllocSmall(policy, size_class, size, capacity);
  } else {
    p = do_malloc_pages(policy, size, 1, capacity);
    if (ABSL_PREDICT_FALSE(p == nullptr)) {
      return Policy::handle_oom(size);
    }
  }
  if (Policy::invoke_hooks()) {
  }
  return p;
}

template <typename Policy, typename CapacityPtr = std::nullptr_t>
static inline void* ABSL_ATTRIBUTE_ALWAYS_INLINE
fast_alloc(Policy policy, size_t size, CapacityPtr capacity = nullptr) {
  // If size is larger than kMaxSize, it's not fast-path anymore. In
  // such case, GetSizeClass will return false, and we'll delegate to the slow
  // path. If malloc is not yet initialized, we may end up with size_class == 0
  // (regardless of size), but in this case should also delegate to the slow
  // path by the fast path check further down.
#ifdef ENABLE_STATISTIC
  tc_globals.malloc_cnt++;
#endif

#ifdef ENABLE_PROTECTION
  // when the size of object is the same as the size of chunk, a ptr pointing
  // to the end of obj a will point to the start of of the adjacent chunk as
  // well. This will confuse escape when maintaining the "who points to me".
  // mitigate this issue by padding one extra byte for each allocation.
  // |  chunk a  |  chunk b  |
  //            /|\
  //             |
  //            ptr
  size = size + 1;
#endif

  uint32_t size_class;
  bool is_small = tc_globals.sizemap().GetSizeClass(policy, size, &size_class);
  if (ABSL_PREDICT_FALSE(!is_small)) {
    return slow_alloc(policy, size, capacity);
  }

  // When using per-thread caches, we have to check for the presence of the
  // cache for this thread before we try to sample, as slow_alloc will
  // also try to sample the allocation.
#ifdef TCMALLOC_DEPRECATED_PERTHREAD
  ThreadCache* const cache = ThreadCache::GetCacheIfPresent();
  if (ABSL_PREDICT_FALSE(cache == nullptr)) {
    return slow_alloc(policy, size, capacity);
  }
#endif
  // TryRecordAllocationFast() returns true if no extra logic is required, e.g.:
  // - this allocation does not need to be sampled
  // - no new/delete hooks need to be invoked
  // - no need to initialize thread globals, data or caches.
  // The method updates 'bytes until next sample' thread sampler counters.
  if (ABSL_PREDICT_FALSE(!GetThreadSampler()->TryRecordAllocationFast(size))) {
    return slow_alloc(policy, size, capacity);
  }

  // Fast path implementation for allocating small size memory.
  // This code should only be reached if all of the below conditions are met:
  // - the size does not exceed the maximum size (size class > 0)
  // - cpu / thread cache data has been initialized.
  // - the allocation is not subject to sampling / gwp-asan.
  // - no new/delete hook is installed and required to be called.
  ASSERT(size_class != 0);
  void* ret;
#ifndef TCMALLOC_DEPRECATED_PERTHREAD
  // The CPU cache should be ready.
  ret = tc_globals.cpu_cache().Allocate<Policy::handle_oom>(size_class);
#else   // !defined(TCMALLOC_DEPRECATED_PERTHREAD)
  // The ThreadCache should be ready.
  ASSERT(cache != nullptr);
  ret = cache->Allocate<Policy::handle_oom>(size_class);
#endif  // TCMALLOC_DEPRECATED_PERTHREAD
  if (!Policy::can_return_nullptr()) {
    ASSUME(ret != nullptr);
  }
  SetClassCapacity(ret, size_class, capacity);
  return ret;
}

using tcmalloc::tcmalloc_internal::GetOwnership;
using tcmalloc::tcmalloc_internal::GetSize;

extern "C" size_t MallocExtension_Internal_GetAllocatedSize(const void* ptr) {
  ASSERT(!ptr ||
         GetOwnership(ptr) != tcmalloc::MallocExtension::Ownership::kNotOwned);
  return GetSize(ptr);
}

extern "C" void MallocExtension_Internal_MarkThreadBusy() {
  // Allocate to force the creation of a thread cache, but avoid
  // invoking any hooks.
  tc_globals.InitIfNecessary();

  if (UsePerCpuCache()) {
    return;
  }

  do_free_no_hooks(slow_alloc(CppPolicy().Nothrow().WithoutHooks(), 0));
}

absl::StatusOr<tcmalloc::malloc_tracing_extension::AllocatedAddressRanges>
MallocTracingExtension_Internal_GetAllocatedAddressRanges() {
  tcmalloc::malloc_tracing_extension::AllocatedAddressRanges
      allocated_address_ranges;
  constexpr float kAllocatedSpansSizeReserveFactor = 1.2;
  constexpr int kMaxAttempts = 10;
  for (int i = 0; i < kMaxAttempts; i++) {
    int estimated_span_count;
    {
      absl::base_internal::SpinLockHolder l(
          &tcmalloc::tcmalloc_internal::pageheap_lock);
      estimated_span_count = tc_globals.span_allocator().stats().total;
    }
    // We need to avoid allocation events during GetAllocatedSpans, as that may
    // cause a deadlock on pageheap_lock. To this end, we ensure that the result
    // vector already has a capacity greater than the current total span count.
    allocated_address_ranges.spans.reserve(estimated_span_count *
                                           kAllocatedSpansSizeReserveFactor);
    int actual_span_count =
        tc_globals.pagemap().GetAllocatedSpans(allocated_address_ranges.spans);
    if (allocated_address_ranges.spans.size() == actual_span_count) {
      return allocated_address_ranges;
    }
    allocated_address_ranges.spans.clear();
  }
  return absl::InternalError(
      "Could not fetch all Spans due to insufficient reserved capacity in the "
      "output vector.");
}

//-------------------------------------------------------------------
// Exported routines
//-------------------------------------------------------------------

using tcmalloc::tcmalloc_internal::AlignAsPolicy;
using tcmalloc::tcmalloc_internal::CorrectAlignment;
using tcmalloc::tcmalloc_internal::CorrectSize;
using tcmalloc::tcmalloc_internal::DefaultAlignPolicy;
using tcmalloc::tcmalloc_internal::do_free;
using tcmalloc::tcmalloc_internal::do_free_with_size;

// depends on TCMALLOC_HAVE_STRUCT_MALLINFO, so needs to come after that.
#include "tcmalloc/libc_override.h"

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalMalloc(
    size_t size) noexcept {
  // Use TCMallocInternalMemalign to avoid requiring size %
  // alignof(std::max_align_t) == 0. TCMallocInternalAlignedAlloc enforces this
  // property.
  return TCMallocInternalMemalign(alignof(std::max_align_t), size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNew(size_t size) {
  return fast_alloc(CppPolicy(), size);
}

extern "C" ABSL_ATTRIBUTE_SECTION(google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new(size_t size) {
  size_t capacity;
  void* p = fast_alloc(CppPolicy(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_ATTRIBUTE_SECTION(google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new_aligned(size_t size,
                                                 std::align_val_t alignment) {
  size_t capacity;
  void* p = fast_alloc(CppPolicy().AlignAs(alignment), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_ATTRIBUTE_SECTION(google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new_hot_cold(
        size_t size, tcmalloc::hot_cold_t hot_cold) {
  size_t capacity;
  void* p = static_cast<uint8_t>(hot_cold) >= uint8_t{128}
                ? fast_alloc(CppPolicy().AccessAsHot(), size, &capacity)
                : fast_alloc(CppPolicy().AccessAsCold(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_ATTRIBUTE_SECTION(google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new_aligned_hot_cold(
        size_t size, std::align_val_t alignment,
        tcmalloc::hot_cold_t hot_cold) {
  size_t capacity;
  void* p = static_cast<uint8_t>(hot_cold) >= uint8_t{128}
                ? fast_alloc(CppPolicy().AlignAs(alignment).AccessAsHot(), size,
                             &capacity)
                : fast_alloc(CppPolicy().AlignAs(alignment).AccessAsCold(),
                             size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalMalloc_aligned(
    size_t size, std::align_val_t alignment) noexcept {
  return fast_alloc(MallocPolicy().AlignAs(alignment), size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalNewAligned(
    size_t size, std::align_val_t alignment) {
  return fast_alloc(CppPolicy().AlignAs(alignment), size);
}

#ifdef TCMALLOC_ALIAS
extern "C" void* TCMallocInternalNewAligned_nothrow(
    size_t size, std::align_val_t alignment, const std::nothrow_t& nt) noexcept
    // Note: we use malloc rather than new, as we are allowed to return nullptr.
    // The latter crashes in that case.
    TCMALLOC_ALIAS(TCMallocInternalMalloc_aligned);
#else
extern "C" ABSL_ATTRIBUTE_SECTION(
    google_malloc) void* TCMallocInternalNewAligned_nothrow(size_t size,
                                                            std::align_val_t
                                                                alignment,
                                                            const std::nothrow_t&
                                                                nt) noexcept {
  return fast_alloc(CppPolicy().Nothrow().AlignAs(alignment), size);
}
#endif  // TCMALLOC_ALIAS

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalFree(
    void* ptr) noexcept {
  do_free(ptr);
}

extern "C" void TCMallocInternalSdallocx(void* ptr, size_t size,
                                         int flags) noexcept {
  size_t alignment = alignof(std::max_align_t);

  if (ABSL_PREDICT_FALSE(flags != 0)) {
    ASSERT((flags & ~0x3f) == 0);
    alignment = static_cast<size_t>(1ull << (flags & 0x3f));
  }

  return do_free_with_size(ptr, size, AlignAsPolicy(alignment));
}

extern "C" void* TCMallocInternalCalloc(size_t n, size_t elem_size) noexcept {
  // Overflow check
  const size_t size = n * elem_size;
  if (elem_size != 0 && size / elem_size != n) {
    return MallocPolicy::handle_oom(std::numeric_limits<size_t>::max());
  }
  void* result = fast_alloc(MallocPolicy(), size);
  if (result != nullptr) {
    memset(result, 0, size);
  }
  return result;
}

// Here and below we use TCMALLOC_ALIAS (if supported) to make
// identical functions aliases.  This saves space in L1 instruction
// cache.  As of now it saves ~9K.
extern "C" void TCMallocInternalCfree(void* ptr) noexcept
#ifdef TCMALLOC_ALIAS
    TCMALLOC_ALIAS(TCMallocInternalFree);
#else
{
  do_free(ptr);
}
#endif  // TCMALLOC_ALIAS

static inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* do_realloc(void* old_ptr,
                                                            size_t new_size) {
  tc_globals.InitIfNecessary();
  // Get the size of the old entry
  const size_t old_size = GetSize(old_ptr);

#ifdef ENABLE_PROTECTION
  // 2 extra bytes for realloc is intended
  new_size = new_size + 1;
#endif

  // Reallocate if the new size is larger than the old size,
  // or if the new size is significantly smaller than the old size.
  // We do hysteresis to avoid resizing ping-pongs:
  //    . If we need to grow, grow to max(new_size, old_size * 1.X)
  //    . Don't shrink unless new_size < old_size * 0.Y
  // X and Y trade-off time for wasted space.  For now we do 1.25 and 0.5.
  const size_t min_growth = std::min(
      old_size / 4,
      std::numeric_limits<size_t>::max() - old_size);  // Avoid overflow.
  const size_t lower_bound_to_grow = old_size + min_growth;
  const size_t upper_bound_to_shrink = old_size / 2;
  if ((new_size > old_size) || (new_size < upper_bound_to_shrink)) {
    // Need to reallocate.
    void* new_ptr = nullptr;

    if (new_size > old_size && new_size < lower_bound_to_grow) {
      // Avoid fast_alloc() reporting a hook with the lower bound size
      // as we the expectation for pointer returning allocation functions
      // is that malloc hooks are invoked with the requested_size.
      new_ptr = fast_alloc(MallocPolicy().Nothrow().WithoutHooks(),
                           lower_bound_to_grow);
      if (new_ptr != nullptr) {
      }
    }
    if (new_ptr == nullptr) {
      // Either new_size is not a tiny increment, or last do_malloc failed.
      new_ptr = fast_alloc(MallocPolicy(), new_size);
    }
    if (new_ptr == nullptr) {
      return nullptr;
    }
    memcpy(new_ptr, old_ptr, ((old_size < new_size) ? old_size : new_size));
    // We could use a variant of do_free() that leverages the fact
    // that we already know the sizeclass of old_ptr.  The benefit
    // would be small, so don't bother.
    do_free(old_ptr);
    return new_ptr;
  } else {
    return old_ptr;
  }
}

extern "C" void* TCMallocInternalRealloc(void* old_ptr,
                                         size_t new_size) noexcept {
  if (old_ptr == NULL) {
    return fast_alloc(MallocPolicy(), new_size);
  }
  if (new_size == 0) {
    do_free(old_ptr);
    return NULL;
  }
  return do_realloc(old_ptr, new_size);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalStrcatCheck(
    void* dst, void* src) noexcept {
  return do_strcat_check(dst, src);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalStrncatCheck(
    void* dst, void* src, size_t maxlen) noexcept {
  return do_strncat_check(dst, src, maxlen);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalStrcpyCheck(
    void* dst, void* src) noexcept {
  return do_strcpy_check(dst, src);
}

extern "C" ABSL_CACHELINE_ALIGNED void* TCMallocInternalStrncpyCheck(
    void* dst, void* src, size_t maxlen) noexcept {
  return do_strncpy_check(dst, src, maxlen);
}

extern "C" ABSL_CACHELINE_ALIGNED int TCMallocInternalGepCheckBoundary(
    void *base, void *ptr, size_t size) noexcept {
#ifdef ENABLE_STATISTIC
  tc_globals.gep_check_cnt++;
#endif

#ifdef ENABLE_PROTECTION
  return do_gep_check_boundary(base, ptr, size);
#else
  return 0;
#endif
}

extern "C" ABSL_CACHELINE_ALIGNED int TCMallocInternalBcCheckBoundary(
    void *base, size_t size) noexcept {
#ifdef ENABLE_STATISTIC
  tc_globals.bc_check_cnt++;
#endif
#ifdef ENABLE_PROTECTION
  return do_bc_check_boundary(base, size);
#else
  return 0;
#endif
}

extern "C" ABSL_CACHELINE_ALIGNED void TCReportError() noexcept {
#ifdef ENABLE_PROTECTION
  do_report_error();
#endif
}

extern "C" ABSL_CACHELINE_ALIGNED size_t TCGetChunkRange(void* base, size_t* start) noexcept {
  return do_get_chunk_range(base, start);
}

extern "C" ABSL_CACHELINE_ALIGNED int TCMallocInternalEscape(
    void** loc, void* ptr) noexcept {
#ifdef ENABLE_STATISTIC
  tc_globals.escape_cnt++;
#endif
#ifdef ENABLE_PROTECTION
  return do_escape(loc, ptr);
#endif
}

extern "C" ABSL_CACHELINE_ALIGNED void TCReportStatistic() noexcept {
  return do_report_statistic();
}

extern "C" void* TCMallocInternalNewNothrow(size_t size,
                                            const std::nothrow_t&) noexcept {
  return fast_alloc(CppPolicy().Nothrow(), size);
}

extern "C" tcmalloc::sized_ptr_t tcmalloc_size_returning_operator_new_nothrow(
    size_t size) noexcept {
  size_t capacity;
  void* p = fast_alloc(CppPolicy().Nothrow(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_ATTRIBUTE_SECTION(google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new_aligned_nothrow(
        size_t size, std::align_val_t alignment) noexcept {
  size_t capacity;
  void* p =
      fast_alloc(CppPolicy().AlignAs(alignment).Nothrow(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_ATTRIBUTE_SECTION(google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new_hot_cold_nothrow(
        size_t size, tcmalloc::hot_cold_t hot_cold) noexcept {
  size_t capacity;
  void* p =
      static_cast<uint8_t>(hot_cold) >= uint8_t{128}
          ? fast_alloc(CppPolicy().AccessAsHot().Nothrow(), size, &capacity)
          : fast_alloc(CppPolicy().AccessAsCold().Nothrow(), size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_ATTRIBUTE_SECTION(google_malloc) tcmalloc::sized_ptr_t
    tcmalloc_size_returning_operator_new_aligned_hot_cold_nothrow(
        size_t size, std::align_val_t alignment,
        tcmalloc::hot_cold_t hot_cold) noexcept {
  size_t capacity;
  void* p =
      static_cast<uint8_t>(hot_cold) >= uint8_t{128}
          ? fast_alloc(CppPolicy().AlignAs(alignment).AccessAsHot().Nothrow(),
                       size, &capacity)
          : fast_alloc(CppPolicy().AlignAs(alignment).AccessAsCold().Nothrow(),
                       size, &capacity);
  return {p, capacity};
}

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalDelete(void* p) noexcept
#ifdef TCMALLOC_ALIAS
    TCMALLOC_ALIAS(TCMallocInternalFree);
#else
{
  do_free(p);
}
#endif  // TCMALLOC_ALIAS

extern "C" void TCMallocInternalDeleteAligned(
    void* p, std::align_val_t alignment) noexcept
#if defined(TCMALLOC_ALIAS) && defined(NDEBUG)
    TCMALLOC_ALIAS(TCMallocInternalDelete);
#else
{
  // Note: The aligned delete/delete[] implementations differ slightly from
  // their respective aliased implementations to take advantage of checking the
  // passed-in alignment.
  ASSERT(CorrectAlignment(p, alignment));
  return TCMallocInternalDelete(p);
}
#endif

extern "C" ABSL_CACHELINE_ALIGNED void TCMallocInternalDeleteSized(
    void* p, size_t size) noexcept {
  ASSERT(CorrectSize(p, size, DefaultAlignPolicy()));
  do_free_with_size(p, size, DefaultAlignPolicy());
}

extern "C" void TCMallocInternalDeleteSizedAligned(
    void* p, size_t t, std::align_val_t alignment) noexcept {
  return do_free_with_size(p, t, AlignAsPolicy(alignment));
}

extern "C" void TCMallocInternalDeleteArraySized(void* p, size_t size) noexcept
#ifdef TCMALLOC_ALIAS
    TCMALLOC_ALIAS(TCMallocInternalDeleteSized);
#else
{
  do_free_with_size(p, size, DefaultAlignPolicy());
}
#endif

extern "C" void TCMallocInternalDeleteArraySizedAligned(
    void* p, size_t t, std::align_val_t alignment) noexcept
#ifdef TCMALLOC_ALIAS
    TCMALLOC_ALIAS(TCMallocInternalDeleteSizedAligned);
#else
{
  return TCMallocInternalDeleteSizedAligned(p, t, alignment);
}
#endif

// Standard C++ library implementations define and use this
// (via ::operator delete(ptr, nothrow)).
// But it's really the same as normal delete, so we just do the same thing.
extern "C" void TCMallocInternalDeleteNothrow(void* p,
                                              const std::nothrow_t&) noexcept
#ifdef TCMALLOC_ALIAS
    TCMALLOC_ALIAS(TCMallocInternalFree);
#else
{
  do_free(p);
}
#endif  // TCMALLOC_ALIAS

#if defined(TCMALLOC_ALIAS) && defined(NDEBUG)
extern "C" void TCMallocInternalDeleteAligned_nothrow(
    void* p, std::align_val_t alignment, const std::nothrow_t& nt) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDelete);
#else
extern "C" ABSL_ATTRIBUTE_SECTION(
    google_malloc) void TCMallocInternalDeleteAligned_nothrow(void* p,
                                                              std::align_val_t
                                                                  alignment,
                                                              const std::nothrow_t&
                                                                  nt) noexcept {
  ASSERT(CorrectAlignment(p, alignment));
  return TCMallocInternalDelete(p);
}
#endif

extern "C" void* TCMallocInternalNewArray(size_t size)
#ifdef TCMALLOC_ALIAS
    TCMALLOC_ALIAS(TCMallocInternalNew);
#else
{
  void* p = fast_alloc(CppPolicy().WithoutHooks(), size);
  return p;
}
#endif  // TCMALLOC_ALIAS

extern "C" void* TCMallocInternalNewArrayAligned(size_t size,
                                                 std::align_val_t alignment)
#if defined(TCMALLOC_ALIAS) && defined(NDEBUG)
    TCMALLOC_ALIAS(TCMallocInternalNewAligned);
#else
{
  return TCMallocInternalNewAligned(size, alignment);
}
#endif

extern "C" void* TCMallocInternalNewArrayNothrow(size_t size,
                                                 const std::nothrow_t&) noexcept
#ifdef TCMALLOC_ALIAS
    TCMALLOC_ALIAS(TCMallocInternalNewNothrow);
#else
{
  return fast_alloc(CppPolicy().Nothrow(), size);
}
#endif  // TCMALLOC_ALIAS

// Note: we use malloc rather than new, as we are allowed to return nullptr.
// The latter crashes in that case.
#if defined(TCMALLOC_ALIAS) && defined(NDEBUG)
extern "C" void* TCMallocInternalNewArrayAligned_nothrow(
    size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalMalloc_aligned);
#else
extern "C" ABSL_ATTRIBUTE_SECTION(
    google_malloc) void* TCMallocInternalNewArrayAligned_nothrow(size_t size,
                                                                 std::align_val_t
                                                                     alignment,
                                                                 const std::
                                                                     nothrow_t&) noexcept {
  return TCMallocInternalMalloc_aligned(size, alignment);
}
#endif

extern "C" void TCMallocInternalDeleteArray(void* p) noexcept
#ifdef TCMALLOC_ALIAS
    TCMALLOC_ALIAS(TCMallocInternalFree);
#else
{
  do_free(p);
}
#endif  // TCMALLOC_ALIAS

extern "C" void TCMallocInternalDeleteArrayAligned(
    void* p, std::align_val_t alignment) noexcept
#if defined(TCMALLOC_ALIAS) && defined(NDEBUG)
    TCMALLOC_ALIAS(TCMallocInternalDelete);
#else
{
  ASSERT(CorrectAlignment(p, alignment));
  return TCMallocInternalDelete(p);
}
#endif

extern "C" void TCMallocInternalDeleteArrayNothrow(
    void* p, const std::nothrow_t&) noexcept
#ifdef TCMALLOC_ALIAS
    TCMALLOC_ALIAS(TCMallocInternalFree);
#else
{
  do_free(p);
}
#endif  // TCMALLOC_ALIAS

#if defined(TCMALLOC_ALIAS) && defined(NDEBUG)
extern "C" void TCMallocInternalDeleteArrayAligned_nothrow(
    void* p, std::align_val_t alignment, const std::nothrow_t&) noexcept
    TCMALLOC_ALIAS(TCMallocInternalDelete);
#else
extern "C" ABSL_ATTRIBUTE_SECTION(
    google_malloc) void TCMallocInternalDeleteArrayAligned_nothrow(void* p,
                                                                   std::align_val_t
                                                                       alignment,
                                                                   const std::
                                                                       nothrow_t&) noexcept {
  ASSERT(CorrectAlignment(p, alignment));
  return TCMallocInternalDelete(p);
}
#endif

extern "C" void* TCMallocInternalMemalign(size_t align, size_t size) noexcept {
  ASSERT(absl::has_single_bit(align));
  return fast_alloc(MallocPolicy().AlignAs(align), size);
}

extern "C" void* TCMallocInternalAlignedAlloc(size_t align,
                                              size_t size) noexcept
#if defined(TCMALLOC_ALIAS) && defined(NDEBUG)
    TCMALLOC_ALIAS(TCMallocInternalMemalign);
#else
{
  // aligned_alloc is memalign, but with the requirement that:
  //   align be a power of two (like memalign)
  //   size be a multiple of align (for the time being).
  ASSERT(align != 0);
  ASSERT(size % align == 0);

  return TCMallocInternalMemalign(align, size);
}
#endif

extern "C" int TCMallocInternalPosixMemalign(void** result_ptr, size_t align,
                                             size_t size) noexcept {
  if (((align % sizeof(void*)) != 0) || !absl::has_single_bit(align)) {
    return EINVAL;
  }
  void* result = fast_alloc(MallocPolicy().Nothrow().AlignAs(align), size);
  if (result == NULL) {
    return ENOMEM;
  } else {
    *result_ptr = result;
    return 0;
  }
}

static size_t pagesize = 0;

extern "C" void* TCMallocInternalValloc(size_t size) noexcept {
  // Allocate page-aligned object of length >= size bytes
  if (pagesize == 0) pagesize = getpagesize();
  return fast_alloc(MallocPolicy().Nothrow().AlignAs(pagesize), size);
}

extern "C" void* TCMallocInternalPvalloc(size_t size) noexcept {
  // Round up size to a multiple of pagesize
  if (pagesize == 0) pagesize = getpagesize();
  if (size == 0) {    // pvalloc(0) should allocate one page, according to
    size = pagesize;  // http://man.free4web.biz/man3/libmpatrol.3.html
  }
  size = (size + pagesize - 1) & ~(pagesize - 1);
  return fast_alloc(MallocPolicy().Nothrow().AlignAs(pagesize), size);
}

extern "C" void TCMallocInternalMallocStats(void) noexcept {
  do_malloc_stats();
}

#ifdef TCMALLOC_HAVE_MALLOC_TRIM
extern "C" int TCMallocInternalMallocTrim(size_t pad) noexcept {
  return do_malloc_trim(pad);
}
#endif

extern "C" int TCMallocInternalMallOpt(int cmd, int value) noexcept {
  return do_mallopt(cmd, value);
}

#ifdef TCMALLOC_HAVE_STRUCT_MALLINFO
extern "C" struct mallinfo TCMallocInternalMallocInfo(void) noexcept {
  return do_mallinfo();
}
#endif

extern "C" size_t TCMallocInternalMallocSize(void* ptr) noexcept {
  ASSERT(GetOwnership(ptr) != tcmalloc::MallocExtension::Ownership::kNotOwned);
  return GetSize(ptr) - 1;
}

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

// The constructor allocates an object to ensure that initialization
// runs before main(), and therefore we do not have a chance to become
// multi-threaded before initialization.  We also create the TSD key
// here.  Presumably by the time this constructor runs, glibc is in
// good enough shape to handle pthread_key_create().
//
// The destructor prints stats when the program exits.
class TCMallocGuard {
 public:
  TCMallocGuard() {
    TCMallocInternalFree(TCMallocInternalMalloc(1));
    ThreadCache::InitTSD();
    TCMallocInternalFree(TCMallocInternalMalloc(1));
  }
};

static TCMallocGuard module_enter_exit_hook;

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

void* operator new(size_t size, tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().AccessAsCold(), size, nullptr);
  }
}

void* operator new(size_t size, std::nothrow_t,
                   tcmalloc::hot_cold_t hot_cold) noexcept {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().Nothrow().AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().Nothrow().AccessAsCold(), size, nullptr);
  }
}

void* operator new(size_t size, std::align_val_t align,
                   tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().AlignAs(align).AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().AlignAs(align).AccessAsCold(), size, nullptr);
  }
}

void* operator new(size_t size, std::align_val_t align, std::nothrow_t,
                   tcmalloc::hot_cold_t hot_cold) noexcept {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().Nothrow().AlignAs(align).AccessAsHot(), size,
                      nullptr);
  } else {
    return fast_alloc(CppPolicy().Nothrow().AlignAs(align).AccessAsCold(), size,
                      nullptr);
  }
}

void* operator new[](size_t size,
                     tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().AccessAsCold(), size, nullptr);
  }
}

void* operator new[](size_t size, std::nothrow_t,
                     tcmalloc::hot_cold_t hot_cold) noexcept {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().Nothrow().AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().Nothrow().AccessAsCold(), size, nullptr);
  }
}

void* operator new[](size_t size, std::align_val_t align,
                     tcmalloc::hot_cold_t hot_cold) noexcept(false) {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().AlignAs(align).AccessAsHot(), size, nullptr);
  } else {
    return fast_alloc(CppPolicy().AlignAs(align).AccessAsCold(), size, nullptr);
  }
}

void* operator new[](size_t size, std::align_val_t align, std::nothrow_t,
                     tcmalloc::hot_cold_t hot_cold) noexcept {
  if (static_cast<uint8_t>(hot_cold) >= uint8_t{128}) {
    return fast_alloc(CppPolicy().Nothrow().AlignAs(align).AccessAsHot(), size,
                      nullptr);
  } else {
    return fast_alloc(CppPolicy().Nothrow().AlignAs(align).AccessAsCold(), size,
                      nullptr);
  }
}
