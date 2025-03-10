//===-- release.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_RELEASE_H_
#define SCUDO_RELEASE_H_

#include "common.h"
#include "list.h"
#include "mutex.h"
#include "thread_annotations.h"

namespace scudo {

class ReleaseRecorder {
public:
  ReleaseRecorder(uptr Base, MapPlatformData *Data = nullptr)
      : Base(Base), Data(Data) {}

  uptr getReleasedRangesCount() const { return ReleasedRangesCount; }

  uptr getReleasedBytes() const { return ReleasedBytes; }

  uptr getBase() const { return Base; }

  // Releases [From, To) range of pages back to OS.
  void releasePageRangeToOS(uptr From, uptr To) {
    const uptr Size = To - From;
    releasePagesToOS(Base, From, Size, Data);
    ReleasedRangesCount++;
    ReleasedBytes += Size;
  }

private:
  uptr ReleasedRangesCount = 0;
  uptr ReleasedBytes = 0;
  uptr Base = 0;
  MapPlatformData *Data = nullptr;
};

// A Region page map is used to record the usage of pages in the regions. It
// implements a packed array of Counters. Each counter occupies 2^N bits, enough
// to store counter's MaxValue. Ctor will try to use a static buffer first, and
// if that fails (the buffer is too small or already locked), will allocate the
// required Buffer via map(). The caller is expected to check whether the
// initialization was successful by checking isAllocated() result. For
// performance sake, none of the accessors check the validity of the arguments,
// It is assumed that Index is always in [0, N) range and the value is not
// incremented past MaxValue.
class RegionPageMap {
public:
  RegionPageMap()
      : Regions(0),
        NumCounters(0),
        CounterSizeBitsLog(0),
        CounterMask(0),
        PackingRatioLog(0),
        BitOffsetMask(0),
        SizePerRegion(0),
        BufferSize(0),
        Buffer(nullptr) {}
  RegionPageMap(uptr NumberOfRegions, uptr CountersPerRegion, uptr MaxValue) {
    reset(NumberOfRegions, CountersPerRegion, MaxValue);
  }
  ~RegionPageMap() {
    if (!isAllocated())
      return;
    if (Buffer == &StaticBuffer[0])
      Mutex.unlock();
    else
      unmap(reinterpret_cast<void *>(Buffer),
            roundUp(BufferSize, getPageSizeCached()));
    Buffer = nullptr;
  }

  // Lock of `StaticBuffer` is acquired conditionally and there's no easy way to
  // specify the thread-safety attribute properly in current code structure.
  // Besides, it's the only place we may want to check thread safety. Therefore,
  // it's fine to bypass the thread-safety analysis now.
  void reset(uptr NumberOfRegion, uptr CountersPerRegion,
             uptr MaxValue) NO_THREAD_SAFETY_ANALYSIS {
    DCHECK_GT(NumberOfRegion, 0);
    DCHECK_GT(CountersPerRegion, 0);
    DCHECK_GT(MaxValue, 0);

    Regions = NumberOfRegion;
    NumCounters = CountersPerRegion;

    constexpr uptr MaxCounterBits = sizeof(*Buffer) * 8UL;
    // Rounding counter storage size up to the power of two allows for using
    // bit shifts calculating particular counter's Index and offset.
    const uptr CounterSizeBits =
        roundUpPowerOfTwo(getMostSignificantSetBitIndex(MaxValue) + 1);
    DCHECK_LE(CounterSizeBits, MaxCounterBits);
    CounterSizeBitsLog = getLog2(CounterSizeBits);
    CounterMask = ~(static_cast<uptr>(0)) >> (MaxCounterBits - CounterSizeBits);

    const uptr PackingRatio = MaxCounterBits >> CounterSizeBitsLog;
    DCHECK_GT(PackingRatio, 0);
    PackingRatioLog = getLog2(PackingRatio);
    BitOffsetMask = PackingRatio - 1;

    SizePerRegion =
        roundUp(NumCounters, static_cast<uptr>(1U) << PackingRatioLog) >>
        PackingRatioLog;
    BufferSize = SizePerRegion * sizeof(*Buffer) * Regions;
    if (BufferSize <= (StaticBufferCount * sizeof(Buffer[0])) &&
        Mutex.tryLock()) {
      Buffer = &StaticBuffer[0];
      memset(Buffer, 0, BufferSize);
    } else {
      // When using a heap-based buffer, precommit the pages backing the
      // Vmar by passing |MAP_PRECOMMIT| flag. This allows an optimization
      // where page fault exceptions are skipped as the allocated memory
      // is accessed.
      const uptr MmapFlags =
          MAP_ALLOWNOMEM | (SCUDO_FUCHSIA ? MAP_PRECOMMIT : 0);
      Buffer = reinterpret_cast<uptr *>(
          map(nullptr, roundUp(BufferSize, getPageSizeCached()),
              "scudo:counters", MmapFlags, &MapData));
    }
  }

