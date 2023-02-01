#ifndef JEMALLOC_H
#define JEMALLOC_H

void *je_malloc(unsigned long size);
void je_free(void *ptr);

#endif