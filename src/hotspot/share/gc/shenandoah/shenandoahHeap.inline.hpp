/*
 * Copyright (c) 2015, 2020, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP

#include "gc/shenandoah/shenandoahHeap.hpp"

#include "classfile/javaClasses.inline.hpp"
#include "gc/shared/markBitMap.inline.hpp"
#include "gc/shared/threadLocalAllocBuffer.inline.hpp"
#include "gc/shared/continuationGCSupport.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc/shenandoah/shenandoahCollectionSet.inline.hpp"
#include "gc/shenandoah/shenandoahForwarding.inline.hpp"
#include "gc/shenandoah/shenandoahWorkGroup.hpp"
#include "gc/shenandoah/shenandoahHeapRegionSet.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc/shenandoah/shenandoahControlThread.hpp"
#include "gc/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.inline.hpp"
#include "gc/shenandoah/shenandoahThreadLocalData.hpp"
#include "gc/shenandoah/shenandoahScanRemembered.inline.hpp"
#include "gc/shenandoah/mode/shenandoahMode.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/prefetch.inline.hpp"
#include "utilities/copy.hpp"
#include "utilities/globalDefinitions.hpp"

inline ShenandoahHeap* ShenandoahHeap::heap() {
  return named_heap<ShenandoahHeap>(CollectedHeap::Shenandoah);
}

inline ShenandoahHeapRegion* ShenandoahRegionIterator::next() {
  size_t new_index = Atomic::add(&_index, (size_t) 1, memory_order_relaxed);
  // get_region() provides the bounds-check and returns NULL on OOB.
  return _heap->get_region(new_index - 1);
}

inline bool ShenandoahHeap::has_forwarded_objects() const {
  return _gc_state.is_set(HAS_FORWARDED);
}

inline WorkerThreads* ShenandoahHeap::workers() const {
  return _workers;
}

inline WorkerThreads* ShenandoahHeap::safepoint_workers() {
  return _safepoint_workers;
}

inline size_t ShenandoahHeap::heap_region_index_containing(const void* addr) const {
  uintptr_t region_start = ((uintptr_t) addr);
  uintptr_t index = (region_start - (uintptr_t) base()) >> ShenandoahHeapRegion::region_size_bytes_shift();
  assert(index < num_regions(), "Region index is in bounds: " PTR_FORMAT, p2i(addr));
  return index;
}

inline ShenandoahHeapRegion* const ShenandoahHeap::heap_region_containing(const void* addr) const {
  size_t index = heap_region_index_containing(addr);
  ShenandoahHeapRegion* const result = get_region(index);
  assert(addr >= result->bottom() && addr < result->end(), "Heap region contains the address: " PTR_FORMAT, p2i(addr));
  return result;
}

inline void ShenandoahHeap::enter_evacuation(Thread* t) {
  _oom_evac_handler.enter_evacuation(t);
}

inline void ShenandoahHeap::leave_evacuation(Thread* t) {
  _oom_evac_handler.leave_evacuation(t);
}

template <class T>
inline void ShenandoahHeap::update_with_forwarded(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    if (in_collection_set(obj)) {
      // Corner case: when evacuation fails, there are objects in collection
      // set that are not really forwarded. We can still go and try and update them
      // (uselessly) to simplify the common path.
      shenandoah_assert_forwarded_except(p, obj, cancelled_gc());
      oop fwd = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      shenandoah_assert_not_in_cset_except(p, fwd, cancelled_gc());

      // Unconditionally store the update: no concurrent updates expected.
      RawAccess<IS_NOT_NULL>::oop_store(p, fwd);
    }
  }
}

template <class T>
inline void ShenandoahHeap::conc_update_with_forwarded(T* p) {
  T o = RawAccess<>::oop_load(p);
  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);
    if (in_collection_set(obj)) {
      // Corner case: when evacuation fails, there are objects in collection
      // set that are not really forwarded. We can still go and try CAS-update them
      // (uselessly) to simplify the common path.
      shenandoah_assert_forwarded_except(p, obj, cancelled_gc());
      oop fwd = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      shenandoah_assert_not_in_cset_except(p, fwd, cancelled_gc());

      // Sanity check: we should not be updating the cset regions themselves,
      // unless we are recovering from the evacuation failure.
      shenandoah_assert_not_in_cset_loc_except(p, !is_in(p) || cancelled_gc());

      // Either we succeed in updating the reference, or something else gets in our way.
      // We don't care if that is another concurrent GC update, or another mutator update.
      atomic_update_oop(fwd, p, obj);
    }
  }
}

// Atomic updates of heap location. This is only expected to work with updating the same
// logical object with its forwardee. The reason why we need stronger-than-relaxed memory
// ordering has to do with coordination with GC barriers and mutator accesses.
//
// In essence, stronger CAS access is required to maintain the transitive chains that mutator
// accesses build by themselves. To illustrate this point, consider the following example.
//
// Suppose "o" is the object that has a field "x" and the reference to "o" is stored
// to field at "addr", which happens to be Java volatile field. Normally, the accesses to volatile
// field at "addr" would be matched with release/acquire barriers. This changes when GC moves
// the object under mutator feet.
//
// Thread 1 (Java)
//         // --- previous access starts here
//         ...
//   T1.1: store(&o.x, 1, mo_relaxed)
//   T1.2: store(&addr, o, mo_release) // volatile store
//
//         // --- new access starts here
//         // LRB: copy and install the new copy to fwdptr
//   T1.3: var copy = copy(o)
//   T1.4: cas(&fwd, t, copy, mo_release) // pointer-mediated publication
//         <access continues>
//
// Thread 2 (GC updater)
//   T2.1: var f = load(&fwd, mo_{consume|acquire}) // pointer-mediated acquisition
//   T2.2: cas(&addr, o, f, mo_release) // this method
//
// Thread 3 (Java)
//   T3.1: var o = load(&addr, mo_acquire) // volatile read
//   T3.2: if (o != null)
//   T3.3:   var r = load(&o.x, mo_relaxed)
//
// r is guaranteed to contain "1".
//
// Without GC involvement, there is synchronizes-with edge from T1.2 to T3.1,
// which guarantees this. With GC involvement, when LRB copies the object and
// another thread updates the reference to it, we need to have the transitive edge
// from T1.4 to T2.1 (that one is guaranteed by forwarding accesses), plus the edge
// from T2.2 to T3.1 (which is brought by this CAS).
//
// Note that we do not need to "acquire" in these methods, because we do not read the
// failure witnesses contents on any path, and "release" is enough.
//

inline void ShenandoahHeap::atomic_update_oop(oop update, oop* addr, oop compare) {
  assert(is_aligned(addr, HeapWordSize), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  Atomic::cmpxchg(addr, compare, update, memory_order_release);
}

inline void ShenandoahHeap::atomic_update_oop(oop update, narrowOop* addr, narrowOop compare) {
  assert(is_aligned(addr, sizeof(narrowOop)), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  narrowOop u = CompressedOops::encode(update);
  Atomic::cmpxchg(addr, compare, u, memory_order_release);
}

inline void ShenandoahHeap::atomic_update_oop(oop update, narrowOop* addr, oop compare) {
  assert(is_aligned(addr, sizeof(narrowOop)), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  narrowOop c = CompressedOops::encode(compare);
  narrowOop u = CompressedOops::encode(update);
  Atomic::cmpxchg(addr, c, u, memory_order_release);
}

inline bool ShenandoahHeap::atomic_update_oop_check(oop update, oop* addr, oop compare) {
  assert(is_aligned(addr, HeapWordSize), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  return (oop) Atomic::cmpxchg(addr, compare, update, memory_order_release) == compare;
}

inline bool ShenandoahHeap::atomic_update_oop_check(oop update, narrowOop* addr, narrowOop compare) {
  assert(is_aligned(addr, sizeof(narrowOop)), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  narrowOop u = CompressedOops::encode(update);
  return (narrowOop) Atomic::cmpxchg(addr, compare, u, memory_order_release) == compare;
}

inline bool ShenandoahHeap::atomic_update_oop_check(oop update, narrowOop* addr, oop compare) {
  assert(is_aligned(addr, sizeof(narrowOop)), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  narrowOop c = CompressedOops::encode(compare);
  narrowOop u = CompressedOops::encode(update);
  return CompressedOops::decode(Atomic::cmpxchg(addr, c, u, memory_order_release)) == compare;
}

// The memory ordering discussion above does not apply for methods that store NULLs:
// then, there is no transitive reads in mutator (as we see NULLs), and we can do
// relaxed memory ordering there.

inline void ShenandoahHeap::atomic_clear_oop(oop* addr, oop compare) {
  assert(is_aligned(addr, HeapWordSize), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  Atomic::cmpxchg(addr, compare, oop(), memory_order_relaxed);
}

inline void ShenandoahHeap::atomic_clear_oop(narrowOop* addr, oop compare) {
  assert(is_aligned(addr, sizeof(narrowOop)), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  narrowOop cmp = CompressedOops::encode(compare);
  Atomic::cmpxchg(addr, cmp, narrowOop(), memory_order_relaxed);
}

inline void ShenandoahHeap::atomic_clear_oop(narrowOop* addr, narrowOop compare) {
  assert(is_aligned(addr, sizeof(narrowOop)), "Address should be aligned: " PTR_FORMAT, p2i(addr));
  Atomic::cmpxchg(addr, compare, narrowOop(), memory_order_relaxed);
}

inline bool ShenandoahHeap::cancelled_gc() const {
  return _cancelled_gc.get() == CANCELLED;
}

inline bool ShenandoahHeap::check_cancelled_gc_and_yield(bool sts_active) {
  if (! (sts_active && ShenandoahSuspendibleWorkers)) {
    return cancelled_gc();
  }

  jbyte prev = _cancelled_gc.cmpxchg(NOT_CANCELLED, CANCELLABLE);
  if (prev == CANCELLABLE || prev == NOT_CANCELLED) {
    if (SuspendibleThreadSet::should_yield()) {
      SuspendibleThreadSet::yield();
    }

    // Back to CANCELLABLE. The thread that poked NOT_CANCELLED first gets
    // to restore to CANCELLABLE.
    if (prev == CANCELLABLE) {
      _cancelled_gc.set(CANCELLABLE);
    }
    return false;
  } else {
    return true;
  }
}

inline void ShenandoahHeap::clear_cancelled_gc(bool clear_oom_handler) {
  _cancelled_gc.set(CANCELLABLE);
  if (_cancel_requested_time > 0) {
    double cancel_time = os::elapsedTime() - _cancel_requested_time;
    log_info(gc)("GC cancellation took %.3fs", cancel_time);
    _cancel_requested_time = 0;
  }

  if (clear_oom_handler) {
    _oom_evac_handler.clear();
  }
}

inline HeapWord* ShenandoahHeap::allocate_from_gclab(Thread* thread, size_t size) {
  assert(UseTLAB, "TLABs should be enabled");

  PLAB* gclab = ShenandoahThreadLocalData::gclab(thread);
  if (gclab == NULL) {
    assert(!thread->is_Java_thread() && !thread->is_Worker_thread(),
           "Performance: thread should have GCLAB: %s", thread->name());
    // No GCLABs in this thread, fallback to shared allocation
    return NULL;
  }
  HeapWord* obj = gclab->allocate(size);
  if (obj != NULL) {
    return obj;
  }
  return allocate_from_gclab_slow(thread, size);
}

inline HeapWord* ShenandoahHeap::allocate_from_plab(Thread* thread, size_t size, bool is_promotion) {
  assert(UseTLAB, "TLABs should be enabled");

  PLAB* plab = ShenandoahThreadLocalData::plab(thread);
  HeapWord* obj;
  if (plab == NULL) {
    assert(!thread->is_Java_thread() && !thread->is_Worker_thread(), "Performance: thread should have PLAB: %s", thread->name());
    // No PLABs in this thread, fallback to shared allocation
    return nullptr;
  } else if (is_promotion && (plab->words_remaining() > 0) && !ShenandoahThreadLocalData::allow_plab_promotions(thread)) {
    return nullptr;
  }
  // if plab->word_size() <= 0, thread's plab not yet initialized for this pass, so allow_plab_promotions() is not trustworthy
  obj = plab->allocate(size);
  if ((obj == nullptr) && (plab->words_remaining() < PLAB::min_size())) {
    // allocate_from_plab_slow will establish allow_plab_promotions(thread) for future invocations
    obj = allocate_from_plab_slow(thread, size, is_promotion);
  }
  // if plab->words_remaining() >= PLAB::min_size(), just return nullptr so we can use a shared allocation
  if (obj == nullptr) {
    return nullptr;
  }

  if (is_promotion) {
    ShenandoahThreadLocalData::add_to_plab_promoted(thread, size * HeapWordSize);
  } else {
    ShenandoahThreadLocalData::add_to_plab_evacuated(thread, size * HeapWordSize);
  }
  return obj;
}

inline oop ShenandoahHeap::evacuate_object(oop p, Thread* thread) {
  assert(thread == Thread::current(), "Expected thread parameter to be current thread.");
  if (ShenandoahThreadLocalData::is_oom_during_evac(thread)) {
    // This thread went through the OOM during evac protocol and it is safe to return
    // the forward pointer. It must not attempt to evacuate any more.
    return ShenandoahBarrierSet::resolve_forwarded(p);
  }

  assert(ShenandoahThreadLocalData::is_evac_allowed(thread), "must be enclosed in oom-evac scope");

  ShenandoahHeapRegion* r = heap_region_containing(p);
  assert(!r->is_humongous(), "never evacuate humongous objects");

  ShenandoahRegionAffiliation target_gen = r->affiliation();
  if (mode()->is_generational() && ShenandoahHeap::heap()->is_gc_generation_young() &&
      target_gen == YOUNG_GENERATION && ShenandoahPromoteTenuredObjects) {
    markWord mark = p->mark();
    if (mark.is_marked()) {
      // Already forwarded.
      return ShenandoahBarrierSet::resolve_forwarded(p);
    }
    if (mark.has_displaced_mark_helper()) {
      // We don't want to deal with MT here just to ensure we read the right mark word.
      // Skip the potential promotion attempt for this one.
    } else if (r->age() + mark.age() >= InitialTenuringThreshold) {
      oop result = try_evacuate_object(p, thread, r, OLD_GENERATION);
      if (result != NULL) {
        return result;
      }
      // If we failed to promote this aged object, we'll fall through to code below and evacuate to young-gen.
    }
  }
  return try_evacuate_object(p, thread, r, target_gen);
}

// try_evacuate_object registers the object and dirties the associated remembered set information when evacuating
// to OLD_GENERATION.
inline oop ShenandoahHeap::try_evacuate_object(oop p, Thread* thread, ShenandoahHeapRegion* from_region,
                                               ShenandoahRegionAffiliation target_gen) {
  bool alloc_from_lab = true;
  bool has_plab = false;
  HeapWord* copy = NULL;
  size_t size = p->size();
  bool is_promotion = (target_gen == OLD_GENERATION) && from_region->is_young();

#ifdef ASSERT
  if (ShenandoahOOMDuringEvacALot &&
      (os::random() & 1) == 0) { // Simulate OOM every ~2nd slow-path call
        copy = NULL;
  } else {
#endif
    if (UseTLAB) {
      switch (target_gen) {
        case YOUNG_GENERATION: {
           copy = allocate_from_gclab(thread, size);
           if ((copy == nullptr) && (size < ShenandoahThreadLocalData::gclab_size(thread))) {
             // GCLAB allocation failed because we are bumping up against the limit on young evacuation reserve.  Try resetting
             // the desired GCLAB size and retry GCLAB allocation to avoid cascading of shared memory allocations.
             ShenandoahThreadLocalData::set_gclab_size(thread, PLAB::min_size());
             copy = allocate_from_gclab(thread, size);
             // If we still get nullptr, we'll try a shared allocation below.
           }
           break;
        }
        case OLD_GENERATION: {
           if (ShenandoahUsePLAB) {
             PLAB* plab = ShenandoahThreadLocalData::plab(thread);
             if (plab != nullptr) {
               has_plab = true;
             }
             copy = allocate_from_plab(thread, size, is_promotion);
             if ((copy == nullptr) && (size < ShenandoahThreadLocalData::plab_size(thread)) &&
                 ShenandoahThreadLocalData::plab_retries_enabled(thread)) {
               // PLAB allocation failed because we are bumping up against the limit on old evacuation reserve or because
               // the requested object does not fit within the current plab but the plab still has an "abundance" of memory,
               // where abundance is defined as >= PLAB::min_size().  In the former case, we try resetting the desired
               // PLAB size and retry PLAB allocation to avoid cascading of shared memory allocations.

               // In this situation, PLAB memory is precious.  We'll try to preserve our existing PLAB by forcing
               // this particular allocation to be shared.
               if (plab->words_remaining() < PLAB::min_size()) {
                 ShenandoahThreadLocalData::set_plab_size(thread, PLAB::min_size());
                 copy = allocate_from_plab(thread, size, is_promotion);
                 // If we still get nullptr, we'll try a shared allocation below.
                 if (copy == nullptr) {
                   // If retry fails, don't continue to retry until we have success (probably in next GC pass)
                   ShenandoahThreadLocalData::disable_plab_retries(thread);
                 }
               }
               // else, copy still equals nullptr.  this causes shared allocation below, preserving this plab for future needs.
             }
           }
           break;
        }
        default: {
          ShouldNotReachHere();
          break;
        }
      }
    }

    if (copy == NULL) {
      // If we failed to allocate in LAB, we'll try a shared allocation.
      if (!is_promotion || !has_plab || (size > PLAB::min_size())) {
        ShenandoahAllocRequest req = ShenandoahAllocRequest::for_shared_gc(size, target_gen);
        copy = allocate_memory(req, is_promotion);
        alloc_from_lab = false;
      }
      // else, we leave copy equal to NULL, signaling a promotion failure below if appropriate.
      // We choose not to promote objects smaller than PLAB::min_size() by way of shared allocations, as this is too
      // costly.  Instead, we'll simply "evacuate" to young-gen memory (using a GCLAB) and will promote in a future
      // evacuation pass.  This condition is denoted by: is_promotion && has_plab && (size <= PLAB::min_size())
    }
#ifdef ASSERT
  }
#endif

  if (copy == NULL) {
    if (target_gen == OLD_GENERATION) {
      assert(mode()->is_generational(), "Should only be here in generational mode.");
      if (from_region->is_young()) {
        // Signal that promotion failed. Will evacuate this old object somewhere in young gen.

        // We squelch excessive reports to reduce noise in logs.  Squelch enforcement is not "perfect" because
        // this same code can be in-lined in multiple contexts, and each context will have its own copy of the static
        // last_report_epoch and this_epoch_report_count variables.
        const uint MaxReportsPerEpoch = 4;
        static uint last_report_epoch = 0;
        static uint epoch_report_count = 0;
        PLAB* plab = ShenandoahThreadLocalData::plab(thread);
        size_t words_remaining = (plab == nullptr)? 0: plab->words_remaining();
        const char* promote_enabled = ShenandoahThreadLocalData::allow_plab_promotions(thread)? "enabled": "disabled";
        size_t promotion_reserve;
        size_t promotion_expended;
        // We can only query GCId::current() if current thread is a named thread.  If current thread is not a
        // named thread, then we don't even try to squelch the promotion failure report, we don't update the
        // the last_report_epoch, and we don't increment the epoch_report_count
        if (thread->is_Named_thread()) {
          uint gc_id = GCId::current();
          if ((gc_id != last_report_epoch) || (epoch_report_count++ < MaxReportsPerEpoch)) {
            {
              // Promotion failures should be very rare.  Invest in providing useful diagnostic info.
              ShenandoahHeapLocker locker(lock());
              promotion_reserve = get_promoted_reserve();
              promotion_expended = get_promoted_expended();
            }
            log_info(gc, ergo)("Promotion failed, size " SIZE_FORMAT ", has plab? %s, PLAB remaining: " SIZE_FORMAT
                               ", plab promotions %s, promotion reserve: " SIZE_FORMAT ", promotion expended: " SIZE_FORMAT,
                               size, plab == nullptr? "no": "yes",
                               words_remaining, promote_enabled, promotion_reserve, promotion_expended);
            if ((gc_id == last_report_epoch) && (epoch_report_count >= MaxReportsPerEpoch)) {
              log_info(gc, ergo)("Squelching additional promotion failure reports for epoch %d\n", last_report_epoch);
            } else if (gc_id != last_report_epoch) {
              last_report_epoch = gc_id;;
              epoch_report_count = 1;
            }
          }
        } else if (epoch_report_count < MaxReportsPerEpoch) {
          // Unnamed threads are much less common than named threads.  In the rare case that an unnamed thread experiences
          // a promotion failure before a named thread within a given epoch, the report for the unnamed thread will be squelched.
          {
            // Promotion failures should be very rare.  Invest in providing useful diagnostic info.
            ShenandoahHeapLocker locker(lock());
            promotion_reserve = get_promoted_reserve();
            promotion_expended = get_promoted_expended();
          }
          log_info(gc, ergo)("Promotion failed (unfiltered), size " SIZE_FORMAT ", has plab? %s, PLAB remaining: " SIZE_FORMAT
                             ", plab promotions %s, promotion reserve: " SIZE_FORMAT ", promotion expended: " SIZE_FORMAT,
                             size, plab == nullptr? "no": "yes",
                             words_remaining, promote_enabled, promotion_reserve, promotion_expended);
        }
        handle_promotion_failure();
        return NULL;
      } else {
        // Remember that evacuation to old gen failed. We'll want to trigger a full gc to recover from this
        // after the evacuation threads have finished.
        handle_old_evacuation_failure();
      }
    }

    control_thread()->handle_alloc_failure_evac(size);

    _oom_evac_handler.handle_out_of_memory_during_evacuation();

    return ShenandoahBarrierSet::resolve_forwarded(p);
  }

  // Copy the object:
  Copy::aligned_disjoint_words(cast_from_oop<HeapWord*>(p), copy, size);

  oop copy_val = cast_to_oop(copy);

  if (mode()->is_generational() && target_gen == YOUNG_GENERATION && is_aging_cycle()) {
    ShenandoahHeap::increase_object_age(copy_val, from_region->age() + 1);
  }

  // Try to install the new forwarding pointer.
  ContinuationGCSupport::relativize_stack_chunk(copy_val);

  oop result = ShenandoahForwarding::try_update_forwardee(p, copy_val);
  if (result == copy_val) {
    // Successfully evacuated. Our copy is now the public one!
    if (mode()->is_generational() && target_gen == OLD_GENERATION) {
      handle_old_evacuation(copy, size, from_region->is_young());
    }
    shenandoah_assert_correct(NULL, copy_val);
    return copy_val;
  }  else {
    // Failed to evacuate. We need to deal with the object that is left behind. Since this
    // new allocation is certainly after TAMS, it will be considered live in the next cycle.
    // But if it happens to contain references to evacuated regions, those references would
    // not get updated for this stale copy during this cycle, and we will crash while scanning
    // it the next cycle.
    if (alloc_from_lab) {
       // For LAB allocations, it is enough to rollback the allocation ptr. Either the next
       // object will overwrite this stale copy, or the filler object on LAB retirement will
       // do this.
       switch (target_gen) {
         case YOUNG_GENERATION: {
             ShenandoahThreadLocalData::gclab(thread)->undo_allocation(copy, size);
            break;
         }
         case OLD_GENERATION: {
            ShenandoahThreadLocalData::plab(thread)->undo_allocation(copy, size);
            if (is_promotion) {
              ShenandoahThreadLocalData::subtract_from_plab_promoted(thread, size * HeapWordSize);
            } else {
              ShenandoahThreadLocalData::subtract_from_plab_evacuated(thread, size * HeapWordSize);
            }
            break;
         }
         default: {
           ShouldNotReachHere();
           break;
         }
       }
    } else {
      // For non-LAB allocations, we have no way to retract the allocation, and
      // have to explicitly overwrite the copy with the filler object. With that overwrite,
      // we have to keep the fwdptr initialized and pointing to our (stale) copy.
      fill_with_object(copy, size);
      shenandoah_assert_correct(NULL, copy_val);
      // For non-LAB allocations, the object has already been registered
    }
    shenandoah_assert_correct(NULL, result);
    return result;
  }
}

void ShenandoahHeap::increase_object_age(oop obj, uint additional_age) {
  markWord w = obj->has_displaced_mark() ? obj->displaced_mark() : obj->mark();
  w = w.set_age(MIN2(markWord::max_age, w.age() + additional_age));
  if (obj->has_displaced_mark()) {
    obj->set_displaced_mark(w);
  } else {
    obj->set_mark(w);
  }
}

inline bool ShenandoahHeap::clear_old_evacuation_failure() {
  return _old_gen_oom_evac.try_unset();
}

inline bool ShenandoahHeap::is_old(oop obj) const {
  return is_gc_generation_young() && is_in_old(obj);
}

inline bool ShenandoahHeap::requires_marking(const void* entry) const {
  oop obj = cast_to_oop(entry);
  return !_marking_context->is_marked_strong(obj);
}

inline bool ShenandoahHeap::in_collection_set(oop p) const {
  assert(collection_set() != NULL, "Sanity");
  return collection_set()->is_in(p);
}

inline bool ShenandoahHeap::in_collection_set_loc(void* p) const {
  assert(collection_set() != NULL, "Sanity");
  return collection_set()->is_in_loc(p);
}

inline bool ShenandoahHeap::is_stable() const {
  return _gc_state.is_clear();
}

inline bool ShenandoahHeap::is_idle() const {
  return _gc_state.is_unset(YOUNG_MARKING | OLD_MARKING | EVACUATION | UPDATEREFS);
}

inline bool ShenandoahHeap::is_concurrent_mark_in_progress() const {
  return _gc_state.is_set(YOUNG_MARKING | OLD_MARKING);
}

inline bool ShenandoahHeap::is_concurrent_young_mark_in_progress() const {
  return _gc_state.is_set(YOUNG_MARKING);
}

inline bool ShenandoahHeap::is_concurrent_old_mark_in_progress() const {
  return _gc_state.is_set(OLD_MARKING);
}

inline bool ShenandoahHeap::is_evacuation_in_progress() const {
  return _gc_state.is_set(EVACUATION);
}

inline bool ShenandoahHeap::is_gc_in_progress_mask(uint mask) const {
  return _gc_state.is_set(mask);
}

inline bool ShenandoahHeap::is_degenerated_gc_in_progress() const {
  return _degenerated_gc_in_progress.is_set();
}

inline bool ShenandoahHeap::is_full_gc_in_progress() const {
  return _full_gc_in_progress.is_set();
}

inline bool ShenandoahHeap::is_full_gc_move_in_progress() const {
  return _full_gc_move_in_progress.is_set();
}

inline bool ShenandoahHeap::is_update_refs_in_progress() const {
  return _gc_state.is_set(UPDATEREFS);
}

inline bool ShenandoahHeap::is_stw_gc_in_progress() const {
  return is_full_gc_in_progress() || is_degenerated_gc_in_progress();
}

inline bool ShenandoahHeap::is_concurrent_strong_root_in_progress() const {
  return _concurrent_strong_root_in_progress.is_set();
}

inline bool ShenandoahHeap::is_concurrent_weak_root_in_progress() const {
  return _gc_state.is_set(WEAK_ROOTS);
}

inline bool ShenandoahHeap::is_aging_cycle() const {
  return _is_aging_cycle.is_set();
}

inline bool ShenandoahHeap::is_prepare_for_old_mark_in_progress() const {
  return _prepare_for_old_mark;
}

inline size_t ShenandoahHeap::set_promoted_reserve(size_t new_val) {
  size_t orig = _promoted_reserve;
  _promoted_reserve = new_val;
  return orig;
}

inline size_t ShenandoahHeap::get_promoted_reserve() const {
  return _promoted_reserve;
}

// returns previous value
size_t ShenandoahHeap::capture_old_usage(size_t old_usage) {
  size_t previous_value = _captured_old_usage;
  _captured_old_usage = old_usage;
  return previous_value;
}

void ShenandoahHeap::set_previous_promotion(size_t promoted_bytes) {
  shenandoah_assert_heaplocked();
  _previous_promotion = promoted_bytes;
}

size_t ShenandoahHeap::get_previous_promotion() const {
  return _previous_promotion;
}

inline size_t ShenandoahHeap::set_old_evac_reserve(size_t new_val) {
  size_t orig = _old_evac_reserve;
  _old_evac_reserve = new_val;
  return orig;
}

inline size_t ShenandoahHeap::get_old_evac_reserve() const {
  return _old_evac_reserve;
}

inline void ShenandoahHeap::reset_old_evac_expended() {
  Atomic::store(&_old_evac_expended, (size_t) 0);
}

inline size_t ShenandoahHeap::expend_old_evac(size_t increment) {
  return Atomic::add(&_old_evac_expended, increment);
}

inline size_t ShenandoahHeap::get_old_evac_expended() {
  return Atomic::load(&_old_evac_expended);
}

inline void ShenandoahHeap::reset_promoted_expended() {
  Atomic::store(&_promoted_expended, (size_t) 0);
}

inline size_t ShenandoahHeap::expend_promoted(size_t increment) {
  return Atomic::add(&_promoted_expended, increment);
}

inline size_t ShenandoahHeap::unexpend_promoted(size_t decrement) {
  return Atomic::sub(&_promoted_expended, decrement);
}

inline size_t ShenandoahHeap::get_promoted_expended() {
  return Atomic::load(&_promoted_expended);
}

inline size_t ShenandoahHeap::set_young_evac_reserve(size_t new_val) {
  size_t orig = _young_evac_reserve;
  _young_evac_reserve = new_val;
  return orig;
}

inline size_t ShenandoahHeap::get_young_evac_reserve() const {
  return _young_evac_reserve;
}

inline intptr_t ShenandoahHeap::set_alloc_supplement_reserve(intptr_t new_val) {
  intptr_t orig = _alloc_supplement_reserve;
  _alloc_supplement_reserve = new_val;
  return orig;
}

inline intptr_t ShenandoahHeap::get_alloc_supplement_reserve() const {
  return _alloc_supplement_reserve;
}

template<class T>
inline void ShenandoahHeap::marked_object_iterate(ShenandoahHeapRegion* region, T* cl) {
  marked_object_iterate(region, cl, region->top());
}

template<class T>
inline void ShenandoahHeap::marked_object_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* limit) {
  assert(! region->is_humongous_continuation(), "no humongous continuation regions here");

  ShenandoahMarkingContext* const ctx = marking_context();

  HeapWord* tams = ctx->top_at_mark_start(region);

  size_t skip_bitmap_delta = 1;
  HeapWord* start = region->bottom();
  HeapWord* end = MIN2(tams, region->end());

  // Step 1. Scan below the TAMS based on bitmap data.
  HeapWord* limit_bitmap = MIN2(limit, tams);

  // Try to scan the initial candidate. If the candidate is above the TAMS, it would
  // fail the subsequent "< limit_bitmap" checks, and fall through to Step 2.
  HeapWord* cb = ctx->get_next_marked_addr(start, end);

  intx dist = ShenandoahMarkScanPrefetch;
  if (dist > 0) {
    // Batched scan that prefetches the oop data, anticipating the access to
    // either header, oop field, or forwarding pointer. Not that we cannot
    // touch anything in oop, while it still being prefetched to get enough
    // time for prefetch to work. This is why we try to scan the bitmap linearly,
    // disregarding the object size. However, since we know forwarding pointer
    // precedes the object, we can skip over it. Once we cannot trust the bitmap,
    // there is no point for prefetching the oop contents, as oop->size() will
    // touch it prematurely.

    // No variable-length arrays in standard C++, have enough slots to fit
    // the prefetch distance.
    static const int SLOT_COUNT = 256;
    guarantee(dist <= SLOT_COUNT, "adjust slot count");
    HeapWord* slots[SLOT_COUNT];

    int avail;
    do {
      avail = 0;
      for (int c = 0; (c < dist) && (cb < limit_bitmap); c++) {
        Prefetch::read(cb, oopDesc::mark_offset_in_bytes());
        slots[avail++] = cb;
        cb += skip_bitmap_delta;
        if (cb < limit_bitmap) {
          cb = ctx->get_next_marked_addr(cb, limit_bitmap);
        }
      }

      for (int c = 0; c < avail; c++) {
        assert (slots[c] < tams,  "only objects below TAMS here: "  PTR_FORMAT " (" PTR_FORMAT ")", p2i(slots[c]), p2i(tams));
        assert (slots[c] < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(slots[c]), p2i(limit));
        oop obj = cast_to_oop(slots[c]);
        assert(oopDesc::is_oop(obj), "sanity");
        assert(ctx->is_marked(obj), "object expected to be marked");
        cl->do_object(obj);
      }
    } while (avail > 0);
  } else {
    while (cb < limit_bitmap) {
      assert (cb < tams,  "only objects below TAMS here: "  PTR_FORMAT " (" PTR_FORMAT ")", p2i(cb), p2i(tams));
      assert (cb < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(cb), p2i(limit));
      oop obj = cast_to_oop(cb);
      assert(oopDesc::is_oop(obj), "sanity");
      assert(ctx->is_marked(obj), "object expected to be marked");
      cl->do_object(obj);
      cb += skip_bitmap_delta;
      if (cb < limit_bitmap) {
        cb = ctx->get_next_marked_addr(cb, limit_bitmap);
      }
    }
  }

  // Step 2. Accurate size-based traversal, happens past the TAMS.
  // This restarts the scan at TAMS, which makes sure we traverse all objects,
  // regardless of what happened at Step 1.
  HeapWord* cs = tams;
  while (cs < limit) {
    assert (cs >= tams, "only objects past TAMS here: "   PTR_FORMAT " (" PTR_FORMAT ")", p2i(cs), p2i(tams));
    assert (cs < limit, "only objects below limit here: " PTR_FORMAT " (" PTR_FORMAT ")", p2i(cs), p2i(limit));
    oop obj = cast_to_oop(cs);
    assert(oopDesc::is_oop(obj), "sanity");
    assert(ctx->is_marked(obj), "object expected to be marked");
    size_t size = obj->size();
    cl->do_object(obj);
    cs += size;
  }
}

template <class T>
class ShenandoahObjectToOopClosure : public ObjectClosure {
  T* _cl;
public:
  ShenandoahObjectToOopClosure(T* cl) : _cl(cl) {}

  void do_object(oop obj) {
    obj->oop_iterate(_cl);
  }
};

template <class T>
class ShenandoahObjectToOopBoundedClosure : public ObjectClosure {
  T* _cl;
  MemRegion _bounds;
public:
  ShenandoahObjectToOopBoundedClosure(T* cl, HeapWord* bottom, HeapWord* top) :
    _cl(cl), _bounds(bottom, top) {}

  void do_object(oop obj) {
    obj->oop_iterate(_cl, _bounds);
  }
};

template<class T>
inline void ShenandoahHeap::marked_object_oop_iterate(ShenandoahHeapRegion* region, T* cl, HeapWord* top) {
  if (region->is_humongous()) {
    HeapWord* bottom = region->bottom();
    if (top > bottom) {
      region = region->humongous_start_region();
      ShenandoahObjectToOopBoundedClosure<T> objs(cl, bottom, top);
      marked_object_iterate(region, &objs);
    }
  } else {
    ShenandoahObjectToOopClosure<T> objs(cl);
    marked_object_iterate(region, &objs, top);
  }
}

inline ShenandoahHeapRegion* const ShenandoahHeap::get_region(size_t region_idx) const {
  if (region_idx < _num_regions) {
    return _regions[region_idx];
  } else {
    return NULL;
  }
}

inline ShenandoahMarkingContext* ShenandoahHeap::complete_marking_context() const {
  assert (_marking_context->is_complete()," sanity");
  return _marking_context;
}

inline ShenandoahMarkingContext* ShenandoahHeap::marking_context() const {
  return _marking_context;
}

inline void ShenandoahHeap::clear_cards_for(ShenandoahHeapRegion* region) {
  if (mode()->is_generational()) {
    _card_scan->mark_range_as_empty(region->bottom(), pointer_delta(region->end(), region->bottom()));
  }
}

inline void ShenandoahHeap::dirty_cards(HeapWord* start, HeapWord* end) {
  assert(mode()->is_generational(), "Should only be used for generational mode");
  size_t words = pointer_delta(end, start);
  _card_scan->mark_range_as_dirty(start, words);
}

inline void ShenandoahHeap::clear_cards(HeapWord* start, HeapWord* end) {
  assert(mode()->is_generational(), "Should only be used for generational mode");
  size_t words = pointer_delta(end, start);
  _card_scan->mark_range_as_clean(start, words);
}

inline void ShenandoahHeap::mark_card_as_dirty(void* location) {
  if (mode()->is_generational()) {
    _card_scan->mark_card_as_dirty((HeapWord*)location);
  }
}

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHHEAP_INLINE_HPP
