//#define MPOOL_RESERVE

#include <stdio.h>



#include "../../mpool.c"



int
main (int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  printf("mpool test:\n");

  struct mpool testpool;
  mpool_init (&testpool, 32, 32000, NULL);
  printf("free %zu\n", mpool_available (&testpool));

  mpool_reserve (&testpool, 32);
  printf("free %zu\n", mpool_available (&testpool));

  void* element1 = mpool_alloc (&testpool, NULL);
  printf("element1 at %p, free %zu\n", element1, mpool_available (&testpool));

  void* element2 = mpool_alloc (&testpool, NULL);
  printf("element2 at %p, free %zu\n", element2, mpool_available (&testpool));


  mpool_free (&testpool, element1);
  printf("free %zu\n", mpool_available (&testpool));

  element1 = mpool_alloc (&testpool, NULL);
  printf("element1 at %p, free %zu\n", element1, mpool_available (&testpool));
  
  mpool_destroy (&testpool);

  printf("done\n");
  return 0;
}




/// Local Variables:
/// compile-command: "gcc -Wall -Wextra -g -O0 mpool_test.c && valgrind ./a.out"
/// End:
