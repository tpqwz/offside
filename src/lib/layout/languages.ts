import type { LangId, Profile } from "./types.ts";

const C_KEYWORDS = [
  "auto", "break", "case", "char", "const", "continue", "default", "do",
  "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
  "int", "long", "register", "restrict", "return", "short", "signed", "sizeof",
  "static", "struct", "switch", "typedef", "union", "unsigned", "void",
  "volatile", "while", "_Bool", "_Complex", "_Imaginary",
  // C11
  "_Alignas", "_Alignof", "_Atomic", "_Generic", "_Noreturn", "_Static_assert", "_Thread_local",
  // C23 keywords, plus the header spellings compilers accept as macros or keywords
  "alignas", "alignof", "bool", "constexpr", "false", "nullptr", "static_assert",
  "thread_local", "true", "typeof", "typeof_unqual", "_BitInt",
  // GNU / MSVC spellings that show up in real headers
  "asm", "__asm__", "__attribute__", "__attribute", "__declspec", "__typeof__",
  "__typeof_unqual__", "__auto_type", "__volatile__", "__inline__", "__restrict__",
  "__extension__", "__label__", "__thread", "__alignof__", "__alignof", "__complex__",
  "__real__", "__imag__", "__int128", "__signed__", "__unsigned__",
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
  "with", "yield", "async", "await", "of",
];

const C_PROGRAM = `#include <stdio.h>

struct Point:
    int x
    int y

int dist2(struct Point p):
    return p.x * p.x + p.y * p.y

int main(void):
    struct Point p = {1, 2}
    int n = dist2(p)

    if (n > 0):
        printf("n = %d\\n", n)
    else:
        printf("zero\\n")

    for (int i = 0; i < n && i < 3; i++):
        printf("i=%d\\n", i)

    switch (n):
        case 5:
            puts("five")
            break
        default:
            puts("other")

    int flag = n > 1 ? 1 : 0
    printf("flag %d\\n", flag)
    return 0
`;

const C_EDGES = `#include <stdio.h>

int main(void):
    printf("a:b %s\\n",
        "still one call")

    char c = ':'
    int t = 1 > 0 ?
        2 :
        3

    error:
        /* label, not a block */
        return c == ':' ? t : 0
`;

const JAVA_PROGRAM = `class Greeter:
    String hello(String name):
        if (name == null || name.isEmpty()):
            return "hello"
        return "hello, " + name

    public static void main(String[] args):
        Greeter g = new Greeter()
        for (String s : args):
            System.out.println(g.hello(s))
        if (args.length == 0):
            System.out.println(g.hello("world"))
`;

const JAVA_EDGES = `record Pair(int a, int b):
    int sum():
        return a + b

class App:
    static int pick(boolean flag):
        return flag ?
            1 :
            0
`;

const JS_PROGRAM = `function greet(name):
    if (!name):
        return "hello"
    return \`hello, \${name}\`

const summarize = (items) =>:
    let n = 0
    for (const item of items):
        n += item.length
    return n

console.log(greet("ada"))
console.log(summarize(["layout", "tokens"]))
`;

const JS_EDGES = `function poem():
    const text = \`line one
line two
line \${1 + 1}\`
    return text

const bump = (x) =>:
    return x + 1
`;

const PASCAL_PROGRAM = `{$mode objfpc}{$H+}

program Demo:

    function Sign(N: Integer): Integer:
        if N < 0 then:
            Sign := -1
        else:
            Sign := 1

    if Sign(2) <> 1 then Halt(1)
    WriteLn('ok')
`;

const PASCAL_UNIT = `{$mode objfpc}{$H+}

unit Boxes

interface

type
    TBox = class:
        public
            N: Integer
            procedure Bump

implementation

procedure TBox.Bump:
    N := N + 1

end.
`;

function profile(
  id: LangId,
  label: string,
  filename: string,
  keywords: string[],
  flags: { preprocessor: boolean; templates: boolean; regex: boolean; semiClose: string[] },
  policy: Profile["policy"],
  examples: Profile["examples"],
): Profile {
  return {
    id,
    label,
    filename,
    keywords: new Set(keywords),
    preprocessor: flags.preprocessor,
    templates: flags.templates,
    regex: flags.regex,
    semiClose: new Set(flags.semiClose),
    policy,
    examples,
  };
}

export const PROFILES: Record<LangId, Profile> = {
  c: profile(
    "c",
    "C",
    "out.c",
    C_KEYWORDS,
    { preprocessor: true, templates: false, regex: false, semiClose: ["struct", "union", "enum"] },
    {
      cue: "Header colon after ), else, do, struct, union, or enum. A colon on the next line works too. C11, C17, C23, and GNU attributes, _Generic, and asm stay intact",
      terminator: "Semicolon on a finished line. Open parens, operators, attributes, and split types stay open. Enums and { } initializers get commas",
      semiClose: "struct, union, enum close with }; a typedef name or __attribute__ can follow",
      transparent: "Preprocessor lines, including #elifdef and #embed, pass through",
    },
    [
      { id: "program", label: "Program", source: C_PROGRAM },
      { id: "edges", label: "Edges", source: C_EDGES },
    ],
  ),
  java: profile(
    "java",
    "Java",
    "Out.java",
    JAVA_KEYWORDS,
    { preprocessor: false, templates: false, regex: false, semiClose: [] },
    {
      cue: "Same colon rule. for (T x : xs) keeps its colon",
      terminator: "Semicolon at paren depth zero",
      semiClose: "class, interface, and record close with }",
      transparent: "No preprocessor",
    },
    [
      { id: "program", label: "Program", source: JAVA_PROGRAM },
      { id: "edges", label: "Edges", source: JAVA_EDGES },
    ],
  ),
  javascript: profile(
    "javascript",
    "JavaScript",
    "out.js",
    JS_KEYWORDS,
    { preprocessor: false, templates: true, regex: true, semiClose: [] },
    {
      cue: "Trailing colon. Arrow blocks are written =>:",
      terminator: "Semicolon inserted, so ASI is not in charge",
      semiClose: "Blocks close with }",
      transparent: "No preprocessor",
    },
    [
      { id: "program", label: "Program", source: JS_PROGRAM },
      { id: "edges", label: "Edges", source: JS_EDGES },
    ],
  ),
  pascal: profile(
    "pascal",
    "Pascal",
    "out.pas",
    PASCAL_KEYWORDS,
    { preprocessor: false, templates: false, regex: false, semiClose: [] },
    {
      cue: "A trailing colon replaces begin/end. Routines, if/then, for/do, repeat, try/except or try/finally, case/of, record, and class use it. Nest a second try when you need both except and finally. Lazarus modes objfpc, delphi, delphiunicode, fpc, tp, macpas, iso, and extendedpascal share this profile",
      terminator: "Semicolon after a statement or declaration. var/type/const headings and except/finally stay bare",
      semiClose: "Blocks close with end; a program or library closes with end.",
      transparent: "{$...} and (*$...*) directives pass through",
    },
    [
      { id: "program", label: "Program", source: PASCAL_PROGRAM },
      { id: "unit", label: "Unit", source: PASCAL_UNIT },
    ],
  ),
};

export const LANGS: LangId[] = ["c", "java", "javascript", "pascal"];
