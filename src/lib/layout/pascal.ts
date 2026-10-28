import { lex } from "./lex.ts";
import { PROFILES } from "./languages.ts";
import type { LayoutNote, Tok } from "./types.ts";

const ROUTINE = new Set(["procedure", "function", "constructor", "destructor", "operator"]);
const TYPE_BODY = new Set([
  "record", "object", "class", "interface", "dispinterface",
  "objcclass", "objccategory", "objcprotocol",
]);
const SECTION = new Set(["var", "type", "const", "label", "threadvar", "resourcestring"]);
const VISIBILITY = new Set(["public", "private", "protected", "published", "automated"]);
export const MODIFIER = new Set([
  "forward", "external", "overload", "inline", "noinline", "noreturn", "nostackframe",
  "virtual", "abstract", "override", "reintroduce", "static", "cdecl", "stdcall", "pascal",
  "register", "safecall", "assembler", "message", "deprecated", "platform", "far", "near",
  "export", "name", "saveregisters", "interrupt", "varargs", "alias", "cppdecl", "mwpascal",
  "hardfloat", "softfloat", "iocheck", "extdecl",
]);
const NO_BODY = new Set(["forward", "external", "abstract"]);
const HARD_STOP = new Set(["end", "until", "except", "finally", "else", "otherwise"]);

