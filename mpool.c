/*
    mpool.c - memory pool for constant sized objects

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

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <assert.h>

#include "mpool.h"

#ifndef MPOOL_ASSERT
#define MPOOL_ASSERT(pred) assert(pred)
#endif

#ifndef MPOOL_MSG
#define MPOOL_MSG(...)
#endif

//
// MPool
// The management structure for related allocations of same element size.
// Maintains freelists of all free chunks.
// Memory is allocated in fixed sized clusters, managed by MPool.
//
// Allocation strategies:
// a 'near' allocation is cluster local and searches 3*bits_per_pointer around the near hint for free elements.
//
// MPoolcluster
// A bitmap with bits set at free-chunk borders.
// Followed by elements. Minimum size for an element is 3 pointers. Aligned on pointer size.
//
// free-chunks are ranges of unused elements where the first and the last element contain
// management information to manage the freed memory. Free chunks are always coalesced.
//


//PLANNED: mt support, start with one cluster per thread which becomes the entry for new allocations
//PLANNED+  implement locking/stealing for clusters and main mpool

//  Implementation Notes:
//
// * Free elements are chained on linked lists
// * Ranges of free elements are coalesced
// * the first and last bit of any free range is set. These nodes are interpreted as firstfree/lastfree
//  * all other bits in the bitmap are cleared



#if UINTPTR_MAX > 4294967295U   /* 64 bit */
#define MPOOL_DIV_SHIFT 6
#define MPOOL_C(c) c ## ULL
#else                           /* 32 bit */
#define MPOOL_DIV_SHIFT 5
#define MPOOL_C(c) c ## UL
#endif

/*
  defaults for the hooks, used when creating mpools
 */
#ifdef MPOOL_MALLOC_HOOKS
void *(*mpool_malloc_hook)(size_t size) = malloc;
void (*mpool_free_hook)(void *ptr) = free;
#endif


#ifdef MPOOL_INIT_HOOKS
/* called after a mpool got initialized */
void (*mpool_init_hook) (MPool self) = NULL;
/* called before a mpool gets destroyed */
void (*mpool_destroy_hook) (MPool self) = NULL;
#endif

#define MPOOL_BITMAP_SIZE(elements_per_cluster)                 \
  (((elements_per_cluster) + sizeof(uintptr_t)*CHAR_BIT - 1)    \
  / (sizeof(uintptr_t) * CHAR_BIT) * sizeof (uintptr_t))

//
//  Cluster and node structures are private
//
typedef struct mpoolcluster* MPoolcluster;

struct mpoolcluster
{
  struct llist node;    /* all clusters */
  char data[];          /* bitmap and elements */
};


typedef union mpoolnode* MPoolnode;

union mpoolnode
{
  // the first element on a free range (or single) is used for the freelist
  struct {
    struct llist node;
    size_t nelements; //TODO: coalesce
  } firstfree;

  // the last element on a free range (>1 elements) is used as backpointer to the first
  struct {
    MPoolnode first;
    void* null; // being NULL identifies a lastfree element
  } lastfree;
};



//
// Forward declarations
//

static MPool
mpool_cluster_alloc (MPool self);

static void
mpool_freellist_insert (MPool self, MPoolnode node);

static void
cluster_bit_set (MPoolcluster cluster, size_t index);

static void
cluster_bit_clear (MPoolcluster cluster, size_t index);

static void*
cluster_get_element (MPoolcluster cluster, MPool self, size_t n);

static size_t
cluster_get_index (MPoolcluster cluster, MPool self, void* element);

static bool
cluster_get_bit (MPoolcluster cluster, size_t index);

static void*
mpool_alloc_far (MPool self, size_t n);

static MPoolcluster
mpool_get_cluster (MPool self, void* element);

//
// MPool (public api)
//

