#include "engine.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
  COL_KEEP = 0,
  COL_BRACE,
  COL_DROP,
  COL_SEMI,
  COL_BEGIN,
  COL_ARM
} ColonMode;

typedef enum { BR_INIT = 0, BR_STMT } BraceKind;
typedef enum { FK_BLOCK = 0, FK_ENUM, FK_STRUCT, FK_UNION } FrameKind;
typedef enum { OP_LBRACE = 0, OP_BEGIN, OP_NONE } Opener;
typedef enum { CL_RBRACE = 0, CL_END, CL_UNTIL, CL_NONE } Closer;
typedef enum { CK_OTHER = 0, CK_OPEN, CK_CLOSE, CK_MID } CondKind;
typedef enum { LK_BLANK = 0, LK_COMMENT, LK_DIRECTIVE, LK_CODE } LineKind;

typedef struct LogicalLine {
  int no;
  const char *lead;
  const char **gaps;
  size_t ngaps;
  size_t gapcap;
  Tok *tokens;
  size_t ntokens;
  size_t tokcap;
  const char *trailing;
} LogicalLine;

typedef struct Frame {
  int header_indent;
  int body_indent;
  bool semi_on_close;
  const char *name;
  FrameKind kind;
  bool wants_tail;
  int line;
  Opener opener;
  Closer closer;
  bool delayed_begin;
  bool body_started;
  bool dot;
  bool asm_frame;
  bool keep_open;
  const char *arm;
  int cond_depth;
  int body_cond;
  bool skip_shell;
} Frame;

typedef struct LayoutState {
  int ternary;
  int paren;
  int bracket;
  BraceKind *braces;
  int *brace_paren;
  size_t nbraces;
  size_t bracecap;
} LayoutState;

typedef struct Classified {
  bool opens;
  bool hanging;
  int ternary;
  int paren;
  int bracket;
  BraceKind *braces;
  int *brace_paren;
  size_t nbraces;
  bool closed_init;
} Classified;

typedef struct PascalStyle {
  bool open;
  ColonMode colon;
  Opener opener;
  Closer closer;
  bool delayed;
  bool dot;
  bool asm_frame;
  bool keep_open;
  bool present;
} PascalStyle;

typedef struct SegBuf {
  OutSeg *items;
  size_t len;
  size_t cap;
} SegBuf;

typedef struct Layout {
  Arena *arena;
  const Profile *profile;
  LogicalLine *logical;
  size_t nlogical;
  OutLine *out;
  size_t nout;
  size_t outcap;
  NoteBuf notes;
  Frame *stack;
  size_t nstack;
  size_t stackcap;
  bool has_pending;
  Frame pending;
  LayoutState state;
  int *pp_brace;
  size_t npp;
  size_t ppcap;
  bool *dead_if0;
  bool *dead_flag;
  size_t ndead;
  size_t deadcap;
  bool hanging;
  int hang_indent;
  const char *hang_text;
  bool expect_while;
  LogicalLine *held;
  size_t nheld;
  size_t heldcap;
} Layout;

static const char *const HANGING[] = {
  "+", "-", "*", "/", "%", "=", "<", ">", "!", "&", "|", "^", "~", "?", ":", ",", ".", "\\",
  "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
  "<<", ">>", ">>>", "->", "=>", "?.", "??", "::", "<<=", ">>=", ">>>=",
};
static const char *const CONT_START[] = {
  "+", "-", "/", "%", "?", "<", ">", "=",
  "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", ">>>",
  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>=",
  "->", ".", "|", "^",
};
static const char *const TYPE_PREFIX[] = {
  "auto", "char", "const", "double", "enum", "extern", "float", "inline",
  "int", "long", "register", "restrict", "short", "signed", "static", "struct",
  "typedef", "union", "unsigned", "void", "volatile", "_Bool", "_Complex", "_Imaginary",
  "bool", "constexpr", "thread_local", "_Thread_local", "__thread",
  "typeof", "typeof_unqual", "__typeof__", "__typeof_unqual__", "__auto_type",
  "_Atomic", "_BitInt", "__int128", "__complex__", "__signed__", "__unsigned__",
  "alignas", "_Alignas", "_Noreturn", "noreturn", "__inline__", "__restrict__",
};
static const char *const SPEC_CALL[] = {
  "typeof", "typeof_unqual", "__typeof__", "__typeof_unqual__",
  "_Atomic", "_BitInt", "alignas", "_Alignas", "alignof", "_Alignof",
  "__alignof__", "__alignof",
};
static const char *const ATTR_NAME[] = { "__attribute__", "__attribute", "__declspec" };
static const char *const COLON_HEADER[] = {
  "else", "do", "struct", "union", "enum", "try", "finally", "class", "interface", "record",
};
static const char *const HEADER_KEYWORDS[] = {
  "else", "do", "struct", "union", "enum", "class", "interface", "record", "try", "finally",
};
static const char *const DECL_START[] = {
  "if", "else", "for", "while", "do", "switch", "return", "goto", "break", "continue",
  "case", "default", "sizeof", "typedef", "struct", "union", "enum", "int", "void", "char",
  "long", "short", "float", "double", "signed", "unsigned", "const", "static", "extern",
  "volatile", "inline", "auto", "register", "restrict", "_Bool", "_Complex", "_Imaginary",
};
static const char *const HEADER_SKIP[] = {
  "int", "void", "char", "long", "short", "float", "double", "signed", "unsigned",
  "const", "static", "extern", "volatile", "inline", "public", "private", "protected",
  "final", "abstract", "synchronized", "native", "strictfp", "var", "let",
};
static const char *const MODIFIER[] = {
  "forward", "external", "overload", "inline", "noinline", "noreturn", "nostackframe",
  "virtual", "abstract", "override", "reintroduce", "static", "cdecl", "stdcall", "pascal",
  "register", "safecall", "assembler", "message", "deprecated", "platform", "far", "near",
  "export", "name", "saveregisters", "interrupt", "varargs", "alias", "cppdecl", "mwpascal",
  "hardfloat", "softfloat", "iocheck", "extdecl",
};
static const char *const PASCAL_BARE[] = {
  "var", "type", "const", "label", "threadvar", "resourcestring",
  "public", "private", "protected", "published", "strict", "strict private",
  "strict protected", "required", "optional", "interface", "implementation",
  "initialization", "finalization", "contains", "requires", "begin", "asm",
  "repeat", "try", "except", "finally", "else", "otherwise",
};
static const char *const STMT_NAMES[] = {
  "if", "else", "otherwise", "while", "for", "with", "on", "repeat", "case", "try",
};
static const char *const NOT_ATTR_HEAD[] = {
  "case", "default", "if", "for", "while", "switch", "return", "goto", "break", "continue", "do", "else",
};
static const char *const CTRL_NOT_INIT[] = { "if", "while", "for", "switch" };
static const char *const FRESH_HEAD[] = {
  "procedure", "function", "constructor", "destructor", "operator", "begin", "end",
  "var", "type", "const", "label", "threadvar", "resourcestring", "if", "for", "while",
  "repeat", "try", "case", "with", "asm", "uses", "initialization", "finalization",
  "interface", "implementation", "program", "unit", "library",
};
static const char *const DECL_HEAD[] = {
  "var", "type", "const", "label", "threadvar", "resourcestring", "exports",
  "property", "generic", "uses", "public", "private", "protected", "published", "strict",
};
static const char *const ROUTINE_HEAD[] = {
  "procedure", "function", "constructor", "destructor", "operator",
};
static const char *const NOT_DECL_HEAD[] = {
  "if", "while", "for", "repeat", "try", "case", "with", "asm", "raise", "break", "continue",
  "exit", "goto", "on", "inherited", "else", "otherwise", "until", "begin", "end",
};
static const char *const CLASS_MEMBER[] = {
  "procedure", "function", "constructor", "destructor", "operator", "var", "property", "const",
};
static const char *const CLASS_SECTION[] = { "var", "property", "const", "threadvar" };
static const char *const END_MOD[] = { "deprecated", "platform", "experimental", "unimplemented" };
static const char *const TYPE_TAIL[] = { "abstract", "sealed" };
static const char *const CUE_WORDS[] = {
  "procedure", "function", "constructor", "destructor", "operator", "program", "library",
  "if", "for", "while", "with", "repeat", "try", "case", "asm", "else", "otherwise", "on",
};

static bool set_has(const char *const *set, size_t n, const char *text, size_t len) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (rt_eq_cstr(text, len, set[i])) {
      return true;
    }
  }
  return false;
}

static bool set_has_ci(const char *const *set, size_t n, const char *text) {
  size_t i;
  if (text == NULL) {
    return false;
  }
  for (i = 0; i < n; i++) {
    if (rt_eq_ci(text, strlen(text), set[i])) {
      return true;
    }
  }
  return false;
}

#define HAS(set, text, len) set_has((set), sizeof(set) / sizeof((set)[0]), (text), (len))
#define HAS_C(set, cstr) set_has((set), sizeof(set) / sizeof((set)[0]), (cstr), strlen(cstr))

static bool tok_is(const Tok *tok, const char *text) {
  return tok != NULL && rt_eq_cstr(tok->text, tok->len, text);
}

static const char *lower_tok(Arena *arena, const Tok *tok) {
  if (tok == NULL) {
    return "";
  }
  return arena_lower(arena, tok->text, tok->len);
}

static char *join_text(Arena *arena, const Tok *tokens, size_t n) {
  Buf buf;
  size_t i;
  char *out;
  buf_init(&buf);
  for (i = 0; i < n; i++) {
    buf_append(&buf, tokens[i].text, tokens[i].len);
  }
  out = buf_arena(&buf, arena);
  buf_free(&buf);
  return out;
}

static void *grow_to(void *ptr, size_t *cap, size_t need, size_t elem) {
  size_t ncap;
  if (need <= *cap) {
    return ptr;
  }
  ncap = *cap == 0 ? 8u : *cap;
  while (ncap < need) {
    if (ncap > SIZE_MAX / 2u) {
      rt_oom();
    }
    ncap *= 2u;
  }
  if (elem != 0 && ncap > SIZE_MAX / elem) {
    rt_oom();
  }
  *cap = ncap;
  return rt_realloc(ptr, ncap * elem);
}

static void line_add_tok(Arena *arena, LogicalLine *line, Tok tok, const char *gap) {
  (void)arena;
  if (line->ntokens == line->tokcap) {
    line->tokens = grow_to(line->tokens, &line->tokcap, line->ntokens + 1u, sizeof(Tok));
  }
  if (line->ngaps == line->gapcap) {
    line->gaps = grow_to(line->gaps, &line->gapcap, line->ngaps + 1u, sizeof(char *));
  }
  line->tokens[line->ntokens++] = tok;
  line->gaps[line->ngaps++] = gap;
}

static LogicalLine blank_line(Arena *arena, int no, const char *lead) {
  LogicalLine line;
  memset(&line, 0, sizeof line);
  line.no = no;
  line.lead = lead == NULL ? arena_cstr(arena, "") : lead;
  line.trailing = arena_cstr(arena, "");
  return line;
}

static size_t skip_group(const Tok *tokens, size_t n, size_t index, const char *open, const char *close) {
  size_t i;
  int depth = 0;
  if (index >= n || !tok_is(&tokens[index], open)) {
    return index;
  }
  for (i = index; i < n; i++) {
    if (tok_is(&tokens[i], open)) {
      depth++;
    } else if (tok_is(&tokens[i], close)) {
      depth--;
      if (depth == 0) {
        return i + 1u;
      }
    }
  }
  return n;
}

static Tok *strip_attributes(Arena *arena, const Tok *tokens, size_t n, size_t *out_n) {
  Tok *out = NULL;
  size_t len = 0;
  size_t cap = 0;
  size_t i = 0;
  while (i < n) {
    if (HAS(ATTR_NAME, tokens[i].text, tokens[i].len) || HAS(SPEC_CALL, tokens[i].text, tokens[i].len)) {
      size_t next = skip_group(tokens, n, i + 1u, "(", ")");
      i = next == i + 1u ? i + 1u : next;
      continue;
    }
    if (tok_is(&tokens[i], "[") && i + 1u < n && tok_is(&tokens[i + 1u], "[")) {
      i = skip_group(tokens, n, i, "[", "]");
      continue;
    }
    if (len == cap) {
      out = grow_to(out, &cap, len + 1u, sizeof(Tok));
    }
    out[len++] = tokens[i];
    i++;
  }
  (void)arena;
  *out_n = len;
  return out;
}

static bool is_attribute_line(const Tok *tokens, size_t n) {
  size_t i = 0;
  bool saw = false;
  while (i < n) {
    if (tok_is(&tokens[i], "[")) {
      size_t next = skip_group(tokens, n, i, "[", "]");
      if (next == i) {
        return false;
      }
      i = next;
      saw = true;
      continue;
    }
    if (HAS(ATTR_NAME, tokens[i].text, tokens[i].len) || tok_is(&tokens[i], "alignas") || tok_is(&tokens[i], "_Alignas")) {
      size_t next = skip_group(tokens, n, i + 1u, "(", ")");
      if (next == i + 1u) {
        return false;
      }
      i = next;
      saw = true;
      continue;
    }
    return false;
  }
  return saw;
}

static Tok *code_tokens(const LogicalLine *line, size_t *out_n) {
  Tok *out = NULL;
  size_t n = 0;
  size_t cap = 0;
  size_t i;
  for (i = 0; i < line->ntokens; i++) {
    if (line->tokens[i].kind == TOK_COMMENT) {
      continue;
    }
    if (n == cap) {
      out = grow_to(out, &cap, n + 1u, sizeof(Tok));
    }
    out[n++] = line->tokens[i];
  }
  *out_n = n;
  return out;
}

static LineKind line_kind(const LogicalLine *line) {
  size_t n = 0;
  Tok *code;
  LineKind kind;
  if (line->ntokens == 0) {
    return LK_BLANK;
  }
  code = code_tokens(line, &n);
  if (n == 0) {
    free(code);
    return LK_COMMENT;
  }
  kind = code[0].kind == TOK_DIRECTIVE ? LK_DIRECTIVE : LK_CODE;
  free(code);
  return kind;
}

static const char *trim_lower(Arena *arena, const char *text) {
  const char *start;
  size_t n;
  rt_trim(text, text == NULL ? 0 : strlen(text), &start, &n);
  return arena_lower(arena, start, n);
}

