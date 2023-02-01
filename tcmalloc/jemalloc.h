#ifndef JEMALLOC_H
#define JEMALLOC_H

extern "C" void *je_malloc(unsigned long size);
extern "C" void je_free(void *ptr);

#endif