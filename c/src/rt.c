#include "rt.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void rt_oom(void) {
  (void)fputs("offside: out of memory\n", stderr);
  exit(1);
}

void *rt_malloc(size_t n) {
  void *p = malloc(n == 0 ? 1 : n);
  if (p == NULL) {
    rt_oom();
  }
  return p;
}

void *rt_realloc(void *p, size_t n) {
  void *q = realloc(p, n == 0 ? 1 : n);
  if (q == NULL) {
    rt_oom();
  }
  return q;
}

void *rt_malloc_array(size_t count, size_t elem) {
  if (elem != 0 && count > SIZE_MAX / elem) {
    rt_oom();
  }
  return rt_malloc(count * elem);
}

typedef struct ArenaChunk {
  struct ArenaChunk *next;
  size_t used;
  size_t cap;
  unsigned char *data;
} ArenaChunk;

struct Arena {
  ArenaChunk *head;
};

static ArenaChunk *chunk_new(size_t cap) {
  ArenaChunk *chunk;
  if (cap < 4096u) {
    cap = 4096u;
  }
  chunk = rt_malloc(sizeof *chunk);
  chunk->data = rt_malloc(cap);
  chunk->next = NULL;
  chunk->used = 0;
  chunk->cap = cap;
  return chunk;
}

Arena *arena_new(void) {
  Arena *arena = rt_malloc(sizeof *arena);
  arena->head = chunk_new(8192u);
  return arena;
}

void arena_free(Arena *arena) {
  ArenaChunk *chunk;
  if (arena == NULL) {
    return;
  }
  chunk = arena->head;
  while (chunk != NULL) {
    ArenaChunk *next = chunk->next;
    free(chunk->data);
    free(chunk);
    chunk = next;
  }
  free(arena);
}

static size_t align_up(size_t value, size_t align) {
  size_t mask = align - 1u;
  if (value > SIZE_MAX - mask) {
    rt_oom();
  }
  return (value + mask) & ~mask;
}

void *arena_alloc(Arena *arena, size_t size, size_t align) {
  ArenaChunk *chunk;
  size_t off;
  if (arena == NULL) {
    rt_oom();
  }
  if (align == 0 || (align & (align - 1u)) != 0) {
    align = sizeof(void *);
  }
  if (size > SIZE_MAX - align) {
    rt_oom();
  }
  chunk = arena->head;
  off = align_up(chunk->used, align);
  if (off > chunk->cap || size > chunk->cap - off) {
    size_t cap = chunk->cap > SIZE_MAX / 2u ? SIZE_MAX : chunk->cap * 2u;
    size_t need = size + align;
    ArenaChunk *fresh;
    if (cap < need) {
      cap = need;
    }
    fresh = chunk_new(cap);
    fresh->next = chunk;
    arena->head = fresh;
    chunk = fresh;
    off = align_up(0, align);
  }
  chunk->used = off + size;
  return chunk->data + off;
}

char *arena_copy(Arena *arena, const char *bytes, size_t n) {
  char *out = arena_alloc(arena, n + 1u, 1u);
  if (n > 0 && bytes != NULL) {
    memcpy(out, bytes, n);
  }
  out[n] = '\0';
  return out;
}

char *arena_cstr(Arena *arena, const char *cstr) {
  if (cstr == NULL) {
    cstr = "";
  }
  return arena_copy(arena, cstr, strlen(cstr));
}

char *arena_printf(Arena *arena, const char *fmt, ...) {
  va_list ap;
  va_list aq;
  int need;
  char *out;
  int wrote;
  va_start(ap, fmt);
  va_copy(aq, ap);
  need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) {
    va_end(aq);
    rt_oom();
  }
  out = arena_alloc(arena, (size_t)need + 1u, 1u);
  wrote = vsnprintf(out, (size_t)need + 1u, fmt, aq);
  va_end(aq);
  if (wrote < 0 || wrote != need) {
    rt_oom();
  }
  return out;
}

char *arena_lower(Arena *arena, const char *bytes, size_t n) {
  char *out = arena_alloc(arena, n + 1u, 1u);
  size_t i;
  for (i = 0; i < n; i++) {
    out[i] = rt_lower_ch(bytes[i]);
  }
  out[n] = '\0';
  return out;
}

char *arena_repeat(Arena *arena, char ch, size_t n) {
  char *out = arena_alloc(arena, n + 1u, 1u);
  if (n > 0) {
    memset(out, ch, n);
  }
  out[n] = '\0';
  return out;
}

char *arena_join(Arena *arena, const char *a, size_t an, const char *b, size_t bn) {
  char *out;
  if (an > SIZE_MAX - bn) {
    rt_oom();
  }
  out = arena_alloc(arena, an + bn + 1u, 1u);
  if (an > 0 && a != NULL) {
    memcpy(out, a, an);
  }
  if (bn > 0 && b != NULL) {
    memcpy(out + an, b, bn);
  }
  out[an + bn] = '\0';
  return out;
}

void buf_init(Buf *buf) {
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

void buf_free(Buf *buf) {
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

void buf_clear(Buf *buf) {
  buf->len = 0;
  if (buf->data != NULL) {
    buf->data[0] = '\0';
  }
}

static void buf_grow(Buf *buf, size_t need) {
  size_t cap;
  if (need <= buf->cap) {
    return;
  }
  cap = buf->cap == 0 ? 64u : buf->cap;
  while (cap < need) {
    if (cap > SIZE_MAX / 2u) {
      rt_oom();
    }
    cap *= 2u;
  }
  buf->data = rt_realloc(buf->data, cap);
  buf->cap = cap;
}

void buf_append(Buf *buf, const char *bytes, size_t n) {
  if (n == 0) {
    return;
  }
  if (buf->len > SIZE_MAX - n - 1u) {
    rt_oom();
  }
  buf_grow(buf, buf->len + n + 1u);
  memcpy(buf->data + buf->len, bytes, n);
  buf->len += n;
  buf->data[buf->len] = '\0';
}

void buf_puts(Buf *buf, const char *cstr) {
  if (cstr == NULL) {
    return;
  }
  buf_append(buf, cstr, strlen(cstr));
}

void buf_putc(Buf *buf, char ch) {
  buf_append(buf, &ch, 1u);
}

void buf_repeat(Buf *buf, char ch, size_t n) {
  size_t i;
  if (n == 0) {
    return;
  }
  if (buf->len > SIZE_MAX - n - 1u) {
    rt_oom();
  }
  buf_grow(buf, buf->len + n + 1u);
  for (i = 0; i < n; i++) {
    buf->data[buf->len + i] = ch;
  }
  buf->len += n;
  buf->data[buf->len] = '\0';
}

char *buf_arena(const Buf *buf, Arena *arena) {
  return arena_copy(arena, buf->data == NULL ? "" : buf->data, buf->len);
}

char *buf_detach(Buf *buf) {
  char *out;
  if (buf->data == NULL) {
    out = rt_malloc(1u);
    out[0] = '\0';
    return out;
  }
  if (buf->len + 1u != buf->cap) {
    buf->data = rt_realloc(buf->data, buf->len + 1u);
  }
  out = buf->data;
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
  return out;
}

bool rt_eq(const char *a, size_t an, const char *b, size_t bn) {
  if (an != bn) {
    return false;
  }
  if (an == 0) {
    return true;
  }
  return memcmp(a, b, an) == 0;
}

bool rt_eq_cstr(const char *a, size_t an, const char *b) {
  if (b == NULL) {
    return an == 0;
  }
  return rt_eq(a, an, b, strlen(b));
}

bool rt_eq_ci(const char *a, size_t an, const char *b) {
  size_t i;
  size_t bn;
  if (b == NULL) {
    return an == 0;
  }
  bn = strlen(b);
  if (an != bn) {
    return false;
  }
  for (i = 0; i < an; i++) {
    if (rt_lower_ch(a[i]) != rt_lower_ch(b[i])) {
      return false;
    }
  }
  return true;
}

bool rt_starts(const char *s, size_t n, const char *prefix) {
  size_t pn = prefix == NULL ? 0 : strlen(prefix);
  if (pn > n) {
    return false;
  }
  return pn == 0 || memcmp(s, prefix, pn) == 0;
}

bool rt_ends(const char *s, size_t n, const char *suffix) {
  size_t sn = suffix == NULL ? 0 : strlen(suffix);
  if (sn > n) {
    return false;
  }
  return sn == 0 || memcmp(s + (n - sn), suffix, sn) == 0;
}

bool rt_has(const char *s, size_t n, const char *needle) {
  size_t nn;
  size_t i;
  if (needle == NULL) {
    return true;
  }
  nn = strlen(needle);
  if (nn == 0) {
    return true;
  }
  if (nn > n || s == NULL) {
    return false;
  }
  for (i = 0; i + nn <= n; i++) {
    if (memcmp(s + i, needle, nn) == 0) {
      return true;
    }
  }
  return false;
}

void rt_trim(const char *s, size_t n, const char **out, size_t *out_n) {
  size_t begin = 0;
  if (s == NULL) {
    *out = "";
    *out_n = 0;
    return;
  }
  while (begin < n && rt_is_space(s[begin])) {
    begin++;
  }
  while (n > begin && rt_is_space(s[n - 1u])) {
    n--;
  }
  *out = s + begin;
  *out_n = n - begin;
}

bool rt_is_digit(char ch) {
  return isdigit((unsigned char)ch) != 0;
}

bool rt_is_alpha(char ch) {
  return isalpha((unsigned char)ch) != 0;
}

bool rt_is_alnum(char ch) {
  return isalnum((unsigned char)ch) != 0;
}

bool rt_is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

char rt_lower_ch(char ch) {
  unsigned char u = (unsigned char)ch;
  if (u >= 'A' && u <= 'Z') {
    return (char)(u - 'A' + 'a');
  }
  return (char)u;
}

char *rt_normalize_newlines(Arena *arena, const char *s, size_t n, size_t *out_n) {
  char *out = arena_alloc(arena, n + 1u, 1u);
  size_t w = 0;
  size_t i = 0;
  if (s == NULL) {
    s = "";
    n = 0;
  }
  while (i < n) {
    if (s[i] == '\r') {
      out[w++] = '\n';
      if (i + 1u < n && s[i + 1u] == '\n') {
        i++;
      }
    } else {
      out[w++] = s[i];
    }
    i++;
  }
  out[w] = '\0';
  *out_n = w;
  return out;
}
