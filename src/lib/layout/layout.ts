import { lex } from "./lex.ts";
import { MODIFIER } from "./pascal.ts";
import { PROFILES } from "./languages.ts";
import type { LangId, LayoutEvent, LayoutNote, LayoutResult, OutLine, OutSegment, Profile, Tok } from "./types.ts";

const HANGING = new Set([
  "+", "-", "*", "/", "%", "=", "<", ">", "!", "&", "|", "^", "~", "?", ":", ",", ".", "\\",
  "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
  "<<", ">>", ">>>", "->", "=>", "?.", "??", "::", "<<=", ">>=", ">>>=",
]);

const CONT_START = new Set([
  "+", "-", "/", "%", "?", "<", ">", "=",
  "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", ">>>",
  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>=",
  "->", ".", "|", "^",
]);

const TYPE_PREFIX = new Set([
  "auto", "char", "const", "double", "enum", "extern", "float", "inline",
  "int", "long", "register", "restrict", "short", "signed", "static", "struct",
  "typedef", "union", "unsigned", "void", "volatile", "_Bool", "_Complex", "_Imaginary",
  "bool", "constexpr", "thread_local", "_Thread_local", "__thread",
  "typeof", "typeof_unqual", "__typeof__", "__typeof_unqual__", "__auto_type",
  "_Atomic", "_BitInt", "__int128", "__complex__", "__signed__", "__unsigned__",
  "alignas", "_Alignas", "_Noreturn", "noreturn", "__inline__", "__restrict__",
]);

const SPEC_CALL = new Set([
  "typeof", "typeof_unqual", "__typeof__", "__typeof_unqual__",
  "_Atomic", "_BitInt", "alignas", "_Alignas", "alignof", "_Alignof",
  "__alignof__", "__alignof",
]);

const ATTR_NAME = new Set(["__attribute__", "__attribute", "__declspec"]);

function skipGroup(tokens: Tok[], index: number, open: string, close: string): number {
  if (tokens[index]?.text !== open) return index;
  let depth = 0;
  for (let i = index; i < tokens.length; i++) {
    const text = tokens[i]?.text;
    if (text === open) depth++;
    else if (text === close) {
      depth--;
      if (depth === 0) return i + 1;
    }
  }
  return tokens.length;
}

function stripAttributeTokens(tokens: Tok[]): Tok[] {
  const out: Tok[] = [];
  let i = 0;
  while (i < tokens.length) {
    const tok = tokens[i];
    if (!tok) break;
    if (ATTR_NAME.has(tok.text) || SPEC_CALL.has(tok.text)) {
      const next = skipGroup(tokens, i + 1, "(", ")");
      i = next === i + 1 ? i + 1 : next;
      continue;
    }
    if (tok.text === "[" && tokens[i + 1]?.text === "[") {
      i = skipGroup(tokens, i, "[", "]");
      continue;
    }
    out.push(tok);
    i++;
  }
  return out;
}

function isAttributeLine(tokens: Tok[]): boolean {
  let i = 0;
  let saw = false;
  while (i < tokens.length) {
    const tok = tokens[i];
    if (!tok) break;
    if (tok.text === "[") {
      const next = skipGroup(tokens, i, "[", "]");
      if (next === i) return false;
      i = next;
      saw = true;
      continue;
    }
    if (ATTR_NAME.has(tok.text)) {
      const next = skipGroup(tokens, i + 1, "(", ")");
      if (next === i + 1) return false;
      i = next;
      saw = true;
      continue;
    }
    if (tok.text === "alignas" || tok.text === "_Alignas") {
      const next = skipGroup(tokens, i + 1, "(", ")");
      if (next === i + 1) return false;
      i = next;
      saw = true;
      continue;
    }
    return false;
  }
  return saw;
}

function attributeContinues(code: Tok[], ahead: Tok[]): boolean {
  if (!isAttributeLine(code) || ahead.length === 0) return false;
  const head = ahead[0]?.text ?? "";
  return !["case", "default", "if", "for", "while", "switch", "return", "goto", "break", "continue", "do", "else"].includes(head);
}

/** A following attribute belongs to this declaration, unless it is a statement attribute such as [[fallthrough]]. */
function attributeSuffix(ahead: Tok[], after: Tok[]): boolean {
  if (ahead.length === 0) return false;
  const body = ahead.at(-1)?.text === ":" ? ahead.slice(0, -1) : ahead;
  if (!isAttributeLine(body)) return false;
  if (ahead.at(-1)?.text === ":") return true;
  return attributeContinues(body, after);
}

function specifierContinues(code: Tok[], ahead: Tok[]): boolean {
  const name = code[0]?.text ?? "";
  if (!SPEC_CALL.has(name) || code.at(-1)?.text !== ")") return false;
  const first = ahead[0];
  return !!first && (first.kind === "ident" || first.kind === "keyword" || first.text === "*" || first.text === "[");
}

const COLON_HEADER = new Set([
  "else", "do", "struct", "union", "enum", "try", "finally", "class", "interface", "record",
]);

interface LogicalLine {
  no: number;
  lead: string;
  gaps: string[];
  tokens: Tok[];
  trailing: string;
}

interface Frame {
  headerIndent: number;
  bodyIndent: number;
  semiOnClose: boolean;
  name: string;
  kind: "enum" | "struct" | "union" | "block";
  wantsTail: boolean;
  line: number;
  opener: "{" | "begin" | "none";
  closer: "}" | "end" | "until" | "none";
  delayedBegin: boolean;
  bodyStarted: boolean;
  dot: boolean;
  asm: boolean;
  keepOpen: boolean;
  /** Set when a Pascal try has already taken except or finally. */
  arm: string;
  /** Unclosed `{$if...}` emitted before a delayed `begin`. */
  condDepth: number;
  /** Unclosed `{$if...}` emitted inside this frame's body. Their `{$endif}` stays before `end`. */
  bodyCond: number;
  /** Body begin/end live inside `{$IF}` branches, so the routine must not grow its own. */
  skipShell: boolean;
}

function groupLines(source: string, tokens: Tok[]): LogicalLine[] {
  const lines: LogicalLine[] = [];
  let pos = 0;
  let lineNo = 1;
  let current: LogicalLine | null = null;

  function flush(): void {
    if (current) lines.push(current);
    current = null;
  }

  function applyGap(gap: string, tok: Tok | null): void {
    const parts = gap.split("\n");
    if (parts.length === 1) {
      if (tok) {
        if (!current) current = { no: lineNo, lead: "", gaps: [], tokens: [], trailing: "" };
        if (current.tokens.length === 0) {
          current.lead += parts[0];
          current.gaps.push("");
        } else {
          current.gaps.push(parts[0]);
        }
        current.tokens.push(tok);
      } else if (current) {
        current.trailing += parts[0];
      } else if (parts[0].length) {
        current = { no: lineNo, lead: parts[0], gaps: [], tokens: [], trailing: "" };
      }
      return;
    }

    if (current) {
      current.trailing += parts[0];
      flush();
    } else {
      lines.push({ no: lineNo, lead: parts[0], gaps: [], tokens: [], trailing: "" });
    }
    for (let p = 1; p < parts.length - 1; p++) {
      lineNo++;
      lines.push({ no: lineNo, lead: parts[p], gaps: [], tokens: [], trailing: "" });
    }
    lineNo++;
    const lead = parts[parts.length - 1] ?? "";
    if (tok) {
      current = { no: lineNo, lead, gaps: [""], tokens: [tok], trailing: "" };
    } else if (lead.length > 0) {
      current = { no: lineNo, lead, gaps: [], tokens: [], trailing: "" };
    } else {
      current = null;
    }
  }

  for (const tok of tokens) {
    applyGap(source.slice(pos, tok.start), tok);
    pos = tok.end;
  }
  applyGap(source.slice(pos), null);
  flush();
  return lines;
}

function codeTokens(line: LogicalLine): Tok[] {
  return line.tokens.filter((t) => t.kind !== "comment");
}

function lineKind(line: LogicalLine): "blank" | "comment" | "directive" | "code" {
  if (line.tokens.length === 0) return "blank";
  const code = codeTokens(line);
  if (code.length === 0) return "comment";
  if (code[0]?.kind === "directive") return "directive";
  return "code";
}

function condKind(text: string): "open" | "close" | "mid" | "other" {
  const body = text.trim().toLowerCase();
  if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(body)) return "open";
  if (/^\{\$\s*(endif|ifend)\b/.test(body)) return "close";
  if (/^\{\$\s*(else|elseif)\b/.test(body)) return "mid";
  if (/^#\s*(if|ifdef|ifndef)\b/.test(body)) return "open";
  if (/^#\s*(endif)\b/.test(body)) return "close";
  if (/^#\s*(else|elifdef|elifndef|elif)\b/.test(body)) return "mid";
  return "other";
}

function indentOf(line: LogicalLine): number {
  const code = codeTokens(line);
  return code[0]?.col ?? 1;
}

function headerName(tokens: Tok[]): string {
  const paren = tokens.findIndex((t) => t.text === "(");
  if (paren > 0) {
    const prev = tokens[paren - 1];
    if (prev && (prev.kind === "ident" || prev.kind === "keyword")) return `${prev.text}()`;
  }
  const skip = new Set([
    "int", "void", "char", "long", "short", "float", "double", "signed", "unsigned",
    "const", "static", "extern", "volatile", "inline", "public", "private", "protected",
    "final", "abstract", "synchronized", "native", "strictfp", "var", "let",
  ]);
  const kw = tokens.find((t) => t.kind === "keyword" && !skip.has(t.text));
  return kw?.text ?? "block";
}

function semiOnClose(tokens: Tok[], profile: Profile): boolean {
  const stripped = stripAttributeTokens(tokens);
  if (stripped.some((t) => t.text === "(")) return false;
  return stripped.some((t) => t.kind === "keyword" && profile.semiClose.has(t.text));
}

function aggregateInfo(tokens: Tok[]): { kind: Frame["kind"]; wantsTail: boolean } {
  let kind: Frame["kind"] = "block";
  let kw = -1;
  let paren = 0;
  for (let i = 0; i < tokens.length; i++) {
    const text = tokens[i]?.text;
    if (text === "(") paren++;
    else if (text === ")" && paren > 0) paren--;
    else if (paren === 0 && (text === "enum" || text === "struct" || text === "union")) {
      kind = text;
      kw = i;
      break;
    }
  }
  if (kind === "block" || kw < 0) return { kind, wantsTail: false };
  let depth = 0;
  for (let i = kw + 1; i < tokens.length; i++) {
    const tok = tokens[i];
    if (!tok || tok.text === ":") break;
    if (tok.text === "(") {
      if (depth === 0) return { kind: "block", wantsTail: false };
      depth++;
    } else if (tok.text === ")" && depth > 0) depth--;
  }
  return { kind, wantsTail: true };
}

const HEADER_KEYWORDS = new Set([
  "else", "do", "struct", "union", "enum", "class", "interface", "record", "try", "finally",
]);

function opensBlock(code: Tok[], blockColon: boolean, labelColon: boolean): boolean {
  if (!blockColon) return false;
  const first = code[0];
  if (first && (first.text === "case" || first.text === "default")) return false;
  if (code.length === 2 && code[0]?.kind === "ident" && code[1]?.text === ":") return false;
  if (code.length === 1 && code[0]?.text === ":") return !labelColon;
  const prev = code[code.length - 2];
  if (prev && (prev.text === ")" || prev.text === "=>")) return true;
  return code.some((t) => HEADER_KEYWORDS.has(t.text));
}

const DECL_START = new Set([
  "if", "else", "for", "while", "do", "switch", "return", "goto", "break", "continue",
  "case", "default", "sizeof", "typedef", "struct", "union", "enum", "int", "void", "char",
  "long", "short", "float", "double", "signed", "unsigned", "const", "static", "extern",
  "volatile", "inline", "auto", "register", "restrict", "_Bool", "_Complex", "_Imaginary",
]);

function isDeclaratorTail(line: LogicalLine): boolean {
  const code = codeTokens(line);
  if (code.length === 0) return false;
  if (code.some((t) => t.text === "(" || t.text === ":" || t.text === "=" || t.text === "?" || t.text === "." || t.text === "->" || t.text === "{")) {
    return false;
  }
  const first = code[0];
  if (!first || DECL_START.has(first.text)) return false;
  let bracket = 0;
  for (const t of code) {
    if (t.text === "[") {
      bracket++;
      continue;
    }
    if (t.text === "]") {
      if (bracket === 0) return false;
      bracket--;
      continue;
    }
    if (t.kind === "ident" || t.text === "*" || t.text === "," || t.text === "const" || t.text === "volatile" || t.text === "restrict") continue;
    if (bracket > 0 && (t.kind === "number" || t.text === "+" || t.text === "-" || t.text === "/" || t.text === "%" || t.text === "~" || t.text === "&" || t.text === "|" || t.text === "^" || t.text === "<<" || t.text === ">>")) continue;
    return false;
  }
  return bracket === 0;
}

function isTypePrefix(code: Tok[]): boolean {
  let saw = false;
  let tag = false;
  for (const tok of code) {
    if (tok.text === "*") {
      tag = false;
      saw = true;
      continue;
    }
    if (tok.kind === "keyword" && TYPE_PREFIX.has(tok.text)) {
      tag = tok.text === "struct" || tok.text === "union" || tok.text === "enum";
      saw = true;
      continue;
    }
    if (tok.kind === "ident" && tag) {
      tag = false;
      continue;
    }
    return false;
  }
  return saw;
}

/** A parameter list, not a cast: `(...)` with nothing after it but an optional colon. */
function parameterListLine(tokens: Tok[]): boolean {
  if (tokens[0]?.text !== "(") return false;
  let depth = 0;
  for (let i = 0; i < tokens.length; i++) {
    const text = tokens[i]?.text;
    if (text === "(") depth++;
    else if (text === ")") {
      depth--;
      if (depth === 0) {
        const rest = tokens[i + 1]?.text;
        return rest === undefined || rest === ":";
      }
    }
  }
  return true;
}

function finishedAggregate(code: Tok[]): boolean {
  for (let i = 0; i < code.length; i++) {
    const text = code[i]?.text;
    if ((text === "struct" || text === "union" || text === "enum") && code[i + 1]?.kind === "ident" && i + 2 === code.length) {
      return true;
    }
  }
  return false;
}

function declarationContinues(code: Tok[], ahead: Tok[]): boolean {
  if (!isTypePrefix(code) || ahead.length === 0) return false;
  const first = ahead[0];
  if (!first) return false;
  if (first.kind === "ident" || first.text === "*" || first.text === "(") return true;
  if (finishedAggregate(code)) return false;
  return first.kind === "keyword" && TYPE_PREFIX.has(first.text);
}

function controlContinues(code: Tok[], ahead: Tok[], lineIndent: number): boolean {
  const head = code[0]?.text;
  if ((head !== "return" && head !== "goto") || code.length !== 1) return false;
  const first = ahead[0];
  return !!first && first.col > lineIndent;
}

type BraceKind = "init" | "stmt";

interface LayoutState {
  ternary: number;
  paren: number;
  bracket: number;
  braces: BraceKind[];
  braceParen: number[];
}

function braceKindAt(code: Tok[], index: number, hangText: string): BraceKind {
  const prev = code[index - 1];
  if (!prev) return hangText === "=" || hangText === "," ? "init" : "stmt";
  if (prev.text === "=" || prev.text === "," || prev.text === "{") return "init";
  if (prev.text === "(") return "stmt";
  if (prev.text !== ")") return "stmt";
  let depth = 0;
  for (let i = index - 1; i >= 0; i--) {
    const tok = code[i];
    if (!tok) continue;
    if (tok.text === ")") depth++;
    else if (tok.text === "(") {
      depth--;
      if (depth === 0) {
        const before = code[i - 1];
        if (!before || before.text === "=" || before.text === "," || before.text === "return" || before.text === "(") return "init";
        if (before.text === "]" || before.text === "struct" || before.text === "union" || before.text === "enum") return "init";
        if (before.kind === "keyword" && DECL_START.has(before.text) && !["if", "while", "for", "switch"].includes(before.text)) return "init";
        return "stmt";
      }
    }
  }
  return "stmt";
}

interface Classified {
  opens: boolean;
  hanging: boolean;
  ternary: number;
  paren: number;
  bracket: number;
  braces: BraceKind[];
  braceParen: number[];
  closedInit: boolean;
}

function atStatementLevel(braces: BraceKind[], braceParen: number[], paren: number, bracket: number): boolean {
  if (bracket !== 0) return false;
  for (let i = braces.length - 1; i >= 0; i--) {
    if (braces[i] === "init") return false;
    if (braces[i] === "stmt") return paren === (braceParen[i] ?? 0);
  }
  return paren === 0;
}

function classify(line: LogicalLine, state: LayoutState, hangText: string, labelColon: boolean): Classified {
  let { ternary, paren, bracket } = state;
  const braces = state.braces.slice();
  const braceParen = state.braceParen.slice();
  const code = codeTokens(line);
  let blockColon = false;
  let closedInit = false;
  for (let n = 0; n < code.length; n++) {
    const t = code[n];
    if (!t) continue;
    if (t.text === "(") paren++;
    else if (t.text === ")" && paren > 0) paren--;
    else if (t.text === "[") bracket++;
    else if (t.text === "]" && bracket > 0) bracket--;
    else if (t.text === "{") {
      let kind = braceKindAt(code, n, hangText);
      if (n === 0 && braces.at(-1) === "init") kind = "init";
      braces.push(kind);
      braceParen.push(paren);
    } else if (t.text === "}" && braces.length > 0) {
      const kind = braces.pop();
      braceParen.pop();
      if (kind === "init" && braces.at(-1) !== "init") closedInit = true;
    } else if (t.text === "?") ternary++;
    else if (t.text === ":") {
      const isLast = n === code.length - 1;
      if (ternary > 0) ternary--;
      else if (isLast && atStatementLevel(braces, braceParen, paren, bracket)) blockColon = true;
    }
  }
  const opens = opensBlock(code, blockColon, labelColon);
  const last = code[code.length - 1];
  const hanging = !opens && !!last && (HANGING.has(last.text) || ternary > 0);
  return { opens, hanging, ternary, paren, bracket, braces, braceParen, closedInit };
}

function effectiveParen(paren: number, braces: BraceKind[], braceParen: number[]): number {
  for (let i = braces.length - 1; i >= 0; i--) {
    if (braces[i] === "stmt") return Math.max(0, paren - (braceParen[i] ?? 0));
  }
  return paren;
}

function terminatorOf(
  line: LogicalLine,
  opened: boolean,
  hanging: boolean,
  paren: number,
  bracket: number,
  inEnum: boolean,
  insideInit: boolean,
  closedInit: boolean,
  joinString: boolean,
): ";" | "," | "" {
  if (opened || hanging || paren !== 0 || bracket !== 0 || joinString) return "";
  const code = codeTokens(line);
  if (code.length === 0) return "";
  const last = code[code.length - 1]?.text;
  if (!last || last === ";" || last === "{") return "";
  if (last === ":") return "";
  if (insideInit || inEnum) return last === "," ? "" : ",";
  if (last === "}" && code.every((t) => t.text === "}" || t.text === "{")) {
    return closedInit ? ";" : "";
  }
  return ";";
}

function renderLine(
  line: LogicalLine,
  colon: "keep" | "brace" | "drop" | "semi" | "begin" | "arm",
  terminator: string,
  skipLead = false,
): OutSegment[] {
  const segments: OutSegment[] = [];
  const push = (text: string, inserted: boolean) => {
    if (!text) return;
    const prev = segments[segments.length - 1];
    if (prev && prev.inserted === inserted) prev.text += text;
    else segments.push({ text, inserted });
  };
  if (!skipLead) push(line.lead, false);
  const code = line.tokens;
  let lastCode = -1;
  for (let n = 0; n < code.length; n++) {
    if (code[n]?.kind !== "comment") lastCode = n;
  }
  for (let n = 0; n < code.length; n++) {
    const tok = code[n];
    if (!tok) continue;
    push(line.gaps[n] ?? "", false);
    const colonHere = n === lastCode && tok.text === ":";
    if (colonHere && colon === "brace") {
      const prev = segments[segments.length - 1]?.text ?? "";
      if (prev.length > 0 && !/\s$/.test(prev)) push(" ", true);
      push("{", true);
    } else if (colonHere && colon === "begin") {
      push(" begin", true);
    } else if (colonHere && colon === "arm") {
      push(":", false);
      push(" begin", true);
    } else if (colonHere && colon === "semi") {
      push(";", true);
    } else if (colonHere && colon === "drop") {
      if (terminator) push(terminator, true);
    } else {
      push(tok.text, false);
      if (terminator && n === lastCode) push(terminator, true);
    }
  }
  push(line.trailing, false);
  return segments;
}

function explode(segments: OutSegment[], sourceLine: number): OutLine[] {
  const lines: OutLine[] = [];
  let lineNo = sourceLine;
  let current: OutLine = { segments: [], sourceLine: lineNo };
  const emit = (text: string, inserted: boolean) => {
    const parts = text.split("\n");
    for (let p = 0; p < parts.length; p++) {
      if (p > 0) {
        lines.push(current);
        lineNo++;
        current = { segments: [], sourceLine: lineNo };
      }
      const part = parts[p];
      if (!part) continue;
      const prev = current.segments[current.segments.length - 1];
      if (prev && prev.inserted === inserted) prev.text += part;
      else current.segments.push({ text: part, inserted });
    }
  };
  for (const seg of segments) emit(seg.text, seg.inserted);
  lines.push(current);
  return lines;
}

function textOf(line: OutLine): string {
  return line.segments.map((s) => s.text).join("");
}

/** Split a comma list at depth 0. Quotes stay intact so a string comma is not a break. */
function splitTopCommas(inner: string): string[] {
  const parts: string[] = [];
  let start = 0;
  let paren = 0;
  let bracket = 0;
  let quote: "'" | '"' | "" = "";
  for (let i = 0; i < inner.length; i++) {
    const ch = inner[i];
    if (quote) {
      if (ch === quote) {
        if (quote === "'" && inner[i + 1] === "'") i++;
        else quote = "";
      }
      continue;
    }
    if (ch === "'" || ch === '"') quote = ch;
    else if (ch === "(") paren++;
    else if (ch === ")" && paren > 0) paren--;
    else if (ch === "[") bracket++;
    else if (ch === "]" && bracket > 0) bracket--;
    else if (ch === "," && paren === 0 && bracket === 0) {
      parts.push(inner.slice(start, i));
      start = i + 1;
    }
  }
  parts.push(inner.slice(start));
  return parts;
}

/**
 * A 64k-element Pascal initializer on one line crashes the host compiler.
 * Break the parenthesized list into one element per line. The last element
 * has no trailing comma; Pascal constructors reject one.
 */
function wrapPascalInit(text: string): string {
  if (!text.includes(",")) return text;
  const open = text.indexOf("(");
  const close = text.lastIndexOf(")");
  if (open < 0 || close <= open + 1) return text;
  const parts = splitTopCommas(text.slice(open + 1, close)).map((part) => part.trim()).filter(Boolean);
  if (parts.length < 32) return text;
  const indent = text.match(/^\s*/)?.[0] ?? "";
  const head = text.slice(0, open + 1).trimEnd();
  const tail = text.slice(close).trim();
  const body = parts.map((part, i) => `${indent}    ${part}${i < parts.length - 1 ? "," : ""}`);
  return [head, ...body, `${indent}${tail}`].join("\n");
}

interface PascalStyle {
  open: boolean;
  colon: "keep" | "drop" | "semi" | "begin" | "arm";
  opener: "begin" | "none";
  closer: "end" | "until" | "none";
  delayed: boolean;
  dot: boolean;
  asm: boolean;
  keepOpen: boolean;
}

const PASCAL_BARE = new Set([
  "var", "type", "const", "label", "threadvar", "resourcestring",
  "public", "private", "protected", "published", "strict", "strict private",
  "strict protected", "required", "optional", "interface", "implementation",
  "initialization", "finalization", "contains", "requires", "begin", "asm",
  "repeat", "try", "except", "finally", "else", "otherwise",
]);

function foldText(token: Tok | undefined): string {
  return token?.text.toLowerCase() ?? "";
}

function pascalWords(code: Tok[]): string[] {
  return code.filter((tok) => tok.kind !== "directive" && tok.text !== ":").map((tok) => tok.text.toLowerCase());
}

/** The block colon is the header semicolon, unless that semicolon is already in front of a trailing directive. */
function routineColon(code: Tok[]): "semi" | "drop" {
  for (let i = code.length - 2; i >= 0; i--) {
    const tok = code[i];
    if (!tok || tok.kind === "directive") continue;
    return tok.text === ";" ? "drop" : "semi";
  }
  return "semi";
}

function pascalStyle(code: Tok[]): PascalStyle {
  const closed: PascalStyle = {
    open: false, colon: "keep", opener: "none", closer: "end",
    delayed: false, dot: false, asm: false, keepOpen: false,
  };
  if (code.at(-1)?.text !== ":") return closed;
  const words = pascalWords(code);
  const prev = words[words.length - 1] ?? "";
  const has = (word: string) => words.includes(word);
  // `object abstract` / `class sealed` — the modifier is not the type keyword.
  const typeTail = new Set(["abstract", "sealed"]);
  let typeAt = words.length - 1;
  while (typeAt > 0 && typeTail.has(words[typeAt] ?? "")) typeAt--;
  const typeWord = words[typeAt] ?? "";
  const routine = ["procedure", "function", "constructor", "destructor", "operator", "program", "library"].find((word) => has(word));
  if (routine === "program" || routine === "library") {
    return { open: true, colon: routineColon(code), opener: "begin", closer: "end", delayed: true, dot: true, asm: false, keepOpen: false };
  }
  if (routine) {
    return { open: true, colon: routineColon(code), opener: "begin", closer: "end", delayed: true, dot: false, asm: false, keepOpen: false };
  }
  if (prev === "repeat") {
    return { open: true, colon: "drop", opener: "none", closer: "until", delayed: false, dot: false, asm: false, keepOpen: false };
  }
  if (prev === "asm") {
    return { open: true, colon: "drop", opener: "none", closer: "end", delayed: false, dot: false, asm: true, keepOpen: false };
  }
  if (prev === "try") {
    return { open: true, colon: "drop", opener: "none", closer: "end", delayed: false, dot: false, asm: false, keepOpen: true };
  }
  if (prev === "of" && has("case")) {
    return { open: true, colon: "drop", opener: "none", closer: "end", delayed: false, dot: false, asm: false, keepOpen: false };
  }
  if (typeWord === "record" || typeWord === "object" || typeWord === "dispinterface" || typeWord === "objcclass" || typeWord === "objccategory" || typeWord === "objcprotocol" || (has("helper") && has("for"))) {
    return { open: true, colon: "drop", opener: "none", closer: "end", delayed: false, dot: false, asm: false, keepOpen: false };
  }
  const objc = has("objcclass") || has("objccategory") || has("objcprotocol");
  if (
    !["then", "do", "else", "otherwise"].includes(prev) &&
    (typeWord === "class" || typeWord === "interface" || prev === ")" || objc) &&
    (has("class") || has("object") || has("interface") || has("dispinterface") || objc) &&
    !has("procedure") && !has("function") && !has("constructor") && !has("destructor") && !has("operator")
  ) {
    return { open: true, colon: "drop", opener: "none", closer: "end", delayed: false, dot: false, asm: false, keepOpen: false };
  }
  if (words.length === 1 && words[0] === "begin") {
    return { open: true, colon: "drop", opener: "none", closer: "end", delayed: false, dot: false, asm: false, keepOpen: false };
  }
  if (prev === "then" || prev === "do" || prev === "else" || prev === "otherwise") {
    return { open: true, colon: "begin", opener: "begin", closer: "end", delayed: false, dot: false, asm: false, keepOpen: false };
  }
  if (words.length === 1 && (words[0] === "except" || words[0] === "finally")) {
    return { open: false, colon: "drop", opener: "none", closer: "none", delayed: false, dot: false, asm: false, keepOpen: false };
  }
  return closed;
}

function anglesBalance(code: Tok[]): boolean {
  let depth = 0;
  for (const tok of code) {
    if (tok.text === "<") depth++;
    else if (tok.text === ">") {
      if (depth === 0) return false;
      depth--;
    }
  }
  return depth === 0;
}

function depth0(code: Tok[], text: string): boolean {
  let paren = 0;
  let bracket = 0;
  for (const tok of code) {
    if (tok.text === "(") paren++;
    else if (tok.text === ")" && paren > 0) paren--;
    else if (tok.text === "[") bracket++;
    else if (tok.text === "]" && bracket > 0) bracket--;
    else if (paren === 0 && bracket === 0 && tok.text === text) return true;
  }
  return false;
}

function pascalDeclaration(code: Tok[]): boolean {
  const words = pascalWords(code);
  const head = words[0] ?? "";
  if ([
    "var", "type", "const", "label", "threadvar", "resourcestring", "exports",
    "property", "generic", "uses", "public", "private", "protected", "published", "strict",
  ].includes(head)) return true;
  if (["procedure", "function", "constructor", "destructor", "operator"].includes(head)) return true;
  if (words.length === 1 && MODIFIER.has(head)) return true;
  if (head === "class" && ["procedure", "function", "constructor", "destructor", "operator", "var", "property", "const"].includes(words[1] ?? "")) {
    return true;
  }
  if (depth0(code, ":=")) return false;
  let paren = 0;
  let bracket = 0;
  for (let i = 0; i < code.length; i++) {
    const tok = code[i];
    if (!tok) continue;
    if (tok.text === "(") paren++;
    else if (tok.text === ")" && paren > 0) paren--;
    else if (tok.text === "[") bracket++;
    else if (tok.text === "]" && bracket > 0) bracket--;
    else if (paren === 0 && bracket === 0 && tok.text === ":" && i < code.length - 1 && head !== "on") return true;
  }
  if (["if", "while", "for", "repeat", "try", "case", "with", "asm", "raise", "break", "continue", "exit", "goto", "on", "inherited", "else", "otherwise", "until", "begin", "end"].includes(head)) {
    return false;
  }
  if (depth0(code, "=")) return true;
  return false;
}

function pascalEndModifier(line: LogicalLine): string | null {
  const code = codeTokens(line);
  const word = code[0]?.text.toLowerCase() ?? "";
  if (!["deprecated", "platform", "experimental", "unimplemented"].includes(word)) return null;
  if (code.length === 1) return word;
  const msg = code[1];
  if (code.length === 2 && msg && (msg.kind === "string" || msg.kind === "char")) return `${word} ${msg.text}`;
  return null;
}

function pascalTrySection(line: LogicalLine): boolean {
  const words = codeTokens(line).filter((tok) => tok.text !== ":").map((tok) => tok.text.toLowerCase());
  return words.length === 1 && (words[0] === "except" || words[0] === "finally");
}

export function layoutSource(source: string, lang: LangId): LayoutResult {
  const profile = PROFILES[lang];
  const normalized = source.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
  const { tokens, notes } = lex(normalized, profile);
  const events: LayoutEvent[] = [];
  const logical = groupLines(normalized, tokens);
  const out: OutLine[] = [];

  const tabAt = normalized.indexOf("\t");
  if (tabAt >= 0) {
    notes.unshift({
      line: normalized.slice(0, tabAt).split("\n").length,
      message: "Tabs are preserved. Indent columns use stops of 8, so spaces are safer.",
    });
  }

  const stack: Frame[] = [];
  let pending: Frame | null = null;
  let state: LayoutState = { ternary: 0, paren: 0, bracket: 0, braces: [], braceParen: [] };
  const ppBrace: number[] = [];
  const deadStack: { if0: boolean; dead: boolean }[] = [];
  const inDead = (): boolean => deadStack.some((frame) => frame.dead);
  let hanging = false;
  let hangIndent = 1;
  let hangText = "";
  let expectWhile = false;
  const held: LogicalLine[] = [];

  function closeEmptyPending(frame: Frame, sourceLine: number, tail: string | null, attaches: boolean): void {
    const inner: LogicalLine[] = [];
    const outer: LogicalLine[] = [];
    for (const item of held) {
      const col = item.tokens[0]?.col ?? 1;
      if (col > frame.headerIndent) inner.push(item);
      else outer.push(item);
    }
    held.length = 0;
    if (inner.length > 0 && frame.delayedBegin && !frame.bodyStarted && frame.opener === "begin") {
      const beginPad = " ".repeat(Math.max(0, frame.headerIndent - 1));
      out.push({ segments: [{ text: `${beginPad}begin`, inserted: true }], sourceLine: frame.line });
      frame.bodyStarted = true;
      events.push({ line: frame.line, message: `Inserted begin for ${frame.name}` });
    }
    held.push(...inner);
    flushClosing(frame);
    held.push(...outer);
    if (tail) closeFrame(frame, sourceLine, tail);
    else closeStatement(frame, sourceLine, held, attaches);
    notes.push({ line: frame.line, message: `${frame.name} has no indented body.` });
    events.push({ line: sourceLine, message: `Closed empty ${frame.name}` });
  }

  function closeFrame(frame: Frame, sourceLine: number, tail: string | null, openDecl = false, withSemi = true): void {
    if (frame.closer === "until" || frame.closer === "none" || frame.skipShell) {
      frame.bodyStarted = true;
      return;
    }
    if (frame.delayedBegin && !frame.bodyStarted && frame.opener === "begin") {
      const beginPad = " ".repeat(Math.max(0, frame.headerIndent - 1));
      out.push({ segments: [{ text: `${beginPad}begin`, inserted: true }], sourceLine });
      frame.bodyStarted = true;
      events.push({ line: sourceLine, message: `Inserted begin for ${frame.name}` });
    }
    const pad = " ".repeat(Math.max(0, frame.headerIndent - 1));
    const brace = frame.closer === "end"
      ? (frame.dot ? "end." : tail ? `end ${tail};` : withSemi ? "end;" : "end")
      : tail ? (openDecl ? `} ${tail}` : `} ${tail};`) : frame.semiOnClose ? "};" : "}";
    out.push({ segments: [{ text: pad + brace, inserted: true }], sourceLine });
    events.push({
      line: sourceLine,
      message: tail ? `Closed ${frame.name} as } ${tail};` : `Dedent closed ${frame.name} with ${brace}`,
    });
    if (frame.name === "do") expectWhile = true;
  }

  /** Close-directives at this statement's indent belong before its semicolon (`end` / `{$ENDIF};`). */
  function takeTrailingCloses(bucket: LogicalLine[], minCol: number): LogicalLine[] {
    let lastClose = -1;
    const prelim: LogicalLine[] = [];
    for (const item of bucket) {
      const kind = lineKind(item);
      if (kind === "blank") {
        prelim.push(item);
        continue;
      }
      const col = item.tokens[0]?.col ?? 1;
      if (col < minCol) break;
      if (kind === "comment") {
        prelim.push(item);
        continue;
      }
      if (kind === "directive" && condKind(item.tokens.map((tok) => tok.text).join("")) === "close") {
        prelim.push(item);
        lastClose = prelim.length - 1;
        continue;
      }
      break;
    }
    if (lastClose < 0) return [];
    const taken = prelim.slice(0, lastClose + 1);
    bucket.splice(0, taken.length);
    return taken;
  }

  function closeStatement(frame: Frame, sourceLine: number, bucket: LogicalLine[], attaches: boolean): void {
    // Only a statement frame. A routine's `end;` has to stay inside the conditional
    // that wraps the whole routine (`function Pick; begin ... end; {$ENDIF}`).
    // A case-else's semicolon has to stay inside too: after `{$ENDIF}` it is a stray
    // arm when the else is compiled out (`{$ENDIF}; end`).
    const statement = /^(if|else|otherwise|while|for|with|on|repeat|case|try)$/i.test(frame.name);
    const inCase = stack.at(-1)?.name === "case";
    const canMove = statement && !inCase && profile.id === "pascal" && frame.closer === "end" && !frame.dot && !frame.skipShell;
    const closes = canMove ? takeTrailingCloses(bucket, frame.headerIndent) : [];
    // The first `bodyCond` endifs close ifdefs opened in this body (`stmt {$IFDEF} more {$ENDIF} end`).
    // Later ones close a conditional wrapped around the statement (`end {$ENDIF}; else`).
    let keep = 0;
    if (closes.length > 0 && frame.bodyCond > 0) {
      let remaining = frame.bodyCond;
      for (let i = 0; i < closes.length; i++) {
        const item = closes[i];
        if (!item) continue;
        if (lineKind(item) === "directive" && condKind(item.tokens.map((tok) => tok.text).join("")) === "close") {
          remaining--;
          keep = i + 1;
          if (remaining === 0) break;
        }
      }
      if (remaining > 0) keep = closes.length;
    }
    const stay = closes.slice(0, keep);
    const move = closes.slice(keep);
    for (const line of stay) emitLogical(line, "");
    if (attaches) {
      bucket.unshift(...move);
      closeFrame(frame, sourceLine, null, false, true);
      return;
    }
    closeFrame(frame, sourceLine, null, false, move.length === 0);
    if (move.length === 0) return;
    for (const line of move) emitLogical(line, "");
    const last = out.at(-1);
    if (last) last.segments.push({ text: ";", inserted: true });
  }

  function emitLogical(line: LogicalLine, terminator: string): void {
    out.push(...explode(renderLine(line, "keep", terminator), line.no));
  }

  function flushHeld(): void {
    for (const line of held) {
      emitLogical(line, "");
      if (lineKind(line) === "directive") {
        events.push({ line: line.no, message: "Preprocessor line passed through" });
        const top = stack.at(-1);
        const role = condKind(line.tokens.map((tok) => tok.text).join(""));
        if (top?.delayedBegin && !top.bodyStarted) {
          if (role === "open") top.condDepth++;
          else if (role === "close") top.condDepth = Math.max(0, top.condDepth - 1);
        }
        // An ifdef opened in the body has to be closed before `end`. An endif
        // that only finishes a conditional from outside the body does not.
        if (profile.id === "pascal" && top && indentOf(line) >= top.bodyIndent) {
          if (role === "open") top.bodyCond++;
          else if (role === "close") top.bodyCond = Math.max(0, top.bodyCond - 1);
        }
      }
    }
    held.length = 0;
  }

  /** Trivia flushed while closing a frame sits beside the inserted `end`, so an ifdef there wraps that `end` and must not swallow a later endif. An endif flushed here already closed a body ifdef. */
  function flushClosing(frame: Frame): void {
    let closed = 0;
    for (const line of held) {
      if (lineKind(line) !== "directive" || indentOf(line) < frame.bodyIndent) continue;
      if (condKind(line.tokens.map((tok) => tok.text).join("")) === "close") closed++;
    }
    const prior = frame.bodyCond;
    flushHeld();
    frame.bodyCond = Math.max(0, prior - closed);
  }

  function nextCode(from: number): LogicalLine | null {
    for (let i = from + 1; i < logical.length; i++) {
      const candidate = logical[i];
      if (candidate && lineKind(candidate) === "code") return candidate;
    }
    return null;
  }

  /** A declarator split by a conditional continues when the next code line is the other branch and a parameter list follows it. */
  function paramListAcross(from: number): boolean {
    const own = codeTokens(logical[from]!);
    if (!isTypePrefix(own.slice(0, -1)) || own.at(-1)?.kind !== "ident") return false;
    const next = nextCode(from);
    if (!next || indentOf(next) !== indentOf(logical[from]!)) return false;
    const between = logical.slice(from + 1, logical.indexOf(next));
    const splits = between.some((line) => line && lineKind(line) === "directive" && condKind(line.tokens.map((tok) => tok.text).join("")) === "mid");
    if (!splits) return false;
    return parameterListLine(codeTokens(nextCode(logical.indexOf(next)) ?? { no: 0, lead: "", gaps: [], tokens: [], trailing: "" }));
  }

  function previousCode(from: number): LogicalLine | null {
    for (let i = from - 1; i >= 0; i--) {
      const candidate = logical[i];
      if (candidate && lineKind(candidate) === "code") return candidate;
    }
    return null;
  }

  function isLabelColon(index: number, line: LogicalLine): boolean {
    const toks = codeTokens(line);
    if (!(toks.length === 1 && toks[0]?.text === ":")) return false;
    const prev = previousCode(index);
    if (!prev) return false;
    const earlier = codeTokens(prev);
    if (earlier.length === 1 && earlier[0]?.kind === "ident") return true;
    const head = earlier[0]?.text;
    return head === "case" || head === "default";
  }

  function unbalanced(line: LogicalLine): boolean {
    let paren = 0;
    let bracket = 0;
    for (const tok of codeTokens(line)) {
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
    }
    return paren > 0 || bracket > 0;
  }

  function pascalCueTokens(index: number, line: LogicalLine, enteringParen: number): Tok[] {
    const own = codeTokens(line);
    const needsPrev = enteringParen > 0 || (own.length === 1 && own[0]?.text === ":");
    if (!needsPrev) return own;
    const lastOwn = own.at(-1)?.text ?? "";
    // `25,` inside a constructor is not a split header. Walking every comma is quadratic.
    if (enteringParen > 0 && (lastOwn === "," || lastOwn === ";")) return own;
    const cue = new Set([
      "procedure", "function", "constructor", "destructor", "operator", "program", "library",
      "if", "for", "while", "with", "repeat", "try", "case", "asm", "else", "otherwise", "on",
    ]);
    const lines: Tok[][] = [];
    for (let i = index - 1; i >= 0; i--) {
      const prev = logical[i];
      if (!prev || lineKind(prev) !== "code") continue;
      const toks = codeTokens(prev);
      lines.unshift(toks);
      if (pascalWords(toks).some((word) => cue.has(word))) break;
      const last = toks.at(-1)?.text ?? "";
      if (!unbalanced(prev) && last !== "(" && last !== ",") break;
    }
    return [...lines.flat(), ...own];
  }

  function statementStartIndent(index: number, line: LogicalLine): number {
    let indent = indentOf(line);
    let paren = 0;
    let bracket = 0;
    const count = (toks: Tok[]): void => {
      for (const tok of toks) {
        if (tok.text === "(") paren++;
        else if (tok.text === ")") paren--;
        else if (tok.text === "[") bracket++;
        else if (tok.text === "]") bracket--;
      }
    };
    count(codeTokens(line));
    if (paren < 0 || bracket < 0) {
      for (let i = index - 1; i >= 0 && (paren < 0 || bracket < 0); i--) {
        const prev = logical[i];
        if (!prev || lineKind(prev) !== "code") continue;
        indent = Math.min(indent, indentOf(prev));
        count(codeTokens(prev));
      }
      return indent;
    }
    for (let i = index - 1; i >= 0; i--) {
      const prev = logical[i];
      if (!prev || lineKind(prev) !== "code") continue;
      const toks = codeTokens(prev);
      const last = toks.at(-1)?.text ?? "";
      if (last === "(") {
        indent = Math.min(indent, indentOf(prev));
        break;
      }
      const continues =
        last === "," ||
        last === "\\" ||
        isTypePrefix(toks) ||
        (last !== ":" && HANGING.has(last)) ||
        unbalanced(prev);
      if (!continues) break;
      indent = Math.min(indent, indentOf(prev));
    }
    return indent;
  }

  /** A directive between this line and the next code line splits a header from its body. */
  function directiveBeforeNext(index: number, until: LogicalLine | null): boolean {
    if (!until) return false;
    for (let i = index + 1; i < logical.length; i++) {
      const cand = logical[i];
      if (!cand || cand === until) return false;
      if (lineKind(cand) === "directive") return true;
      if (lineKind(cand) === "code") return false;
    }
    return false;
  }

  /** The colon body starts on a conditional boundary, so `begin` has to follow that boundary. */
  function branchEndsBeforeBody(index: number): boolean {
    for (let i = index + 1; i < logical.length; i++) {
      const cand = logical[i];
      if (!cand) continue;
      const kind = lineKind(cand);
      if (kind === "blank" || kind === "comment") continue;
      if (kind !== "directive") return false;
      const role = condKind(cand.tokens.map((tok) => tok.text).join(""));
      return role === "close" || role === "mid";
    }
    return false;
  }

  /** An offside body after that boundary is one statement, so it must not grow its own begin/end. */
  function singleStatementBody(index: number, headerIndent: number): boolean {
    let bodyIndent = -1;
    let statements = 0;
    for (let i = index + 1; i < logical.length; i++) {
      const cand = logical[i];
      if (!cand) continue;
      const kind = lineKind(cand);
      if (kind === "blank" || kind === "comment" || kind === "directive") continue;
      const col = indentOf(cand);
      if (col <= headerIndent) break;
      if (bodyIndent < 0) bodyIndent = col;
      if (col < bodyIndent) break;
      if (col === bodyIndent) {
        statements++;
        if (statements > 1) return false;
      }
    }
    return true;
  }

  function isOpenHeader(index: number, line: LogicalLine, paren: number): boolean {
    if (paren !== 0) return false;
    const code = codeTokens(line);
    const first = code[0];
    if (!first || (first.kind !== "keyword" && first.kind !== "ident")) return false;
    if (code.at(-1)?.text !== ")" || !code.some((t) => t.text === "(")) return false;
    const headerIndent = indentOf(line);
    for (let i = index + 1; i < logical.length; i++) {
      const cand = logical[i];
      if (!cand || lineKind(cand) !== "code") continue;
      const toks = codeTokens(cand);
      if (toks.length === 1 && toks[0]?.text === ":") return true;
      if (indentOf(cand) > headerIndent) continue;
      return false;
    }
    return false;
  }

  /** The next preprocessor arm opens a `{` the previous arm also opened. */
  function armOpensBrace(from: number): boolean {
    let net = 0;
    const stack: { if0: boolean; dead: boolean }[] = [];
    const dead = (): boolean => stack.some((frame) => frame.dead);
    for (let i = from + 1; i < logical.length; i++) {
      const cand = logical[i];
      if (!cand) continue;
      if (lineKind(cand) === "directive") {
        const text = cand.tokens.map((tok) => tok.text).join("");
        const role = condKind(text);
        const zero = /^#\s*if\s+0\b/.test(text.trim().toLowerCase());
        if (role === "open") stack.push({ if0: zero, dead: dead() || zero });
        else if (role === "close") {
          if (stack.length === 0) break;
          stack.pop();
        } else if (role === "mid") {
          if (stack.length === 0) break;
          const frame = stack[stack.length - 1]!;
          const outer = stack.slice(0, -1).some((item) => item.dead);
          if (frame.if0 && !outer) frame.dead = /^#\s*elif\s+0\b/.test(text.trim().toLowerCase());
        }
        continue;
      }
      if (dead() || lineKind(cand) !== "code") continue;
      for (const tok of codeTokens(cand)) {
        if (tok.text === "{") net++;
        else if (tok.text === "}") net--;
      }
    }
    return net > 0;
  }

  for (let index = 0; index < logical.length; index++) {
    const line = logical[index];
    if (!line) continue;
    const kind = lineKind(line);
    if (kind === "directive") {
      const text = line.tokens.map((tok) => tok.text).join("");
      const col = line.tokens[0]?.col ?? 1;
      if (profile.id === "c") {
        if (pending && col > pending.headerIndent) {
          stack.push({ ...pending, bodyIndent: col });
          events.push({ line: line.no, message: `Body of ${pending.name} starts at column ${col}` });
          pending = null;
        }
        while (stack.length > 0) {
          const top = stack[stack.length - 1];
          if (!top || col >= top.bodyIndent) break;
          const inner: LogicalLine[] = [];
          const outer: LogicalLine[] = [];
          for (const item of held) {
            const at = item.tokens[0]?.col ?? 1;
            if (at >= top.bodyIndent) inner.push(item);
            else outer.push(item);
          }
          held.length = 0;
          held.push(...inner);
          flushHeld();
          held.push(...outer);
          closeFrame(top, line.no, null);
          stack.pop();
        }
        if (pending && col <= pending.headerIndent) {
          closeFrame(pending, line.no, null);
          pending = null;
        }
      }
      const role = condKind(text);
      const zero = /^#\s*if\s+0\b/.test(text.trim().toLowerCase());
      if (role === "open") {
        ppBrace.push(state.braces.length);
        deadStack.push({ if0: zero, dead: inDead() || zero });
      } else if (role === "mid" && deadStack.length > 0) {
        const frame = deadStack[deadStack.length - 1]!;
        const outer = deadStack.slice(0, -1).some((item) => item.dead);
        if (frame.if0 && !outer) frame.dead = /^#\s*elif\s+0\b/.test(text.trim().toLowerCase());
        else if (!inDead() && ppBrace.length > 0) {
          const base = ppBrace[ppBrace.length - 1]!;
          if (armOpensBrace(index) && state.braces.length > base) {
            state = { ...state, braces: state.braces.slice(0, base), braceParen: state.braceParen.slice(0, base) };
          }
        }
      } else if (role === "close") {
        if (ppBrace.length > 0) ppBrace.pop();
        deadStack.pop();
      }
      held.push(line);
      continue;
    }
    if (kind !== "code") {
      held.push(line);
      continue;
    }

    if (profile.id === "pascal" && hanging && indentOf(line) <= hangIndent) {
      const head = foldText(codeTokens(line)[0]);
      const fresh = [
        "procedure", "function", "constructor", "destructor", "operator", "begin", "end",
        "var", "type", "const", "label", "threadvar", "resourcestring", "if", "for", "while",
        "repeat", "try", "case", "with", "asm", "uses", "initialization", "finalization",
        "interface", "implementation", "program", "unit", "library",
      ].includes(head);
      if (fresh) {
        const prev = out.at(-1);
        const text = prev ? textOf(prev).trimEnd() : "";
        if (prev && text && !text.endsWith(";") && !text.endsWith(":")) {
          prev.segments.push({ text: ";", inserted: true });
        }
        hanging = false;
      }
    }

    const suppressed =
      hanging ||
      effectiveParen(state.paren, state.braces, state.braceParen) > 0 ||
      state.bracket > 0 ||
      state.braces.at(-1) === "init";
    let closed = 0;
    let lastClosedHeader = -1;
    let absorbed = false;
    let absorbedHang = false;
    const bitfield = codeTokens(line).at(-1)?.text === ":" && /^\d/.test((nextCode(index) ? codeTokens(nextCode(index)!)[0]?.text : "") ?? "");
    if (!suppressed) {
      const indent = indentOf(line);
      if (
        pending &&
        indent <= pending.headerIndent &&
        !(pending.wantsTail && indent === pending.headerIndent && isDeclaratorTail(line))
      ) {
        const endMod = profile.id === "pascal" && indent === pending.headerIndent ? pascalEndModifier(line) : null;
        const headNow = profile.id === "pascal" ? foldText(codeTokens(line)[0]) : (codeTokens(line)[0]?.text ?? "");
        const attachesNow = headNow === "else" || headNow === "otherwise" || headNow === "catch";
        closeEmptyPending(pending, line.no, endMod, attachesNow);
        if (endMod) absorbed = true;
        lastClosedHeader = pending.headerIndent;
        closed++;
        pending = null;
      }
      while (stack.length > 0) {
        const top = stack[stack.length - 1];
        if (!top || indent >= top.bodyIndent) break;
        if (bitfield && indent > indentOf(line)) break;
        if (profile.id === "pascal" && top.keepOpen && indent === top.headerIndent && pascalTrySection(line)) {
          const section = codeTokens(line)[0]?.text.toLowerCase() ?? "";
          if (top.arm && section && top.arm !== section) {
            notes.push({
              line: line.no,
              message: `This try already has ${top.arm}. A Pascal try takes except or finally, not both — nest a second try.`,
            });
          }
          top.arm = section || top.arm;
          break;
        }
        const inner: LogicalLine[] = [];
        const outer: LogicalLine[] = [];
        for (const item of held) {
          const col = item.tokens[0]?.col ?? 1;
          if (col >= top.bodyIndent) inner.push(item);
          else outer.push(item);
        }
        held.length = 0;
        held.push(...inner);
        flushClosing(top);
        held.push(...outer);
        stack.pop();
        lastClosedHeader = top.headerIndent;
        const lineCode = codeTokens(line);
        const declTail = top.wantsTail && isDeclaratorTail(line);
        const attrTail = top.closer === "}" && top.semiOnClose && isAttributeLine(lineCode);
        const endMod = profile.id === "pascal" ? pascalEndModifier(line) : null;
        const tail =
          !absorbed &&
          stack.every((frame) => indent >= frame.bodyIndent) &&
          indent === top.headerIndent &&
          (declTail || attrTail || !!endMod);
        if (tail) {
          const rendered = textOf({ segments: renderLine(line, "keep", ""), sourceLine: line.no }).trim();
          const openDecl = declTail && lineCode.at(-1)?.text === ",";
          closeFrame(top, line.no, declTail || attrTail ? rendered : endMod, openDecl);
          absorbed = true;
          if (openDecl) absorbedHang = true;
        } else {
          const word = profile.id === "pascal" ? foldText(lineCode[0]) : (lineCode[0]?.text ?? "");
          const attaches = word === "else" || word === "otherwise" || word === "catch";
          closeStatement(top, line.no, held, attaches);
        }
        closed++;
      }
      if (closed > 0 && !absorbed) {
        const aligned =
          indent === 1 ||
          state.braces.at(-1) === "stmt" ||
          stack.some((frame) => frame.bodyIndent === indent || frame.headerIndent === indent);
        if (!aligned && profile.id !== "pascal") {
          notes.push({
            line: line.no,
            message: `Indent column ${indent} is not an open block boundary.`,
          });
        }
      }
      if (!absorbed && pending) {
        if (indent > pending.headerIndent) {
          stack.push({ ...pending, bodyIndent: indent });
          events.push({
            line: line.no,
            message: `Body of ${pending.name} starts at column ${indent}`,
          });
        } else if (pending.wantsTail && indent === pending.headerIndent && isDeclaratorTail(line)) {
          const openDecl = codeTokens(line).at(-1)?.text === ",";
          closeFrame(pending, line.no, textOf({ segments: renderLine(line, "keep", ""), sourceLine: line.no }).trim(), openDecl);
          absorbed = true;
          if (openDecl) absorbedHang = true;
          notes.push({ line: pending.line, message: `${pending.name} has no indented body.` });
        } else {
          const pendingHead = profile.id === "pascal" ? foldText(codeTokens(line)[0]) : (codeTokens(line)[0]?.text ?? "");
          const attaches = pendingHead === "else" || pendingHead === "otherwise" || pendingHead === "catch";
          closeStatement(pending, line.no, held, attaches);
          lastClosedHeader = pending.headerIndent;
          if (closed === 0 && (pendingHead === "else" || pendingHead === "otherwise" || pendingHead === "catch")) closed = 1;
          notes.push({ line: pending.line, message: `${pending.name} has no indented body.` });
          events.push({ line: line.no, message: `Closed empty ${pending.name}` });
        }
        pending = null;
      }
    }

    if (absorbed) {
      hanging = absorbedHang;
      hangText = absorbedHang ? "," : "";
      if (absorbedHang) hangIndent = indentOf(line);
      expectWhile = false;
      continue;
    }

    const code = codeTokens(line);
    const head = code[0];
    const headWord = profile.id === "pascal" ? foldText(head) : (head?.text ?? "");
    const doWhile = expectWhile && headWord === "while";
    expectWhile = false;
    const directiveBetween = held.some((line) => lineKind(line) === "directive");
    const glueWord = headWord === "else" || headWord === "catch" || headWord === "otherwise" || (profile.id !== "pascal" && headWord === "finally") || doWhile;
    const attaches = closed >= 1 && lastClosedHeader === indentOf(line);
    const glue =
      !!head &&
      (head.kind === "keyword" || profile.id === "pascal") &&
      glueWord &&
      attaches &&
      !directiveBetween &&
      held.every((line) => lineKind(line) !== "code");
    let prefix: OutSegment[] | null = null;
    if (
      !glue &&
      directiveBetween &&
      attaches &&
      glueWord &&
      /^\s*end;$/.test(textOf(out[out.length - 1] ?? { segments: [] }))
    ) {
      const braceLine = out.pop();
      if (braceLine) {
        out.push({
          segments: [{ text: textOf(braceLine).replace(/end;$/, "end"), inserted: true }],
          sourceLine: braceLine.sourceLine,
        });
      }
      flushHeld();
    }
    if (inDead()) {
      flushHeld();
      out.push(...explode(renderLine(line, "keep", ""), line.no));
      hanging = false;
      hangText = "";
      continue;
    }
    if (glue) {
      const braceLine = out[out.length - 1];
      const glued = braceLine ? textOf(braceLine) : "";
      if (glued.trim() === "}") {
        out.pop();
        prefix = [...(braceLine?.segments ?? []), { text: " ", inserted: false }];
      } else if (/^\s*end;$/.test(glued) && braceLine) {
        out.pop();
        prefix = [{ text: glued.replace(/end;$/, "end "), inserted: true }];
      }
    }

    const frameNow = stack.at(-1);
    const openingBody =
      !!frameNow?.delayedBegin &&
      !frameNow.bodyStarted &&
      indentOf(line) > frameNow.headerIndent &&
      !pascalDeclaration(code);
    if (openingBody && frameNow) {
      const bodyTrivia: LogicalLine[] = [];
      const headerTrivia: LogicalLine[] = [];
      let depth = frameNow.condDepth;
      let inBody = false;
      const boundaryAhead = (from: number): boolean => {
        for (let i = from; i < held.length; i++) {
          const ahead = held[i];
          if (!ahead) continue;
          const kind = lineKind(ahead);
          if (kind === "blank" || kind === "comment") continue;
          if (kind !== "directive") return false;
          const role = condKind(ahead.tokens.map((tok) => tok.text).join(""));
          return role === "close" || role === "mid";
        }
        return false;
      };
      for (let heldIndex = 0; heldIndex < held.length; heldIndex++) {
        const item = held[heldIndex];
        if (!item) continue;
        if (!inBody) {
          const kind = lineKind(item);
          const role = kind === "directive" ? condKind(item.tokens.map((tok) => tok.text).join("")) : "other";
          if (role === "close" && depth > 0) {
            headerTrivia.push(item);
            depth--;
            continue;
          }
          if ((role === "mid" || role === "open") && depth > 0) {
            headerTrivia.push(item);
            if (role === "open") depth++;
            continue;
          }
          // `{$endif}` / `{$else}` that close a conditional opened in the parent
          // must be emitted before this frame's begin, or the end survives when the branch is off.
          if (depth === 0 && (role === "close" || role === "mid" || ((kind === "blank" || kind === "comment") && boundaryAhead(heldIndex + 1)))) {
            headerTrivia.push(item);
            continue;
          }
          if (kind !== "code" && depth > 0) {
            headerTrivia.push(item);
            continue;
          }
          if ((item.tokens[0]?.col ?? 1) <= frameNow.headerIndent) {
            headerTrivia.push(item);
            continue;
          }
          inBody = true;
        }
        bodyTrivia.push(item);
      }
      frameNow.condDepth = depth;
      frameNow.bodyStarted = true;
      const openingWords = pascalWords(code);
      const branchShell = openingWords.length === 1 && (openingWords[0] === "begin" || openingWords[0] === "asm");
      let around = frameNow.condDepth;
      if (branchShell) {
        for (const item of held) {
          if (lineKind(item) !== "directive") continue;
          const role = condKind(item.tokens.map((tok) => tok.text).join(""));
          if (role === "open") around++;
          else if (role === "close") around = Math.max(0, around - 1);
        }
      }
      held.length = 0;
      held.push(...headerTrivia);
      flushHeld();
      if (depth > 0 || around > 0) {
        frameNow.skipShell = true;
        events.push({ line: line.no, message: `Left begin/end to the conditional branches of ${frameNow.name}` });
      } else {
        const beginPad = " ".repeat(Math.max(0, frameNow.headerIndent - 1));
        out.push({ segments: [{ text: `${beginPad}begin`, inserted: true }], sourceLine: line.no });
        events.push({ line: line.no, message: `Inserted begin for ${frameNow.name}` });
      }
      held.push(...bodyTrivia);
      flushHeld();
    } else {
      flushHeld();
    }

    const next = classify(line, state, hangText, isLabelColon(index, line));
    const ahead = nextCode(index);
    const aheadCode = ahead ? codeTokens(ahead) : [];
    const aheadIndex = ahead ? logical.indexOf(ahead) : -1;
    const afterAhead = aheadIndex >= 0 ? nextCode(aheadIndex) : null;
    const afterCode = afterAhead ? codeTokens(afterAhead) : [];
    const headText = code[0]?.text;
    const loneColonNext =
      aheadCode.length === 1 &&
      aheadCode[0]?.text === ":" &&
      (COLON_HEADER.has(headText ?? "") ||
        code.at(-1)?.text === ")" ||
        (code.length === 1 && code[0]?.kind === "ident") ||
        headText === "case" ||
        headText === "default");
    const joinString = code.at(-1)?.kind === "string" && aheadCode[0]?.kind === "string";
    const inEnum = stack.at(-1)?.kind === "enum" && next.braces.at(-1) !== "init";
    const indentedStar =
      (aheadCode[0]?.text === "*" || aheadCode[0]?.text === "&") && (aheadCode[0]?.col ?? 0) > indentOf(line);
    const cont =
      isOpenHeader(index, line, next.paren) ||
      loneColonNext ||
      bitfield ||
      (!!aheadCode[0] && CONT_START.has(aheadCode[0].text) && aheadCode[0].text !== "=") ||
      (isTypePrefix(code.slice(0, -1)) && code.at(-1)?.kind === "ident" && ahead && ahead === nextCode(index) && ahead.no > line.no + 1 && parameterListLine(aheadCode)) ||
      paramListAcross(index) ||
      (headText === "else" && (code.length === 1 || aheadCode[0]?.text === "if" || aheadCode[0]?.text === "do")) ||
      (aheadCode[0]?.text === ":" && aheadCode.length > 1) ||
      indentedStar ||
      (aheadCode[0]?.text === "=" && !(code.at(-1)?.text === ")" && code.some((t) => t.text === "("))) ||
      declarationContinues(code, aheadCode) ||
      controlContinues(code, aheadCode, indentOf(line)) ||
      attributeContinues(code, aheadCode) ||
      attributeSuffix(aheadCode, afterCode) ||
      specifierContinues(code, aheadCode) ||
      (aheadCode[0]?.text === "(" && (aheadCode[0]?.col ?? 0) > indentOf(line));

    const styleSource = profile.id === "pascal" ? pascalCueTokens(index, line, state.paren) : code;
    let style = profile.id === "pascal" ? pascalStyle(styleSource) : null;
    const topFrame = stack.at(-1);
    if (
      profile.id === "pascal" &&
      !style?.open &&
      code.at(-1)?.text === ":" &&
      topFrame?.name === "case" &&
      indentOf(line) === topFrame.bodyIndent &&
      ahead &&
      indentOf(ahead) > indentOf(line)
    ) {
      style = {
        open: true,
        colon: "arm",
        opener: "begin",
        closer: "end",
        delayed: false,
        dot: false,
        asm: false,
        keepOpen: false,
      };
    }
    let opens = next.opens;
    let colon: "keep" | "brace" | "drop" | "semi" | "begin" | "arm" = opens ? "brace" : "keep";
    if (doWhile) {
      opens = false;
      colon = code.at(-1)?.text === ":" ? "drop" : "keep";
    }
    if (
      profile.id === "pascal" &&
      style?.open &&
      style.colon === "begin" &&
      !style.delayed &&
      branchEndsBeforeBody(index)
    ) {
      // `else` / `{$endif}` / statement shares one statement with the branch that is off.
      // begin/end around it would leave the end inside the other arm.
      style = singleStatementBody(index, statementStartIndent(index, line))
        ? { ...style, colon: "drop", delayed: false, opener: "none", closer: "none" }
        : { ...style, colon: "drop", delayed: true };
    }
    if (style?.open) {
      opens = true;
      colon = style.colon;
    } else if (style?.colon === "drop") {
      opens = false;
      colon = "drop";
    }
    const anglesClosed = profile.id === "pascal" && code.at(-1)?.text === ">" && anglesBalance(code);
    const trySection = profile.id === "pascal" && pascalTrySection(line);
    const lineHanging = style?.open || anglesClosed || trySection ? false : next.hanging && !(profile.id === "pascal" && code.at(-1)?.text === "^");
    const effParen = effectiveParen(next.paren, next.braces, next.braceParen);
    const base = terminatorOf(
      line,
      opens,
      lineHanging,
      effParen,
      next.bracket,
      inEnum,
      next.braces.at(-1) === "init",
      next.closedInit,
      joinString,
    );
    let term = base === ";" && cont ? "" : base;
    const whileDone = effParen === 0 && next.bracket === 0 && (code.at(-1)?.text === ":" || !lineHanging);
    if (doWhile && code.at(-1)?.text !== ";" && !cont && whileDone) term = ";";
    const splitHead = headText === "if" || headText === "while" || headText === "for" || headText === "switch";
    if (splitHead && !doWhile && code.at(-1)?.text === ")" && directiveBeforeNext(index, ahead)) term = "";
    if (term === "," && aheadCode[0]?.text === ",") term = "";
    if (term === ";" && aheadCode.length === 1 && aheadCode[0]?.text === ";") term = "";
    if (profile.id === "pascal") {
      if (stack.at(-1)?.asm) term = "";
      const words = pascalWords(code);
      const bare = words[0] ?? "";
      const pair = `${bare} ${words[1] ?? ""}`.trim();
      const sectionOnly = (PASCAL_BARE.has(bare) && words.length === 1) || (PASCAL_BARE.has(pair) && words.length === 2);
      if (sectionOnly) term = "";
      if (bare === "class" && words.length === 2 && ["var", "property", "const", "threadvar"].includes(words[1] ?? "")) term = "";
      if (code.at(-1)?.text === "." && words.includes("end")) term = "";
      let tail = code.at(-1);
      for (let i = code.length - 1; i >= 0; i--) {
        const tok = code[i];
        if (!tok || tok.kind === "directive") continue;
        tail = tok;
        break;
      }
      if (tail?.text === ";") term = "";
    }

    const rendered = renderLine(line, colon, term, prefix !== null);
    out.push(...explode(prefix ? [...prefix, ...rendered] : rendered, line.no));
    if (profile.id === "pascal") {
      const last = out.at(-1);
      const joined = last ? textOf(last) : "";
      const wrapped = joined.includes(",") ? wrapPascalInit(joined) : joined;
      if (last && wrapped !== joined) {
        out.pop();
        for (const piece of wrapped.split("\n")) {
          out.push({ segments: [{ text: piece, inserted: false }], sourceLine: line.no });
        }
      }
    }
    if (term === ";") events.push({ line: line.no, message: "Inserted semicolon" });
    if (term === ",") events.push({ line: line.no, message: "Inserted comma" });
    if (cont && base === ";" && term === "") {
      events.push({ line: line.no, message: "Left this line open for the next line" });
    }
    if (opens) {
      const tokens = styleSource;
      const info = aggregateInfo(code);
      const pwords = profile.id === "pascal" ? pascalWords(code) : [];
      const openedName = pwords.includes("case") && pwords[pwords.length - 1] === "of" ? "case" : headerName(tokens);
      pending = {
        headerIndent: statementStartIndent(index, line),
        bodyIndent: indentOf(line),
        semiOnClose: semiOnClose(tokens, profile),
        name: openedName,
        kind: info.kind,
        wantsTail: info.wantsTail,
        line: line.no,
        opener: style?.opener ?? "{",
        closer: style?.closer ?? "}",
        delayedBegin: style?.delayed ?? false,
        bodyStarted: false,
        dot: style?.dot ?? false,
        asm: style?.asm ?? false,
        keepOpen: style?.keepOpen ?? false,
        arm: "",
        condDepth: 0,
        bodyCond: 0,
        skipShell: false,
      };
      events.push({
        line: line.no,
        message: pending.semiOnClose ? `Opened ${pending.name}, will close with };` : `Opened ${pending.name}`,
      });
    }
    state = {
      ternary: next.ternary,
      paren: next.paren,
      bracket: next.bracket,
      braces: next.braces,
      braceParen: next.braceParen,
    };
    hanging = lineHanging;
    hangIndent = indentOf(line);
    hangText = hanging ? (code.at(-1)?.text ?? "") : "";
  }

  while (held.length > 0 && lineKind(held[held.length - 1]!) === "blank") held.pop();

  if (pending) {
    const inner: LogicalLine[] = [];
    const outer: LogicalLine[] = [];
    for (const line of held) {
      const col = line.tokens[0]?.col ?? 1;
      if (col > pending.headerIndent) inner.push(line);
      else outer.push(line);
    }
    held.length = 0;
    if (inner.length > 0 && pending.delayedBegin && !pending.bodyStarted && pending.opener === "begin") {
      const beginPad = " ".repeat(Math.max(0, pending.headerIndent - 1));
      out.push({ segments: [{ text: `${beginPad}begin`, inserted: true }], sourceLine: pending.line });
      pending.bodyStarted = true;
      events.push({ line: pending.line, message: `Inserted begin for ${pending.name}` });
    }
    held.push(...inner);
    flushClosing(pending);
    closeStatement(pending, logical[logical.length - 1]?.no ?? pending.line, held, false);
    notes.push({ line: pending.line, message: `${pending.name} has no indented body.` });
    pending = null;
    held.push(...outer);
  }
  while (stack.length > 0) {
    const frame = stack[stack.length - 1];
    if (!frame) break;
    const inner: LogicalLine[] = [];
    const outer: LogicalLine[] = [];
    for (const item of held) {
      const col = item.tokens[0]?.col ?? 1;
      if (col >= frame.bodyIndent) inner.push(item);
      else outer.push(item);
    }
    held.length = 0;
    held.push(...inner);
    flushClosing(frame);
    held.push(...outer);
    stack.pop();
    closeStatement(frame, logical[logical.length - 1]?.no ?? 1, held, false);
  }
  flushHeld();
  if (state.braces.length > 0) notes.push({ line: 1, message: "Unclosed { in the source." });
  if (state.paren > 0) notes.push({ line: 1, message: "Unclosed ( in the source." });

  const code = out.length === 0 ? "" : `${out.map(textOf).join("\n")}\n`;
  return { language: lang, lines: out, code, events, notes };
}