static bool word_at(const char *body, const char *word) {
  size_t n = strlen(word);
  if (!rt_starts(body, strlen(body), word)) {
    return false;
  }
  return body[n] == '\0' || !(rt_is_alnum(body[n]) || body[n] == '_');
}

static CondKind cond_kind(Arena *arena, const char *text) {
  const char *body = trim_lower(arena, text);
  const char *p = body;
  if (rt_starts(body, strlen(body), "{$")) {
    p = body + 2;
    while (*p == ' ' || *p == '\t') {
      p++;
    }
    if (word_at(p, "if") || word_at(p, "ifdef") || word_at(p, "ifndef") || word_at(p, "ifopt") || word_at(p, "ifc")) {
      return CK_OPEN;
    }
    if (word_at(p, "endif") || word_at(p, "ifend")) {
      return CK_CLOSE;
    }
    if (word_at(p, "else") || word_at(p, "elseif")) {
      return CK_MID;
    }
    return CK_OTHER;
  }
  if (*body == '#') {
    p = body + 1;
    while (*p == ' ' || *p == '\t') {
      p++;
    }
    if (word_at(p, "if") || word_at(p, "ifdef") || word_at(p, "ifndef")) {
      return CK_OPEN;
    }
    if (word_at(p, "endif")) {
      return CK_CLOSE;
    }
    if (word_at(p, "else") || word_at(p, "elifdef") || word_at(p, "elifndef") || word_at(p, "elif")) {
      return CK_MID;
    }
  }
  return CK_OTHER;
}

static bool hash_if_zero(Arena *arena, const char *text, bool elif) {
  const char *body = trim_lower(arena, text);
  const char *p;
  if (*body != '#') {
    return false;
  }
  p = body + 1;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (elif) {
    if (!word_at(p, "elif")) {
      return false;
    }
    p += 4;
  } else {
    if (!word_at(p, "if")) {
      return false;
    }
    p += 2;
  }
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  return *p == '0' && (p[1] == '\0' || !(rt_is_alnum(p[1]) || p[1] == '_'));
}

static int indent_of(const LogicalLine *line) {
  size_t n = 0;
  Tok *code = code_tokens(line, &n);
  int col = n == 0 ? 1 : code[0].col;
  free(code);
  return col;
}

static const char *header_name(Arena *arena, const Tok *tokens, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (tok_is(&tokens[i], "(") && i > 0) {
      const Tok *prev = &tokens[i - 1u];
      if (prev->kind == TOK_IDENT || prev->kind == TOK_KEYWORD) {
        return arena_printf(arena, "%s()", prev->text);
      }
    }
  }
  for (i = 0; i < n; i++) {
    if (tokens[i].kind == TOK_KEYWORD && !HAS(HEADER_SKIP, tokens[i].text, tokens[i].len)) {
      return tokens[i].text;
    }
  }
  return "block";
}

static bool semi_on_close(const Tok *tokens, size_t n, const Profile *profile) {
  size_t sn = 0;
  size_t i;
  Tok *stripped = strip_attributes(NULL, tokens, n, &sn);
  bool paren = false;
  bool semi = false;
  for (i = 0; i < sn; i++) {
    if (tok_is(&stripped[i], "(")) {
      paren = true;
    }
    if (stripped[i].kind == TOK_KEYWORD && profile_semi_close(profile, stripped[i].text, stripped[i].len)) {
      semi = true;
    }
  }
  free(stripped);
  return !paren && semi;
}

static void aggregate_info(const Tok *tokens, size_t n, FrameKind *kind, bool *wants_tail) {
  size_t i;
  int paren = 0;
  int kw = -1;
  *kind = FK_BLOCK;
  *wants_tail = false;
  for (i = 0; i < n; i++) {
    if (tok_is(&tokens[i], "(")) {
      paren++;
    } else if (tok_is(&tokens[i], ")") && paren > 0) {
      paren--;
    } else if (paren == 0 && (tok_is(&tokens[i], "enum") || tok_is(&tokens[i], "struct") || tok_is(&tokens[i], "union"))) {
      *kind = tok_is(&tokens[i], "enum") ? FK_ENUM : tok_is(&tokens[i], "struct") ? FK_STRUCT : FK_UNION;
      kw = (int)i;
      break;
    }
  }
  if (*kind == FK_BLOCK || kw < 0) {
    return;
  }
  {
    int depth = 0;
    for (i = (size_t)kw + 1u; i < n; i++) {
      if (tok_is(&tokens[i], ":")) {
        break;
      }
      if (tok_is(&tokens[i], "(")) {
        if (depth == 0) {
          *kind = FK_BLOCK;
          return;
        }
        depth++;
      } else if (tok_is(&tokens[i], ")") && depth > 0) {
        depth--;
      }
    }
  }
  *wants_tail = true;
}

static bool opens_block(const Tok *code, size_t n, bool block_colon, bool label_colon) {
  size_t i;
  if (!block_colon) {
    return false;
  }
  if (n > 0 && (tok_is(&code[0], "case") || tok_is(&code[0], "default"))) {
    return false;
  }
  if (n == 2 && code[0].kind == TOK_IDENT && tok_is(&code[1], ":")) {
    return false;
  }
  if (n == 1 && tok_is(&code[0], ":")) {
    return !label_colon;
  }
  if (n >= 2 && (tok_is(&code[n - 2u], ")") || tok_is(&code[n - 2u], "=>"))) {
    return true;
  }
  for (i = 0; i < n; i++) {
    if (HAS(HEADER_KEYWORDS, code[i].text, code[i].len)) {
      return true;
    }
  }
  return false;
}

static bool is_declarator_tail(const LogicalLine *line) {
  size_t n = 0;
  size_t i;
  int bracket = 0;
  Tok *code = code_tokens(line, &n);
  bool ok = true;
  if (n == 0) {
    free(code);
    return false;
  }
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], "(") || tok_is(&code[i], ":") || tok_is(&code[i], "=") || tok_is(&code[i], "?")
        || tok_is(&code[i], ".") || tok_is(&code[i], "->") || tok_is(&code[i], "{")) {
      free(code);
      return false;
    }
  }
  if (HAS(DECL_START, code[0].text, code[0].len)) {
    free(code);
    return false;
  }
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], "[")) {
      bracket++;
      continue;
    }
    if (tok_is(&code[i], "]")) {
      if (bracket == 0) {
        ok = false;
        break;
      }
      bracket--;
      continue;
    }
    if (code[i].kind == TOK_IDENT || tok_is(&code[i], "*") || tok_is(&code[i], ",") || tok_is(&code[i], "const")
        || tok_is(&code[i], "volatile") || tok_is(&code[i], "restrict")) {
      continue;
    }
    if (bracket > 0 && (code[i].kind == TOK_NUMBER || tok_is(&code[i], "+") || tok_is(&code[i], "-") || tok_is(&code[i], "/")
            || tok_is(&code[i], "%") || tok_is(&code[i], "~") || tok_is(&code[i], "&") || tok_is(&code[i], "|")
            || tok_is(&code[i], "^") || tok_is(&code[i], "<<") || tok_is(&code[i], ">>"))) {
      continue;
    }
    ok = false;
    break;
  }
  free(code);
  return ok && bracket == 0;
}

static bool is_type_prefix(const Tok *code, size_t n) {
  bool saw = false;
  bool tag = false;
  size_t i;
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], "*")) {
      tag = false;
      saw = true;
      continue;
    }
    if (code[i].kind == TOK_KEYWORD && HAS(TYPE_PREFIX, code[i].text, code[i].len)) {
      tag = tok_is(&code[i], "struct") || tok_is(&code[i], "union") || tok_is(&code[i], "enum");
      saw = true;
      continue;
    }
    if (code[i].kind == TOK_IDENT && tag) {
      tag = false;
      continue;
    }
    return false;
  }
  return saw;
}

static bool parameter_list_line(const Tok *tokens, size_t n) {
  int depth = 0;
  size_t i;
  if (n == 0 || !tok_is(&tokens[0], "(")) {
    return false;
  }
  for (i = 0; i < n; i++) {
    if (tok_is(&tokens[i], "(")) {
      depth++;
    } else if (tok_is(&tokens[i], ")")) {
      depth--;
      if (depth == 0) {
        return i + 1u >= n || tok_is(&tokens[i + 1u], ":");
      }
    }
  }
  return true;
}

static bool finished_aggregate(const Tok *code, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if ((tok_is(&code[i], "struct") || tok_is(&code[i], "union") || tok_is(&code[i], "enum"))
        && i + 1u < n && code[i + 1u].kind == TOK_IDENT && i + 2u == n) {
      return true;
    }
  }
  return false;
}

static bool attribute_continues(const Tok *code, size_t n, const Tok *ahead, size_t an) {
  if (!is_attribute_line(code, n) || an == 0) {
    return false;
  }
  return !HAS(NOT_ATTR_HEAD, ahead[0].text, ahead[0].len);
}

static bool attribute_suffix(const Tok *ahead, size_t an, const Tok *after, size_t after_n) {
  const Tok *body = ahead;
  size_t bn = an;
  if (an == 0) {
    return false;
  }
  if (tok_is(&ahead[an - 1u], ":")) {
    bn = an - 1u;
  }
  if (!is_attribute_line(body, bn)) {
    return false;
  }
  if (tok_is(&ahead[an - 1u], ":")) {
    return true;
  }
  return attribute_continues(body, bn, after, after_n);
}

static bool specifier_continues(const Tok *code, size_t n, const Tok *ahead, size_t an) {
  if (n == 0 || !HAS(SPEC_CALL, code[0].text, code[0].len) || !tok_is(&code[n - 1u], ")")) {
    return false;
  }
  if (an == 0) {
    return false;
  }
  return ahead[0].kind == TOK_IDENT || ahead[0].kind == TOK_KEYWORD || tok_is(&ahead[0], "*") || tok_is(&ahead[0], "[");
}

static bool declaration_continues(const Tok *code, size_t n, const Tok *ahead, size_t an) {
  if (!is_type_prefix(code, n) || an == 0) {
    return false;
  }
  if (ahead[0].kind == TOK_IDENT || tok_is(&ahead[0], "*") || tok_is(&ahead[0], "(")) {
    return true;
  }
  if (finished_aggregate(code, n)) {
    return false;
  }
  return ahead[0].kind == TOK_KEYWORD && HAS(TYPE_PREFIX, ahead[0].text, ahead[0].len);
}

static bool control_continues(const Tok *code, size_t n, const Tok *ahead, size_t an, int line_indent) {
  if (n != 1 || (!tok_is(&code[0], "return") && !tok_is(&code[0], "goto"))) {
    return false;
  }
  return an > 0 && ahead[0].col > line_indent;
}

static BraceKind brace_kind_at(const Tok *code, size_t n, size_t index, const char *hang) {
  const Tok *prev;
  int depth;
  size_t i;
  if (index == 0 || index > n) {
    return (hang != NULL && (strcmp(hang, "=") == 0 || strcmp(hang, ",") == 0)) ? BR_INIT : BR_STMT;
  }
  prev = &code[index - 1u];
  if (tok_is(prev, "=") || tok_is(prev, ",") || tok_is(prev, "{")) {
    return BR_INIT;
  }
  if (tok_is(prev, "(") || !tok_is(prev, ")")) {
    return BR_STMT;
  }
  depth = 0;
  for (i = index; i > 0; i--) {
    const Tok *tok = &code[i - 1u];
    if (tok_is(tok, ")")) {
      depth++;
    } else if (tok_is(tok, "(")) {
      depth--;
      if (depth == 0) {
        const Tok *before = i >= 2u ? &code[i - 2u] : NULL;
        if (before == NULL || tok_is(before, "=") || tok_is(before, ",") || tok_is(before, "return") || tok_is(before, "(")) {
          return BR_INIT;
        }
        if (tok_is(before, "]") || tok_is(before, "struct") || tok_is(before, "union") || tok_is(before, "enum")) {
          return BR_INIT;
        }
        if (before->kind == TOK_KEYWORD && HAS(DECL_START, before->text, before->len) && !HAS(CTRL_NOT_INIT, before->text, before->len)) {
          return BR_INIT;
        }
        return BR_STMT;
      }
    }
  }
  return BR_STMT;
}

static bool at_statement_level(const BraceKind *braces, const int *brace_paren, size_t n, int paren, int bracket) {
  size_t i;
  if (bracket != 0) {
    return false;
  }
  for (i = n; i > 0; i--) {
    if (braces[i - 1u] == BR_INIT) {
      return false;
    }
    if (braces[i - 1u] == BR_STMT) {
      return paren == brace_paren[i - 1u];
    }
  }
  return paren == 0;
}

static void state_copy(LayoutState *dst, const LayoutState *src) {
  dst->ternary = src->ternary;
  dst->paren = src->paren;
  dst->bracket = src->bracket;
  dst->nbraces = src->nbraces;
  dst->bracecap = src->nbraces;
  dst->braces = NULL;
  dst->brace_paren = NULL;
  if (src->nbraces > 0) {
    dst->braces = rt_malloc_array(src->nbraces, sizeof(BraceKind));
    dst->brace_paren = rt_malloc_array(src->nbraces, sizeof(int));
    memcpy(dst->braces, src->braces, src->nbraces * sizeof(BraceKind));
    memcpy(dst->brace_paren, src->brace_paren, src->nbraces * sizeof(int));
  }
}

static void state_free(LayoutState *state) {
  free(state->braces);
  free(state->brace_paren);
  state->braces = NULL;
  state->brace_paren = NULL;
  state->nbraces = 0;
  state->bracecap = 0;
}

static void state_push_brace(LayoutState *state, BraceKind kind, int paren) {
  if (state->nbraces == state->bracecap) {
    size_t cap = state->bracecap;
    state->braces = grow_to(state->braces, &cap, state->nbraces + 1u, sizeof(BraceKind));
    cap = state->bracecap;
    state->brace_paren = grow_to(state->brace_paren, &cap, state->nbraces + 1u, sizeof(int));
    state->bracecap = cap;
  }
  state->braces[state->nbraces] = kind;
  state->brace_paren[state->nbraces] = paren;
  state->nbraces++;
}

