#include "arena.h"
#include <stdlib.h>
#include <sys/mman.h>
Arena *arena_new(size_t capacity) {
  Arena *a = malloc(sizeof(Arena));
  a->base = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  a->offset = 0;
  a->capacity = capacity;
  return a;
}
void *arena_alloc(Arena *a, size_t size) {
  size_t aligned_size = (size + 7) & ~7;
  if (a->offset + aligned_size > a->capacity) {
    return NULL;
  }
  void *ptr = a->base + a->offset;
  a->offset += aligned_size;
  return ptr;
}
void arena_reset(Arena *a) { a->offset = 0; }
void arena_free(Arena *a) {
  munmap(a->base, a->capacity);
  free(a);
}
