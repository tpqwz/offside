/* Highlighting scanner for the offside dialects.
   A colon is a block only when the next content line is indented strictly
   deeper than the line that holds the colon. ::, :=, and a mid-line colon
   are not blocks. Columns match the compiler: tabs are stops of 8.

   get_column() is not used. Tree-sitter resumes at mark_end, so a column
   measured after a lookahead does not describe the next token. The indent
   of a line is recorded while peeking from the newline that precedes it.
*/
#include "tree_sitter/parser.h"
#include <stdlib.h>
#include <string.h>

#ifndef TS_HAS_BLOCK_COMMENT
#define TS_HAS_BLOCK_COMMENT 1
#endif
#ifndef TS_HAS_PASCAL_COMMENT
#define TS_HAS_PASCAL_COMMENT 0
#endif

enum {
  BLOCK_COLON,
  NEWLINE,
  DEDENT,
};

typedef struct {
  uint16_t stack[128];
  uint8_t nstack;
  uint16_t line_indent;
  uint8_t bol;
  uint8_t line_blank;
  uint8_t eof_newline;
} Scanner;

static void eat(TSLexer *lexer) { lexer->advance(lexer, false); }

static int at_eol(TSLexer *lexer) {
  return lexer->eof(lexer) || lexer->lookahead == '\n' || lexer->lookahead == '\r';
}

static uint16_t tab_stop(uint16_t col) {
  uint16_t next = (uint16_t)((col + 8u) & ~7u);
  return next == col ? (uint16_t)(col + 8u) : next;
}

static void skip_horiz(TSLexer *lexer, uint16_t *col) {
  for (;;) {
    if (lexer->lookahead == ' ') {
      if (*col < 65535u) {
        *col += 1u;
      }
      eat(lexer);
    } else if (lexer->lookahead == '\t') {
      *col = tab_stop(*col);
      eat(lexer);
    } else if (lexer->lookahead == '\f' || lexer->lookahead == '\v') {
      eat(lexer);
    } else {
      break;
    }
  }
}

static void skip_until_eol(TSLexer *lexer) {
  while (!at_eol(lexer)) {
    eat(lexer);
  }
}

static void skip_block_comment(TSLexer *lexer) {
  while (!lexer->eof(lexer)) {
    if (lexer->lookahead == '*') {
      eat(lexer);
      if (lexer->lookahead == '/') {
        eat(lexer);
        return;
      }
    } else {
      eat(lexer);
    }
  }
}

static void skip_pascal_brace(TSLexer *lexer) {
  while (!at_eol(lexer) && lexer->lookahead != '}') {
    eat(lexer);
  }
  if (lexer->lookahead == '}') {
    eat(lexer);
  }
}

static void skip_pascal_paren(TSLexer *lexer) {
  while (!lexer->eof(lexer)) {
    if (lexer->lookahead == '*') {
      eat(lexer);
      if (lexer->lookahead == ')') {
        eat(lexer);
        return;
      }
    } else {
      eat(lexer);
    }
  }
}

/* 1 if the rest of this line has no code. Records the column of the first
   code character when it returns 0. Leaves the lexer at eol for a blank
   line, and somewhere on the line otherwise. Peek only: call after mark_end
   or from a scan that will return false. */
static int rest_is_blank(TSLexer *lexer, uint16_t *content_col) {
  uint16_t col = 0;
  skip_horiz(lexer, &col);
  *content_col = col;
  for (;;) {
    if (at_eol(lexer)) {
      return 1;
    }
    if (lexer->lookahead == '/') {
      eat(lexer);
      if (lexer->lookahead == '/') {
        skip_until_eol(lexer);
        continue;
      }
#if TS_HAS_BLOCK_COMMENT
      if (lexer->lookahead == '*') {
        eat(lexer);
        skip_block_comment(lexer);
        skip_horiz(lexer, &col);
        continue;
      }
#endif
      return 0;
    }
#if TS_HAS_PASCAL_COMMENT
    if (lexer->lookahead == '{') {
      eat(lexer);
      if (lexer->lookahead == '$') {
        return 0;
      }
      skip_pascal_brace(lexer);
      skip_horiz(lexer, &col);
      continue;
    }
    if (lexer->lookahead == '(') {
      eat(lexer);
      if (lexer->lookahead == '*') {
        eat(lexer);
        if (lexer->lookahead == '$') {
          return 0;
        }
        skip_pascal_paren(lexer);
        skip_horiz(lexer, &col);
        continue;
      }
      return 0;
    }
#endif
    return 0;
  }
}

static void consume_nl(TSLexer *lexer) {
  if (lexer->lookahead == '\r') {
    eat(lexer);
  }
  if (lexer->lookahead == '\n') {
    eat(lexer);
  }
}

/* From the EOL after a colon, find the next line that contains code. */
static int peek_body_indent(TSLexer *lexer, uint16_t *out) {
  uint16_t ignored = 0;
  if (!rest_is_blank(lexer, &ignored)) {
    return 0;
  }
  if (lexer->eof(lexer)) {
    return 0;
  }
  for (;;) {
    if (lexer->lookahead != '\n' && lexer->lookahead != '\r') {
      return 0;
    }
    consume_nl(lexer);
    if (lexer->eof(lexer)) {
      return 0;
    }
    uint16_t col = 0;
    if (!rest_is_blank(lexer, &col)) {
      *out = col;
      return 1;
    }
    if (lexer->eof(lexer)) {
      return 0;
    }
    if (lexer->lookahead != '\n' && lexer->lookahead != '\r') {
      skip_until_eol(lexer);
    }
  }
}

