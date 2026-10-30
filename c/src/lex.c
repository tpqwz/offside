#include "engine.h"

#include <string.h>

static const char *const MULTI[] = {
  ">>>=", ">>=", "<<=", "...", ">>>", "=>", "?.", "??", "==", "!=", "<=", ">=",
  "<>", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
  "<<", ">>", "->", "::", ":=", "..",
};

typedef struct Lexer {
  Arena *arena;
  const char *src;
  size_t n;
  size_t i;
  int line;
  int col;
  bool at_bol;
  const Profile *profile;
  Tok *tokens;
  size_t ntokens;
  size_t tcap;
  NoteBuf notes;
} Lexer;

static char at(const Lexer *lx, size_t i) {
  if (lx->src == NULL || i >= lx->n) {
    return '\0';
  }
  return lx->src[i];
}

static bool starts_at(const Lexer *lx, size_t i, const char *lit) {
  size_t n = lit == NULL ? 0 : strlen(lit);
  if (n == 0 || i > lx->n || n > lx->n - i) {
    return false;
  }
  return memcmp(lx->src + i, lit, n) == 0;
}

static char bump(Lexer *lx) {
  char ch = at(lx, lx->i);
  if (lx->i < lx->n) {
    lx->i++;
  }
  if (ch == '\n') {
    lx->line++;
    lx->col = 1;
  } else if (ch == '\t') {
    lx->col += 8 - ((lx->col - 1) % 8);
  } else {
    lx->col++;
  }
  return ch;
}

static void push_tok(Lexer *lx, TokenKind kind, int line, int col, size_t start) {
  Tok *tok;
  if (lx->ntokens == lx->tcap) {
    size_t cap = lx->tcap == 0 ? 64u : lx->tcap * 2u;
    Tok *next;
    if (lx->tcap != 0 && cap < lx->tcap) {
      rt_oom();
    }
    next = arena_alloc(lx->arena, cap * sizeof(Tok), sizeof(void *));
    if (lx->ntokens > 0) {
      memcpy(next, lx->tokens, lx->ntokens * sizeof(Tok));
    }
    lx->tokens = next;
    lx->tcap = cap;
  }
  tok = &lx->tokens[lx->ntokens];
  tok->kind = kind;
  tok->len = lx->i >= start ? lx->i - start : 0;
  tok->text = arena_copy(lx->arena, lx->src + start, tok->len);
  tok->line = line;
  tok->col = col;
  tok->start = start;
  tok->end = lx->i;
  lx->ntokens++;
  lx->at_bol = false;
}

static void note(Lexer *lx, int line, const char *message) {
  note_push(&lx->notes, line, message);
}

static void consume_string(Lexer *lx, char quote) {
  int line = lx->line;
  int col = lx->col;
  size_t start = lx->i;
  char ch;
  bump(lx);
  while (lx->i < lx->n) {
    ch = at(lx, lx->i);
    if (ch == '\\') {
      bump(lx);
      if (lx->i < lx->n) {
        bump(lx);
      }
      continue;
    }
    if (ch == '\n') {
      break;
    }
    bump(lx);
    if (ch == quote) {
      push_tok(lx, quote == '\'' ? TOK_CHAR : TOK_STRING, line, col, start);
      return;
    }
  }
  note(lx, line, "Unclosed string");
  push_tok(lx, quote == '\'' ? TOK_CHAR : TOK_STRING, line, col, start);
}

static void consume_line_comment(Lexer *lx) {
  int line = lx->line;
  int col = lx->col;
  size_t start = lx->i;
  bump(lx);
  bump(lx);
  while (lx->i < lx->n && at(lx, lx->i) != '\n') {
    bump(lx);
  }
  push_tok(lx, TOK_COMMENT, line, col, start);
}