  bool isAllocated() const { return !!Buffer; }

  uptr getCount() const { return NumCounters; }

  uptr get(uptr Region, uptr I) const {
    DCHECK_LT(Region, Regions);
    DCHECK_LT(I, NumCounters);
    const uptr Index = I >> PackingRatioLog;
    const uptr BitOffset = (I & BitOffsetMask) << CounterSizeBitsLog;
    return (Buffer[Region * SizePerRegion + Index] >> BitOffset) & CounterMask;
  }

  void inc(uptr Region, uptr I) const {
    DCHECK_LT(get(Region, I), CounterMask);
    const uptr Index = I >> PackingRatioLog;
    const uptr BitOffset = (I & BitOffsetMask) << CounterSizeBitsLog;
    DCHECK_LT(BitOffset, SCUDO_WORDSIZE);
    DCHECK_EQ(isAllCounted(Region, I), false);
    Buffer[Region * SizePerRegion + Index] += static_cast<uptr>(1U)
                                              << BitOffset;
  }

  void incN(uptr Region, uptr I, uptr N) const {
    DCHECK_GT(N, 0U);
    DCHECK_LE(N, CounterMask);
    DCHECK_LE(get(Region, I), CounterMask - N);
    const uptr Index = I >> PackingRatioLog;
    const uptr BitOffset = (I & BitOffsetMask) << CounterSizeBitsLog;
    DCHECK_LT(BitOffset, SCUDO_WORDSIZE);
    DCHECK_EQ(isAllCounted(Region, I), false);
    Buffer[Region * SizePerRegion + Index] += N << BitOffset;
  }

  void incRange(uptr Region, uptr From, uptr To) const {
    DCHECK_LE(From, To);
    const uptr Top = Min(To + 1, NumCounters);
    for (uptr I = From; I < Top; I++)
      inc(Region, I);
  }

  // Set the counter to the max value. Note that the max number of blocks in a
  // page may vary. To provide an easier way to tell if all the blocks are
  // counted for different pages, set to the same max value to denote the
  // all-counted status.
  void setAsAllCounted(uptr Region, uptr I) const {
    DCHECK_LE(get(Region, I), CounterMask);
    const uptr Index = I >> PackingRatioLog;
    const uptr BitOffset = (I & BitOffsetMask) << CounterSizeBitsLog;
    DCHECK_LT(BitOffset, SCUDO_WORDSIZE);
    Buffer[Region * SizePerRegion + Index] |= CounterMask << BitOffset;
  }
  void setAsAllCountedRange(uptr Region, uptr From, uptr To) const {
    DCHECK_LE(From, To);
    const uptr Top = Min(To + 1, NumCounters);
    for (uptr I = From; I < Top; I++)
      setAsAllCounted(Region, I);
  }

  bool updateAsAllCountedIf(uptr Region, uptr I, uptr MaxCount) {
    const uptr Count = get(Region, I);
    if (Count == CounterMask)
      return true;
    if (Count == MaxCount) {
      setAsAllCounted(Region, I);
      return true;
    }
    return false;
  }
  bool isAllCounted(uptr Region, uptr I) const {
    return get(Region, I) == CounterMask;
  }

