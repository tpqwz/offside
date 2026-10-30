#include "engine.h"

#include <string.h>

/* Behavior-preserving C11 translation of src/lib/layout/pascal.ts. */

typedef struct StrVec {
  const char **items;
  size_t len;
  size_t cap;
} StrVec;

typedef struct AfterItem {
  int col;
  const char *text;
} AfterItem;

typedef struct AfterVec {
  AfterItem *items;
  size_t len;
  size_t cap;
} AfterVec;

typedef struct PascalBack {
  Arena *arena;
  const char *source;
  size_t sourcelen;
  const Tok *tokens;
  size_t ntokens;
  Tok *code;
  size_t ncode;
  NoteBuf notes;
  StrVec out;
  StrVec trivia_lines;
  AfterVec after_lines;
  StrVec pending_after;
  int pending_epoch;
  size_t i;
  size_t mark;
  size_t trivia;
} PascalBack;

typedef enum DirKind {
  DK_NONE = 0,
  DK_OPEN,
  DK_MID,
  DK_CLOSE
} DirKind;

typedef struct WrapFrame {
  int col;
  int mode;
  int depth;
} WrapFrame;

typedef struct WrapStack {
  WrapFrame *items;
  size_t len;
  size_t cap;
} WrapStack;

static const char *const ROUTINE[] = {
  "procedure", "function", "constructor", "destructor", "operator", NULL
};
static const char *const TYPE_BODY[] = {
  "record", "object", "class", "interface", "dispinterface",
  "objcclass", "objccategory", "objcprotocol", NULL
};
static const char *const SECTION[] = {
  "var", "type", "const", "label", "threadvar", "resourcestring", NULL
};
static const char *const VISIBILITY[] = {
  "public", "private", "protected", "published", "automated", NULL
};
static const char *const MODIFIER[] = {
  "forward", "external", "overload", "inline", "noinline", "noreturn", "nostackframe",
  "virtual", "abstract", "override", "reintroduce", "static", "cdecl", "stdcall", "pascal",
  "register", "safecall", "assembler", "message", "deprecated", "platform", "far", "near",
  "export", "name", "saveregisters", "interrupt", "varargs", "alias", "cppdecl", "mwpascal",
  "hardfloat", "softfloat", "iocheck", "extdecl", NULL
};
static const char *const NO_BODY[] = { "forward", "external", "abstract", NULL };
static const char *const HARD_STOP[] = {
  "end", "until", "except", "finally", "else", "otherwise", NULL
};
static const char *const STOP_END[] = { "end", NULL };
static const char *const STOP_UNTIL[] = { "until", NULL };
static const char *const STOP_NONE[] = { NULL };
static const char *const EXPR_WORDS[] = {
  "or", "and", "xor", "div", "mod", "shl", "shr", "not", "in", "is", "as", "to", "downto", NULL
};
static const char *const ASSIGN_SYMS[] = {
  ":=", "=", "<", ">", "<=", ">=", "<>", "+", "-", "*", "/", ".", "^", ",", "(", "[", NULL
};
static const char *const DECL_SYMS[] = { "^", "(", "[", "+", "-", "@", NULL };
static const char *const DECL_WORDS[] = {
  "not", "nil", "array", "set", "file", "string", "packed", "specialize", "type", NULL
};
static const char *const CASE_STMT[] = {
  "if", "for", "while", "repeat", "try", "case", "with", "asm", "begin", "raise", "goto", NULL
};
static const char *const MEMBER_CLASS[] = { "var", "property", "const", "threadvar", NULL };

static bool str_eq(const char *a, const char *b);
static bool is_one_of(const char *word, const char *const *list);
static bool is_hard_stop(const char *word);
static bool is_section(const char *word);
static bool is_routine(const char *word);
static bool ends_section(const char *word);
static const Tok *pb_code_at(const PascalBack *pb, size_t index);
static const char *pb_word_at(const PascalBack *pb, size_t index);
static const char *pb_word(const PascalBack *pb);
static bool pb_at(const PascalBack *pb, const char *word);
static bool pb_cur_is(const PascalBack *pb, const char *text);
static const char *pb_cur_text(const PascalBack *pb);
static int pb_line_no(const PascalBack *pb);
static bool pb_starts_routine(PascalBack *pb, size_t at);
static void pb_pull_until(PascalBack *pb, size_t until);
static void pb_pull(PascalBack *pb);
static void pb_flush_trivia(PascalBack *pb, int indent);
static void pb_flush_pending(PascalBack *pb, int indent);
static void pb_line(PascalBack *pb, int indent, const char *text);
static void pb_inline_directives(PascalBack *pb, size_t from_pos, size_t to_pos, StrVec *out);
static const char *pb_flat(PascalBack *pb, size_t from, size_t to, bool trail);
static int pb_conditional_depth(const PascalBack *pb, size_t from_pos, size_t to_pos);
static bool pb_endif_before_semi(const PascalBack *pb, size_t from, size_t semi_index);
static bool pb_same_decl_arm(const PascalBack *pb, size_t from, size_t semi_index);
static bool pb_statement_boundary(PascalBack *pb, size_t from, size_t index);
static void pb_park_boundary(PascalBack *pb, size_t index);
static const char *pb_take_until(PascalBack *pb, const char *const *stops);
static const char *pb_take_semi(PascalBack *pb);
static const char *pb_take_balanced(PascalBack *pb);
static void pb_consume_end(PascalBack *pb);
static void pb_parse_sequence(PascalBack *pb, int indent, const char *const *stops, bool bodies);
static void pb_parse_item(PascalBack *pb, int indent, bool bodies);
static void pb_parse_section(PascalBack *pb, int indent, bool bodies, bool (*stop_here)(PascalBack *));
static void pb_parse_type_decl(PascalBack *pb, int indent);
static void pb_parse_members(PascalBack *pb, int indent);
static void pb_parse_routine(PascalBack *pb, int indent, bool bodies);
static void pb_parse_asm(PascalBack *pb, int indent);
static void pb_parse_repeat(PascalBack *pb, int indent, bool bodies);
static void pb_parse_try(PascalBack *pb, int indent, bool bodies);
static void pb_parse_case(PascalBack *pb, int indent, bool bodies);
static void pb_parse_headed(PascalBack *pb, int indent, bool bodies);
static void pb_parse_cue_body(PascalBack *pb, int indent, bool bodies, size_t cue_end);
static void pb_unwrap_arm(PascalBack *pb, int indent, bool bodies, bool empty_as_semi);
static void pb_prefix_else(PascalBack *pb, int indent);
static bool pb_spread_init(PascalBack *pb, int indent, const char *text);
static bool pb_member_stop(PascalBack *pb);

static bool str_eq(const char *a, const char *b) {
  if (a == NULL) {
    a = "";
  }
  if (b == NULL) {
    b = "";
  }
  return strcmp(a, b) == 0;
}

static bool is_one_of(const char *word, const char *const *list) {
  size_t i;
  if (word == NULL || word[0] == '\0' || list == NULL) {
    return false;
  }
  for (i = 0; list[i] != NULL; i++) {
    if (strcmp(word, list[i]) == 0) {
      return true;
    }
  }
  return false;
}

static bool tok_text_is(const Tok *tok, const char *text) {
  if (tok == NULL || tok->text == NULL) {
    return false;
  }
  return rt_eq_cstr(tok->text, tok->len, text);
}

static bool tok_word_is(const Tok *tok, const char *word) {
  if (tok == NULL || tok->text == NULL) {
    return false;
  }
  return rt_eq_ci(tok->text, tok->len, word);
}

static bool is_hard_stop(const char *word) {
  return is_one_of(word, HARD_STOP);
}

static bool is_section(const char *word) {
  return is_one_of(word, SECTION);
}

static bool is_routine(const char *word) {
  return is_one_of(word, ROUTINE);
}

static bool is_type_body(const char *word) {
  return is_one_of(word, TYPE_BODY);
}

static bool is_visibility(const char *word) {
  return is_one_of(word, VISIBILITY);
}

static bool is_modifier(const char *word) {
  return is_one_of(word, MODIFIER);
}

static bool is_no_body(const char *word) {
  return is_one_of(word, NO_BODY);
}

static bool ends_section(const char *word) {
  return str_eq(word, "begin") || str_eq(word, "end") || str_eq(word, "until")
    || str_eq(word, "except") || str_eq(word, "finally") || str_eq(word, "else")
    || str_eq(word, "otherwise") || str_eq(word, "implementation") || str_eq(word, "interface")
    || str_eq(word, "initialization") || str_eq(word, "finalization") || str_eq(word, "uses")
    || str_eq(word, "program") || str_eq(word, "unit") || str_eq(word, "library")
    || is_section(word) || is_routine(word) || str_eq(word, "generic") || str_eq(word, "class")
    || str_eq(word, "if") || str_eq(word, "for") || str_eq(word, "while") || str_eq(word, "repeat")
    || str_eq(word, "try") || str_eq(word, "case") || str_eq(word, "asm") || str_eq(word, "with");
}

static bool is_expr_word(const char *word) {
  return is_one_of(word, EXPR_WORDS);
}

static bool is_blank_text(const char *s) {
  size_t i;
  if (s == NULL) {
    return true;
  }
  for (i = 0; s[i] != '\0'; i++) {
    if (!rt_is_space(s[i])) {
      return false;
    }
  }
  return true;
}

static bool is_hex_ch(char ch) {
  unsigned char u = (unsigned char)ch;
  return rt_is_digit(ch) || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

static bool is_caret_class(char ch) {
  return rt_is_alpha(ch) || ch == '@' || ch == '[' || ch == ']' || ch == '\\' || ch == '^' || ch == '_';
}

static bool is_pascal_literal(const char *s, size_t n) {
  size_t i = 0;
  if (s == NULL || n == 0) {
    return false;
  }
  while (i < n) {
    char ch = s[i];
    if (ch == '#') {
      i++;
      if (i >= n) {
        return false;
      }
      if (s[i] == '$') {
        size_t start;
        i++;
        start = i;
        while (i < n && is_hex_ch(s[i])) {
          i++;
        }
        if (i == start) {
          return false;
        }
      } else if (s[i] == '%') {
        size_t start;
        i++;
        start = i;
        while (i < n && (s[i] == '0' || s[i] == '1')) {
          i++;
        }
        if (i == start) {
          return false;
        }
      } else if (rt_is_digit(s[i])) {
        while (i < n && rt_is_digit(s[i])) {
          i++;
        }
      } else {
        return false;
      }
    } else if (ch == '\'' || ch == '"') {
      char quote = ch;
      bool closed = false;
      i++;
      while (i < n) {
        if (s[i] == quote) {
          if (i + 1u < n && s[i + 1u] == quote) {
            i += 2u;
            continue;
          }
          i++;
          closed = true;
          break;
        }
        i++;
      }
      if (!closed) {
        return false;
      }
    } else if (ch == '^') {
      if (i + 1u >= n || !is_caret_class(s[i + 1u])) {
        return false;
      }
      i += 2u;
    } else {
      return false;
    }
  }
  return true;
}

static bool has_blank_line(const char *s, size_t n) {
  size_t i;
  if (s == NULL) {
    return false;
  }
  for (i = 0; i + 1u < n; i++) {
    if (s[i] == '\n') {
      size_t j = i + 1u;
      while (j < n && (s[j] == ' ' || s[j] == '\t')) {
        j++;
      }
      if (j < n && s[j] == '\n') {
        return true;
      }
    }
  }
  return false;
}

static bool word_boundary_at(const char *s, size_t n, size_t i) {
  char ch;
  if (i >= n) {
    return true;
  }
  ch = s[i];
  return !(rt_is_alnum(ch) || ch == '_');
}

static bool dir_keyword(const char *s, size_t n, size_t i, const char *kw) {
  size_t kn = strlen(kw);
  size_t k;
  if (i > n || kn > n - i) {
    return false;
  }
  for (k = 0; k < kn; k++) {
    if (rt_lower_ch(s[i + k]) != kw[k]) {
      return false;
    }
  }
  return word_boundary_at(s, n, i + kn);
}

static DirKind classify_dir(const char *text, size_t n) {
  const char *s;
  size_t sn;
  size_t i;
  rt_trim(text, n, &s, &sn);
  if (sn < 2u || s[0] != '{' || s[1] != '$') {
    return DK_NONE;
  }
  i = 2u;
  while (i < sn && rt_is_space(s[i])) {
    i++;
  }
  if (dir_keyword(s, sn, i, "ifndef") || dir_keyword(s, sn, i, "ifdef")
      || dir_keyword(s, sn, i, "ifopt") || dir_keyword(s, sn, i, "ifc")
      || dir_keyword(s, sn, i, "if")) {
    return DK_OPEN;
  }
  if (dir_keyword(s, sn, i, "elseif") || dir_keyword(s, sn, i, "else")) {
    return DK_MID;
  }
  if (dir_keyword(s, sn, i, "ifend") || dir_keyword(s, sn, i, "endif")) {
    return DK_CLOSE;
  }
  return DK_NONE;
}

static DirKind tok_dir_kind(const Tok *tok) {
  if (tok == NULL || tok->text == NULL || tok->kind != TOK_DIRECTIVE) {
    return DK_NONE;
  }
  return classify_dir(tok->text, tok->len);
}

static const char *trim_copy(Arena *arena, const char *s, size_t n) {
  const char *t;
  size_t tn;
  rt_trim(s == NULL ? "" : s, s == NULL ? 0u : n, &t, &tn);
  return arena_copy(arena, t, tn);
}

static const char *collapse_ws_trim(Arena *arena, const char *s) {
  Buf buf;
  size_t i;
  size_t n;
  bool in_ws = false;
  bool any = false;
  char *out;
  if (s == NULL) {
    s = "";
  }
  n = strlen(s);
  buf_init(&buf);
  for (i = 0; i < n; i++) {
    if (rt_is_space(s[i])) {
      in_ws = true;
      continue;
    }
    if (any && in_ws) {
      buf_putc(&buf, ' ');
    }
    in_ws = false;
    any = true;
    buf_putc(&buf, s[i]);
  }
  out = buf_arena(&buf, arena);
  buf_free(&buf);
  return out;
}

static const char *join_space(Arena *arena, const StrVec *parts) {
  Buf buf;
  size_t i;
  char *out;
  if (parts == NULL || parts->len == 0) {
    return "";
  }
  buf_init(&buf);
  for (i = 0; i < parts->len; i++) {
    if (i > 0) {
      buf_putc(&buf, ' ');
    }
    buf_puts(&buf, parts->items[i] == NULL ? "" : parts->items[i]);
  }
  out = buf_arena(&buf, arena);
  buf_free(&buf);
  return out;
}

static void strvec_init(StrVec *v) {
  v->items = NULL;
  v->len = 0;
  v->cap = 0;
}

static void strvec_push(Arena *arena, StrVec *v, const char *s) {
  if (v->len == v->cap) {
    size_t cap = v->cap == 0 ? 8u : v->cap * 2u;
    const char **next;
    if (v->cap != 0 && cap < v->cap) {
      rt_oom();
    }
    if (cap > SIZE_MAX / sizeof(const char *)) {
      rt_oom();
    }
    next = arena_alloc(arena, cap * sizeof(const char *), sizeof(void *));
    if (v->len > 0) {
      memcpy(next, v->items, v->len * sizeof(const char *));
    }
    v->items = next;
    v->cap = cap;
  }
  v->items[v->len++] = s == NULL ? "" : s;
}

static void strvec_splice_front(StrVec *v, size_t n) {
  if (n >= v->len) {
    v->len = 0;
    return;
  }
  memmove(v->items, v->items + n, (v->len - n) * sizeof(const char *));
  v->len -= n;
}

static const char **strvec_copy_front(Arena *arena, StrVec *v, size_t n, size_t *out_n) {
  const char **copy;
  size_t i;
  if (n > v->len) {
    n = v->len;
  }
  *out_n = n;
  if (n == 0) {
    return NULL;
  }
  if (n > SIZE_MAX / sizeof(const char *)) {
    rt_oom();
  }
  copy = arena_alloc(arena, n * sizeof(const char *), sizeof(void *));
  for (i = 0; i < n; i++) {
    copy[i] = v->items[i];
  }
  strvec_splice_front(v, n);
  return copy;
}

static void after_init(AfterVec *v) {
  v->items = NULL;
  v->len = 0;
  v->cap = 0;
}

static void after_push(Arena *arena, AfterVec *v, int col, const char *text) {
  if (v->len == v->cap) {
    size_t cap = v->cap == 0 ? 8u : v->cap * 2u;
    AfterItem *next;
    if (v->cap != 0 && cap < v->cap) {
      rt_oom();
    }
    if (cap > SIZE_MAX / sizeof(AfterItem)) {
      rt_oom();
    }
    next = arena_alloc(arena, cap * sizeof(AfterItem), sizeof(void *));
    if (v->len > 0) {
      memcpy(next, v->items, v->len * sizeof(AfterItem));
    }
    v->items = next;
    v->cap = cap;
  }
  v->items[v->len].col = col;
  v->items[v->len].text = text == NULL ? "" : text;
  v->len++;
}

static const char *join_pascal(Arena *arena, const StrVec *parts) {
  StrVec out;
  size_t i;
  Buf buf;
  char *joined;
  if (parts == NULL) {
    return "";
  }
  strvec_init(&out);
  for (i = 0; i < parts->len; i++) {
    const char *part = parts->items[i];
    size_t n;
    if (part == NULL || part[0] == '\0') {
      continue;
    }
    n = strlen(part);
    if (out.len > 0 && is_pascal_literal(out.items[out.len - 1u], strlen(out.items[out.len - 1u]))
        && is_pascal_literal(part, n)) {
      out.items[out.len - 1u] = arena_join(arena, out.items[out.len - 1u], strlen(out.items[out.len - 1u]), part, n);
    } else {
      strvec_push(arena, &out, part);
    }
  }
  buf_init(&buf);
  for (i = 0; i < out.len; i++) {
    if (i > 0) {
      buf_putc(&buf, ' ');
    }
    buf_puts(&buf, out.items[i]);
  }
  joined = buf_arena(&buf, arena);
  buf_free(&buf);
  return collapse_ws_trim(arena, joined);
}

static void split_top_commas(Arena *arena, const char *inner, size_t n, StrVec *parts) {
  size_t start = 0;
  int paren = 0;
  int bracket = 0;
  char quote = 0;
  size_t i;
  strvec_init(parts);
  if (inner == NULL) {
    inner = "";
    n = 0;
  }
  for (i = 0; i < n; i++) {
    char ch = inner[i];
    if (quote != 0) {
      if (ch == quote) {
        if (quote == '\'' && i + 1u < n && inner[i + 1u] == '\'') {
          i++;
        } else {
          quote = 0;
        }
      }
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
    } else if (ch == '(') {
      paren++;
    } else if (ch == ')' && paren > 0) {
      paren--;
    } else if (ch == '[') {
      bracket++;
    } else if (ch == ']' && bracket > 0) {
      bracket--;
    } else if (ch == ',' && paren == 0 && bracket == 0) {
      strvec_push(arena, parts, arena_copy(arena, inner + start, i - start));
      start = i + 1u;
    }
  }
  strvec_push(arena, parts, arena_copy(arena, inner + start, n - start));
}

static const char *pad_of(PascalBack *pb, int indent) {
  if (indent < 0) {
    indent = 0;
  }
  return arena_repeat(pb->arena, ' ', (size_t)indent);
}

static const char *concat2(Arena *arena, const char *a, const char *b) {
  return arena_printf(arena, "%s%s", a == NULL ? "" : a, b == NULL ? "" : b);
}

static bool ends_with_colon(const char *s) {
  size_t n;
  if (s == NULL) {
    return false;
  }
  n = strlen(s);
  return n > 0 && s[n - 1u] == ':';
}

static const char *ensure_colon(Arena *arena, const char *header) {
  if (header == NULL) {
    header = "";
  }
  if (ends_with_colon(header)) {
    return header;
  }
  return arena_printf(arena, "%s:", header);
}

static const char *strip_trailing_semi(Arena *arena, const char *s) {
  size_t n;
  size_t end;
  if (s == NULL) {
    return "";
  }
  n = strlen(s);
  end = n;
  while (end > 0 && rt_is_space(s[end - 1u])) {
    end--;
  }
  if (end > 0 && s[end - 1u] == ';') {
    return arena_copy(arena, s, end - 1u);
  }
  return s;
}

static const Tok *pb_code_at(const PascalBack *pb, size_t index) {
  if (index >= pb->ncode) {
    return NULL;
  }
  return &pb->code[index];
}

static const char *pb_word_at(const PascalBack *pb, size_t index) {
  const Tok *tok = pb_code_at(pb, index);
  if (tok == NULL || tok->text == NULL || tok->len == 0) {
    return "";
  }
  return arena_lower(pb->arena, tok->text, tok->len);
}

static const char *pb_word(const PascalBack *pb) {
  return pb_word_at(pb, pb->i);
}

static bool pb_at(const PascalBack *pb, const char *word) {
  return str_eq(pb_word(pb), word);
}

static bool pb_cur_is(const PascalBack *pb, const char *text) {
  return tok_text_is(pb_code_at(pb, pb->i), text);
}

static const char *pb_cur_text(const PascalBack *pb) {
  const Tok *tok = pb_code_at(pb, pb->i);
  if (tok == NULL || tok->text == NULL) {
    return "";
  }
  return tok->text;
}

static int pb_line_no(const PascalBack *pb) {
  const Tok *tok = pb_code_at(pb, pb->i);
  return tok == NULL ? 1 : tok->line;
}

static int tok_col(const Tok *tok) {
  return tok == NULL ? 1 : tok->col;
}

static size_t tok_start_or(const Tok *tok, size_t fallback) {
  return tok == NULL ? fallback : tok->start;
}

static size_t tok_end_or(const Tok *tok, size_t fallback) {
  return tok == NULL ? fallback : tok->end;
}

static size_t prev_code_end(const PascalBack *pb, size_t index) {
  const Tok *tok;
  if (index == 0) {
    return 0;
  }
  tok = pb_code_at(pb, index - 1u);
  return tok == NULL ? 0 : tok->end;
}

static bool pb_starts_routine(PascalBack *pb, size_t at) {
  const char *word = pb_word_at(pb, at);
  const char *next;
  if (is_routine(word)) {
    return true;
  }
  if (str_eq(word, "class") && is_routine(pb_word_at(pb, at + 1u))) {
    return true;
  }
  if (str_eq(word, "generic")) {
    next = pb_word_at(pb, at + 1u);
    if (is_routine(next)) {
      return true;
    }
    if (str_eq(next, "class") && is_routine(pb_word_at(pb, at + 2u))) {
      return true;
    }
  }
  return false;
}

static bool pb_looks_like_body(PascalBack *pb) {
  const char *word = pb_word(pb);
  return str_eq(word, "begin") || str_eq(word, "asm") || is_section(word) || pb_starts_routine(pb, pb->i);
}

static bool pb_looks_like_type_decl(PascalBack *pb) {
  size_t i = pb->i;
  const Tok *first = pb_code_at(pb, i);
  const char *kind;
  if (first == NULL || (first->kind != TOK_IDENT && !tok_word_is(first, "generic"))) {
    return false;
  }
  if (tok_word_is(first, "generic")) {
    i++;
  }
  if (pb_code_at(pb, i) == NULL || pb_code_at(pb, i)->kind != TOK_IDENT) {
    return false;
  }
  i++;
  if (tok_text_is(pb_code_at(pb, i), "<")) {
    int depth = 0;
    while (i < pb->ncode) {
      const Tok *tok = pb_code_at(pb, i);
      if (tok_text_is(tok, "<")) {
        depth++;
      } else if (tok_text_is(tok, ">")) {
        depth--;
        if (depth == 0) {
          i++;
          break;
        }
      }
      i++;
    }
  }
  if (!tok_text_is(pb_code_at(pb, i), "=")) {
    return false;
  }
  i++;
  if (str_eq(pb_word_at(pb, i), "packed") || str_eq(pb_word_at(pb, i), "type")) {
    i++;
  }
  kind = pb_word_at(pb, i);
  return is_type_body(kind)
    || ((str_eq(kind, "type") || str_eq(kind, "class") || str_eq(kind, "record"))
        && str_eq(pb_word_at(pb, i + 1u), "helper"));
}

static void push_gap(PascalBack *pb, size_t from, size_t to) {
  if (pb->source == NULL || to <= from || from >= pb->sourcelen) {
    return;
  }
  if (to > pb->sourcelen) {
    to = pb->sourcelen;
  }
  if (has_blank_line(pb->source + from, to - from)) {
    strvec_push(pb->arena, &pb->trivia_lines, "");
  }
}

static void pb_pull_until(PascalBack *pb, size_t until) {
  size_t cursor;
  if (until < pb->mark) {
    return;
  }
  cursor = pb->mark;
  while (pb->trivia < pb->ntokens) {
    const Tok *item = &pb->tokens[pb->trivia];
    if (item->start >= until) {
      break;
    }
    pb->trivia++;
    if (item->start < pb->mark) {
      continue;
    }
    if (item->kind != TOK_COMMENT && item->kind != TOK_DIRECTIVE) {
      continue;
    }
    push_gap(pb, cursor, item->start);
    strvec_push(pb->arena, &pb->trivia_lines, trim_copy(pb->arena, item->text, item->len));
    cursor = item->end;
  }
  push_gap(pb, cursor, until);
  pb->mark = until;
}

static void pb_pull(PascalBack *pb) {
  const Tok *tok = pb_code_at(pb, pb->i);
  pb_pull_until(pb, tok == NULL ? pb->sourcelen : tok->start);
}

static void pb_flush_trivia(PascalBack *pb, int indent) {
  const char *pad = pad_of(pb, indent);
  size_t i;
  for (i = 0; i < pb->trivia_lines.len; i++) {
    const char *text = pb->trivia_lines.items[i] == NULL ? "" : pb->trivia_lines.items[i];
    if (text[0] == '\0') {
      const char *last = pb->out.len == 0 ? NULL : pb->out.items[pb->out.len - 1u];
      if (last == NULL || last[0] != '\0') {
        strvec_push(pb->arena, &pb->out, "");
      }
      continue;
    }
    strvec_push(pb->arena, &pb->out, concat2(pb->arena, pad, text));
  }
  pb->trivia_lines.len = 0;
}

static void pb_flush_pending(PascalBack *pb, int indent) {
  const char *pad = pad_of(pb, indent);
  size_t i;
  for (i = 0; i < pb->pending_after.len; i++) {
    const char *text = pb->pending_after.items[i] == NULL ? "" : pb->pending_after.items[i];
    strvec_push(pb->arena, &pb->out, concat2(pb->arena, pad, text));
  }
  pb->pending_after.len = 0;
}

static void pb_line(PascalBack *pb, int indent, const char *text) {
  const char *trimmed = collapse_ws_trim(pb->arena, text == NULL ? "" : text);
  size_t mine_n = pb->after_lines.len;
  const char **mine = NULL;
  size_t k;
  const char *pad;
  if (mine_n > 0) {
    if (mine_n > SIZE_MAX / sizeof(const char *)) {
      rt_oom();
    }
    mine = arena_alloc(pb->arena, mine_n * sizeof(const char *), sizeof(void *));
    for (k = 0; k < mine_n; k++) {
      mine[k] = pb->after_lines.items[k].text;
    }
  }
  pb->after_lines.len = 0;
  pb_flush_pending(pb, indent);
  pad = pad_of(pb, indent);
  if (trimmed[0] == '\0') {
    pb_flush_trivia(pb, indent);
    for (k = 0; k < mine_n; k++) {
      strvec_push(pb->arena, &pb->out, concat2(pb->arena, pad, mine[k]));
    }
    return;
  }
  pb_flush_trivia(pb, indent);
  strvec_push(pb->arena, &pb->out, concat2(pb->arena, pad, trimmed));
  for (k = 0; k < mine_n; k++) {
    strvec_push(pb->arena, &pb->pending_after, mine[k]);
  }
  pb->pending_epoch++;
}

static void pb_inline_directives(PascalBack *pb, size_t from_pos, size_t to_pos, StrVec *out) {
  pb_pull_until(pb, from_pos);
  while (pb->trivia < pb->ntokens) {
    const Tok *item = &pb->tokens[pb->trivia];
    if (item->start >= to_pos) {
      break;
    }
    if (item->start < from_pos) {
      pb->trivia++;
      continue;
    }
    pb->trivia++;
    if (item->kind == TOK_DIRECTIVE) {
      strvec_push(pb->arena, out, trim_copy(pb->arena, item->text, item->len));
    } else if (item->kind == TOK_COMMENT) {
      strvec_push(pb->arena, &pb->trivia_lines, trim_copy(pb->arena, item->text, item->len));
    }
  }
  if (to_pos > pb->mark) {
    pb->mark = to_pos;
  }
}

static const char *pb_flat(PascalBack *pb, size_t from, size_t to, bool trail) {
  StrVec parts;
  size_t end_pos;
  size_t k;
  size_t last_index;
  const Tok *last;
  if (from >= to || from >= pb->ncode) {
    return "";
  }
  end_pos = to < pb->ncode ? pb->code[to].start : pb->sourcelen;
  strvec_init(&parts);
  for (k = from; k < to && k < pb->ncode; k++) {
    const Tok *tok = &pb->code[k];
    if (k > from) {
      const Tok *prev = &pb->code[k - 1u];
      pb_inline_directives(pb, prev->end, tok->start, &parts);
    }
    strvec_push(pb->arena, &parts, tok->text == NULL ? "" : tok->text);
  }
  last_index = (to < pb->ncode ? to : pb->ncode);
  if (last_index == 0) {
    return "";
  }
  last = &pb->code[last_index - 1u];
  if (trail) {
    pb_inline_directives(pb, last->end, end_pos, &parts);
  }
  return join_pascal(pb->arena, &parts);
}

static int pb_conditional_depth(const PascalBack *pb, size_t from_pos, size_t to_pos) {
  int depth = 0;
  size_t i;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < from_pos) {
      continue;
    }
    if (item->start >= to_pos) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      depth++;
    } else if (kind == DK_CLOSE) {
      if (depth > 0) {
        depth--;
      }
    }
  }
  return depth;
}