MPool
mpool_init (MPool self, size_t elem_size, size_t elements_per_cluster, mpool_destroy_fn dtor)
{
  if (self)
    {
      for (unsigned i = 0; i < MPOOL_BUCKETS; ++i)
        {
          llist_init (&self->freelists[i]);
        }
      llist_init (&self->clusters);

      /* minimum size is the size of a union mpoolnode */
      if (elem_size < sizeof(union mpoolnode))
        elem_size = sizeof(union mpoolnode);

      self->elem_size = (elem_size+sizeof(void*)-1) / sizeof(void*) * sizeof(void*);    /* pointer aligned */

      self->elements_per_cluster = elements_per_cluster;

      self->cluster_size = sizeof (struct mpoolcluster) +       /* header */
        MPOOL_BITMAP_SIZE (self->elements_per_cluster) +        /* bitmap */
        self->elem_size * self->elements_per_cluster;           /* elements */

      self->linger_cluster = NULL;
      self->elements_free = 0;
      self->clusters_allocated = 0;
      self->destroy = dtor;

#ifdef MPOOL_MALLOC_HOOKS
      self->malloc_hook = mpool_malloc_hook;
      self->free_hook = mpool_free_hook;
#endif

#ifdef MPOOL_INIT_HOOKS
      if (mpool_init_hook)
        mpool_init_hook (self);
#endif
    }

  return self;
}

MPool
mpool_destroy (MPool self)
{
  if (self)
    {
#ifdef MPOOL_INIT_HOOKS
      if (mpool_destroy_hook)
        mpool_destroy_hook (self);
#endif

      LLIST_WHILE_TAIL(&self->clusters, cluster)
        {
          if (self->destroy)
            {
              // important: destroy is called per element, even when ranges where allocated
              //TODO: should destroy return a size/skip value?
              bool isfree = cluster_get_bit ((MPoolcluster)cluster, 0);
              for (size_t i = isfree; i < self->elements_per_cluster; ++i)
                {
                  if(cluster_get_bit ((MPoolcluster)cluster, i))
                    {
                      isfree = !isfree;
                    }
                  else if (!isfree)
                    {
                      void* element = cluster_get_element ((MPoolcluster)cluster, self, i);
                      self->destroy (element);
                    }
                }
            }

          llist_unlink_fast_ (cluster);
#ifdef MPOOL_MALLOC_HOOKS
          self->free_hook (cluster);
#else
          free (cluster);
#endif
        }

      for (unsigned i = 0; i < MPOOL_BUCKETS; ++i)
        {
          llist_init (&self->freelists[i]);
        }

      self->elements_free = 0;
      self->clusters_allocated = 0;
    }

  return self;
}

#if 0 //PLANNED: not implemented
MPool
mpool_purge (MPool self)
{
  return self;
}
#endif


// alloc/free/reserve
void*
mpool_alloc (MPool self, void* near)
{
  if (!self)
    return NULL;

  if (!self->elements_free || (near == NULL && self->elements_free < self->elements_per_cluster/2))
    {
      if (mpool_cluster_alloc (self))
        {
          near = NULL; /* supress alloc_near(), we have a new cluster */
        }
      else if (!self->elements_free)
        {
          return NULL;
        }
    }

  MPoolnode node = NULL;

#if 0 //FIXME: alloc_near isn't ready yet
  if (near)
    {
      node = mpool_alloc_near (self, near, 1);
    }
#endif

  if (!node)
    {
      node = mpool_alloc_far (self, 1);
    }

  if (node)
    --self->elements_free;

  return node;
}