  uptr getBufferSize() const { return BufferSize; }

  static const uptr StaticBufferCount = 2048U;

private:
  uptr Regions;
  uptr NumCounters;
  uptr CounterSizeBitsLog;
  uptr CounterMask;
  uptr PackingRatioLog;
  uptr BitOffsetMask;

  uptr SizePerRegion;
  uptr BufferSize;
  uptr *Buffer;
  [[no_unique_address]] MapPlatformData MapData = {};

  static HybridMutex Mutex;
  static uptr StaticBuffer[StaticBufferCount] GUARDED_BY(Mutex);
};

template <class ReleaseRecorderT> class FreePagesRangeTracker {
public:
  explicit FreePagesRangeTracker(ReleaseRecorderT &Recorder)
      : Recorder(Recorder), PageSizeLog(getLog2(getPageSizeCached())) {}

  void processNextPage(bool Released) {
    if (Released) {
      if (!InRange) {
        CurrentRangeStatePage = CurrentPage;
        InRange = true;
      }
    } else {
      closeOpenedRange();
    }
    CurrentPage++;
  }

  void skipPages(uptr N) {
    closeOpenedRange();
    CurrentPage += N;
  }

  void finish() { closeOpenedRange(); }

private:
  void closeOpenedRange() {
    if (InRange) {
      Recorder.releasePageRangeToOS((CurrentRangeStatePage << PageSizeLog),
                                    (CurrentPage << PageSizeLog));
      InRange = false;
    }
  }

  ReleaseRecorderT &Recorder;
  const uptr PageSizeLog;
  bool InRange = false;
  uptr CurrentPage = 0;
  uptr CurrentRangeStatePage = 0;
};

struct PageReleaseContext {
  PageReleaseContext(uptr BlockSize, uptr RegionSize, uptr NumberOfRegions,
                     uptr ReleaseSize, uptr ReleaseOffset = 0)
      : BlockSize(BlockSize), RegionSize(RegionSize),
        NumberOfRegions(NumberOfRegions) {
    PageSize = getPageSizeCached();
    if (BlockSize <= PageSize) {
      if (PageSize % BlockSize == 0) {
        // Same number of chunks per page, no cross overs.
        FullPagesBlockCountMax = PageSize / BlockSize;
        SameBlockCountPerPage = true;
      } else if (BlockSize % (PageSize % BlockSize) == 0) {
        // Some chunks are crossing page boundaries, which means that the page
        // contains one or two partial chunks, but all pages contain the same
        // number of chunks.
        FullPagesBlockCountMax = PageSize / BlockSize + 1;
        SameBlockCountPerPage = true;
      } else {
        // Some chunks are crossing page boundaries, which means that the page
        // contains one or two partial chunks.
        FullPagesBlockCountMax = PageSize / BlockSize + 2;
        SameBlockCountPerPage = false;
      }
    } else {
      if (BlockSize % PageSize == 0) {
        // One chunk covers multiple pages, no cross overs.
        FullPagesBlockCountMax = 1;
        SameBlockCountPerPage = true;
      } else {
        // One chunk covers multiple pages, Some chunks are crossing page
        // boundaries. Some pages contain one chunk, some contain two.
        FullPagesBlockCountMax = 2;
        SameBlockCountPerPage = false;
      }
    }

    // TODO: For multiple regions, it's more complicated to support partial
    // region marking (which includes the complexity of how to handle the last
    // block in a region). We may consider this after markFreeBlocks() accepts
    // only free blocks from the same region.
    if (NumberOfRegions != 1) {
      DCHECK_EQ(ReleaseSize, RegionSize);
      DCHECK_EQ(ReleaseOffset, 0U);
    }

    PagesCount = roundUp(ReleaseSize, PageSize) / PageSize;
    PageSizeLog = getLog2(PageSize);
    RoundedRegionSize = roundUp(RegionSize, PageSize);
    RoundedSize = NumberOfRegions * RoundedRegionSize;
    ReleasePageOffset = ReleaseOffset >> PageSizeLog;
  }