static bool pb_endif_between(const PascalBack *pb, size_t from_pos, size_t to_pos) {
  size_t i;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    if (item->start < from_pos) {
      continue;
    }
    if (item->start >= to_pos) {
      break;
    }
    if (tok_dir_kind(item) == DK_CLOSE) {
      return true;
    }
  }
  return false;
}

static bool pb_open_cond_between(const PascalBack *pb, size_t from_pos, size_t to_pos) {
  int depth = 0;
  bool opened = false;
  size_t i;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < from_pos) {
      continue;
    }
    if (item->start >= to_pos) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      if (depth == 0) {
        opened = true;
      }
      depth++;
    } else if (kind == DK_CLOSE && depth > 0) {
      depth--;
    }
  }
  return opened && depth > 0;
}

static bool pb_if_already_ended(const PascalBack *pb) {
  const Tok *semi;
  const Tok *before;
  if (pb->i == 0) {
    return false;
  }
  semi = pb_code_at(pb, pb->i - 1u);
  if (!tok_text_is(semi, ";")) {
    return false;
  }
  if (pb->i < 2u) {
    return true;
  }
  before = pb_code_at(pb, pb->i - 2u);
  if (tok_word_is(before, "then") || tok_word_is(before, "do")) {
    return false;
  }
  return true;
}

static bool pb_else_closes_outer(const PascalBack *pb, size_t if_pos) {
  const Tok *else_tok;
  int base;
  int depth;
  size_t i;
  if (!pb_at(pb, "else") && !pb_at(pb, "otherwise")) {
    return false;
  }
  else_tok = pb_code_at(pb, pb->i);
  if (else_tok == NULL) {
    return false;
  }
  base = pb_conditional_depth(pb, 0, if_pos);
  if (base == 0) {
    return false;
  }
  depth = base;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < if_pos) {
      continue;
    }
    if (item->start >= else_tok->start) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      depth++;
    } else if (kind == DK_CLOSE) {
      depth--;
      if (depth < base) {
        return true;
      }
    }
  }
  return false;
}

static bool pb_left_branch(const PascalBack *pb, size_t begin_pos, size_t token_pos) {
  int base = pb_conditional_depth(pb, 0, begin_pos);
  int depth;
  size_t i;
  if (base == 0) {
    return false;
  }
  depth = base;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < begin_pos) {
      continue;
    }
    if (item->start >= token_pos) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      depth++;
    } else if (kind == DK_MID) {
      if (depth == base) {
        return true;
      }
    } else if (kind == DK_CLOSE) {
      depth--;
      if (depth < base) {
        return true;
      }
    }
  }
  return false;
}

static void pb_flush_routine_tail(PascalBack *pb, size_t routine_start, size_t to_pos, int indent) {
  int base = pb_conditional_depth(pb, 0, routine_start);
  int depth = base;
  size_t stop = to_pos;
  size_t i;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < routine_start) {
      continue;
    }
    if (item->start >= to_pos) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      depth++;
    } else if (kind == DK_CLOSE) {
      if (depth <= base) {
        stop = item->start;
        break;
      }
      depth--;
    }
  }
  pb_pull_until(pb, stop);
  pb_flush_trivia(pb, indent);
  pb_flush_pending(pb, indent);
}

static void wrap_push(PascalBack *pb, WrapStack *stack, int col, int mode, int depth) {
  if (stack->len == stack->cap) {
    size_t cap = stack->cap == 0 ? 8u : stack->cap * 2u;
    WrapFrame *next;
    if (stack->cap != 0 && cap < stack->cap) {
      rt_oom();
    }
    if (cap > SIZE_MAX / sizeof(WrapFrame)) {
      rt_oom();
    }
    next = arena_alloc(pb->arena, cap * sizeof(WrapFrame), sizeof(void *));
    if (stack->len > 0) {
      memcpy(next, stack->items, stack->len * sizeof(WrapFrame));
    }
    stack->items = next;
    stack->cap = cap;
  }
  stack->items[stack->len].col = col;
  stack->items[stack->len].mode = mode;
  stack->items[stack->len].depth = depth;
  stack->len++;
}

static bool pb_wrapped_by_conditional(PascalBack *pb, size_t routine_start, int start_col) {
  WrapStack stack;
  int paren = 0;
  size_t k = pb->i;
  stack.items = NULL;
  stack.len = 0;
  stack.cap = 0;
  while (k < pb->ncode) {
    const Tok *tok = &pb->code[k];
    const char *word;
    int col;
    bool reprocess = false;
    if (tok_text_is(tok, "(")) {
      paren++;
      k++;
      continue;
    }
    if (tok_text_is(tok, ")") && paren > 0) {
      paren--;
      k++;
      continue;
    }
    if (paren > 0) {
      k++;
      continue;
    }
    word = pb_word_at(pb, k);
    col = tok_col(tok);
    if (stack.len > 0) {
      WrapFrame *top = &stack.items[stack.len - 1u];
      if (top->mode == 0) {
        if (pb_starts_routine(pb, k) && col > top->col) {
          wrap_push(pb, &stack, col, 0, 0);
          k++;
          continue;
        }
        if ((str_eq(word, "begin") || str_eq(word, "asm")) && col > start_col + 1) {
          top = &stack.items[stack.len - 1u];
          top->mode = 1;
          top->depth = 1;
          k++;
          continue;
        }
        if ((str_eq(word, "begin") || str_eq(word, "asm") || str_eq(word, "end") || pb_starts_routine(pb, k))
            && col <= start_col + 1) {
          stack.len--;
          reprocess = true;
        }
        if (!reprocess) {
          k++;
        }
        continue;
      }
      top = &stack.items[stack.len - 1u];
      if (str_eq(word, "begin") || str_eq(word, "asm")) {
        top->depth++;
      } else if (str_eq(word, "end")) {
        top->depth--;
        if (top->depth <= 0) {
          stack.len--;
        }
      } else if (pb_starts_routine(pb, k) && col > top->col) {
        wrap_push(pb, &stack, col, 0, 0);
      }
      k++;
      continue;
    }
    if (pb_starts_routine(pb, k) && col > start_col + 1) {
      wrap_push(pb, &stack, col, 0, 0);
      k++;
      continue;
    }
    if (str_eq(word, "begin") || str_eq(word, "asm")) {
      return pb_conditional_depth(pb, routine_start, tok->start) > 0;
    }
    if (col <= start_col + 1 && (pb_starts_routine(pb, k) || is_section(word) || str_eq(word, "end"))) {
      return false;
    }
    k++;
  }
  return false;
}