void
mpool_cfree (MPool self, void* element, size_t n)
{
  if (self && element)
    {
      MPoolcluster cluster = mpool_get_cluster (self, element);
      MPOOL_ASSERT(cluster); // address not in pool

      size_t startindex = cluster_get_index (cluster, self, element);
      size_t chunksize = n;

      MPoolnode chunkstart = (MPoolnode)element;
      MPoolnode chunkend = (MPoolnode)((char*)element + self->elem_size * (chunksize-1));

      llist_init (&chunkstart->firstfree.node);  // maybe unused when coalescing front

      if (startindex > 0 && cluster_get_bit (cluster, startindex-1))
        {
          // coalesce front
          chunkstart = cluster_get_element (cluster, self, startindex-1);
          if (chunkstart->lastfree.null != NULL)
            {
              // front size == 1
              MPOOL_ASSERT (chunkstart->firstfree.nelements == 1);
              startindex -= 1;
              chunksize += 1;
            }
          else
            {
              // front size > 1
              cluster_bit_clear (cluster, startindex-1);
              chunkstart = chunkstart->lastfree.first;
              startindex -= chunkstart->firstfree.nelements;
              chunksize += chunkstart->firstfree.nelements;
            }
          llist_unlink_fast_ (&chunkstart->firstfree.node);
        }
      else
        cluster_bit_set (cluster, startindex);

      if (startindex + chunksize < self->elements_per_cluster-1 && cluster_get_bit (cluster, startindex+chunksize))
        {
          // coalesce back
          chunkend = cluster_get_element (cluster, self, startindex+chunksize);
          if (chunkend->firstfree.nelements == 1)
            {
              // back size == 1
              chunksize += 1;
              llist_unlink_fast_ (&chunkend->firstfree.node);
            }
          else
            {
              // back size > 1
              cluster_bit_clear (cluster, startindex+chunksize);
              llist_unlink_fast_ (&chunkend->firstfree.node);
              chunksize += chunkend->firstfree.nelements;
              chunkend = (MPoolnode)((char*)chunkend + self->elem_size * (chunkend->firstfree.nelements-1));
            }
        }
      else if (chunksize > 1)
        cluster_bit_set (cluster, startindex+chunksize-1);

      chunkstart->firstfree.nelements = chunksize;

      if (chunkend != chunkstart)
        {
          chunkend->lastfree.first = chunkstart;
          chunkend->lastfree.null = NULL;
        }

      MPOOL_ASSERT(cluster_get_bit (cluster, startindex));
      MPOOL_ASSERT(cluster_get_bit (cluster, startindex+chunksize-1));

      mpool_freellist_insert (self, chunkstart);

      self->elements_free += n;

      if (chunksize == self->elements_per_cluster)
        {
          if (self->linger_cluster && cluster_get_bit(self->linger_cluster, 0))
            {
              MPoolnode maybefree = (MPoolnode)cluster_get_element (self->linger_cluster, self, 0);
              if (maybefree->firstfree.nelements == self->elements_per_cluster)
                {
                  MPOOL_MSG("freeing cluster %p", self->linger_cluster);
                  llist_unlink_fast_ (&maybefree->firstfree.node);
                  llist_unlink_fast_ (&self->linger_cluster->node);
#ifdef MPOOL_MALLOC_HOOKS
                  self->free_hook (self->linger_cluster);
#else
                  free (self->linger_cluster);
#endif
                }
            }
          self->linger_cluster = cluster;
        }
    }
}

void
mpool_free (MPool self, void* element)
{
  mpool_cfree (self, element, 1);
}



MPool
mpool_reserve (MPool self, size_t nelements)
{
  //TODO: interaction with self->linger_cluster
  if (self)
    while (self->elements_free < nelements)
      if (!mpool_cluster_alloc (self))
        return NULL;

  return self;
}







//
// MPool private
//

static void
mpool_freellist_insert (MPool self, MPoolnode node)
{
  unsigned i = 0;
  while (1U<<i < node->firstfree.nelements && i < MPOOL_BUCKETS-1)
    ++i;

  llist_insert_tail (&self->freelists[i], llist_init (&node->firstfree.node));
}