  // PageMap is lazily allocated when markFreeBlocks() is invoked.
  bool hasBlockMarked() const {
    return PageMap.isAllocated();
  }

  void ensurePageMapAllocated() {
    if (PageMap.isAllocated())
      return;
    PageMap.reset(NumberOfRegions, PagesCount, FullPagesBlockCountMax);
    DCHECK(PageMap.isAllocated());
  }

  // Mark all the blocks in the given range [From, to). Instead of visiting all
  // the blocks, we will just mark the page as all counted. Note the `From` and
  // `To` has to be page aligned but with one exception, if `To` is equal to the
  // RegionSize, it's not necessary to be aligned with page size.
  void markRangeAsAllCounted(uptr From, uptr To, uptr Base) {
    DCHECK_LT(From, To);
    DCHECK_EQ(From % PageSize, 0U);

    ensurePageMapAllocated();

    const uptr FromOffset = From - Base;
    const uptr ToOffset = To - Base;

    const uptr RegionIndex =
        NumberOfRegions == 1U ? 0 : FromOffset / RegionSize;
    if (SCUDO_DEBUG) {
      const uptr ToRegionIndex =
          NumberOfRegions == 1U ? 0 : (ToOffset - 1) / RegionSize;
      CHECK_EQ(RegionIndex, ToRegionIndex);
    }

    uptr FromInRegion = FromOffset - RegionIndex * RegionSize;
    uptr ToInRegion = ToOffset - RegionIndex * RegionSize;
    uptr FirstBlockInRange = roundUpSlow(FromInRegion, BlockSize);

    // The straddling block sits across entire range.
    if (FirstBlockInRange >= ToInRegion)
      return;

    // First block may not sit at the first pape in the range, move
    // `FromInRegion` to the first block page.
    FromInRegion = roundDown(FirstBlockInRange, PageSize);

    // When The first block is not aligned to the range boundary, which means
    // there is a block sitting acorss `From`, that looks like,
    //
    //   From                                             To
    //     V                                               V
    //     +-----------------------------------------------+
    //  +-----+-----+-----+-----+
    //  |     |     |     |     | ...
    //  +-----+-----+-----+-----+
    //     |-    first page     -||-    second page    -||- ...
    //
    // Therefore, we can't just mark the first page as all counted. Instead, we
    // increment the number of blocks in the first page in the page map and
    // then round up the `From` to the next page.
    if (FirstBlockInRange != FromInRegion) {
      DCHECK_GT(FromInRegion + PageSize, FirstBlockInRange);
      uptr NumBlocksInFirstPage =
          (FromInRegion + PageSize - FirstBlockInRange + BlockSize - 1) /
          BlockSize;
      PageMap.incN(RegionIndex, getPageIndex(FromInRegion),
                   NumBlocksInFirstPage);
      FromInRegion = roundUp(FromInRegion + 1, PageSize);
    }

    uptr LastBlockInRange = roundDownSlow(ToInRegion - 1, BlockSize);
    if (LastBlockInRange < FromInRegion)
      return;

    // When the last block sits across `To`, we can't just mark the pages
    // occupied by the last block as all counted. Instead, we increment the
    // counters of those pages by 1. The exception is that if it's the last
    // block in the region, it's fine to mark those pages as all counted.
    if (LastBlockInRange + BlockSize != RegionSize) {
      DCHECK_EQ(ToInRegion % PageSize, 0U);
      // The case below is like,
      //
      //   From                                      To
      //     V                                        V
      //     +----------------------------------------+
      //                          +-----+-----+-----+-----+
      //                          |     |     |     |     | ...
      //                          +-----+-----+-----+-----+
      //                    ... -||-    last page    -||-    next page    -|
      //
      // The last block is not aligned to `To`, we need to increment the
      // counter of `next page` by 1.
      if (LastBlockInRange + BlockSize != ToInRegion) {
        PageMap.incRange(RegionIndex, getPageIndex(ToInRegion),
                         getPageIndex(LastBlockInRange + BlockSize - 1));
      }
    } else {
      ToInRegion = RegionSize;
    }

    // After handling the first page and the last block, it's safe to mark any
    // page in between the range [From, To).
    if (FromInRegion < ToInRegion) {
      PageMap.setAsAllCountedRange(RegionIndex, getPageIndex(FromInRegion),
                                   getPageIndex(ToInRegion - 1));
    }
  }

