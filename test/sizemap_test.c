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



#define ALLOC_AMOUNT 0x30000
#define MAX_SIZE 0x800000
#define MIN_SIZE 0x10

int main() {
  clock_t main_begin, begin, end;
  double time_spent;

  // pesudo random
  srand(0);

  main_begin = clock();
  void **global = tracked_malloc(ALLOC_AMOUNT * sizeof(void *));
  size_t *global_size = tracked_malloc(ALLOC_AMOUNT * sizeof(size_t));

  assert(global != 0);
  assert(global_size != 0);
  
  // allocation test
  for (int i=0; i<ALLOC_AMOUNT; i++) {
    size_t size = (rand() % MAX_SIZE) + MIN_SIZE;
    global[i] = tracked_malloc(size);
    assert(global[i] != 0);
    global_size[i] = size;
  }
  end = clock();
  time_spent = (double)(end - main_begin) / CLOCKS_PER_SEC;
  printf("time spent for allocation is %f s\n", time_spent);


  // random test
  begin = clock();
  for (int i=0; i<ALLOC_AMOUNT*0x10; i++) {
    int x = rand() % ALLOC_AMOUNT;
    if (global_size[x] == 0) {
      size_t size = (rand() % MAX_SIZE) + MIN_SIZE;
      global[x] = tracked_malloc(size);
      global_size[x] = size;
    }
    size_t offset = rand() % global_size[x];
    int res = texas_check_boundary(global[x] + offset, global[x] + offset, global_size[x] - offset);
    if (res < 0) {
      printf("range %p - %p, size %lx\n", global[x], global[x] + global_size[x], global_size[x]);
      printf("base %p access %lx\n", global[x]+offset, global_size[x]-offset);
    }
    
    // random free something
    int y = rand() % ALLOC_AMOUNT;
    if (global[y] == 0) continue;
    tracked_free(global[y]);
    global[y] = global_size[y] = 0;
  }
  end = clock();
  time_spent = (double)(end - begin) / CLOCKS_PER_SEC;
  printf("time spent for check boundary is %f s\n", time_spent);

  // free test
  begin = clock();
  for (int i=0; i<ALLOC_AMOUNT; i++) {
    // if (i % 0x1000 == 0) printf("%x\n", i);
    if (global[i] == 0) continue;
    tracked_free(global[i]);
  }
  tracked_free(global);
  tracked_free(global_size);
  end = clock();
  time_spent = (double)(end - begin) / CLOCKS_PER_SEC;
  printf("time spent for free is %f s\n\n", time_spent);


  time_spent = (double)(end - main_begin) / CLOCKS_PER_SEC;
  printf("total time spent is %f s", time_spent);
}

