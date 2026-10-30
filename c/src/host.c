#include "engine.h"

#include <string.h>

static const char *base_name(const char *path) {
  const char *base = path == NULL ? "" : path;
  const char *p;
  for (p = base; *p != '\0'; p++) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base;
}

static const char *ext_of(const char *path) {
  const char *base = base_name(path);
  const char *dot = NULL;
  const char *p;
  for (p = base; *p != '\0'; p++) {
    if (*p == '.') {
      dot = p;
    }
  }
  if (dot == NULL || dot == base) {
    return "";
  }
  return dot;
}

static bool ext_is(const char *ext, const char *want) {
  size_t i;
  if (ext == NULL || want == NULL) {
    return false;
  }
  for (i = 0; want[i] != '\0'; i++) {
    if (rt_lower_ch(ext[i]) != want[i]) {
      return false;
    }
  }
  return ext[i] == '\0';
}

static bool spaced_ext(const char *ext, LangId *lang) {
  if (ext_is(ext, ".csp")) {
    *lang = LANG_C;
    return true;
  }
  if (ext_is(ext, ".sjava")) {
    *lang = LANG_JAVA;
    return true;
  }
  if (ext_is(ext, ".sjs")) {
    *lang = LANG_JAVASCRIPT;
    return true;
  }
  if (ext_is(ext, ".spas")) {
    *lang = LANG_PASCAL;
    return true;
  }
  return false;
}

static bool host_ext(const char *ext, LangId *lang) {
  if (ext_is(ext, ".c") || ext_is(ext, ".h")) {
    *lang = LANG_C;
    return true;
  }
  if (ext_is(ext, ".java")) {
    *lang = LANG_JAVA;
    return true;
  }
  if (ext_is(ext, ".js") || ext_is(ext, ".mjs") || ext_is(ext, ".cjs")) {
    *lang = LANG_JAVASCRIPT;
    return true;
  }
  if (ext_is(ext, ".pas") || ext_is(ext, ".pp") || ext_is(ext, ".lpr") || ext_is(ext, ".dpr") || ext_is(ext, ".inc")) {
    *lang = LANG_PASCAL;
    return true;
  }
  return false;
}

SourceKind classify_file(const char *path, const char *override_lang) {
  SourceKind kind;
  LangId spaced;
  LangId host;
  bool is_spaced;
  bool is_host;
  const char *ext = ext_of(path);
  kind.ok = true;
  kind.error = NULL;
  kind.host = false;
  kind.lang = LANG_C;
  is_spaced = spaced_ext(ext, &spaced);
  is_host = host_ext(ext, &host);
  if (override_lang != NULL && override_lang[0] != '\0') {
    if (!lang_parse(override_lang, &kind.lang)) {
      kind.ok = false;
      kind.error = "Unknown language. Use c, java, javascript, or pascal.";
      return kind;
    }
  } else if (is_spaced) {
    kind.lang = spaced;
  } else if (is_host) {
    kind.lang = host;
  } else {
    kind.ok = false;
    kind.error = "Cannot tell the language. Use .c/.csp, .java/.sjava, .js/.sjs, .pas/.spas, or pass --lang.";
    return kind;
  }
  kind.host = is_host && !is_spaced;
  return kind;
}

static bool ident_char(char ch) {
  return rt_is_alnum(ch) || ch == '_' || ch == '$';
}

static const char *java_class_name(Arena *arena, const char *code, const char *fallback) {
  const char *p = code == NULL ? "" : code;
  while (*p != '\0') {
    if ((p == code || !ident_char(p[-1])) && strncmp(p, "class", 5) == 0 && !ident_char(p[5])) {
      const char *q = p + 5;
      const char *name;
      size_t n = 0;
      while (rt_is_space(*q)) {
        q++;
      }
      name = q;
      while (ident_char(*q)) {
        q++;
        n++;
      }
      if (n > 0) {
        return arena_copy(arena, name, n);
      }
    }
    p++;
  }
  return fallback;
}

static const char *stem_of(Arena *arena, const char *path) {
  const char *base = base_name(path);
  const char *dot = NULL;
  const char *p;
  size_t n;
  for (p = base; *p != '\0'; p++) {
    if (*p == '.') {
      dot = p;
    }
  }
  n = (dot == NULL || dot == base) ? strlen(base) : (size_t)(dot - base);
  return arena_copy(arena, base, n);
}

const char *host_file_name(Arena *arena, LangId lang, const char *source_path, const char *code) {
  const char *stem = stem_of(arena, source_path);
  if (lang == LANG_JAVA) {
    return arena_printf(arena, "%s.java", java_class_name(arena, code, stem));
  }
  if (lang == LANG_JAVASCRIPT) {
    return arena_printf(arena, "%s.js", stem);
  }
  if (lang == LANG_PASCAL) {
    return arena_printf(arena, "%s.pas", stem);
  }
  return arena_printf(arena, "%s.c", stem);
}

