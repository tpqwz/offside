/* Host C, Java, or JavaScript back into the spaced dialect.
   Behavior-preserving translation of src/lib/layout/spaced.ts. */
#include "engine.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct Str {
  const char *s;
  size_t n;
} Str;

typedef enum NodeKind {
  NK_TEXT = 0,
  NK_BLANK,
  NK_RAW,
  NK_COMMENT,
  NK_KEEP,
  NK_SEXPR,
  NK_COLON,
  NK_GROUP
} NodeKind;

typedef enum ColonWhere {
  WHERE_INLINE = 0,
  WHERE_OWN,
  WHERE_READY
} ColonWhere;

typedef enum BraceRole {
  ROLE_INIT = 0,
  ROLE_EXPR,
  ROLE_COLON,
  ROLE_KEEP
} BraceRole;

typedef enum DirRole {
  DIR_OPEN = 0,
  DIR_CLOSE,
  DIR_MID,
  DIR_OTHER
} DirRole;

typedef struct Node Node;

typedef struct NodeVec {
  Node **items;
  size_t len;
  size_t cap;
} NodeVec;

struct Node {
  NodeKind kind;
  Str text;
  Str suffix;
  Str tail;
  Str follow;
  ColonWhere where;
  bool has_lead;
  NodeVec lead;
  NodeVec kids;
};

typedef struct StrVec {
  Str *items;
  size_t len;
  size_t cap;
} StrVec;

typedef struct Span {
  size_t start;
  size_t end;
} Span;

typedef struct Mix {
  bool sexpr;
  Str text;
  NodeVec body;
} Mix;

typedef struct MixVec {
  Mix *items;
  size_t len;
  size_t cap;
} MixVec;

typedef struct Converter {
  Arena *arena;
  const char *source;
  size_t sourcelen;
  LangId lang;
  Tok *code;
  int ncode;
  Tok *tokens;
  int ntokens;
  NoteBuf notes;
  size_t mark;
  int trivia_at;
  Span *spans;
  size_t nspans;
  bool spans_ready;
} Converter;

typedef struct Parsed {
  Node *node;
  int next;
  size_t end;
} Parsed;

typedef struct SeqOut {
  NodeVec nodes;
  int next;
} SeqOut;

typedef struct BodyOut {
  NodeVec nodes;
  NodeVec lead;
  int next;
  size_t end;
} BodyOut;

static const char *const CONTROL_WORDS[] = {
  "if", "while", "for", "switch", "catch", "synchronized"
};
static const char *const AGG_WORDS[] = {
  "struct", "union", "enum", "class", "interface", "record"
};
static const char *const KR_WORDS[] = {
  "int", "char", "short", "long", "float", "double", "void", "signed", "unsigned",
  "const", "volatile", "restrict", "register", "static", "extern", "auto",
  "struct", "union", "enum", "inline", "_Bool", "_Complex", "_Imaginary"
};
static const char *const HEADER_WORDS[] = {
  "else", "do", "struct", "union", "enum", "class", "interface", "record", "try", "finally"
};

static Str str_make(const char *s, size_t n);
static Str str_empty(void);
static Str str_lit(const char *s);
static Str str_trim(Str s);
static Str str_rtrim_ws(Str s);
static Str str_rtrim_ht(Arena *arena, Str s);
static Str str_join(Arena *arena, Str a, Str b);
static bool str_eq_c(Str a, const char *b);
static bool str_has_nonspace(Str s);
static bool str_has_blank(Str s);
static bool str_starts(Str a, const char *prefix);
static Str src_slice(const Converter *cv, size_t from, size_t to);
static Str str_strip_line_ht(Arena *arena, Str s);

static void nodevec_push(Arena *arena, NodeVec *v, Node *node);
static Node *node_new(Arena *arena, NodeKind kind);
static Node *node_text(Arena *arena, Str text);
static Node *node_kind_text(Arena *arena, NodeKind kind, Str text);
static bool is_empty_node(const Node *node);
static bool vec_has_raw(const NodeVec *v);

static const Tok *code_at(const Converter *cv, int i);
static bool tok_is(const Tok *tok, const char *s);
static bool text_in(const char *s, size_t n, const char *const *words, size_t nwords);
static bool tok_in(const Tok *tok, const char *const *words, size_t nwords);

static SeqOut parse_seq(Converter *cv, int i, bool stop_brace);
static Parsed parse_stmt(Converter *cv, int i);
static Parsed parse_label(Converter *cv, int i);
static Parsed parse_case(Converter *cv, int i);
static Parsed parse_control(Converter *cv, int i);
static Parsed parse_else(Converter *cv, int i);
static Parsed parse_do(Converter *cv, int i);
static Parsed parse_try(Converter *cv, int i);
static Parsed parse_finally(Converter *cv, int i);
static BodyOut parse_body(Converter *cv, int i);
static Parsed parse_open_brace(Converter *cv, int i);
static Parsed parse_general(Converter *cv, int i);
static Parsed finish_colon(Converter *cv, int start, int brace);
static void render_node(Converter *cv, const Node *node, int indent, StrVec *out);
static NodeVec pull_trivia(Converter *cv, size_t until);
static Str strip_semis(Converter *cv, int from, int to);
static size_t offset_after(Converter *cv, int next);
static int match_tok(Converter *cv, int open, const char *left, const char *right);
static int scan_semi(const Converter *cv, int i);
static BraceRole brace_role(Converter *cv, int index);

static Str str_empty(void) {
  Str r;
  r.s = "";
  r.n = 0;
  return r;
}

static Str str_make(const char *s, size_t n) {
  Str r;
  if (s == NULL || n == 0) {
    return str_empty();
  }
  r.s = s;
  r.n = n;
  return r;
}

static Str str_lit(const char *s) {
  if (s == NULL) {
    return str_empty();
  }
  return str_make(s, strlen(s));
}

static Str str_trim(Str s) {
  const char *p;
  size_t n;
  if (s.s == NULL) {
    return str_empty();
  }
  rt_trim(s.s, s.n, &p, &n);
  return str_make(p, n);
}

static Str str_rtrim_ws(Str s) {
  if (s.s == NULL) {
    return str_empty();
  }
  while (s.n > 0 && rt_is_space(s.s[s.n - 1u])) {
    s.n--;
  }
  if (s.n == 0) {
    return str_empty();
  }
  return s;
}

static Str str_rtrim_ht(Arena *arena, Str s) {
  Buf buf;
  Str r;
  size_t m;
  if (s.s == NULL || s.n == 0) {
    return str_empty();
  }
  if (s.s[s.n - 1u] == '\n') {
    m = s.n - 1u;
    while (m > 0 && (s.s[m - 1u] == ' ' || s.s[m - 1u] == '\t')) {
      m--;
    }
    if (m < s.n - 1u) {
      buf_init(&buf);
      buf_append(&buf, s.s, m);
      buf_putc(&buf, '\n');
      r.n = buf.len;
      r.s = buf_arena(&buf, arena);
      buf_free(&buf);
      return r;
    }
    return s;
  }
  while (s.n > 0 && (s.s[s.n - 1u] == ' ' || s.s[s.n - 1u] == '\t')) {
    s.n--;
  }
  return str_make(s.s, s.n);
}

static Str str_join(Arena *arena, Str a, Str b) {
  Str r;
  if (a.n > SIZE_MAX - b.n) {
    rt_oom();
  }
  r.s = arena_join(arena, a.n == 0 ? "" : a.s, a.n, b.n == 0 ? "" : b.s, b.n);
  r.n = a.n + b.n;
  return r;
}

static Str str_join3(Arena *arena, Str a, Str b, Str c) {
  return str_join(arena, str_join(arena, a, b), c);
}

static bool str_eq_c(Str a, const char *b) {
  return rt_eq_cstr(a.s == NULL ? "" : a.s, a.n, b);
}

static bool str_has_nonspace(Str s) {
  size_t i;
  if (s.s == NULL) {
    return false;
  }
  for (i = 0; i < s.n; i++) {
    if (!rt_is_space(s.s[i])) {
      return true;
    }
  }
  return false;
}

static bool str_has_blank(Str s) {
  size_t i;
  if (s.s == NULL) {
    return false;
  }
  for (i = 0; i < s.n; i++) {
    if (s.s[i] == '\n') {
      size_t j = i + 1u;
      while (j < s.n && (s.s[j] == ' ' || s.s[j] == '\t')) {
        j++;
      }
      if (j < s.n && s.s[j] == '\n') {
        return true;
      }
    }
  }
  return false;
}

static bool str_starts(Str a, const char *prefix) {
  return rt_starts(a.s == NULL ? "" : a.s, a.n, prefix);
}

static Str src_slice(const Converter *cv, size_t from, size_t to) {
  if (cv->source == NULL || from >= to || from >= cv->sourcelen) {
    return str_empty();
  }
  if (to > cv->sourcelen) {
    to = cv->sourcelen;
  }
  return str_make(cv->source + from, to - from);
}

static Str str_strip_line_ht(Arena *arena, Str s) {
  Buf buf;
  size_t i = 0;
  Str r;
  if (s.s == NULL || s.n == 0) {
    return str_empty();
  }
  buf_init(&buf);
  while (i < s.n) {
    size_t line = i;
    size_t end;
    while (i < s.n && s.s[i] != '\n') {
      i++;
    }
    end = i;
    while (end > line && (s.s[end - 1u] == ' ' || s.s[end - 1u] == '\t')) {
      end--;
    }
    buf_append(&buf, s.s + line, end - line);
    if (i < s.n && s.s[i] == '\n') {
      buf_putc(&buf, '\n');
      i++;
    }
  }
  r.n = buf.len;
  r.s = buf_arena(&buf, arena);
  buf_free(&buf);
  return r;
}

static void *vec_grow(Arena *arena, void *items, size_t len, size_t *cap, size_t elem, size_t align) {
  size_t ncap;
  void *next;
  if (*cap == 0) {
    ncap = 8u;
  } else {
    if (*cap > SIZE_MAX / 2u) {
      rt_oom();
    }
    ncap = *cap * 2u;
  }
  if (elem != 0 && ncap > SIZE_MAX / elem) {
    rt_oom();
  }
  next = arena_alloc(arena, ncap * elem == 0 ? 1u : ncap * elem, align);
  if (len > 0 && items != NULL) {
    if (elem != 0 && len > SIZE_MAX / elem) {
      rt_oom();
    }
    memcpy(next, items, len * elem);
  }
  *cap = ncap;
  return next;
}

static void nodevec_push(Arena *arena, NodeVec *v, Node *node) {
  if (v->len == v->cap) {
    v->items = vec_grow(arena, v->items, v->len, &v->cap, sizeof(Node *), _Alignof(Node *));
  }
  v->items[v->len++] = node;
}

static void strvec_push(Arena *arena, StrVec *v, Str s) {
  if (v->len == v->cap) {
    v->items = vec_grow(arena, v->items, v->len, &v->cap, sizeof(Str), _Alignof(Str));
  }
  v->items[v->len++] = s;
}

