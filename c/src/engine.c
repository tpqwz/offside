#include "engine.h"

#include <string.h>

void note_init(NoteBuf *notes, Arena *arena) {
  notes->items = NULL;
  notes->len = 0;
  notes->cap = 0;
  notes->arena = arena;
}

static void note_grow(NoteBuf *notes) {
  size_t cap = notes->cap == 0 ? 8u : notes->cap * 2u;
  Note *next;
  if (notes->cap != 0 && cap < notes->cap) {
    rt_oom();
  }
  if (cap > SIZE_MAX / sizeof(Note)) {
    rt_oom();
  }
  next = arena_alloc(notes->arena, cap * sizeof(Note), sizeof(void *));
  if (notes->len > 0) {
    memcpy(next, notes->items, notes->len * sizeof(Note));
  }
  notes->items = next;
  notes->cap = cap;
}

void note_push(NoteBuf *notes, int line, const char *message) {
  if (notes->len == notes->cap) {
    note_grow(notes);
  }
  notes->items[notes->len].line = line;
  notes->items[notes->len].message = message;
  notes->len++;
}

void note_unshift(NoteBuf *notes, int line, const char *message) {
  size_t i;
  if (notes->len == notes->cap) {
    note_grow(notes);
  }
  for (i = notes->len; i > 0; i--) {
    notes->items[i] = notes->items[i - 1u];
  }
  notes->items[0].line = line;
  notes->items[0].message = message;
  notes->len++;
}

void result_free(Result *result) {
  if (result == NULL) {
    return;
  }
  arena_free(result->arena);
  result->arena = NULL;
  result->lines = NULL;
  result->code = NULL;
  result->notes = NULL;
  result->nlines = 0;
  result->nnotes = 0;
}
