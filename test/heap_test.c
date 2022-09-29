#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <time.h>

int __gep_check_boundary(void *, void *, size_t);

#define MAX_SIZE 0x300000
#define MIN_SIZE 0x30000

#define ARRAY_SIZE 0x1000

int main() {
  setbuf(stdin, 0);
  setbuf(stdout, 0);
  setbuf(stderr, 0);

  void *data = malloc(MAX_SIZE);
  printf("Heap allocated at [%p, %p] size %lx\n", data, data+MAX_SIZE, MAX_SIZE);

  for (size_t i=0x1fff; i<MAX_SIZE; i++) {
    size_t offset = i;
    size_t tail = MAX_SIZE - offset;
    assert(data+offset <= data+MAX_SIZE);
    assert(data+offset >= data);
    __gep_check_boundary(data+offset, data+offset, tail-1);
  }
}