static bool continues_line(const char *text) {
  size_t n;
  if (text == NULL) {
    return false;
  }
  n = strlen(text);
  return n > 0 && text[n - 1u] == '\\';
}

static char *c_quote(Arena *arena, const char *value) {
  Buf buf;
  const char *p;
  buf_init(&buf);
  buf_putc(&buf, '"');
  for (p = value == NULL ? "" : value; *p != '\0'; p++) {
    if (*p == '\\' || *p == '"') {
      buf_putc(&buf, '\\');
    }
    buf_putc(&buf, *p);
  }
  buf_putc(&buf, '"');
  {
    char *out = buf_arena(&buf, arena);
    buf_free(&buf);
    return out;
  }
}

static const char *join_line(Arena *arena, const OutLine *line) {
  Buf buf;
  size_t i;
  char *out;
  if (line->nsegs == 1 && line->segs != NULL) {
    return line->segs[0].text;
  }
  buf_init(&buf);
  for (i = 0; i < line->nsegs; i++) {
    if (line->segs[i].text != NULL) {
      buf_puts(&buf, line->segs[i].text);
    }
  }
  out = buf_arena(&buf, arena);
  buf_free(&buf);
  return out;
}

void render_host(Arena *arena, const Result *result, const char *source_name, bool line_directives, HostFile *out) {
  Buf buf;
  size_t i;
  bool use_line;
  bool open_cont = false;
  size_t map_len = 1;
  int *map;
  memset(out, 0, sizeof *out);
  use_line = line_directives && result->language == LANG_C;
  for (i = 0; i < result->nlines; i++) {
    map_len++;
    if (use_line) {
      map_len++;
    }
  }
  map = arena_alloc(arena, map_len * sizeof(int), sizeof(int));
  for (i = 0; i < map_len; i++) {
    map[i] = 0;
  }
  buf_init(&buf);
  map_len = 1;
  for (i = 0; i < result->nlines; i++) {
    const char *text = join_line(arena, &result->lines[i]);
    if (use_line && !open_cont) {
      buf_puts(&buf, "#line ");
      buf_puts(&buf, arena_printf(arena, "%d", result->lines[i].source_line));
      buf_putc(&buf, ' ');
      buf_puts(&buf, c_quote(arena, source_name));
      buf_putc(&buf, '\n');
      map[map_len++] = result->lines[i].source_line;
    }
    buf_puts(&buf, text);
    buf_putc(&buf, '\n');
    map[map_len++] = result->lines[i].source_line;
    open_cont = continues_line(text);
  }
  out->code = buf.len == 0 ? arena_cstr(arena, "") : buf_arena(&buf, arena);
  out->line_map = map;
  out->nmap = map_len;
  buf_free(&buf);
}

static const char *after_digits(const char *p, int *value) {
  int acc = 0;
  const char *start = p;
  if (!rt_is_digit(*p)) {
    return NULL;
  }
  while (rt_is_digit(*p)) {
    acc = acc * 10 + (*p - '0');
    p++;
  }
  if (p == start) {
    return NULL;
  }
  *value = acc;
  return p;
}

char *remap_diagnostics(Arena *arena, const char *text, const char *generated_name, const char *source_name, const int *line_map, size_t nmap) {
  const char *names[2];
  size_t nnames = 0;
  const char *base;
  Buf buf;
  const char *cursor;
  size_t i;
  if (text == NULL) {
    text = "";
  }
  names[nnames++] = generated_name == NULL ? "" : generated_name;
  base = base_name(names[0]);
  if (base[0] != '\0' && strcmp(base, names[0]) != 0) {
    names[nnames++] = base;
  }
  buf_init(&buf);
  cursor = text;
  while (*cursor != '\0') {
    bool hit = false;
    for (i = 0; i < nnames; i++) {
      size_t nlen = strlen(names[i]);
      if (nlen == 0 || strncmp(cursor, names[i], nlen) != 0) {
        continue;
      }
      if (cursor[nlen] == ':' || cursor[nlen] == '(') {
        int line = 0;
        const char *rest = after_digits(cursor + nlen + 1, &line);
        char sep = cursor[nlen];
        if (rest != NULL) {
          int mapped = (line >= 0 && (size_t)line < nmap) ? line_map[line] : line;
          if (mapped == 0) {
            mapped = line;
          }
          buf_puts(&buf, source_name);
          buf_putc(&buf, sep);
          buf_puts(&buf, arena_printf(arena, "%d", mapped));
          cursor = rest;
          hit = true;
          break;
        }
      }
    }
    if (!hit) {
      buf_putc(&buf, *cursor);
      cursor++;
    }
  }
  {
    char *out = buf_arena(&buf, arena);
    buf_free(&buf);
    return out;
  }
}