static bool pb_finished_assignment(const PascalBack *pb, size_t from, size_t index) {
  int paren = 0;
  int bracket = 0;
  bool saw = false;
  size_t k;
  const Tok *last;
  size_t s;
  for (k = from; k < index; k++) {
    const Tok *tok = pb_code_at(pb, k);
    if (tok == NULL) {
      continue;
    }
    if (tok_text_is(tok, "(")) {
      paren++;
    } else if (tok_text_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_text_is(tok, "[")) {
      bracket++;
    } else if (tok_text_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && tok_text_is(tok, ":=")) {
      saw = true;
    }
  }
  if (!saw || paren != 0 || bracket != 0) {
    return false;
  }
  if (index == 0) {
    return false;
  }
  last = pb_code_at(pb, index - 1u);
  if (last == NULL || last->text == NULL) {
    return false;
  }
  for (s = 0; ASSIGN_SYMS[s] != NULL; s++) {
    if (rt_eq_cstr(last->text, last->len, ASSIGN_SYMS[s])) {
      return false;
    }
  }
  return !is_expr_word(pb_word_at(pb, index - 1u));
}

static bool pb_starts_fresh_statement(const PascalBack *pb, size_t index) {
  const Tok *tok = pb_code_at(pb, index);
  const Tok *next;
  const char *word;
  if (tok == NULL || (tok->kind != TOK_IDENT && tok->kind != TOK_KEYWORD)) {
    return false;
  }
  word = pb_word_at(pb, index);
  if (is_expr_word(word)) {
    return false;
  }
  next = pb_code_at(pb, index + 1u);
  return tok_text_is(next, ":=") || tok_text_is(next, "(");
}

static bool pb_arm_split_before(const PascalBack *pb, size_t index) {
  const Tok *prev;
  const Tok *tok;
  size_t i;
  if (index == 0) {
    return false;
  }
  prev = pb_code_at(pb, index - 1u);
  tok = pb_code_at(pb, index);
  if (prev == NULL || tok == NULL) {
    return false;
  }
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < prev->end) {
      continue;
    }
    if (item->start >= tok->start) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_MID || kind == DK_CLOSE) {
      return true;
    }
  }
  return false;
}

static bool pb_statement_boundary(PascalBack *pb, size_t from, size_t index) {
  const Tok *tok = pb_code_at(pb, index);
  const char *word;
  size_t start;
  size_t prev_end;
  int depth = 0;
  bool boundary = false;
  size_t i;
  if (tok == NULL || (tok->kind != TOK_IDENT && tok->kind != TOK_KEYWORD)) {
    return false;
  }
  word = pb_word_at(pb, index);
  if (is_expr_word(word)) {
    return false;
  }
  if (pb_finished_assignment(pb, from, index) && pb_starts_fresh_statement(pb, index) && pb_arm_split_before(pb, index)) {
    return true;
  }
  start = tok_start_or(pb_code_at(pb, from), 0);
  prev_end = prev_code_end(pb, from);
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < prev_end) {
      continue;
    }
    if (item->start >= start) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      depth++;
    } else if (kind == DK_CLOSE) {
      if (depth > 0) {
        depth--;
      }
    }
  }
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < start) {
      continue;
    }
    if (item->start >= tok->start) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      depth++;
    } else if (kind == DK_CLOSE) {
      if (depth == 0) {
        boundary = true;
      } else {
        depth--;
      }
    } else if (kind == DK_MID && depth == 0) {
      boundary = true;
    }
  }
  return boundary;
}

static bool pb_semi_in_open_cond(const PascalBack *pb, size_t from, size_t index) {
  size_t start = prev_code_end(pb, from);
  const Tok *at = pb_code_at(pb, index);
  size_t pos = at == NULL ? start : at->start;
  return pb_conditional_depth(pb, start, pos) > 0;
}

static bool pb_is_decl_value(const PascalBack *pb, size_t index) {
  const Tok *tok = pb_code_at(pb, index);
  const char *word;
  size_t s;
  if (tok == NULL) {
    return false;
  }
  if (tok->kind == TOK_NUMBER || tok->kind == TOK_STRING) {
    return true;
  }
  word = pb_word_at(pb, index);
  for (s = 0; DECL_SYMS[s] != NULL; s++) {
    if (tok_text_is(tok, DECL_SYMS[s])) {
      return true;
    }
  }
  if (is_one_of(word, DECL_WORDS)) {
    return true;
  }
  if (tok->kind != TOK_IDENT && tok->kind != TOK_KEYWORD) {
    return false;
  }
  if (is_modifier(word) || is_hard_stop(word) || ends_section(word)) {
    return false;
  }
  if (is_routine(word) || str_eq(word, "generic") || str_eq(word, "class")
      || str_eq(word, "record") || str_eq(word, "object")) {
    return false;
  }
  if (pb_starts_fresh_statement(pb, index)) {
    return false;
  }
  return true;
}

static bool pb_same_decl_arm(const PascalBack *pb, size_t from, size_t semi_index) {
  size_t next;
  if (!pb_semi_in_open_cond(pb, from, semi_index)) {
    return false;
  }
  next = semi_index + 1u;
  if (pb_code_at(pb, next) == NULL) {
    return false;
  }
  if (!pb_semi_in_open_cond(pb, from, next)) {
    return false;
  }
  return pb_is_decl_value(pb, next);
}

static bool pb_endif_before_semi(const PascalBack *pb, size_t from, size_t semi_index) {
  const Tok *semi = pb_code_at(pb, semi_index);
  const Tok *first = pb_code_at(pb, from);
  int base;
  int depth;
  size_t i;
  if (semi == NULL || first == NULL) {
    return false;
  }
  base = pb_conditional_depth(pb, prev_code_end(pb, from), first->start);
  depth = base;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < first->start) {
      continue;
    }
    if (item->start >= semi->start) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      depth++;
    } else if (kind == DK_CLOSE) {
      if (depth <= base) {
        return true;
      }
      depth--;
    }
  }
  return false;
}

static void pb_park_boundary(PascalBack *pb, size_t index) {
  const Tok *tok = pb_code_at(pb, index);
  const Tok *prev;
  if (index == 0 || tok == NULL) {
    return;
  }
  prev = pb_code_at(pb, index - 1u);
  if (prev == NULL) {
    return;
  }
  pb_pull_until(pb, prev->end);
  while (pb->trivia < pb->ntokens) {
    const Tok *item = &pb->tokens[pb->trivia];
    if (item->start >= tok->start) {
      break;
    }
    pb->trivia++;
    if (item->start < prev->end) {
      continue;
    }
    if (item->kind != TOK_COMMENT && item->kind != TOK_DIRECTIVE) {
      continue;
    }
    after_push(pb->arena, &pb->after_lines, item->col, trim_copy(pb->arena, item->text, item->len));
  }
  if (tok->start > pb->mark) {
    pb->mark = tok->start;
  }
}

static const char *pb_take_until(PascalBack *pb, const char *const *stops) {
  size_t from = pb->i;
  int paren = 0;
  int bracket = 0;
  while (pb->i < pb->ncode) {
    const Tok *tok = pb_code_at(pb, pb->i);
    const char *word;
    if (tok == NULL) {
      break;
    }
    word = pb_word(pb);
    if (tok_text_is(tok, "(")) {
      paren++;
    } else if (tok_text_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_text_is(tok, "[")) {
      bracket++;
    } else if (tok_text_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && tok_text_is(tok, ";") && pb_same_decl_arm(pb, from, pb->i)) {
      pb->i++;
      continue;
    } else if (paren == 0 && bracket == 0 && pb->i > from && pb_statement_boundary(pb, from, pb->i)) {
      const char *text = pb_flat(pb, from, pb->i, false);
      pb_park_boundary(pb, pb->i);
      return text;
    } else if (paren == 0 && bracket == 0 && tok_text_is(tok, ";")) {
      bool trail = !pb_endif_before_semi(pb, from, pb->i);
      const char *text = pb_flat(pb, from, pb->i, trail);
      pb->i++;
      return text;
    } else if (pb->i > from && paren == 0 && bracket == 0 && is_one_of(word, stops)) {
      const char *text = pb_flat(pb, from, pb->i, false);
      pb_park_boundary(pb, pb->i);
      return text;
    }
    pb->i++;
  }
  return pb_flat(pb, from, pb->i, true);
}

static const char *pb_take_semi(PascalBack *pb) {
  return pb_take_until(pb, HARD_STOP);
}

static bool opens_balanced(PascalBack *pb, size_t index, const char *word) {
  const char *prev = pb_word_at(pb, index == 0 ? SIZE_MAX : index - 1u);
  const char *next = pb_word_at(pb, index + 1u);
  if (str_eq(word, "record") || str_eq(word, "begin") || str_eq(word, "try") || str_eq(word, "asm")
      || str_eq(word, "interface") || str_eq(word, "dispinterface")) {
    return true;
  }
  if ((str_eq(word, "class") || str_eq(word, "object")) && !str_eq(prev, "of") && !str_eq(next, "of")) {
    return true;
  }
  return false;
}

static const char *pb_take_balanced(PascalBack *pb) {
  size_t from = pb->i;
  int paren = 0;
  int bracket = 0;
  int ends = 0;
  while (pb->i < pb->ncode) {
    const Tok *tok = pb_code_at(pb, pb->i);
    const char *word;
    if (tok == NULL) {
      break;
    }
    word = pb_word(pb);
    if (tok_text_is(tok, "(")) {
      paren++;
    } else if (tok_text_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_text_is(tok, "[")) {
      bracket++;
    } else if (tok_text_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && ends == 0 && tok_text_is(tok, ";") && pb_same_decl_arm(pb, from, pb->i)) {
      pb->i++;
      continue;
    } else if (paren == 0 && bracket == 0 && opens_balanced(pb, pb->i, word)) {
      ends++;
    } else if (paren == 0 && bracket == 0 && str_eq(word, "end") && ends > 0) {
      ends--;
    } else if (paren == 0 && bracket == 0 && ends == 0 && tok_text_is(tok, ";")) {
      const char *body = pb_flat(pb, from, pb->i, true);
      pb->i++;
      return body;
    } else if (pb->i > from && paren == 0 && bracket == 0 && ends == 0 && is_hard_stop(word)) {
      return pb_flat(pb, from, pb->i, true);
    }
    pb->i++;
  }
  return pb_flat(pb, from, pb->i, true);
}

static size_t pb_find_word(const PascalBack *pb, const char *word) {
  int paren = 0;
  int bracket = 0;
  size_t k;
  for (k = pb->i; k < pb->ncode; k++) {
    const Tok *tok = &pb->code[k];
    if (tok_text_is(tok, "(")) {
      paren++;
    } else if (tok_text_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_text_is(tok, "[")) {
      bracket++;
    } else if (tok_text_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && tok_word_is(tok, word)) {
      return k;
    } else if (paren == 0 && bracket == 0 && tok_text_is(tok, ";")) {
      return SIZE_MAX;
    }
  }
  return SIZE_MAX;
}