static void consume_block_comment(Lexer *lx) {
  int line = lx->line;
  int col = lx->col;
  size_t start = lx->i;
  bump(lx);
  bump(lx);
  while (lx->i < lx->n) {
    if (at(lx, lx->i) == '*' && at(lx, lx->i + 1u) == '/') {
      bump(lx);
      bump(lx);
      push_tok(lx, TOK_COMMENT, line, col, start);
      return;
    }
    bump(lx);
  }
  note(lx, line, "Unclosed block comment");
  push_tok(lx, TOK_COMMENT, line, col, start);
}

static void consume_template(Lexer *lx) {
  int depth = 0;
  bump(lx);
  while (lx->i < lx->n) {
    char ch = at(lx, lx->i);
    if (ch == '\\') {
      bump(lx);
      if (lx->i < lx->n) {
        bump(lx);
      }
      continue;
    }
    if (ch == '`' && depth == 0) {
      bump(lx);
      return;
    }
    if (ch == '$' && at(lx, lx->i + 1u) == '{' && depth == 0) {
      bump(lx);
      bump(lx);
      depth = 1;
      continue;
    }
    if (depth > 0 && ch == '`') {
      consume_template(lx);
      continue;
    }
    if (depth > 0 && (ch == '"' || ch == '\'')) {
      bool save = lx->at_bol;
      consume_string(lx, ch);
      if (lx->ntokens > 0) {
        lx->ntokens--;
      }
      lx->at_bol = save;
      continue;
    }
    if (depth > 0 && ch == '/' && at(lx, lx->i + 1u) == '/') {
      while (lx->i < lx->n && at(lx, lx->i) != '\n') {
        bump(lx);
      }
      continue;
    }
    if (depth > 0 && ch == '/' && at(lx, lx->i + 1u) == '*') {
      bump(lx);
      bump(lx);
      while (lx->i < lx->n && !(at(lx, lx->i) == '*' && at(lx, lx->i + 1u) == '/')) {
        bump(lx);
      }
      if (lx->i < lx->n) {
        bump(lx);
        bump(lx);
      }
      continue;
    }
    if (depth > 0 && ch == '{') {
      depth++;
      bump(lx);
      continue;
    }
    if (depth > 0 && ch == '}') {
      depth--;
      bump(lx);
      continue;
    }
    bump(lx);
  }
}