  template<class TransferBatchT, typename DecompactPtrT>
  void markFreeBlocks(const IntrusiveList<TransferBatchT> &FreeList,
                      DecompactPtrT DecompactPtr, uptr Base) {
    ensurePageMapAllocated();

    const uptr LastBlockInRegion = ((RegionSize / BlockSize) - 1U) * BlockSize;

    // The last block in a region may not use the entire page, so if it's free,
    // we mark the following "pretend" memory block(s) as free.
    auto markLastBlock = [this, LastBlockInRegion](const uptr RegionIndex) {
      uptr PInRegion = LastBlockInRegion + BlockSize;
      while (PInRegion < RoundedRegionSize) {
        PageMap.incRange(RegionIndex, getPageIndex(PInRegion),
                         getPageIndex(PInRegion + BlockSize - 1));
        PInRegion += BlockSize;
      }
    };

    // Iterate over free chunks and count how many free chunks affect each
    // allocated page.
    if (BlockSize <= PageSize && PageSize % BlockSize == 0) {
      // Each chunk affects one page only.
      for (const auto &It : FreeList) {
        for (u16 I = 0; I < It.getCount(); I++) {
          const uptr P = DecompactPtr(It.get(I)) - Base;
          if (P >= RoundedSize)
            continue;
          const uptr RegionIndex = NumberOfRegions == 1U ? 0 : P / RegionSize;
          const uptr PInRegion = P - RegionIndex * RegionSize;
          PageMap.inc(RegionIndex, getPageIndex(PInRegion));
          if (PInRegion == LastBlockInRegion)
            markLastBlock(RegionIndex);
        }
      }
    } else {
      // In all other cases chunks might affect more than one page.
      DCHECK_GE(RegionSize, BlockSize);
      for (const auto &It : FreeList) {
        for (u16 I = 0; I < It.getCount(); I++) {
          const uptr P = DecompactPtr(It.get(I)) - Base;
          if (P >= RoundedSize)
            continue;
          const uptr RegionIndex = NumberOfRegions == 1U ? 0 : P / RegionSize;
          uptr PInRegion = P - RegionIndex * RegionSize;
          PageMap.incRange(RegionIndex, getPageIndex(PInRegion),
                           getPageIndex(PInRegion + BlockSize - 1));
          if (PInRegion == LastBlockInRegion)
            markLastBlock(RegionIndex);
        }
      }
    }
  }

  uptr getPageIndex(uptr P) { return (P >> PageSizeLog) - ReleasePageOffset; }

