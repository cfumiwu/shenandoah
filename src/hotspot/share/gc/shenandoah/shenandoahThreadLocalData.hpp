/*
 * Copyright (c) 2018, 2022, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHTHREADLOCALDATA_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHTHREADLOCALDATA_HPP

#include "gc/shared/plab.hpp"
#include "gc/shared/gcThreadLocalData.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.hpp"
#include "gc/shenandoah/shenandoahCodeRoots.hpp"
#include "gc/shenandoah/shenandoahSATBMarkQueueSet.hpp"
#include "runtime/javaThread.hpp"
#include "utilities/debug.hpp"
#include "utilities/sizes.hpp"

class ShenandoahThreadLocalData {
private:
  char _gc_state;
  // Evacuation OOM state
  uint8_t                 _oom_scope_nesting_level;
  bool                    _oom_during_evac;
  bool                    _plab_allows_promotion; // If false, no more promotion by this thread during this evacuation phase.
  SATBMarkQueue           _satb_mark_queue;

  // Thread-local allocation buffer for object evacuations.
  // In generational mode, it is exclusive to the young generation.
  PLAB* _gclab;
  size_t _gclab_size;
  double _paced_time;

  // Thread-local allocation buffer only used in generational mode.
  // Used both by mutator threads and by GC worker threads
  // for evacuations within the old generation and
  // for promotions from the young generation into the old generation.
  PLAB* _plab;
  size_t _plab_size;

  size_t _plab_evacuated;
  size_t _plab_promoted;
  size_t _plab_preallocated_promoted;
  bool   _plab_retries_enabled;

  ShenandoahThreadLocalData() :
    _gc_state(0),
    _oom_scope_nesting_level(0),
    _oom_during_evac(false),
    _satb_mark_queue(&ShenandoahBarrierSet::satb_mark_queue_set()),
    _gclab(NULL),
    _gclab_size(0),
    _paced_time(0),
    _plab(NULL),
    _plab_size(0),
    _plab_evacuated(0),
    _plab_promoted(0),
    _plab_preallocated_promoted(0),
    _plab_retries_enabled(true) {
  }

  ~ShenandoahThreadLocalData() {
    if (_gclab != NULL) {
      delete _gclab;
    }
    if (_plab != NULL) {
      ShenandoahHeap::heap()->retire_plab(_plab);
      delete _plab;
    }
  }

  static ShenandoahThreadLocalData* data(Thread* thread) {
    assert(UseShenandoahGC, "Sanity");
    return thread->gc_data<ShenandoahThreadLocalData>();
  }

  static ByteSize satb_mark_queue_offset() {
    return Thread::gc_data_offset() + byte_offset_of(ShenandoahThreadLocalData, _satb_mark_queue);
  }

public:
  static void create(Thread* thread) {
    new (data(thread)) ShenandoahThreadLocalData();
  }

  static void destroy(Thread* thread) {
    data(thread)->~ShenandoahThreadLocalData();
  }

  static SATBMarkQueue& satb_mark_queue(Thread* thread) {
    return data(thread)->_satb_mark_queue;
  }

  static void set_gc_state(Thread* thread, char gc_state) {
    data(thread)->_gc_state = gc_state;
  }

  static char gc_state(Thread* thread) {
    return data(thread)->_gc_state;
  }

  static void initialize_gclab(Thread* thread) {
    assert (thread->is_Java_thread() || thread->is_Worker_thread(), "Only Java and GC worker threads are allowed to get GCLABs");
    assert(data(thread)->_gclab == NULL, "Only initialize once");
    data(thread)->_gclab = new PLAB(PLAB::min_size());
    data(thread)->_gclab_size = 0;
    data(thread)->_plab = new PLAB(PLAB::min_size());
    data(thread)->_plab_size = 0;
  }

  static PLAB* gclab(Thread* thread) {
    return data(thread)->_gclab;
  }

  static size_t gclab_size(Thread* thread) {
    return data(thread)->_gclab_size;
  }

  static void set_gclab_size(Thread* thread, size_t v) {
    data(thread)->_gclab_size = v;
  }

  static PLAB* plab(Thread* thread) {
    return data(thread)->_plab;
  }

  static size_t plab_size(Thread* thread) {
    return data(thread)->_plab_size;
  }

  static void set_plab_size(Thread* thread, size_t v) {
    data(thread)->_plab_size = v;
  }

  static void enable_plab_retries(Thread* thread) {
    data(thread)->_plab_retries_enabled = true;
  }

  static void disable_plab_retries(Thread* thread) {
    data(thread)->_plab_retries_enabled = false;
  }

  static bool plab_retries_enabled(Thread* thread) {
    return data(thread)->_plab_retries_enabled;
  }

  static void enable_plab_promotions(Thread* thread) {
    data(thread)->_plab_allows_promotion = true;
  }

  static void disable_plab_promotions(Thread* thread) {
    data(thread)->_plab_allows_promotion = false;
  }

  static bool allow_plab_promotions(Thread* thread) {
    return data(thread)->_plab_allows_promotion;
  }

  static void reset_plab_evacuated(Thread* thread) {
    data(thread)->_plab_evacuated = 0;
  }

  static void add_to_plab_evacuated(Thread* thread, size_t increment) {
    data(thread)->_plab_evacuated += increment;
  }

  static void subtract_from_plab_evacuated(Thread* thread, size_t increment) {
    data(thread)->_plab_evacuated -= increment;
  }

  static size_t get_plab_evacuated(Thread* thread) {
    return data(thread)->_plab_evacuated;
  }

  static void reset_plab_promoted(Thread* thread) {
    data(thread)->_plab_promoted = 0;
  }

  static void add_to_plab_promoted(Thread* thread, size_t increment) {
    data(thread)->_plab_promoted += increment;
  }

  static void subtract_from_plab_promoted(Thread* thread, size_t increment) {
    data(thread)->_plab_promoted -= increment;
  }

  static size_t get_plab_promoted(Thread* thread) {
    return data(thread)->_plab_promoted;
  }

  static void set_plab_preallocated_promoted(Thread* thread, size_t value) {
    data(thread)->_plab_preallocated_promoted = value;
  }

  static size_t get_plab_preallocated_promoted(Thread* thread) {
    return data(thread)->_plab_preallocated_promoted;
  }

  static void add_paced_time(Thread* thread, double v) {
    data(thread)->_paced_time += v;
  }

  static double paced_time(Thread* thread) {
    return data(thread)->_paced_time;
  }

  static void reset_paced_time(Thread* thread) {
    data(thread)->_paced_time = 0;
  }

  // Evacuation OOM handling
  static bool is_oom_during_evac(Thread* thread) {
    return data(thread)->_oom_during_evac;
  }

  static void set_oom_during_evac(Thread* thread, bool oom) {
    data(thread)->_oom_during_evac = oom;
  }

  static uint8_t evac_oom_scope_level(Thread* thread) {
    return data(thread)->_oom_scope_nesting_level;
  }

  // Push the scope one level deeper, return previous level
  static uint8_t push_evac_oom_scope(Thread* thread) {
    uint8_t level = evac_oom_scope_level(thread);
    assert(level < 254, "Overflow nesting level"); // UINT8_MAX = 255
    data(thread)->_oom_scope_nesting_level = level + 1;
    return level;
  }

  // Pop the scope by one level, return previous level
  static uint8_t pop_evac_oom_scope(Thread* thread) {
    uint8_t level = evac_oom_scope_level(thread);
    assert(level > 0, "Underflow nesting level");
    data(thread)->_oom_scope_nesting_level = level - 1;
    return level;
  }

  static bool is_evac_allowed(Thread* thread) {
    return evac_oom_scope_level(thread) > 0;
  }

  // Offsets
  static ByteSize satb_mark_queue_active_offset() {
    return satb_mark_queue_offset() + SATBMarkQueue::byte_offset_of_active();
  }

  static ByteSize satb_mark_queue_index_offset() {
    return satb_mark_queue_offset() + SATBMarkQueue::byte_offset_of_index();
  }

  static ByteSize satb_mark_queue_buffer_offset() {
    return satb_mark_queue_offset() + SATBMarkQueue::byte_offset_of_buf();
  }

  static ByteSize gc_state_offset() {
    return Thread::gc_data_offset() + byte_offset_of(ShenandoahThreadLocalData, _gc_state);
  }
};

STATIC_ASSERT(sizeof(ShenandoahThreadLocalData) <= sizeof(GCThreadLocalData));

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHTHREADLOCALDATA_HPP