static void mixvec_push(Arena *arena, MixVec *v, Mix item) {
  if (v->len == v->cap) {
    v->items = vec_grow(arena, v->items, v->len, &v->cap, sizeof(Mix), _Alignof(Mix));
  }
  v->items[v->len++] = item;
}

static NodeVec vec_concat(Arena *arena, const NodeVec *a, const NodeVec *b) {
  NodeVec out;
  size_t i;
  memset(&out, 0, sizeof out);
  if (a != NULL) {
    for (i = 0; i < a->len; i++) {
      nodevec_push(arena, &out, a->items[i]);
    }
  }
  if (b != NULL) {
    for (i = 0; i < b->len; i++) {
      nodevec_push(arena, &out, b->items[i]);
    }
  }
  return out;
}

static bool vec_has_raw(const NodeVec *v) {
  size_t i;
  for (i = 0; i < v->len; i++) {
    if (v->items[i] != NULL && v->items[i]->kind == NK_RAW) {
      return true;
    }
  }
  return false;
}

static Node *node_new(Arena *arena, NodeKind kind) {
  Node *n = arena_alloc(arena, sizeof(Node), _Alignof(Node));
  memset(n, 0, sizeof *n);
  n->kind = kind;
  n->text = str_empty();
  n->suffix = str_empty();
  n->tail = str_empty();
  n->follow = str_empty();
  return n;
}

static Node *node_text(Arena *arena, Str text) {
  Node *n = node_new(arena, NK_TEXT);
  n->text = text;
  return n;
}

static Node *node_kind_text(Arena *arena, NodeKind kind, Str text) {
  Node *n = node_new(arena, kind);
  n->text = text;
  return n;
}

static Node *node_clone(Arena *arena, const Node *src) {
  Node *n = arena_alloc(arena, sizeof(Node), _Alignof(Node));
  *n = *src;
  n->kids.cap = n->kids.len;
  n->lead.cap = n->lead.len;
  return n;
}

static bool is_empty_node(const Node *node) {
  return node != NULL && node->kind == NK_TEXT && str_trim(node->text).n == 0;
}

static const Tok *code_at(const Converter *cv, int i) {
  if (i < 0 || i >= cv->ncode) {
    return NULL;
  }
  return &cv->code[i];
}

static const Tok *tok_at(const Converter *cv, int i) {
  if (i < 0 || i >= cv->ntokens) {
    return NULL;
  }
  return &cv->tokens[i];
}

static bool tok_is(const Tok *tok, const char *s) {
  return tok != NULL && tok->text != NULL && rt_eq_cstr(tok->text, tok->len, s);
}

static bool text_in(const char *s, size_t n, const char *const *words, size_t nwords) {
  size_t i;
  for (i = 0; i < nwords; i++) {
    if (rt_eq_cstr(s == NULL ? "" : s, n, words[i])) {
      return true;
    }
  }
  return false;
}

static bool tok_in(const Tok *tok, const char *const *words, size_t nwords) {
  if (tok == NULL || tok->text == NULL) {
    return false;
  }
  return text_in(tok->text, tok->len, words, nwords);
}

static bool is_control_tok(const Tok *tok) {
  return tok_in(tok, CONTROL_WORDS, sizeof CONTROL_WORDS / sizeof CONTROL_WORDS[0]);
}

static bool is_agg_tok(const Tok *tok) {
  return tok_in(tok, AGG_WORDS, sizeof AGG_WORDS / sizeof AGG_WORDS[0]);
}

static bool is_kr_text(const char *s, size_t n) {
  return text_in(s, n, KR_WORDS, sizeof KR_WORDS / sizeof KR_WORDS[0]);
}

static bool is_word_char(unsigned char u) {
  return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_';
}

static bool kw_at(Str body, size_t i, const char *kw) {
  size_t kn = strlen(kw);
  if (body.s == NULL || i > body.n || kn > body.n - i) {
    return false;
  }
  if (!rt_eq(body.s + i, kn, kw, kn)) {
    return false;
  }
  if (i + kn < body.n && is_word_char((unsigned char)body.s[i + kn])) {
    return false;
  }
  return true;
}

static bool dir_hash_kw(Str body, const char *kw) {
  size_t i;
  if (body.s == NULL || body.n == 0 || body.s[0] != '#') {
    return false;
  }
  i = 1u;
  while (i < body.n && rt_is_space(body.s[i])) {
    i++;
  }
  return kw_at(body, i, kw);
}

static bool dir_if0(Str body) {
  size_t i;
  if (!dir_hash_kw(body, "if")) {
    return false;
  }
  i = 1u;
  while (i < body.n && rt_is_space(body.s[i])) {
    i++;
  }
  /* skip the matched "if" */
  i += 2u;
  if (i >= body.n || !rt_is_space(body.s[i])) {
    return false;
  }
  while (i < body.n && rt_is_space(body.s[i])) {
    i++;
  }
  if (i >= body.n || body.s[i] != '0') {
    return false;
  }
  i++;
  if (i < body.n && is_word_char((unsigned char)body.s[i])) {
    return false;
  }
  return true;
}

static bool dir_elif0(Str body) {
  size_t i;
  if (body.s == NULL || body.n == 0 || body.s[0] != '#') {
    return false;
  }
  i = 1u;
  while (i < body.n && rt_is_space(body.s[i])) {
    i++;
  }
  if (!kw_at(body, i, "elif")) {
    return false;
  }
  i += 4u;
  if (i >= body.n || !rt_is_space(body.s[i])) {
    return false;
  }
  while (i < body.n && rt_is_space(body.s[i])) {
    i++;
  }
  if (i >= body.n || body.s[i] != '0') {
    return false;
  }
  i++;
  if (i < body.n && is_word_char((unsigned char)body.s[i])) {
    return false;
  }
  return true;
}

static Str dir_norm(Converter *cv, const Tok *tok) {
  Str trimmed;
  Str low;
  if (tok == NULL || tok->text == NULL) {
    return str_empty();
  }
  trimmed = str_trim(str_make(tok->text, tok->len));
  low.s = arena_lower(cv->arena, trimmed.s, trimmed.n);
  low.n = trimmed.n;
  return low;
}

static DirRole directive_role(Str body) {
  if (dir_hash_kw(body, "if") || dir_hash_kw(body, "ifdef") || dir_hash_kw(body, "ifndef")) {
    return DIR_OPEN;
  }
  if (dir_hash_kw(body, "endif")) {
    return DIR_CLOSE;
  }
  if (dir_hash_kw(body, "else") || dir_hash_kw(body, "elifdef") || dir_hash_kw(body, "elifndef") || dir_hash_kw(body, "elif")) {
    return DIR_MID;
  }
  return DIR_OTHER;
}

typedef struct DeadFrame {
  bool if0;
  bool dead;
} DeadFrame;

typedef struct DeadStack {
  DeadFrame *items;
  size_t len;
  size_t cap;
} DeadStack;

static bool dead_any(const DeadStack *st) {
  size_t i;
  for (i = 0; i < st->len; i++) {
    if (st->items[i].dead) {
      return true;
    }
  }
  return false;
}

static bool dead_outer(const DeadStack *st) {
  size_t i;
  size_t n = st->len == 0 ? 0 : st->len - 1u;
  for (i = 0; i < n; i++) {
    if (st->items[i].dead) {
      return true;
    }
  }
  return false;
}

static void dead_push(Arena *arena, DeadStack *st, DeadFrame frame) {
  if (st->len == st->cap) {
    st->items = vec_grow(arena, st->items, st->len, &st->cap, sizeof(DeadFrame), _Alignof(DeadFrame));
  }
  st->items[st->len++] = frame;
}

static int branch_net(Converter *cv, int from) {
  int net = 0;
  DeadStack st;
  int t;
  memset(&st, 0, sizeof st);
  for (t = from; t < cv->ntokens; t++) {
    const Tok *tok = tok_at(cv, t);
    DirRole role;
    Str body;
    if (tok == NULL) {
      break;
    }
    if (tok->kind == TOK_DIRECTIVE) {
      body = dir_norm(cv, tok);
      role = directive_role(body);
      if (role == DIR_OPEN) {
        DeadFrame frame;
        bool zero = dir_if0(body);
        frame.if0 = zero;
        frame.dead = dead_any(&st) || zero;
        dead_push(cv->arena, &st, frame);
      } else if (role == DIR_CLOSE) {
        if (st.len == 0) {
          break;
        }
        st.len--;
      } else if (role == DIR_MID) {
        if (st.len == 0) {
          break;
        }
        if (st.items[st.len - 1u].if0 && !dead_outer(&st)) {
          st.items[st.len - 1u].dead = dir_elif0(body);
        }
      }
      continue;
    }
    if (dead_any(&st) || tok->kind == TOK_COMMENT) {
      continue;
    }
    if (tok_is(tok, "{")) {
      net++;
    } else if (tok_is(tok, "}")) {
      net--;
    }
  }
  return net;
}

typedef struct PpFrame {
  int base;
  size_t start;
  bool if0;
  bool dead;
} PpFrame;

typedef struct PpStack {
  PpFrame *items;
  size_t len;
  size_t cap;
} PpStack;

typedef struct Pending {
  size_t start;
  int base;
} Pending;

typedef struct PendStack {
  Pending *items;
  size_t len;
  size_t cap;
} PendStack;

typedef struct SpanVec {
  Span *items;
  size_t len;
  size_t cap;
} SpanVec;

static bool pp_dead(const PpStack *st) {
  size_t i;
  for (i = 0; i < st->len; i++) {
    if (st->items[i].dead) {
      return true;
    }
  }
  return false;
}

static bool pp_outer(const PpStack *st) {
  size_t i;
  size_t n = st->len == 0 ? 0 : st->len - 1u;
  for (i = 0; i < n; i++) {
    if (st->items[i].dead) {
      return true;
    }
  }
  return false;
}

static void pp_push(Arena *arena, PpStack *st, PpFrame frame) {
  if (st->len == st->cap) {
    st->items = vec_grow(arena, st->items, st->len, &st->cap, sizeof(PpFrame), _Alignof(PpFrame));
  }
  st->items[st->len++] = frame;
}

static void pend_push(Arena *arena, PendStack *st, Pending item) {
  if (st->len == st->cap) {
    st->items = vec_grow(arena, st->items, st->len, &st->cap, sizeof(Pending), _Alignof(Pending));
  }
  st->items[st->len++] = item;
}

static void span_push(Arena *arena, SpanVec *st, Span span) {
  if (st->len == st->cap) {
    st->items = vec_grow(arena, st->items, st->len, &st->cap, sizeof(Span), _Alignof(Span));
  }
  st->items[st->len++] = span;
}

static int span_cmp(const void *a, const void *b) {
  const Span *sa = a;
  const Span *sb = b;
  if (sa->start < sb->start) {
    return -1;
  }
  if (sa->start > sb->start) {
    return 1;
  }
  if (sa->end > sb->end) {
    return -1;
  }
  if (sa->end < sb->end) {
    return 1;
  }
  return 0;
}