static Classified classify_line(const LogicalLine *line, const LayoutState *state, const char *hang, bool label_colon) {
  Classified out;
  size_t n = 0;
  size_t i;
  Tok *code = code_tokens(line, &n);
  LayoutState local;
  bool block_colon = false;
  memset(&out, 0, sizeof out);
  state_copy(&local, state);
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], "(")) {
      local.paren++;
    } else if (tok_is(&code[i], ")") && local.paren > 0) {
      local.paren--;
    } else if (tok_is(&code[i], "[")) {
      local.bracket++;
    } else if (tok_is(&code[i], "]") && local.bracket > 0) {
      local.bracket--;
    } else if (tok_is(&code[i], "{")) {
      BraceKind kind = brace_kind_at(code, n, i, hang);
      if (i == 0 && local.nbraces > 0 && local.braces[local.nbraces - 1u] == BR_INIT) {
        kind = BR_INIT;
      }
      state_push_brace(&local, kind, local.paren);
    } else if (tok_is(&code[i], "}") && local.nbraces > 0) {
      BraceKind kind = local.braces[local.nbraces - 1u];
      local.nbraces--;
      if (kind == BR_INIT && (local.nbraces == 0 || local.braces[local.nbraces - 1u] != BR_INIT)) {
        out.closed_init = true;
      }
    } else if (tok_is(&code[i], "?")) {
      local.ternary++;
    } else if (tok_is(&code[i], ":")) {
      bool is_last = i + 1u == n;
      if (local.ternary > 0) {
        local.ternary--;
      } else if (is_last && at_statement_level(local.braces, local.brace_paren, local.nbraces, local.paren, local.bracket)) {
        block_colon = true;
      }
    }
  }
  out.opens = opens_block(code, n, block_colon, label_colon);
  out.hanging = !out.opens && n > 0 && (HAS(HANGING, code[n - 1u].text, code[n - 1u].len) || local.ternary > 0);
  out.ternary = local.ternary;
  out.paren = local.paren;
  out.bracket = local.bracket;
  out.braces = local.braces;
  out.brace_paren = local.brace_paren;
  out.nbraces = local.nbraces;
  free(code);
  return out;
}

static int effective_paren(int paren, const BraceKind *braces, const int *brace_paren, size_t n) {
  size_t i;
  for (i = n; i > 0; i--) {
    if (braces[i - 1u] == BR_STMT) {
      int diff = paren - brace_paren[i - 1u];
      return diff < 0 ? 0 : diff;
    }
  }
  return paren;
}

static const char *terminator_of(const LogicalLine *line, bool opened, bool hanging, int paren, int bracket, bool in_enum, bool inside_init, bool closed_init, bool join_string) {
  size_t n = 0;
  Tok *code;
  const char *last;
  size_t i;
  bool only_braces;
  if (opened || hanging || paren != 0 || bracket != 0 || join_string) {
    return "";
  }
  code = code_tokens(line, &n);
  if (n == 0) {
    free(code);
    return "";
  }
  last = code[n - 1u].text;
  if (rt_eq_cstr(last, code[n - 1u].len, ";") || rt_eq_cstr(last, code[n - 1u].len, "{") || rt_eq_cstr(last, code[n - 1u].len, ":")) {
    free(code);
    return "";
  }
  if (inside_init || in_enum) {
    bool comma = rt_eq_cstr(last, code[n - 1u].len, ",");
    free(code);
    return comma ? "" : ",";
  }
  only_braces = true;
  for (i = 0; i < n; i++) {
    if (!tok_is(&code[i], "}") && !tok_is(&code[i], "{")) {
      only_braces = false;
    }
  }
  if (tok_is(&code[n - 1u], "}") && only_braces) {
    free(code);
    return closed_init ? ";" : "";
  }
  free(code);
  return ";";
}

static void seg_push(Arena *arena, SegBuf *segs, const char *text, bool inserted) {
  OutSeg *prev;
  if (text == NULL || text[0] == '\0') {
    return;
  }
  if (segs->len > 0) {
    prev = &segs->items[segs->len - 1u];
    if (prev->inserted == inserted) {
      prev->text = arena_join(arena, prev->text, strlen(prev->text), text, strlen(text));
      return;
    }
  }
  if (segs->len == segs->cap) {
    segs->items = grow_to(segs->items, &segs->cap, segs->len + 1u, sizeof(OutSeg));
  }
  segs->items[segs->len].text = text;
  segs->items[segs->len].inserted = inserted;
  segs->len++;
}

static bool ends_space(const char *text) {
  size_t n;
  if (text == NULL) {
    return false;
  }
  n = strlen(text);
  return n > 0 && rt_is_space(text[n - 1u]);
}

static SegBuf render_line(Arena *arena, const LogicalLine *line, ColonMode colon, const char *terminator, bool skip_lead) {
  SegBuf segs;
  size_t last_code = (size_t)-1;
  size_t n;
  memset(&segs, 0, sizeof segs);
  if (!skip_lead) {
    seg_push(arena, &segs, line->lead, false);
  }
  for (n = 0; n < line->ntokens; n++) {
    if (line->tokens[n].kind != TOK_COMMENT) {
      last_code = n;
    }
  }
  for (n = 0; n < line->ntokens; n++) {
    const Tok *tok = &line->tokens[n];
    bool colon_here = n == last_code && tok_is(tok, ":");
    const char *gap = n < line->ngaps && line->gaps[n] != NULL ? line->gaps[n] : "";
    seg_push(arena, &segs, gap, false);
    if (colon_here && colon == COL_BRACE) {
      const char *prev = segs.len > 0 ? segs.items[segs.len - 1u].text : "";
      if (prev[0] != '\0' && !ends_space(prev)) {
        seg_push(arena, &segs, " ", true);
      }
      seg_push(arena, &segs, "{", true);
    } else if (colon_here && colon == COL_BEGIN) {
      seg_push(arena, &segs, " begin", true);
    } else if (colon_here && colon == COL_ARM) {
      seg_push(arena, &segs, ":", false);
      seg_push(arena, &segs, " begin", true);
    } else if (colon_here && colon == COL_SEMI) {
      seg_push(arena, &segs, ";", true);
    } else if (colon_here && colon == COL_DROP) {
      if (terminator != NULL && terminator[0] != '\0') {
        seg_push(arena, &segs, terminator, true);
      }
    } else {
      seg_push(arena, &segs, tok->text, false);
      if (terminator != NULL && terminator[0] != '\0' && n == last_code) {
        seg_push(arena, &segs, terminator, true);
      }
    }
  }
  seg_push(arena, &segs, line->trailing, false);
  return segs;
}

static const char *text_of_segs(Arena *arena, const OutSeg *segs, size_t n) {
  Buf buf;
  size_t i;
  char *out;
  buf_init(&buf);
  for (i = 0; i < n; i++) {
    if (segs[i].text != NULL) {
      buf_puts(&buf, segs[i].text);
    }
  }
  out = buf_arena(&buf, arena);
  buf_free(&buf);
  return out;
}

static const char *text_of_line(Arena *arena, const OutLine *line) {
  return text_of_segs(arena, line->segs, line->nsegs);
}

static void push_out_line(Layout *lx, OutSeg *segs, size_t nsegs, int source_line) {
  OutLine *line;
  if (lx->nout == lx->outcap) {
    lx->out = grow_to(lx->out, &lx->outcap, lx->nout + 1u, sizeof(OutLine));
  }
  line = &lx->out[lx->nout++];
  line->segs = segs;
  line->nsegs = nsegs;
  line->source_line = source_line;
}

static OutSeg *copy_segs(Arena *arena, const OutSeg *segs, size_t n) {
  OutSeg *out;
  if (n == 0) {
    return NULL;
  }
  out = arena_alloc(arena, n * sizeof(OutSeg), sizeof(void *));
  memcpy(out, segs, n * sizeof(OutSeg));
  return out;
}

static void explode_into(Layout *lx, const OutSeg *segs, size_t nsegs, int source_line) {
  OutSeg *current = NULL;
  size_t ncur = 0;
  size_t cap = 0;
  int line_no = source_line;
  size_t s;
  for (s = 0; s < nsegs; s++) {
    const char *text = segs[s].text == NULL ? "" : segs[s].text;
    bool inserted = segs[s].inserted;
    size_t len = strlen(text);
    size_t from = 0;
    size_t i;
    for (i = 0; i <= len; i++) {
      if (i == len || text[i] == '\n') {
        if (i > from) {
          const char *part = arena_copy(lx->arena, text + from, i - from);
          if (ncur > 0 && current[ncur - 1u].inserted == inserted) {
            current[ncur - 1u].text = arena_join(lx->arena, current[ncur - 1u].text, strlen(current[ncur - 1u].text), part, strlen(part));
          } else {
            if (ncur == cap) {
              current = grow_to(current, &cap, ncur + 1u, sizeof(OutSeg));
            }
            current[ncur].text = part;
            current[ncur].inserted = inserted;
            ncur++;
          }
        }
        if (i < len && text[i] == '\n') {
          push_out_line(lx, copy_segs(lx->arena, current, ncur), ncur, line_no);
          free(current);
          current = NULL;
          ncur = 0;
          cap = 0;
          line_no++;
        }
        from = i + 1u;
      }
    }
  }
  push_out_line(lx, copy_segs(lx->arena, current, ncur), ncur, line_no);
  free(current);
}

static char **split_top_commas(Arena *arena, const char *inner, size_t n, size_t *out_n) {
  char **parts = NULL;
  size_t np = 0;
  size_t cap = 0;
  size_t start = 0;
  size_t i;
  int paren = 0;
  int bracket = 0;
  char quote = 0;
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
      if (np == cap) {
        parts = grow_to(parts, &cap, np + 1u, sizeof(char *));
      }
      parts[np++] = arena_copy(arena, inner + start, i - start);
      start = i + 1u;
    }
  }
  if (np == cap) {
    parts = grow_to(parts, &cap, np + 1u, sizeof(char *));
  }
  parts[np++] = arena_copy(arena, inner + start, n - start);
  *out_n = np;
  return parts;
}

static const char *wrap_pascal_init(Arena *arena, const char *text) {
  const char *open;
  const char *close;
  const char *indent_end;
  size_t indent_n;
  size_t np = 0;
  size_t i;
  size_t kept = 0;
  char **parts;
  Buf buf;
  char *out;
  if (text == NULL || strchr(text, ',') == NULL) {
    return text;
  }
  open = strchr(text, '(');
  close = strrchr(text, ')');
  if (open == NULL || close == NULL || close <= open + 1) {
    return text;
  }
  parts = split_top_commas(arena, open + 1, (size_t)(close - open - 1), &np);
  for (i = 0; i < np; i++) {
    const char *s;
    size_t n;
    rt_trim(parts[i], strlen(parts[i]), &s, &n);
    if (n == 0) {
      parts[i] = NULL;
    } else {
      parts[i] = arena_copy(arena, s, n);
      kept++;
    }
  }
  if (kept < 32u) {
    free(parts);
    return text;
  }
  indent_end = text;
  while (*indent_end == ' ' || *indent_end == '\t') {
    indent_end++;
  }
  indent_n = (size_t)(indent_end - text);
  buf_init(&buf);
  {
    const char *head_end = open + 1;
    while (head_end > text && rt_is_space(head_end[-1]) && head_end - 1 > open) {
      head_end--;
    }
    /* trimEnd of head, but keep the '('. */
    {
      const char *head = text;
      size_t hn = (size_t)(open + 1 - text);
      while (hn > 0 && rt_is_space(head[hn - 1u]) && hn > (size_t)(open - text) + 1u) {
        hn--;
      }
      buf_append(&buf, head, hn);
    }
    (void)head_end;
  }
  {
    size_t seen = 0;
    for (i = 0; i < np; i++) {
      if (parts[i] == NULL) {
        continue;
      }
      seen++;
      buf_putc(&buf, '\n');
      buf_append(&buf, text, indent_n);
      buf_puts(&buf, "    ");
      buf_puts(&buf, parts[i]);
      if (seen < kept) {
        buf_putc(&buf, ',');
      }
    }
  }
  buf_putc(&buf, '\n');
  buf_append(&buf, text, indent_n);
  {
    const char *tail = close;
    size_t tn;
    while (*tail != '\0' && rt_is_space(*tail) && tail == close) {
      tail++;
    }
    /* trim() of the tail starting at ')'. */
    rt_trim(close, strlen(close), &tail, &tn);
    buf_append(&buf, tail, tn);
  }
  out = buf_arena(&buf, arena);
  buf_free(&buf);
  free(parts);
  return out;
}

static char **pascal_words(Arena *arena, const Tok *code, size_t n, size_t *out_n) {
  char **words = NULL;
  size_t nw = 0;
  size_t cap = 0;
  size_t i;
  for (i = 0; i < n; i++) {
    if (code[i].kind == TOK_DIRECTIVE || tok_is(&code[i], ":")) {
      continue;
    }
    if (nw == cap) {
      words = grow_to(words, &cap, nw + 1u, sizeof(char *));
    }
    words[nw++] = arena_lower(arena, code[i].text, code[i].len);
  }
  *out_n = nw;
  return words;
}

static bool words_has(char **words, size_t n, const char *word) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (strcmp(words[i], word) == 0) {
      return true;
    }
  }
  return false;
}

static ColonMode routine_colon(const Tok *code, size_t n) {
  size_t i;
  if (n < 2u) {
    return COL_SEMI;
  }
  for (i = n - 1u; i > 0; i--) {
    if (code[i - 1u].kind == TOK_DIRECTIVE) {
      continue;
    }
    return tok_is(&code[i - 1u], ";") ? COL_DROP : COL_SEMI;
  }
  return COL_SEMI;
}

static PascalStyle style_closed(void) {
  PascalStyle style;
  memset(&style, 0, sizeof style);
  style.colon = COL_KEEP;
  style.opener = OP_NONE;
  style.closer = CL_END;
  style.present = true;
  return style;
}

