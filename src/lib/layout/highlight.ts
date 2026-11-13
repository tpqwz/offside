import { lex } from "./lex.ts";
import type { Profile, TokenKind } from "./types.ts";

export interface Hue {
  text: string;
  cls?: string;
  inserted?: boolean;
}

const KIND: Partial<Record<TokenKind, string>> = {
  keyword: "tok-key",
  string: "tok-str",
  char: "tok-str",
  template: "tok-str",
  regex: "tok-str",
  comment: "tok-cmt",
  number: "tok-num",
  directive: "tok-dir",
};

function classFor(kind: TokenKind, text: string, source: string, end: number): string | undefined {
  if (kind === "punct" && text === ":") {
    const nl = source.indexOf("\n", end);
    const rest = source.slice(end, nl === -1 ? source.length : nl);
    if (rest.trim() === "") return "tok-colon";
  }
  return KIND[kind];
}

function insertedAt(ranges: Array<[number, number]>, pos: number): boolean {
  for (const [start, end] of ranges) {
    if (pos < start) return false;
    if (pos < end) return true;
  }
  return false;
}

/** Color a buffer with the layout lexer. Inserted ranges stay unmarked so the page can paint them as new. */
export function highlight(source: string, profile: Profile, inserted: Array<[number, number]> = []): Hue[] {
  if (source.length === 0) return [];
  const { tokens } = lex(source, profile);
  const marks = inserted.filter((range) => range[1] > range[0]).sort((a, b) => a[0] - b[0]);
  const cuts = new Set<number>([0, source.length]);
  for (const tok of tokens) {
    cuts.add(tok.start);
    cuts.add(tok.end);
  }
  for (const [start, end] of marks) {
    if (start > 0 && start < source.length) cuts.add(start);
    if (end > 0 && end < source.length) cuts.add(end);
  }
  for (let i = 0; i < source.length; i++) {
    if (source[i] === "\n") {
      cuts.add(i);
      cuts.add(i + 1);
    }
  }
  const points = [...cuts].sort((a, b) => a - b);
  const out: Hue[] = [];
  let ti = 0;
  for (let p = 0; p < points.length - 1; p++) {
    const start = points[p]!;
    const end = points[p + 1]!;
    if (end <= start) continue;
    while (ti < tokens.length && tokens[ti]!.end <= start) ti++;
    const tok = ti < tokens.length && tokens[ti]!.start <= start ? tokens[ti]! : undefined;
    const text = source.slice(start, end);
    const fresh = text !== "\n" && insertedAt(marks, start);
    const cls = text === "\n" || fresh || !tok ? undefined : classFor(tok.kind, tok.text, source, tok.end);
    const prev = out[out.length - 1];
    if (prev && prev.cls === cls && Boolean(prev.inserted) === fresh && prev.text !== "\n" && text !== "\n") {
      prev.text += text;
      continue;
    }
    const hue: Hue = { text };
    if (cls) hue.cls = cls;
    if (fresh) hue.inserted = true;
    out.push(hue);
  }
  return out;
}