static const Span *brace_spans(Converter *cv) {
  PpStack stack;
  PendStack pending;
  SpanVec spans;
  SpanVec kept;
  int t;
  int depth = 0;
  size_t i;
  if (cv->spans_ready) {
    return cv->spans;
  }
  memset(&stack, 0, sizeof stack);
  memset(&pending, 0, sizeof pending);
  memset(&spans, 0, sizeof spans);
  memset(&kept, 0, sizeof kept);
  for (t = 0; t < cv->ntokens; t++) {
    const Tok *tok = tok_at(cv, t);
    if (tok == NULL) {
      break;
    }
    if (tok->kind == TOK_DIRECTIVE) {
      Str body = dir_norm(cv, tok);
      DirRole role = directive_role(body);
      if (role == DIR_OPEN) {
        PpFrame frame;
        bool zero = dir_if0(body);
        frame.base = depth;
        frame.start = tok->start;
        frame.if0 = zero;
        frame.dead = pp_dead(&stack) || zero;
        pp_push(cv->arena, &stack, frame);
      } else if (role == DIR_MID && stack.len > 0) {
        PpFrame *frame = &stack.items[stack.len - 1u];
        bool outer = pp_outer(&stack);
        if (frame->if0 && !outer) {
          if (frame->dead) {
            Span sp;
            sp.start = frame->start;
            sp.end = tok->start;
            span_push(cv->arena, &spans, sp);
          }
          frame->dead = dir_elif0(body);
          frame->start = tok->start;
        } else if (!pp_dead(&stack) && branch_net(cv, t + 1) > 0 && depth != frame->base) {
          depth = frame->base;
        }
      } else if (role == DIR_CLOSE && stack.len > 0) {
        PpFrame frame = stack.items[stack.len - 1u];
        bool outer;
        stack.len--;
        outer = pp_dead(&stack);
        if (frame.if0 && frame.dead && !outer) {
          Span sp;
          sp.start = frame.start;
          sp.end = tok->end;
          span_push(cv->arena, &spans, sp);
        } else if (!frame.dead && !outer && depth > frame.base && pending.len == 0) {
          Pending item;
          item.start = frame.start;
          item.base = frame.base;
          pend_push(cv->arena, &pending, item);
        }
      }
      continue;
    }
    if (pp_dead(&stack) || tok->kind == TOK_COMMENT) {
      continue;
    }
    if (tok_is(tok, "{")) {
      depth++;
    } else if (tok_is(tok, "}")) {
      depth--;
      while (pending.len > 0 && depth <= pending.items[pending.len - 1u].base) {
        Pending done = pending.items[--pending.len];
        Span sp;
        sp.start = done.start;
        sp.end = tok->end;
        span_push(cv->arena, &spans, sp);
      }
    }
  }
  for (i = 0; i < pending.len; i++) {
    Span sp;
    sp.start = pending.items[i].start;
    sp.end = cv->sourcelen;
    span_push(cv->arena, &spans, sp);
  }
  if (spans.len > 1u) {
    qsort(spans.items, spans.len, sizeof(Span), span_cmp);
  }
  for (i = 0; i < spans.len; i++) {
    size_t k;
    bool covered = false;
    Span span = spans.items[i];
    for (k = 0; k < kept.len; k++) {
      if (kept.items[k].start <= span.start && kept.items[k].end >= span.end) {
        covered = true;
        break;
      }
    }
    if (!covered) {
      span_push(cv->arena, &kept, span);
    }
  }
  cv->spans = kept.items;
  cv->nspans = kept.len;
  cv->spans_ready = true;
  return cv->spans;
}

static const Span *span_covering(Converter *cv, size_t start) {
  size_t i;
  const Span *spans = brace_spans(cv);
  for (i = 0; i < cv->nspans; i++) {
    if (start >= spans[i].start && start < spans[i].end) {
      return &spans[i];
    }
  }
  return NULL;
}

static bool has_directive_between(const Converter *cv, size_t from, size_t to) {
  int i;
  for (i = 0; i < cv->ntokens; i++) {
    const Tok *tok = &cv->tokens[i];
    if (tok->start < from) {
      continue;
    }
    if (tok->start >= to) {
      break;
    }
    if (tok->kind == TOK_DIRECTIVE) {
      return true;
    }
  }
  return false;
}

static int index_of_code(const Converter *cv, size_t start) {
  int lo = 0;
  int hi = cv->ncode - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    size_t at = cv->code[mid].start;
    if (at == start) {
      return mid;
    }
    if (at < start) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return -1;
}

typedef struct BraceFrame {
  int depth;
  bool if0;
  bool dead;
} BraceFrame;

typedef struct BraceStack {
  BraceFrame *items;
  size_t len;
  size_t cap;
} BraceStack;

static bool brace_dead(const BraceStack *st) {
  size_t i;
  for (i = 0; i < st->len; i++) {
    if (st->items[i].dead) {
      return true;
    }
  }
  return false;
}

static bool brace_outer(const BraceStack *st) {
  size_t i;
  size_t n = st->len == 0 ? 0 : st->len - 1u;
  for (i = 0; i < n; i++) {
    if (st->items[i].dead) {
      return true;
    }
  }
  return false;
}

static void brace_push(Arena *arena, BraceStack *st, BraceFrame frame) {
  if (st->len == st->cap) {
    st->items = vec_grow(arena, st->items, st->len, &st->cap, sizeof(BraceFrame), _Alignof(BraceFrame));
  }
  st->items[st->len++] = frame;
}

static int match_brace(Converter *cv, int open) {
  const Tok *origin_tok = code_at(cv, open);
  size_t origin;
  int depth = 0;
  BraceStack stack;
  int t;
  if (origin_tok == NULL) {
    return -1;
  }
  origin = origin_tok->start;
  memset(&stack, 0, sizeof stack);
  for (t = 0; t < cv->ntokens; t++) {
    const Tok *tok = tok_at(cv, t);
    if (tok == NULL || tok->start < origin) {
      continue;
    }
    if (tok->kind == TOK_DIRECTIVE) {
      Str body = dir_norm(cv, tok);
      DirRole role = directive_role(body);
      if (role == DIR_OPEN) {
        BraceFrame frame;
        bool zero = dir_if0(body);
        frame.depth = depth;
        frame.if0 = zero;
        frame.dead = brace_dead(&stack) || zero;
        brace_push(cv->arena, &stack, frame);
      } else if (role == DIR_MID && stack.len > 0) {
        BraceFrame *frame = &stack.items[stack.len - 1u];
        bool outer = brace_outer(&stack);
        if (frame->if0 && !outer) {
          frame->dead = dir_elif0(body);
        } else if (!brace_dead(&stack) && branch_net(cv, t + 1) > 0) {
          depth = frame->depth;
        }
      } else if (role == DIR_CLOSE) {
        if (stack.len > 0) {
          stack.len--;
        }
      }
      continue;
    }
    if (brace_dead(&stack) || tok->kind == TOK_COMMENT) {
      continue;
    }
    if (tok_is(tok, "{")) {
      depth++;
    } else if (tok_is(tok, "}")) {
      depth--;
      if (depth == 0) {
        return index_of_code(cv, tok->start);
      }
    }
  }
  return -1;
}

static int match_tok(Converter *cv, int open, const char *left, const char *right) {
  int depth = 0;
  int k;
  if (left != NULL && left[0] == '{' && left[1] == '\0') {
    return match_brace(cv, open);
  }
  for (k = open; k < cv->ncode; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      break;
    }
    if (tok_is(tok, left)) {
      depth++;
    } else if (tok_is(tok, right)) {
      depth--;
      if (depth == 0) {
        return k;
      }
    }
  }
  return -1;
}

static int match_back(const Converter *cv, int close, const char *right, const char *left) {
  int depth = 0;
  int k;
  for (k = close; k >= 0; k--) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      continue;
    }
    if (tok_is(tok, right)) {
      depth++;
    } else if (tok_is(tok, left)) {
      depth--;
      if (depth == 0) {
        return k;
      }
    }
  }
  return -1;
}

static int scan_semi(const Converter *cv, int i) {
  int paren = 0;
  int bracket = 0;
  int brace = 0;
  int k;
  for (k = i; k < cv->ncode; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      break;
    }
    if (tok_is(tok, "(")) {
      paren++;
    } else if (tok_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_is(tok, "[")) {
      bracket++;
    } else if (tok_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (tok_is(tok, "{")) {
      brace++;
    } else if (tok_is(tok, "}") && brace > 0) {
      brace--;
    } else if (tok_is(tok, "}") && brace == 0 && paren == 0) {
      return -1;
    } else if (tok_is(tok, ";") && paren == 0 && bracket == 0 && brace == 0) {
      return k;
    }
  }
  return -1;
}

static const Tok *comment_on_line_after(const Converter *cv, const Tok *tok) {
  int lo = 0;
  int hi;
  const Tok *cand;
  if (tok == NULL) {
    return NULL;
  }
  hi = cv->ntokens;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (cv->tokens[mid].start < tok->end) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo >= cv->ntokens) {
    return NULL;
  }
  cand = &cv->tokens[lo];
  if (cand->line != tok->line || cand->kind != TOK_COMMENT) {
    return NULL;
  }
  return cand;
}

static size_t offset_after(Converter *cv, int next) {
  const Tok *prev;
  const Tok *trail;
  if (next <= 0) {
    return cv->mark;
  }
  prev = code_at(cv, next - 1);
  if (prev == NULL) {
    return cv->mark;
  }
  trail = comment_on_line_after(cv, prev);
  return trail != NULL ? trail->end : prev->end;
}