static PascalStyle pascal_style(Arena *arena, const Tok *code, size_t n) {
  PascalStyle style = style_closed();
  size_t nw = 0;
  char **words;
  const char *prev;
  const char *type_word;
  size_t type_at;
  bool objc;
  if (n == 0 || !tok_is(&code[n - 1u], ":")) {
    style.open = false;
    return style;
  }
  words = pascal_words(arena, code, n, &nw);
  prev = nw == 0 ? "" : words[nw - 1u];
  type_at = nw == 0 ? 0 : nw - 1u;
  while (type_at > 0 && HAS_C(TYPE_TAIL, words[type_at])) {
    type_at--;
  }
  type_word = nw == 0 ? "" : words[type_at];
  if (words_has(words, nw, "procedure") || words_has(words, nw, "function") || words_has(words, nw, "constructor")
      || words_has(words, nw, "destructor") || words_has(words, nw, "operator")) {
    style.open = true;
    style.colon = routine_colon(code, n);
    style.opener = OP_BEGIN;
    style.closer = CL_END;
    style.delayed = true;
    free(words);
    return style;
  }
  if (words_has(words, nw, "program") || words_has(words, nw, "library")) {
    style.open = true;
    style.colon = routine_colon(code, n);
    style.opener = OP_BEGIN;
    style.closer = CL_END;
    style.delayed = true;
    style.dot = true;
    free(words);
    return style;
  }
  if (strcmp(prev, "repeat") == 0) {
    style.open = true;
    style.colon = COL_DROP;
    style.opener = OP_NONE;
    style.closer = CL_UNTIL;
    free(words);
    return style;
  }
  if (strcmp(prev, "asm") == 0) {
    style.open = true;
    style.colon = COL_DROP;
    style.opener = OP_NONE;
    style.closer = CL_END;
    style.asm_frame = true;
    free(words);
    return style;
  }
  if (strcmp(prev, "try") == 0) {
    style.open = true;
    style.colon = COL_DROP;
    style.opener = OP_NONE;
    style.closer = CL_END;
    style.keep_open = true;
    free(words);
    return style;
  }
  if (strcmp(prev, "of") == 0 && words_has(words, nw, "case")) {
    style.open = true;
    style.colon = COL_DROP;
    style.opener = OP_NONE;
    style.closer = CL_END;
    free(words);
    return style;
  }
  objc = words_has(words, nw, "objcclass") || words_has(words, nw, "objccategory") || words_has(words, nw, "objcprotocol");
  if (strcmp(type_word, "record") == 0 || strcmp(type_word, "object") == 0 || strcmp(type_word, "dispinterface") == 0
      || strcmp(type_word, "objcclass") == 0 || strcmp(type_word, "objccategory") == 0 || strcmp(type_word, "objcprotocol") == 0
      || (words_has(words, nw, "helper") && words_has(words, nw, "for"))) {
    style.open = true;
    style.colon = COL_DROP;
    style.opener = OP_NONE;
    style.closer = CL_END;
    free(words);
    return style;
  }
  if (strcmp(prev, "then") != 0 && strcmp(prev, "do") != 0 && strcmp(prev, "else") != 0 && strcmp(prev, "otherwise") != 0
      && (strcmp(type_word, "class") == 0 || strcmp(type_word, "interface") == 0 || strcmp(prev, ")") == 0 || objc)
      && (words_has(words, nw, "class") || words_has(words, nw, "object") || words_has(words, nw, "interface")
          || words_has(words, nw, "dispinterface") || objc)
      && !words_has(words, nw, "procedure") && !words_has(words, nw, "function") && !words_has(words, nw, "constructor")
      && !words_has(words, nw, "destructor") && !words_has(words, nw, "operator")) {
    style.open = true;
    style.colon = COL_DROP;
    style.opener = OP_NONE;
    style.closer = CL_END;
    free(words);
    return style;
  }
  if (nw == 1 && strcmp(words[0], "begin") == 0) {
    style.open = true;
    style.colon = COL_DROP;
    style.opener = OP_NONE;
    style.closer = CL_END;
    free(words);
    return style;
  }
  if (strcmp(prev, "then") == 0 || strcmp(prev, "do") == 0 || strcmp(prev, "else") == 0 || strcmp(prev, "otherwise") == 0) {
    style.open = true;
    style.colon = COL_BEGIN;
    style.opener = OP_BEGIN;
    style.closer = CL_END;
    free(words);
    return style;
  }
  if (nw == 1 && (strcmp(words[0], "except") == 0 || strcmp(words[0], "finally") == 0)) {
    style.open = false;
    style.colon = COL_DROP;
    style.opener = OP_NONE;
    style.closer = CL_NONE;
    free(words);
    return style;
  }
  free(words);
  style.open = false;
  return style;
}

static bool depth0_text(const Tok *code, size_t n, const char *text) {
  int paren = 0;
  int bracket = 0;
  size_t i;
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], "(")) {
      paren++;
    } else if (tok_is(&code[i], ")") && paren > 0) {
      paren--;
    } else if (tok_is(&code[i], "[")) {
      bracket++;
    } else if (tok_is(&code[i], "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && tok_is(&code[i], text)) {
      return true;
    }
  }
  return false;
}

static bool angles_balance(const Tok *code, size_t n) {
  int depth = 0;
  size_t i;
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], "<")) {
      depth++;
    } else if (tok_is(&code[i], ">")) {
      if (depth == 0) {
        return false;
      }
      depth--;
    }
  }
  return depth == 0;
}

static bool pascal_declaration(Arena *arena, const Tok *code, size_t n) {
  size_t nw = 0;
  char **words = pascal_words(arena, code, n, &nw);
  const char *head = nw == 0 ? "" : words[0];
  bool result;
  int paren = 0;
  int bracket = 0;
  size_t i;
  if (HAS_C(DECL_HEAD, head) || HAS_C(ROUTINE_HEAD, head)) {
    free(words);
    return true;
  }
  if (nw == 1 && HAS_C(MODIFIER, head)) {
    free(words);
    return true;
  }
  if (strcmp(head, "class") == 0 && nw > 1 && HAS_C(CLASS_MEMBER, words[1])) {
    free(words);
    return true;
  }
  if (depth0_text(code, n, ":=")) {
    free(words);
    return false;
  }
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], "(")) {
      paren++;
    } else if (tok_is(&code[i], ")") && paren > 0) {
      paren--;
    } else if (tok_is(&code[i], "[")) {
      bracket++;
    } else if (tok_is(&code[i], "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && tok_is(&code[i], ":") && i + 1u < n && strcmp(head, "on") != 0) {
      free(words);
      return true;
    }
  }
  if (HAS_C(NOT_DECL_HEAD, head)) {
    free(words);
    return false;
  }
  result = depth0_text(code, n, "=");
  free(words);
  return result;
}

static const char *pascal_end_modifier(Arena *arena, const LogicalLine *line) {
  size_t n = 0;
  Tok *code = code_tokens(line, &n);
  const char *word;
  const char *out = NULL;
  if (n == 0) {
    free(code);
    return NULL;
  }
  word = arena_lower(arena, code[0].text, code[0].len);
  if (!HAS_C(END_MOD, word)) {
    free(code);
    return NULL;
  }
  if (n == 1) {
    free(code);
    return word;
  }
  if (n == 2 && (code[1].kind == TOK_STRING || code[1].kind == TOK_CHAR)) {
    out = arena_printf(arena, "%s %s", word, code[1].text);
  }
  free(code);
  return out;
}

static bool pascal_try_section(Arena *arena, const LogicalLine *line) {
  size_t n = 0;
  size_t i;
  size_t words = 0;
  const char *only = NULL;
  Tok *code = code_tokens(line, &n);
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], ":")) {
      continue;
    }
    words++;
    only = arena_lower(arena, code[i].text, code[i].len);
  }
  free(code);
  return words == 1 && only != NULL && (strcmp(only, "except") == 0 || strcmp(only, "finally") == 0);
}

/* Forward layout: spaced source to host tokens the compiler already accepts. */
typedef struct Group {
  Arena *arena;
  LogicalLine *lines;
  size_t n;
  size_t cap;
  int line_no;
  bool has;
  LogicalLine current;
} Group;

static void group_push(Group *group, LogicalLine line) {
  if (group->n == group->cap) {
    group->lines = grow_to(group->lines, &group->cap, group->n + 1u, sizeof(LogicalLine));
  }
  group->lines[group->n++] = line;
}

static void group_flush(Group *group) {
  if (group->has) {
    group_push(group, group->current);
  }
  group->has = false;
}

static void apply_gap(Group *group, const char *gap, size_t glen, const Tok *tok) {
  size_t parts = 1;
  size_t i;
  size_t start;
  size_t part_i;
  for (i = 0; i < glen; i++) {
    if (gap[i] == '\n') {
      parts++;
    }
  }
  if (parts == 1u) {
    const char *piece = arena_copy(group->arena, gap, glen);
    if (tok != NULL) {
      if (!group->has) {
        group->current = blank_line(group->arena, group->line_no, arena_cstr(group->arena, ""));
        group->has = true;
      }
      if (group->current.ntokens == 0) {
        group->current.lead = arena_join(group->arena, group->current.lead, strlen(group->current.lead), piece, glen);
        line_add_tok(group->arena, &group->current, *tok, arena_cstr(group->arena, ""));
      } else {
        line_add_tok(group->arena, &group->current, *tok, piece);
      }
    } else if (group->has) {
      group->current.trailing = arena_join(group->arena, group->current.trailing, strlen(group->current.trailing), piece, glen);
    } else if (glen > 0) {
      group->current = blank_line(group->arena, group->line_no, piece);
      group->has = true;
    }
    return;
  }
  start = 0;
  part_i = 0;
  for (i = 0; i <= glen; i++) {
    if (i < glen && gap[i] != '\n') {
      continue;
    }
    {
      const char *piece = arena_copy(group->arena, gap + start, i - start);
      size_t plen = i - start;
      if (part_i == 0) {
        if (group->has) {
          group->current.trailing = arena_join(group->arena, group->current.trailing, strlen(group->current.trailing), piece, plen);
          group_flush(group);
        } else {
          group_push(group, blank_line(group->arena, group->line_no, piece));
        }
      } else if (part_i + 1u < parts) {
        group->line_no++;
        group_push(group, blank_line(group->arena, group->line_no, piece));
      } else {
        group->line_no++;
        if (tok != NULL) {
          group->current = blank_line(group->arena, group->line_no, piece);
          line_add_tok(group->arena, &group->current, *tok, arena_cstr(group->arena, ""));
          group->has = true;
        } else if (plen > 0) {
          group->current = blank_line(group->arena, group->line_no, piece);
          group->has = true;
        } else {
          group->has = false;
        }
      }
      part_i++;
      start = i + 1u;
    }
  }
}

static LogicalLine *group_lines(Arena *arena, const char *source, size_t nsrc, const Tok *tokens, size_t nt, size_t *out_n) {
  Group group;
  size_t pos = 0;
  size_t i;
  memset(&group, 0, sizeof group);
  group.arena = arena;
  group.line_no = 1;
  for (i = 0; i < nt; i++) {
    size_t from = pos;
    size_t to = tokens[i].start;
    if (to < from) {
      to = from;
    }
    if (to > nsrc) {
      to = nsrc;
    }
    apply_gap(&group, source + from, to - from, &tokens[i]);
    pos = tokens[i].end;
    if (pos > nsrc) {
      pos = nsrc;
    }
  }
  apply_gap(&group, source + pos, nsrc > pos ? nsrc - pos : 0, NULL);
  group_flush(&group);
  *out_n = group.n;
  return group.lines;
}

static void hold(Layout *lx, LogicalLine line) {
  if (lx->nheld == lx->heldcap) {
    lx->held = grow_to(lx->held, &lx->heldcap, lx->nheld + 1u, sizeof(LogicalLine));
  }
  lx->held[lx->nheld++] = line;
}

static bool in_dead(const Layout *lx) {
  size_t i;
  for (i = 0; i < lx->ndead; i++) {
    if (lx->dead_flag[i]) {
      return true;
    }
  }
  return false;
}

static void push_dead(Layout *lx, bool if0, bool dead) {
  if (lx->ndead == lx->deadcap) {
    size_t cap = lx->deadcap;
    lx->dead_if0 = grow_to(lx->dead_if0, &cap, lx->ndead + 1u, sizeof(bool));
    cap = lx->deadcap;
    lx->dead_flag = grow_to(lx->dead_flag, &cap, lx->ndead + 1u, sizeof(bool));
    lx->deadcap = cap;
  }
  lx->dead_if0[lx->ndead] = if0;
  lx->dead_flag[lx->ndead] = dead;
  lx->ndead++;
}

static void push_pp(Layout *lx, int base) {
  if (lx->npp == lx->ppcap) {
    lx->pp_brace = grow_to(lx->pp_brace, &lx->ppcap, lx->npp + 1u, sizeof(int));
  }
  lx->pp_brace[lx->npp++] = base;
}

static void push_stack(Layout *lx, Frame frame) {
  if (lx->nstack == lx->stackcap) {
    lx->stack = grow_to(lx->stack, &lx->stackcap, lx->nstack + 1u, sizeof(Frame));
  }
  lx->stack[lx->nstack++] = frame;
}

static int col_of(const LogicalLine *line) {
  return line->ntokens == 0 ? 1 : line->tokens[0].col;
}

static void emit_logical(Layout *lx, const LogicalLine *line, const char *terminator) {
  SegBuf segs = render_line(lx->arena, line, COL_KEEP, terminator, false);
  explode_into(lx, segs.items, segs.len, line->no);
  free(segs.items);
}

static void flush_held(Layout *lx) {
  size_t i;
  for (i = 0; i < lx->nheld; i++) {
    LogicalLine *line = &lx->held[i];
    emit_logical(lx, line, "");
    if (line_kind(line) == LK_DIRECTIVE) {
      char *text = join_text(lx->arena, line->tokens, line->ntokens);
      CondKind role = cond_kind(lx->arena, text);
      if (lx->nstack > 0) {
        Frame *top = &lx->stack[lx->nstack - 1u];
        if (top->delayed_begin && !top->body_started) {
          if (role == CK_OPEN) {
            top->cond_depth++;
          } else if (role == CK_CLOSE && top->cond_depth > 0) {
            top->cond_depth--;
          }
        }
        if (lx->profile->id == LANG_PASCAL && indent_of(line) >= top->body_indent) {
          if (role == CK_OPEN) {
            top->body_cond++;
          } else if (role == CK_CLOSE && top->body_cond > 0) {
            top->body_cond--;
          }
        }
      }
    }
  }
  lx->nheld = 0;
}

static void flush_closing(Layout *lx, Frame *frame) {
  size_t i;
  int closed = 0;
  int prior = frame->body_cond;
  for (i = 0; i < lx->nheld; i++) {
    if (line_kind(&lx->held[i]) != LK_DIRECTIVE || indent_of(&lx->held[i]) < frame->body_indent) {
      continue;
    }
    if (cond_kind(lx->arena, join_text(lx->arena, lx->held[i].tokens, lx->held[i].ntokens)) == CK_CLOSE) {
      closed++;
    }
  }
  flush_held(lx);
  frame->body_cond = prior - closed;
  if (frame->body_cond < 0) {
    frame->body_cond = 0;
  }
}