static bool is_cue_boundary(const Tok *tok) {
  if (tok_text_is(tok, ";")) {
    return true;
  }
  return tok_word_is(tok, "begin") || tok_word_is(tok, "end") || tok_word_is(tok, "else")
    || tok_word_is(tok, "otherwise") || tok_word_is(tok, "do") || tok_word_is(tok, "then")
    || tok_word_is(tok, "if") || tok_word_is(tok, "for") || tok_word_is(tok, "while")
    || tok_word_is(tok, "with") || tok_word_is(tok, "repeat") || tok_word_is(tok, "case")
    || tok_word_is(tok, "try") || tok_word_is(tok, "asm") || tok_word_is(tok, "on");
}

static size_t pb_find_cue(PascalBack *pb, const char *word) {
  size_t first = pb_find_word(pb, word);
  int paren = 0;
  int bracket = 0;
  size_t later;
  size_t k;
  if (first == SIZE_MAX) {
    return SIZE_MAX;
  }
  later = first;
  for (k = first + 1u; k < pb->ncode; k++) {
    const Tok *tok = &pb->code[k];
    if (tok_text_is(tok, "(")) {
      paren++;
      continue;
    }
    if (tok_text_is(tok, ")") && paren > 0) {
      paren--;
      continue;
    }
    if (tok_text_is(tok, "[")) {
      bracket++;
      continue;
    }
    if (tok_text_is(tok, "]") && bracket > 0) {
      bracket--;
      continue;
    }
    if (paren != 0 || bracket != 0) {
      continue;
    }
    if (tok_word_is(tok, word)) {
      later = k;
      continue;
    }
    if (is_cue_boundary(tok)) {
      break;
    }
  }
  return later;
}

static size_t pb_find_token(const PascalBack *pb, const char *text) {
  int paren = 0;
  int bracket = 0;
  size_t k;
  for (k = pb->i; k < pb->ncode; k++) {
    const Tok *tok = &pb->code[k];
    if (tok_text_is(tok, "(")) {
      paren++;
    } else if (tok_text_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_text_is(tok, "[")) {
      bracket++;
    } else if (tok_text_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && tok_text_is(tok, text)) {
      return k;
    } else if (paren == 0 && bracket == 0 && tok_text_is(tok, ";")) {
      return SIZE_MAX;
    }
  }
  return SIZE_MAX;
}

static void pb_skip_group(PascalBack *pb, const char *open, const char *close) {
  int depth = 0;
  if (!pb_cur_is(pb, open)) {
    return;
  }
  while (pb->i < pb->ncode) {
    if (pb_cur_is(pb, open)) {
      depth++;
    } else if (pb_cur_is(pb, close)) {
      depth--;
      pb->i++;
      if (depth == 0) {
        return;
      }
      continue;
    }
    pb->i++;
  }
}

static void pb_consume_end(PascalBack *pb) {
  if (!pb_at(pb, "end")) {
    return;
  }
  pb->i++;
  if (pb_cur_is(pb, ".") || pb_cur_is(pb, ";")) {
    pb->i++;
  }
}

static bool pb_looks_like_case_label(PascalBack *pb) {
  size_t colon = pb_find_token(pb, ":");
  size_t k;
  if (colon == SIZE_MAX || colon == pb->i) {
    return false;
  }
  for (k = pb->i; k < colon; k++) {
    const Tok *tok = pb_code_at(pb, k);
    if (tok == NULL) {
      return false;
    }
    if (tok->kind == TOK_IDENT || tok->kind == TOK_NUMBER || tok->kind == TOK_STRING) {
      continue;
    }
    if (tok_text_is(tok, ",") || tok_text_is(tok, "..") || tok_text_is(tok, "-") || tok_text_is(tok, "+")) {
      continue;
    }
    return false;
  }
  return true;
}

static bool line_is_real_code(const char *line) {
  const char *t;
  size_t tn;
  if (line == NULL) {
    return false;
  }
  rt_trim(line, strlen(line), &t, &tn);
  if (tn == 0) {
    return false;
  }
  if (rt_starts(t, tn, "//") || rt_starts(t, tn, "{") || rt_starts(t, tn, "(*")) {
    return false;
  }
  return true;
}

static void pb_unwrap_arm(PascalBack *pb, int indent, bool bodies, bool empty_as_semi) {
  size_t mark;
  size_t n;
  bool code_added = false;
  pb->i++;
  mark = pb->out.len;
  pb_parse_sequence(pb, indent, STOP_END, bodies);
  pb_consume_end(pb);
  for (n = mark; n < pb->out.len; n++) {
    if (line_is_real_code(pb->out.items[n])) {
      code_added = true;
      break;
    }
  }
  if (empty_as_semi && !code_added) {
    pb_line(pb, indent, ";");
  }
}

static bool at_exact_indent(const char *line, int indent) {
  size_t n;
  int i;
  if (line == NULL) {
    return false;
  }
  if (indent < 0) {
    indent = 0;
  }
  n = strlen(line);
  if ((size_t)indent > n) {
    return false;
  }
  for (i = 0; i < indent; i++) {
    if (line[i] != ' ') {
      return false;
    }
  }
  if ((size_t)indent < n && line[indent] == ' ') {
    return false;
  }
  return true;
}

static bool leading_word(const char *line, const char *word) {
  size_t i = 0;
  size_t n;
  size_t wn;
  size_t k;
  if (line == NULL) {
    return false;
  }
  n = strlen(line);
  wn = strlen(word);
  while (i < n && rt_is_space(line[i])) {
    i++;
  }
  if (i > n || wn > n - i) {
    return false;
  }
  for (k = 0; k < wn; k++) {
    if (rt_lower_ch(line[i + k]) != word[k]) {
      return false;
    }
  }
  return word_boundary_at(line, n, i + wn);
}

static const char *replace_leading_if(Arena *arena, const char *line) {
  size_t i = 0;
  size_t n = strlen(line);
  Buf buf;
  char *out;
  while (i < n && rt_is_space(line[i])) {
    i++;
  }
  buf_init(&buf);
  buf_append(&buf, line, i);
  buf_puts(&buf, "else if");
  buf_puts(&buf, line + i + 2u);
  out = buf_arena(&buf, arena);
  buf_free(&buf);
  return out;
}

static void pb_prefix_else(PascalBack *pb, int indent) {
  size_t n;
  for (n = pb->out.len; n > 0; n--) {
    const char *line = pb->out.items[n - 1u];
    const char *trimmed;
    size_t tn;
    const char *tp;
    if (line == NULL || is_blank_text(line)) {
      continue;
    }
    if (!at_exact_indent(line, indent)) {
      continue;
    }
    if (leading_word(line, "if")) {
      pb->out.items[n - 1u] = replace_leading_if(pb->arena, line);
      return;
    }
    if (leading_word(line, "else")) {
      continue;
    }
    rt_trim(line, strlen(line), &tp, &tn);
    trimmed = tp;
    if (rt_starts(trimmed, tn, "//") || rt_starts(trimmed, tn, "{") || rt_starts(trimmed, tn, "(*")) {
      continue;
    }
    return;
  }
}

static bool pb_pending_has_mid(const PascalBack *pb) {
  size_t i;
  for (i = 0; i < pb->pending_after.len; i++) {
    const char *text = pb->pending_after.items[i] == NULL ? "" : pb->pending_after.items[i];
    if (classify_dir(text, strlen(text)) == DK_MID) {
      return true;
    }
  }
  return false;
}

static bool pb_open_body_cond(const PascalBack *pb, size_t from_pos, size_t to_pos) {
  int depth = 0;
  size_t i;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < from_pos) {
      continue;
    }
    if (item->start >= to_pos) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      depth++;
    } else if (kind == DK_CLOSE && depth > 0) {
      depth--;
    }
  }
  return depth > 0;
}

static bool pb_pending_continues_arm(const PascalBack *pb, size_t cue_end) {
  if (!pb_pending_has_mid(pb)) {
    return false;
  }
  return pb_open_body_cond(pb, cue_end, pb->mark);
}

static bool pb_else_arm_ahead(const PascalBack *pb, size_t cue_end) {
  const Tok *next;
  size_t limit;
  size_t i;
  if (!pb_open_body_cond(pb, cue_end, pb->mark)) {
    return false;
  }
  next = pb_code_at(pb, pb->i);
  limit = next == NULL ? pb->sourcelen : next->start;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < pb->mark) {
      continue;
    }
    if (item->start >= limit) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_MID) {
      return true;
    }
    if (kind == DK_OPEN) {
      return false;
    }
  }
  return false;
}

static size_t pb_body_close_at(const PascalBack *pb, size_t cue_end) {
  const Tok *next = pb_code_at(pb, pb->i);
  size_t limit = next == NULL ? pb->sourcelen : next->start;
  int depth = 0;
  size_t close_at = SIZE_MAX;
  size_t i;
  for (i = 0; i < pb->ntokens; i++) {
    const Tok *item = &pb->tokens[i];
    DirKind kind;
    if (item->start < cue_end) {
      continue;
    }
    if (item->start >= limit) {
      break;
    }
    kind = tok_dir_kind(item);
    if (kind == DK_OPEN) {
      depth++;
    } else if (kind == DK_CLOSE && depth > 0) {
      depth--;
      if (depth == 0) {
        close_at = item->end;
      }
    }
  }
  return close_at;
}

static bool pb_body_cond_closes(const PascalBack *pb, size_t cue_end) {
  return pb_body_close_at(pb, cue_end) != SIZE_MAX;
}

static void pb_flush_pending_through(PascalBack *pb, size_t cue_end, size_t close_at, int indent) {
  size_t used = 0;
  size_t i;
  const char *pad;
  if (pb->pending_after.len == 0) {
    return;
  }
  for (i = 0; i < pb->ntokens && used < pb->pending_after.len; i++) {
    const Tok *item = &pb->tokens[i];
    const char *text;
    const char *want;
    if (item->start < cue_end) {
      continue;
    }
    if (item->end > close_at) {
      break;
    }
    if (item->kind != TOK_DIRECTIVE && item->kind != TOK_COMMENT) {
      continue;
    }
    text = trim_copy(pb->arena, item->text, item->len);
    want = pb->pending_after.items[used] == NULL ? "" : pb->pending_after.items[used];
    if (strcmp(want, text) == 0) {
      used++;
    }
  }
  if (used == 0) {
    return;
  }
  pad = pad_of(pb, indent);
  for (i = 0; i < used; i++) {
    const char *text = pb->pending_after.items[i] == NULL ? "" : pb->pending_after.items[i];
    strvec_push(pb->arena, &pb->out, concat2(pb->arena, pad, text));
  }
  strvec_splice_front(&pb->pending_after, used);
}

static void pb_pull_body_tail(PascalBack *pb, size_t cue_end, int indent) {
  size_t close_at = pb_body_close_at(pb, cue_end);
  if (close_at == SIZE_MAX) {
    return;
  }
  pb_flush_pending_through(pb, cue_end, close_at, indent);
  pb_pull_until(pb, close_at);
  pb_flush_trivia(pb, indent);
}

