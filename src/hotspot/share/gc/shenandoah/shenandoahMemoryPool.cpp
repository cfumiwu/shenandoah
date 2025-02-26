/*
 * Copyright (c) 2013, 2019, Red Hat, Inc. All rights reserved.
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
#include "gc/shenandoah/shenandoahMemoryPool.hpp"
#include "gc/shenandoah/shenandoahYoungGeneration.hpp"
#include "gc/shenandoah/shenandoahOldGeneration.hpp"

ShenandoahMemoryPool::ShenandoahMemoryPool(ShenandoahHeap* heap,
                                           const char* name) :
        CollectedMemoryPool(name,
                            heap->initial_capacity(),
                            heap->max_capacity(),
                            true /* support_usage_threshold */),
                            _heap(heap) {}

ShenandoahMemoryPool::ShenandoahMemoryPool(ShenandoahHeap* heap,
                                           const char* name,
                                           size_t initial_capacity,
                                           size_t max_capacity) :
        CollectedMemoryPool(name,
                            initial_capacity,
                            max_capacity,
                            true /* support_usage_threshold */),
                            _heap(heap) {}


MemoryUsage ShenandoahMemoryPool::get_memory_usage() {
  size_t initial   = initial_size();
  size_t max       = max_size();
  size_t used      = used_in_bytes();
  size_t committed = _heap->committed();

  // These asserts can never fail: max is stable, and all updates to other values never overflow max.
  assert(initial <= max,    "initial: "   SIZE_FORMAT ", max: "       SIZE_FORMAT, initial,   max);
  assert(used <= max,       "used: "      SIZE_FORMAT ", max: "       SIZE_FORMAT, used,      max);
  assert(committed <= max,  "committed: " SIZE_FORMAT ", max: "       SIZE_FORMAT, committed, max);

  // Committed and used are updated concurrently and independently. They can momentarily break
  // the assert below, which would also fail in downstream code. To avoid that, adjust values
  // to make sense under the race. See JDK-8207200.
  committed = MAX2(used, committed);
  assert(used <= committed, "used: "      SIZE_FORMAT ", committed: " SIZE_FORMAT, used,      committed);

  return MemoryUsage(initial, used, committed, max);
}

size_t ShenandoahMemoryPool::used_in_bytes() {
  return _heap->used();
}

size_t ShenandoahMemoryPool::max_size() const {
  return _heap->max_capacity();
}

ShenandoahYoungGenMemoryPool::ShenandoahYoungGenMemoryPool(ShenandoahHeap* heap) :
        ShenandoahMemoryPool(heap,
                             "Shenandoah Young Gen",
                             0,
                             heap->max_capacity()) { }

MemoryUsage ShenandoahYoungGenMemoryPool::get_memory_usage() {
  size_t initial   = initial_size();
  size_t max       = max_size();
  size_t used      = used_in_bytes();
  size_t committed = _heap->young_generation()->used_regions_size();

  return MemoryUsage(initial, used, committed, max);
}

size_t ShenandoahYoungGenMemoryPool::used_in_bytes() {
  return _heap->young_generation()->used();
}

size_t ShenandoahYoungGenMemoryPool::max_size() const {
  return _heap->young_generation()->max_capacity();
}

ShenandoahOldGenMemoryPool::ShenandoahOldGenMemoryPool(ShenandoahHeap* heap) :
        ShenandoahMemoryPool(heap,
                             "Shenandoah Old Gen",
                             0,
                             heap->max_capacity()) { }

MemoryUsage ShenandoahOldGenMemoryPool::get_memory_usage() {
  size_t initial   = initial_size();
  size_t max       = max_size();
  size_t used      = used_in_bytes();
  size_t committed = _heap->old_generation()->used_regions_size();

  return MemoryUsage(initial, used, committed, max);
}

size_t ShenandoahOldGenMemoryPool::used_in_bytes() {
  return _heap->old_generation()->used();
}

size_t ShenandoahOldGenMemoryPool::max_size() const {
  return _heap->old_generation()->max_capacity();
}