static Str strip_semis(Converter *cv, int from, int to) {
  Buf buf;
  size_t pos;
  int paren = 0;
  int bracket = 0;
  int brace = 0;
  int k;
  const Tok *next;
  const Tok *anchor;
  const Tok *trail;
  Str out;
  if (from > to || from < 0 || to >= cv->ncode) {
    return str_empty();
  }
  buf_init(&buf);
  pos = cv->code[from].start;
  for (k = from; k <= to; k++) {
    const Tok *tok = code_at(cv, k);
    bool skip;
    if (tok == NULL) {
      continue;
    }
    buf_append(&buf, src_slice(cv, pos, tok->start).s, src_slice(cv, pos, tok->start).n);
    skip = tok_is(tok, ";") && paren == 0 && bracket == 0 && brace == 0;
    if (!skip && tok->text != NULL) {
      buf_append(&buf, tok->text, tok->len);
    }
    if (tok_is(tok, "(")) {
      paren++;
    } else if (tok_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_is(tok, "[")) {
      bracket++;
    } else if (tok_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (tok_is(tok, "{")) {
      brace++;
    } else if (tok_is(tok, "}") && brace > 0) {
      brace--;
    }
    pos = tok->end;
  }
  next = code_at(cv, to + 1);
  anchor = code_at(cv, to);
  if (tok_is(next, ";") && next->start >= pos) {
    Str gap = src_slice(cv, pos, next->start);
    if (str_has_nonspace(gap)) {
      gap = str_rtrim_ht(cv->arena, gap);
      buf_append(&buf, gap.s, gap.n);
    }
    anchor = next;
  }
  trail = anchor == NULL ? NULL : comment_on_line_after(cv, anchor);
  if (trail != NULL && anchor != NULL && trail->start >= pos && trail->line == anchor->line) {
    Str base;
    base.n = buf.len;
    base.s = buf_arena(&buf, cv->arena);
    buf_free(&buf);
    base = str_rtrim_ht(cv->arena, base);
    buf_init(&buf);
    buf_append(&buf, base.s, base.n);
    buf_putc(&buf, ' ');
    if (trail->text != NULL) {
      buf_append(&buf, trail->text, trail->len);
    }
  }
  out.n = buf.len;
  out.s = buf_arena(&buf, cv->arena);
  buf_free(&buf);
  return out;
}

static Str header_text(Converter *cv, int from, int brace) {
  if (brace <= 0) {
    return str_empty();
  }
  return strip_semis(cv, from, brace - 1);
}

static Str flat_join(Converter *cv, int from, int to) {
  Buf buf;
  int k;
  Str out;
  bool any = false;
  if (from > to || from < 0) {
    return str_empty();
  }
  buf_init(&buf);
  for (k = from; k <= to && k < cv->ncode; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL || tok->text == NULL) {
      continue;
    }
    if (any) {
      buf_putc(&buf, ' ');
    }
    buf_append(&buf, tok->text, tok->len);
    any = true;
  }
  out.n = buf.len;
  out.s = buf_arena(&buf, cv->arena);
  buf_free(&buf);
  return out;
}

static NodeVec pull_trivia(Converter *cv, size_t until) {
  NodeVec nodes;
  size_t cursor;
  memset(&nodes, 0, sizeof nodes);
  if (until < cv->mark) {
    return nodes;
  }
  while (cv->trivia_at < cv->ntokens && cv->tokens[cv->trivia_at].start < cv->mark) {
    cv->trivia_at++;
  }
  cursor = cv->mark;
  while (cv->trivia_at < cv->ntokens && cv->tokens[cv->trivia_at].start < until) {
    const Tok *tok = &cv->tokens[cv->trivia_at];
    cv->trivia_at++;
    if (tok->kind != TOK_COMMENT && tok->kind != TOK_DIRECTIVE) {
      continue;
    }
    if (str_has_blank(src_slice(cv, cursor, tok->start))) {
      nodevec_push(cv->arena, &nodes, node_new(cv->arena, NK_BLANK));
    }
    if (tok->kind == TOK_DIRECTIVE) {
      Str raw = str_rtrim_ht(cv->arena, str_make(tok->text, tok->len));
      nodevec_push(cv->arena, &nodes, node_kind_text(cv->arena, NK_RAW, raw));
    } else {
      nodevec_push(cv->arena, &nodes, node_kind_text(cv->arena, NK_COMMENT, str_make(tok->text, tok->len)));
    }
    cursor = tok->end;
  }
  if (str_has_blank(src_slice(cv, cursor, until))) {
    nodevec_push(cv->arena, &nodes, node_new(cv->arena, NK_BLANK));
  }
  cv->mark = until;
  return nodes;
}

static NodeVec lead_in(Converter *cv, int brace) {
  const Tok *prev = code_at(cv, brace - 1);
  const Tok *open = code_at(cv, brace);
  const Tok *glued;
  size_t from;
  NodeVec nodes;
  if (prev == NULL || open == NULL) {
    memset(&nodes, 0, sizeof nodes);
    return nodes;
  }
  from = prev->end;
  glued = comment_on_line_after(cv, prev);
  if (glued != NULL && glued->start < open->start) {
    from = glued->end;
  }
  if (from > cv->mark) {
    cv->mark = from;
  }
  nodes = pull_trivia(cv, open->start);
  if (open->end > cv->mark) {
    cv->mark = open->end;
  }
  return nodes;
}

static bool looks_like_aggregate(const Converter *cv, int brace) {
  int paren = 0;
  int bracket = 0;
  int k;
  for (k = brace - 1; k >= 0; k--) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      continue;
    }
    if (tok_is(tok, ")")) {
      paren++;
    } else if (tok_is(tok, "(")) {
      if (paren == 0) {
        return false;
      }
      paren--;
    } else if (tok_is(tok, "]")) {
      bracket++;
    } else if (tok_is(tok, "[")) {
      if (bracket == 0) {
        return false;
      }
      bracket--;
    } else if (paren == 0 && bracket == 0) {
      if (tok_is(tok, "=") || tok_is(tok, ";") || tok_is(tok, "{") || tok_is(tok, "}")) {
        return false;
      }
      if (is_agg_tok(tok)) {
        return true;
      }
    }
  }
  return false;
}

static BraceRole brace_role(Converter *cv, int index) {
  const Tok *prev = code_at(cv, index - 1);
  if (prev == NULL) {
    return ROLE_KEEP;
  }
  if (tok_is(prev, "=") || tok_is(prev, ",") || tok_is(prev, "{")) {
    return ROLE_INIT;
  }
  if (tok_is(prev, "=>")) {
    return ROLE_COLON;
  }
  if (tok_is(prev, "(")) {
    return ROLE_EXPR;
  }
  if (tok_is(prev, "]") && tok_is(code_at(cv, index - 2), "]")) {
    return ROLE_COLON;
  }
  if (tok_is(prev, "else") || tok_is(prev, "do") || tok_is(prev, "try") || tok_is(prev, "finally")) {
    return ROLE_COLON;
  }
  if (is_agg_tok(prev)) {
    return ROLE_COLON;
  }
  if (tok_is(prev, ")")) {
    int open = match_back(cv, index - 1, ")", "(");
    const Tok *before = open > 0 ? code_at(cv, open - 1) : NULL;
    if (before != NULL && (tok_is(before, "__attribute__") || tok_is(before, "__attribute") || tok_is(before, "__declspec") || tok_is(before, "alignas") || tok_is(before, "_Alignas"))) {
      return ROLE_COLON;
    }
    if (before == NULL || tok_is(before, "=") || tok_is(before, ",") || tok_is(before, "return") || tok_is(before, "(")) {
      return ROLE_INIT;
    }
    if (tok_is(before, "]") || is_agg_tok(before)) {
      return ROLE_INIT;
    }
    if (before->kind == TOK_KEYWORD && !is_control_tok(before) && !tok_is(before, "else") && !tok_is(before, "do")) {
      return ROLE_INIT;
    }
    return ROLE_COLON;
  }
  if (looks_like_aggregate(cv, index)) {
    return ROLE_COLON;
  }
  return ROLE_KEEP;
}

static bool has_aggregate(const Converter *cv, int from, int brace) {
  int paren = 0;
  int bracket = 0;
  bool seen = false;
  int k;
  for (k = from; k < brace; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      continue;
    }
    if (tok_is(tok, "(")) {
      if (paren == 0 && seen) {
        return false;
      }
      paren++;
    } else if (tok_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_is(tok, "[")) {
      bracket++;
    } else if (tok_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && is_agg_tok(tok)) {
      seen = true;
    }
  }
  return seen;
}

static bool has_keyword(const Converter *cv, int from, int to, const char *word) {
  int paren = 0;
  int bracket = 0;
  int k;
  for (k = from; k < to; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      continue;
    }
    if (tok_is(tok, "(")) {
      paren++;
    } else if (tok_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_is(tok, "[")) {
      bracket++;
    } else if (tok_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (paren == 0 && bracket == 0 && tok_is(tok, word)) {
      return true;
    }
  }
  return false;
}

static bool tail_is_simple(Str tail) {
  size_t i;
  if (tail.s == NULL) {
    return true;
  }
  for (i = 0; i < tail.n; i++) {
    char c = tail.s[i];
    if (c == '=' || c == '?' || c == ':' || c == '.' || c == '(' || c == ')' || c == '{' || c == '}') {
      return false;
    }
    if (c == '-' && i + 1u < tail.n && tail.s[i + 1u] == '>') {
      return false;
    }
  }
  return true;
}

static bool is_attribute_tail(Str tail) {
  Str text = str_trim(tail);
  return str_starts(text, "__attribute__") || str_starts(text, "__attribute") || str_starts(text, "__declspec")
    || str_starts(text, "alignas") || str_starts(text, "_Alignas") || str_starts(text, "[[");
}

static bool kr_body(const Converter *cv, int fn_close, int brace) {
  int k;
  for (k = fn_close + 1; k < brace; k++) {
    if (tok_is(code_at(cv, k), "=")) {
      return false;
    }
  }
  return true;
}

static bool kr_decl(const Converter *cv, int from, int to) {
  int k;
  for (k = from + 1; k < to; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      return false;
    }
    if (tok_is(tok, "(")) {
      return false;
    }
    if (tok->kind == TOK_IDENT || tok_is(tok, "*") || tok_is(tok, ",") || tok_is(tok, ";") || tok_is(tok, "[") || tok_is(tok, "]")) {
      continue;
    }
    if (tok->text != NULL && is_kr_text(tok->text, tok->len)) {
      continue;
    }
    return false;
  }
  return true;
}

static bool kr_continues(const Converter *cv, int semi) {
  int bracket = 0;
  int k;
  for (k = semi + 1; k < cv->ncode; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      break;
    }
    if (tok_is(tok, "(") || tok_is(tok, ")") || tok_is(tok, "=") || tok_is(tok, "}")) {
      return false;
    }
    if (tok_is(tok, "[")) {
      bracket++;
    } else if (tok_is(tok, "]")) {
      if (bracket == 0) {
        return false;
      }
      bracket--;
    } else if (tok_is(tok, "{")) {
      return bracket == 0;
    } else if (tok_is(tok, ";") || tok_is(tok, "*") || tok_is(tok, ",") || tok->kind == TOK_IDENT
        || (tok->text != NULL && is_kr_text(tok->text, tok->len))) {
      continue;
    } else {
      return false;
    }
  }
  return false;
}

static bool has_header_word(Str line) {
  size_t i;
  size_t w;
  if (line.s == NULL) {
    return false;
  }
  for (i = 0; i < line.n; i++) {
    if (i > 0 && is_word_char((unsigned char)line.s[i - 1u])) {
      continue;
    }
    if (!is_word_char((unsigned char)line.s[i])) {
      continue;
    }
    for (w = 0; w < sizeof HEADER_WORDS / sizeof HEADER_WORDS[0]; w++) {
      size_t wn = strlen(HEADER_WORDS[w]);
      if (i + wn <= line.n && rt_eq(line.s + i, wn, HEADER_WORDS[w], wn)
          && (i + wn == line.n || !is_word_char((unsigned char)line.s[i + wn]))) {
        return true;
      }
    }
  }
  return false;
}