static bool pb_spread_init(PascalBack *pb, int indent, const char *text) {
  size_t n;
  size_t open = SIZE_MAX;
  size_t close = SIZE_MAX;
  size_t i;
  StrVec raw;
  StrVec parts;
  size_t extra;
  size_t header_n = 0;
  size_t note_n = 0;
  const char **headers;
  const char **notes;
  size_t mine_n;
  const char **mine = NULL;
  const char *pad;
  const char *inner;
  const char *head;
  if (text == NULL) {
    return false;
  }
  n = strlen(text);
  for (i = 0; i < n; i++) {
    if (text[i] == '(') {
      open = i;
      break;
    }
  }
  if (open == SIZE_MAX) {
    return false;
  }
  for (i = n; i > 0; i--) {
    if (text[i - 1u] == ')') {
      close = i - 1u;
      break;
    }
  }
  if (close == SIZE_MAX || close <= open) {
    return false;
  }
  split_top_commas(pb->arena, text + open + 1u, close - open - 1u, &raw);
  strvec_init(&parts);
  for (i = 0; i < raw.len; i++) {
    const char *part = raw.items[i] == NULL ? "" : raw.items[i];
    const char *tp;
    size_t tn;
    rt_trim(part, strlen(part), &tp, &tn);
    if (tn == 0) {
      continue;
    }
    strvec_push(pb->arena, &parts, arena_copy(pb->arena, tp, tn));
  }
  if (parts.len < 32u) {
    return false;
  }
  if (pb->trivia_lines.len < parts.len) {
    return false;
  }
  extra = pb->trivia_lines.len - parts.len;
  if (extra > 8u) {
    return false;
  }
  headers = strvec_copy_front(pb->arena, &pb->trivia_lines, extra, &header_n);
  notes = strvec_copy_front(pb->arena, &pb->trivia_lines, parts.len, &note_n);
  mine_n = pb->after_lines.len;
  if (mine_n > 0) {
    mine = arena_alloc(pb->arena, mine_n * sizeof(const char *), sizeof(void *));
    for (i = 0; i < mine_n; i++) {
      mine[i] = pb->after_lines.items[i].text;
    }
  }
  pb->after_lines.len = 0;
  pb_flush_pending(pb, indent);
  pad = pad_of(pb, indent);
  inner = pad_of(pb, indent + 4);
  for (i = 0; i < header_n; i++) {
    strvec_push(pb->arena, &pb->out, concat2(pb->arena, pad, headers[i]));
  }
  head = collapse_ws_trim(pb->arena, arena_copy(pb->arena, text, open + 1u));
  strvec_push(pb->arena, &pb->out, concat2(pb->arena, pad, head));
  for (i = 0; i < parts.len; i++) {
    const char *comma = i + 1u < parts.len ? "," : "";
    const char *note = (i < note_n && notes[i] != NULL && notes[i][0] != '\0') ? notes[i] : "";
    const char *piece;
    if (note[0] == '\0') {
      piece = arena_printf(pb->arena, "%s%s", parts.items[i], comma);
    } else {
      piece = arena_printf(pb->arena, "%s%s %s", parts.items[i], comma, note);
    }
    strvec_push(pb->arena, &pb->out, concat2(pb->arena, inner, piece));
  }
  strvec_push(pb->arena, &pb->out, concat2(pb->arena, pad, ")"));
  for (i = 0; i < mine_n; i++) {
    strvec_push(pb->arena, &pb->pending_after, mine[i]);
  }
  return true;
}

static void pb_parse_sequence(PascalBack *pb, int indent, const char *const *stops, bool bodies) {
  int epoch = pb->pending_epoch;
  while (pb->i < pb->ncode) {
    const char *word;
    size_t before;
    pb_pull(pb);
    word = pb_word(pb);
    if (word[0] == '\0' || is_one_of(word, stops)) {
      break;
    }
    before = pb->i;
    pb_parse_item(pb, indent, bodies);
    if (pb->i == before) {
      pb->i++;
    }
  }
  /* `stmt {$endif} end` has no semicolon, so the endif was parked. Flush it
     at this block's indent only when the block itself emitted a line. Trivia
     pulled for the block is always emitted here, even when nothing was emitted. */
  if (pb->pending_epoch != epoch) {
    pb_flush_pending(pb, indent);
  }
  pb_flush_trivia(pb, indent);
}

static void pb_parse_program(PascalBack *pb, int indent) {
  const char *header = pb_take_semi(pb);
  pb_line(pb, indent, arena_printf(pb->arena, "%s:", header == NULL ? "" : header));
  while (pb->i < pb->ncode && !pb_at(pb, "begin") && !pb_at(pb, "end")) {
    pb_parse_item(pb, indent + 4, true);
  }
  if (pb_at(pb, "begin")) {
    pb->i++;
    pb_parse_sequence(pb, indent + 4, STOP_END, true);
    pb_consume_end(pb);
  }
}

static void pb_parse_unit(PascalBack *pb) {
  const char *header = pb_take_semi(pb);
  bool bodies = false;
  pb_line(pb, 0, header);
  while (pb->i < pb->ncode) {
    const char *word;
    pb_pull(pb);
    word = pb_word(pb);
    if (word[0] == '\0') {
      break;
    }
    if (str_eq(word, "interface") || str_eq(word, "implementation")
        || str_eq(word, "initialization") || str_eq(word, "finalization")) {
      pb_line(pb, 0, word);
      pb->i++;
      bodies = !str_eq(word, "interface");
      continue;
    }
    if (str_eq(word, "end")) {
      const char *punct = "";
      pb->i++;
      if (pb_cur_is(pb, ".") || pb_cur_is(pb, ";")) {
        punct = pb_cur_text(pb);
        pb->i++;
      }
      pb_line(pb, 0, arena_printf(pb->arena, "end%s", punct));
      continue;
    }
    pb_parse_item(pb, 0, bodies);
  }
}

static bool pb_has_bare_implementation(const PascalBack *pb) {
  int paren = 0;
  int bracket = 0;
  size_t i;
  for (i = 0; i < pb->ncode; i++) {
    const Tok *tok = &pb->code[i];
    if (tok_text_is(tok, "(")) {
      paren++;
    } else if (tok_text_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_text_is(tok, "[")) {
      bracket++;
    } else if (tok_text_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && tok_word_is(tok, "implementation")) {
      return true;
    }
  }
  return false;
}

static void pb_parse_interface_fragment(PascalBack *pb) {
  bool bodies = false;
  while (pb->i < pb->ncode) {
    const char *word;
    pb_pull(pb);
    word = pb_word(pb);
    if (word[0] == '\0') {
      break;
    }
    if (str_eq(word, "interface") || str_eq(word, "implementation")
        || str_eq(word, "initialization") || str_eq(word, "finalization")) {
      pb_line(pb, 0, word);
      pb->i++;
      bodies = !str_eq(word, "interface");
      continue;
    }
    pb_parse_item(pb, 0, bodies);
  }
}

static void pb_parse_branced_body(PascalBack *pb, int indent, size_t routine_start) {
  while (pb->i < pb->ncode) {
    const Tok *tok = pb_code_at(pb, pb->i);
    size_t before;
    if (tok == NULL) {
      break;
    }
    if (pb_conditional_depth(pb, routine_start, tok->start) == 0) {
      size_t to_pos;
      if (pb_at(pb, "end")) {
        pb_consume_end(pb);
      }
      to_pos = tok_start_or(pb_code_at(pb, pb->i), pb->sourcelen);
      pb_flush_routine_tail(pb, routine_start, to_pos, indent);
      break;
    }
    if (pb_at(pb, "begin")) {
      size_t begin_pos = tok->start;
      pb_pull(pb);
      pb_line(pb, indent, "begin:");
      pb->i++;
      while (pb->i < pb->ncode && !pb_at(pb, "end")) {
        const Tok *next = pb_code_at(pb, pb->i);
        if (next == NULL || pb_left_branch(pb, begin_pos, next->start)) {
          break;
        }
        before = pb->i;
        pb_parse_item(pb, indent + 4, true);
        if (pb->i == before) {
          pb->i++;
        }
      }
      if (pb_at(pb, "end")) {
        const Tok *end_tok = pb_code_at(pb, pb->i);
        if (!pb_left_branch(pb, begin_pos, tok_start_or(end_tok, 0))) {
          pb_consume_end(pb);
        }
      }
      continue;
    }
    before = pb->i;
    pb_parse_item(pb, indent, true);
    if (pb->i == before) {
      pb->i++;
    }
  }
}

static void pb_parse_routine(PascalBack *pb, int indent, bool bodies) {
  size_t routine_start = tok_start_or(pb_code_at(pb, pb->i), 0);
  int start_col = tok_col(pb_code_at(pb, pb->i));
  const char *header = pb_take_semi(pb);
  bool forwarded = false;
  int mods = 0;
  bool semi_emitted = false;
  int next_col;
  bool sibling;
  bool branched;
  bool has_body;
  const char *shown;
  while (is_modifier(pb_word(pb))) {
    size_t mod_pos = tok_start_or(pb_code_at(pb, pb->i), 0);
    const Tok *mod_tok;
    StrVec lead;
    StrVec trail;
    const char *lead_text;
    const char *trail_text;
    const char *flag;
    const char *text;
    const Tok *trail_tok;
    bool next_is_mod;
    bool trail_open;
    const char *sep;
    if (pb_conditional_depth(pb, routine_start, mod_pos) > 0 && pb_wrapped_by_conditional(pb, routine_start, start_col)) {
      break;
    }
    mod_tok = pb_code_at(pb, pb->i);
    strvec_init(&lead);
    if (mod_tok != NULL) {
      pb_inline_directives(pb, pb->mark, mod_tok->start, &lead);
    }
    flag = pb_word(pb);
    text = pb_take_semi(pb);
    trail_tok = pb_code_at(pb, pb->i);
    next_is_mod = is_modifier(pb_word(pb));
    trail_open = !next_is_mod && trail_tok != NULL && pb_conditional_depth(pb, pb->mark, trail_tok->start) > 0;
    strvec_init(&trail);
    if (trail_tok != NULL && !trail_open) {
      pb_inline_directives(pb, pb->mark, trail_tok->start, &trail);
    }
    lead_text = join_space(pb->arena, &lead);
    trail_text = join_space(pb->arena, &trail);
    sep = (mods == 0 || !semi_emitted) ? "; " : " ";
    if (lead_text[0] == '\0') {
      header = collapse_ws_trim(pb->arena, arena_printf(pb->arena, "%s%s%s", header, sep, text));
    } else {
      header = collapse_ws_trim(pb->arena, arena_printf(pb->arena, "%s%s%s %s", header, sep, lead_text, text));
    }
    if (trail_text[0] != '\0') {
      header = collapse_ws_trim(pb->arena, arena_printf(pb->arena, "%s; %s", header, trail_text));
      semi_emitted = true;
    } else {
      semi_emitted = false;
    }
    if (is_no_body(flag)) {
      forwarded = true;
    }
    mods++;
  }
  next_col = tok_col(pb_code_at(pb, pb->i));
  sibling = pb_starts_routine(pb, pb->i) && next_col <= start_col + 1;
  branched = pb_wrapped_by_conditional(pb, routine_start, start_col);
  has_body = !forwarded && bodies && !sibling && (pb_looks_like_body(pb) || branched);
  if (has_body) {
    shown = arena_printf(pb->arena, "%s:", header == NULL ? "" : header);
  } else if (mods > 0 && !semi_emitted) {
    shown = arena_printf(pb->arena, "%s;", header == NULL ? "" : header);
  } else {
    shown = header;
  }
  pb_line(pb, indent, shown);
  if (!has_body) {
    return;
  }
  while (pb->i < pb->ncode && !pb_at(pb, "begin") && !pb_at(pb, "asm") && !pb_at(pb, "end")) {
    if (is_section(pb_word(pb)) || pb_starts_routine(pb, pb->i)) {
      pb_parse_item(pb, indent + 4, true);
    } else {
      break;
    }
  }
  if (branched) {
    pb_parse_branced_body(pb, indent + 4, routine_start);
    return;
  }
  if (pb_at(pb, "asm")) {
    pb_parse_asm(pb, indent + 4);
    return;
  }
  if (pb_at(pb, "begin") && pb_conditional_depth(pb, routine_start, tok_start_or(pb_code_at(pb, pb->i), routine_start)) > 0) {
    pb_parse_branced_body(pb, indent + 4, routine_start);
    return;
  }
  if (pb_at(pb, "begin")) {
    pb->i++;
    pb_parse_sequence(pb, indent + 4, STOP_END, true);
    pb_consume_end(pb);
  }
}