static bool hex_digit(char ch) {
  unsigned char u = (unsigned char)ch;
  return rt_is_digit(ch) || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

static bool consume_regex(Lexer *lx) {
  int line = lx->line;
  int col = lx->col;
  size_t start = lx->i;
  size_t saved_i = lx->i;
  int saved_line = lx->line;
  int saved_col = lx->col;
  bool saved_bol = lx->at_bol;
  bool closed = false;
  bump(lx);
  while (lx->i < lx->n) {
    char ch = at(lx, lx->i);
    if (ch == '\n') {
      break;
    }
    if (ch == '\\') {
      bump(lx);
      if (lx->i < lx->n && at(lx, lx->i) != '\n') {
        bump(lx);
      }
      continue;
    }
    if (ch == '[') {
      bump(lx);
      while (lx->i < lx->n && at(lx, lx->i) != '\n' && at(lx, lx->i) != ']') {
        if (at(lx, lx->i) == '\\') {
          bump(lx);
        }
        if (lx->i < lx->n) {
          bump(lx);
        }
      }
      if (at(lx, lx->i) == ']') {
        bump(lx);
      }
      continue;
    }
    bump(lx);
    if (ch == '/') {
      closed = true;
      break;
    }
  }
  if (!closed) {
    lx->i = saved_i;
    lx->line = saved_line;
    lx->col = saved_col;
    lx->at_bol = saved_bol;
    return false;
  }
  while (lx->i < lx->n && rt_is_alpha(at(lx, lx->i))) {
    bump(lx);
  }
  push_tok(lx, TOK_REGEX, line, col, start);
  return true;
}

static bool is_u(char ch) { return ch == 'u' || ch == 'U'; }
static bool is_l(char ch) { return ch == 'l' || ch == 'L'; }
static bool is_w(char ch) { return ch == 'w' || ch == 'W'; }
static bool is_b(char ch) { return ch == 'b' || ch == 'B'; }
static bool is_d(char ch) { return ch == 'd' || ch == 'D'; }
static bool is_f(char ch) { return ch == 'f' || ch == 'F'; }

static bool c_suffix_at(const char *s, size_t n) {
  if (n == 3u) {
    return (is_u(s[0]) && is_w(s[1]) && is_b(s[2]))
      || (is_u(s[0]) && is_l(s[1]) && is_l(s[2]))
      || (is_l(s[0]) && is_l(s[1]) && is_u(s[2]));
  }
  if (n == 2u) {
    return (is_w(s[0]) && is_b(s[1]))
      || (is_d(s[0]) && (is_f(s[1]) || is_l(s[1])))
      || (is_u(s[0]) && is_l(s[1]))
      || (is_l(s[0]) && is_l(s[1]))
      || (is_l(s[0]) && is_u(s[1]));
  }
  return n == 1u && (is_u(s[0]) || is_l(s[0]) || is_f(s[0]));
}

static bool ident_cont(char ch) {
  return rt_is_alnum(ch) || ch == '_' || ch == '$';
}

static size_t c_number_suffix(const char *s, size_t n) {
  size_t len;
  size_t max = n < 3u ? n : 3u;
  for (len = max; len >= 1u; len--) {
    char next;
    if (!c_suffix_at(s, len)) {
      if (len == 1u) {
        break;
      }
      continue;
    }
    next = len < n ? s[len] : '\0';
    if (len == n || !ident_cont(next)) {
      return len;
    }
    if (len == 1u) {
      break;
    }
  }
  return 0;
}

static bool number_digit(bool hex, bool bin, char ch) {
  if (hex) {
    return hex_digit(ch);
  }
  if (bin) {
    return ch == '0' || ch == '1';
  }
  return rt_is_digit(ch);
}

static void eat_number_digits(Lexer *lx, bool hex, bool bin) {
  while (lx->i < lx->n) {
    char ch = at(lx, lx->i);
    if (number_digit(hex, bin, ch)) {
      bump(lx);
      continue;
    }
    if (lx->profile->id != LANG_C && lx->profile->id != LANG_PASCAL && ch == '_') {
      bump(lx);
      continue;
    }
    if (lx->profile->id == LANG_C && ch == '\'' && number_digit(hex, bin, at(lx, lx->i + 1u))) {
      bump(lx);
      continue;
    }
    break;
  }
}

static void consume_number(Lexer *lx) {
  int line = lx->line;
  int col = lx->col;
  size_t start = lx->i;
  bool hex = at(lx, lx->i) == '0' && (at(lx, lx->i + 1u) == 'x' || at(lx, lx->i + 1u) == 'X');
  bool bin = at(lx, lx->i) == '0' && (at(lx, lx->i + 1u) == 'b' || at(lx, lx->i + 1u) == 'B');
  bool oct = at(lx, lx->i) == '0' && (at(lx, lx->i + 1u) == 'o' || at(lx, lx->i + 1u) == 'O');
  size_t suffix;
  size_t k;
  if (hex || bin || oct) {
    bump(lx);
    bump(lx);
  }
  eat_number_digits(lx, hex, bin);
  if (!hex && !bin && !oct && at(lx, lx->i) == '.' && rt_is_digit(at(lx, lx->i + 1u))) {
    bump(lx);
    eat_number_digits(lx, false, false);
  }
  if (!hex && !bin && (at(lx, lx->i) == 'e' || at(lx, lx->i) == 'E')) {
    char nch = at(lx, lx->i + 1u);
    if (nch == '+' || nch == '-' || rt_is_digit(nch)) {
      bump(lx);
      if (at(lx, lx->i) == '+' || at(lx, lx->i) == '-') {
        bump(lx);
      }
      eat_number_digits(lx, false, false);
    }
  }
  if (lx->profile->id == LANG_C) {
    suffix = c_number_suffix(lx->src + lx->i, lx->n - lx->i);
    for (k = 0; k < suffix; k++) {
      bump(lx);
    }
  } else {
    while (lx->i < lx->n) {
      char ch = at(lx, lx->i);
      if (ch != 'u' && ch != 'U' && ch != 'l' && ch != 'L' && ch != 'f' && ch != 'F' && ch != 'n') {
        break;
      }
      bump(lx);
    }
  }
  push_tok(lx, TOK_NUMBER, line, col, start);
}

static const Tok *previous_code(const Lexer *lx) {
  size_t k = lx->ntokens;
  while (k > 0) {
    k--;
    if (lx->tokens[k].kind != TOK_COMMENT) {
      return &lx->tokens[k];
    }
  }
  return NULL;
}

static bool can_start_regex(const Lexer *lx) {
  const Tok *prev = previous_code(lx);
  if (prev == NULL) {
    return true;
  }
  if (prev->kind == TOK_IDENT || prev->kind == TOK_NUMBER || prev->kind == TOK_STRING
      || prev->kind == TOK_CHAR || prev->kind == TOK_TEMPLATE || prev->kind == TOK_REGEX) {
    return false;
  }
  if (rt_eq_cstr(prev->text, prev->len, ")") || rt_eq_cstr(prev->text, prev->len, "]")
      || rt_eq_cstr(prev->text, prev->len, "++") || rt_eq_cstr(prev->text, prev->len, "--")) {
    return false;
  }
  if (prev->kind == TOK_KEYWORD
      && (rt_eq_cstr(prev->text, prev->len, "this") || rt_eq_cstr(prev->text, prev->len, "super"))) {
    return false;
  }
  return true;
}

static void consume_pascal_string(Lexer *lx) {
  int line = lx->line;
  int col = lx->col;
  size_t start = lx->i;
  bump(lx);
  while (lx->i < lx->n) {
    if (at(lx, lx->i) == '\'' && at(lx, lx->i + 1u) == '\'') {
      bump(lx);
      bump(lx);
      continue;
    }
    if (at(lx, lx->i) == '\'') {
      bump(lx);
      break;
    }
    if (at(lx, lx->i) == '\n') {
      note(lx, line, "Unclosed string");
      break;
    }
    bump(lx);
  }
  push_tok(lx, TOK_STRING, line, col, start);
}

static void consume_nested(Lexer *lx, const char *open, const char *close, TokenKind kind) {
  int line = lx->line;
  int col = lx->col;
  size_t start = lx->i;
  size_t on = strlen(open);
  size_t cn = strlen(close);
  size_t k;
  int depth = 1;
  for (k = 0; k < on; k++) {
    bump(lx);
  }
  while (lx->i < lx->n && depth > 0) {
    if (starts_at(lx, lx->i, open)) {
      depth++;
      for (k = 0; k < on; k++) {
        bump(lx);
      }
      continue;
    }
    if (starts_at(lx, lx->i, close)) {
      depth--;
      for (k = 0; k < cn; k++) {
        bump(lx);
      }
      continue;
    }
    bump(lx);
  }
  if (depth > 0) {
    note(lx, line, kind == TOK_COMMENT ? "Unclosed comment" : "Unclosed directive");
  }
  push_tok(lx, kind, line, col, start);
}

static void consume_until(Lexer *lx, const char *end, TokenKind kind) {
  int line = lx->line;
  int col = lx->col;
  size_t start = lx->i;
  size_t en = strlen(end);
  size_t k;
  while (lx->i < lx->n && !starts_at(lx, lx->i, end)) {
    bump(lx);
  }
  if (lx->i >= lx->n) {
    note(lx, line, kind == TOK_COMMENT ? "Unclosed comment" : "Unclosed directive");
  } else {
    for (k = 0; k < en; k++) {
      bump(lx);
    }
  }
  push_tok(lx, kind, line, col, start);
}

static bool lex_pascal(Lexer *lx) {
  char ch = at(lx, lx->i);
  if (ch == '^') {
    unsigned char nch = (unsigned char)at(lx, lx->i + 1u);
    bool ctrl = (nch >= 64u && nch <= 95u) || (nch >= 97u && nch <= 122u);
    bool continues = rt_is_alnum(at(lx, lx->i + 2u)) || at(lx, lx->i + 2u) == '_';
    const Tok *prev = previous_code(lx);
    bool deref = prev != NULL && (prev->kind == TOK_IDENT
      || rt_eq_cstr(prev->text, prev->len, ")")
      || rt_eq_cstr(prev->text, prev->len, "]")
      || rt_eq_cstr(prev->text, prev->len, "^"));
    if (ctrl && !continues && !deref) {
      int line = lx->line;
      int col = lx->col;
      size_t start = lx->i;
      bump(lx);
      bump(lx);
      push_tok(lx, TOK_CHAR, line, col, start);
      return true;
    }
  }
  if (ch == '\'') {
    consume_pascal_string(lx);
    return true;
  }
  if (ch == '{') {
    if (at(lx, lx->i + 1u) == '$' || at(lx, lx->i + 1u) == '%') {
      consume_until(lx, "}", TOK_DIRECTIVE);
    } else {
      consume_nested(lx, "{", "}", TOK_COMMENT);
    }
    return true;
  }
  if (ch == '(' && at(lx, lx->i + 1u) == '*') {
    if (at(lx, lx->i + 2u) == '$') {
      consume_until(lx, "*)", TOK_DIRECTIVE);
    } else {
      consume_nested(lx, "(*", "*)", TOK_COMMENT);
    }
    return true;
  }
  if (ch == '/' && at(lx, lx->i + 1u) == '/') {
    consume_line_comment(lx);
    return true;
  }
  if (ch == '$' && hex_digit(at(lx, lx->i + 1u))) {
    int line = lx->line;
    int col = lx->col;
    size_t start = lx->i;
    bump(lx);
    while (lx->i < lx->n && hex_digit(at(lx, lx->i))) {
      bump(lx);
    }
    push_tok(lx, TOK_NUMBER, line, col, start);
    return true;
  }
  if (ch == '%' && (at(lx, lx->i + 1u) == '0' || at(lx, lx->i + 1u) == '1')) {
    int line = lx->line;
    int col = lx->col;
    size_t start = lx->i;
    bump(lx);
    while (lx->i < lx->n && (at(lx, lx->i) == '0' || at(lx, lx->i) == '1')) {
      bump(lx);
    }
    push_tok(lx, TOK_NUMBER, line, col, start);
    return true;
  }
  if (ch == '&' && at(lx, lx->i + 1u) >= '0' && at(lx, lx->i + 1u) <= '7') {
    int line = lx->line;
    int col = lx->col;
    size_t start = lx->i;
    bump(lx);
    while (lx->i < lx->n && at(lx, lx->i) >= '0' && at(lx, lx->i) <= '7') {
      bump(lx);
    }
    push_tok(lx, TOK_NUMBER, line, col, start);
    return true;
  }
  if (ch == '&' && (rt_is_alpha(at(lx, lx->i + 1u)) || at(lx, lx->i + 1u) == '_')) {
    int line = lx->line;
    int col = lx->col;
    size_t start = lx->i;
    bump(lx);
    while (lx->i < lx->n && (rt_is_alnum(at(lx, lx->i)) || at(lx, lx->i) == '_')) {
      bump(lx);
    }
    push_tok(lx, TOK_IDENT, line, col, start);
    return true;
  }
  if (ch == '#' && (at(lx, lx->i + 1u) == '$' || at(lx, lx->i + 1u) == '%' || rt_is_digit(at(lx, lx->i + 1u)))) {
    int line = lx->line;
    int col = lx->col;
    size_t start = lx->i;
    bump(lx);
    if (at(lx, lx->i) == '$') {
      bump(lx);
      while (lx->i < lx->n && hex_digit(at(lx, lx->i))) {
        bump(lx);
      }
    } else if (at(lx, lx->i) == '%') {
      bump(lx);
      while (lx->i < lx->n && (at(lx, lx->i) == '0' || at(lx, lx->i) == '1')) {
        bump(lx);
      }
    } else {
      while (lx->i < lx->n && rt_is_digit(at(lx, lx->i))) {
        bump(lx);
      }
    }
    push_tok(lx, TOK_NUMBER, line, col, start);
    return true;
  }
  return false;
}

static void push_digraph(Lexer *lx, const char *text, size_t width) {
  int line = lx->line;
  int col = lx->col;
  size_t start = lx->i;
  size_t k;
  Tok *tok;
  for (k = 0; k < width; k++) {
    bump(lx);
  }
  push_tok(lx, TOK_PUNCT, line, col, start);
  tok = &lx->tokens[lx->ntokens - 1u];
  tok->text = arena_cstr(lx->arena, text);
  tok->len = strlen(text);
}

void lex_source(Arena *arena, const char *source, size_t len, const Profile *profile, LexOut *out) {
  Lexer lx;
  size_t norm_len = 0;
  memset(&lx, 0, sizeof lx);
  memset(out, 0, sizeof *out);
  lx.arena = arena;
  lx.profile = profile;
  note_init(&lx.notes, arena);
  lx.src = rt_normalize_newlines(arena, source, len, &norm_len);
  lx.n = norm_len;
  lx.line = 1;
  lx.col = 1;
  lx.at_bol = true;
  while (lx.i < lx.n) {
    char ch = at(&lx, lx.i);
    if (ch == ' ' || ch == '\t' || ch == '\r') {
      bump(&lx);
      continue;
    }
    if (ch == '\n') {
      bump(&lx);
      lx.at_bol = true;
      continue;
    }
    if (lx.at_bol && profile->preprocessor && (ch == '#' || (profile->id == LANG_C && starts_at(&lx, lx.i, "%:")))) {
      int line = lx.line;
      int col = lx.col;
      size_t start = lx.i;
      bool digraph = ch == '%';
      bool comment = false;
      if (digraph) {
        bump(&lx);
        bump(&lx);
      }
      while (lx.i < lx.n && (comment || at(&lx, lx.i) != '\n')) {
        if (at(&lx, lx.i) == '\\' && at(&lx, lx.i + 1u) == '\n') {
          bump(&lx);
          bump(&lx);
          continue;
        }
        if (!comment && at(&lx, lx.i) == '/' && at(&lx, lx.i + 1u) == '*') {
          comment = true;
        } else if (comment && at(&lx, lx.i) == '*' && at(&lx, lx.i + 1u) == '/') {
          comment = false;
          bump(&lx);
        }
        bump(&lx);
      }
      push_tok(&lx, TOK_DIRECTIVE, line, col, start);
      if (digraph && lx.ntokens > 0) {
        Tok *last = &lx.tokens[lx.ntokens - 1u];
        if (last->len >= 2u) {
          last->text = arena_join(lx.arena, "#", 1u, last->text + 2, last->len - 2u);
          last->len = 1u + (last->len - 2u);
        }
      }
      continue;
    }
    if (profile->id == LANG_PASCAL && lex_pascal(&lx)) {
      continue;
    }
    if (ch == '/' && at(&lx, lx.i + 1u) == '/') {
      consume_line_comment(&lx);
      continue;
    }
    if (ch == '/' && at(&lx, lx.i + 1u) == '*') {
      consume_block_comment(&lx);
      continue;
    }
    if (profile->regex && ch == '/' && can_start_regex(&lx)) {
      if (consume_regex(&lx)) {
        continue;
      }
    }
    if (ch == '"' || ch == '\'') {
      consume_string(&lx, ch);
      continue;
    }
    if (profile->templates && ch == '`') {
      int line = lx.line;
      int col = lx.col;
      size_t start = lx.i;
      consume_template(&lx);
      if (lx.i >= lx.n || (lx.src[start] == '`' && (lx.i == 0 || lx.src[lx.i - 1u] != '`'))) {
        note(&lx, line, "Unclosed template");
      }
      push_tok(&lx, TOK_TEMPLATE, line, col, start);
      continue;
    }
    if (rt_is_alpha(ch) || ch == '_' || ch == '$' || (profile->id != LANG_JAVASCRIPT && (unsigned char)ch > 127u)) {
      int line = lx.line;
      int col = lx.col;
      size_t start = lx.i;
      const char *text;
      size_t tlen;
      const char *key;
      size_t keylen;
      while (lx.i < lx.n) {
        char cur = at(&lx, lx.i);
        if (rt_is_alnum(cur) || cur == '_' || cur == '$' || (unsigned char)cur > 127u
            || (profile->id == LANG_C && (starts_at(&lx, lx.i, "\\u") || starts_at(&lx, lx.i, "\\U")))) {
          if (profile->id == LANG_C && (starts_at(&lx, lx.i, "\\u") || starts_at(&lx, lx.i, "\\U"))) {
            int hexes = at(&lx, lx.i + 1u) == 'u' ? 4 : 8;
            int h;
            bump(&lx);
            bump(&lx);
            for (h = 0; h < hexes && hex_digit(at(&lx, lx.i)); h++) {
              bump(&lx);
            }
            continue;
          }
          bump(&lx);
          continue;
        }
        break;
      }
      text = lx.src + start;
      tlen = lx.i - start;
      if (profile->id == LANG_PASCAL) {
        key = arena_lower(lx.arena, text, tlen);
      } else {
        key = text;
      }
      keylen = tlen;
      push_tok(&lx, profile_keyword(profile, key, keylen) ? TOK_KEYWORD : TOK_IDENT, line, col, start);
      continue;
    }
    if (rt_is_digit(ch) || (ch == '.' && rt_is_digit(at(&lx, lx.i + 1u)))) {
      consume_number(&lx);
      continue;
    }
    if (profile->id == LANG_C && starts_at(&lx, lx.i, "%:%:")) {
      push_digraph(&lx, "##", 4u);
      continue;
    }
    if (profile->id == LANG_C && starts_at(&lx, lx.i, "%:")) {
      push_digraph(&lx, "#", 2u);
      continue;
    }
    if (profile->id == LANG_C && starts_at(&lx, lx.i, "<%")) {
      push_digraph(&lx, "{", 2u);
      continue;
    }
    if (profile->id == LANG_C && starts_at(&lx, lx.i, "%>")) {
      push_digraph(&lx, "}", 2u);
      continue;
    }
    if (profile->id == LANG_C && starts_at(&lx, lx.i, "<:")) {
      push_digraph(&lx, "[", 2u);
      continue;
    }
    if (profile->id == LANG_C && starts_at(&lx, lx.i, ":>")) {
      push_digraph(&lx, "]", 2u);
      continue;
    }
    {
      int line = lx.line;
      int col = lx.col;
      size_t start = lx.i;
      size_t m;
      bool matched = false;
      for (m = 0; m < sizeof MULTI / sizeof MULTI[0]; m++) {
        if (starts_at(&lx, lx.i, MULTI[m])) {
          size_t k;
          size_t mn = strlen(MULTI[m]);
          for (k = 0; k < mn; k++) {
            bump(&lx);
          }
          push_tok(&lx, TOK_PUNCT, line, col, start);
          matched = true;
          break;
        }
      }
      if (matched) {
        continue;
      }
      bump(&lx);
      push_tok(&lx, TOK_PUNCT, line, col, start);
    }
  }
  out->tokens = lx.tokens;
  out->ntokens = lx.ntokens;
  out->notes = lx.notes.items;
  out->nnotes = lx.notes.len;
  out->source = lx.src;
  out->sourcelen = lx.n;
}
