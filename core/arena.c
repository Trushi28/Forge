#include "arena.h"
#include <stdlib.h>
#include <sys/mman.h>
Arena *arena_new(size_t capacity) {
  if (capacity == 0) return NULL;
  Arena *a = malloc(sizeof(Arena));
  if (!a) return NULL;
  a->base = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (a->base == MAP_FAILED) {
    free(a);
    return NULL;
  }
  a->offset = 0;
  a->capacity = capacity;
  return a;
}
void *arena_alloc(Arena *a, size_t size) {
  if (!a || !a->base || size > SIZE_MAX - 7) return NULL;
  size_t aligned_size = (size + 7) & ~7;
  if (a->offset > a->capacity || aligned_size > a->capacity - a->offset) {
    return NULL;
  }
  void *ptr = a->base + a->offset;
  a->offset += aligned_size;
  return ptr;
}
void arena_reset(Arena *a) { if (a) a->offset = 0; }
void arena_free(Arena *a) {
  if (!a) return;
  munmap(a->base, a->capacity);
  free(a);
}