static Str last_line(Str s) {
  size_t i;
  size_t start = 0;
  if (s.s == NULL || s.n == 0) {
    return str_empty();
  }
  for (i = 0; i < s.n; i++) {
    if (s.s[i] == '\n') {
      start = i + 1u;
    }
  }
  if (start >= s.n) {
    return str_empty();
  }
  return str_make(s.s + start, s.n - start);
}

static ColonWhere colon_where(const Converter *cv, int brace, Str header) {
  const Tok *prev = code_at(cv, brace - 1);
  if (prev != NULL && (tok_is(prev, ")") || tok_is(prev, "=>"))) {
    return WHERE_INLINE;
  }
  return has_header_word(last_line(header)) ? WHERE_INLINE : WHERE_OWN;
}

static bool is_label(const Converter *cv, int i) {
  const Tok *tok = code_at(cv, i);
  const Tok *colon = code_at(cv, i + 1);
  return tok != NULL && tok->kind == TOK_IDENT && tok_is(colon, ":");
}

static int find_case_colon(const Converter *cv, int i) {
  int paren = 0;
  int bracket = 0;
  int ternary = 0;
  int k;
  for (k = i; k < cv->ncode; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      break;
    }
    if (tok_is(tok, "(")) {
      paren++;
    } else if (tok_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_is(tok, "[")) {
      bracket++;
    } else if (tok_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (tok_is(tok, "?")) {
      ternary++;
    } else if (tok_is(tok, ":")) {
      if (ternary > 0) {
        ternary--;
      } else if (paren == 0 && bracket == 0) {
        return k;
      }
    } else if (tok_is(tok, ";") && paren == 0 && bracket == 0) {
      return -1;
    }
  }
  return -1;
}

static int find_paren(const Converter *cv, int i) {
  int k;
  for (k = i; k < cv->ncode; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      break;
    }
    if (tok_is(tok, "(")) {
      return k;
    }
    if (tok_is(tok, ";") || tok_is(tok, "{")) {
      return -1;
    }
  }
  return -1;
}

static bool only_trivia(int from, int semi) {
  return from >= semi;
}

static Str inline_after(Converter *cv, int i) {
  int semi = scan_semi(cv, i);
  if (semi < 0 || semi == i) {
    return str_empty();
  }
  return strip_semis(cv, i, semi - 1);
}

static void flatten_into(Arena *arena, Node *node, NodeVec *out) {
  size_t i;
  if (node == NULL) {
    return;
  }
  if (node->kind == NK_GROUP) {
    for (i = 0; i < node->kids.len; i++) {
      flatten_into(arena, node->kids.items[i], out);
    }
    return;
  }
  nodevec_push(arena, out, node);
}

static Node *prefix_header(Converter *cv, Node *node, Str prefix) {
  size_t i;
  if (node == NULL) {
    return NULL;
  }
  if (node->kind == NK_COLON) {
    Node *copy = node_clone(cv->arena, node);
    copy->text = str_join(cv->arena, prefix, node->text);
    return copy;
  }
  if (node->kind == NK_GROUP) {
    Node *g;
    NodeVec kids;
    if (node->kids.len == 0) {
      return node;
    }
    memset(&kids, 0, sizeof kids);
    nodevec_push(cv->arena, &kids, prefix_header(cv, node->kids.items[0], prefix));
    for (i = 1; i < node->kids.len; i++) {
      nodevec_push(cv->arena, &kids, node->kids.items[i]);
    }
    g = node_new(cv->arena, NK_GROUP);
    g->kids = kids;
    return g;
  }
  return node;
}

static Node *headed(Converter *cv, Str header, ColonWhere where, const BodyOut *body, Str follow) {
  Node *node = node_new(cv->arena, NK_COLON);
  bool split = vec_has_raw(&body->lead);
  node->text = header;
  node->follow = follow;
  if (split) {
    node->where = WHERE_OWN;
    node->has_lead = true;
    node->lead = body->lead;
    node->kids = body->nodes;
  } else {
    node->where = where;
    node->has_lead = false;
    node->kids = vec_concat(cv->arena, &body->lead, &body->nodes);
  }
  return node;
}

