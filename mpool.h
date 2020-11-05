/*
    mpool.h - memory pool for constant sized objects

  Copyright (C)         Lumiera.org
    2009,               Christian Thaeter <ct@pipapo.org>
    2020,               Christian Thaeter <ct@pipapo.org>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifndef MPOOL_H
#define MPOOL_H

#include <stdint.h>
#include "llist.h"

//mpool:
//: Memory Pools
//: ------------
//:
//: Memory pools are implemented as clusters of fixed sized elements. New clusters
//: are allocated on demand or manually preallocated with a `reserve()` operation.
//: Some efforts are taken to ensure (cache) locality of the provided memory.
//: All functions are reentrant but not threadsafe, if this is desired it is advised to
//: care for proper locking elsewhere.
//:


//PLANNED: cluster dispatcher for calloc different range sizes prefer different clusters
//PLANNED: macro calculation elements_per_cluster for a given cluster size, allowing page aligned clusters
// support continuous allocations of n elements at once
//PLANNED: #define MPOOL_CONTINUOUS_ALLOCATIONS

// coalescing of freed elements
//PLANNED: #define MPOOL_COALESCE

// bucketize the freelist
#define MPOOL_BUCKETS 8

//PLANNED: RANK/INCREMENT of each bucket 2 => (1 4 16 64 256 1024 4096 16384 65536 ...)

//PLANNED: strategy for non-near allocations: new bucket if less than 50% of a bucket is free



//: [[mpool_destroy_fn]]
//: .mpool_destroy_fn
//: When a memory pool gets destroyed it can call a destructor for any element which is still in the pool.
//: This destructor is optional.
//:
//:  typedef void (*mpool_destroy_fn)(void* self)
//:
//:  `self`::
//:         element to be destroyed
//:
typedef void (*mpool_destroy_fn)(void* self);


//: [[struct_mpool]]
//: .mpool pointer
//:  typedef struct mpool* MPool
//:
//: Handle to a mpool. The struct mpool is exposed, but should only be used readonly for querying attributes
typedef struct mpool* MPool;
typedef struct mpoolcluster* MPoolcluster;

struct mpool
{
  struct llist freelists[MPOOL_BUCKETS];
  struct llist clusters;
  MPoolcluster linger_cluster; // when a cluster becomes completely unused its cached here
  size_t elem_size;
  size_t elements_per_cluster;
  size_t cluster_size;
  size_t elements_free;
  size_t clusters_allocated;
  //TODO: void* locality; removed for now //PLANNED: tls -> clusters
  mpool_destroy_fn destroy;
#ifdef MPOOL_MALLOC_HOOKS
  void *(*malloc_hook)(size_t);
  void (*free_hook)(void *);
#endif
};


#ifdef MPOOL_MALLOC_HOOKS
extern void *(*mpool_malloc_hook)(size_t size);
extern void (*mpool_free_hook)(void *ptr);
#endif

#ifdef MPOOL_INIT_HOOKS
/* called after a mpool got initialized */
extern void (*mpool_init_hook) (MPool self);
/* called before a mpool gets destroyed */
extern void (*mpool_destroy_hook) (MPool self);
#endif

//: [[mpool_init]]
//: .mpool_init
//: Initialize a memory pool, memory pools must be initialized before being used. One can supply
//: an optional destructor function for elements, this will be used to destroy elements which are still
//: in the pool when it gets destroyed itself. The destructor is _NOT_ called when elemented are freed.
//:
//:  MPool mpool_init (MPool self, size_t elem_size, size_t elements_per_cluster, mpool_move_fn mv, mpool_destroy_fn dtor)
//:
//:  `self`::
//:         pointer to the memory pool structure to be initialized
//:  `elem_size`::
//:         size for a single element
//:  `elements_per_cluster`::
//:         how many elements to put into a cluster
//:  `dtor`::
//:         pointer to an optional destructor function or NULL
//:  return::
//:         self
//:
MPool
mpool_init (MPool self, size_t elem_size, size_t elements_per_cluster, mpool_destroy_fn dtor);


//: [[mpool_destroy]]
//: .mpool_destroy
//: A memory pool is not used anymore it should be destroyed. This frees all memory allocated with it.
//: When a destructor was provided at construction time, then this destructor is used on all non free elements
//: before before the clusters are freed. If no destructor was given then the clusters are just freed.
//: The destroyed memory pool behaves as if it was freshly initialized and can be used again, this is some kindof
//: exceptional behaviour.
//:
//:  MPool mpool_destroy (MPool self)
//:
//:  `self`::
//:         pointer to an initialized memory pool to be destroyed.
//:  return::
//:         self
//:
//:
MPool
mpool_destroy (MPool self);

// : [[mpool_purge]]
// : .mpool_purge
// :
//PLANNED: mpool_purge() destroy all elements, but keep the mpool initialized
// :
// :
//MPool
//mpool_purge (MPool self);


//: [[mpool_available]]
//: .mpool_available
//: One can check how much elements are available without a new cluster allocation in a memory pool.
//:
//:  size_t mpool_available (MPool self)
//:
//:  `self`::
//:         pointer to the memory pool to be queried
//:  return::
//:         number of available elements
//:
static inline size_t
mpool_available (MPool self)
{
  return self->elements_free;
}

//: [[mpool_reserve]]
//: .mpool_reserve
//: Resize the pool that at least nelements become available without cluster reallocations
//:
//:  MPool mpool_reserve (MPool self, size_t nelements)
//:
//:  `self`::
//:         pointer to the memory pool
//:  `nelements`::
//:         minimum number of elements to preallocate
//:  return::
//:         self on success or NULL on error
//:
MPool
mpool_reserve (MPool self, size_t nelements);

//: [[mpool_alloc]]
//: .mpool_alloc
//: Allocates on element from a mpool. To improve cache locality the allocation
//: tries to get an element close to another.
//:
//:  void* mpool_alloc (MPool self, void* near)
//:
//:  `self`::
//:         pointer to the memory pool
//:  `near`::
//:         reference to another element which should be close to the returned element (hint only)
//TODO:     NULL request an element with explicitly no locality (possibly allocating a new cluster)
//:  return::
//:         pointer to the allocated memory on success or NULL on error
//:         will never fail when enough space was preallocated
//:
void*
mpool_alloc (MPool self, void* near);

//PLANNED: implement me, allocate ncont continuous elements
// :  return::
// :         pointer to the allocated memory on success or NULL on error
// :         will fail when more elements than elements per cluster where requested
// :
// : Should be freed with mpool_cfree, but plain mpool_free can be used for partial frees.
// : important is to free all memory eventually and not to leak.
// : does not clear the memory
// :
//void*
//mpool_calloc (MPool self, size_t ncont, void* near);

//: [[mpool_free]]
//: .mpool_free
//: Frees the given element and puts it back into the pool for furhter allocations.
//:
//:  void mpool_free (MPool self, void** element)
//:
//:  `self`::
//:         pointer to the memory pool
//:  `element`::
//:         reference to element to be freed
//:
void
mpool_free (MPool self, void** element);

//PLANNED: implement me, free ncont continuous elements
//void
//mpool_cfree (MPool self, void* element, size_t ncont);

//PLANNED * alloc/free for n contingous elements


#endif

//      Local Variables:
//      mode: C
//      c-file-style: "gnu"
//      indent-tabs-mode: nil
//      End:
