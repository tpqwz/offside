/* Runtime for the native offside converter.
   Held to the SEI CERT C Coding Standard (Robert C. Seacord) and the
   rules that became ISO/IEC TS 17961. In particular:
   STR31-C, STR32-C, STR37-C  bounded copies, explicit lengths, ctype on unsigned char
   MEM30-C, MEM31-C, MEM34-C  one owner for each block; realloc failure keeps the old pointer
   INT30-C                    size arithmetic is checked before it wraps
   ERR33-C                    allocation failure aborts the process
   EXP34-C, ARR30-C           no null dereference, indexes stay inside the buffer
   ENV33-C                    the CLI never calls system()
   MSC24-C                    no gets, strcpy, strcat, sprintf, or alloca
   No variable-length arrays.
*/
#ifndef OFFSIDE_RT_H
#define OFFSIDE_RT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

void rt_oom(void);
void *rt_malloc(size_t n);
void *rt_realloc(void *p, size_t n);
void *rt_malloc_array(size_t count, size_t elem);

typedef struct Arena Arena;

Arena *arena_new(void);
void arena_free(Arena *arena);
void *arena_alloc(Arena *arena, size_t size, size_t align);
char *arena_copy(Arena *arena, const char *bytes, size_t n);
char *arena_cstr(Arena *arena, const char *cstr);
char *arena_printf(Arena *arena, const char *fmt, ...);
char *arena_lower(Arena *arena, const char *bytes, size_t n);
char *arena_repeat(Arena *arena, char ch, size_t n);
char *arena_join(Arena *arena, const char *a, size_t an, const char *b, size_t bn);

typedef struct Buf {
  char *data;
  size_t len;
  size_t cap;
} Buf;

void buf_init(Buf *buf);
void buf_free(Buf *buf);
void buf_clear(Buf *buf);
void buf_append(Buf *buf, const char *bytes, size_t n);
void buf_puts(Buf *buf, const char *cstr);
void buf_putc(Buf *buf, char ch);
void buf_repeat(Buf *buf, char ch, size_t n);
char *buf_arena(const Buf *buf, Arena *arena);
char *buf_detach(Buf *buf);

bool rt_eq(const char *a, size_t an, const char *b, size_t bn);
bool rt_eq_cstr(const char *a, size_t an, const char *b);
bool rt_eq_ci(const char *a, size_t an, const char *b);
bool rt_starts(const char *s, size_t n, const char *prefix);
bool rt_ends(const char *s, size_t n, const char *suffix);
bool rt_has(const char *s, size_t n, const char *needle);
void rt_trim(const char *s, size_t n, const char **out, size_t *out_n);
bool rt_is_digit(char ch);
bool rt_is_alpha(char ch);
bool rt_is_alnum(char ch);
bool rt_is_space(char ch);
char rt_lower_ch(char ch);

char *rt_normalize_newlines(Arena *arena, const char *s, size_t n, size_t *out_n);

#endif