static void pb_parse_headed(PascalBack *pb, int indent, bool bodies) {
  const char *kind = pb_word(pb);
  size_t if_pos = tok_start_or(pb_code_at(pb, pb->i), 0);
  const char *cue_word = (str_eq(kind, "if") || str_eq(kind, "on")) ? "then" : "do";
  size_t cue = pb_find_cue(pb, cue_word);
  const char *header;
  size_t cue_end;
  if (cue == SIZE_MAX) {
    pb_line(pb, indent, pb_take_semi(pb));
    return;
  }
  header = pb_flat(pb, pb->i, cue + 1u, false);
  pb->i = cue + 1u;
  cue_end = tok_end_or(pb_code_at(pb, cue), 0);
  pb_line(pb, indent, ensure_colon(pb->arena, header));
  pb_parse_cue_body(pb, indent + 4, bodies, cue_end);
  if ((str_eq(kind, "if") || str_eq(kind, "on"))
      && (pb_at(pb, "else") || pb_at(pb, "otherwise"))
      && !pb_if_already_ended(pb)
      && !pb_else_closes_outer(pb, if_pos)) {
    const char *word = pb_word(pb);
    size_t else_end;
    const Tok *if_tok;
    pb->i++;
    else_end = tok_end_or(pb_code_at(pb, pb->i - 1u), 0);
    if_tok = pb_code_at(pb, pb->i);
    if (str_eq(word, "else") && pb_at(pb, "if") && if_tok != NULL
        && !pb_open_cond_between(pb, else_end, if_tok->start)) {
      pb_parse_headed(pb, indent, bodies);
      pb_prefix_else(pb, indent);
    } else {
      pb_line(pb, indent, arena_printf(pb->arena, "%s:", word));
      pb_parse_cue_body(pb, indent + 4, bodies, else_end);
    }
  }
}

static void pb_parse_one_arm(PascalBack *pb, int indent, bool bodies) {
  size_t before = pb->i;
  pb_parse_item(pb, indent, bodies);
  if (pb->i == before) {
    pb->i++;
  }
}

static void pb_parse_cue_body(PascalBack *pb, int indent, bool bodies, size_t cue_end) {
  size_t begin_pos;
  bool arms = false;
  pb_pull(pb);
  begin_pos = tok_start_or(pb_code_at(pb, pb->i), 0);
  if (pb_at(pb, "begin") && pb_endif_between(pb, cue_end, begin_pos)) {
    pb_flush_trivia(pb, indent);
    pb_line(pb, indent, "begin:");
    pb->i++;
    pb_parse_sequence(pb, indent + 4, STOP_END, bodies);
    pb_consume_end(pb);
    return;
  }
  if (pb_at(pb, "begin") && !pb_open_cond_between(pb, cue_end, begin_pos)) {
    pb_unwrap_arm(pb, indent, bodies, false);
    return;
  }
  if (pb_word(pb)[0] == '\0' || pb_at(pb, "end") || pb_at(pb, "else") || pb_at(pb, "otherwise")
      || pb_at(pb, "until") || pb_at(pb, "except") || pb_at(pb, "finally")) {
    pb_flush_trivia(pb, indent);
    return;
  }
  pb_parse_one_arm(pb, indent, bodies);
  while (pb_pending_continues_arm(pb, cue_end) || pb_else_arm_ahead(pb, cue_end)) {
    arms = true;
    pb_parse_one_arm(pb, indent, bodies);
  }
  if (pb_body_cond_closes(pb, cue_end)) {
    pb_pull_body_tail(pb, cue_end, indent);
  } else if (arms) {
    pb_flush_pending(pb, indent);
  }
}

static void pb_parse_repeat(PascalBack *pb, int indent, bool bodies) {
  pb_pull(pb);
  pb->i++;
  pb_line(pb, indent, "repeat:");
  pb_parse_sequence(pb, indent + 4, STOP_UNTIL, bodies);
  if (pb_at(pb, "until")) {
    pb_line(pb, indent, pb_take_semi(pb));
  }
}

static void pb_parse_try(PascalBack *pb, int indent, bool bodies) {
  pb_pull(pb);
  pb->i++;
  pb_line(pb, indent, "try:");
  while (pb->i < pb->ncode) {
    const char *word;
    size_t before;
    pb_pull(pb);
    word = pb_word(pb);
    if (word[0] == '\0') {
      break;
    }
    if (str_eq(word, "except") || str_eq(word, "finally")) {
      pb->i++;
      pb_line(pb, indent, arena_printf(pb->arena, "%s:", word));
      pb_pull(pb);
      if (pb_at(pb, "end")) {
        pb_flush_trivia(pb, indent + 4);
      }
      continue;
    }
    if (str_eq(word, "end")) {
      pb_consume_end(pb);
      return;
    }
    before = pb->i;
    pb_parse_item(pb, indent + 4, bodies);
    if (pb->i == before) {
      pb->i++;
    }
  }
}

static void pb_parse_case(PascalBack *pb, int indent, bool bodies) {
  size_t of_at = pb_find_cue(pb, "of");
  const char *header;
  int arm_indent;
  bool arm_open = false;
  size_t arm_cue = 0;
  if (of_at == SIZE_MAX) {
    pb_line(pb, indent, pb_take_semi(pb));
    return;
  }
  header = pb_flat(pb, pb->i, of_at + 1u, false);
  pb->i = of_at + 1u;
  pb_line(pb, indent, ensure_colon(pb->arena, header));
  arm_indent = indent + 4;
  while (pb->i < pb->ncode && !pb_at(pb, "end")) {
    const char *word;
    if (arm_open && pb_body_cond_closes(pb, arm_cue)) {
      pb_pull_body_tail(pb, arm_cue, indent + 8);
    }
    pb_pull(pb);
    word = pb_word(pb);
    if (word[0] == '\0' || pb_at(pb, "end")) {
      break;
    }
    if (pb_at(pb, "else") || pb_at(pb, "otherwise")) {
      arm_open = false;
      pb->i++;
      pb_line(pb, indent + 4, arena_printf(pb->arena, "%s:", word));
      if (pb_at(pb, "begin")) {
        pb_unwrap_arm(pb, indent + 8, bodies, false);
      } else if (!pb_at(pb, "end") && !pb_looks_like_case_label(pb)) {
        pb_parse_item(pb, indent + 8, bodies);
        if (pb_word(pb)[0] != '\0' && !pb_at(pb, "end") && !pb_at(pb, "else") && !pb_at(pb, "otherwise")) {
          pb_parse_sequence(pb, indent + 8, STOP_END, bodies);
        }
      }
      continue;
    }
    if (pb_starts_routine(pb, pb->i) || is_one_of(word, CASE_STMT)) {
      pb_parse_item(pb, arm_indent, bodies);
      continue;
    }
    {
      size_t colon = pb_find_token(pb, ":");
      const char *label;
      size_t body_pos;
      if (colon == SIZE_MAX) {
        pb_parse_item(pb, arm_indent, bodies);
        continue;
      }
      arm_open = true;
      label = pb_flat(pb, pb->i, colon, true);
      pb->i = colon + 1u;
      arm_cue = tok_end_or(pb_code_at(pb, colon), 0);
      if (pb_cur_is(pb, ";")) {
        const char *shown;
        pb->i++;
        if (ends_with_colon(label) && label[strlen(label) - 1u] == ';') {
          shown = label;
        } else if (strlen(label) > 0 && label[strlen(label) - 1u] == ';') {
          shown = label;
        } else {
          shown = arena_printf(pb->arena, "%s:;", label);
        }
        pb_line(pb, indent + 4, shown);
        continue;
      }
      pb_line(pb, indent + 4, arena_printf(pb->arena, "%s:", label == NULL ? "" : label));
      body_pos = tok_start_or(pb_code_at(pb, pb->i), 0);
      if (pb_at(pb, "begin") && !pb_open_cond_between(pb, arm_cue, body_pos)) {
        pb_unwrap_arm(pb, indent + 8, bodies, true);
      } else if (pb_looks_like_case_label(pb)) {
        /* The next arm. Leave it. */
      } else if (!pb_at(pb, "end") && !pb_at(pb, "else") && !pb_at(pb, "otherwise")) {
        pb_parse_cue_body(pb, indent + 8, bodies, arm_cue);
      }
    }
  }
  if (arm_open && pb_body_cond_closes(pb, arm_cue)) {
    pb_pull_body_tail(pb, arm_cue, indent + 8);
  }
  if (pb_at(pb, "end")) {
    pb_pull_until(pb, tok_start_or(pb_code_at(pb, pb->i), pb->mark));
  }
  pb_flush_trivia(pb, indent + 4);
  pb_consume_end(pb);
}

static void pb_parse_asm(PascalBack *pb, int indent) {
  const Tok *asm_tok;
  size_t start;
  size_t end;
  size_t line_at;
  size_t p;
  pb_pull(pb);
  asm_tok = pb_code_at(pb, pb->i);
  if (pb->i < pb->ncode) {
    pb->i++;
  }
  pb_line(pb, indent, "asm:");
  if (pb->i < pb->ncode) {
    start = pb->code[pb->i].start;
  } else if (asm_tok != NULL) {
    start = asm_tok->end;
  } else {
    start = 0;
  }
  while (pb->i < pb->ncode && !pb_at(pb, "end")) {
    pb->i++;
  }
  end = pb->i < pb->ncode ? pb->code[pb->i].start : pb->sourcelen;
  if (start > pb->sourcelen) {
    start = pb->sourcelen;
  }
  if (end > pb->sourcelen) {
    end = pb->sourcelen;
  }
  if (end < start) {
    end = start;
  }
  line_at = start;
  for (p = start; p <= end; p++) {
    if (p == end || pb->source[p] == '\n') {
      const char *raw = pb->source + line_at;
      size_t raw_n = p - line_at;
      const char *tp;
      size_t tn;
      rt_trim(raw, raw_n, &tp, &tn);
      if (tn > 0) {
        const char *trimmed = arena_copy(pb->arena, tp, tn);
        pb_line(pb, indent + 4, strip_trailing_semi(pb->arena, trimmed));
      }
      line_at = p + 1u;
    }
  }
  pb->mark = end;
  pb_consume_end(pb);
}

static bool pb_member_stop(PascalBack *pb) {
  const char *word = pb_word(pb);
  return is_visibility(word) || str_eq(word, "strict") || str_eq(word, "property")
    || str_eq(word, "end") || pb_starts_routine(pb, pb->i);
}

static void pb_parse_section(PascalBack *pb, int indent, bool bodies, bool (*stop_here)(PascalBack *)) {
  const char *kind = pb_word(pb);
  (void)bodies;
  pb_line(pb, indent, kind);
  pb->i++;
  while (pb->i < pb->ncode) {
    const char *word;
    const char *text;
    pb_pull(pb);
    word = pb_word(pb);
    if (word[0] == '\0') {
      break;
    }
    if (stop_here != NULL && stop_here(pb)) {
      break;
    }
    if (str_eq(kind, "type") && (str_eq(word, "generic") || !ends_section(word))) {
      size_t before = pb->i;
      pb_parse_type_decl(pb, indent + 4);
      if (pb->i == before) {
        pb->i++;
      }
      continue;
    }
    if (ends_section(word)) {
      break;
    }
    text = str_eq(kind, "type") ? pb_take_semi(pb) : pb_take_balanced(pb);
    if (text[0] != '\0' && !str_eq(kind, "type") && pb_spread_init(pb, indent + 4, text)) {
      continue;
    }
    if (text[0] != '\0') {
      pb_line(pb, indent + 4, text);
    }
  }
  pb_flush_trivia(pb, indent + 4);
}