static void close_frame(Layout *lx, Frame *frame, int source_line, const char *tail, bool open_decl, bool with_semi) {
  size_t pad_n = frame->header_indent > 0 ? (size_t)(frame->header_indent - 1) : 0;
  const char *pad = arena_repeat(lx->arena, ' ', pad_n);
  const char *brace;
  OutSeg seg;
  if (frame->closer == CL_UNTIL || frame->closer == CL_NONE || frame->skip_shell) {
    frame->body_started = true;
    return;
  }
  if (frame->delayed_begin && !frame->body_started && frame->opener == OP_BEGIN) {
    seg.text = arena_printf(lx->arena, "%sbegin", pad);
    seg.inserted = true;
    explode_into(lx, &seg, 1, source_line);
    frame->body_started = true;
  }
  if (frame->closer == CL_END) {
    if (frame->dot) {
      brace = "end.";
    } else if (tail != NULL) {
      brace = arena_printf(lx->arena, "end %s;", tail);
    } else if (with_semi) {
      brace = "end;";
    } else {
      brace = "end";
    }
  } else if (tail != NULL) {
    brace = open_decl ? arena_printf(lx->arena, "} %s", tail) : arena_printf(lx->arena, "} %s;", tail);
  } else {
    brace = frame->semi_on_close ? "};" : "}";
  }
  seg.text = arena_printf(lx->arena, "%s%s", pad, brace);
  seg.inserted = true;
  explode_into(lx, &seg, 1, source_line);
  if (strcmp(frame->name, "do") == 0) {
    lx->expect_while = true;
  }
}

static bool statement_name(const char *name) {
  return set_has_ci(STMT_NAMES, sizeof STMT_NAMES / sizeof STMT_NAMES[0], name);
}

static LogicalLine *take_trailing_closes(Layout *lx, int min_col, size_t *out_n) {
  LogicalLine *prelim = NULL;
  size_t n = 0;
  size_t cap = 0;
  int last_close = -1;
  size_t i;
  for (i = 0; i < lx->nheld; i++) {
    LineKind kind = line_kind(&lx->held[i]);
    int col;
    if (kind == LK_BLANK) {
      if (n == cap) {
        prelim = grow_to(prelim, &cap, n + 1u, sizeof(LogicalLine));
      }
      prelim[n++] = lx->held[i];
      continue;
    }
    col = col_of(&lx->held[i]);
    if (col < min_col) {
      break;
    }
    if (kind == LK_COMMENT) {
      if (n == cap) {
        prelim = grow_to(prelim, &cap, n + 1u, sizeof(LogicalLine));
      }
      prelim[n++] = lx->held[i];
      continue;
    }
    if (kind == LK_DIRECTIVE && cond_kind(lx->arena, join_text(lx->arena, lx->held[i].tokens, lx->held[i].ntokens)) == CK_CLOSE) {
      if (n == cap) {
        prelim = grow_to(prelim, &cap, n + 1u, sizeof(LogicalLine));
      }
      prelim[n++] = lx->held[i];
      last_close = (int)n - 1;
      continue;
    }
    break;
  }
  if (last_close < 0) {
    free(prelim);
    *out_n = 0;
    return NULL;
  }
  n = (size_t)last_close + 1u;
  memmove(lx->held, lx->held + n, (lx->nheld - n) * sizeof(LogicalLine));
  lx->nheld -= n;
  *out_n = n;
  return prelim;
}

static void unshift_held(Layout *lx, const LogicalLine *lines, size_t n) {
  size_t i;
  if (n == 0) {
    return;
  }
  while (lx->nheld + n > lx->heldcap) {
    lx->held = grow_to(lx->held, &lx->heldcap, lx->nheld + n, sizeof(LogicalLine));
  }
  memmove(lx->held + n, lx->held, lx->nheld * sizeof(LogicalLine));
  for (i = 0; i < n; i++) {
    lx->held[i] = lines[i];
  }
  lx->nheld += n;
}

static void close_statement(Layout *lx, Frame *frame, int source_line, bool attaches) {
  bool statement = statement_name(frame->name);
  bool in_case = lx->nstack > 0 && strcmp(lx->stack[lx->nstack - 1u].name, "case") == 0;
  bool can_move = statement && !in_case && lx->profile->id == LANG_PASCAL && frame->closer == CL_END && !frame->dot && !frame->skip_shell;
  size_t nclose = 0;
  LogicalLine *closes = can_move ? take_trailing_closes(lx, frame->header_indent, &nclose) : NULL;
  size_t keep = 0;
  size_t i;
  if (nclose > 0 && frame->body_cond > 0) {
    int remaining = frame->body_cond;
    for (i = 0; i < nclose; i++) {
      if (line_kind(&closes[i]) == LK_DIRECTIVE
          && cond_kind(lx->arena, join_text(lx->arena, closes[i].tokens, closes[i].ntokens)) == CK_CLOSE) {
        remaining--;
        keep = i + 1u;
        if (remaining == 0) {
          break;
        }
      }
    }
    if (remaining > 0) {
      keep = nclose;
    }
  }
  for (i = 0; i < keep; i++) {
    emit_logical(lx, &closes[i], "");
  }
  if (attaches) {
    unshift_held(lx, closes + keep, nclose - keep);
    close_frame(lx, frame, source_line, NULL, false, true);
    free(closes);
    return;
  }
  close_frame(lx, frame, source_line, NULL, false, nclose == keep);
  if (nclose == keep) {
    free(closes);
    return;
  }
  for (i = keep; i < nclose; i++) {
    emit_logical(lx, &closes[i], "");
  }
  if (lx->nout > 0) {
    OutLine *last = &lx->out[lx->nout - 1u];
    OutSeg *next = arena_alloc(lx->arena, (last->nsegs + 1u) * sizeof(OutSeg), sizeof(void *));
    if (last->nsegs > 0) {
      memcpy(next, last->segs, last->nsegs * sizeof(OutSeg));
    }
    next[last->nsegs].text = ";";
    next[last->nsegs].inserted = true;
    last->segs = next;
    last->nsegs++;
  }
  free(closes);
}

static void partition_held(Layout *lx, int limit, bool strict_gt, LogicalLine **inner, size_t *ni, LogicalLine **outer, size_t *no) {
  size_t i;
  *inner = NULL;
  *outer = NULL;
  *ni = 0;
  *no = 0;
  for (i = 0; i < lx->nheld; i++) {
    int col = col_of(&lx->held[i]);
    bool inside = strict_gt ? col > limit : col >= limit;
    if (inside) {
      *inner = grow_to(*inner, ni, *ni + 1u, sizeof(LogicalLine));
      /* grow_to updates *ni as cap if we pass ni as cap. Don't. */
    }
  }
  free(*inner);
  *inner = rt_malloc_array(lx->nheld + 1u, sizeof(LogicalLine));
  *outer = rt_malloc_array(lx->nheld + 1u, sizeof(LogicalLine));
  *ni = 0;
  *no = 0;
  for (i = 0; i < lx->nheld; i++) {
    int col = col_of(&lx->held[i]);
    bool inside = strict_gt ? col > limit : col >= limit;
    if (inside) {
      (*inner)[(*ni)++] = lx->held[i];
    } else {
      (*outer)[(*no)++] = lx->held[i];
    }
  }
  lx->nheld = 0;
}

static void held_set(Layout *lx, const LogicalLine *lines, size_t n) {
  size_t i;
  lx->nheld = 0;
  for (i = 0; i < n; i++) {
    hold(lx, lines[i]);
  }
}

static void close_empty_pending(Layout *lx, Frame *frame, int source_line, const char *tail, bool attaches) {
  LogicalLine *inner = NULL;
  LogicalLine *outer = NULL;
  size_t ni = 0;
  size_t no = 0;
  size_t i;
  partition_held(lx, frame->header_indent, true, &inner, &ni, &outer, &no);
  if (ni > 0 && frame->delayed_begin && !frame->body_started && frame->opener == OP_BEGIN) {
    OutSeg seg;
    size_t pad_n = frame->header_indent > 0 ? (size_t)(frame->header_indent - 1) : 0;
    seg.text = arena_printf(lx->arena, "%sbegin", arena_repeat(lx->arena, ' ', pad_n));
    seg.inserted = true;
    explode_into(lx, &seg, 1, frame->line);
    frame->body_started = true;
  }
  held_set(lx, inner, ni);
  flush_closing(lx, frame);
  for (i = 0; i < no; i++) {
    hold(lx, outer[i]);
  }
  if (tail != NULL) {
    close_frame(lx, frame, source_line, tail, false, true);
  } else {
    close_statement(lx, frame, source_line, attaches);
  }
  note_push(&lx->notes, frame->line, arena_printf(lx->arena, "%s has no indented body.", frame->name));
  free(inner);
  free(outer);
}

static size_t next_code_at(const Layout *lx, size_t from) {
  size_t i;
  for (i = from + 1u; i < lx->nlogical; i++) {
    if (line_kind(&lx->logical[i]) == LK_CODE) {
      return i;
    }
  }
  return (size_t)-1;
}

static size_t prev_code_at(const Layout *lx, size_t from) {
  size_t i = from;
  while (i > 0) {
    i--;
    if (line_kind(&lx->logical[i]) == LK_CODE) {
      return i;
    }
  }
  return (size_t)-1;
}

static bool is_label_colon(Layout *lx, size_t index, const LogicalLine *line) {
  size_t n = 0;
  Tok *toks = code_tokens(line, &n);
  size_t prev_i;
  bool yes = false;
  if (!(n == 1 && tok_is(&toks[0], ":"))) {
    free(toks);
    return false;
  }
  free(toks);
  prev_i = prev_code_at(lx, index);
  if (prev_i == (size_t)-1) {
    return false;
  }
  toks = code_tokens(&lx->logical[prev_i], &n);
  if (n == 1 && toks[0].kind == TOK_IDENT) {
    yes = true;
  } else if (n > 0 && (tok_is(&toks[0], "case") || tok_is(&toks[0], "default"))) {
    yes = true;
  }
  free(toks);
  return yes;
}

static bool unbalanced_line(const LogicalLine *line) {
  size_t n = 0;
  Tok *code = code_tokens(line, &n);
  int paren = 0;
  int bracket = 0;
  size_t i;
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], "(")) {
      paren++;
    } else if (tok_is(&code[i], ")") && paren > 0) {
      paren--;
    } else if (tok_is(&code[i], "[")) {
      bracket++;
    } else if (tok_is(&code[i], "]") && bracket > 0) {
      bracket--;
    }
  }
  free(code);
  return paren > 0 || bracket > 0;
}

static Tok *pascal_cue_tokens(Layout *lx, size_t index, const LogicalLine *line, int entering_paren, size_t *out_n) {
  size_t own_n = 0;
  Tok *own = code_tokens(line, &own_n);
  bool needs_prev;
  const char *last_own;
  Tok **chunks = NULL;
  size_t *lens = NULL;
  size_t nchunks = 0;
  size_t cap = 0;
  size_t i;
  size_t total;
  Tok *out;
  size_t at;
  if (own_n == 0) {
    *out_n = 0;
    return own;
  }
  needs_prev = entering_paren > 0 || (own_n == 1 && tok_is(&own[0], ":"));
  last_own = own[own_n - 1u].text;
  if (!needs_prev || (entering_paren > 0 && (rt_eq_cstr(last_own, own[own_n - 1u].len, ",") || rt_eq_cstr(last_own, own[own_n - 1u].len, ";")))) {
    *out_n = own_n;
    return own;
  }
  for (i = index; i > 0; i--) {
    size_t n = 0;
    Tok *toks;
    size_t nw = 0;
    char **words;
    bool cue;
    const char *last;
    if (line_kind(&lx->logical[i - 1u]) != LK_CODE) {
      continue;
    }
    toks = code_tokens(&lx->logical[i - 1u], &n);
    if (nchunks == cap) {
      chunks = grow_to(chunks, &cap, nchunks + 1u, sizeof(Tok *));
      cap = nchunks;
      lens = grow_to(lens, &cap, nchunks + 1u, sizeof(size_t));
    }
    chunks[nchunks] = toks;
    lens[nchunks] = n;
    nchunks++;
    words = pascal_words(lx->arena, toks, n, &nw);
    cue = false;
    {
      size_t w;
      for (w = 0; w < nw; w++) {
        if (HAS_C(CUE_WORDS, words[w])) {
          cue = true;
        }
      }
    }
    free(words);
    last = n == 0 ? "" : toks[n - 1u].text;
    if (cue || (!unbalanced_line(&lx->logical[i - 1u]) && !rt_eq_cstr(last, n == 0 ? 0 : toks[n - 1u].len, "(") && !rt_eq_cstr(last, n == 0 ? 0 : toks[n - 1u].len, ","))) {
      break;
    }
  }
  total = own_n;
  for (i = 0; i < nchunks; i++) {
    total += lens[i];
  }
  out = rt_malloc_array(total + 1u, sizeof(Tok));
  at = 0;
  for (i = nchunks; i > 0; i--) {
    memcpy(out + at, chunks[i - 1u], lens[i - 1u] * sizeof(Tok));
    at += lens[i - 1u];
    free(chunks[i - 1u]);
  }
  memcpy(out + at, own, own_n * sizeof(Tok));
  free(own);
  free(chunks);
  free(lens);
  *out_n = total;
  return out;
}

