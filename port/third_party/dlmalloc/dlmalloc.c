/* dlmalloc configured as a pure mspace allocator over the guest arena (no sbrk/mmap growth). */
#define ONLY_MSPACES 1
#define MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 1
#define USE_DL_PREFIX 1
#define FOOTERS 0
#define ABORT_ON_ASSERT_FAILURE 0
#include "malloc.c"