static void pb_parse_type_decl(PascalBack *pb, int indent) {
  size_t from = pb->i;
  int paren = 0;
  int angle = 0;
  size_t eq = SIZE_MAX;
  const char *kind;
  bool helper;
  while (pb->i < pb->ncode) {
    const char *text = pb_cur_text(pb);
    if (str_eq(text, "(")) {
      paren++;
    } else if (str_eq(text, ")") && paren > 0) {
      paren--;
    } else if (str_eq(text, "<")) {
      angle++;
    } else if (str_eq(text, ">") && angle > 0) {
      angle--;
    } else if (paren == 0 && angle == 0 && str_eq(text, "=")) {
      eq = pb->i;
      pb->i++;
      break;
    } else if (pb->i > from && paren == 0 && angle == 0 && (str_eq(text, ";") || ends_section(pb_word(pb)))) {
      break;
    }
    pb->i++;
  }
  if (eq == SIZE_MAX) {
    bool stop_semi = pb_cur_is(pb, ";");
    const char *text = pb_flat(pb, from, pb->i, true);
    if (stop_semi) {
      pb->i++;
    }
    if (text[0] != '\0') {
      pb_line(pb, indent, text);
    }
    return;
  }
  if (str_eq(pb_word(pb), "packed") && is_type_body(pb_word_at(pb, pb->i + 1u))) {
    pb->i++;
  }
  kind = pb_word(pb);
  helper = (str_eq(kind, "type") || str_eq(kind, "class") || str_eq(kind, "record"))
    && str_eq(pb_word_at(pb, pb->i + 1u), "helper");
  if ((is_type_body(kind) || helper) && !(str_eq(kind, "class") && str_eq(pb_word_at(pb, pb->i + 1u), "of"))) {
    const char *head;
    pb->i++;
    if (str_eq(pb_word(pb), "external")) {
      pb->i++;
      if (str_eq(pb_word(pb), "name")) {
        const char *next;
        pb->i++;
        next = pb_cur_text(pb);
        if (next[0] != '\0' && !str_eq(next, "(") && !str_eq(next, ";") && !str_eq(next, ":")) {
          pb->i++;
        }
      }
    }
    if (str_eq(pb_word(pb), "helper")) {
      pb->i++;
      if (str_eq(pb_word(pb), "for")) {
        pb->i++;
      }
      while (pb->i < pb->ncode) {
        const char *word = pb_word(pb);
        if (word[0] == '\0' || str_eq(word, "end") || str_eq(word, "strict") || is_visibility(word)
            || is_section(word) || str_eq(word, "property") || str_eq(word, "class")
            || pb_starts_routine(pb, pb->i) || pb_cur_is(pb, ";")) {
          break;
        }
        pb->i++;
      }
    }
    while (str_eq(pb_word(pb), "sealed") || str_eq(pb_word(pb), "abstract")) {
      pb->i++;
    }
    if (pb_cur_is(pb, "(")) {
      pb_skip_group(pb, "(", ")");
    }
    while (str_eq(pb_word(pb), "sealed") || str_eq(pb_word(pb), "abstract")) {
      pb->i++;
    }
    if (pb_cur_is(pb, ";")) {
      const char *text = pb_flat(pb, from, pb->i, true);
      pb->i++;
      pb_line(pb, indent, text);
      return;
    }
    head = pb_flat(pb, from, pb->i, true);
    pb_line(pb, indent, ensure_colon(pb->arena, head));
    pb_parse_members(pb, indent + 4);
    if (pb_at(pb, "end")) {
      pb_consume_end(pb);
    }
    return;
  }
  {
    const char *left = pb_flat(pb, from, eq + 1u, true);
    const char *rest = pb_take_balanced(pb);
    const char *text = collapse_ws_trim(pb->arena, arena_printf(pb->arena, "%s %s", left, rest));
    if (text[0] != '\0') {
      pb_line(pb, indent, text);
    }
  }
}

static void pb_parse_members(PascalBack *pb, int indent) {
  size_t guard = SIZE_MAX;
  while (pb->i < pb->ncode && !pb_at(pb, "end")) {
    const char *word;
    const char *field;
    if (pb->i == guard) {
      note_push(&pb->notes, pb_line_no(pb), "Skipped a class member the reverse pass could not read.");
      pb->i++;
      if (pb->i >= pb->ncode) {
        break;
      }
    }
    guard = pb->i;
    pb_pull(pb);
    if (pb_at(pb, "end") || pb->i >= pb->ncode) {
      break;
    }
    word = pb_word(pb);
    if (str_eq(word, "strict") && is_visibility(pb_word_at(pb, pb->i + 1u))) {
      pb_line(pb, indent, arena_printf(pb->arena, "%s %s", word, pb_word_at(pb, pb->i + 1u)));
      pb->i += 2u;
      continue;
    }
    if (is_visibility(word) || str_eq(word, "optional") || str_eq(word, "required")) {
      pb_line(pb, indent, word);
      pb->i++;
      continue;
    }
    if (str_eq(word, "class") && is_one_of(pb_word_at(pb, pb->i + 1u), MEMBER_CLASS)) {
      pb_line(pb, indent, arena_printf(pb->arena, "%s %s", word, pb_word_at(pb, pb->i + 1u)));
      pb->i += 2u;
      continue;
    }
    if (pb_starts_routine(pb, pb->i)) {
      pb_parse_routine(pb, indent, false);
      continue;
    }
    if (str_eq(word, "property") || str_eq(word, "var") || str_eq(word, "const")) {
      pb_line(pb, indent, pb_take_semi(pb));
      continue;
    }
    if (is_section(word)) {
      pb_parse_section(pb, indent, false, pb_member_stop);
      continue;
    }
    field = pb_take_balanced(pb);
    if (field[0] != '\0') {
      pb_line(pb, indent, field);
    }
  }
  if (pb_at(pb, "end")) {
    pb_pull_until(pb, tok_start_or(pb_code_at(pb, pb->i), pb->mark));
  }
  pb_flush_trivia(pb, indent);
}

static void pb_parse_item(PascalBack *pb, int indent, bool bodies) {
  const char *word;
  pb_pull(pb);
  word = pb_word(pb);
  if (word[0] == '\0') {
    return;
  }
  if (str_eq(word, "uses") || str_eq(word, "requires") || str_eq(word, "contains") || str_eq(word, "exports")) {
    pb_line(pb, indent, pb_take_semi(pb));
    return;
  }
  if (is_section(word)) {
    pb_parse_section(pb, indent, bodies, NULL);
    return;
  }
  if (pb_starts_routine(pb, pb->i)) {
    pb_parse_routine(pb, indent, bodies);
    return;
  }
  if (str_eq(word, "begin")) {
    pb->i++;
    pb_line(pb, indent, "begin:");
    pb_parse_sequence(pb, indent + 4, STOP_END, true);
    pb_consume_end(pb);
    return;
  }
  if (str_eq(word, "asm")) {
    pb_parse_asm(pb, indent);
    return;
  }
  if (str_eq(word, "repeat")) {
    pb_parse_repeat(pb, indent, bodies);
    return;
  }
  if (str_eq(word, "try")) {
    pb_parse_try(pb, indent, bodies);
    return;
  }
  if (str_eq(word, "case")) {
    pb_parse_case(pb, indent, bodies);
    return;
  }
  if (str_eq(word, "if") || str_eq(word, "while") || str_eq(word, "for") || str_eq(word, "with") || str_eq(word, "on")) {
    pb_parse_headed(pb, indent, bodies);
    return;
  }
  if (str_eq(word, "else") || str_eq(word, "otherwise")) {
    size_t else_end;
    pb->i++;
    else_end = tok_end_or(pb_code_at(pb, pb->i == 0 ? 0 : pb->i - 1u), 0);
    if (str_eq(word, "else") && pb_at(pb, "if")) {
      pb_parse_headed(pb, indent, bodies);
      pb_prefix_else(pb, indent);
    } else {
      pb_line(pb, indent, arena_printf(pb->arena, "%s:", word));
      pb_parse_cue_body(pb, indent + 4, bodies, else_end);
    }
    return;
  }
  if (str_eq(word, "initialization") || str_eq(word, "finalization")) {
    pb_line(pb, indent, word);
    pb->i++;
    return;
  }
  if (str_eq(word, "end")) {
    const char *punct = "";
    pb->i++;
    if (pb_cur_is(pb, ".") || pb_cur_is(pb, ";")) {
      punct = pb_cur_text(pb);
      pb->i++;
    }
    pb_line(pb, indent, arena_printf(pb->arena, "end%s", punct));
    return;
  }
  if (pb_looks_like_type_decl(pb)) {
    pb_parse_type_decl(pb, indent);
    return;
  }
  {
    const char *text = pb_take_semi(pb);
    if (text[0] != '\0') {
      pb_line(pb, indent, text);
    }
  }
}

static void pb_run(PascalBack *pb) {
  const char *word;
  pb_pull(pb);
  word = pb_word(pb);
  if (str_eq(word, "program") || str_eq(word, "library")) {
    pb_parse_program(pb, 0);
  } else if (str_eq(word, "unit")) {
    pb_parse_unit(pb);
  } else if (pb_has_bare_implementation(pb)) {
    pb_parse_interface_fragment(pb);
  } else {
    pb_parse_sequence(pb, 0, STOP_NONE, true);
  }
  pb_pull(pb);
  pb_flush_trivia(pb, 0);
  pb_flush_pending(pb, 0);
}

static const char *finish(PascalBack *pb) {
  StrVec out;
  size_t i;
  Buf buf;
  char *text;
  strvec_init(&out);
  for (i = 0; i < pb->out.len; i++) {
    const char *line = pb->out.items[i] == NULL ? "" : pb->out.items[i];
    size_t n;
    if (is_blank_text(line) && (out.len == 0 || str_eq(out.items[out.len - 1u], ""))) {
      continue;
    }
    n = strlen(line);
    while (n > 0 && (line[n - 1u] == ' ' || line[n - 1u] == '\t')) {
      n--;
    }
    strvec_push(pb->arena, &out, arena_copy(pb->arena, line, n));
  }
  while (out.len > 0 && str_eq(out.items[out.len - 1u], "")) {
    out.len--;
  }
  if (out.len == 0) {
    return arena_cstr(pb->arena, "");
  }
  buf_init(&buf);
  for (i = 0; i < out.len; i++) {
    if (i > 0) {
      buf_putc(&buf, '\n');
    }
    buf_puts(&buf, out.items[i]);
  }
  buf_putc(&buf, '\n');
  text = buf_arena(&buf, pb->arena);
  buf_free(&buf);
  return text;
}

static void pb_init(PascalBack *pb, Arena *arena, const LexOut *lexed) {
  size_t i;
  size_t n = 0;
  size_t w = 0;
  memset(pb, 0, sizeof *pb);
  pb->arena = arena;
  pb->source = lexed->source == NULL ? "" : lexed->source;
  pb->sourcelen = lexed->sourcelen;
  pb->tokens = lexed->tokens;
  pb->ntokens = lexed->ntokens;
  for (i = 0; i < lexed->ntokens; i++) {
    if (lexed->tokens[i].kind != TOK_COMMENT && lexed->tokens[i].kind != TOK_DIRECTIVE) {
      n++;
    }
  }
  if (n > 0) {
    if (n > SIZE_MAX / sizeof(Tok)) {
      rt_oom();
    }
    pb->code = arena_alloc(arena, n * sizeof(Tok), sizeof(void *));
    for (i = 0; i < lexed->ntokens; i++) {
      if (lexed->tokens[i].kind != TOK_COMMENT && lexed->tokens[i].kind != TOK_DIRECTIVE) {
        pb->code[w++] = lexed->tokens[i];
      }
    }
  }
  pb->ncode = n;
  note_init(&pb->notes, arena);
  for (i = 0; i < lexed->nnotes; i++) {
    note_push(&pb->notes, lexed->notes[i].line, lexed->notes[i].message);
  }
  strvec_init(&pb->out);
  strvec_init(&pb->trivia_lines);
  after_init(&pb->after_lines);
  strvec_init(&pb->pending_after);
}

Result to_pascal_spaced(const char *source, size_t len) {
  Arena *arena = arena_new();
  LexOut lexed;
  PascalBack pb;
  Result result;
  memset(&result, 0, sizeof result);
  lex_source(arena, source, len, profile_get(LANG_PASCAL), &lexed);
  pb_init(&pb, arena, &lexed);
  pb_run(&pb);
  result.arena = arena;
  result.language = LANG_PASCAL;
  result.lines = NULL;
  result.nlines = 0;
  result.code = finish(&pb);
  result.notes = pb.notes.items;
  result.nnotes = pb.notes.len;
  return result;
}