  uptr BlockSize;
  uptr RegionSize;
  uptr NumberOfRegions;
  // For partial region marking, some pages in front are not needed to be
  // counted.
  uptr ReleasePageOffset;
  uptr PageSize;
  uptr PagesCount;
  uptr PageSizeLog;
  uptr RoundedRegionSize;
  uptr RoundedSize;
  uptr FullPagesBlockCountMax;
  bool SameBlockCountPerPage;
  RegionPageMap PageMap;
};

// Try to release the page which doesn't have any in-used block, i.e., they are
// all free blocks. The `PageMap` will record the number of free blocks in each
// page.
template <class ReleaseRecorderT, typename SkipRegionT>
NOINLINE void
releaseFreeMemoryToOS(PageReleaseContext &Context,
                      ReleaseRecorderT &Recorder, SkipRegionT SkipRegion) {
  const uptr PageSize = Context.PageSize;
  const uptr BlockSize = Context.BlockSize;
  const uptr PagesCount = Context.PagesCount;
  const uptr NumberOfRegions = Context.NumberOfRegions;
  const uptr ReleasePageOffset = Context.ReleasePageOffset;
  const uptr FullPagesBlockCountMax = Context.FullPagesBlockCountMax;
  const bool SameBlockCountPerPage = Context.SameBlockCountPerPage;
  RegionPageMap &PageMap = Context.PageMap;

  // Iterate over pages detecting ranges of pages with chunk Counters equal
  // to the expected number of chunks for the particular page.
  FreePagesRangeTracker<ReleaseRecorderT> RangeTracker(Recorder);
  if (SameBlockCountPerPage) {
    // Fast path, every page has the same number of chunks affecting it.
    for (uptr I = 0; I < NumberOfRegions; I++) {
      if (SkipRegion(I)) {
        RangeTracker.skipPages(PagesCount);
        continue;
      }
      for (uptr J = 0; J < PagesCount; J++) {
        const bool CanRelease =
            PageMap.updateAsAllCountedIf(I, J, FullPagesBlockCountMax);
        RangeTracker.processNextPage(CanRelease);
      }
    }
  } else {
    // Slow path, go through the pages keeping count how many chunks affect
    // each page.
    const uptr Pn = BlockSize < PageSize ? PageSize / BlockSize : 1;
    const uptr Pnc = Pn * BlockSize;
    // The idea is to increment the current page pointer by the first chunk
    // size, middle portion size (the portion of the page covered by chunks
    // except the first and the last one) and then the last chunk size, adding
    // up the number of chunks on the current page and checking on every step
    // whether the page boundary was crossed.
    for (uptr I = 0; I < NumberOfRegions; I++) {
      if (SkipRegion(I)) {
        RangeTracker.skipPages(PagesCount);
        continue;
      }
      uptr PrevPageBoundary = 0;
      uptr CurrentBoundary = 0;
      if (ReleasePageOffset > 0) {
        PrevPageBoundary = ReleasePageOffset * PageSize;
        CurrentBoundary = roundUpSlow(PrevPageBoundary, BlockSize);
      }
      for (uptr J = 0; J < PagesCount; J++) {
        const uptr PageBoundary = PrevPageBoundary + PageSize;
        uptr BlocksPerPage = Pn;
        if (CurrentBoundary < PageBoundary) {
          if (CurrentBoundary > PrevPageBoundary)
            BlocksPerPage++;
          CurrentBoundary += Pnc;
          if (CurrentBoundary < PageBoundary) {
            BlocksPerPage++;
            CurrentBoundary += BlockSize;
          }
        }
        PrevPageBoundary = PageBoundary;
        const bool CanRelease =
            PageMap.updateAsAllCountedIf(I, J, BlocksPerPage);
        RangeTracker.processNextPage(CanRelease);
      }
    }
  }
  RangeTracker.finish();
}

// An overload releaseFreeMemoryToOS which doesn't require the page usage
// information after releasing.
template <class TransferBatchT, class ReleaseRecorderT, typename DecompactPtrT,
          typename SkipRegionT>
NOINLINE void
releaseFreeMemoryToOS(const IntrusiveList<TransferBatchT> &FreeList,
                      uptr RegionSize, uptr NumberOfRegions, uptr BlockSize,
                      ReleaseRecorderT &Recorder, DecompactPtrT DecompactPtr,
                      SkipRegionT SkipRegion) {
  PageReleaseContext Context(BlockSize, /*ReleaseSize=*/RegionSize, RegionSize,
                             NumberOfRegions);
  Context.markFreeBlocks(FreeList, DecompactPtr, Recorder.getBase());
  releaseFreeMemoryToOS(Context, Recorder, SkipRegion);
}

} // namespace scudo

#endif // SCUDO_RELEASE_H_