static int statement_start_indent(Layout *lx, size_t index, const LogicalLine *line) {
  int indent = indent_of(line);
  int paren = 0;
  int bracket = 0;
  size_t n = 0;
  Tok *toks = code_tokens(line, &n);
  size_t i;
  size_t t;
  for (t = 0; t < n; t++) {
    if (tok_is(&toks[t], "(")) {
      paren++;
    } else if (tok_is(&toks[t], ")")) {
      paren--;
    } else if (tok_is(&toks[t], "[")) {
      bracket++;
    } else if (tok_is(&toks[t], "]")) {
      bracket--;
    }
  }
  free(toks);
  if (paren < 0 || bracket < 0) {
    for (i = index; i > 0 && (paren < 0 || bracket < 0); i--) {
      if (line_kind(&lx->logical[i - 1u]) != LK_CODE) {
        continue;
      }
      if (indent_of(&lx->logical[i - 1u]) < indent) {
        indent = indent_of(&lx->logical[i - 1u]);
      }
      toks = code_tokens(&lx->logical[i - 1u], &n);
      for (t = 0; t < n; t++) {
        if (tok_is(&toks[t], "(")) {
          paren++;
        } else if (tok_is(&toks[t], ")")) {
          paren--;
        } else if (tok_is(&toks[t], "[")) {
          bracket++;
        } else if (tok_is(&toks[t], "]")) {
          bracket--;
        }
      }
      free(toks);
    }
    return indent;
  }
  for (i = index; i > 0; i--) {
    const char *last;
    bool continues;
    if (line_kind(&lx->logical[i - 1u]) != LK_CODE) {
      continue;
    }
    toks = code_tokens(&lx->logical[i - 1u], &n);
    last = n == 0 ? "" : toks[n - 1u].text;
    if (n > 0 && tok_is(&toks[n - 1u], "(")) {
      if (indent_of(&lx->logical[i - 1u]) < indent) {
        indent = indent_of(&lx->logical[i - 1u]);
      }
      free(toks);
      break;
    }
    continues = (n > 0 && (tok_is(&toks[n - 1u], ",") || tok_is(&toks[n - 1u], "\\"))) || is_type_prefix(toks, n)
      || (n > 0 && !tok_is(&toks[n - 1u], ":") && HAS(HANGING, toks[n - 1u].text, toks[n - 1u].len))
      || unbalanced_line(&lx->logical[i - 1u]);
    free(toks);
    (void)last;
    if (!continues) {
      break;
    }
    if (indent_of(&lx->logical[i - 1u]) < indent) {
      indent = indent_of(&lx->logical[i - 1u]);
    }
  }
  return indent;
}

static bool directive_before_next(const Layout *lx, size_t index, size_t until) {
  size_t i;
  if (until == (size_t)-1) {
    return false;
  }
  for (i = index + 1u; i < lx->nlogical; i++) {
    LineKind kind;
    if (i == until) {
      return false;
    }
    kind = line_kind(&lx->logical[i]);
    if (kind == LK_DIRECTIVE) {
      return true;
    }
    if (kind == LK_CODE) {
      return false;
    }
  }
  return false;
}

static bool branch_ends_before_body(Layout *lx, size_t index) {
  size_t i;
  for (i = index + 1u; i < lx->nlogical; i++) {
    LineKind kind = line_kind(&lx->logical[i]);
    CondKind role;
    if (kind == LK_BLANK || kind == LK_COMMENT) {
      continue;
    }
    if (kind != LK_DIRECTIVE) {
      return false;
    }
    role = cond_kind(lx->arena, join_text(lx->arena, lx->logical[i].tokens, lx->logical[i].ntokens));
    return role == CK_CLOSE || role == CK_MID;
  }
  return false;
}

static bool single_statement_body(Layout *lx, size_t index, int header_indent) {
  int body_indent = -1;
  int statements = 0;
  size_t i;
  for (i = index + 1u; i < lx->nlogical; i++) {
    LineKind kind = line_kind(&lx->logical[i]);
    int col;
    if (kind == LK_BLANK || kind == LK_COMMENT || kind == LK_DIRECTIVE) {
      continue;
    }
    col = indent_of(&lx->logical[i]);
    if (col <= header_indent) {
      break;
    }
    if (body_indent < 0) {
      body_indent = col;
    }
    if (col < body_indent) {
      break;
    }
    if (col == body_indent) {
      statements++;
      if (statements > 1) {
        return false;
      }
    }
  }
  return true;
}

static bool is_open_header(Layout *lx, size_t index, const LogicalLine *line, int paren) {
  size_t n = 0;
  Tok *code = code_tokens(line, &n);
  bool has_paren = false;
  size_t i;
  int header;
  bool yes = false;
  if (paren != 0 || n == 0 || (code[0].kind != TOK_KEYWORD && code[0].kind != TOK_IDENT) || !tok_is(&code[n - 1u], ")")) {
    free(code);
    return false;
  }
  for (i = 0; i < n; i++) {
    if (tok_is(&code[i], "(")) {
      has_paren = true;
    }
  }
  if (!has_paren) {
    free(code);
    return false;
  }
  free(code);
  header = indent_of(line);
  for (i = index + 1u; i < lx->nlogical; i++) {
    size_t cn = 0;
    Tok *toks;
    if (line_kind(&lx->logical[i]) != LK_CODE) {
      continue;
    }
    toks = code_tokens(&lx->logical[i], &cn);
    if (cn == 1 && tok_is(&toks[0], ":")) {
      yes = true;
      free(toks);
      break;
    }
    if (indent_of(&lx->logical[i]) > header) {
      free(toks);
      continue;
    }
    free(toks);
    break;
  }
  return yes;
}

static bool arm_opens_brace(Layout *lx, size_t from) {
  int net = 0;
  bool *if0 = NULL;
  bool *dead = NULL;
  size_t nd = 0;
  size_t cap = 0;
  size_t i;
  for (i = from + 1u; i < lx->nlogical; i++) {
    LineKind kind = line_kind(&lx->logical[i]);
    if (kind == LK_DIRECTIVE) {
      char *text = join_text(lx->arena, lx->logical[i].tokens, lx->logical[i].ntokens);
      CondKind role = cond_kind(lx->arena, text);
      bool zero = hash_if_zero(lx->arena, text, false);
      bool any = false;
      size_t d;
      for (d = 0; d < nd; d++) {
        if (dead[d]) {
          any = true;
        }
      }
      if (role == CK_OPEN) {
        if (nd == cap) {
          size_t c2 = cap;
          if0 = grow_to(if0, &c2, nd + 1u, sizeof(bool));
          c2 = cap;
          dead = grow_to(dead, &c2, nd + 1u, sizeof(bool));
          cap = c2;
        }
        if0[nd] = zero;
        dead[nd] = any || zero;
        nd++;
      } else if (role == CK_CLOSE) {
        if (nd == 0) {
          break;
        }
        nd--;
      } else if (role == CK_MID) {
        bool outer = false;
        if (nd == 0) {
          break;
        }
        for (d = 0; d + 1u < nd; d++) {
          if (dead[d]) {
            outer = true;
          }
        }
        if (if0[nd - 1u] && !outer) {
          dead[nd - 1u] = hash_if_zero(lx->arena, text, true);
        }
      }
      continue;
    }
    {
      bool any = false;
      size_t d;
      size_t n = 0;
      Tok *code;
      for (d = 0; d < nd; d++) {
        if (dead[d]) {
          any = true;
        }
      }
      if (any || kind != LK_CODE) {
        continue;
      }
      code = code_tokens(&lx->logical[i], &n);
      for (d = 0; d < n; d++) {
        if (tok_is(&code[d], "{")) {
          net++;
        } else if (tok_is(&code[d], "}")) {
          net--;
        }
      }
      free(code);
    }
  }
  free(if0);
  free(dead);
  return net > 0;
}

static bool param_list_across(Layout *lx, size_t from) {
  size_t own_n = 0;
  Tok *own = code_tokens(&lx->logical[from], &own_n);
  size_t next;
  size_t between;
  bool splits = false;
  size_t after;
  bool yes = false;
  if (own_n == 0 || !is_type_prefix(own, own_n - 1u) || own[own_n - 1u].kind != TOK_IDENT) {
    free(own);
    return false;
  }
  free(own);
  next = next_code_at(lx, from);
  if (next == (size_t)-1 || indent_of(&lx->logical[next]) != indent_of(&lx->logical[from])) {
    return false;
  }
  for (between = from + 1u; between < next; between++) {
    if (line_kind(&lx->logical[between]) == LK_DIRECTIVE
        && cond_kind(lx->arena, join_text(lx->arena, lx->logical[between].tokens, lx->logical[between].ntokens)) == CK_MID) {
      splits = true;
    }
  }
  if (!splits) {
    return false;
  }
  after = next_code_at(lx, next);
  if (after == (size_t)-1) {
    return false;
  }
  own = code_tokens(&lx->logical[after], &own_n);
  yes = parameter_list_line(own, own_n);
  free(own);
  return yes;
}

static const char *trim_copy(Arena *arena, const char *text) {
  const char *s;
  size_t n;
  rt_trim(text, text == NULL ? 0 : strlen(text), &s, &n);
  return arena_copy(arena, s, n);
}

static bool stack_body_ok(const Layout *lx, int indent) {
  size_t i;
  for (i = 0; i < lx->nstack; i++) {
    if (indent < lx->stack[i].body_indent) {
      return false;
    }
  }
  return true;
}

static void adopt_state(Layout *lx, Classified *next) {
  free(lx->state.braces);
  free(lx->state.brace_paren);
  lx->state.ternary = next->ternary;
  lx->state.paren = next->paren;
  lx->state.bracket = next->bracket;
  lx->state.braces = next->braces;
  lx->state.brace_paren = next->brace_paren;
  lx->state.nbraces = next->nbraces;
  lx->state.bracecap = next->nbraces;
  next->braces = NULL;
  next->brace_paren = NULL;
}

static void free_logical(LogicalLine *lines, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    free(lines[i].tokens);
    free(lines[i].gaps);
  }
  free(lines);
}