static MPool
mpool_cluster_alloc (MPool self)
{
#ifdef MPOOL_MALLOC_HOOKS
  MPoolcluster cluster = self->malloc_hook (self->cluster_size);
#else
  MPoolcluster cluster = malloc (self->cluster_size);
#endif

  MPOOL_MSG ("allocated cluster %p", cluster);

  if (!cluster)
    return NULL;

  MPoolnode first = cluster_get_element (cluster, self, 0);
  MPoolnode last = cluster_get_element (cluster, self, self->elements_per_cluster-1);

  memset (&cluster->data, 0, MPOOL_BITMAP_SIZE (self->elements_per_cluster));
  cluster_bit_set (cluster, 0);
  cluster_bit_set (cluster, self->elements_per_cluster-1);

  last->lastfree.null = NULL;
  last->lastfree.first = first;

  llist_init (&first->firstfree.node);
  first->firstfree.nelements = self->elements_per_cluster;

  mpool_freellist_insert (self, first);

  self->elements_free += self->elements_per_cluster;

  llist_insert_head (&self->clusters, llist_init (&cluster->node));
  ++self->clusters_allocated;

  return self;
}


//TODO: refactor
static int
cmp_cluster_contains_element (const_LList cluster, const_LList element, void* cluster_size)
{
  if (element < cluster)
    return -1;

  if ((char*)element > (char*)cluster + (uintptr_t)cluster_size)
    return 1;

  return 0;
}

static MPoolcluster
mpool_get_cluster (MPool self, void* element)
{
  return (MPoolcluster) llist_ufind (&self->clusters, (const LList) element, cmp_cluster_contains_element, (void*)self->cluster_size);
}


static int
find_larger_or_equal (const_LList node, const_LList unused, void* extra)
{
  (void) unused;

  if (((MPoolnode)node)->firstfree.nelements >= (size_t) extra)
    return 0;
  else
    return -1;
}


static void*
mpool_alloc_far (MPool self, size_t n)
{
  unsigned i = 0;
  while ((1U<<i < n || llist_is_empty (&self->freelists[i])) && i < MPOOL_BUCKETS-1)
    ++i;

  MPoolnode chunkstart = (MPoolnode)llist_find (&self->freelists[i], NULL, find_larger_or_equal, (void*)n);

  if (!chunkstart)
    return NULL;

  llist_unlink_fast_ (&chunkstart->firstfree.node);

  MPoolcluster cluster = mpool_get_cluster (self, chunkstart);
  MPOOL_ASSERT(cluster);

  size_t startindex = cluster_get_index(cluster, self, chunkstart);
  cluster_bit_clear (cluster, startindex);

  if (chunkstart->firstfree.nelements > n)
    {
      // split chunkstart and reinsert the reset
      MPoolnode nchunk = (MPoolnode)((char*)chunkstart + self->elem_size * n);
      llist_init (&nchunk->firstfree.node);
      nchunk->firstfree.nelements = chunkstart->firstfree.nelements - n;

      if (nchunk->firstfree.nelements > 1)
        cluster_bit_set (cluster, startindex + n);

      if (nchunk->firstfree.nelements > 1)
        {
          MPoolnode chunkend = (MPoolnode)((char*)chunkstart + self->elem_size * nchunk->firstfree.nelements);
          MPOOL_ASSERT (chunkend->lastfree.null == NULL);
          chunkend->lastfree.first = nchunk;
        }
      mpool_freellist_insert (self, nchunk);
    }
  else
    {
      MPOOL_ASSERT(chunkstart->firstfree.nelements == n);
      if (n>1)
        cluster_bit_clear (cluster, startindex+n);
    }
  return chunkstart;
}


