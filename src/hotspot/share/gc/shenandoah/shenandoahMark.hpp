/*
 * Copyright (c) 2021, 2022, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHMARK_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHMARK_HPP

#include "gc/shared/stringdedup/stringDedup.hpp"
#include "gc/shared/taskTerminator.hpp"
#include "gc/shenandoah/shenandoahOopClosures.hpp"
#include "gc/shenandoah/shenandoahTaskqueue.hpp"

// Base class for mark
// Mark class does not maintain states. Instead, mark states are
// maintained by task queues, mark bitmap and SATB buffers (concurrent mark)
class ShenandoahMark: public StackObj {

protected:
  ShenandoahGeneration* const _generation;
  ShenandoahObjToScanQueueSet* const _task_queues;
  ShenandoahObjToScanQueueSet* const _old_gen_task_queues;

protected:
  ShenandoahMark(ShenandoahGeneration* generation);

public:
  template<class T, GenerationMode GENERATION>
  static inline void mark_through_ref(T* p, ShenandoahObjToScanQueue* q, ShenandoahObjToScanQueue* old, ShenandoahMarkingContext* const mark_context, bool weak);

  // Loom support
  void start_mark();
  void end_mark();

  // Helpers
  inline ShenandoahObjToScanQueueSet* task_queues() const;
  ShenandoahObjToScanQueueSet* old_task_queues() {
    return _old_gen_task_queues;
  }

  inline ShenandoahObjToScanQueue* get_queue(uint index) const;
  inline ShenandoahObjToScanQueue* get_old_queue(uint index) const;

  inline ShenandoahGeneration* generation() { return _generation; };

// ---------- Marking loop and tasks
private:
  template <class T, StringDedupMode STRING_DEDUP>
  inline void do_task(ShenandoahObjToScanQueue* q, T* cl, ShenandoahLiveData* live_data, StringDedup::Requests* const req, ShenandoahMarkTask* task);

  template <class T>
  inline void do_chunked_array_start(ShenandoahObjToScanQueue* q, T* cl, oop array, bool weak);

  template <class T>
  inline void do_chunked_array(ShenandoahObjToScanQueue* q, T* cl, oop array, int chunk, int pow, bool weak);

  inline void count_liveness(ShenandoahLiveData* live_data, oop obj);

  template <class T, GenerationMode GENERATION, bool CANCELLABLE, StringDedupMode STRING_DEDUP>
  void mark_loop_work(T* cl, ShenandoahLiveData* live_data, uint worker_id, TaskTerminator *t, StringDedup::Requests* const req);

  template <GenerationMode GENERATION, bool CANCELLABLE, StringDedupMode STRING_DEDUP>
  void mark_loop_prework(uint worker_id, TaskTerminator *terminator, ShenandoahReferenceProcessor *rp, StringDedup::Requests* const req, bool update_refs);

  template <GenerationMode GENERATION>
  static bool in_generation(oop obj);

  static void mark_ref(ShenandoahObjToScanQueue* q,
                       ShenandoahMarkingContext* const mark_context,
                       bool weak, oop obj);

  template <StringDedupMode STRING_DEDUP>
  inline void dedup_string(oop obj, StringDedup::Requests* const req);
protected:
  template<bool CANCELLABLE, StringDedupMode STRING_DEDUP>
  void mark_loop(GenerationMode generation, uint worker_id, TaskTerminator* terminator, ShenandoahReferenceProcessor *rp,
                 StringDedup::Requests* const req);

  void mark_loop(GenerationMode generation, uint worker_id, TaskTerminator* terminator, ShenandoahReferenceProcessor *rp,
                 bool cancellable, StringDedupMode dedup_mode, StringDedup::Requests* const req);
};

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHMARK_HPP