Result layout_source(const char *source, size_t len, LangId lang) {
  Arena *arena = arena_new();
  const Profile *profile = profile_get(lang);
  LexOut lexed;
  Layout lx;
  size_t index;
  Result result;
  const char *tab;
  memset(&lx, 0, sizeof lx);
  memset(&result, 0, sizeof result);
  lx.arena = arena;
  lx.profile = profile;
  note_init(&lx.notes, arena);
  lex_source(arena, source == NULL ? "" : source, source == NULL ? 0 : len, profile, &lexed);
  {
    size_t i;
    for (i = 0; i < lexed.nnotes; i++) {
      note_push(&lx.notes, lexed.notes[i].line, lexed.notes[i].message);
    }
  }
  lx.logical = group_lines(arena, lexed.source, lexed.sourcelen, lexed.tokens, lexed.ntokens, &lx.nlogical);
  tab = memchr(lexed.source, '\t', lexed.sourcelen);
  if (tab != NULL) {
    int line = 1;
    size_t i;
    for (i = 0; i < (size_t)(tab - lexed.source); i++) {
      if (lexed.source[i] == '\n') {
        line++;
      }
    }
    note_unshift(&lx.notes, line, "Tabs are preserved. Indent columns use stops of 8, so spaces are safer.");
  }
  lx.hang_indent = 1;
  lx.hang_text = "";
  for (index = 0; index < lx.nlogical; index++) {
    LogicalLine *line = &lx.logical[index];
    LineKind kind = line_kind(line);
    size_t code_n = 0;
    Tok *code = NULL;
    if (kind == LK_DIRECTIVE) {
      char *text = join_text(arena, line->tokens, line->ntokens);
      int col = col_of(line);
      CondKind role = cond_kind(arena, text);
      if (profile->id == LANG_C) {
        if (lx.has_pending && col > lx.pending.header_indent) {
          Frame body = lx.pending;
          body.body_indent = col;
          push_stack(&lx, body);
          lx.has_pending = false;
        }
        while (lx.nstack > 0 && col < lx.stack[lx.nstack - 1u].body_indent) {
          Frame top = lx.stack[lx.nstack - 1u];
          LogicalLine *inner = NULL;
          LogicalLine *outer = NULL;
          size_t ni = 0;
          size_t no = 0;
          size_t h;
          partition_held(&lx, top.body_indent, false, &inner, &ni, &outer, &no);
          held_set(&lx, inner, ni);
          flush_held(&lx);
          for (h = 0; h < no; h++) {
            hold(&lx, outer[h]);
          }
          close_frame(&lx, &top, line->no, NULL, false, true);
          lx.nstack--;
          free(inner);
          free(outer);
        }
        if (lx.has_pending && col <= lx.pending.header_indent) {
          close_frame(&lx, &lx.pending, line->no, NULL, false, true);
          lx.has_pending = false;
        }
      }
      if (role == CK_OPEN) {
        push_pp(&lx, (int)lx.state.nbraces);
        push_dead(&lx, hash_if_zero(arena, text, false), in_dead(&lx) || hash_if_zero(arena, text, false));
      } else if (role == CK_MID && lx.ndead > 0) {
        bool outer = false;
        size_t d;
        for (d = 0; d + 1u < lx.ndead; d++) {
          if (lx.dead_flag[d]) {
            outer = true;
          }
        }
        if (lx.dead_if0[lx.ndead - 1u] && !outer) {
          lx.dead_flag[lx.ndead - 1u] = hash_if_zero(arena, text, true);
        } else if (!in_dead(&lx) && lx.npp > 0) {
          int base = lx.pp_brace[lx.npp - 1u];
          if (arm_opens_brace(&lx, index) && (int)lx.state.nbraces > base) {
            lx.state.nbraces = (size_t)base;
          }
        }
      } else if (role == CK_CLOSE) {
        if (lx.npp > 0) {
          lx.npp--;
        }
        if (lx.ndead > 0) {
          lx.ndead--;
        }
      }
      hold(&lx, *line);
      continue;
    }
    if (kind != LK_CODE) {
      hold(&lx, *line);
      continue;
    }
    code = code_tokens(line, &code_n);
    if (profile->id == LANG_PASCAL && lx.hanging && indent_of(line) <= lx.hang_indent) {
      const char *head = code_n == 0 ? "" : lower_tok(arena, &code[0]);
      if (HAS_C(FRESH_HEAD, head)) {
        if (lx.nout > 0) {
          const char *prev = trim_copy(arena, text_of_line(arena, &lx.out[lx.nout - 1u]));
          size_t pn = strlen(prev);
          while (pn > 0 && (prev[pn - 1u] == ' ' || prev[pn - 1u] == '\t')) {
            pn--;
          }
          if (pn > 0 && prev[pn - 1u] != ';' && prev[pn - 1u] != ':') {
            OutLine *last = &lx.out[lx.nout - 1u];
            OutSeg *next = arena_alloc(arena, (last->nsegs + 1u) * sizeof(OutSeg), sizeof(void *));
            if (last->nsegs > 0) {
              memcpy(next, last->segs, last->nsegs * sizeof(OutSeg));
            }
            next[last->nsegs].text = ";";
            next[last->nsegs].inserted = true;
            last->segs = next;
            last->nsegs++;
          }
        }
        lx.hanging = false;
      }
    }
    {
      bool suppressed = lx.hanging || effective_paren(lx.state.paren, lx.state.braces, lx.state.brace_paren, lx.state.nbraces) > 0
        || lx.state.bracket > 0 || (lx.state.nbraces > 0 && lx.state.braces[lx.state.nbraces - 1u] == BR_INIT);
      int closed = 0;
      int last_closed = -1;
      bool absorbed = false;
      bool absorbed_hang = false;
      size_t ahead_i = next_code_at(&lx, index);
      int indent = indent_of(line);
      bool bitfield = false;
      if (code_n > 0 && tok_is(&code[code_n - 1u], ":") && ahead_i != (size_t)-1) {
        size_t an = 0;
        Tok *ahead = code_tokens(&lx.logical[ahead_i], &an);
        if (an > 0 && ahead[0].len > 0 && rt_is_digit(ahead[0].text[0])) {
          bitfield = true;
        }
        free(ahead);
      }
      if (!suppressed) {
        const char *head_now = profile->id == LANG_PASCAL ? (code_n == 0 ? "" : lower_tok(arena, &code[0])) : (code_n == 0 ? "" : code[0].text);
        if (lx.has_pending && indent <= lx.pending.header_indent
            && !(lx.pending.wants_tail && indent == lx.pending.header_indent && is_declarator_tail(line))) {
          const char *end_mod = profile->id == LANG_PASCAL && indent == lx.pending.header_indent ? pascal_end_modifier(arena, line) : NULL;
          bool attaches_now = strcmp(head_now, "else") == 0 || strcmp(head_now, "otherwise") == 0 || strcmp(head_now, "catch") == 0;
          close_empty_pending(&lx, &lx.pending, line->no, end_mod, attaches_now);
          if (end_mod != NULL) {
            absorbed = true;
          }
          last_closed = lx.pending.header_indent;
          closed++;
          lx.has_pending = false;
        }
        while (lx.nstack > 0) {
          Frame *top = &lx.stack[lx.nstack - 1u];
          LogicalLine *inner = NULL;
          LogicalLine *outer = NULL;
          size_t ni = 0;
          size_t no = 0;
          size_t h;
          bool decl_tail;
          bool attr_tail;
          const char *end_mod;
          bool tail;
          const char *word;
          bool attaches;
          if (indent >= top->body_indent) {
            break;
          }
          if (bitfield && indent > indent_of(line)) {
            break;
          }
          if (profile->id == LANG_PASCAL && top->keep_open && indent == top->header_indent && pascal_try_section(arena, line)) {
            const char *section = code_n == 0 ? "" : lower_tok(arena, &code[0]);
            if (top->arm != NULL && top->arm[0] != '\0' && section[0] != '\0' && strcmp(top->arm, section) != 0) {
              note_push(&lx.notes, line->no, arena_printf(arena, "This try already has %s. A Pascal try takes except or finally, not both — nest a second try.", top->arm));
            }
            if (section[0] != '\0') {
              top->arm = section;
            }
            break;
          }
          partition_held(&lx, top->body_indent, false, &inner, &ni, &outer, &no);
          held_set(&lx, inner, ni);
          flush_closing(&lx, top);
          for (h = 0; h < no; h++) {
            hold(&lx, outer[h]);
          }
          free(inner);
          free(outer);
          lx.nstack--;
          last_closed = top->header_indent;
          decl_tail = top->wants_tail && is_declarator_tail(line);
          attr_tail = top->closer == CL_RBRACE && top->semi_on_close && is_attribute_line(code, code_n);
          end_mod = profile->id == LANG_PASCAL ? pascal_end_modifier(arena, line) : NULL;
          tail = !absorbed && stack_body_ok(&lx, indent) && indent == top->header_indent && (decl_tail || attr_tail || end_mod != NULL);
          if (tail) {
            SegBuf rendered = render_line(arena, line, COL_KEEP, "", false);
            const char *shown = trim_copy(arena, text_of_segs(arena, rendered.items, rendered.len));
            bool open_decl = decl_tail && code_n > 0 && tok_is(&code[code_n - 1u], ",");
            close_frame(&lx, top, line->no, decl_tail || attr_tail ? shown : end_mod, open_decl, true);
            absorbed = true;
            if (open_decl) {
              absorbed_hang = true;
            }
            free(rendered.items);
          } else {
            word = profile->id == LANG_PASCAL ? (code_n == 0 ? "" : lower_tok(arena, &code[0])) : (code_n == 0 ? "" : code[0].text);
            attaches = strcmp(word, "else") == 0 || strcmp(word, "otherwise") == 0 || strcmp(word, "catch") == 0;
            close_statement(&lx, top, line->no, attaches);
          }
          closed++;
        }
        if (closed > 0 && !absorbed) {
          bool aligned = indent == 1 || (lx.state.nbraces > 0 && lx.state.braces[lx.state.nbraces - 1u] == BR_STMT);
          size_t s;
          if (!aligned) {
            for (s = 0; s < lx.nstack; s++) {
              if (lx.stack[s].body_indent == indent || lx.stack[s].header_indent == indent) {
                aligned = true;
              }
            }
          }
          if (!aligned && profile->id != LANG_PASCAL) {
            note_push(&lx.notes, line->no, arena_printf(arena, "Indent column %d is not an open block boundary.", indent));
          }
        }
        if (!absorbed && lx.has_pending) {
          if (indent > lx.pending.header_indent) {
            Frame body = lx.pending;
            body.body_indent = indent;
            push_stack(&lx, body);
          } else if (lx.pending.wants_tail && indent == lx.pending.header_indent && is_declarator_tail(line)) {
            SegBuf rendered = render_line(arena, line, COL_KEEP, "", false);
            bool open_decl = code_n > 0 && tok_is(&code[code_n - 1u], ",");
            close_frame(&lx, &lx.pending, line->no, trim_copy(arena, text_of_segs(arena, rendered.items, rendered.len)), open_decl, true);
            absorbed = true;
            if (open_decl) {
              absorbed_hang = true;
            }
            note_push(&lx.notes, lx.pending.line, arena_printf(arena, "%s has no indented body.", lx.pending.name));
            free(rendered.items);
          } else {
            const char *pending_head = profile->id == LANG_PASCAL ? (code_n == 0 ? "" : lower_tok(arena, &code[0])) : (code_n == 0 ? "" : code[0].text);
            bool attaches = strcmp(pending_head, "else") == 0 || strcmp(pending_head, "otherwise") == 0 || strcmp(pending_head, "catch") == 0;
            close_statement(&lx, &lx.pending, line->no, attaches);
            last_closed = lx.pending.header_indent;
            if (closed == 0 && attaches) {
              closed = 1;
            }
            note_push(&lx.notes, lx.pending.line, arena_printf(arena, "%s has no indented body.", lx.pending.name));
          }
          lx.has_pending = false;
        }
      }
      if (absorbed) {
        lx.hanging = absorbed_hang;
        lx.hang_text = absorbed_hang ? "," : "";
        if (absorbed_hang) {
          lx.hang_indent = indent;
        }
        lx.expect_while = false;
        free(code);
        continue;
      }
      {
        const char *head_word = profile->id == LANG_PASCAL ? (code_n == 0 ? "" : lower_tok(arena, &code[0])) : (code_n == 0 ? "" : code[0].text);
        bool do_while = lx.expect_while && strcmp(head_word, "while") == 0;
        bool directive_between = false;
        bool glue_word;
        bool attaches;
        bool glue;
        bool only_trivia = true;
        size_t h;
        OutSeg *prefix = NULL;
        size_t nprefix = 0;
        Frame *frame_now = lx.nstack > 0 ? &lx.stack[lx.nstack - 1u] : NULL;
        bool opening_body;
        Classified next;
        size_t ahead = next_code_at(&lx, index);
        size_t ahead_n = 0;
        Tok *ahead_code = NULL;
        size_t after = ahead == (size_t)-1 ? (size_t)-1 : next_code_at(&lx, ahead);
        size_t after_n = 0;
        Tok *after_code = NULL;
        const char *head_text = code_n == 0 ? NULL : code[0].text;
        bool lone_colon_next;
        bool join_string;
        bool in_enum;
        bool indented_star;
        bool cont;
        size_t style_n = 0;
        Tok *style_source = NULL;
        PascalStyle style;
        bool opens;
        ColonMode colon;
        const char *base_term;
        const char *term;
        bool line_hanging;
        int eff;
        memset(&style, 0, sizeof style);
        lx.expect_while = false;
        for (h = 0; h < lx.nheld; h++) {
          if (line_kind(&lx.held[h]) == LK_DIRECTIVE) {
            directive_between = true;
          }
          if (line_kind(&lx.held[h]) == LK_CODE) {
            only_trivia = false;
          }
        }
        glue_word = strcmp(head_word, "else") == 0 || strcmp(head_word, "catch") == 0 || strcmp(head_word, "otherwise") == 0
          || (profile->id != LANG_PASCAL && strcmp(head_word, "finally") == 0) || do_while;
        attaches = closed >= 1 && last_closed == indent;
        glue = code_n > 0 && (code[0].kind == TOK_KEYWORD || profile->id == LANG_PASCAL) && glue_word && attaches && !directive_between && only_trivia;
        if (!glue && directive_between && attaches && glue_word && lx.nout > 0) {
          const char *glued = text_of_line(arena, &lx.out[lx.nout - 1u]);
          const char *trim = glued;
          while (*trim == ' ' || *trim == '\t') {
            trim++;
          }
          if (strcmp(trim, "end;") == 0) {
            OutSeg seg;
            lx.nout--;
            seg.text = arena_cstr(arena, "end");
            /* Preserve leading spaces of the original end line when they were the whole text minus end;. */
            if (glued != trim) {
              seg.text = arena_printf(arena, "%.*send", (int)(trim - glued), glued);
            }
            seg.inserted = true;
            explode_into(&lx, &seg, 1, line->no);
            flush_held(&lx);
          }
        }
        if (in_dead(&lx)) {
          flush_held(&lx);
          {
            SegBuf rendered = render_line(arena, line, COL_KEEP, "", false);
            explode_into(&lx, rendered.items, rendered.len, line->no);
            free(rendered.items);
          }
          lx.hanging = false;
          lx.hang_text = "";
          free(code);
          continue;
        }
        if (glue && lx.nout > 0) {
          const char *glued = text_of_line(arena, &lx.out[lx.nout - 1u]);
          const char *trim = glued;
          while (*trim == ' ' || *trim == '\t') {
            trim++;
          }
          if (strcmp(trim, "}") == 0) {
            OutLine *brace = &lx.out[lx.nout - 1u];
            prefix = rt_malloc_array(brace->nsegs + 1u, sizeof(OutSeg));
            if (brace->nsegs > 0) {
              memcpy(prefix, brace->segs, brace->nsegs * sizeof(OutSeg));
            }
            nprefix = brace->nsegs;
            prefix[nprefix].text = " ";
            prefix[nprefix].inserted = false;
            nprefix++;
            lx.nout--;
          } else if (strcmp(trim, "end;") == 0) {
            prefix = rt_malloc_array(1, sizeof(OutSeg));
            prefix[0].text = glued == trim ? "end " : arena_printf(arena, "%.*send ", (int)(trim - glued), glued);
            prefix[0].inserted = true;
            nprefix = 1;
            lx.nout--;
          }
        }
        opening_body = frame_now != NULL && frame_now->delayed_begin && !frame_now->body_started && indent > frame_now->header_indent && !pascal_declaration(arena, code, code_n);
        if (opening_body) {
          /* Header trivia before a delayed begin stays outside the inserted begin. */
          LogicalLine *header_tr = rt_malloc_array(lx.nheld + 1u, sizeof(LogicalLine));
          LogicalLine *body_tr = rt_malloc_array(lx.nheld + 1u, sizeof(LogicalLine));
          size_t nh = 0;
          size_t nb = 0;
          int depth = frame_now->cond_depth;
          bool in_body = false;
          size_t hi;
          for (hi = 0; hi < lx.nheld; hi++) {
            LogicalLine *item = &lx.held[hi];
            if (!in_body) {
              LineKind ik = line_kind(item);
              CondKind role = ik == LK_DIRECTIVE ? cond_kind(arena, join_text(arena, item->tokens, item->ntokens)) : CK_OTHER;
              bool boundary = false;
              size_t look;
              if (role == CK_CLOSE && depth > 0) {
                header_tr[nh++] = *item;
                depth--;
                continue;
              }
              if ((role == CK_MID || role == CK_OPEN) && depth > 0) {
                header_tr[nh++] = *item;
                if (role == CK_OPEN) {
                  depth++;
                }
                continue;
              }
              for (look = hi + 1u; look < lx.nheld; look++) {
                LineKind ak = line_kind(&lx.held[look]);
                if (ak == LK_BLANK || ak == LK_COMMENT) {
                  continue;
                }
                if (ak == LK_DIRECTIVE) {
                  CondKind ar = cond_kind(arena, join_text(arena, lx.held[look].tokens, lx.held[look].ntokens));
                  boundary = ar == CK_CLOSE || ar == CK_MID;
                }
                break;
              }
              if (depth == 0 && (role == CK_CLOSE || role == CK_MID || ((ik == LK_BLANK || ik == LK_COMMENT) && boundary))) {
                header_tr[nh++] = *item;
                continue;
              }
              if (ik != LK_CODE && depth > 0) {
                header_tr[nh++] = *item;
                continue;
              }
              if (col_of(item) <= frame_now->header_indent) {
                header_tr[nh++] = *item;
                continue;
              }
              in_body = true;
            }
            body_tr[nb++] = *item;
          }
          frame_now->cond_depth = depth;
          frame_now->body_started = true;
          {
            size_t nw = 0;
            char **words = pascal_words(arena, code, code_n, &nw);
            bool branch_shell = nw == 1 && (strcmp(words[0], "begin") == 0 || strcmp(words[0], "asm") == 0);
            int around = frame_now->cond_depth;
            if (branch_shell) {
              for (hi = 0; hi < lx.nheld; hi++) {
                CondKind role;
                if (line_kind(&lx.held[hi]) != LK_DIRECTIVE) {
                  continue;
                }
                role = cond_kind(arena, join_text(arena, lx.held[hi].tokens, lx.held[hi].ntokens));
                if (role == CK_OPEN) {
                  around++;
                } else if (role == CK_CLOSE && around > 0) {
                  around--;
                }
              }
            }
            lx.nheld = 0;
            held_set(&lx, header_tr, nh);
            flush_held(&lx);
            if (depth > 0 || around > 0) {
              frame_now->skip_shell = true;
            } else {
              OutSeg seg;
              size_t pad_n = frame_now->header_indent > 0 ? (size_t)(frame_now->header_indent - 1) : 0;
              seg.text = arena_printf(arena, "%sbegin", arena_repeat(arena, ' ', pad_n));
              seg.inserted = true;
              explode_into(&lx, &seg, 1, line->no);
            }
            held_set(&lx, body_tr, nb);
            flush_held(&lx);
            free(words);
          }
          free(header_tr);
          free(body_tr);
        } else {
          flush_held(&lx);
        }
        next = classify_line(line, &lx.state, lx.hang_text, is_label_colon(&lx, index, line));
        if (ahead != (size_t)-1) {
          ahead_code = code_tokens(&lx.logical[ahead], &ahead_n);
        }
        if (after != (size_t)-1) {
          after_code = code_tokens(&lx.logical[after], &after_n);
        }
        lone_colon_next = ahead_n == 1 && tok_is(&ahead_code[0], ":")
          && ((head_text != NULL && HAS(COLON_HEADER, head_text, strlen(head_text))) || (code_n > 0 && tok_is(&code[code_n - 1u], ")"))
              || (code_n == 1 && code[0].kind == TOK_IDENT) || (head_text != NULL && (strcmp(head_text, "case") == 0 || strcmp(head_text, "default") == 0)));
        join_string = code_n > 0 && code[code_n - 1u].kind == TOK_STRING && ahead_n > 0 && ahead_code[0].kind == TOK_STRING;
        in_enum = lx.nstack > 0 && lx.stack[lx.nstack - 1u].kind == FK_ENUM && !(next.nbraces > 0 && next.braces[next.nbraces - 1u] == BR_INIT);
        indented_star = ahead_n > 0 && (tok_is(&ahead_code[0], "*") || tok_is(&ahead_code[0], "&")) && ahead_code[0].col > indent;
        cont = is_open_header(&lx, index, line, next.paren) || lone_colon_next || bitfield
          || (ahead_n > 0 && HAS(CONT_START, ahead_code[0].text, ahead_code[0].len) && !tok_is(&ahead_code[0], "="))
          || (code_n > 0 && is_type_prefix(code, code_n - 1u) && code[code_n - 1u].kind == TOK_IDENT && ahead != (size_t)-1 && lx.logical[ahead].no > line->no + 1 && parameter_list_line(ahead_code, ahead_n))
          || param_list_across(&lx, index)
          || (head_text != NULL && strcmp(head_text, "else") == 0 && (code_n == 1 || (ahead_n > 0 && (tok_is(&ahead_code[0], "if") || tok_is(&ahead_code[0], "do")))))
          || (ahead_n > 1 && tok_is(&ahead_code[0], ":"))
          || indented_star
          || (ahead_n > 0 && tok_is(&ahead_code[0], "=") && !(code_n > 0 && tok_is(&code[code_n - 1u], ")") && depth0_text(code, code_n, "(")))
          || declaration_continues(code, code_n, ahead_code, ahead_n)
          || control_continues(code, code_n, ahead_code, ahead_n, indent)
          || attribute_continues(code, code_n, ahead_code, ahead_n)
          || attribute_suffix(ahead_code, ahead_n, after_code, after_n)
          || specifier_continues(code, code_n, ahead_code, ahead_n)
          || (ahead_n > 0 && tok_is(&ahead_code[0], "(") && ahead_code[0].col > indent);
        if (profile->id == LANG_PASCAL) {
          style_source = pascal_cue_tokens(&lx, index, line, lx.state.paren, &style_n);
          style = pascal_style(arena, style_source, style_n);
        } else {
          style_source = code;
          style_n = code_n;
          style.present = false;
        }
        if (profile->id == LANG_PASCAL && !style.open && code_n > 0 && tok_is(&code[code_n - 1u], ":") && lx.nstack > 0
            && strcmp(lx.stack[lx.nstack - 1u].name, "case") == 0 && indent == lx.stack[lx.nstack - 1u].body_indent
            && ahead != (size_t)-1 && indent_of(&lx.logical[ahead]) > indent) {
          memset(&style, 0, sizeof style);
          style.open = true;
          style.colon = COL_ARM;
          style.opener = OP_BEGIN;
          style.closer = CL_END;
          style.present = true;
        }
        opens = next.opens;
        colon = opens ? COL_BRACE : COL_KEEP;
        if (do_while) {
          opens = false;
          colon = code_n > 0 && tok_is(&code[code_n - 1u], ":") ? COL_DROP : COL_KEEP;
        }
        if (profile->id == LANG_PASCAL && style.open && style.colon == COL_BEGIN && !style.delayed && branch_ends_before_body(&lx, index)) {
          if (single_statement_body(&lx, index, statement_start_indent(&lx, index, line))) {
            style.colon = COL_DROP;
            style.delayed = false;
            style.opener = OP_NONE;
            style.closer = CL_NONE;
          } else {
            style.colon = COL_DROP;
            style.delayed = true;
          }
        }
        if (style.open) {
          opens = true;
          colon = style.colon;
        } else if (style.present && style.colon == COL_DROP) {
          opens = false;
          colon = COL_DROP;
        }
        line_hanging = style.open || (profile->id == LANG_PASCAL && code_n > 0 && tok_is(&code[code_n - 1u], ">") && angles_balance(code, code_n))
            || (profile->id == LANG_PASCAL && pascal_try_section(arena, line))
          ? false
          : next.hanging && !(profile->id == LANG_PASCAL && code_n > 0 && tok_is(&code[code_n - 1u], "^"));
        eff = effective_paren(next.paren, next.braces, next.brace_paren, next.nbraces);
        base_term = terminator_of(line, opens, line_hanging, eff, next.bracket, in_enum, next.nbraces > 0 && next.braces[next.nbraces - 1u] == BR_INIT, next.closed_init, join_string);
        term = (strcmp(base_term, ";") == 0 && cont) ? "" : base_term;
        if (do_while && !(code_n > 0 && tok_is(&code[code_n - 1u], ";")) && !cont && eff == 0 && next.bracket == 0
            && ((code_n > 0 && tok_is(&code[code_n - 1u], ":")) || !line_hanging)) {
          term = ";";
        }
        if ((head_text != NULL && (strcmp(head_text, "if") == 0 || strcmp(head_text, "while") == 0 || strcmp(head_text, "for") == 0 || strcmp(head_text, "switch") == 0))
            && !do_while && code_n > 0 && tok_is(&code[code_n - 1u], ")") && directive_before_next(&lx, index, ahead)) {
          term = "";
        }
        if (strcmp(term, ",") == 0 && ahead_n > 0 && tok_is(&ahead_code[0], ",")) {
          term = "";
        }
        if (strcmp(term, ";") == 0 && ahead_n == 1 && tok_is(&ahead_code[0], ";")) {
          term = "";
        }
        if (profile->id == LANG_PASCAL) {
          size_t nw = 0;
          char **words = pascal_words(arena, code, code_n, &nw);
          const char *bare = nw == 0 ? "" : words[0];
          char pair[128];
          const Tok *tail_tok = code_n == 0 ? NULL : &code[code_n - 1u];
          size_t ti;
          if (lx.nstack > 0 && lx.stack[lx.nstack - 1u].asm_frame) {
            term = "";
          }
          snprintf(pair, sizeof pair, "%s %s", bare, nw > 1 ? words[1] : "");
          if ((HAS_C(PASCAL_BARE, bare) && nw == 1) || (nw == 2 && HAS_C(PASCAL_BARE, pair))) {
            term = "";
          }
          if (strcmp(bare, "class") == 0 && nw == 2 && HAS_C(CLASS_SECTION, words[1])) {
            term = "";
          }
          if (code_n > 0 && tok_is(&code[code_n - 1u], ".") && words_has(words, nw, "end")) {
            term = "";
          }
          for (ti = code_n; ti > 0; ti--) {
            if (code[ti - 1u].kind == TOK_DIRECTIVE) {
              continue;
            }
            tail_tok = &code[ti - 1u];
            break;
          }
          if (tail_tok != NULL && tok_is(tail_tok, ";")) {
            term = "";
          }
          free(words);
        }
        {
          SegBuf rendered = render_line(arena, line, colon, term, prefix != NULL);
          OutSeg *all = NULL;
          size_t na = nprefix + rendered.len;
          if (na > 0) {
            all = rt_malloc_array(na, sizeof(OutSeg));
          }
          if (nprefix > 0) {
            memcpy(all, prefix, nprefix * sizeof(OutSeg));
          }
          if (rendered.len > 0) {
            memcpy(all + nprefix, rendered.items, rendered.len * sizeof(OutSeg));
          }
          explode_into(&lx, all, na, line->no);
          free(all);
          free(rendered.items);
          free(prefix);
        }
        if (profile->id == LANG_PASCAL && lx.nout > 0) {
          const char *joined = text_of_line(arena, &lx.out[lx.nout - 1u]);
          const char *wrapped = strchr(joined, ',') != NULL ? wrap_pascal_init(arena, joined) : joined;
          if (wrapped != joined) {
            const char *p = wrapped;
            lx.nout--;
            while (*p != '\0') {
              const char *nl = strchr(p, '\n');
              size_t piece_n = nl == NULL ? strlen(p) : (size_t)(nl - p);
              OutSeg seg;
              seg.text = arena_copy(arena, p, piece_n);
              seg.inserted = false;
              explode_into(&lx, &seg, 1, line->no);
              if (nl == NULL) {
                break;
              }
              p = nl + 1;
            }
          }
        }
        if (opens) {
          FrameKind fk = FK_BLOCK;
          bool wants = false;
          size_t nw = 0;
          char **pwords = profile->id == LANG_PASCAL ? pascal_words(arena, code, code_n, &nw) : NULL;
          Frame opened;
          memset(&opened, 0, sizeof opened);
          aggregate_info(code, code_n, &fk, &wants);
          opened.header_indent = statement_start_indent(&lx, index, line);
          opened.body_indent = indent;
          opened.semi_on_close = semi_on_close(style_source, style_n, profile);
          opened.name = (pwords != NULL && words_has(pwords, nw, "case") && nw > 0 && strcmp(pwords[nw - 1u], "of") == 0)
            ? "case" : header_name(arena, style_source, style_n);
          opened.kind = fk;
          opened.wants_tail = wants;
          opened.line = line->no;
          opened.opener = style.open || style.present ? style.opener : OP_LBRACE;
          opened.closer = style.open || style.present ? style.closer : CL_RBRACE;
          if (!(style.open || style.present)) {
            opened.opener = OP_LBRACE;
            opened.closer = CL_RBRACE;
          } else {
            opened.opener = style.opener;
            opened.closer = style.closer;
          }
          opened.delayed_begin = style.delayed;
          opened.dot = style.dot;
          opened.asm_frame = style.asm_frame;
          opened.keep_open = style.keep_open;
          opened.arm = "";
          lx.pending = opened;
          lx.has_pending = true;
          free(pwords);
        }
        adopt_state(&lx, &next);
        lx.hanging = line_hanging;
        lx.hang_indent = indent;
        lx.hang_text = line_hanging && code_n > 0 ? code[code_n - 1u].text : "";
        if (profile->id == LANG_PASCAL && style_source != code) {
          free(style_source);
        }
        free(ahead_code);
        free(after_code);
        free(code);
        code = NULL;
      }
    }
  }
  while (lx.nheld > 0 && line_kind(&lx.held[lx.nheld - 1u]) == LK_BLANK) {
    lx.nheld--;
  }
  if (lx.has_pending) {
    LogicalLine *inner = NULL;
    LogicalLine *outer = NULL;
    size_t ni = 0;
    size_t no = 0;
    int src = lx.nlogical == 0 ? lx.pending.line : lx.logical[lx.nlogical - 1u].no;
    partition_held(&lx, lx.pending.header_indent, true, &inner, &ni, &outer, &no);
    if (ni > 0 && lx.pending.delayed_begin && !lx.pending.body_started && lx.pending.opener == OP_BEGIN) {
      OutSeg seg;
      size_t pad_n = lx.pending.header_indent > 0 ? (size_t)(lx.pending.header_indent - 1) : 0;
      seg.text = arena_printf(arena, "%sbegin", arena_repeat(arena, ' ', pad_n));
      seg.inserted = true;
      explode_into(&lx, &seg, 1, lx.pending.line);
      lx.pending.body_started = true;
    }
    held_set(&lx, inner, ni);
    flush_closing(&lx, &lx.pending);
    close_statement(&lx, &lx.pending, src, false);
    note_push(&lx.notes, lx.pending.line, arena_printf(arena, "%s has no indented body.", lx.pending.name));
    lx.has_pending = false;
    held_set(&lx, outer, no);
    free(inner);
    free(outer);
  }
  while (lx.nstack > 0) {
    Frame frame = lx.stack[lx.nstack - 1u];
    LogicalLine *inner = NULL;
    LogicalLine *outer = NULL;
    size_t ni = 0;
    size_t no = 0;
    int src = lx.nlogical == 0 ? 1 : lx.logical[lx.nlogical - 1u].no;
    partition_held(&lx, frame.body_indent, false, &inner, &ni, &outer, &no);
    held_set(&lx, inner, ni);
    flush_closing(&lx, &frame);
    held_set(&lx, outer, no);
    lx.nstack--;
    close_statement(&lx, &frame, src, false);
    free(inner);
    free(outer);
  }
  flush_held(&lx);
  if (lx.state.nbraces > 0) {
    note_push(&lx.notes, 1, "Unclosed { in the source.");
  }
  if (lx.state.paren > 0) {
    note_push(&lx.notes, 1, "Unclosed ( in the source.");
  }
  {
    Buf buf;
    size_t i;
    buf_init(&buf);
    for (i = 0; i < lx.nout; i++) {
      if (i > 0) {
        buf_putc(&buf, '\n');
      }
      buf_puts(&buf, text_of_line(arena, &lx.out[i]));
    }
    if (lx.nout > 0) {
      buf_putc(&buf, '\n');
    }
    result.code = buf_arena(&buf, arena);
    buf_free(&buf);
  }
  {
    OutLine *copied = NULL;
    if (lx.nout > 0) {
      copied = arena_alloc(arena, lx.nout * sizeof(OutLine), sizeof(void *));
      memcpy(copied, lx.out, lx.nout * sizeof(OutLine));
    }
    result.lines = copied;
    result.nlines = lx.nout;
  }
  result.arena = arena;
  result.language = lang;
  result.notes = lx.notes.items;
  result.nnotes = lx.notes.len;
  free_logical(lx.logical, lx.nlogical);
  free(lx.out);
  free(lx.stack);
  free(lx.pp_brace);
  free(lx.dead_if0);
  free(lx.dead_flag);
  free(lx.held);
  state_free(&lx.state);
  return result;
}