static NodeVec enum_items(Converter *cv, int brace, int close) {
  NodeVec items;
  int start = brace + 1;
  int paren = 0;
  int bracket = 0;
  int nest = 0;
  int k;
  memset(&items, 0, sizeof items);
  for (k = brace + 1; k < close; k++) {
    const Tok *tok = code_at(cv, k);
    if (tok == NULL) {
      break;
    }
    if (tok_is(tok, "(")) {
      paren++;
    } else if (tok_is(tok, ")") && paren > 0) {
      paren--;
    } else if (tok_is(tok, "[")) {
      bracket++;
    } else if (tok_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (tok_is(tok, "{")) {
      nest++;
    } else if (tok_is(tok, "}") && nest > 0) {
      nest--;
    } else if (tok_is(tok, ",") && paren == 0 && bracket == 0 && nest == 0) {
      if (start <= k - 1) {
        Str text = str_trim(flat_join(cv, start, k - 1));
        if (text.n > 0) {
          nodevec_push(cv->arena, &items, node_text(cv->arena, text));
        }
      }
      start = k + 1;
    }
  }
  if (start <= close - 1) {
    Str text = str_trim(flat_join(cv, start, close - 1));
    if (text.n > 0) {
      nodevec_push(cv->arena, &items, node_text(cv->arena, text));
    }
  }
  return items;
}

static Node *reshape_aggregate(Converter *cv, int start, int brace, Node *node, Str tail) {
  int semi;
  int end;
  const Tok *start_tok;
  const Tok *end_tok;
  if (node == NULL || node->kind != NK_COLON) {
    return node;
  }
  if (is_attribute_tail(tail) || tail_is_simple(tail)) {
    Node *copy = node_clone(cv->arena, node);
    copy->tail = str_trim(tail);
    return copy;
  }
  start_tok = code_at(cv, start);
  note_push(&cv->notes, start_tok == NULL ? 1 : start_tok->line,
      "Left this aggregate braced. A declarator with an initializer cannot follow a colon.");
  semi = scan_semi(cv, brace);
  end = semi < 0 ? match_tok(cv, brace, "{", "}") : semi;
  end_tok = code_at(cv, end);
  if (start_tok == NULL || end_tok == NULL) {
    return node_text(cv->arena, str_empty());
  }
  return node_text(cv->arena, src_slice(cv, start_tok->start, end_tok->end));
}

static NodeVec mix_nodes(Converter *cv, const MixVec *items) {
  NodeVec nodes;
  Str prefix = str_empty();
  size_t n;
  memset(&nodes, 0, sizeof nodes);
  if (items->len == 0) {
    return nodes;
  }
  if (items->len == 1 && !items->items[0].sexpr) {
    nodevec_push(cv->arena, &nodes, node_text(cv->arena, items->items[0].text));
    return nodes;
  }
  for (n = 0; n < items->len; n++) {
    const Mix *item = &items->items[n];
    if (!item->sexpr) {
      bool next_sexpr = n + 1u < items->len && items->items[n + 1u].sexpr;
      if (next_sexpr) {
        prefix = item->text;
      } else if (prefix.n > 0 && nodes.len > 0 && nodes.items[nodes.len - 1u] != NULL
          && nodes.items[nodes.len - 1u]->kind == NK_SEXPR) {
        nodes.items[nodes.len - 1u]->suffix = str_trim(item->text);
        prefix = str_empty();
      } else {
        nodevec_push(cv->arena, &nodes, node_text(cv->arena, item->text));
      }
    } else {
      Node *sx = node_new(cv->arena, NK_SEXPR);
      sx->text = str_trim(prefix);
      sx->kids = item->body;
      sx->suffix = str_empty();
      nodevec_push(cv->arena, &nodes, sx);
      prefix = str_empty();
    }
  }
  return nodes;
}

static Node *unwrap_nodes(Converter *cv, NodeVec nodes) {
  Node *g;
  if (nodes.len == 1 && nodes.items[0] != NULL) {
    return nodes.items[0];
  }
  g = node_new(cv->arena, NK_GROUP);
  g->kids = nodes;
  return g;
}

static void push_text_item(Converter *cv, MixVec *items, int chunk, int to) {
  Str text;
  Mix item;
  if (to < chunk) {
    return;
  }
  text = strip_semis(cv, chunk, to);
  if (str_trim(text).n == 0) {
    return;
  }
  memset(&item, 0, sizeof item);
  item.sexpr = false;
  item.text = text;
  mixvec_push(cv->arena, items, item);
}

static SeqOut parse_seq(Converter *cv, int i, bool stop_brace) {
  SeqOut seq;
  memset(&seq, 0, sizeof seq);
  while (i < cv->ncode) {
    const Tok *tok = code_at(cv, i);
    const Span *span;
    Parsed parsed;
    if (tok == NULL) {
      break;
    }
    span = span_covering(cv, tok->start);
    if (span != NULL && !(stop_brace && tok_is(tok, "}") && tok->start >= span->end)) {
      size_t from;
      Str chunk;
      if (cv->mark < span->start) {
        NodeVec trivia = pull_trivia(cv, span->start);
        seq.nodes = vec_concat(cv->arena, &seq.nodes, &trivia);
      }
      from = span->start > cv->mark ? span->start : cv->mark;
      chunk = str_strip_line_ht(cv->arena, src_slice(cv, from, span->end));
      if (str_trim(chunk).n > 0) {
        nodevec_push(cv->arena, &seq.nodes, node_text(cv->arena, chunk));
      }
      if (span->end > cv->mark) {
        cv->mark = span->end;
      }
      while (i < cv->ncode) {
        const Tok *at = code_at(cv, i);
        if (at == NULL || at->start >= span->end) {
          break;
        }
        i++;
      }
      continue;
    }
    if (stop_brace && tok_is(tok, "}")) {
      NodeVec trivia = pull_trivia(cv, tok->start);
      seq.nodes = vec_concat(cv->arena, &seq.nodes, &trivia);
      break;
    }
    {
      NodeVec trivia = pull_trivia(cv, tok->start);
      seq.nodes = vec_concat(cv->arena, &seq.nodes, &trivia);
    }
    parsed = parse_stmt(cv, i);
    if (parsed.next <= i) {
      note_push(&cv->notes, tok->line, "Skipped a statement the reverse pass could not read.");
      if (tok->end > cv->mark) {
        cv->mark = tok->end;
      }
      i++;
      continue;
    }
    if (!is_empty_node(parsed.node)) {
      nodevec_push(cv->arena, &seq.nodes, parsed.node);
    }
    if (parsed.end > cv->mark) {
      cv->mark = parsed.end;
    }
    i = parsed.next;
  }
  seq.next = i;
  return seq;
}

static Parsed parse_label(Converter *cv, int i) {
  const Tok *tok = code_at(cv, i);
  const Tok *colon = code_at(cv, i + 1);
  const Tok *trail;
  const Tok *third;
  Str text;
  Str shown;
  size_t end;
  bool use_trail = false;
  Parsed p;
  text = str_join(cv->arena, str_make(tok->text, tok->len), str_lit(":"));
  trail = comment_on_line_after(cv, colon != NULL ? colon : tok);
  third = code_at(cv, i + 2);
  if (trail != NULL && (third == NULL || trail->start < third->start)) {
    use_trail = true;
  }
  if (use_trail) {
    shown = str_join3(cv->arena, text, str_lit(" "), str_make(trail->text, trail->len));
  } else {
    shown = text;
  }
  end = trail != NULL ? trail->end : (colon != NULL ? colon->end : cv->mark);
  p.node = node_text(cv->arena, shown);
  p.next = i + 2;
  p.end = end;
  return p;
}

static Parsed parse_case(Converter *cv, int i) {
  int colon = find_case_colon(cv, i);
  const Tok *colon_tok;
  const Tok *glued;
  const Tok *after_colon;
  NodeVec nodes;
  int after;
  Parsed p;
  Str header;
  memset(&nodes, 0, sizeof nodes);
  if (colon < 0) {
    const Tok *tok = code_at(cv, i);
    p.node = node_text(cv->arena, tok == NULL ? str_empty() : str_make(tok->text, tok->len));
    p.next = i + 1;
    p.end = offset_after(cv, i + 1);
    return p;
  }
  header = header_text(cv, i, colon + 1);
  colon_tok = code_at(cv, colon);
  glued = colon_tok == NULL ? NULL : comment_on_line_after(cv, colon_tok);
  after_colon = code_at(cv, colon + 1);
  if (glued != NULL && (after_colon == NULL || glued->start < after_colon->start)) {
    if (glued->end > cv->mark) {
      cv->mark = glued->end;
    }
  } else if (colon_tok != NULL && colon_tok->end > cv->mark) {
    cv->mark = colon_tok->end;
  }
  after = colon + 1;
  if (tok_is(code_at(cv, after), "{")) {
    NodeVec lead = lead_in(cv, after);
    SeqOut inner = parse_seq(cv, after + 1, true);
    const Tok *close = code_at(cv, inner.next);
    nodes = vec_concat(cv->arena, &nodes, &lead);
    if (tok_is(close, "}")) {
      after = inner.next + 1;
      if (close->end > cv->mark) {
        cv->mark = close->end;
      }
    } else {
      after = inner.next;
    }
    {
      Node *keep = node_new(cv->arena, NK_KEEP);
      keep->kids = inner.nodes;
      nodevec_push(cv->arena, &nodes, keep);
    }
  }
  while (after < cv->ncode) {
    const Tok *tok = code_at(cv, after);
    Parsed one;
    NodeVec trivia;
    if (tok == NULL || tok_is(tok, "}") || tok_is(tok, "case") || tok_is(tok, "default")) {
      break;
    }
    trivia = pull_trivia(cv, tok->start);
    nodes = vec_concat(cv->arena, &nodes, &trivia);
    one = parse_stmt(cv, after);
    if (one.next <= after) {
      break;
    }
    if (!is_empty_node(one.node)) {
      nodevec_push(cv->arena, &nodes, one.node);
    }
    if (one.end > cv->mark) {
      cv->mark = one.end;
    }
    after = one.next;
  }
  {
    Node *node = node_new(cv->arena, NK_COLON);
    node->text = header;
    node->where = WHERE_READY;
    node->kids = nodes;
    p.node = node;
  }
  p.next = after;
  p.end = offset_after(cv, after);
  return p;
}

static Parsed parse_control(Converter *cv, int i) {
  const Tok *kw = code_at(cv, i);
  Str keyword;
  int open;
  int close;
  Str header;
  const Tok *next_tok;
  BodyOut body;
  Node *node;
  const Tok *else_at;
  Parsed p;
  keyword = kw == NULL ? str_empty() : str_make(kw->text, kw->len);
  open = find_paren(cv, i);
  if (open < 0) {
    p.node = node_text(cv->arena, keyword);
    p.next = i + 1;
    p.end = offset_after(cv, i + 1);
    return p;
  }
  close = match_tok(cv, open, "(", ")");
  if (close < 0) {
    note_push(&cv->notes, kw == NULL ? 1 : kw->line,
        arena_printf(cv->arena, "Unclosed ( after %.*s.", kw == NULL ? 0 : (int)kw->len, kw == NULL ? "" : kw->text));
    p.node = node_text(cv->arena, keyword);
    p.next = i + 1;
    p.end = offset_after(cv, i + 1);
    return p;
  }
  header = header_text(cv, i, close + 1);
  next_tok = code_at(cv, close + 1);
  if (next_tok != NULL && !tok_is(next_tok, "{") && !tok_is(next_tok, ";")
      && has_directive_between(cv, cv->code[close].end, next_tok->start)) {
    Parsed stmt = parse_stmt(cv, close + 1);
    Str text = str_strip_line_ht(cv->arena, src_slice(cv, kw->start, offset_after(cv, stmt.next)));
    p.node = node_text(cv->arena, text);
    p.next = stmt.next;
    p.end = stmt.end;
    return p;
  }
  body = parse_body(cv, close + 1);
  node = headed(cv, header, WHERE_INLINE, &body, str_empty());
  else_at = code_at(cv, body.next);
  if (!str_eq_c(keyword, "if") || !tok_is(else_at, "else") || span_covering(cv, else_at->start) != NULL) {
    p.node = node;
    p.next = body.next;
    p.end = body.end;
    return p;
  }
  {
    NodeVec between = pull_trivia(cv, else_at->start);
    Parsed other = parse_else(cv, body.next);
    NodeVec flat;
    NodeVec nodes;
    Node *group;
    memset(&flat, 0, sizeof flat);
    flatten_into(cv->arena, other.node, &flat);
    memset(&nodes, 0, sizeof nodes);
    nodevec_push(cv->arena, &nodes, node);
    if (between.len > 0) {
      nodes = vec_concat(cv->arena, &nodes, &between);
    }
    nodes = vec_concat(cv->arena, &nodes, &flat);
    group = node_new(cv->arena, NK_GROUP);
    group->kids = nodes;
    p.node = group;
    p.next = other.next;
    p.end = other.end;
    return p;
  }
}

static Parsed parse_else(Converter *cv, int i) {
  const Tok *next = code_at(cv, i + 1);
  NodeVec between;
  bool split;
  Parsed p;
  memset(&between, 0, sizeof between);
  if (next != NULL) {
    between = pull_trivia(cv, next->start);
  }
  split = vec_has_raw(&between);
  if (tok_is(next, "if")) {
    Parsed inner = parse_control(cv, i + 1);
    if (split) {
      NodeVec flat;
      NodeVec nodes;
      Node *group;
      memset(&flat, 0, sizeof flat);
      flatten_into(cv->arena, inner.node, &flat);
      memset(&nodes, 0, sizeof nodes);
      nodevec_push(cv->arena, &nodes, node_text(cv->arena, str_lit("else")));
      nodes = vec_concat(cv->arena, &nodes, &between);
      nodes = vec_concat(cv->arena, &nodes, &flat);
      group = node_new(cv->arena, NK_GROUP);
      group->kids = nodes;
      p.node = group;
      p.next = inner.next;
      p.end = inner.end;
      return p;
    }
    p = inner;
    p.node = prefix_header(cv, inner.node, str_lit("else "));
    return p;
  }
  if (split && next != NULL) {
    Parsed stmt = parse_stmt(cv, i + 1);
    NodeVec flat;
    NodeVec nodes;
    Node *group;
    memset(&flat, 0, sizeof flat);
    flatten_into(cv->arena, stmt.node, &flat);
    memset(&nodes, 0, sizeof nodes);
    nodevec_push(cv->arena, &nodes, node_text(cv->arena, str_lit("else")));
    nodes = vec_concat(cv->arena, &nodes, &between);
    nodes = vec_concat(cv->arena, &nodes, &flat);
    group = node_new(cv->arena, NK_GROUP);
    group->kids = nodes;
    p.node = group;
    p.next = stmt.next;
    p.end = stmt.end;
    return p;
  }
  {
    BodyOut body = parse_body(cv, i + 1);
    Node *node = headed(cv, str_lit("else"), WHERE_INLINE, &body, str_empty());
    if (node->kind == NK_COLON && between.len > 0) {
      node->kids = vec_concat(cv->arena, &between, &node->kids);
    }
    p.node = node;
    p.next = body.next;
    p.end = body.end;
    return p;
  }
}

static Parsed parse_do(Converter *cv, int i) {
  BodyOut body = parse_body(cv, i + 1);
  int next = body.next;
  Str follow = str_empty();
  size_t end = body.end;
  Parsed p;
  if (tok_is(code_at(cv, next), "while")) {
    int open = find_paren(cv, next);
    int close = open < 0 ? -1 : match_tok(cv, open, "(", ")");
    if (close >= 0) {
      follow = str_trim(strip_semis(cv, next, close));
      next = close + 1;
      if (tok_is(code_at(cv, next), ";")) {
        next++;
      }
      end = offset_after(cv, next);
    }
  }
  p.node = headed(cv, str_lit("do"), WHERE_INLINE, &body, follow);
  p.next = next;
  p.end = end;
  return p;
}

static Parsed parse_try(Converter *cv, int i) {
  BodyOut body = parse_body(cv, i + 1);
  NodeVec nodes;
  int next = body.next;
  size_t end = body.end;
  Parsed p;
  memset(&nodes, 0, sizeof nodes);
  nodevec_push(cv->arena, &nodes, headed(cv, str_lit("try"), WHERE_INLINE, &body, str_empty()));
  while (tok_is(code_at(cv, next), "catch") || tok_is(code_at(cv, next), "finally")) {
    if (tok_is(code_at(cv, next), "finally")) {
      Parsed rest = parse_finally(cv, next);
      NodeVec flat;
      memset(&flat, 0, sizeof flat);
      flatten_into(cv->arena, rest.node, &flat);
      nodes = vec_concat(cv->arena, &nodes, &flat);
      next = rest.next;
      end = rest.end;
      continue;
    }
    {
      Parsed caught = parse_control(cv, next);
      NodeVec flat;
      memset(&flat, 0, sizeof flat);
      flatten_into(cv->arena, caught.node, &flat);
      nodes = vec_concat(cv->arena, &nodes, &flat);
      next = caught.next;
      end = caught.end;
    }
  }
  if (nodes.len == 1) {
    p.node = nodes.items[0];
  } else {
    Node *group = node_new(cv->arena, NK_GROUP);
    group->kids = nodes;
    p.node = group;
  }
  p.next = next;
  p.end = end;
  return p;
}

static Parsed parse_finally(Converter *cv, int i) {
  BodyOut body = parse_body(cv, i + 1);
  Parsed p;
  p.node = headed(cv, str_lit("finally"), WHERE_INLINE, &body, str_empty());
  p.next = body.next;
  p.end = body.end;
  return p;
}

static BodyOut parse_body(Converter *cv, int i) {
  BodyOut body;
  memset(&body, 0, sizeof body);
  if (i >= cv->ncode) {
    body.next = i;
    body.end = cv->mark;
    return body;
  }
  if (tok_is(code_at(cv, i), ";")) {
    body.next = i + 1;
    body.end = offset_after(cv, i + 1);
    return body;
  }
  if (tok_is(code_at(cv, i), "{")) {
    SeqOut inner;
    const Tok *close;
    body.lead = lead_in(cv, i);
    inner = parse_seq(cv, i + 1, true);
    close = code_at(cv, inner.next);
    body.nodes = inner.nodes;
    body.next = tok_is(close, "}") ? inner.next + 1 : inner.next;
    body.end = offset_after(cv, body.next);
    return body;
  }
  {
    Parsed one = parse_stmt(cv, i);
    if (!is_empty_node(one.node) && one.node != NULL) {
      nodevec_push(cv->arena, &body.nodes, one.node);
    }
    body.next = one.next;
    body.end = one.end;
    return body;
  }
}

static Parsed parse_open_brace(Converter *cv, int i) {
  BraceRole role = brace_role(cv, i);
  int close;
  Parsed p;
  if (role == ROLE_COLON) {
    return finish_colon(cv, i, i);
  }
  close = match_tok(cv, i, "{", "}");
  if (close < 0) {
    const Tok *tok = code_at(cv, i);
    note_push(&cv->notes, tok == NULL ? 1 : tok->line, "Unclosed { in the host source.");
    p.node = node_text(cv->arena, str_lit("{"));
    p.next = i + 1;
    p.end = offset_after(cv, i + 1);
    return p;
  }
  if (role == ROLE_INIT) {
    int semi = scan_semi(cv, close + 1);
    int to = semi < 0 ? close : semi;
    int next = semi < 0 ? close + 1 : semi + 1;
    p.node = node_text(cv->arena, strip_semis(cv, i, to));
    p.next = next;
    p.end = offset_after(cv, next);
    return p;
  }
  if (cv->code[i].end > cv->mark) {
    cv->mark = cv->code[i].end;
  }
  {
    SeqOut inner = parse_seq(cv, i + 1, true);
    Node *keep = node_new(cv->arena, NK_KEEP);
    int next = tok_is(code_at(cv, inner.next), "}") ? inner.next + 1 : close + 1;
    keep->kids = inner.nodes;
    p.node = keep;
    p.next = next;
    p.end = offset_after(cv, next);
    return p;
  }
}

static Parsed finish_colon(Converter *cv, int start, int brace) {
  int close = match_tok(cv, brace, "{", "}");
  Parsed p;
  if (close < 0) {
    const Tok *tok = code_at(cv, brace);
    note_push(&cv->notes, tok == NULL ? 1 : tok->line, "Unclosed { in the host source.");
    p.node = node_text(cv->arena, strip_semis(cv, start, brace));
    p.next = brace + 1;
    p.end = offset_after(cv, brace + 1);
    return p;
  }
  {
    Str header = header_text(cv, start, brace);
    bool is_enum = has_keyword(cv, start, brace, "enum");
    NodeVec lead = lead_in(cv, brace);
    NodeVec body;
    int next = close + 1;
    bool aggregate = cv->lang == LANG_C && has_aggregate(cv, start, brace);
    Str tail = str_empty();
    ColonWhere where;
    Node *node;
    memset(&body, 0, sizeof body);
    if (is_enum) {
      NodeVec items = enum_items(cv, brace, close);
      body = vec_concat(cv->arena, &lead, &items);
    } else {
      SeqOut inner = parse_seq(cv, brace + 1, true);
      body = vec_concat(cv->arena, &lead, &inner.nodes);
    }
    if (aggregate && code_at(cv, next) != NULL && !tok_is(code_at(cv, next), ";") && !tok_is(code_at(cv, next), "}")) {
      int semi = scan_semi(cv, next);
      if (semi >= 0) {
        tail = strip_semis(cv, next, semi - 1);
        next = semi + 1;
      }
    } else if (aggregate && tok_is(code_at(cv, next), ";")) {
      next++;
    }
    where = colon_where(cv, brace, header);
    node = node_new(cv->arena, NK_COLON);
    node->text = header;
    node->where = where;
    node->kids = body;
    if (str_trim(tail).n > 0) {
      node = reshape_aggregate(cv, start, brace, node, tail);
    }
    p.node = node;
    p.next = next;
    p.end = offset_after(cv, next);
    return p;
  }
}

static Parsed parse_general(Converter *cv, int i) {
  int j = i;
  int paren = 0;
  int bracket = 0;
  bool saw_eq = false;
  int fn_close = -1;
  MixVec items;
  int chunk = i;
  Parsed p;
  memset(&items, 0, sizeof items);
  while (j < cv->ncode) {
    const Tok *tok = code_at(cv, j);
    if (tok == NULL) {
      break;
    }
    if (tok_is(tok, "}") && paren == 0 && bracket == 0) {
      break;
    }
    if ((tok_is(tok, "case") || tok_is(tok, "default")) && paren == 0 && bracket == 0 && j > i) {
      break;
    }
    if (j > i && paren == 0 && bracket == 0
        && (is_control_tok(tok) || tok_is(tok, "else") || tok_is(tok, "do") || tok_is(tok, "try") || tok_is(tok, "finally"))) {
      break;
    }
    if (cv->lang == LANG_C && tok_is(tok, "{") && j > 0 && tok_is(code_at(cv, j - 1), "(")) {
      const Tok *open = tok;
      SeqOut inner;
      int after;
      Str suffix = str_empty();
      Mix sexpr;
      push_text_item(cv, &items, chunk, j - 2);
      if (open->end > cv->mark) {
        cv->mark = open->end;
      }
      inner = parse_seq(cv, j + 1, true);
      after = tok_is(code_at(cv, inner.next), "}") ? inner.next + 1 : inner.next;
      if (tok_is(code_at(cv, after), ")")) {
        suffix = inline_after(cv, after + 1);
        after++;
      }
      memset(&sexpr, 0, sizeof sexpr);
      sexpr.sexpr = true;
      sexpr.body = inner.nodes;
      mixvec_push(cv->arena, &items, sexpr);
      if (str_trim(suffix).n > 0) {
        Mix extra;
        memset(&extra, 0, sizeof extra);
        extra.sexpr = false;
        extra.text = suffix;
        mixvec_push(cv->arena, &items, extra);
      }
      {
        int semi = scan_semi(cv, after);
        if (semi >= 0 && only_trivia(after, semi)) {
          j = semi + 1;
          chunk = j;
          break;
        }
      }
      j = after;
      chunk = j;
      continue;
    }
    if (tok_is(tok, "(")) {
      paren++;
    } else if (tok_is(tok, ")") && paren > 0) {
      paren--;
      if (paren == 0 && bracket == 0 && fn_close < 0) {
        fn_close = j;
      }
    } else if (tok_is(tok, "[")) {
      bracket++;
    } else if (tok_is(tok, "]") && bracket > 0) {
      bracket--;
    } else if (tok_is(tok, "=") && paren == 0 && bracket == 0) {
      saw_eq = true;
    } else if (tok_is(tok, "{") && paren == 0 && bracket == 0) {
      BraceRole role = brace_role(cv, j);
      bool kr = !saw_eq && fn_close >= 0 && j > fn_close + 1 && kr_body(cv, fn_close, j);
      if (role == ROLE_COLON || kr) {
        Parsed colon;
        if (items.len == 0) {
          return finish_colon(cv, i, j);
        }
        colon = finish_colon(cv, i, j);
        {
          NodeVec mixed = mix_nodes(cv, &items);
          NodeVec nodes;
          Node *group;
          memset(&nodes, 0, sizeof nodes);
          nodes = vec_concat(cv->arena, &mixed, NULL);
          nodevec_push(cv->arena, &nodes, colon.node);
          group = node_new(cv->arena, NK_GROUP);
          group->kids = nodes;
          p.node = group;
          p.next = colon.next;
          p.end = colon.end;
          return p;
        }
      }
      {
        int close = match_tok(cv, j, "{", "}");
        if (close < 0) {
          note_push(&cv->notes, tok->line, "Unclosed { in the host source.");
          break;
        }
        j = close + 1;
        continue;
      }
    } else if (tok_is(tok, ";") && paren == 0 && bracket == 0) {
      NodeVec mixed;
      if (!saw_eq && fn_close >= 0 && j > fn_close + 1 && kr_decl(cv, fn_close, j) && kr_continues(cv, j)) {
        j++;
        continue;
      }
      push_text_item(cv, &items, chunk, j - 1);
      {
        const Tok *prev = code_at(cv, j - 1);
        if (prev != NULL && has_directive_between(cv, prev->end, tok->start)) {
          if (items.len > 0 && !items.items[items.len - 1u].sexpr) {
            Str last = str_rtrim_ws(items.items[items.len - 1u].text);
            items.items[items.len - 1u].text = str_join(cv->arena, last, str_lit("\n;"));
          } else {
            Mix semi;
            memset(&semi, 0, sizeof semi);
            semi.sexpr = false;
            semi.text = str_lit(";");
            mixvec_push(cv->arena, &items, semi);
          }
        }
      }
      mixed = mix_nodes(cv, &items);
      p.node = unwrap_nodes(cv, mixed);
      p.next = j + 1;
      p.end = offset_after(cv, j + 1);
      return p;
    }
    j++;
  }
  push_text_item(cv, &items, chunk, j - 1);
  p.node = unwrap_nodes(cv, mix_nodes(cv, &items));
  p.next = j;
  p.end = offset_after(cv, j);
  return p;
}

static Parsed parse_stmt(Converter *cv, int i) {
  const Tok *tok = code_at(cv, i);
  Parsed p;
  if (tok == NULL) {
    p.node = node_text(cv->arena, str_empty());
    p.next = i;
    p.end = cv->mark;
    return p;
  }
  if (tok_is(tok, "case") || tok_is(tok, "default")) {
    return parse_case(cv, i);
  }
  if (is_label(cv, i)) {
    return parse_label(cv, i);
  }
  if (is_control_tok(tok)) {
    return parse_control(cv, i);
  }
  if (tok_is(tok, "do")) {
    return parse_do(cv, i);
  }
  if (tok_is(tok, "try")) {
    return parse_try(cv, i);
  }
  if (tok_is(tok, "else")) {
    return parse_else(cv, i);
  }
  if (tok_is(tok, "finally")) {
    return parse_finally(cv, i);
  }
  if (tok_is(tok, ";")) {
    const Tok *prev = code_at(cv, i - 1);
    if (prev != NULL && has_directive_between(cv, prev->end, tok->start)) {
      p.node = node_text(cv->arena, str_lit(";"));
      p.next = i + 1;
      p.end = tok->end;
      return p;
    }
    p.node = node_text(cv->arena, str_empty());
    p.next = i + 1;
    p.end = offset_after(cv, i + 1);
    return p;
  }
  if (tok_is(tok, "{")) {
    return parse_open_brace(cv, i);
  }
  return parse_general(cv, i);
}

static bool line_blank(const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (!rt_is_space(s[i])) {
      return false;
    }
  }
  return true;
}

