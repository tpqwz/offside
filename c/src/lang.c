#include "engine.h"

#include <string.h>

static const char *const C_KEYWORDS[] = {
  "auto", "break", "case", "char", "const", "continue", "default", "do",
  "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
  "int", "long", "register", "restrict", "return", "short", "signed", "sizeof",
  "static", "struct", "switch", "typedef", "union", "unsigned", "void",
  "volatile", "while", "_Bool", "_Complex", "_Imaginary",
  "_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn", "_Static_assert", "_Thread_local",
  "alignas", "alignof", "bool", "constexpr", "false", "nullptr", "static_assert",
  "thread_local", "true", "typeof", "typeof_unqual", "_BitInt",
  "asm", "__asm__", "__attribute__", "__attribute", "__declspec", "__typeof__",
  "__typeof_unqual__", "__auto_type", "__volatile__", "__inline__", "__restrict__",
  "__extension__", "__label__", "__thread", "__alignof__", "__alignof", "__complex__",
  "__real__", "__imag__", "__int128", "__signed__", "__unsigned__",
};

static const char *const PASCAL_KEYWORDS[] = {
  "absolute", "abstract", "add", "and", "array", "as", "asm", "assembler",
  "begin", "break", "case", "cdecl", "class", "const", "constructor", "contains",
  "continue", "cppdecl", "default", "destructor", "dispinterface", "div", "do",
  "downto", "else", "end", "except", "exit", "export", "exports", "external",
  "false", "far", "file", "final", "finalization", "finally", "for", "forward",
  "function", "generic", "goto", "helper", "if", "implementation", "implements",
  "in", "index", "inherited", "initialization", "inline", "interface", "is",
  "label", "library", "message", "mod", "nil", "not", "object", "of", "on",
  "operator", "or", "otherwise", "out", "overload", "override", "packed",
  "pascal", "platform", "private", "procedure", "program", "property",
  "protected", "public", "published", "raise", "read", "readonly", "record",
  "reference", "register", "reintroduce", "remove", "repeat", "requires",
  "resourcestring", "result", "safecall", "sealed", "self", "set", "shl",
  "shr", "specialize", "static", "stdcall", "strict", "string", "then",
  "threadvar", "to", "true", "try", "type", "unit", "unsafe", "until", "uses",
  "var", "virtual", "while", "with", "write", "writeonly", "xor",
};

static const char *const JAVA_KEYWORDS[] = {
  "abstract", "assert", "boolean", "break", "byte", "case", "catch", "char",
  "class", "const", "continue", "default", "do", "double", "else", "enum",
  "extends", "final", "finally", "float", "for", "goto", "if", "implements",
  "import", "instanceof", "int", "interface", "long", "native", "new",
  "package", "private", "protected", "public", "return", "short", "static",
  "strictfp", "super", "switch", "synchronized", "this", "throw", "throws",
  "transient", "try", "void", "volatile", "while", "record", "var", "yield",
  "sealed", "permits", "when",
};

static const char *const JS_KEYWORDS[] = {
  "break", "case", "catch", "class", "const", "continue", "debugger",
  "default", "delete", "do", "else", "export", "extends", "finally", "for",
  "function", "if", "import", "in", "instanceof", "let", "new", "return",
  "super", "switch", "this", "throw", "try", "typeof", "var", "void", "while",
  "with", "yield", "async", "await", "of",
};

static const char *const C_SEMI[] = { "struct", "union", "enum" };

static const Profile PROFILES[] = {
  { LANG_C, C_KEYWORDS, sizeof C_KEYWORDS / sizeof C_KEYWORDS[0], C_SEMI, 3, true, false, false },
  { LANG_JAVA, JAVA_KEYWORDS, sizeof JAVA_KEYWORDS / sizeof JAVA_KEYWORDS[0], NULL, 0, false, false, false },
  { LANG_JAVASCRIPT, JS_KEYWORDS, sizeof JS_KEYWORDS / sizeof JS_KEYWORDS[0], NULL, 0, false, true, true },
  { LANG_PASCAL, PASCAL_KEYWORDS, sizeof PASCAL_KEYWORDS / sizeof PASCAL_KEYWORDS[0], NULL, 0, false, false, false },
};

const Profile *profile_get(LangId lang) {
  if (lang > LANG_PASCAL) {
    return &PROFILES[0];
  }
  return &PROFILES[lang];
}

static bool list_has(const char *const *words, size_t n, const char *word, size_t len) {
  size_t i;
  if (word == NULL) {
    return false;
  }
  for (i = 0; i < n; i++) {
    if (rt_eq_cstr(word, len, words[i])) {
      return true;
    }
  }
  return false;
}

bool profile_keyword(const Profile *profile, const char *word, size_t len) {
  if (profile == NULL) {
    return false;
  }
  return list_has(profile->keywords, profile->nkeywords, word, len);
}

bool profile_semi_close(const Profile *profile, const char *word, size_t len) {
  if (profile == NULL || profile->semi_close == NULL) {
    return false;
  }
  return list_has(profile->semi_close, profile->nsemi_close, word, len);
}

bool lang_parse(const char *text, LangId *out) {
  if (text == NULL || out == NULL) {
    return false;
  }
  if (strcmp(text, "c") == 0) {
    *out = LANG_C;
    return true;
  }
  if (strcmp(text, "java") == 0) {
    *out = LANG_JAVA;
    return true;
  }
  if (strcmp(text, "javascript") == 0) {
    *out = LANG_JAVASCRIPT;
    return true;
  }
  if (strcmp(text, "pascal") == 0) {
    *out = LANG_PASCAL;
    return true;
  }
  return false;
}
