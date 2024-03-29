#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#ifdef TEXAS_RUNTIME
void texas_escape(void **loc, void* new);
int texas_check_boundary(void *ptr, void *res, size_t size);
void *tracked_malloc(size_t sz);
void tracked_free(void* p);
#else
#ifdef CAMP_RUNTIME
int __gep_check_boundary(void *, void *, size_t);
void __escape(void **, void *);
#endif

static __always_inline int texas_check_boundary(void *base, void *ptr, size_t size) {
#ifdef CAMP_RUNTIME
  return __gep_check_boundary(base, ptr, size);
#else
  return 0;
#endif
}
static __always_inline void texas_escape(void **loc, void* new) {
#ifdef CAMP_RUNTIME
  __escape(loc, new);
#endif
}
static __always_inline void *tracked_malloc(size_t sz) { return malloc(sz); };
static __always_inline void tracked_free(void* p) { return free(p); };
#endif



#define ALLOC_SIZE 0x30000
#define MAX_SIZE 0x20000
#define MIN_SIZE 0x100

int main() {
  clock_t main_begin, begin, end;
  double time_spent;

  // pesudo random
  srand(0);

  main_begin = clock();
  void **global = tracked_malloc(ALLOC_SIZE * sizeof(void *));
  size_t *global_size = tracked_malloc(ALLOC_SIZE * sizeof(size_t));
  void **global_escapes = tracked_malloc(ALLOC_SIZE * sizeof(void *));
  assert(global != 0);
  assert(global_size != 0);

  // non heap test
  begin = clock();
  char data[0x100];
  void *heap = tracked_malloc(0x100);
  for (size_t i=0; i<ALLOC_SIZE*0x1000; i++) {
    assert(texas_check_boundary(data, data, 0x100) == 1);
    // assert(texas_check_boundary(heap, heap, 0x100) == 0);
  }
  end = clock();
  time_spent = (double)(end - main_begin) / CLOCKS_PER_SEC;
  printf("time spent for checking non-heap is %f s\n\n", time_spent);
  
  // allocation test
  begin = clock();
  for (int i=0; i<ALLOC_SIZE; i++) {
    size_t size = (rand() % MAX_SIZE) + MIN_SIZE;
    global[i] = tracked_malloc(size + MIN_SIZE);
    assert(global[i] != 0);
    global_size[i] = size;
  }
  end = clock();
  time_spent = (double)(end - begin) / CLOCKS_PER_SEC;
  printf("time spent for allocation is %f s\n", time_spent);

  // escape test
  begin = clock();
  for (int i=0; i<ALLOC_SIZE*0x100; i++) {
    int x = rand() % ALLOC_SIZE;
    int y = rand() % ALLOC_SIZE;
    texas_escape(&global_escapes[x], global[y]);
    global_escapes[x] = global[y];
  }
  end = clock();
  time_spent = (double)(end - begin) / CLOCKS_PER_SEC;
  printf("time spent for escape is %f s\n", time_spent);

  // check boundary test
  begin = clock();
  for (int i=0; i<ALLOC_SIZE*0x300; i++) {
    int x = rand() % ALLOC_SIZE;
    assert(global_size[x] != 0);
    size_t offset = rand() % global_size[x];
    texas_check_boundary(global[x] + offset, global[x] + offset, MIN_SIZE);
    memset(global[x] + offset, 0, MIN_SIZE);
  }
  end = clock();
  time_spent = (double)(end - begin) / CLOCKS_PER_SEC;
  printf("time spent for check boundary is %f s\n", time_spent);

  // free test
  begin = clock();
  for (int i=0; i<ALLOC_SIZE; i++) {
    tracked_free(global[i]);
  }
  tracked_free(global);
  tracked_free(global_escapes);
  tracked_free(global_size);
  end = clock();
  time_spent = (double)(end - begin) / CLOCKS_PER_SEC;
  printf("time spent for free is %f s\n\n", time_spent);


  time_spent = (double)(end - main_begin) / CLOCKS_PER_SEC;
  printf("total time spent is %f s", time_spent);
}