static uint16_t top_of(const Scanner *scanner) {
  return scanner->stack[scanner->nstack - 1u];
}

static int scan_block_colon(Scanner *scanner, TSLexer *lexer) {
  uint16_t body = 0;
  if (lexer->lookahead != ':') {
    return 0;
  }
  eat(lexer);
  if (lexer->lookahead == ':' || lexer->lookahead == '=') {
    return 0;
  }
  lexer->mark_end(lexer);
  if (!peek_body_indent(lexer, &body)) {
    return 0;
  }
  if (body <= scanner->line_indent) {
    return 0;
  }
  if (scanner->nstack < 128) {
    scanner->stack[scanner->nstack++] = body;
  }
  lexer->result_symbol = BLOCK_COLON;
  return 1;
}

static int scan_newline(Scanner *scanner, TSLexer *lexer) {
  uint16_t col = 0;
  if (lexer->eof(lexer)) {
    if (scanner->eof_newline) {
      return 0;
    }
    scanner->eof_newline = 1;
    scanner->bol = 1;
    scanner->line_indent = 0;
    scanner->line_blank = 1;
    lexer->result_symbol = NEWLINE;
    return 1;
  }
  if (lexer->lookahead != '\n' && lexer->lookahead != '\r') {
    return 0;
  }
  consume_nl(lexer);
  lexer->mark_end(lexer);
  scanner->line_blank = (uint8_t)rest_is_blank(lexer, &col);
  scanner->line_indent = scanner->line_blank ? 0u : col;
  scanner->bol = 1;
  scanner->eof_newline = 0;
  lexer->result_symbol = NEWLINE;
  return 1;
}

void *TS_SCANNER_CREATE(void) {
  Scanner *scanner = calloc(1, sizeof(Scanner));
  scanner->nstack = 1;
  scanner->stack[0] = 0;
  scanner->bol = 1;
  return scanner;
}

void TS_SCANNER_DESTROY(void *payload) { free(payload); }

unsigned TS_SCANNER_SERIALIZE(void *payload, char *buffer) {
  Scanner *scanner = payload;
  unsigned i;
  unsigned n = 6u + (unsigned)scanner->nstack * 2u;
  if (n > 1024u) {
    return 0;
  }
  buffer[0] = (char)scanner->nstack;
  buffer[1] = (char)(scanner->line_indent & 0xffu);
  buffer[2] = (char)((scanner->line_indent >> 8) & 0xffu);
  buffer[3] = (char)scanner->bol;
  buffer[4] = (char)scanner->line_blank;
  buffer[5] = (char)scanner->eof_newline;
  for (i = 0; i < scanner->nstack; i++) {
    buffer[6u + i * 2u] = (char)(scanner->stack[i] & 0xffu);
    buffer[7u + i * 2u] = (char)((scanner->stack[i] >> 8) & 0xffu);
  }
  return n;
}

void TS_SCANNER_DESERIALIZE(void *payload, const char *buffer, unsigned length) {
  Scanner *scanner = payload;
  unsigned i;
  scanner->nstack = 1;
  scanner->stack[0] = 0;
  scanner->line_indent = 0;
  scanner->bol = 1;
  scanner->line_blank = 0;
  scanner->eof_newline = 0;
  if (length < 6) {
    return;
  }
  scanner->nstack = (uint8_t)buffer[0];
  if (scanner->nstack == 0 || scanner->nstack > 128) {
    scanner->nstack = 1;
  }
  scanner->line_indent = (uint16_t)((unsigned char)buffer[1] | ((unsigned char)buffer[2] << 8));
  scanner->bol = (uint8_t)buffer[3];
  scanner->line_blank = (uint8_t)buffer[4];
  scanner->eof_newline = (uint8_t)buffer[5];
  if (length < 6u + (unsigned)scanner->nstack * 2u) {
    scanner->nstack = 1;
    return;
  }
  for (i = 0; i < scanner->nstack; i++) {
    scanner->stack[i] = (uint16_t)((unsigned char)buffer[6u + i * 2u] | ((unsigned char)buffer[7u + i * 2u] << 8));
  }
}

bool TS_SCANNER_SCAN(void *payload, TSLexer *lexer, const bool *valid) {
  Scanner *scanner = payload;

  if (lexer->eof(lexer)) {
    if (scanner->nstack > 1 && valid[DEDENT]) {
      scanner->nstack--;
      lexer->result_symbol = DEDENT;
      return true;
    }
    if (valid[NEWLINE]) {
      return scan_newline(scanner, lexer);
    }
    return false;
  }

  if (scanner->bol && !scanner->line_blank && scanner->line_indent < top_of(scanner) &&
      scanner->nstack > 1 && valid[DEDENT]) {
    scanner->nstack--;
    lexer->mark_end(lexer);
    lexer->result_symbol = DEDENT;
    return true;
  }

  if (scanner->bol && lexer->lookahead != ' ' && lexer->lookahead != '\t' &&
      lexer->lookahead != '\f' && lexer->lookahead != '\v' && lexer->lookahead != '\n' &&
      lexer->lookahead != '\r') {
    scanner->bol = 0;
  }

  if (valid[BLOCK_COLON] && scan_block_colon(scanner, lexer)) {
    return true;
  }

  if (valid[NEWLINE] && scan_newline(scanner, lexer)) {
    return true;
  }

  return false;
}
