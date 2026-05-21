#ifndef FORGE_ARENA_H
#define FORGE_ARENA_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
  uint8_t *base;
  size_t offset;
  size_t capacity;
} Arena;
Arena *arena_new(size_t capacity);
void *arena_alloc(Arena *a, size_t size);
void arena_reset(Arena *a);
void arena_free(Arena *a);

#endif