static size_t leading_spaces(const char *s, size_t n) {
  size_t i = 0;
  while (i < n && s[i] == ' ') {
    i++;
  }
  return i;
}

static Str expand_tabs(Arena *arena, Str text) {
  Buf buf;
  size_t i;
  Str r;
  if (text.s == NULL || text.n == 0) {
    return str_empty();
  }
  buf_init(&buf);
  for (i = 0; i < text.n; i++) {
    if (text.s[i] == '\t') {
      buf_repeat(&buf, ' ', 4u);
    } else {
      buf_putc(&buf, text.s[i]);
    }
  }
  r.n = buf.len;
  r.s = buf_arena(&buf, arena);
  buf_free(&buf);
  return r;
}

static Str make_indent_line(Arena *arena, size_t nspaces, Str rest) {
  Buf buf;
  Str r;
  buf_init(&buf);
  buf_repeat(&buf, ' ', nspaces);
  buf_append(&buf, rest.s, rest.n);
  r.n = buf.len;
  r.s = buf_arena(&buf, arena);
  buf_free(&buf);
  return r;
}

static void reindent(Arena *arena, Str text, int indent, bool anchor_first, StrVec *out) {
  Str cleaned;
  size_t b;
  size_t n;
  size_t base = 0;
  bool have = false;
  size_t i;
  int ind = indent < 0 ? 0 : indent;
  cleaned = expand_tabs(arena, text);
  b = 0;
  while (b < cleaned.n && cleaned.s[b] == '\n') {
    b++;
  }
  n = cleaned.n;
  while (n > b && rt_is_space(cleaned.s[n - 1u])) {
    n--;
  }
  cleaned = str_make(cleaned.n == 0 ? "" : cleaned.s + b, n - b);
  if (str_trim(cleaned).n == 0) {
    return;
  }
  i = 0;
  while (i < cleaned.n) {
    size_t start = i;
    size_t len;
    while (i < cleaned.n && cleaned.s[i] != '\n') {
      i++;
    }
    len = i - start;
    if (!line_blank(cleaned.s + start, len)) {
      size_t w = leading_spaces(cleaned.s + start, len);
      if (!have) {
        base = w;
        have = true;
      } else if (!anchor_first && w < base) {
        base = w;
      }
    }
    if (i < cleaned.n && cleaned.s[i] == '\n') {
      i++;
    }
  }
  i = 0;
  while (i < cleaned.n) {
    size_t start = i;
    size_t len;
    while (i < cleaned.n && cleaned.s[i] != '\n') {
      i++;
    }
    len = i - start;
    if (line_blank(cleaned.s + start, len)) {
      strvec_push(arena, out, str_empty());
    } else {
      size_t w = leading_spaces(cleaned.s + start, len);
      size_t extra = w > base ? w - base : 0;
      size_t c = 0;
      size_t total;
      Str rest;
      while (c < len && rt_is_space(cleaned.s[start + c])) {
        c++;
      }
      if ((size_t)ind > SIZE_MAX - extra) {
        rt_oom();
      }
      total = (size_t)ind + extra;
      rest = str_make(cleaned.s + start + c, len - c);
      strvec_push(arena, out, make_indent_line(arena, total, rest));
    }
    if (i < cleaned.n && cleaned.s[i] == '\n') {
      i++;
    }
  }
}

