/*
 * Copyright (c) 2021, Amazon.com, Inc. or its affiliates. All rights reserved.
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


#include "precompiled.hpp"

#include "gc/shared/strongRootsScope.hpp"
#include "gc/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc/shenandoah/heuristics/shenandoahAdaptiveHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahAggressiveHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahCompactHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahOldHeuristics.hpp"
#include "gc/shenandoah/heuristics/shenandoahStaticHeuristics.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahFreeSet.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahMarkClosures.hpp"
#include "gc/shenandoah/shenandoahMark.inline.hpp"
#include "gc/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"
#include "gc/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc/shenandoah/shenandoahReferenceProcessor.hpp"
#include "gc/shenandoah/shenandoahStringDedup.hpp"
#include "gc/shenandoah/shenandoahUtils.hpp"
#include "gc/shenandoah/shenandoahWorkerPolicy.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "prims/jvmtiTagMap.hpp"
#include "runtime/threads.hpp"
#include "utilities/events.hpp"

class ShenandoahFlushAllSATB : public ThreadClosure {
 private:
  SATBMarkQueueSet& _satb_qset;
  uintx _claim_token;

 public:
  explicit ShenandoahFlushAllSATB(SATBMarkQueueSet& satb_qset) :
    _satb_qset(satb_qset),
    _claim_token(Threads::thread_claim_token()) { }

  void do_thread(Thread* thread) {
    if (thread->claim_threads_do(true, _claim_token)) {
      // Transfer any partial buffer to the qset for completed buffer processing.
      _satb_qset.flush_queue(ShenandoahThreadLocalData::satb_mark_queue(thread));
    }
  }
};

class ShenandoahProcessOldSATB : public SATBBufferClosure {
 private:
  ShenandoahObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
  ShenandoahMarkingContext* const _mark_context;

 public:
  size_t _trashed_oops;

  explicit ShenandoahProcessOldSATB(ShenandoahObjToScanQueue* q) :
    _queue(q),
    _heap(ShenandoahHeap::heap()),
    _mark_context(_heap->marking_context()),
    _trashed_oops(0) {}

  void do_buffer(void **buffer, size_t size) {
    assert(size == 0 || !_heap->has_forwarded_objects() || _heap->is_concurrent_old_mark_in_progress(), "Forwarded objects are not expected here");
    for (size_t i = 0; i < size; ++i) {
      oop *p = (oop *) &buffer[i];
      ShenandoahHeapRegion* region = _heap->heap_region_containing(*p);
      if (region->is_old() && region->is_active()) {
          ShenandoahMark::mark_through_ref<oop, OLD>(p, _queue, NULL, _mark_context, false);
      } else {
        ++_trashed_oops;
      }
    }
  }
};

class ShenandoahPurgeSATBTask : public WorkerTask {
private:
  ShenandoahObjToScanQueueSet* _mark_queues;

public:
  volatile size_t _trashed_oops;

  explicit ShenandoahPurgeSATBTask(ShenandoahObjToScanQueueSet* queues) :
    WorkerTask("Purge SATB"),
    _mark_queues(queues),
    _trashed_oops(0) {
    Threads::change_thread_claim_token();
  }

  ~ShenandoahPurgeSATBTask() {
    if (_trashed_oops > 0) {
      log_info(gc)("Purged " SIZE_FORMAT " oops from old generation SATB buffers.", _trashed_oops);
    }
  }

  void work(uint worker_id) {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahSATBMarkQueueSet &satb_queues = ShenandoahBarrierSet::satb_mark_queue_set();
    ShenandoahFlushAllSATB flusher(satb_queues);
    Threads::threads_do(&flusher);

    ShenandoahObjToScanQueue* mark_queue = _mark_queues->queue(worker_id);
    ShenandoahProcessOldSATB processor(mark_queue);
    while (satb_queues.apply_closure_to_completed_buffer(&processor)) {}

    Atomic::add(&_trashed_oops, processor._trashed_oops);
  }
};

class ShenandoahConcurrentCoalesceAndFillTask : public WorkerTask {
 private:
  uint _nworkers;
  ShenandoahHeapRegion** _coalesce_and_fill_region_array;
  uint _coalesce_and_fill_region_count;
  volatile bool _is_preempted;

 public:
  ShenandoahConcurrentCoalesceAndFillTask(uint nworkers, ShenandoahHeapRegion** coalesce_and_fill_region_array,
                                          uint region_count) :
    WorkerTask("Shenandoah Concurrent Coalesce and Fill"),
    _nworkers(nworkers),
    _coalesce_and_fill_region_array(coalesce_and_fill_region_array),
    _coalesce_and_fill_region_count(region_count),
    _is_preempted(false) {
  }

  void work(uint worker_id) {
    for (uint region_idx = worker_id; region_idx < _coalesce_and_fill_region_count; region_idx += _nworkers) {
      ShenandoahHeapRegion* r = _coalesce_and_fill_region_array[region_idx];
      if (r->is_humongous()) {
        // there's only one object in this region and it's not garbage, so no need to coalesce or fill
        continue;
      }

      if (!r->oop_fill_and_coalesce()) {
        // Coalesce and fill has been preempted
        Atomic::store(&_is_preempted, true);
        return;
      }
    }
  }

  // Value returned from is_completed() is only valid after all worker thread have terminated.
  bool is_completed() {
    return !Atomic::load(&_is_preempted);
  }
};

ShenandoahOldGeneration::ShenandoahOldGeneration(uint max_queues, size_t max_capacity, size_t soft_max_capacity)
  : ShenandoahGeneration(OLD, max_queues, max_capacity, soft_max_capacity),
    _coalesce_and_fill_region_array(NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, ShenandoahHeap::heap()->num_regions(), mtGC)),
    _state(IDLE)
{
  // Always clear references for old generation
  ref_processor()->set_soft_reference_policy(true);
}

const char* ShenandoahOldGeneration::name() const {
  return "OLD";
}

bool ShenandoahOldGeneration::contains(ShenandoahHeapRegion* region) const {
  return region->affiliation() != YOUNG_GENERATION;
}

void ShenandoahOldGeneration::parallel_heap_region_iterate(ShenandoahHeapRegionClosure* cl) {
  ShenandoahGenerationRegionClosure<OLD> old_regions(cl);
  ShenandoahHeap::heap()->parallel_heap_region_iterate(&old_regions);
}

void ShenandoahOldGeneration::heap_region_iterate(ShenandoahHeapRegionClosure* cl) {
  ShenandoahGenerationRegionClosure<OLD> old_regions(cl);
  ShenandoahHeap::heap()->heap_region_iterate(&old_regions);
}

void ShenandoahOldGeneration::set_concurrent_mark_in_progress(bool in_progress) {
  ShenandoahHeap::heap()->set_concurrent_old_mark_in_progress(in_progress);
}

bool ShenandoahOldGeneration::is_concurrent_mark_in_progress() {
  return ShenandoahHeap::heap()->is_concurrent_old_mark_in_progress();
}

void ShenandoahOldGeneration::cancel_marking() {
  if (is_concurrent_mark_in_progress()) {
    log_info(gc)("Abandon satb buffers.");
    ShenandoahBarrierSet::satb_mark_queue_set().abandon_partial_marking();
  }

  ShenandoahGeneration::cancel_marking();
}

void ShenandoahOldGeneration::prepare_gc() {

  // Make the old generation regions parseable, so they can be safely
  // scanned when looking for objects in memory indicated by dirty cards.
  entry_coalesce_and_fill();

  // Now that we have made the old generation parseable, it is safe to reset the mark bitmap.
  {
    static const char* msg = "Concurrent reset (OLD)";
    ShenandoahConcurrentPhase gc_phase(msg, ShenandoahPhaseTimings::conc_reset_old);
    ShenandoahWorkerScope scope(ShenandoahHeap::heap()->workers(),
                                ShenandoahWorkerPolicy::calc_workers_for_conc_reset(),
                                msg);
    ShenandoahGeneration::prepare_gc();
  }
}

bool ShenandoahOldGeneration::entry_coalesce_and_fill() {
  char msg[1024];
  ShenandoahHeap* const heap = ShenandoahHeap::heap();

  ShenandoahConcurrentPhase gc_phase("Coalescing and filling (OLD)", ShenandoahPhaseTimings::coalesce_and_fill);

  // TODO: I don't think we're using these concurrent collection counters correctly.
  TraceCollectorStats tcs(heap->monitoring_support()->concurrent_collection_counters());
  EventMark em("%s", msg);
  ShenandoahWorkerScope scope(heap->workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_marking(),
                              "concurrent coalesce and fill");

  return coalesce_and_fill();
}

bool ShenandoahOldGeneration::coalesce_and_fill() {
  ShenandoahHeap* const heap = ShenandoahHeap::heap();
  heap->set_prepare_for_old_mark_in_progress(true);
  transition_to(FILLING);

  ShenandoahOldHeuristics* old_heuristics = heap->old_heuristics();
  WorkerThreads* workers = heap->workers();
  uint nworkers = workers->active_workers();

  log_debug(gc)("Starting (or resuming) coalesce-and-fill of old heap regions");
  uint coalesce_and_fill_regions_count = old_heuristics->get_coalesce_and_fill_candidates(_coalesce_and_fill_region_array);
  assert(coalesce_and_fill_regions_count <= heap->num_regions(), "Sanity");
  ShenandoahConcurrentCoalesceAndFillTask task(nworkers, _coalesce_and_fill_region_array, coalesce_and_fill_regions_count);

  workers->run_task(&task);
  if (task.is_completed()) {
    // Remember that we're done with coalesce-and-fill.
    heap->set_prepare_for_old_mark_in_progress(false);
    transition_to(BOOTSTRAPPING);
    return true;
  } else {
    log_debug(gc)("Suspending coalesce-and-fill of old heap regions");
    // Otherwise, we got preempted before the work was done.
    return false;
  }
}

void ShenandoahOldGeneration::transfer_pointers_from_satb() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  shenandoah_assert_safepoint();
  assert(heap->is_concurrent_old_mark_in_progress(), "Only necessary during old marking.");
  log_info(gc)("Transfer satb buffers.");
  uint nworkers = heap->workers()->active_workers();
  StrongRootsScope scope(nworkers);

  ShenandoahPurgeSATBTask purge_satb_task(task_queues());
  heap->workers()->run_task(&purge_satb_task);
}

bool ShenandoahOldGeneration::contains(oop obj) const {
  return ShenandoahHeap::heap()->is_in_old(obj);
}

void ShenandoahOldGeneration::prepare_regions_and_collection_set(bool concurrent) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  assert(!heap->is_full_gc_in_progress(), "Only for concurrent and degenerated GC");

  {
    ShenandoahGCPhase phase(concurrent ? ShenandoahPhaseTimings::final_update_region_states : ShenandoahPhaseTimings::degen_gc_final_update_region_states);
    ShenandoahFinalMarkUpdateRegionStateClosure cl(complete_marking_context());

    parallel_heap_region_iterate(&cl);
    heap->assert_pinned_region_status();
  }

  {
    // This doesn't actually choose a collection set, but prepares a list of
    // regions as 'candidates' for inclusion in a mixed collection.
    ShenandoahGCPhase phase(concurrent ? ShenandoahPhaseTimings::choose_cset : ShenandoahPhaseTimings::degen_gc_choose_cset);
    ShenandoahHeapLocker locker(heap->lock());
    heuristics()->choose_collection_set(nullptr, nullptr);
  }

  {
    // Though we did not choose a collection set above, we still may have
    // freed up immediate garbage regions so proceed with rebuilding the free set.
    ShenandoahGCPhase phase(concurrent ? ShenandoahPhaseTimings::final_rebuild_freeset : ShenandoahPhaseTimings::degen_gc_final_rebuild_freeset);
    ShenandoahHeapLocker locker(heap->lock());
    heap->free_set()->rebuild();
  }
}

const char* ShenandoahOldGeneration::state_name(State state) {
  switch (state) {
    case IDLE:          return "Idle";
    case FILLING:       return "Coalescing";
    case BOOTSTRAPPING: return "Bootstrapping";
    case MARKING:       return "Marking";
    case WAITING:       return "Waiting";
    default:
      ShouldNotReachHere();
      return "Unknown";
  }
}

void ShenandoahOldGeneration::transition_to(State new_state) {
  if (_state != new_state) {
    log_info(gc)("Old generation transition from %s to %s", state_name(_state), state_name(new_state));
    assert(validate_transition(new_state), "Invalid state transition.");
    _state = new_state;
  }
}

#ifdef ASSERT
// This diagram depicts the expected state transitions for marking the old generation
// and preparing for old collections. When a young generation cycle executes, the
// remembered set scan must visit objects in old regions. Visiting an object which
// has become dead on previous old cycles will result in crashes. To avoid visiting
// such objects, the remembered set scan will use the old generation mark bitmap when
// possible. It is _not_ possible to use the old generation bitmap when old marking
// is active (bitmap is not complete). For this reason, the old regions are made
// parseable _before_ the old generation bitmap is reset. The diagram does not depict
// global and full collections, both of which cancel any old generation activity.
//
//                              +-----------------+
//               +------------> |      IDLE       |
//               |   +--------> |                 |
//               |   |          +-----------------+
//               |   |            |
//               |   |            | Begin Old Mark
//               |   |            v
//               |   |          +-----------------+     +--------------------+
//               |   |          |     FILLING     | <-> |      YOUNG GC      |
//               |   |          |                 |     | (RSet Uses Bitmap) |
//               |   |          +-----------------+     +--------------------+
//               |   |            |
//               |   |            | Reset Bitmap
//               |   |            v
//               |   |          +-----------------+
//               |   |          |    BOOTSTRAP    |
//               |   |          |                 |
//               |   |          +-----------------+
//               |   |            |
//               |   |            | Continue Marking
//               |   |            v
//               |   |          +-----------------+     +----------------------+
//               |   |          |    MARKING      | <-> |       YOUNG GC       |
//               |   +----------|                 |     | (RSet Parses Region) |
//               |              +-----------------+     +----------------------+
//               |                |
//               |                | Has Candidates
//               |                v
//               |              +-----------------+
//               |              |     WAITING     |
//               +------------- |                 |
//                              +-----------------+
//
bool ShenandoahOldGeneration::validate_transition(State new_state) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  switch (new_state) {
    case IDLE:
      assert(!heap->is_concurrent_old_mark_in_progress(), "Cannot become idle during old mark.");
      assert(_old_heuristics->unprocessed_old_collection_candidates() == 0, "Cannot become idle with collection candidates");
      assert(!heap->is_prepare_for_old_mark_in_progress(), "Cannot become idle while making old generation parseable.");
      assert(heap->young_generation()->old_gen_task_queues() == nullptr, "Cannot become idle when setup for bootstrapping.");
      return true;
    case FILLING:
      assert(_state == IDLE, "Cannot begin filling without first being idle.");
      assert(heap->is_prepare_for_old_mark_in_progress(), "Should be preparing for old mark now.");
      return true;
    case BOOTSTRAPPING:
      assert(_state == FILLING, "Cannot reset bitmap without making old regions parseable.");
      // assert(heap->young_generation()->old_gen_task_queues() != nullptr, "Cannot bootstrap without old mark queues.");
      assert(!heap->is_prepare_for_old_mark_in_progress(), "Cannot still be making old regions parseable.");
      return true;
    case MARKING:
      assert(_state == BOOTSTRAPPING, "Must have finished bootstrapping before marking.");
      assert(heap->young_generation()->old_gen_task_queues() != nullptr, "Young generation needs old mark queues.");
      assert(heap->is_concurrent_old_mark_in_progress(), "Should be marking old now.");
      return true;
    case WAITING:
      assert(_state == MARKING, "Cannot have old collection candidates without first marking.");
      assert(_old_heuristics->unprocessed_old_collection_candidates() > 0, "Must have collection candidates here.");
      return true;
    default:
      ShouldNotReachHere();
      return false;
  }
}
#endif

ShenandoahHeuristics* ShenandoahOldGeneration::initialize_heuristics(ShenandoahMode* gc_mode) {
  assert(ShenandoahOldGCHeuristics != NULL, "ShenandoahOldGCHeuristics should not equal NULL");
  ShenandoahHeuristics* trigger;
  if (strcmp(ShenandoahOldGCHeuristics, "static") == 0) {
    trigger = new ShenandoahStaticHeuristics(this);
  } else if (strcmp(ShenandoahOldGCHeuristics, "adaptive") == 0) {
    trigger = new ShenandoahAdaptiveHeuristics(this);
  } else if (strcmp(ShenandoahOldGCHeuristics, "compact") == 0) {
    trigger = new ShenandoahCompactHeuristics(this);
  } else {
    vm_exit_during_initialization("Unknown -XX:ShenandoahOldGCHeuristics option (must be one of: static, adaptive, compact)");
    ShouldNotReachHere();
    return NULL;
  }
  trigger->set_guaranteed_gc_interval(ShenandoahGuaranteedOldGCInterval);
  _old_heuristics = new ShenandoahOldHeuristics(this, trigger);
  _heuristics = _old_heuristics;
  return _heuristics;
}

void ShenandoahOldGeneration::record_success_concurrent(bool abbreviated) {
  heuristics()->record_success_concurrent(abbreviated);
  ShenandoahHeap::heap()->shenandoah_policy()->record_success_old();
}
