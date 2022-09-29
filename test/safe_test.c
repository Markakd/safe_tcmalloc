#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#define MEM_SIZE 0x1

void *ptr[MEM_SIZE];
void *ptr_escape[MEM_SIZE];

int __check_boundary(void *, void *, size_t);

void test_invalid_free() {
  void *p;
  p = malloc(80);
  printf("allocated p at %p\n", p);
  free(p+0x10);

  p = malloc(0x200000000);
  printf("allocated p at %p\n", p);
  free(p+0x20);
  free(p+0x2000);
  free(p + 0x200000000 - 0x3000);
}

void test_check_boundary() {
  char *p;

  p = malloc(75);
  printf("got ptr %p\n", p);
  // this should be ok
  assert(__check_boundary(p, p, 80) == 0);
  // p[79] = 1;

  assert(__check_boundary(p, p-3, 5) == -1);
  assert(__check_boundary(p, p-4, 1) == -1);

  // this should fail
  assert(__check_boundary(p, p-1, 1) == -1);
  // p[-1] = 0;

  assert(__check_boundary(p, p, 81) == -1);
  // p[80] = 1;
  free(p);

  p = malloc(0x2000);
  assert(__check_boundary(p, p, 0x2001) == -1);
  // p[0x2000] = 1;
  free(p);
}

// escape stored on stack
// then stack being reused
void test_escape_0() {
  void *p;
  
  void *tmp = malloc(80);
  __escape(&p, tmp);
  p = tmp;

  // stack then being reused without notifying escape
  p = (void*) 0x112233;
  free(tmp);
  assert(p == (void*)0x112233);

  tmp = malloc(80);
  __escape(&p, tmp);
  p = tmp;
  free(p);
  assert(p == 0xdeadbeefdeadbeef);
}

// escape being overwritten
void test_escape_1() {
  void *tmp_1, *tmp_2;

  tmp_1 = malloc(80);
  __escape(&ptr[0], tmp_1);
  ptr[0] = tmp_1;
  
  // the escape from ptr0 to tmp 1 should be removed
  tmp_2 = malloc(0);
  
  __escape(&ptr[0], tmp_2);
  ptr[0] = tmp_2;
  
  // now free tmp 1
  // ptr 0 should not being poisoned
  free(tmp_1);
  assert(ptr[0] == tmp_2);
  free(ptr[0]);
}

void test_escape_2() {
  void **mem = malloc(80);

  void *tmp = malloc(80);
  __escape(&mem[0], tmp);
  mem[0] = tmp;

  free(mem);
  // reclaim freed mem
  char *data = malloc(80);
  memset(data, 'A', 80);

  // this should not poison mem, which is being freed and reclaimed
  free(tmp);
  assert(*(unsigned long*)data == 0x4141414141414141);
  free(data);
}

void test_escape_3() {
  void *tmp_1, *tmp_2;

  tmp_1 = malloc(80);
  tmp_2 = malloc(80);

  ptr[0] = tmp_1;

  // the escape from ptr0 to tmp 1 should be removed
  // it is removing a non-existing escape
  __escape(&ptr[0], tmp_2);
  ptr[0] = tmp_2;

  // now free tmp 1
  // ptr 0 should not being poisoned
  free(tmp_1);
  free(tmp_2);
}


#define ALLOC_SIZE 0x1000
#define ROUND 0x1000

void test_escape_fuzz() {
  printf("RUNNING test_escape_fuzz\n");
  void **table = malloc(ALLOC_SIZE*sizeof(void *));

  for (int i=0; i<ALLOC_SIZE; i++) {
    void *tmp = malloc(128);
    assert(tmp != 0);
    __escape(&table[i], tmp);
    table[i] = tmp;
  }

  for (int i=0; i<ALLOC_SIZE * ROUND; i++) {
    int x = rand() % ALLOC_SIZE;
    int y = rand() % ALLOC_SIZE;
    if (table[x]) {
      __escape(table[x], table[y]);
      *(void **)table[x] = table[y];
    }

    if (rand() % ROUND == 0) {
      int z = rand() % ALLOC_SIZE;
      void *tmp = table[z];
      table[z] = 0;
      __escape(&table[z], 0);
      free(tmp);
    }
  }

  for (int i=0; i<ALLOC_SIZE; i++) {
    if (table[i]) free(table[i]);
  }
  printf("FINISHING test_escape_fuzz\n");
}

int main () {
  test_check_boundary();
  test_escape_0();
  test_escape_1();
  test_escape_2();
  test_escape_3();
  test_escape_fuzz();
  test_invalid_free();

  __report_statistic();
}

