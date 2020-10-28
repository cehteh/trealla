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

#include "mpool.h"

//PLANNED: mt support, start with one cluster per thread which becomes the entry for new allocations
//PLANNED+  implement locking/stealing for clusters and main mpool

/*
  Implementation Notes: (braindump, planned features)

  * Free elements are chained on linked lists and marked on a bitmap
  * Ranges of free elements are coalesced
  * only the first and last bit of any free range is valid. Bits in-between have undefined states.

 */



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

/*
  Cluster and node structures are private
*/
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


MPool
mpool_cluster_alloc_ (MPool self);


#define MPOOL_BITMAP_SIZE(elements_per_cluster)                 \
  (((elements_per_cluster) + sizeof(uintptr_t)*CHAR_BIT - 1)    \
  / (sizeof(uintptr_t) * CHAR_BIT) * sizeof (uintptr_t))

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

      self->elements_free = 0;
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


static void*
cluster_element_get (MPoolcluster cluster, MPool self, size_t n)
{
  return (void*)cluster +                                       /* start address */
    sizeof (*cluster) +                                         /* header */
    MPOOL_BITMAP_SIZE (self->elements_per_cluster) +            /* bitmap */
    self->elem_size * n;                                        /* offset*/
}


static bool
bitmap_bit_get_nth (MPoolcluster cluster, size_t index)
{
  uintptr_t quot = index>>MPOOL_DIV_SHIFT;
  uintptr_t rem = index & ~((~MPOOL_C(0))<<MPOOL_DIV_SHIFT);
  uintptr_t* bitmap = (uintptr_t*)&cluster->data;

  return bitmap[quot] & ((uintptr_t)1<<rem);
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
            for (size_t i = 0; i < self->elements_per_cluster; ++i)
              {
                if (bitmap_bit_get_nth ((MPoolcluster)cluster, i))
                  {
                    void* element = cluster_element_get ((MPoolcluster)cluster, self, i);
                    self->destroy (element);
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
    }

  return self;
}


#if 0 //not implemented
MPool
mpool_purge (MPool self)
{
  return self;
}
#endif


MPool
mpool_cluster_alloc_ (MPool self)
{
#ifdef MPOOL_MALLOC_HOOKS
  MPoolcluster cluster = self->malloc_hook (self->cluster_size);
#else
  MPoolcluster cluster = malloc (self->cluster_size);
#endif

  if (!cluster)
    return NULL;

  /* clear the bitmap */
  memset (&cluster->data, 0, MPOOL_BITMAP_SIZE (self->elements_per_cluster));

  /* initialize freelist */
  //PLANNED: on coalesce only first/last needs to be initialized
  for (size_t i = 0; i < self->elements_per_cluster; ++i)
    {
      MPoolnode node = cluster_element_get (cluster, self, i);
      llist_insert_tail (&self->freelists[0], llist_init (&node->firstfree.node));
    }

  /* we insert the cluster at head because its likely be used next */
  llist_insert_head (&self->clusters, llist_init (&cluster->node));
  self->elements_free += self->elements_per_cluster;

  return self;
}


static int
cmp_cluster_contains_element (const_LList cluster, const_LList element, void* cluster_size)
{
  if (element < cluster)
    return -1;

  if ((void*)element > (void*)cluster + (uintptr_t)cluster_size)
    return 1;

  return 0;
}


static inline MPoolcluster
element_cluster_get (MPool self, void* element)
{
  return (MPoolcluster) llist_ufind (&self->clusters, (const LList) element, cmp_cluster_contains_element, (void*)self->cluster_size);
}


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


static void*
alloc_near (MPoolcluster cluster, MPool self, void* near)
{
  void* begin_of_elements =
    (void*)cluster +
    sizeof (*cluster) +                                                 /* header */
    MPOOL_BITMAP_SIZE (((MPool)self)->elements_per_cluster);            /* bitmap */

  uintptr_t index = (near - begin_of_elements) / self->elem_size;
  uintptr_t quot = index>>MPOOL_DIV_SHIFT;
  uintptr_t rem = index & ~((~MPOOL_C(0))<<MPOOL_DIV_SHIFT);

  uintptr_t* bitmap = (uintptr_t*)&cluster->data;
  size_t r = ~0;

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


static void
bitmap_set_element (MPoolcluster cluster, MPool self, void* element)
{
  void* begin_of_elements =
    (void*)cluster +
    sizeof (*cluster) +                                                 /* header */
    MPOOL_BITMAP_SIZE (((MPool)self)->elements_per_cluster);            /* bitmap */

  uintptr_t index = (element - begin_of_elements) / self->elem_size;
  uintptr_t quot = index>>MPOOL_DIV_SHIFT;
  uintptr_t rem = index & ~((~MPOOL_C(0))<<MPOOL_DIV_SHIFT);

  uintptr_t* bitmap = (uintptr_t*)&cluster->data;
  bitmap[quot] |= ((uintptr_t)1<<rem);
}


static void
bitmap_clear_element (MPoolcluster cluster, MPool self, void* element)
{
  void* begin_of_elements =
    (void*)cluster +
    sizeof (*cluster) +                                                 /* header */
    MPOOL_BITMAP_SIZE (((MPool)self)->elements_per_cluster);            /* bitmap */

  uintptr_t index = (element - begin_of_elements) / self->elem_size;
  uintptr_t quot = index>>MPOOL_DIV_SHIFT;
  uintptr_t rem = index & ~((~MPOOL_C(0))<<MPOOL_DIV_SHIFT);

  uintptr_t* bitmap = (uintptr_t*)&cluster->data;
  bitmap[quot] &= ~((uintptr_t)1<<rem);
}



void*
mpool_alloc (MPool self, void* near)
{
  if (!self->elements_free)
    {
      if (mpool_cluster_alloc_ (self))
        {
          near = NULL; /* supress alloc_near(), we have a new cluster */
        }
      else
        {
          return NULL;
        }
    }

  void* ret = NULL;

  if (near)
    {
      ret = alloc_near (element_cluster_get (self, near), self, near);
    }

  if (!ret)
    {
      //PLANNED: from buckets
      ret = llist_head (&self->freelists[0]);
    }

  if (ret)
    {
      //PLANNED: break coalesce
      bitmap_set_element (element_cluster_get (self, ret), self, ret);
      llist_unlink_fast_ ((LList)ret);
    }

  --self->elements_free;

  return ret;
}


void
mpool_free (MPool self, void** element)
{
  if (self && element)
    {
      MPoolcluster cluster = element_cluster_get (self, *element);

      if (cluster)
        {
          bitmap_clear_element (cluster, self, *element);
          llist_init (&((MPoolnode)*element)->firstfree.node);

          //TODO: coalesce
          llist_insert_tail (&self->freelists[0], &((MPoolnode)*element)->firstfree.node);

          ++self->elements_free;
          *element = NULL;
        }
    }
}



MPool
mpool_reserve (MPool self, size_t nelements)
{
  //TODO: interaction with self->free_cluster
  if (self)
    while (self->elements_free < nelements)
      if (!mpool_cluster_alloc_ (self))
        return NULL;

  return self;
}



/*
//      Local Variables:
//      mode: C
//      c-file-style: "gnu"
//      indent-tabs-mode: nil
//      End:
*/
