#ifndef OFFSIDE_ENGINE_H
#define OFFSIDE_ENGINE_H

#include "rt.h"

typedef enum LangId {
  LANG_C = 0,
  LANG_JAVA = 1,
  LANG_JAVASCRIPT = 2,
  LANG_PASCAL = 3
} LangId;

typedef enum TokenKind {
  TOK_IDENT = 0,
  TOK_KEYWORD,
  TOK_NUMBER,
  TOK_STRING,
  TOK_CHAR,
  TOK_TEMPLATE,
  TOK_REGEX,
  TOK_COMMENT,
  TOK_DIRECTIVE,
  TOK_PUNCT
} TokenKind;

typedef struct Tok {
  TokenKind kind;
  const char *text;
  size_t len;
  int line;
  int col;
  size_t start;
  size_t end;
} Tok;

typedef struct Note {
  int line;
  const char *message;
} Note;

typedef struct NoteBuf {
  Note *items;
  size_t len;
  size_t cap;
  Arena *arena;
} NoteBuf;

void note_init(NoteBuf *notes, Arena *arena);
void note_push(NoteBuf *notes, int line, const char *message);
void note_unshift(NoteBuf *notes, int line, const char *message);

typedef struct Profile {
  LangId id;
  const char *const *keywords;
  size_t nkeywords;
  const char *const *semi_close;
  size_t nsemi_close;
  bool preprocessor;
  bool templates;
  bool regex;
} Profile;

const Profile *profile_get(LangId lang);
bool profile_keyword(const Profile *profile, const char *word, size_t len);
bool profile_semi_close(const Profile *profile, const char *word, size_t len);
bool lang_parse(const char *text, LangId *out);

typedef struct LexOut {
  Tok *tokens;
  size_t ntokens;
  Note *notes;
  size_t nnotes;
  const char *source;
  size_t sourcelen;
} LexOut;

void lex_source(Arena *arena, const char *source, size_t len, const Profile *profile, LexOut *out);

typedef struct OutSeg {
  const char *text;
  bool inserted;
} OutSeg;

typedef struct OutLine {
  OutSeg *segs;
  size_t nsegs;
  int source_line;
} OutLine;

typedef struct Result {
  Arena *arena;
  LangId language;
  OutLine *lines;
  size_t nlines;
  const char *code;
  Note *notes;
  size_t nnotes;
} Result;

Result layout_source(const char *source, size_t len, LangId lang);
Result to_spaced(const char *source, size_t len, LangId lang);
Result to_pascal_spaced(const char *source, size_t len);
void result_free(Result *result);

typedef struct HostFile {
  const char *code;
  int *line_map;
  size_t nmap;
} HostFile;

void render_host(Arena *arena, const Result *result, const char *source_name, bool line_directives, HostFile *out);
char *remap_diagnostics(Arena *arena, const char *text, const char *generated_name, const char *source_name, const int *line_map, size_t nmap);

typedef struct SourceKind {
  LangId lang;
  bool host;
  bool ok;
  const char *error;
} SourceKind;

SourceKind classify_file(const char *path, const char *override_lang);
const char *host_file_name(Arena *arena, LangId lang, const char *source_path, const char *code);

#endif
