import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";

const C_KEYWORDS = [
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
];

const JAVA_KEYWORDS = [
  "abstract", "assert", "boolean", "break", "byte", "case", "catch", "char",
  "class", "const", "continue", "default", "do", "double", "else", "enum",
  "extends", "final", "finally", "float", "for", "goto", "if", "implements",
  "import", "instanceof", "int", "interface", "long", "native", "new",
  "package", "private", "protected", "public", "return", "short", "static",
  "strictfp", "super", "switch", "synchronized", "this", "throw", "throws",
  "transient", "try", "void", "volatile", "while", "record", "var", "yield",
  "sealed", "permits", "when",
];

const JS_KEYWORDS = [
  "break", "case", "catch", "class", "const", "continue", "debugger",
  "default", "delete", "do", "else", "export", "extends", "finally", "for",
  "function", "if", "import", "in", "instanceof", "let", "new", "return",
  "super", "switch", "this", "throw", "try", "typeof", "var", "void", "while",
  "with", "yield", "async", "await", "of", "null", "undefined", "true", "false",
];

const PASCAL_KEYWORDS = [
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
];

const ZIG_KEYWORDS = [
  "addrspace", "align", "allowzero", "and", "anyframe", "anytype", "asm",
  "break", "callconv", "catch", "comptime", "const", "continue", "defer",
  "else", "enum", "errdefer", "error", "export", "extern", "fn", "for", "if",
  "inline", "linksection", "noalias", "noinline", "nosuspend", "opaque", "or",
  "orelse", "packed", "pub", "resume", "return", "struct", "suspend", "switch",
  "test", "threadlocal", "try", "union", "unreachable", "usingnamespace", "var", "volatile", "while",
];

const KOTLIN_KEYWORDS = [
  "abstract", "actual", "annotation", "as", "break", "by", "catch", "class",
  "companion", "const", "constructor", "continue", "crossinline", "data", "do",
  "else", "enum", "expect", "external", "false", "finally", "for", "fun", "get",
  "if", "import", "in", "infix", "init", "inline", "inner", "interface",
  "internal", "is", "lateinit", "noinline", "null", "object", "open", "operator",
  "out", "override", "package", "private", "protected", "public", "reified",
  "return", "sealed", "set", "super", "suspend", "tailrec", "this", "throw",
  "true", "try", "typealias", "val", "value", "var", "vararg", "when", "where",
  "while",
];

const OPS = [
  "<<=", ">>=", "?:", "::", "=>", "->", "==", "!=", "<=", ">=", "&&", "||",
  "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>",
  "?.", "??", ":=", "..",
];

const LANGS = [
  { name: "offsidec", label: "C", scope: "source.offside.c", exts: ["sc"], keywords: C_KEYWORDS, kind: "c" },
  { name: "offsidejava", label: "Java", scope: "source.offside.java", exts: ["sjava"], keywords: JAVA_KEYWORDS, kind: "c" },
  { name: "offsidejavascript", label: "JavaScript", scope: "source.offside.javascript", exts: ["sjs"], keywords: JS_KEYWORDS, kind: "js" },
  { name: "offsidepascal", label: "Pascal", scope: "source.offside.pascal", exts: ["spas"], keywords: PASCAL_KEYWORDS, kind: "pascal" },
  { name: "offsidezig", label: "Zig", scope: "source.offside.zig", exts: ["szig"], keywords: ZIG_KEYWORDS, kind: "zig" },
  { name: "offsidekotlin", label: "Kotlin", scope: "source.offside.kotlin", exts: ["skt"], keywords: KOTLIN_KEYWORDS, kind: "kotlin" },
];

function keywordRule(lang) {
  if (lang.kind !== "pascal") {
    return `choice(${lang.keywords.map((word) => JSON.stringify(word)).join(", ")})`;
  }
  const alts = [...lang.keywords].sort((a, b) => b.length - a.length).map((word) =>
    word.replace(/[A-Za-z]/g, (ch) => `[${ch.toUpperCase()}${ch.toLowerCase()}]`),
  );
  return `token(/${alts.join("|")}/)`;
}

