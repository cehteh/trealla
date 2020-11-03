#undef NDEBUG  // make sure asserts are always enabled

#include <stdio.h>
#include <stdlib.h>

#define MPOOL_MSG(fmt,...) fprintf(stderr, "%s:%d %s: " fmt "\n", __FILE__, __LINE__, __func__, ## __VA_ARGS__)
#define MPOOL_TRACE(fmt,fn) fprintf(stderr, "%s:%d %s: " #fn " = " fmt "\n", __FILE__, __LINE__, __func__, fn)
#define MPOOL_ASSERT(pred) assert(pred)


#include "../../mpool.c"


// Exhaustive constistency check for a MPool
void
mpool_debug_check (MPool self)
{
  // all free chunks bitmaps are correct set at begin/end only
  LLIST_FOREACH(&self->clusters, cluster)
    {
      size_t bits_expected_set = 0;

      for (unsigned i = 0; i < MPOOL_BUCKETS; ++i)
        {
          LLIST_FOREACH(&self->freelists[i], node)
            {
              if (mpool_get_cluster (self, node) == (MPoolcluster)cluster)
                {
                  MPoolnode firstnode = (MPoolnode)node;

                  if (firstnode->firstfree.nelements == 1)
                    {
                      size_t index = cluster_get_index ((MPoolcluster)cluster, self, node);
                      MPOOL_ASSERT (cluster_get_bit((MPoolcluster)cluster, index) == 1);
                      bits_expected_set += 1;
                    }
                  else
                    {
                      size_t startindex = cluster_get_index ((MPoolcluster)cluster, self, node);
                      size_t endindex = startindex+firstnode->firstfree.nelements-1;

                      MPOOL_ASSERT (cluster_get_bit((MPoolcluster)cluster, startindex) == 1);
                      for (size_t z = startindex+1; z < endindex; ++z)
                        MPOOL_ASSERT (cluster_get_bit((MPoolcluster)cluster, z) == 0);
                      MPOOL_ASSERT (cluster_get_bit((MPoolcluster)cluster, endindex) == 1);

                      MPoolnode lastnode = cluster_get_element((MPoolcluster)cluster, self, endindex);
                      MPOOL_ASSERT (lastnode->lastfree.null == NULL);
                      MPOOL_ASSERT (lastnode->lastfree.first == firstnode);

                      bits_expected_set += 2;
                    }
                }
            }
        }

      size_t bits_found_set = 0;

      for (size_t i = 0; i < self->elements_per_cluster; ++i)
        bits_found_set +=  cluster_get_bit((MPoolcluster)cluster, i);

      MPOOL_ASSERT (bits_expected_set == bits_found_set);
    }
}



void
destroy_report(void* p)
{
  MPOOL_MSG("destroyed %p", p);
}



void
test1 (void)
{
  MPOOL_MSG("construct/destruct");
  struct mpool testpool;
  mpool_init (&testpool, 16, 32000, destroy_report);
  MPOOL_MSG("free %zu", mpool_available (&testpool));

  mpool_reserve (&testpool, 32);
  MPOOL_MSG("free %zu", mpool_available (&testpool));
  MPOOL_ASSERT (mpool_available (&testpool) == 32000);

  mpool_destroy (&testpool);
}


