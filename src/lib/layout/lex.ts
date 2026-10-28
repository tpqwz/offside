import type { LayoutNote, Profile, Tok, TokenKind } from "./types.ts";

const MULTI = [
  ">>>=",
  ">>=",
  "<<=",
  "...",
  ">>>",
  "=>",
  "?.",
  "??",
  "==",
  "!=",
  "<=",
  ">=",
  "<>",
  "&&",
  "||",
  "++",
  "--",
  "+=",
  "-=",
  "*=",
  "/=",
  "%=",
  "&=",
  "|=",
  "^=",
  "<<",
  ">>",
  "->",
  "::",
  ":=",
  "..",
];

export function lex(source: string, profile: Profile): { tokens: Tok[]; notes: LayoutNote[] } {
  const tokens: Tok[] = [];
  const notes: LayoutNote[] = [];
  let i = 0;
  let line = 1;
  let col = 1;
  let atBol = true;

  function bump(): string {
    const c = source[i] ?? "";
    i++;
    if (c === "\n") {
      line++;
      col = 1;
    } else if (c === "\t") {
      col += 8 - ((col - 1) % 8);
    } else {
      col++;
    }
    return c;
  }

  function snapshot(): { line: number; col: number; start: number } {
    return { line, col, start: i };
  }

  function push(kind: TokenKind, from: { line: number; col: number; start: number }): void {
    tokens.push({
      kind,
      text: source.slice(from.start, i),
      line: from.line,
      col: from.col,
      start: from.start,
      end: i,
    });
    atBol = false;
  }

  function consumeString(quote: string): void {
    const from = snapshot();
    bump();
    while (i < source.length) {
      const c = source[i];
      if (c === "\\") {
        bump();
        if (i < source.length) bump();
        continue;
      }
      if (c === "\n") break;
      bump();
      if (c === quote) {
        push(quote === "'" ? "char" : "string", from);
        return;
      }
    }
    notes.push({ line: from.line, message: "Unclosed string" });
    push(quote === "'" ? "char" : "string", from);
  }

  function consumeLineComment(): void {
    const from = snapshot();
    bump();
    bump();
    while (i < source.length && source[i] !== "\n") bump();
    push("comment", from);
  }

  function consumeBlockComment(): void {
    const from = snapshot();
    bump();
    bump();
    while (i < source.length) {
      if (source[i] === "*" && source[i + 1] === "/") {
        bump();
        bump();
        push("comment", from);
        return;
      }
      bump();
    }
    notes.push({ line: from.line, message: "Unclosed block comment" });
    push("comment", from);
  }

  function consumeTemplate(): void {
    bump();
    let depth = 0;
    while (i < source.length) {
      const c = source[i];
      if (c === "\\") {
        bump();
        if (i < source.length) bump();
        continue;
      }
      if (c === "`" && depth === 0) {
        bump();
        return;
      }
      if (c === "$" && source[i + 1] === "{" && depth === 0) {
        bump();
        bump();
        depth = 1;
        continue;
      }
      if (depth > 0 && c === "`") {
        consumeTemplate();
        continue;
      }
      if (depth > 0 && (c === '"' || c === "'")) {
        const save = atBol;
        consumeString(c);
        tokens.pop();
        atBol = save;
        continue;
      }
      if (depth > 0 && c === "/" && source[i + 1] === "/") {
        while (i < source.length && source[i] !== "\n") bump();
        continue;
      }
      if (depth > 0 && c === "/" && source[i + 1] === "*") {
        bump();
        bump();
        while (i < source.length && !(source[i] === "*" && source[i + 1] === "/")) bump();
        if (i < source.length) {
          bump();
          bump();
        }
        continue;
      }
      if (depth > 0 && c === "{") {
        depth++;
        bump();
        continue;
      }
      if (depth > 0 && c === "}") {
        depth--;
        bump();
        continue;
      }
      bump();
    }
  }

  function consumeRegex(): boolean {
    const from = snapshot();
    const saved = { i, line, col, atBol };
    bump();
    let closed = false;
    while (i < source.length) {
      const c = source[i];
      if (c === "\n") break;
      if (c === "\\") {
        bump();
        if (i < source.length && source[i] !== "\n") bump();
        continue;
      }
      if (c === "[") {
        bump();
        while (i < source.length && source[i] !== "\n" && source[i] !== "]") {
          if (source[i] === "\\") bump();
          if (i < source.length) bump();
        }
        if (source[i] === "]") bump();
        continue;
      }
      bump();
      if (c === "/") {
        closed = true;
        break;
      }
    }
    if (!closed) {
      i = saved.i;
      line = saved.line;
      col = saved.col;
      atBol = saved.atBol;
      return false;
    }
    while (i < source.length && /[a-z]/i.test(source[i] ?? "")) bump();
    push("regex", from);
    return true;
  }

  function consumeNumber(): void {
    const from = snapshot();
    const hex = source[i] === "0" && (source[i + 1] === "x" || source[i + 1] === "X");
    const bin = source[i] === "0" && (source[i + 1] === "b" || source[i + 1] === "B");
    const oct = source[i] === "0" && (source[i + 1] === "o" || source[i + 1] === "O");
    if (hex || bin || oct) {
      bump();
      bump();
    }
    const isDigit = (ch: string): boolean => {
      if (hex) return /[0-9a-fA-F]/.test(ch);
      if (bin) return ch === "0" || ch === "1";
      return /[0-9]/.test(ch);
    };
    const eatDigits = (): void => {
      while (i < source.length) {
        const ch = source[i] ?? "";
        if (isDigit(ch)) {
          bump();
          continue;
        }
        if (profile.id !== "c" && profile.id !== "pascal" && ch === "_") {
          bump();
          continue;
        }
        // C23 digit separator: 1'000, 0xFF'FF. A char literal cannot follow a digit without a boundary we already passed.
        if (profile.id === "c" && ch === "'" && isDigit(source[i + 1] ?? "")) {
          bump();
          continue;
        }
        break;
      }
    };
    eatDigits();
    if (!hex && !bin && !oct && source[i] === "." && /[0-9]/.test(source[i + 1] ?? "")) {
      bump();
      eatDigits();
    }
    if (!hex && !bin && source[i] === "e" || (!hex && !bin && source[i] === "E")) {
      const n = source[i + 1];
      if (n === "+" || n === "-" || /[0-9]/.test(n ?? "")) {
        bump();
        if (source[i] === "+" || source[i] === "-") bump();
        eatDigits();
      }
    }
    if (profile.id === "c") {
      const rest = source.slice(i);
      const suffix = /^(?:[uU]?[wW][bB]|[dD][fFlL]|[uU][lL]{1,2}|[lL]{1,2}[uU]?|[uU]|[lL]|[fF])/.exec(rest);
      if (suffix && !/[A-Za-z0-9_$]/.test(rest[suffix[0].length] ?? "")) {
        for (let k = 0; k < suffix[0].length; k++) bump();
      }
    } else {
      while (i < source.length && /[uUlLfFn]/.test(source[i] ?? "")) bump();
    }
    push("number", from);
  }

  function previousCode(): Tok | undefined {
    for (let k = tokens.length - 1; k >= 0; k--) {
      if (tokens[k].kind !== "comment") return tokens[k];
    }
    return undefined;
  }

  function canStartRegex(): boolean {
    const prev = previousCode();
    if (!prev) return true;
    if (prev.kind === "ident" || prev.kind === "number" || prev.kind === "string" || prev.kind === "char" || prev.kind === "template" || prev.kind === "regex") {
      return false;
    }
    if (prev.text === ")" || prev.text === "]" || prev.text === "++" || prev.text === "--") return false;
    if (prev.kind === "keyword" && (prev.text === "this" || prev.text === "super")) return false;
    return true;
  }

  function consumePascalString(): void {
    const from = snapshot();
    bump();
    while (i < source.length) {
      if (source[i] === "'" && source[i + 1] === "'") {
        bump();
        bump();
        continue;
      }
      if (source[i] === "'") {
        bump();
        break;
      }
      if (source[i] === "\n") {
        notes.push({ line: from.line, message: "Unclosed string" });
        break;
      }
      bump();
    }
    push("string", from);
  }

  function consumeNested(open: string, close: string, kind: "comment" | "directive"): void {
    const from = snapshot();
    for (let k = 0; k < open.length; k++) bump();
    let depth = 1;
    while (i < source.length && depth > 0) {
      if (source.startsWith(open, i)) {
        depth++;
        for (let k = 0; k < open.length; k++) bump();
        continue;
      }
      if (source.startsWith(close, i)) {
        depth--;
        for (let k = 0; k < close.length; k++) bump();
        continue;
      }
      bump();
    }
    if (depth > 0) notes.push({ line: from.line, message: kind === "comment" ? "Unclosed comment" : "Unclosed directive" });
    push(kind, from);
  }

  function consumeUntil(end: string, kind: "comment" | "directive"): void {
    const from = snapshot();
    while (i < source.length && !source.startsWith(end, i)) bump();
    if (i >= source.length) notes.push({ line: from.line, message: kind === "comment" ? "Unclosed comment" : "Unclosed directive" });
    else for (let k = 0; k < end.length; k++) bump();
    push(kind, from);
  }

  function lexPascal(): boolean {
    const c = source[i];
    if (c === "^") {
      const n = source[i + 1] ?? "";
      const code = n.charCodeAt(0);
      const ctrl = (code >= 64 && code <= 95) || (code >= 97 && code <= 122);
      const continues = /[A-Za-z0-9_]/.test(source[i + 2] ?? "");
      const prev = previousCode();
      const deref = !!prev && (prev.kind === "ident" || prev.text === ")" || prev.text === "]" || prev.text === "^");
      // `^M` / `^m` is a control character. `P^` and `^^` stay dereferences, `^Integer` stays a pointer type.
      if (ctrl && !continues && !deref) {
        const from = snapshot();
        bump();
        bump();
        push("char", from);
        return true;
      }
    }
    if (c === "'") {
      consumePascalString();
      return true;
    }
    if (c === "{") {
      const directive = source[i + 1] === "$" || source[i + 1] === "%";
      if (directive) consumeUntil("}", "directive");
      else consumeNested("{", "}", "comment");
      return true;
    }
    if (c === "(" && source[i + 1] === "*") {
      const directive = source[i + 2] === "$";
      if (directive) consumeUntil("*)", "directive");
      else consumeNested("(*", "*)", "comment");
      return true;
    }
    if (c === "/" && source[i + 1] === "/") {
      consumeLineComment();
      return true;
    }
    if (c === "$" && /[0-9A-Fa-f]/.test(source[i + 1] ?? "")) {
      const from = snapshot();
      bump();
      while (i < source.length && /[0-9A-Fa-f]/.test(source[i] ?? "")) bump();
      push("number", from);
      return true;
    }
    if (c === "%" && (source[i + 1] === "0" || source[i + 1] === "1")) {
      const from = snapshot();
      bump();
      while (i < source.length && (source[i] === "0" || source[i] === "1")) bump();
      push("number", from);
      return true;
    }
    if (c === "&" && /[0-7]/.test(source[i + 1] ?? "")) {
      const from = snapshot();
      bump();
      while (i < source.length && /[0-7]/.test(source[i] ?? "")) bump();
      push("number", from);
      return true;
    }
    if (c === "&" && /[A-Za-z_]/.test(source[i + 1] ?? "")) {
      const from = snapshot();
      bump();
      while (i < source.length && /[A-Za-z0-9_]/.test(source[i] ?? "")) bump();
      push("ident", from);
      return true;
    }
    if (c === "#" && (source[i + 1] === "$" || source[i + 1] === "%" || /[0-9]/.test(source[i + 1] ?? ""))) {
      const from = snapshot();
      bump();
      if (source[i] === "$") {
        bump();
        while (i < source.length && /[0-9A-Fa-f]/.test(source[i] ?? "")) bump();
      } else if (source[i] === "%") {
        bump();
        while (i < source.length && (source[i] === "0" || source[i] === "1")) bump();
      } else {
        while (i < source.length && /[0-9]/.test(source[i] ?? "")) bump();
      }
      push("number", from);
      return true;
    }
    return false;
  }

  function pushDigraph(text: string, width: number): void {
    const from = snapshot();
    for (let k = 0; k < width; k++) bump();
    tokens.push({ kind: "punct", text, line: from.line, col: from.col, start: from.start, end: i });
    atBol = false;
  }

  while (i < source.length) {
    const c = source[i];
    if (c === " " || c === "\t" || c === "\r") {
      bump();
      continue;
    }
    if (c === "\n") {
      bump();
      atBol = true;
      continue;
    }
    if (atBol && profile.preprocessor && (c === "#" || (profile.id === "c" && source.startsWith("%:", i)))) {
      const from = snapshot();
      const digraph = c === "%";
      if (digraph) {
        bump();
        bump();
      }
      let comment = false;
      while (i < source.length && (comment || source[i] !== "\n")) {
        if (source[i] === "\\" && source[i + 1] === "\n") {
          bump();
          bump();
          continue;
        }
        if (!comment && source[i] === "/" && source[i + 1] === "*") comment = true;
        else if (comment && source[i] === "*" && source[i + 1] === "/") {
          comment = false;
          bump();
        }
        bump();
      }
      push("directive", from);
      if (digraph) {
        const last = tokens[tokens.length - 1];
        if (last) last.text = `#${last.text.slice(2)}`;
      }
      continue;
    }
    if (profile.id === "pascal" && lexPascal()) continue;
    if (c === "/" && source[i + 1] === "/") {
      consumeLineComment();
      continue;
    }
    if (c === "/" && source[i + 1] === "*") {
      consumeBlockComment();
      continue;
    }
    if (profile.regex && c === "/" && canStartRegex()) {
      if (consumeRegex()) continue;
    }
    if (c === '"' || c === "'") {
      consumeString(c);
      continue;
    }
    if (profile.templates && c === "`") {
      const from = snapshot();
      consumeTemplate();
      if (i >= source.length || source[from.start] === "`" && source[i - 1] !== "`") {
        notes.push({ line: from.line, message: "Unclosed template" });
      }
      push("template", from);
      continue;
    }
    if (profile.id === "pascal" && c === '"') {
      // Delphi/objfpc can use double quotes as string delimiters when {$mode} allows it. Treat them as strings.
      const from = snapshot();
      bump();
      while (i < source.length && source[i] !== "\n" && source[i] !== '"') bump();
      if (source[i] === '"') bump();
      else notes.push({ line: from.line, message: "Unclosed string" });
      push("string", from);
      continue;
    }
    if (/[A-Za-z_$]/.test(c) || (profile.id !== "javascript" && c.charCodeAt(0) > 127)) {
      const from = snapshot();
      while (i < source.length && (/[A-Za-z0-9_$]/.test(source[i] ?? "") || (profile.id === "c" && source.startsWith("\\u", i)) || (profile.id === "c" && source.startsWith("\\U", i)) || ((source[i] ?? "").charCodeAt(0) > 127))) {
        if (profile.id === "c" && (source.startsWith("\\u", i) || source.startsWith("\\U", i))) {
          const hexes = source[i + 1] === "u" ? 4 : 8;
          bump();
          bump();
          for (let h = 0; h < hexes && /[0-9A-Fa-f]/.test(source[i] ?? ""); h++) bump();
          continue;
        }
        bump();
      }
      const text = source.slice(from.start, i);
      const key = profile.id === "pascal" ? text.toLowerCase() : text;
      push(profile.keywords.has(key) ? "keyword" : "ident", from);
      continue;
    }
    if (/[0-9]/.test(c) || (c === "." && /[0-9]/.test(source[i + 1] ?? ""))) {
      consumeNumber();
      continue;
    }
    if (profile.id === "c" && source.startsWith("%:%:", i)) {
      pushDigraph("##", 4);
      continue;
    }
    if (profile.id === "c" && source.startsWith("%:", i)) {
      pushDigraph("#", 2);
      continue;
    }
    if (profile.id === "c" && source.startsWith("<%", i)) {
      pushDigraph("{", 2);
      continue;
    }
    if (profile.id === "c" && source.startsWith("%>", i)) {
      pushDigraph("}", 2);
      continue;
    }
    if (profile.id === "c" && source.startsWith("<:", i)) {
      pushDigraph("[", 2);
      continue;
    }
    if (profile.id === "c" && source.startsWith(":>", i)) {
      pushDigraph("]", 2);
      continue;
    }
    const from = snapshot();
    const rest = source.slice(i, i + 4);
    const op = MULTI.find((m) => rest.startsWith(m));
    if (op) {
      for (let k = 0; k < op.length; k++) bump();
      push("punct", from);
      continue;
    }
    bump();
    push("punct", from);
  }

  return { tokens, notes };
}