// FPC concatenates `#194#128` and `'a'#10` only when nothing, not even a space, sits between them.
const PASCAL_LITERAL = /^(?:#(?:\$[0-9A-Fa-f]+|%[01]+|\d+)|'(?:[^']|'')*|"(?:[^"]|"")*"|\^[A-Za-z@\[\]\\^_])+$/;

function joinPascal(parts: string[]): string {
  const out: string[] = [];
  for (const part of parts) {
    if (!part) continue;
    const prev = out.at(-1);
    if (prev !== undefined && PASCAL_LITERAL.test(prev) && PASCAL_LITERAL.test(part)) out[out.length - 1] = prev + part;
    else out.push(part);
  }
  return out.join(" ").replace(/\s+/g, " ").trim();
}

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
 * Host Pascal (any Lazarus / Free Pascal mode) back into the colon dialect.
 * `begin`/`end` that the layout pass would emit are removed. `end.` of a unit
 * stays, because a unit is not one big block.
 */
export function toPascalSpaced(source: string): { code: string; notes: LayoutNote[] } {
  const normalized = source.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
  const lexed = lex(normalized, PROFILES.pascal);
  return new PascalBack(normalized, lexed.tokens, lexed.notes).run();
}

class PascalBack {
  readonly source: string;
  readonly tokens: Tok[];
  readonly code: Tok[];
  readonly notes: LayoutNote[];
  readonly out: string[] = [];
  readonly triviaLines: string[] = [];
  readonly afterLines: { col: number; text: string }[] = [];
  readonly pendingAfter: string[] = [];
  /** Bumps on every emitted line so a block can tell its own tail from the parent's. */
  pendingEpoch = 0;
  i = 0;
  mark = 0;
  trivia = 0;

  constructor(source: string, tokens: Tok[], notes: LayoutNote[]) {
    this.source = source;
    this.tokens = tokens;
    this.notes = notes;
    this.code = tokens.filter((tok) => tok.kind !== "comment" && tok.kind !== "directive");
  }

  run(): { code: string; notes: LayoutNote[] } {
    this.pull();
    const word = this.word();
    if (word === "program" || word === "library") this.parseProgram(0);
    else if (word === "unit") this.parseUnit();
    else if (this.hasBareImplementation()) this.parseInterfaceFragment();
    else this.parseSequence(0, new Set(), true);
    this.pull();
    this.flushTrivia(0);
    this.flushPending(0);
    return { code: finish(this.out), notes: this.notes };
  }

  /**
   * `{%MainUnit}` includes hold the tail of `interface` and then `implementation`.
   * Parsed alone they have no `unit` header, so a bare `implementation` is the boundary.
   */
  hasBareImplementation(): boolean {
    let paren = 0;
    let bracket = 0;
    for (const tok of this.code) {
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0 && tok.text.toLowerCase() === "implementation") return true;
    }
    return false;
  }

  parseInterfaceFragment(): void {
    let bodies = false;
    while (this.i < this.code.length) {
      this.pull();
      const word = this.word();
      if (!word) break;
      if (word === "interface" || word === "implementation" || word === "initialization" || word === "finalization") {
        this.line(0, word);
        this.i++;
        bodies = word !== "interface";
        continue;
      }
      this.parseItem(0, bodies);
    }
  }

  parseProgram(indent: number): void {
    const header = this.takeSemi();
    this.line(indent, `${header}:`);
    while (this.i < this.code.length && !this.at("begin") && !this.at("end")) this.parseItem(indent + 4, true);
    if (this.at("begin")) {
      this.i++;
      this.parseSequence(indent + 4, new Set(["end"]), true);
      this.consumeEnd();
    }
  }

  parseUnit(): void {
    const header = this.takeSemi();
    this.line(0, header);
    let bodies = false;
    while (this.i < this.code.length) {
      this.pull();
      const word = this.word();
      if (!word) break;
      if (word === "interface" || word === "implementation" || word === "initialization" || word === "finalization") {
        this.line(0, word);
        this.i++;
        bodies = word !== "interface";
        continue;
      }
      if (word === "end") {
        this.i++;
        const punct = this.curText() === "." || this.curText() === ";" ? this.curText() : "";
        if (punct) this.i++;
        this.line(0, `end${punct}`);
        continue;
      }
      this.parseItem(0, bodies);
    }
  }

  parseSequence(indent: number, stops: Set<string>, bodies: boolean): void {
    const epoch = this.pendingEpoch;
    while (this.i < this.code.length) {
      this.pull();
      const word = this.word();
      if (!word || stops.has(word)) break;
      const before = this.i;
      this.parseItem(indent, bodies);
      if (this.i === before) this.i++;
    }
    // `stmt {$endif} end` has no semicolon, so the endif was parked rather than
    // pulled. It is still inside this block. Leaving it pending flushes it at
    // the parent's indent, and the forward pass then hides `end` inside `{$else}`.
    if (this.pendingEpoch !== epoch) this.flushPending(indent);
    this.flushTrivia(indent);
  }

  parseItem(indent: number, bodies: boolean): void {
    this.pull();
    const word = this.word();
    if (!word) return;
    if (word === "uses" || word === "requires" || word === "contains" || word === "exports") {
      this.line(indent, this.takeSemi());
      return;
    }
    if (SECTION.has(word)) {
      this.parseSection(indent, bodies);
      return;
    }
    if (this.startsRoutine()) {
      this.parseRoutine(indent, bodies);
      return;
    }
    if (word === "begin") {
      this.i++;
      this.line(indent, "begin:");
      this.parseSequence(indent + 4, new Set(["end"]), true);
      this.consumeEnd();
      return;
    }
    if (word === "asm") {
      this.parseAsm(indent);
      return;
    }
    if (word === "repeat") {
      this.parseRepeat(indent, bodies);
      return;
    }
    if (word === "try") {
      this.parseTry(indent, bodies);
      return;
    }
    if (word === "case") {
      this.parseCase(indent, bodies);
      return;
    }
    if (word === "if" || word === "while" || word === "for" || word === "with" || word === "on") {
      this.parseHeaded(indent, bodies);
      return;
    }
    if (word === "else" || word === "otherwise") {
      this.i++;
      const elseEnd = this.code[this.i - 1]?.end ?? 0;
      if (word === "else" && this.at("if")) {
        this.parseHeaded(indent, bodies);
        this.prefixElse(indent);
      } else {
        this.line(indent, `${word}:`);
        this.parseCueBody(indent + 4, bodies, elseEnd);
      }
      return;
    }
    if (word === "initialization" || word === "finalization") {
      this.line(indent, word);
      this.i++;
      return;
    }
    if (word === "end") {
      this.i++;
      const punct = this.curText() === "." || this.curText() === ";" ? this.curText() : "";
      if (punct) this.i++;
      this.line(indent, `end${punct}`);
      return;
    }
    if (this.looksLikeTypeDecl()) {
      this.parseTypeDecl(indent);
      return;
    }
    const text = this.takeSemi();
    if (text) this.line(indent, text);
  }

  parseRoutine(indent: number, bodies: boolean): void {
    const routineStart = this.code[this.i]?.start ?? 0;
    const startCol = this.code[this.i]?.col ?? 1;
    let header = this.takeSemi();
    let forwarded = false;
    let mods = 0;
    let semiEmitted = false;
    while (MODIFIER.has(this.word())) {
      const modPos = this.code[this.i]?.start ?? 0;
      // `{$IFDEF} assembler; asm` / `begin` is one branch of the body, not a header modifier.
      if (this.conditionalDepth(routineStart, modPos) > 0 && this.wrappedByConditional(routineStart, startCol)) break;
      const modTok = this.code[this.i];
      const lead = modTok ? this.inlineDirectives(this.mark, modTok.start) : [];
      const flag = this.word();
      const text = this.takeSemi();
      const trailTok = this.code[this.i];
      const nextIsMod = MODIFIER.has(this.word());
      // An `{$IF}` still open at the next declaration belongs to the body, not the header.
      const trailOpen = !nextIsMod && !!trailTok && this.conditionalDepth(this.mark, trailTok.start) > 0;
      const trail = trailTok && !trailOpen ? this.inlineDirectives(this.mark, trailTok.start) : [];
      const leadText = lead.join(" ");
      const trailText = trail.join(" ");
      const sep = mods === 0 || !semiEmitted ? "; " : " ";
      header = `${header}${sep}${leadText ? `${leadText} ` : ""}${text}`.replace(/\s+/g, " ").trim();
      if (trailText) {
        header = `${header}; ${trailText}`.replace(/\s+/g, " ").trim();
        semiEmitted = true;
      } else {
        semiEmitted = false;
      }
      if (NO_BODY.has(flag)) forwarded = true;
      mods++;
    }
    const nextCol = this.code[this.i]?.col ?? 1;
    // One column of indent is stray alignment, not a nested routine. Real locals sit at least two columns in.
    const sibling = this.startsRoutine() && nextCol <= startCol + 1;
    const branched = this.wrappedByConditional(routineStart, startCol);
    const hasBody = !forwarded && bodies && !sibling && (this.looksLikeBody() || branched);
    const shown = hasBody ? `${header}:` : mods > 0 && !semiEmitted ? `${header};` : header;
    this.line(indent, shown);
    if (!hasBody) return;
    while (this.i < this.code.length && !this.at("begin") && !this.at("asm") && !this.at("end")) {
      if (SECTION.has(this.word()) || this.startsRoutine()) this.parseItem(indent + 4, true);
      else break;
    }
    if (branched) {
      this.parseBranchedBody(indent + 4, routineStart);
      return;
    }
    if (this.at("asm")) {
      this.parseAsm(indent + 4);
      return;
    }
    if (this.at("begin") && this.conditionalDepth(routineStart, this.code[this.i]?.start ?? routineStart) > 0) {
      this.parseBranchedBody(indent + 4, routineStart);
      return;
    }
    if (this.at("begin")) {
      this.i++;
      this.parseSequence(indent + 4, new Set(["end"]), true);
      this.consumeEnd();
    }
  }

  /** A `begin` inside `{$IF}` is one branch, not the routine's only body. Keep each branch's begin until the `{$IF}` closes. */
  parseBranchedBody(indent: number, routineStart: number): void {
    while (this.i < this.code.length) {
      const tok = this.code[this.i];
      if (!tok) break;
      if (this.conditionalDepth(routineStart, tok.start) === 0) {
        if (this.at("end")) this.consumeEnd();
        this.flushRoutineTail(routineStart, this.code[this.i]?.start ?? this.source.length, indent);
        break;
      }
      if (this.at("begin")) {
        const beginPos = tok.start;
        this.pull();
        this.line(indent, "begin:");
        this.i++;
        while (this.i < this.code.length && !this.at("end")) {
          const next = this.code[this.i];
          if (!next || this.leftBranch(beginPos, next.start)) break;
          const before = this.i;
          this.parseItem(indent + 4, true);
          if (this.i === before) this.i++;
        }
        if (this.at("end") && !this.leftBranch(beginPos, this.code[this.i]?.start ?? 0)) this.consumeEnd();
        continue;
      }
      const before = this.i;
      this.parseItem(indent, true);
      if (this.i === before) this.i++;
    }
  }

  /**
   * True when a directive between `beginPos` and `tokenPos` closes the `{$IF}` that contains
   * the begin, or starts its `{$ELSE}` arm. The shared `end` after `{$ENDIF}` is not part of the arm.
   */
  leftBranch(beginPos: number, tokenPos: number): boolean {
    const base = this.conditionalDepth(0, beginPos);
    if (base === 0) return false;
    let depth = base;
    for (const item of this.tokens) {
      if (item.start < beginPos) continue;
      if (item.start >= tokenPos) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) depth++;
      else if (/^\{\$\s*(else|elseif)\b/.test(text)) {
        if (depth === base) return true;
      } else if (/^\{\$\s*(endif|ifend)\b/.test(text)) {
        depth--;
        if (depth < base) return true;
      }
    }
    return false;
  }

  /** Directives that close this routine's `{$IF}` stay here. An `{$ENDIF}` opened before the routine does not. */
  flushRoutineTail(routineStart: number, toPos: number, indent: number): void {
    const base = this.conditionalDepth(0, routineStart);
    let depth = base;
    let stop = toPos;
    for (const item of this.tokens) {
      if (item.start < routineStart) continue;
      if (item.start >= toPos) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) depth++;
      else if (/^\{\$\s*(endif|ifend)\b/.test(text)) {
        if (depth <= base) {
          stop = item.start;
          break;
        }
        depth--;
      }
    }
    this.pullUntil(stop);
    this.flushTrivia(indent);
    this.flushPending(indent);
  }

  conditionalDepth(fromPos: number, toPos: number): number {
    let depth = 0;
    for (const item of this.tokens) {
      if (item.start < fromPos) continue;
      if (item.start >= toPos) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) depth++;
      else if (/^\{\$\s*(endif|ifend)\b/.test(text)) depth = Math.max(0, depth - 1);
    }
    return depth;
  }

  /** The routine's own `begin`/`asm` sits inside `{$IF}`. Nested routines, and routines nested in those, do not count. */
  wrappedByConditional(routineStart: number, startCol: number): boolean {
    const stack: { col: number; mode: "header" | "body"; depth: number }[] = [];
    let paren = 0;
    for (let k = this.i; k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) break;
      if (tok.text === "(") {
        paren++;
        continue;
      }
      if (tok.text === ")" && paren > 0) {
        paren--;
        continue;
      }
      if (paren > 0) continue;
      const word = tok.text.toLowerCase();
      const col = tok.col ?? 1;
      const top = stack.at(-1);
      if (top) {
        if (top.mode === "header") {
          if (this.startsRoutine(k) && col > top.col) {
            stack.push({ col, mode: "header", depth: 0 });
            continue;
          }
          // The nested routine's begin often shares that routine's column. The outer
          // begin sits back at the outer column. Modifiers (`inline`) are still the header.
          if ((word === "begin" || word === "asm") && col > startCol + 1) {
            top.mode = "body";
            top.depth = 1;
            continue;
          }
          if ((word === "begin" || word === "asm" || word === "end" || this.startsRoutine(k)) && col <= startCol + 1) {
            stack.pop();
            k--;
            continue;
          }
          continue;
        }
        if (word === "begin" || word === "asm") top.depth++;
        else if (word === "end") {
          top.depth--;
          if (top.depth <= 0) stack.pop();
        } else if (this.startsRoutine(k) && col > top.col) {
          stack.push({ col, mode: "header", depth: 0 });
        }
        continue;
      }
      if (this.startsRoutine(k) && col > startCol + 1) {
        stack.push({ col, mode: "header", depth: 0 });
        continue;
      }
      if (word === "begin" || word === "asm") return this.conditionalDepth(routineStart, tok.start) > 0;
      if (col <= startCol + 1 && (this.startsRoutine(k) || SECTION.has(word) || word === "end")) return false;
    }
    return false;
  }

  parseHeaded(indent: number, bodies: boolean): void {
    const kind = this.word();
    const ifPos = this.code[this.i]?.start ?? 0;
    const cueWord = kind === "if" || kind === "on" ? "then" : "do";
    const cue = this.findCue(cueWord);
    if (cue < 0) {
      this.line(indent, this.takeSemi());
      return;
    }
    // Include directives that sit between the condition and `then`/`do`
    // (`{$ENDIF} then`). Directives after the cue stay with the body.
    const header = this.flat(this.i, cue + 1, false);
    this.i = cue + 1;
    const cueEnd = this.code[cue]?.end ?? 0;
    this.line(indent, header.endsWith(":") ? header : `${header}:`);
    this.parseCueBody(indent + 4, bodies, cueEnd);
    // `end; else` already closed this if. That else belongs to a case, or to an outer if.
    // `then ; else` is an empty then-statement, so the else still belongs here.
    if ((kind === "if" || kind === "on") && (this.at("else") || this.at("otherwise")) && !this.ifAlreadyEnded() && !this.elseClosesOuter(ifPos)) {
      const word = this.word();
      this.i++;
      const elseEnd = this.code[this.i - 1]?.end ?? 0;
      const ifTok = this.code[this.i];
      // `else` / `{$ifdef}` / `if` is not `else if`. The ifdef has to stay between them.
      if (word === "else" && this.at("if") && ifTok && !this.openCondBetween(elseEnd, ifTok.start)) {
        this.parseHeaded(indent, bodies);
        this.prefixElse(indent);
      } else {
        this.line(indent, `${word}:`);
        this.parseCueBody(indent + 4, bodies, elseEnd);
      }
    }
  }

  /** An `{$endif}` sits between the cue and this `begin`, so the block is the shared else body. */
  endifBetween(fromPos: number, toPos: number): boolean {
    for (const item of this.tokens) {
      if (item.start < fromPos) continue;
      if (item.start >= toPos) break;
      if (item.kind === "directive" && /^\{\$\s*(endif|ifend)\b/i.test(item.text.trim())) return true;
    }
    return false;
  }

  /** An `{$if}` opened between two positions is still open at the second. `else` / `{$ifdef}` / `if` is not `else if`. */
  openCondBetween(fromPos: number, toPos: number): boolean {
    let depth = 0;
    let opened = false;
    for (const item of this.tokens) {
      if (item.start < fromPos) continue;
      if (item.start >= toPos) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) {
        if (depth === 0) opened = true;
        depth++;
      } else if (/^\{\$\s*(endif|ifend)\b/.test(text) && depth > 0) depth--;
    }
    return opened && depth > 0;
  }

  /** `end; else` closed this if. `then ; else` is an empty then-statement, so that else still belongs here. */
  ifAlreadyEnded(): boolean {
    if (this.code[this.i - 1]?.text !== ";") return false;
    const before = this.code[this.i - 2]?.text.toLowerCase() ?? "";
    return before !== "then" && before !== "do";
  }

  /** An `else` after an `{$endif}` that was already open at this `if` belongs to the outer if. */
  elseClosesOuter(ifPos: number): boolean {
    if (!this.at("else") && !this.at("otherwise")) return false;
    const elseTok = this.code[this.i];
    if (!elseTok) return false;
    const base = this.conditionalDepth(0, ifPos);
    if (base === 0) return false;
    let depth = base;
    for (const item of this.tokens) {
      if (item.start < ifPos) continue;
      if (item.start >= elseTok.start) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) depth++;
      else if (/^\{\$\s*(endif|ifend)\b/.test(text)) {
        depth--;
        if (depth < base) return true;
      }
    }
    return false;
  }

  /** One `then`/`do`/`else` body. An `{$if}` opened after the cue can hold several arms. */
  parseCueBody(indent: number, bodies: boolean, cueEnd: number): void {
    this.pull();
    const beginPos = this.code[this.i]?.start ?? 0;
    if (this.at("begin") && this.endifBetween(cueEnd, beginPos)) {
      // `else` / `{$endif}` / `begin` must keep that begin. Unwrapping it would
      // leave only the first statement inside the conditional's else.
      this.flushTrivia(indent);
      this.line(indent, "begin:");
      this.i++;
      this.parseSequence(indent + 4, new Set(["end"]), bodies);
      this.consumeEnd();
      return;
    }
    if (this.at("begin") && !this.openCondBetween(cueEnd, beginPos)) {
      this.unwrapArm(indent, bodies);
      return;
    }
    if (!this.word() || this.at("end") || this.at("else") || this.at("otherwise") || this.at("until") || this.at("except") || this.at("finally")) {
      this.flushTrivia(indent);
      return;
    }
    let arms = false;
    const parseArm = (): void => {
      const before = this.i;
      this.parseItem(indent, bodies);
      if (this.i === before) this.i++;
    };
    parseArm();
    while (this.pendingContinuesArm(cueEnd) || this.elseArmAhead(cueEnd)) {
      arms = true;
      parseArm();
    }
    if (this.bodyCondCloses(cueEnd)) this.pullBodyTail(cueEnd, indent);
    else if (arms) this.flushPending(indent);
  }

  parseRepeat(indent: number, bodies: boolean): void {
    this.pull();
    this.i++;
    this.line(indent, "repeat:");
    this.parseSequence(indent + 4, new Set(["until"]), bodies);
    if (this.at("until")) this.line(indent, this.takeSemi());
  }

  parseTry(indent: number, bodies: boolean): void {
    this.pull();
    this.i++;
    this.line(indent, "try:");
    while (this.i < this.code.length) {
      this.pull();
      const word = this.word();
      if (!word) break;
      if (word === "except" || word === "finally") {
        this.i++;
        this.line(indent, `${word}:`);
        // `{ ignore }` between `except` and `end` is the arm, not trivia for the parent try.
        this.pull();
        if (this.at("end")) this.flushTrivia(indent + 4);
        continue;
      }
      if (word === "end") {
        this.consumeEnd();
        return;
      }
      const before = this.i;
      this.parseItem(indent + 4, bodies);
      if (this.i === before) this.i++;
    }
  }

  parseCase(indent: number, bodies: boolean): void {
    const ofAt = this.findCue("of");
    if (ofAt < 0) {
      this.line(indent, this.takeSemi());
      return;
    }
    const header = this.flat(this.i, ofAt + 1, false);
    this.i = ofAt + 1;
    this.line(indent, header.endsWith(":") ? header : `${header}:`);
    const armIndent = indent + 4;
    let armOpen = false;
    let armCue = 0;
    while (this.i < this.code.length && !this.at("end")) {
      if (armOpen && this.bodyCondCloses(armCue)) this.pullBodyTail(armCue, indent + 8);
      this.pull();
      if (!this.word() || this.at("end")) break;
      if (this.at("else") || this.at("otherwise")) {
        armOpen = false;
        const word = this.word();
        this.i++;
        this.line(indent + 4, `${word}:`);
        if (this.at("begin")) this.unwrapArm(indent + 8, bodies);
        else if (!this.at("end") && !this.looksLikeCaseLabel()) {
          this.parseItem(indent + 8, bodies);
          // One arm stays on the old path: an `{$endif}` that closes the else has to
          // stay beside `else`, not inside a begin the forward pass would insert.
          // Further statements still belong to the else, up to the case `end`.
          if (this.word() && !this.at("end") && !this.at("else") && !this.at("otherwise")) {
            this.parseSequence(indent + 8, new Set(["end"]), bodies);
          }
        }
        continue;
      }
      if (
        this.startsRoutine() ||
        ["if", "for", "while", "repeat", "try", "case", "with", "asm", "begin", "raise", "goto"].includes(this.word())
      ) {
        this.parseItem(armIndent, bodies);
        continue;
      }
      const colon = this.findToken(":", 0);
      if (colon < 0) {
        this.parseItem(armIndent, bodies);
        continue;
      }
      armOpen = true;
      const label = this.flat(this.i, colon);
      this.i = colon + 1;
      armCue = this.code[colon]?.end ?? 0;
      if (this.curText() === ";") {
        this.i++;
        const text = label.endsWith(";") ? label : `${label}:;`;
        this.line(indent + 4, text);
        continue;
      }
      this.line(indent + 4, `${label}:`);
      const bodyPos = this.code[this.i]?.start ?? 0;
      if (this.at("begin") && !this.openCondBetween(armCue, bodyPos)) {
        this.unwrapArm(indent + 8, bodies, true);
      } else if (this.looksLikeCaseLabel()) {
        // The next arm. A bare label is not an empty statement, so leave it.
      } else if (!this.at("end") && !this.at("else") && !this.at("otherwise")) {
        // `{$if} stmt {$else} stmt {$endif}` is one arm. The endif stays inside it.
        this.parseCueBody(indent + 8, bodies, armCue);
      }
    }
    if (armOpen && this.bodyCondCloses(armCue)) this.pullBodyTail(armCue, indent + 8);
    if (this.at("end")) this.pullUntil(this.code[this.i]?.start ?? this.mark);
    this.flushTrivia(indent + 4);
    this.consumeEnd();
  }

  parseAsm(indent: number): void {
    this.pull();
    const asm = this.code[this.i];
    this.i++;
    this.line(indent, "asm:");
    const start = this.code[this.i]?.start ?? asm?.end ?? 0;
    while (this.i < this.code.length && !this.at("end")) this.i++;
    const end = this.code[this.i]?.start ?? this.source.length;
    const body = this.source.slice(start, end);
    for (const raw of body.split("\n")) {
      const trimmed = raw.trim();
      if (trimmed) this.line(indent + 4, trimmed.replace(/;\s*$/, ""));
    }
    this.mark = end;
    this.consumeEnd();
  }

  parseSection(indent: number, bodies: boolean, stopHere?: () => boolean): void {
    const kind = this.word();
    this.line(indent, kind);
    this.i++;
    while (this.i < this.code.length) {
      this.pull();
      const word = this.word();
      if (!word) break;
      if (stopHere?.()) break;
      if (kind === "type" && (word === "generic" || !this.endsSection(word))) {
        const before = this.i;
        this.parseTypeDecl(indent + 4);
        if (this.i === before) this.i++;
        continue;
      }
      if (this.endsSection(word)) break;
      const text = kind === "type" ? this.takeSemi() : this.takeBalanced();
      if (text && kind !== "type" && this.spreadInit(indent + 4, text)) continue;
      if (text) this.line(indent + 4, text);
    }
    this.flushTrivia(indent + 4);
  }

  parseTypeDecl(indent: number): void {
    const from = this.i;
    let paren = 0;
    let angle = 0;
    let eq = -1;
    while (this.i < this.code.length) {
      const text = this.curText();
      if (text === "(") paren++;
      else if (text === ")" && paren > 0) paren--;
      else if (text === "<") angle++;
      else if (text === ">" && angle > 0) angle--;
      else if (paren === 0 && angle === 0 && text === "=") {
        eq = this.i;
        this.i++;
        break;
      } else if (this.i > from && paren === 0 && angle === 0 && (text === ";" || this.endsSection(this.word()))) {
        break;
      }
      this.i++;
    }
    if (eq < 0) {
      const stopSemi = this.curText() === ";";
      const text = this.flat(from, this.i);
      if (stopSemi) this.i++;
      if (text) this.line(indent, text);
      return;
    }
    if (this.word() === "packed" && TYPE_BODY.has(this.wordAt(this.i + 1))) this.i++;
    const kind = this.word();
    const helper = (kind === "type" || kind === "class" || kind === "record") && this.wordAt(this.i + 1) === "helper";
    if ((TYPE_BODY.has(kind) || helper) && !(kind === "class" && this.wordAt(this.i + 1) === "of")) {
      this.i++;
      if (this.word() === "external") {
        this.i++;
        if (this.word() === "name") {
          this.i++;
          const next = this.curText();
          if (next && next !== "(" && next !== ";" && next !== ":") this.i++;
        }
      }
      if (this.word() === "helper") {
        this.i++;
        if (this.word() === "for") this.i++;
        while (this.i < this.code.length) {
          const word = this.word();
          if (
            !word || word === "end" || word === "strict" || VISIBILITY.has(word) ||
            SECTION.has(word) || word === "property" || word === "class" || this.startsRoutine() ||
            this.curText() === ";"
          ) break;
          this.i++;
        }
      }
      while (this.word() === "sealed" || this.word() === "abstract") this.i++;
      if (this.curText() === "(") this.skipGroup("(", ")");
      while (this.word() === "sealed" || this.word() === "abstract") this.i++;
      if (this.curText() === ";") {
        const text = this.flat(from, this.i);
        this.i++;
        this.line(indent, text);
        return;
      }
      const head = this.flat(from, this.i);
      this.line(indent, head.endsWith(":") ? head : `${head}:`);
      this.parseMembers(indent + 4);
      if (this.at("end")) this.consumeEnd();
      return;
    }
    // Flat the left side first. A directive sitting between `=` and the type
    // (`{$ifdef}Name{$else}range{$endif}`) belongs to this declaration.
    const left = this.flat(from, eq + 1);
    const rest = this.takeBalanced();
    const text = `${left} ${rest}`.replace(/\s+/g, " ").trim();
    if (text) this.line(indent, text);
  }

  parseMembers(indent: number): void {
    let guard = -1;
    while (this.i < this.code.length && !this.at("end")) {
      if (this.i === guard) {
        this.notes.push({ line: this.lineNo(), message: "Skipped a class member the reverse pass could not read." });
        this.i++;
        if (this.i >= this.code.length) break;
      }
      guard = this.i;
      this.pull();
      if (this.at("end") || this.i >= this.code.length) break;
      const word = this.word();
      if (word === "strict" && VISIBILITY.has(this.wordAt(this.i + 1))) {
        this.line(indent, `${word} ${this.wordAt(this.i + 1)}`);
        this.i += 2;
        continue;
      }
      if (VISIBILITY.has(word) || word === "optional" || word === "required") {
        this.line(indent, word);
        this.i++;
        continue;
      }
      if (word === "class" && ["var", "property", "const", "threadvar"].includes(this.wordAt(this.i + 1))) {
        this.line(indent, `${word} ${this.wordAt(this.i + 1)}`);
        this.i += 2;
        continue;
      }
      if (this.startsRoutine()) {
        this.parseRoutine(indent, false);
        continue;
      }
      if (word === "property" || word === "var" || word === "const") {
        this.line(indent, this.takeSemi());
        continue;
      }
      if (SECTION.has(word)) {
        this.parseSection(indent, false, () => (
          VISIBILITY.has(this.word()) || this.word() === "strict" || this.word() === "property" ||
          this.word() === "end" || this.startsRoutine()
        ));
        continue;
      }
      const field = this.takeBalanced();
      if (field) this.line(indent, field);
    }
    // Trivia before `end` belongs to this type, not the next declaration.
    if (this.at("end")) this.pullUntil(this.code[this.i]?.start ?? this.mark);
    this.flushTrivia(indent);
  }

  prefixElse(indent: number): void {
    const pad = " ".repeat(indent);
    for (let n = this.out.length - 1; n >= 0; n--) {
      const line = this.out[n];
      if (!line || line.trim() === "") continue;
      if (!(line.startsWith(pad) && !line.startsWith(`${pad} `))) continue;
      if (/^\s*if\b/i.test(line)) {
        this.out[n] = line.replace(/^(\s*)if\b/i, "$1else if");
        return;
      }
      if (/^\s*else\b/i.test(line)) continue;
      const trimmed = line.trim();
      if (trimmed.startsWith("//") || trimmed.startsWith("{") || trimmed.startsWith("(*")) continue;
      return;
    }
  }

  looksLikeBody(): boolean {
    const word = this.word();
    return word === "begin" || word === "asm" || SECTION.has(word) || this.startsRoutine();
  }

  /** `Name = class|record|objcclass...` outside a `type` section, as in Objective-Pascal includes. */
  looksLikeTypeDecl(): boolean {
    let i = this.i;
    const first = this.code[i];
    if (!first || (first.kind !== "ident" && first.text.toLowerCase() !== "generic")) return false;
    if (first.text.toLowerCase() === "generic") i++;
    if (this.code[i]?.kind !== "ident") return false;
    i++;
    if (this.code[i]?.text === "<") {
      let depth = 0;
      for (; i < this.code.length; i++) {
        const text = this.code[i]?.text;
        if (text === "<") depth++;
        else if (text === ">") {
          depth--;
          if (depth === 0) {
            i++;
            break;
          }
        }
      }
    }
    if (this.code[i]?.text !== "=") return false;
    i++;
    if (this.wordAt(i) === "packed" || this.wordAt(i) === "type") i++;
    const kind = this.wordAt(i);
    return TYPE_BODY.has(kind) || ((kind === "type" || kind === "class" || kind === "record") && this.wordAt(i + 1) === "helper");
  }

  startsRoutine(at = this.i): boolean {
    const word = this.wordAt(at);
    if (ROUTINE.has(word)) return true;
    if (word === "class" && ROUTINE.has(this.wordAt(at + 1))) return true;
    if (word === "generic") {
      const next = this.wordAt(at + 1);
      if (ROUTINE.has(next)) return true;
      if (next === "class" && ROUTINE.has(this.wordAt(at + 2))) return true;
    }
    return false;
  }

  endsSection(word: string): boolean {
    return (
      word === "begin" || word === "end" || word === "until" || word === "except" || word === "finally" ||
      word === "else" || word === "otherwise" || word === "implementation" || word === "interface" ||
      word === "initialization" || word === "finalization" || word === "uses" || word === "program" ||
      word === "unit" || word === "library" || SECTION.has(word) || ROUTINE.has(word) || word === "generic" ||
      word === "class" || word === "if" || word === "for" || word === "while" || word === "repeat" ||
      word === "try" || word === "case" || word === "asm" || word === "with"
    );
  }

  /** Like takeSemi, but semicolons inside record/object/class/begin/case/try/asm do not end it. */
  takeBalanced(): string {
    const from = this.i;
    let paren = 0;
    let bracket = 0;
    let ends = 0;
    const opensEnd = new Set(["record", "begin", "try", "asm"]);
    while (this.i < this.code.length) {
      const tok = this.code[this.i];
      if (!tok) break;
      const text = tok.text;
      const word = text.toLowerCase();
      const prev = this.wordAt(this.i - 1);
      const next = this.wordAt(this.i + 1);
      const opens =
        opensEnd.has(word) ||
        word === "interface" ||
        word === "dispinterface" ||
        ((word === "class" || word === "object") && prev !== "of" && next !== "of");
      if (text === "(") paren++;
      else if (text === ")" && paren > 0) paren--;
      else if (text === "[") bracket++;
      else if (text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0 && ends === 0 && text === ";" && this.sameDeclArm(from, this.i)) {
        // `cBreak = {$IFDEF} $0A; {$ELSE} $0B; {$ENDIF}` — keep the arm semicolon.
        // The last semicolon is a real terminator and is stripped below.
        this.i++;
        continue;
      } else if (paren === 0 && bracket === 0 && opens) {
        ends++;
      } else if (paren === 0 && bracket === 0 && word === "end" && ends > 0) {
        ends--;
      } else if (paren === 0 && bracket === 0 && ends === 0 && text === ";") {
        const body = this.flat(from, this.i);
        this.i++;
        return body;
      } else if (this.i > from && paren === 0 && bracket === 0 && ends === 0 && HARD_STOP.has(word)) {
        return this.flat(from, this.i);
      }
      this.i++;
    }
    return this.flat(from, this.i);
  }

  takeSemi(): string {
    return this.takeUntil(HARD_STOP);
  }

  /** Text up to a depth-0 semicolon or stop word. The semicolon is consumed; a stop word is not. */
  takeUntil(stops: Set<string>): string {
    const from = this.i;
    let paren = 0;
    let bracket = 0;
    while (this.i < this.code.length) {
      const tok = this.code[this.i];
      if (!tok) break;
      const word = tok.text.toLowerCase();
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0 && tok.text === ";" && this.sameDeclArm(from, this.i)) {
        this.i++;
        continue;
      } else if (paren === 0 && bracket === 0 && this.i > from && this.statementBoundary(from, this.i)) {
        // Inline directives that sit inside this statement first. Parking first
        // would hoist a `{$IF}` that is still inside the expression.
        const text = this.flat(from, this.i, false);
        this.parkBoundary(this.i);
        return text;
      } else if (paren === 0 && bracket === 0 && tok.text === ";") {
        // `stmt {$ENDIF};` — the endif closes a conditional around this statement.
        // Leave it outside so the semicolon stays after the conditional.
        const text = this.flat(from, this.i, !this.endifBeforeSemi(from, this.i));
        this.i++;
        return text;
      } else if (this.i > from && paren === 0 && bracket === 0 && stops.has(word)) {
        // `{$IFDEF}` between a statement and `else`/`end` belongs to the next line, not the statement.
        const text = this.flat(from, this.i, false);
        this.parkBoundary(this.i);
        return text;
      }
      this.i++;
    }
    return this.flat(from, this.i);
  }

  /** An `{$endif}` before this semicolon closes a conditional that was already open at the start of the statement. An `{$if}` opened inside the statement does not count: `uses A {$ifdef} , B {$endif};` keeps the endif on the uses line so the semicolon stays after it. */
  endifBeforeSemi(from: number, semiIndex: number): boolean {
    const semi = this.code[semiIndex];
    const first = this.code[from];
    if (!semi || !first) return false;
    const base = this.conditionalDepth(this.code[from - 1]?.end ?? 0, first.start);
    let depth = base;
    for (const item of this.tokens) {
      if (item.start < first.start) continue;
      if (item.start >= semi.start) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) depth++;
      else if (/^\{\$\s*(endif|ifend)\b/.test(text)) {
        if (depth <= base) return true;
        depth--;
      }
    }
    return false;
  }

  /** The semicolon is inside an `{$IF}` opened by this statement. Count a directive that sits before the first code token (`= {$IFDEF} ^T;`). */
  semiInOpenCond(from: number, index: number): boolean {
    const start = this.code[from - 1]?.end ?? 0;
    const at = this.code[index]?.start ?? start;
    return this.conditionalDepth(start, at) > 0;
  }

  /**
   * `;` separates two arms of one const/type (`$0A; {$ELSE} $0B`), so it stays in the text.
   * A statement terminator (`Result := 'N'; {$ELSE}`) and a modifier (`inline; {$ENDIF}`,
   * `virtual; {$ELSE} overload`) do not: those semicolons end the take and are stripped.
   */
  sameDeclArm(from: number, semiIndex: number): boolean {
    if (!this.semiInOpenCond(from, semiIndex)) return false;
    const next = semiIndex + 1;
    if (next >= this.code.length || !this.code[next]) return false;
    // Past `{$ENDIF}` the next token is a new statement, not the other arm.
    if (!this.semiInOpenCond(from, next)) return false;
    return this.isDeclValue(next);
  }

  /** A type or const value, not a call, assignment, routine, or method modifier. */
  isDeclValue(index: number): boolean {
    const tok = this.code[index];
    if (!tok) return false;
    if (tok.kind === "number" || tok.kind === "string") return true;
    const word = tok.text.toLowerCase();
    if (["^", "(", "[", "+", "-", "@"].includes(tok.text)) return true;
    if (["not", "nil", "array", "set", "file", "string", "packed", "specialize", "type"].includes(word)) return true;
    if (tok.kind !== "ident" && tok.kind !== "keyword") return false;
    if (MODIFIER.has(word) || HARD_STOP.has(word) || this.endsSection(word)) return false;
    if (ROUTINE.has(word) || word === "generic" || word === "class" || word === "record" || word === "object") return false;
    if (this.startsFreshStatement(index)) return false;
    return true;
  }

  /** `begin`/`end` around a case arm. An empty arm stays as `;`, which is what `begin end` means. */
  unwrapArm(indent: number, bodies: boolean, emptyAsSemi = false): void {
    this.i++;
    const mark = this.out.length;
    this.parseSequence(indent, new Set(["end"]), bodies);
    this.consumeEnd();
    const codeAdded = this.out.slice(mark).some((line) => {
      const text = line.trim();
      return text !== "" && !text.startsWith("//") && !text.startsWith("{") && !text.startsWith("(*");
    });
    if (emptyAsSemi && !codeAdded) this.line(indent, ";");
  }

  /** `1`, `Red`, `1..3`, `'a', 'b'` sitting in front of a colon, not a statement. */
  looksLikeCaseLabel(): boolean {
    const colon = this.findToken(":", 0);
    if (colon < 0 || colon === this.i) return false;
    for (let k = this.i; k < colon; k++) {
      const tok = this.code[k];
      if (!tok) return false;
      if (tok.kind === "ident" || tok.kind === "number" || tok.kind === "string") continue;
      if (tok.text === "," || tok.text === ".." || tok.text === "-" || tok.text === "+") continue;
      return false;
    }
    return true;
  }

  /**
   * The header cue. An `{$IFDEF}` branch can contain its own `then`/`do` before the
   * real one; keep scanning while the gap is still just the expression.
   */
  findCue(word: string): number {
    const first = this.findWord(word);
    if (first < 0) return -1;
    let paren = 0;
    let bracket = 0;
    let later = first;
    for (let k = first + 1; k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) break;
      const w = tok.text.toLowerCase();
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0) {
        if (w === word) {
          later = k;
          continue;
        }
        if (
          tok.text === ";" ||
          w === "begin" || w === "end" || w === "else" || w === "otherwise" ||
          w === "do" || w === "then" ||
          ["if", "for", "while", "with", "repeat", "case", "try", "asm", "on"].includes(w)
        ) break;
      }
    }
    return later;
  }

  findWord(word: string): number {
    let paren = 0;
    let bracket = 0;
    for (let k = this.i; k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) break;
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0 && tok.text.toLowerCase() === word) return k;
      else if (paren === 0 && bracket === 0 && tok.text === ";") return -1;
    }
    return -1;
  }

  findToken(text: string, fromParen: number): number {
    let paren = fromParen;
    let bracket = 0;
    for (let k = this.i; k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) break;
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0 && tok.text === text) return k;
      else if (paren === 0 && bracket === 0 && tok.text === ";") return -1;
    }
    return -1;
  }

  skipGroup(open: string, close: string): void {
    if (this.curText() !== open) return;
    let depth = 0;
    while (this.i < this.code.length) {
      const text = this.curText();
      if (text === open) depth++;
      else if (text === close) {
        depth--;
        this.i++;
        if (depth === 0) return;
        continue;
      }
      this.i++;
    }
  }

  consumeEnd(): void {
    if (!this.at("end")) return;
    this.i++;
    if (this.curText() === "." || this.curText() === ";") this.i++;
  }

  flat(from: number, to: number, trail = true): string {
    if (from >= to || from < 0 || from >= this.code.length) return "";
    const endPos = this.code[to]?.start ?? this.source.length;
    const parts: string[] = [];
    for (let k = from; k < to && k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) continue;
      if (k > from) {
        const prev = this.code[k - 1];
        if (prev) parts.push(...this.inlineDirectives(prev.end, tok.start));
      }
      parts.push(tok.text);
    }
    const last = this.code[Math.min(to, this.code.length) - 1];
    if (trail && last) parts.push(...this.inlineDirectives(last.end, endPos));
    return joinPascal(parts);
  }

  /** A `{$...}` between two tokens of one statement stays in that statement. */
  inlineDirectives(fromPos: number, toPos: number): string[] {
    this.pullUntil(fromPos);
    const inline: string[] = [];
    while (this.trivia < this.tokens.length) {
      const item = this.tokens[this.trivia];
      if (!item || item.start >= toPos) break;
      if (item.start < fromPos) {
        this.trivia++;
        continue;
      }
      this.trivia++;
      if (item.kind === "directive") inline.push(item.text.trim());
      else if (item.kind === "comment") this.triviaLines.push(item.text.replace(/[ \t]+$/g, "").trim());
    }
    if (toPos > this.mark) this.mark = toPos;
    return inline;
  }

  pull(): void {
    const tok = this.code[this.i];
    this.pullUntil(tok ? tok.start : this.source.length);
  }

  /** Queue comments and directives before `until` so a later line can indent them. */
  pullUntil(until: number): void {
    if (until < this.mark) return;
    let cursor = this.mark;
    const pushGap = (gap: string): void => {
      if (/\n[ \t]*\n/.test(gap)) this.triviaLines.push("");
    };
    while (this.trivia < this.tokens.length) {
      const item = this.tokens[this.trivia];
      if (!item || item.start >= until) break;
      this.trivia++;
      if (item.start < this.mark) continue;
      if (item.kind !== "comment" && item.kind !== "directive") continue;
      pushGap(this.source.slice(cursor, item.start));
      this.triviaLines.push(item.text.replace(/[ \t]+$/g, "").trim());
      cursor = item.end;
    }
    pushGap(this.source.slice(cursor, until));
    this.mark = until;
  }

  flushTrivia(indent: number): void {
    const pad = " ".repeat(indent);
    for (const text of this.triviaLines) {
      if (!text) {
        if (this.out.at(-1) !== "") this.out.push("");
        continue;
      }
      this.out.push(`${pad}${text}`);
    }
    this.triviaLines.length = 0;
  }

  flushPending(indent: number): void {
    const pad = " ".repeat(indent);
    for (const text of this.pendingAfter) this.out.push(`${pad}${text}`);
    this.pendingAfter.length = 0;
  }

  /** `{$ELSE}`/`{$ENDIF}` with no `{$IF}` open in this statement ends it. An `{$IF}` in the gap before the statement counts as open, so `stdcall{$ELSE}cdecl` stays one header. */
  statementBoundary(from: number, index: number): boolean {
    const tok = this.code[index];
    if (!tok || (tok.kind !== "ident" && tok.kind !== "keyword")) return false;
    const word = tok.text.toLowerCase();
    if (["or", "and", "xor", "div", "mod", "shl", "shr", "not", "in", "is", "as", "to", "downto"].includes(word)) return false;
    // `Result := Null {$else} Result := ''` is two arms. `stdcall{$else}cdecl` and `:= {$else} 2` are not.
    if (this.finishedAssignment(from, index) && this.startsFreshStatement(index) && this.armSplitBefore(index)) return true;
    const start = this.code[from]?.start ?? 0;
    const prevEnd = this.code[from - 1]?.end ?? 0;
    let depth = 0;
    for (const item of this.tokens) {
      if (item.start < prevEnd) continue;
      if (item.start >= start) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) depth++;
      else if (/^\{\$\s*(endif|ifend)\b/.test(text)) depth = Math.max(0, depth - 1);
    }
    let boundary = false;
    for (const item of this.tokens) {
      if (item.start < start) continue;
      if (item.start >= tok.start) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) depth++;
      else if (/^\{\$\s*(endif|ifend)\b/.test(text)) {
        if (depth === 0) boundary = true;
        else depth--;
      } else if (/^\{\$\s*(else|elseif)\b/.test(text) && depth === 0) boundary = true;
    }
    return boundary;
  }

  /** The tokens so far are an assignment whose right-hand side is complete. */
  finishedAssignment(from: number, index: number): boolean {
    let paren = 0;
    let bracket = 0;
    let sawAssign = false;
    for (let k = from; k < index; k++) {
      const tok = this.code[k];
      if (!tok) continue;
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0 && tok.text === ":=") sawAssign = true;
    }
    if (!sawAssign || paren !== 0 || bracket !== 0) return false;
    const last = this.code[index - 1];
    if (!last) return false;
    const op = new Set([
      ":=", "=", "<", ">", "<=", ">=", "<>", "+", "-", "*", "/", ".", "^", ",", "(", "[",
      "and", "or", "xor", "div", "mod", "shl", "shr", "not", "in", "is", "as", "to", "downto",
    ]);
    return !op.has(last.text.toLowerCase());
  }

  /** `Name :=` or `Name(` — a new statement, not the other arm of one expression. */
  startsFreshStatement(index: number): boolean {
    const tok = this.code[index];
    if (!tok || (tok.kind !== "ident" && tok.kind !== "keyword")) return false;
    const word = tok.text.toLowerCase();
    if (["or", "and", "xor", "div", "mod", "shl", "shr", "not", "in", "is", "as", "to", "downto"].includes(word)) return false;
    const next = this.code[index + 1];
    return next?.text === ":=" || next?.text === "(";
  }

  /** `{$else}` / `{$endif}` sits between this statement and the next code token. */
  armSplitBefore(index: number): boolean {
    const prev = this.code[index - 1];
    const tok = this.code[index];
    if (!prev || !tok) return false;
    for (const item of this.tokens) {
      if (item.start < prev.end) continue;
      if (item.start >= tok.start) break;
      if (item.kind !== "directive") continue;
      if (/^\{\$\s*(else|elseif|endif|ifend)\b/i.test(item.text.trim())) return true;
    }
    return false;
  }

  /** Parked `{$else}` continues this body only when its `{$if}` opened after the cue. An outer `{$else}` stays with the surrounding conditional. */
  pendingContinuesArm(cueEnd: number): boolean {
    if (!this.pendingAfter.some((text) => /^\{\$\s*(else|elseif)\b/i.test(text.trim()))) return false;
    return this.openBodyCond(cueEnd, this.mark);
  }

  /** An `{$if}` opened after `then`/`do` is still open at `toPos`. */
  openBodyCond(fromPos: number, toPos: number): boolean {
    let depth = 0;
    for (const item of this.tokens) {
      if (item.start < fromPos) continue;
      if (item.start >= toPos) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) depth++;
      else if (/^\{\$\s*(endif|ifend)\b/.test(text) && depth > 0) depth--;
    }
    return depth > 0;
  }

  /** The next directive is the other arm of a conditional that opened in this body. */
  elseArmAhead(cueEnd: number): boolean {
    if (!this.openBodyCond(cueEnd, this.mark)) return false;
    const next = this.code[this.i];
    const limit = next?.start ?? this.source.length;
    for (const item of this.tokens) {
      if (item.start < this.mark) continue;
      if (item.start >= limit) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(else|elseif)\b/.test(text)) return true;
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) return false;
    }
    return false;
  }

  /** An `{$if}` opened after `cueEnd` is closed before the next code token. */
  bodyCondCloses(cueEnd: number): boolean {
    return this.bodyCloseAt(cueEnd) >= 0;
  }

  /** End offset of the `{$endif}` that closes the conditional opened after `cueEnd`. */
  bodyCloseAt(cueEnd: number): number {
    const next = this.code[this.i];
    const limit = next?.start ?? this.source.length;
    let depth = 0;
    let closeAt = -1;
    for (const item of this.tokens) {
      if (item.start < cueEnd) continue;
      if (item.start >= limit) break;
      if (item.kind !== "directive") continue;
      const text = item.text.trim().toLowerCase();
      if (/^\{\$\s*(if|ifdef|ifndef|ifopt|ifc)\b/.test(text)) depth++;
      else if (/^\{\$\s*(endif|ifend)\b/.test(text) && depth > 0) {
        depth--;
        if (depth === 0) closeAt = item.end;
      }
    }
    return closeAt;
  }

  /** Pull the `{$endif}` that closes the body conditional, so it stays inside the body. An `{$endif}` that closes an older conditional stays outside. */
  pullBodyTail(cueEnd: number, indent: number): void {
    const closeAt = this.bodyCloseAt(cueEnd);
    if (closeAt < 0) return;
    this.flushPendingThrough(cueEnd, closeAt, indent);
    this.pullUntil(closeAt);
    this.flushTrivia(indent);
  }

  /** Parked directives keep source order. Emit only those that end with the body conditional. */
  flushPendingThrough(cueEnd: number, closeAt: number, indent: number): void {
    if (this.pendingAfter.length === 0) return;
    let used = 0;
    for (const item of this.tokens) {
      if (used >= this.pendingAfter.length) break;
      if (item.start < cueEnd) continue;
      if (item.end > closeAt) break;
      if (item.kind !== "directive" && item.kind !== "comment") continue;
      const text = item.text.replace(/[ \t]+$/g, "").trim();
      if (this.pendingAfter[used] === text) used++;
    }
    if (used === 0) return;
    const pad = " ".repeat(indent);
    for (const text of this.pendingAfter.slice(0, used)) this.out.push(`${pad}${text}`);
    this.pendingAfter.splice(0, used);
  }

  /** Keep a branch directive after the statement so it is not glued into the statement. The next line indents it. */
  parkBoundary(index: number): void {
    const tok = this.code[index];
    const prev = this.code[index - 1];
    if (!tok || !prev) return;
    this.pullUntil(prev.end);
    while (this.trivia < this.tokens.length) {
      const item = this.tokens[this.trivia];
      if (!item || item.start >= tok.start) break;
      this.trivia++;
      if (item.start < prev.end) continue;
      if (item.kind !== "comment" && item.kind !== "directive") continue;
      this.afterLines.push({ col: item.col, text: item.text.replace(/[ \t]+$/g, "").trim() });
    }
    this.mark = Math.max(this.mark, tok.start);
  }

  /**
   * A huge constructor (`array[0..$FFFF] of Byte = (25, // U+0000 ...)`) must
   * stay one element per line. Flattening it makes a line the host compiler
   * cannot parse, and parking every comment above it doubles the line count.
   */
  spreadInit(indent: number, text: string): boolean {
    const open = text.indexOf("(");
    const close = text.lastIndexOf(")");
    if (open < 0 || close <= open) return false;
    const parts = splitTopCommas(text.slice(open + 1, close)).map((part) => part.trim()).filter(Boolean);
    if (parts.length < 32) return false;
    const extra = this.triviaLines.length - parts.length;
    if (extra < 0 || extra > 8) return false;
    const headers = this.triviaLines.splice(0, extra);
    const notes = this.triviaLines.splice(0, parts.length);
    const mine = this.afterLines.map((item) => item.text);
    this.afterLines.length = 0;
    this.flushPending(indent);
    const pad = " ".repeat(indent);
    const inner = " ".repeat(indent + 4);
    for (const header of headers) this.out.push(`${pad}${header}`);
    const head = text.slice(0, open + 1).replace(/\s+/g, " ").trim();
    this.out.push(`${pad}${head}`);
    parts.forEach((part, i) => {
      const comma = i < parts.length - 1 ? "," : "";
      const note = notes[i] ? ` ${notes[i]}` : "";
      this.out.push(`${inner}${part}${comma}${note}`);
    });
    this.out.push(`${pad})`);
    this.pendingAfter.push(...mine);
    return true;
  }

  line(indent: number, text: string): void {
    const trimmed = text.replace(/\s+/g, " ").trim();
    const mine = this.afterLines.map((item) => item.text);
    this.afterLines.length = 0;
    this.flushPending(indent);
    if (!trimmed) {
      this.flushTrivia(indent);
      for (const item of mine) this.out.push(`${" ".repeat(indent)}${item}`);
      return;
    }
    this.flushTrivia(indent);
    this.out.push(`${" ".repeat(indent)}${trimmed}`);
    this.pendingAfter.push(...mine);
    this.pendingEpoch++;
  }

  at(word: string): boolean {
    return this.word() === word;
  }

  word(): string {
    return this.wordAt(this.i);
  }

  wordAt(index: number): string {
    return this.code[index]?.text.toLowerCase() ?? "";
  }

  curText(): string {
    return this.code[this.i]?.text ?? "";
  }

  lineNo(): number {
    return this.code[this.i]?.line ?? 1;
  }
}

function finish(lines: string[]): string {
  const out: string[] = [];
  for (const line of lines) {
    if (line.trim() === "" && (out.length === 0 || out[out.length - 1] === "")) continue;
    out.push(line.replace(/[ \t]+$/g, ""));
  }
  while (out.at(-1) === "") out.pop();
  if (out.length === 0) return "";
  return `${out.join("\n")}\n`;
}