void
test2 (void)
{
  MPOOL_MSG("alloc/free single elements");
  struct mpool testpool;
  mpool_init (&testpool, 16, 32000, destroy_report);

  MPOOL_MSG("alloc first element");
  void* element1 = mpool_alloc (&testpool, NULL);
  MPOOL_MSG("element1 %p", element1);
  MPOOL_ASSERT (element1);
  MPOOL_ASSERT (mpool_available (&testpool) == 31999);

  MPoolcluster cluster = mpool_get_cluster (&testpool, element1);
  MPOOL_ASSERT (cluster);
  {
    MPoolnode firstnode =(MPoolnode)cluster_get_element (cluster, &testpool, 1);
    MPoolnode lastnode =(MPoolnode)cluster_get_element (cluster, &testpool, 31999);
    MPOOL_ASSERT(lastnode->lastfree.first == firstnode);
    MPOOL_ASSERT(lastnode->lastfree.null == NULL);

    MPOOL_ASSERT (cluster_get_bit (cluster, 0) == 0);
    MPOOL_ASSERT (cluster_get_bit (cluster, 1) == 1);
    for (size_t i = 2; i<31999; ++i)
      {
        MPOOL_ASSERT (cluster_get_bit (cluster, i) == 0);
      }
    MPOOL_ASSERT (cluster_get_bit (cluster, 31999) == 1);
  }

  MPOOL_MSG("free first element");
  mpool_free (&testpool, &element1);
  MPOOL_ASSERT (!element1);
  MPOOL_ASSERT (mpool_available (&testpool) == 32000);

  {
    MPoolnode firstnode =(MPoolnode)cluster_get_element (cluster, &testpool, 0);
    MPoolnode lastnode =(MPoolnode)cluster_get_element (cluster, &testpool, 31999);
    MPOOL_MSG("lastnode - firstnode = %zu", ((char*)lastnode-(char*)firstnode)/testpool.elem_size);
    MPOOL_ASSERT(lastnode->lastfree.null == NULL);
    MPOOL_ASSERT(lastnode->lastfree.first == firstnode);

    MPOOL_ASSERT (cluster_get_bit (cluster, 0) == 1);
    for (size_t i = 1; i<31999; ++i)
      {
        MPOOL_ASSERT (cluster_get_bit (cluster, i) == 0);
      }
    MPOOL_ASSERT (cluster_get_bit (cluster, 31999) == 1);
  }

  MPOOL_MSG("alloc first element again");
  element1 = mpool_alloc (&testpool, NULL);
  MPOOL_ASSERT (element1);

  {
    MPoolnode firstnode =(MPoolnode)cluster_get_element (cluster, &testpool, 1);
    MPoolnode lastnode =(MPoolnode)cluster_get_element (cluster, &testpool, 31999);
    MPOOL_ASSERT(lastnode->lastfree.first == firstnode);
    MPOOL_ASSERT(lastnode->lastfree.null == NULL);

    MPOOL_ASSERT (cluster_get_bit (cluster, 0) == 0);
    MPOOL_ASSERT (cluster_get_bit (cluster, 1) == 1);
    for (size_t i = 2; i<31999; ++i)
      {
        MPOOL_ASSERT (cluster_get_bit (cluster, i) == 0);
      }
    MPOOL_ASSERT (cluster_get_bit (cluster, 31999) == 1);
  }

  MPOOL_MSG("alloc second element");
  void* element2 = mpool_alloc (&testpool, NULL);
  MPOOL_ASSERT (element2);
  MPOOL_ASSERT (mpool_available (&testpool) == 31998);

  {
    MPoolnode firstnode =(MPoolnode)cluster_get_element (cluster, &testpool, 2);
    MPoolnode lastnode =(MPoolnode)cluster_get_element (cluster, &testpool, 31999);
    MPOOL_ASSERT(lastnode->lastfree.first == firstnode);
    MPOOL_ASSERT(lastnode->lastfree.null == NULL);

    MPOOL_ASSERT (cluster_get_bit (cluster, 0) == 0);
    MPOOL_ASSERT (cluster_get_bit (cluster, 1) == 0);
    MPOOL_ASSERT (cluster_get_bit (cluster, 2) == 1);
    for (size_t i = 3; i<31999; ++i)
      {
        MPOOL_ASSERT (cluster_get_bit (cluster, i) == 0);
      }
    MPOOL_ASSERT (cluster_get_bit (cluster, 31999) == 1);
  }

  MPOOL_MSG("free second element");
  mpool_free (&testpool, &element2);
  MPOOL_ASSERT (!element2);
  MPOOL_ASSERT (mpool_available (&testpool) == 31999);

  {
    MPoolnode firstnode =(MPoolnode)cluster_get_element (cluster, &testpool, 1);
    MPoolnode lastnode =(MPoolnode)cluster_get_element (cluster, &testpool, 31999);
    MPOOL_ASSERT(lastnode->lastfree.first == firstnode);
    MPOOL_ASSERT(lastnode->lastfree.null == NULL);

    MPOOL_ASSERT (cluster_get_bit (cluster, 0) == 0);
    MPOOL_ASSERT (cluster_get_bit (cluster, 1) == 1);
    for (size_t i = 2; i<31999; ++i)
      {
        MPOOL_ASSERT (cluster_get_bit (cluster, i) == 0);
      }
    MPOOL_ASSERT (cluster_get_bit (cluster, 31999) == 1);
  }

  MPOOL_MSG("free first element");
  mpool_free (&testpool, &element1);
  MPOOL_ASSERT (!element1);
  MPOOL_ASSERT (mpool_available (&testpool) == 32000);

  {
    MPoolnode firstnode =(MPoolnode)cluster_get_element (cluster, &testpool, 0);
    MPoolnode lastnode =(MPoolnode)cluster_get_element (cluster, &testpool, 31999);
    MPOOL_MSG("lastnode - firstnode = %zu", ((char*)lastnode-(char*)firstnode)/testpool.elem_size);
    MPOOL_ASSERT(lastnode->lastfree.null == NULL);
    MPOOL_ASSERT(lastnode->lastfree.first == firstnode);

    MPOOL_ASSERT (cluster_get_bit (cluster, 0) == 1);
    for (size_t i = 1; i<31999; ++i)
      {
        MPOOL_ASSERT (cluster_get_bit (cluster, i) == 0);
      }
    MPOOL_ASSERT (cluster_get_bit (cluster, 31999) == 1);
  }

  mpool_destroy (&testpool);
}

void
test3 (void)
{
  MPOOL_MSG ("alloc/free elements at at random elements");
  struct mpool testpool;
  mpool_init (&testpool, 16, 32000, destroy_report);

  void* elementv[31000];
  for (unsigned i = 0; i < 31000; ++i)
  {
    elementv[i] = NULL;
  }

  for (unsigned i = 0; i < 15000; ++i)
  {
    elementv[i] = mpool_alloc (&testpool, NULL);
  }


  // shuffle
  for (unsigned i = 0; i < 31000; ++i)
  {
    void* tmp = elementv[i];
    unsigned r = rand() % 31000;

    elementv[i] = elementv[r];
    elementv[r] = tmp;
  }

  MPOOL_MSG ("free %zu", mpool_available (&testpool));

  mpool_debug_check (&testpool);

  for (unsigned i = 0; i < 30000; i+=2)
  {
    MPOOL_MSG ("freeing %u %p", i, elementv[i]);
    mpool_debug_check (&testpool);
    mpool_free (&testpool, &elementv[i]);
    MPOOL_MSG ("freed %u", i);
    mpool_debug_check (&testpool);
  }

  MPOOL_MSG ("free %zu", mpool_available (&testpool));

  mpool_destroy (&testpool);
}


int
main (int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  MPOOL_MSG("test start");

  test1 ();
  test2 ();
  test3 ();

  MPOOL_MSG("tests done");
  return 0;
}

/// Local Variables:
/// compile-command: "gcc -Wall -Wextra -g -O0 mpool_test.c && valgrind ./a.out"
/// End:
