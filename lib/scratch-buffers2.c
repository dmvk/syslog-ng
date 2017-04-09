/*
 * Copyright (c) 2017 Balabit
 * Copyright (c) 2017 Balazs Scheidler <balazs.scheidler@balabit.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */
#include "scratch-buffers2.h"
#include "tls-support.h"
#include "stats/stats-registry.h"
#include "timeutils.h"

/*
 * scratch_buffers
 *
 * The design of this API is to allow call-sites to use GString based string
 * buffers easily, without incurring the allocation overhead at all such
 * invocations and the need to always explicitly free them.
 *
 * Use-cases:
 *   - something needs a set of GString buffers, such that their number is
 *     not determinable at init time, so no preallocation is possible.
 *     Allocating/freeing these buffers on-demand generates a non-trivial
 *     overhead.
 *
 *   - We need a GString buffer that is needed for the duration of the
 *     processing of the given message, for example CSVScanner/KVScanner's
 *     buffers.  Right now, these are locked data structures or GString's
 *     are allocated on demand, both of which slows them down.  By using
 *     this API, these could reuse existing allocations and only free them
 *     once processing is finished of the given message.
 *
 * Design principles:
 *   - you can allocate a GString instance using scratch_buffers2_alloc()
 *
 *   - these are thread specific allocations and should not be leaked
 *     through thread boundaries, not until your own next invocation
 *
 *   - you need to assume that once you return from your functions, the
 *     previous allocations are gone!
 *
 *   - you don't need to free these allocations, as they get released
 *     automatically at the end of the message processing by the worker
 *     thread machinery
 *
 *   - if you produce too much garbage, you can still explicitly free these
 *     buffers in bulk using the mark/reclaim functions.
 *
 *   - reclaim works by freeing _all_ allocations up to a mark, so even
 *     those that are allocated by the functions you called.  Please make
 *     sure that those do not hold references after returning.
 *
 * Other notes:
 *   - this is not a complete garbage collector, but a very simple allocator for
 *     buffers that creates coupling between various/independent parts of
 *     the codebase.
 *
 *   - ONLY USE IT IF YOU UNDERSTAND THE IMPLICATIONS AND PROVIDES A
 *     MEASURABLE PERFORMANCE IMPACT.
 */

TLS_BLOCK_START
{
  GPtrArray *scratch_buffers;
  gint scratch_buffers_used;
  glong scratch_buffers_bytes_reported;
  time_t scratch_buffers_time_of_last_maintenance;
}
TLS_BLOCK_END;

static StatsCounterItem *stats_scratch_buffers_count;
static StatsCounterItem *stats_scratch_buffers_bytes;

#define scratch_buffers       __tls_deref(scratch_buffers)
#define scratch_buffers_used  __tls_deref(scratch_buffers_used)
#define scratch_buffers_bytes_reported  __tls_deref(scratch_buffers_bytes_reported)
#define scratch_buffers_time_of_last_maintenance  __tls_deref(scratch_buffers_time_of_last_maintenance)

/* update allocation counters once every period, in seconds */
#define SCRATCH_BUFFERS_MAINTENANCE_PERIOD 5

void
scratch_buffers2_mark(ScratchBuffersMarker *marker)
{
  *marker = scratch_buffers_used;
}

GString *
scratch_buffers2_alloc_and_mark(ScratchBuffersMarker *marker)
{
  if (marker)
    scratch_buffers2_mark(marker);
  if (scratch_buffers_used >= scratch_buffers->len)
    {
      g_ptr_array_add(scratch_buffers, g_string_sized_new(255));
      stats_counter_inc(stats_scratch_buffers_count);
    }

  GString *buffer = g_ptr_array_index(scratch_buffers, scratch_buffers_used);
  g_string_truncate(buffer, 0);
  scratch_buffers_used++;
  return buffer;
}

GString *
scratch_buffers2_alloc(void)
{
  return scratch_buffers2_alloc_and_mark(NULL);
}

void
scratch_buffers2_reclaim_allocations(void)
{
  scratch_buffers2_reclaim_marked(0);
}

void
scratch_buffers2_reclaim_marked(ScratchBuffersMarker marker)
{
  scratch_buffers_used = marker;
}

/* get a snapshot of the global allocation counter, can be racy */
gint
scratch_buffers2_get_global_allocation_count(void)
{
  return stats_counter_get(stats_scratch_buffers_count);
}

/* get the number of thread-local allocations does not race */
gint
scratch_buffers2_get_local_allocation_count(void)
{
  return scratch_buffers->len;
}

glong
scratch_buffers2_get_local_allocation_bytes(void)
{
  glong bytes = 0;

  for (gint i = 0; i < scratch_buffers->len; i++)
    {
      GString *str = g_ptr_array_index(scratch_buffers, i);
      bytes += str->allocated_len;
    }
  return bytes;
}

gint
scratch_buffers2_get_local_usage_count(void)
{
  return scratch_buffers_used;
}

static gboolean
_thread_maintenance_period_elapsed(void)
{
  if (!scratch_buffers_time_of_last_maintenance)
    return TRUE;

  if (scratch_buffers_time_of_last_maintenance - cached_g_current_time_sec() >= SCRATCH_BUFFERS_MAINTENANCE_PERIOD)
    return TRUE;
  return FALSE;
}

static void
_thread_maintenance_update_time(void)
{
  scratch_buffers_time_of_last_maintenance = cached_g_current_time_sec();
}

void
scratch_buffers2_thread_maintenance(void)
{
  glong prev_reported = scratch_buffers_bytes_reported;
  scratch_buffers_bytes_reported = scratch_buffers2_get_local_allocation_bytes();
  stats_counter_add(stats_scratch_buffers_bytes, -prev_reported + scratch_buffers_bytes_reported);
}

void
scratch_buffers2_thread_periodic_maintenance(void)
{
  if (_thread_maintenance_period_elapsed())
    {
      scratch_buffers2_thread_maintenance();
      _thread_maintenance_update_time();
    }
}

void
scratch_buffers2_thread_init(void)
{
  scratch_buffers = g_ptr_array_sized_new(256);
}

void
scratch_buffers2_thread_deinit(void)
{
  /* remove our values from stats */
  stats_counter_add(stats_scratch_buffers_count, -scratch_buffers->len);
  stats_counter_add(stats_scratch_buffers_bytes, -scratch_buffers_bytes_reported);

  /* free thread local scratch buffers */
  for (int i = 0; i < scratch_buffers->len; i++)
    {
      GString *buffer = g_ptr_array_index(scratch_buffers, i);
      g_string_free(buffer, TRUE);
    }
  g_ptr_array_free(scratch_buffers, TRUE);
}

void
scratch_buffers2_global_init(void)
{
  stats_lock();
  stats_register_counter(0, SCS_GLOBAL, "scratch_buffers_count", NULL, SC_TYPE_STORED, &stats_scratch_buffers_count);
  stats_register_counter(0, SCS_GLOBAL, "scratch_buffers_bytes", NULL, SC_TYPE_STORED, &stats_scratch_buffers_bytes);
  stats_unlock();
}

void
scratch_buffers2_global_deinit(void)
{
  stats_lock();
  stats_unregister_counter(SCS_GLOBAL, "scratch_buffers_count", NULL, SC_TYPE_STORED, &stats_scratch_buffers_count);
  stats_unregister_counter(SCS_GLOBAL, "scratch_buffers_bytes", NULL, SC_TYPE_STORED, &stats_scratch_buffers_bytes);
  stats_unlock();
}