function lexical(kind) {
  const string = String.raw`string: $ => token(seq('"', repeat(choice(/[^"\\\n]/, /\\./)), '"'))`;
  const character = String.raw`character: $ => token(seq("'", repeat(choice(/[^'\\\n]/, /\\./)), "'"))`;
  if (kind === "pascal") {
    return `
    string: $ => token(seq("'", repeat(choice(/[^'\\n]/, "''")), "'")),
    directive: $ => token(choice(
      seq('{$', /[^}]*/, '}'),
      seq('(*$', repeat(choice(/[^*]/, /\\*[^)]/)), '*)'),
    )),
    comment: $ => token(choice(
      seq('//', /[^\\n]*/),
      seq('{', /[^$}][^}]*/, '}'),
      seq('(*', repeat(choice(/[^*]/, /\\*[^)]/)), '*)'),
    )),
    number: $ => token(choice(/\\$[0-9A-Fa-f]+/, /&[0-7]+/, /%[01]+/, /\\d+/)),
    builtin: $ => token(/@[A-Za-z_][A-Za-z0-9_]*/),`;
  }
  if (kind === "zig") {
    return `
    ${string},
    line_string: $ => token(seq('\\\\\\\\', /[^\\n]*/)),
    ${character},
    comment: $ => token(seq('//', /[^\\n]*/)),
    number: $ => token(choice(/0x[0-9A-Fa-f]+/, /0b[01]+/, /\\d+(\\.\\d+)?/)),
    builtin: $ => token(/@[A-Za-z_][A-Za-z0-9_]*/),`;
  }
  if (kind === "js") {
    return `
    template: $ => token(seq('\`', repeat(choice(/[^\\\`\\\\]/, /\\\\./)), '\`')),
    ${string},
    ${character},
    comment: $ => token(choice(
      seq('//', /[^\\n]*/),
      seq('/*', /[^*]*\\*+([^/*][^*]*\\*+)*/, '/'),
    )),
    number: $ => token(choice(/0x[0-9A-Fa-f]+/, /\\d+(\\.\\d+)?([eE][+-]?\\d+)?/)),
    builtin: $ => token(/@[A-Za-z_][A-Za-z0-9_]*/),`;
  }
  if (kind === "kotlin") {
    return `
    string: $ => token(choice(
      seq('"""', repeat(choice(/[^"]/, /"[^"]/, /""[^"]/)), '"""'),
      seq('"', repeat(choice(/[^"\\\\\\n]/, /\\\\./)), '"'),
    )),
    ${character},
    comment: $ => token(choice(
      seq('//', /[^\\n]*/),
      seq('/*', /[^*]*\\*+([^/*][^*]*\\*+)*/, '/'),
    )),
    number: $ => token(choice(/0x[0-9A-Fa-f]+/, /\\d+(\\.\\d+)?([eE][+-]?\\d+)?/)),
    builtin: $ => token(/@[A-Za-z_][A-Za-z0-9_]*/),`;
  }
  return `
    ${string},
    ${character},
    preproc: $ => token(seq('#', /[^\\n]*/)),
    comment: $ => token(choice(
      seq('//', /[^\\n]*/),
      seq('/*', /[^*]*\\*+([^/*][^*]*\\*+)*/, '/'),
    )),
    number: $ => token(choice(/0x[0-9A-Fa-f]+/, /\\d+(\\.\\d+)?([eE][+-]?\\d+)?/)),
    builtin: $ => token(/@[A-Za-z_][A-Za-z0-9_]*/),`;
}

function grammar(lang) {
  const word = lang.kind === "pascal" ? "" : "word: $ => $.identifier,";
  const extraAtoms = lang.kind === "pascal"
    ? "$.directive,"
    : lang.kind === "c"
      ? "$.preproc,"
      : lang.kind === "zig"
        ? "$.line_string,"
        : lang.kind === "js"
          ? "$.template,"
          : "";
  const ops = OPS.map((op) => JSON.stringify(op)).join(", ");
  return `// Generated by build-grammars.mjs. Highlighting grammar, not the compiler.
module.exports = grammar({
  name: ${JSON.stringify(lang.name)},
  extras: $ => [/[ \\t\\f\\v]/, $.comment],
  externals: $ => [$.block_colon, $._newline, $._dedent],
  ${word}
  rules: {
    source_file: $ => repeat($._statement),

    _statement: $ => choice(
      seq(repeat1($._atom), $.block),
      seq(repeat1($._atom), $._newline),
      $._newline,
    ),

    block: $ => seq(
      $.block_colon,
      repeat($._statement),
      $._dedent,
    ),

    _atom: $ => choice(
      $.keyword,
      $.identifier,
      $.number,
      $.string,
      ${lang.kind === "zig" || lang.kind === "c" || lang.kind === "js" || lang.kind === "kotlin" ? "$.character," : ""}
      ${extraAtoms}
      $.builtin,
      $.operator,
      $.punctuation,
    ),

    keyword: $ => ${keywordRule(lang)},
    identifier: $ => /[A-Za-z_$][A-Za-z0-9_$]*/,
    ${lexical(lang.kind)}
    operator: $ => token(choice(${ops})),
    punctuation: $ => token(choice("(", ")", "[", "]", "{", "}", ",", ".", ";", ":", "?", "@", "#", "!", "~", "+", "-", "*", "/", "%", "=", "<", ">", "&", "|", "^")),
  },
});
`;
}

function highlights(kind) {
  const lines = [
    "(comment) @comment",
    "(string) @string",
    "(number) @number",
    "(keyword) @keyword",
    "(builtin) @function.builtin",
    "(block_colon) @punctuation.delimiter",
    "(operator) @operator",
    "(punctuation) @punctuation",
    "(identifier) @variable",
  ];
  if (kind === "c" || kind === "js" || kind === "kotlin" || kind === "zig") lines.push("(character) @character");
  if (kind === "js") lines.push("(template) @string");
  if (kind === "zig") lines.push("(line_string) @string");
  if (kind === "pascal") lines.push("(directive) @keyword.directive");
  if (kind === "c") lines.push("(preproc) @keyword.directive");
  return lines.join("\n") + "\n";
}

const FOLDS = `(block) @fold
`;

const INDENTS = `[
  (block)
] @indent.begin
`;

const root = "/tmp/offside-git/editors/tree-sitter";

function write(path, text) {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, text);
}

function scannerSource(lang) {
  const block = lang.kind === "zig" || lang.kind === "pascal" ? 0 : 1;
  const pascal = lang.kind === "pascal" ? 1 : 0;
  return `#define TS_HAS_BLOCK_COMMENT ${block}
#define TS_HAS_PASCAL_COMMENT ${pascal}
#define TS_SCANNER_CREATE tree_sitter_${lang.name}_external_scanner_create
#define TS_SCANNER_DESTROY tree_sitter_${lang.name}_external_scanner_destroy
#define TS_SCANNER_SERIALIZE tree_sitter_${lang.name}_external_scanner_serialize
#define TS_SCANNER_DESERIALIZE tree_sitter_${lang.name}_external_scanner_deserialize
#define TS_SCANNER_SCAN tree_sitter_${lang.name}_external_scanner_scan
#include "../../common/scanner.c"
`;
}

for (const lang of LANGS) {
  const dir = join(root, lang.name);
  write(join(dir, "grammar.js"), grammar(lang));
  write(join(dir, "src/scanner.c"), scannerSource(lang));
  write(join(dir, "queries/highlights.scm"), highlights(lang.kind));
  write(join(dir, "queries/folds.scm"), FOLDS);
  write(join(dir, "queries/indents.scm"), INDENTS);
  write(join(dir, "package.json"), JSON.stringify({
    name: `tree-sitter-${lang.name}`,
    version: "0.1.0-alpha.1",
    description: `Tree-sitter highlighter for the offside dialect of ${lang.label}. Not the compiler.`,
    main: "bindings/node",
    license: "MIT",
    treeSitter: [{
      scope: lang.scope,
      fileTypes: lang.exts,
      highlights: ["queries/highlights.scm"],
    }],
  }, null, 2) + "\n");
  write(join(dir, "tree-sitter.json"), JSON.stringify({
    grammars: [{
      name: lang.name,
      camelcase: lang.name,
      scope: lang.scope,
      path: ".",
      fileTypes: lang.exts,
      highlights: "queries/highlights.scm",
    }],
    metadata: {
      version: "0.1.0-alpha.1",
      license: "MIT",
      description: `Highlighter for the offside dialect of ${lang.label}.`,
      links: { repository: "https://github.com/tpqwz/offside" },
    },
  }, null, 2) + "\n");
}

console.log(`wrote ${LANGS.length} grammars`);