#if 0
static void*
alloc_near (MPoolcluster cluster, MPool self, void* near)
//TODO alloc_near (MPool self, size_t n, void* near)
//TODO: uintptr_t to size_t
{
  void* begin_of_elements =
    (char*)cluster +
    sizeof (*cluster) +                                                 /* header */
    MPOOL_BITMAP_SIZE (((MPool)self)->elements_per_cluster);            /* bitmap */

  uintptr_t index = (near - begin_of_elements) / self->elem_size;
  uintptr_t quot = index>>MPOOL_DIV_SHIFT;
  uintptr_t rem = index & ~((~MPOOL_C(0))<<MPOOL_DIV_SHIFT);

  uintptr_t* bitmap = (uintptr_t*)&cluster->data;
  size_t r = ~0;

  //FIXME: new semantic for bitmap
  /* the bitmap word at near */
  if (bitmap[quot] < UINTPTR_MAX)
    {
      r = uintptr_nearestbit (~bitmap[quot], rem);
    }
  /* the bitmap word before near, this gives a slight bias towards the begin, keeping the pool compact */
  else if (quot && bitmap[quot-1] < UINTPTR_MAX)
    {
      --quot;
      r = uintptr_nearestbit (~bitmap[quot], sizeof(uintptr_t)*CHAR_BIT-1);
    }

  if (r != ~0U && (quot*sizeof(uintptr_t)*CHAR_BIT+r) < self->elements_per_cluster)
    {
      void* ret = begin_of_elements + ((uintptr_t)(quot*sizeof(uintptr_t)*CHAR_BIT+r)*self->elem_size);
      return ret;
    }
  return NULL;
}
#endif


//
// MPoolcluster
//

static void*
cluster_get_element (MPoolcluster cluster, MPool self, size_t n)
{
  return (char*)cluster +                                       /* start address */
    sizeof (*cluster) +                                         /* header */
    MPOOL_BITMAP_SIZE (self->elements_per_cluster) +            /* bitmap */
    self->elem_size * n;                                        /* offset*/
}

static size_t
cluster_get_index (MPoolcluster cluster, MPool self, void* element)
{
  char* elements_start =  (char*)cluster +                      /* start address */
    sizeof (*cluster) +                                         /* header */
    MPOOL_BITMAP_SIZE (self->elements_per_cluster);

  return ((char*)element - elements_start) / self->elem_size;
}


static bool
cluster_get_bit (MPoolcluster cluster, size_t index)
{
  uintptr_t quot = index>>MPOOL_DIV_SHIFT;
  uintptr_t rem = index & ~((~MPOOL_C(0))<<MPOOL_DIV_SHIFT);
  uintptr_t* bitmap = (uintptr_t*)&cluster->data;

  return bitmap[quot] & ((uintptr_t)1<<rem);
}


static void
cluster_bit_set (MPoolcluster cluster, size_t index)
{
  MPOOL_ASSERT(cluster_get_bit (cluster, index) == 0);
  uintptr_t quot = index>>MPOOL_DIV_SHIFT;
  uintptr_t rem = index & ~((~MPOOL_C(0))<<MPOOL_DIV_SHIFT);
  uintptr_t* bitmap = (uintptr_t*)&cluster->data;

  bitmap[quot] |= ((size_t)1<<rem);
}


static void
cluster_bit_clear (MPoolcluster cluster, size_t index)
{
  MPOOL_ASSERT(cluster_get_bit (cluster, index) == 1);
  uintptr_t quot = index>>MPOOL_DIV_SHIFT;
  uintptr_t rem = index & ~((~MPOOL_C(0))<<MPOOL_DIV_SHIFT);
  uintptr_t* bitmap = (uintptr_t*)&cluster->data;

  bitmap[quot] &= ~((size_t)1<<rem);
}



#if 0
static size_t
uintptr_nearestbit (uintptr_t v, size_t n)
{
  size_t r = 0;
  uintptr_t mask = MPOOL_C(1)<<n;

  while (1)
    {
      if (v&mask)
        {
          if (v&mask& ~(~0ULL<<n))
            return n-r;
          else
            return n+r;
        }
      if (mask == ~MPOOL_C(0))
        return ~0U;
      ++r;
      mask |= ((mask<<1)|(mask>>1));
    }
}
#endif


/*
//      Local Variables:
//      mode: C
//      c-file-style: "gnu"
//      indent-tabs-mode: nil
//      End:
*/