static Str spaces_only(Arena *arena, int indent) {
  int n = indent < 0 ? 0 : indent;
  Str r;
  r.s = arena_repeat(arena, ' ', (size_t)n);
  r.n = (size_t)n;
  return r;
}

static void render_children(Converter *cv, const NodeVec *kids, int indent, StrVec *out) {
  size_t i;
  for (i = 0; i < kids->len; i++) {
    render_node(cv, kids->items[i], indent, out);
  }
}

static void render_node(Converter *cv, const Node *node, int indent, StrVec *out) {
  if (node == NULL) {
    return;
  }
  switch (node->kind) {
    case NK_BLANK:
      strvec_push(cv->arena, out, str_empty());
      break;
    case NK_RAW:
    case NK_COMMENT:
    case NK_TEXT:
      reindent(cv->arena, node->text, indent, false, out);
      break;
    case NK_GROUP:
      render_children(cv, &node->kids, indent, out);
      break;
    case NK_KEEP:
      strvec_push(cv->arena, out, str_join(cv->arena, spaces_only(cv->arena, indent), str_lit("{")));
      render_children(cv, &node->kids, indent + 4, out);
      strvec_push(cv->arena, out, str_join(cv->arena, spaces_only(cv->arena, indent), str_lit("}")));
      break;
    case NK_SEXPR: {
      StrVec pre;
      Str suffix;
      size_t k;
      memset(&pre, 0, sizeof pre);
      reindent(cv->arena, node->text, indent, false, &pre);
      if (pre.len == 0) {
        strvec_push(cv->arena, &pre, spaces_only(cv->arena, indent));
      }
      {
        Str last = str_rtrim_ht(cv->arena, pre.items[pre.len - 1u]);
        pre.items[pre.len - 1u] = str_join(cv->arena, last, str_lit(" ({"));
      }
      for (k = 0; k < pre.len; k++) {
        strvec_push(cv->arena, out, pre.items[k]);
      }
      render_children(cv, &node->kids, indent + 4, out);
      suffix = str_trim(node->suffix);
      if (suffix.n > 0) {
        strvec_push(cv->arena, out, str_join3(cv->arena, spaces_only(cv->arena, indent), str_lit("}) "), suffix));
      } else {
        strvec_push(cv->arena, out, str_join(cv->arena, spaces_only(cv->arena, indent), str_lit("})")));
      }
      break;
    }
    case NK_COLON: {
      StrVec lines;
      StrVec lead;
      memset(&lines, 0, sizeof lines);
      memset(&lead, 0, sizeof lead);
      reindent(cv->arena, node->text, indent, true, &lines);
      if (lines.len == 0) {
        strvec_push(cv->arena, &lines, spaces_only(cv->arena, indent));
      }
      if (node->has_lead) {
        size_t k;
        for (k = 0; k < node->lead.len; k++) {
          render_node(cv, node->lead.items[k], indent, &lead);
        }
      }
      if (lead.len == 0 && node->where == WHERE_INLINE) {
        Str last = str_rtrim_ht(cv->arena, lines.items[lines.len - 1u]);
        lines.items[lines.len - 1u] = str_join(cv->arena, last, str_lit(":"));
      } else if (lead.len > 0 || node->where == WHERE_OWN) {
        size_t k;
        for (k = 0; k < lead.len; k++) {
          strvec_push(cv->arena, &lines, lead.items[k]);
        }
        strvec_push(cv->arena, &lines, str_join(cv->arena, spaces_only(cv->arena, indent), str_lit(":")));
      }
      {
        size_t k;
        for (k = 0; k < lines.len; k++) {
          strvec_push(cv->arena, out, lines.items[k]);
        }
      }
      render_children(cv, &node->kids, indent + 4, out);
      if (node->tail.n > 0) {
        reindent(cv->arena, node->tail, indent, false, out);
      }
      if (node->follow.n > 0) {
        reindent(cv->arena, node->follow, indent, false, out);
      }
      break;
    }
    default:
      break;
  }
}

static Str finish_lines(Arena *arena, const StrVec *lines) {
  StrVec out;
  size_t i;
  Buf buf;
  Str result;
  memset(&out, 0, sizeof out);
  for (i = 0; i < lines->len; i++) {
    Str line = lines->items[i];
    if (line.n == 0 && (out.len == 0 || out.items[out.len - 1u].n == 0)) {
      continue;
    }
    strvec_push(arena, &out, str_rtrim_ht(arena, line));
  }
  while (out.len > 0 && out.items[out.len - 1u].n == 0) {
    out.len--;
  }
  if (out.len == 0) {
    return str_lit("");
  }
  buf_init(&buf);
  for (i = 0; i < out.len; i++) {
    if (i > 0) {
      buf_putc(&buf, '\n');
    }
    buf_append(&buf, out.items[i].s, out.items[i].n);
  }
  buf_putc(&buf, '\n');
  result.n = buf.len;
  result.s = buf_arena(&buf, arena);
  buf_free(&buf);
  return result;
}

static void converter_init(Converter *cv, Arena *arena, LangId lang, const LexOut *lexed) {
  int i;
  int ncode = 0;
  size_t n;
  memset(cv, 0, sizeof *cv);
  cv->arena = arena;
  cv->lang = lang;
  cv->source = lexed->source == NULL ? "" : lexed->source;
  cv->sourcelen = lexed->source == NULL ? 0 : lexed->sourcelen;
  if (lexed->ntokens > (size_t)INT_MAX) {
    rt_oom();
  }
  cv->ntokens = (int)lexed->ntokens;
  cv->tokens = lexed->tokens;
  note_init(&cv->notes, arena);
  for (n = 0; n < lexed->nnotes; n++) {
    note_push(&cv->notes, lexed->notes[n].line, lexed->notes[n].message);
  }
  for (i = 0; i < cv->ntokens; i++) {
    if (cv->tokens[i].kind != TOK_COMMENT && cv->tokens[i].kind != TOK_DIRECTIVE) {
      ncode++;
    }
  }
  cv->ncode = ncode;
  if (ncode > 0) {
    int w = 0;
    cv->code = arena_alloc(arena, (size_t)ncode * sizeof(Tok), _Alignof(Tok));
    for (i = 0; i < cv->ntokens; i++) {
      if (cv->tokens[i].kind != TOK_COMMENT && cv->tokens[i].kind != TOK_DIRECTIVE) {
        cv->code[w++] = cv->tokens[i];
      }
    }
  }
}

static Str converter_run(Converter *cv) {
  SeqOut seq = parse_seq(cv, 0, false);
  NodeVec trailing = pull_trivia(cv, cv->sourcelen);
  NodeVec all = vec_concat(cv->arena, &seq.nodes, &trailing);
  StrVec lines;
  size_t i;
  memset(&lines, 0, sizeof lines);
  for (i = 0; i < all.len; i++) {
    render_node(cv, all.items[i], 0, &lines);
  }
  return finish_lines(cv->arena, &lines);
}

Result to_spaced(const char *source, size_t len, LangId lang) {
  Result result;
  Arena *arena;
  LexOut lexed;
  Converter cv;
  Str code;
  memset(&result, 0, sizeof result);
  if (lang == LANG_PASCAL) {
    return to_pascal_spaced(source, len);
  }
  arena = arena_new();
  lex_source(arena, source, len, profile_get(lang), &lexed);
  converter_init(&cv, arena, lang, &lexed);
  code = converter_run(&cv);
  result.arena = arena;
  result.language = lang;
  result.lines = NULL;
  result.nlines = 0;
  result.code = code.s == NULL ? arena_cstr(arena, "") : code.s;
  result.notes = cv.notes.items;
  result.nnotes = cv.notes.len;
  return result;
}
