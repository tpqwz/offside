import { lex } from "./lex.ts";
import { PROFILES } from "./languages.ts";
import { toPascalSpaced } from "./pascal.ts";
import type { LangId, LayoutNote, Profile, Tok } from "./types.ts";

const CONTROL = new Set(["if", "while", "for", "switch", "catch", "synchronized"]);
const AGG = new Set(["struct", "union", "enum", "class", "interface", "record"]);
const KR_DECL = new Set([
  "int", "char", "short", "long", "float", "double", "void", "signed", "unsigned",
  "const", "volatile", "restrict", "register", "static", "extern", "auto",
  "struct", "union", "enum", "inline", "_Bool", "_Complex", "_Imaginary",
]);
const HEADER_WORD = /\b(else|do|struct|union|enum|class|interface|record|try|finally)\b/;

type Node =
  | { type: "text"; text: string }
  | { type: "blank" }
  | { type: "raw"; text: string }
  | { type: "comment"; text: string }
  | { type: "keep"; body: Node[] }
  | { type: "sexpr"; prefix: string; body: Node[]; suffix: string }
  | { type: "colon"; header: string; where: "inline" | "own" | "ready"; body: Node[]; lead?: Node[]; tail?: string; follow?: string }
  | { type: "group"; nodes: Node[] };

interface Parsed {
  node: Node;
  next: number;
  end: number;
}

/**
 * Host C, Java, or JavaScript back into the spaced dialect.
 * Statement braces become a trailing colon and an indent. Semicolons the
 * layout pass would put back are removed. Initializers, statement
 * expressions, and bare blocks keep their braces.
 */
export function toSpaced(source: string, lang: LangId): { code: string; notes: LayoutNote[] } {
  if (lang === "pascal") return toPascalSpaced(source);
  const profile = PROFILES[lang];
  const normalized = source.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
  return new Converter(normalized, lang, profile).run();
}

class Converter {
  readonly source: string;
  readonly lang: LangId;
  readonly code: Tok[];
  readonly notes: LayoutNote[];
  readonly tokens: Tok[];
  mark = 0;
  /** Next token to consider in {@link pullTrivia}. Mark only moves forward. */
  private triviaAt = 0;

  constructor(source: string, lang: LangId, profile: Profile) {
    this.source = source;
    this.lang = lang;
    const lexed = lex(source, profile);
    this.notes = lexed.notes;
    this.tokens = lexed.tokens;
    this.code = lexed.tokens.filter((tok) => tok.kind !== "comment" && tok.kind !== "directive");
  }

  run(): { code: string; notes: LayoutNote[] } {
    const seq = this.parseSeq(0, false);
    const trailing = this.pullTrivia(this.source.length);
    const lines = [...seq.nodes, ...trailing].flatMap((node) => this.render(node, 0));
    return { code: finish(lines), notes: this.notes };
  }

  parseSeq(i: number, stopBrace: boolean): { nodes: Node[]; next: number } {
    const nodes: Node[] = [];
    while (i < this.code.length) {
      const tok = this.code[i];
      if (!tok) break;
      const span = this.spanCovering(tok.start);
      if (span && !(stopBrace && tok.text === "}" && tok.start >= span.end)) {
        if (this.mark < span.start) nodes.push(...this.pullTrivia(span.start));
        const chunk = this.source.slice(Math.max(span.start, this.mark), span.end).replace(/[ \t]+$/gm, "");
        if (chunk.trim()) nodes.push({ type: "text", text: chunk });
        this.mark = Math.max(this.mark, span.end);
        while (i < this.code.length && (this.code[i]?.start ?? 0) < span.end) i++;
        continue;
      }
      if (stopBrace && tok.text === "}") {
        nodes.push(...this.pullTrivia(tok.start));
        break;
      }
      nodes.push(...this.pullTrivia(tok.start));
      const parsed = this.parseStmt(i);
      if (parsed.next <= i) {
        this.notes.push({ line: tok.line, message: "Skipped a statement the reverse pass could not read." });
        this.mark = Math.max(this.mark, tok.end);
        i++;
        continue;
      }
      if (!isEmpty(parsed.node)) nodes.push(parsed.node);
      this.mark = Math.max(this.mark, parsed.end);
      i = parsed.next;
    }
    return { nodes, next: i };
  }

  parseStmt(i: number): Parsed {
    const tok = this.code[i];
    if (!tok) return { node: { type: "text", text: "" }, next: i, end: this.mark };
    if (tok.text === "case" || tok.text === "default") return this.parseCase(i);
    if (this.isLabel(i)) return this.parseLabel(i);
    if (CONTROL.has(tok.text)) return this.parseControl(i);
    if (tok.text === "do") return this.parseDo(i);
    if (tok.text === "try") return this.parseTry(i);
    if (tok.text === "else") return this.parseElse(i);
    if (tok.text === "finally") return this.parseFinally(i);
    if (tok.text === ";") {
      const prev = this.code[i - 1];
      if (prev && this.hasDirectiveBetween(prev.end, tok.start)) {
        return { node: { type: "text", text: ";" }, next: i + 1, end: tok.end };
      }
      return { node: { type: "text", text: "" }, next: i + 1, end: this.offsetAfter(i + 1) };
    }
    if (tok.text === "{") return this.parseOpenBrace(i);
    return this.parseGeneral(i);
  }

  parseLabel(i: number): Parsed {
    const tok = this.code[i]!;
    const text = `${tok.text}:`;
    const trail = this.commentOnLineAfter(this.code[i + 1] ?? tok);
    const shown = trail && trail.start < (this.code[i + 2]?.start ?? Infinity) ? `${text} ${trail.text}` : text;
    const end = trail ? trail.end : this.code[i + 1]!.end;
    return { node: { type: "text", text: shown }, next: i + 2, end };
  }

  parseCase(i: number): Parsed {
    const colon = this.findCaseColon(i);
    if (colon < 0) {
      return { node: { type: "text", text: this.code[i]!.text }, next: i + 1, end: this.offsetAfter(i + 1) };
    }
    const header = this.headerText(i, colon + 1);
    const colonTok = this.code[colon]!;
    const glued = this.commentOnLineAfter(colonTok);
    this.mark = Math.max(this.mark, glued && glued.start < (this.code[colon + 1]?.start ?? Infinity) ? glued.end : colonTok.end);
    const nodes: Node[] = [];
    let after = colon + 1;
    if (this.code[after]?.text === "{") {
      nodes.push(...this.leadIn(after));
      const inner = this.parseSeq(after + 1, true);
      const close = this.code[inner.next];
      if (close?.text === "}") {
        after = inner.next + 1;
        this.mark = Math.max(this.mark, close.end);
      } else after = inner.next;
      nodes.push({ type: "keep", body: inner.nodes });
    }
    while (after < this.code.length) {
      const tok = this.code[after];
      if (!tok || tok.text === "}" || tok.text === "case" || tok.text === "default") break;
      nodes.push(...this.pullTrivia(tok.start));
      const one = this.parseStmt(after);
      if (one.next <= after) break;
      if (!isEmpty(one.node)) nodes.push(one.node);
      this.mark = Math.max(this.mark, one.end);
      after = one.next;
    }
    return {
      node: { type: "colon", header, where: "ready", body: nodes },
      next: after,
      end: this.offsetAfter(after),
    };
  }

  /** A directive between a header and its brace stays outside the block, above a colon of its own. */
  headed(header: string, where: "inline" | "own", body: { nodes: Node[]; lead?: Node[] }, follow = ""): Node {
    const lead = body.lead ?? [];
    const split = lead.some((node) => node.type === "raw");
    return {
      type: "colon",
      header,
      where: split ? "own" : where,
      lead: split ? lead : undefined,
      body: split ? body.nodes : [...lead, ...body.nodes],
      follow,
    };
  }

  parseControl(i: number): Parsed {
    const keyword = this.code[i]!.text;
    const open = this.findParen(i);
    if (open < 0) return { node: { type: "text", text: keyword }, next: i + 1, end: this.offsetAfter(i + 1) };
    const close = this.match(open, "(", ")");
    if (close < 0) {
      this.notes.push({ line: this.code[i]!.line, message: `Unclosed ( after ${keyword}.` });
      return { node: { type: "text", text: keyword }, next: i + 1, end: this.offsetAfter(i + 1) };
    }
    const header = this.headerText(i, close + 1);
    const nextTok = this.code[close + 1];
    if (
      nextTok &&
      nextTok.text !== "{" &&
      nextTok.text !== ";" &&
      this.hasDirectiveBetween(this.code[close]!.end, nextTok.start)
    ) {
      const stmt = this.parseStmt(close + 1);
      const text = this.source.slice(this.code[i]!.start, this.offsetAfter(stmt.next)).replace(/[ \t]+$/gm, "");
      return { node: { type: "text", text }, next: stmt.next, end: stmt.end };
    }
    const body = this.parseBody(close + 1);
    const node = this.headed(header, "inline", body);
    const elseAt = this.code[body.next];
    if (keyword !== "if" || elseAt?.text !== "else" || this.spanCovering(elseAt.start)) {
      return { node, next: body.next, end: body.end };
    }
    const between = this.pullTrivia(this.code[body.next]!.start);
    const other = this.parseElse(body.next);
    const nodes = between.length > 0 ? [node, ...between, ...flatten(other.node)] : [node, ...flatten(other.node)];
    return { node: { type: "group", nodes }, next: other.next, end: other.end };
  }

  parseElse(i: number): Parsed {
    const next = this.code[i + 1];
    const between = next ? this.pullTrivia(next.start) : [];
    const split = between.some((node) => node.type === "raw");
    if (next?.text === "if") {
      const inner = this.parseControl(i + 1);
      if (split) {
        return {
          node: { type: "group", nodes: [{ type: "text", text: "else" }, ...between, ...flatten(inner.node)] },
          next: inner.next,
          end: inner.end,
        };
      }
      return { ...inner, node: prefixHeader(inner.node, "else ") };
    }
    if (split && next) {
      const stmt = this.parseStmt(i + 1);
      return {
        node: { type: "group", nodes: [{ type: "text", text: "else" }, ...between, ...flatten(stmt.node)] },
        next: stmt.next,
        end: stmt.end,
      };
    }
    const body = this.parseBody(i + 1);
    const node = this.headed("else", "inline", body);
    if (node.type === "colon" && between.length > 0) node.body = [...between, ...node.body];
    return { node, next: body.next, end: body.end };
  }

  parseDo(i: number): Parsed {
    const body = this.parseBody(i + 1);
    let next = body.next;
    let follow = "";
    let end = body.end;
    if (this.code[next]?.text === "while") {
      const open = this.findParen(next);
      const close = open < 0 ? -1 : this.match(open, "(", ")");
      if (close >= 0) {
        follow = this.stripSemis(next, close).trim();
        next = close + 1;
        if (this.code[next]?.text === ";") next++;
        end = this.offsetAfter(next);
      }
    }
    return {
      node: this.headed("do", "inline", body, follow),
      next,
      end,
    };
  }

  parseTry(i: number): Parsed {
    const body = this.parseBody(i + 1);
    const nodes: Node[] = [this.headed("try", "inline", body)];
    let next = body.next;
    let end = body.end;
    while (this.code[next]?.text === "catch" || this.code[next]?.text === "finally") {
      if (this.code[next]?.text === "finally") {
        const rest = this.parseFinally(next);
        nodes.push(...flatten(rest.node));
        next = rest.next;
        end = rest.end;
        continue;
      }
      const caught = this.parseControl(next);
      nodes.push(...flatten(caught.node));
      next = caught.next;
      end = caught.end;
    }
    return { node: nodes.length === 1 ? nodes[0]! : { type: "group", nodes }, next, end };
  }

  parseFinally(i: number): Parsed {
    const body = this.parseBody(i + 1);
    return { node: this.headed("finally", "inline", body), next: body.next, end: body.end };
  }

  parseBody(i: number): { nodes: Node[]; lead: Node[]; next: number; end: number } {
    if (i >= this.code.length) return { nodes: [], lead: [], next: i, end: this.mark };
    if (this.code[i]?.text === ";") return { nodes: [], lead: [], next: i + 1, end: this.offsetAfter(i + 1) };
    if (this.code[i]?.text === "{") {
      const lead = this.leadIn(i);
      const inner = this.parseSeq(i + 1, true);
      const close = this.code[inner.next];
      const next = close?.text === "}" ? inner.next + 1 : inner.next;
      return { nodes: inner.nodes, lead, next, end: this.offsetAfter(next) };
    }
    const one = this.parseStmt(i);
    return { nodes: isEmpty(one.node) ? [] : [one.node], lead: [], next: one.next, end: one.end };
  }

  parseOpenBrace(i: number): Parsed {
    const role = this.braceRole(i);
    if (role === "colon") return this.finishColon(i, i);
    const close = this.match(i, "{", "}");
    if (close < 0) {
      this.notes.push({ line: this.code[i]!.line, message: "Unclosed { in the host source." });
      return { node: { type: "text", text: "{" }, next: i + 1, end: this.offsetAfter(i + 1) };
    }
    if (role === "init") {
      const semi = this.scanSemi(close + 1);
      const to = semi < 0 ? close : semi;
      const text = this.stripSemis(i, to);
      const next = semi < 0 ? close + 1 : semi + 1;
      return { node: { type: "text", text }, next, end: this.offsetAfter(next) };
    }
    this.mark = Math.max(this.mark, this.code[i]!.end);
    const inner = this.parseSeq(i + 1, true);
    const next = this.code[inner.next]?.text === "}" ? inner.next + 1 : close + 1;
    return { node: { type: "keep", body: inner.nodes }, next, end: this.offsetAfter(next) };
  }

  parseGeneral(i: number): Parsed {
    let j = i;
    let paren = 0;
    let bracket = 0;
    let sawEq = false;
    let fnClose = -1;
    const items: Mix[] = [];
    let chunk = i;

    const pushText = (to: number): void => {
      if (to < chunk) return;
      const text = this.stripSemis(chunk, to);
      if (text.trim()) items.push({ kind: "text", text });
    };

    while (j < this.code.length) {
      const tok = this.code[j];
      if (!tok) break;
      if (tok.text === "}" && paren === 0 && bracket === 0) break;
      if ((tok.text === "case" || tok.text === "default") && paren === 0 && bracket === 0 && j > i) break;
      /* A statement macro such as IOTRACE(A) carries its own semicolon. The
         next if/for/while is a new statement; absorbing its brace turns the
         macro into a header and drops the body out of the block. */
      if (
        j > i &&
        paren === 0 &&
        bracket === 0 &&
        (CONTROL.has(tok.text) || tok.text === "else" || tok.text === "do" || tok.text === "try" || tok.text === "finally")
      ) {
        break;
      }

      if (this.lang === "c" && tok.text === "{" && this.code[j - 1]?.text === "(") {
        pushText(j - 2);
        const open = tok;
        this.mark = Math.max(this.mark, open.end);
        const inner = this.parseSeq(j + 1, true);
        let after = this.code[inner.next]?.text === "}" ? inner.next + 1 : inner.next;
        let suffix = "";
        if (this.code[after]?.text === ")") {
          suffix = this.inlineAfter(after + 1);
          const semi = this.scanSemi(after + 1);
          if (suffix && semi >= 0) {
            /* suffix already the inline tail; consume through its tokens below */
          }
          after++;
        }
        items.push({ kind: "sexpr", body: inner.nodes });
        if (suffix.trim()) items.push({ kind: "text", text: suffix });
        const semi = this.scanSemi(after);
        if (semi >= 0 && this.onlyTrivia(after, semi)) {
          j = semi + 1;
          chunk = j;
          break;
        }
        j = after;
        chunk = j;
        continue;
      }

      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) {
        paren--;
        if (paren === 0 && bracket === 0 && fnClose < 0) fnClose = j;
      } else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (tok.text === "=" && paren === 0 && bracket === 0) sawEq = true;
      else if (tok.text === "{" && paren === 0 && bracket === 0) {
        const role = this.braceRole(j);
        const krBody = !sawEq && fnClose >= 0 && j > fnClose + 1 && this.krBody(fnClose, j);
        if (role === "colon" || krBody) {
          if (items.length === 0) return this.finishColon(i, j);
          const colon = this.finishColon(i, j);
          return {
            node: { type: "group", nodes: [...this.mixNodes(items), colon.node] },
            next: colon.next,
            end: colon.end,
          };
        }
        const close = this.match(j, "{", "}");
        if (close < 0) {
          this.notes.push({ line: tok.line, message: "Unclosed { in the host source." });
          break;
        }
        j = close + 1;
        continue;
      } else if (tok.text === ";" && paren === 0 && bracket === 0) {
        if (!sawEq && fnClose >= 0 && j > fnClose + 1 && this.krDecl(fnClose, j) && this.krContinues(j)) {
          j++;
          continue;
        }
        pushText(j - 1);
        const prev = this.code[j - 1];
        if (prev && this.hasDirectiveBetween(prev.end, tok.start)) {
          const last = items[items.length - 1];
          if (last && last.kind === "text") last.text = `${last.text.replace(/\s+$/, "")}\n;`;
          else items.push({ kind: "text", text: ";" });
        }
        const nodes = this.mixNodes(items);
        const node = nodes.length === 1 ? nodes[0]! : { type: "group" as const, nodes };
        return { node, next: j + 1, end: this.offsetAfter(j + 1) };
      }
      j++;
    }

    pushText(j - 1);
    const node = this.mixNodes(items);
    return { node: node.length === 1 ? node[0]! : { type: "group", nodes: node }, next: j, end: this.offsetAfter(j) };
  }

  finishColon(start: number, brace: number): Parsed {
    const close = this.match(brace, "{", "}");
    if (close < 0) {
      this.notes.push({ line: this.code[brace]!.line, message: "Unclosed { in the host source." });
      return { node: { type: "text", text: this.stripSemis(start, brace) }, next: brace + 1, end: this.offsetAfter(brace + 1) };
    }
    const header = this.headerText(start, brace);
    const isEnum = this.hasKeyword(start, brace, "enum");
    const lead = this.leadIn(brace);
    let body: Node[] = [];
    if (isEnum) {
      body = [...lead, ...this.enumItems(brace, close)];
    } else {
      body = [...lead, ...this.parseSeq(brace + 1, true).nodes];
    }
    let next = close + 1;
    const aggregate = this.lang === "c" && this.hasAggregate(start, brace);
    let tail = "";
    if (aggregate && this.code[next] && this.code[next]!.text !== ";" && this.code[next]!.text !== "}") {
      const semi = this.scanSemi(next);
      if (semi >= 0) {
        tail = this.stripSemis(next, semi - 1);
        next = semi + 1;
      }
    } else if (aggregate && this.code[next]?.text === ";") {
      next++;
    }
    const where = this.colonWhere(brace, header);
    let node: Node = { type: "colon", header, where, body };
    if (tail.trim()) node = this.reshapeAggregate(start, brace, node, tail);
    return { node, next, end: this.offsetAfter(next) };
  }

  enumItems(brace: number, close: number): Node[] {
    const items: Node[] = [];
    let start = brace + 1;
    let paren = 0;
    let bracket = 0;
    let nest = 0;
    const push = (from: number, to: number): void => {
      if (from > to) return;
      const text = this.flat(from, to).trim();
      if (text) items.push({ type: "text", text });
    };
    for (let k = brace + 1; k < close; k++) {
      const tok = this.code[k];
      if (!tok) break;
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (tok.text === "{") nest++;
      else if (tok.text === "}" && nest > 0) nest--;
      else if (tok.text === "," && paren === 0 && bracket === 0 && nest === 0) {
        push(start, k - 1);
        start = k + 1;
      }
    }
    push(start, close - 1);
    return items;
  }

  reshapeAggregate(start: number, brace: number, node: Node, tail: string): Node {
    if (node.type !== "colon") return node;
    const simple = this.tailIsSimple(tail);
    if (this.isAttributeTail(tail)) return { ...node, tail: tail.trim() };
    if (simple) return { ...node, tail: tail.trim() };
    this.notes.push({
      line: this.code[start]!.line,
      message: "Left this aggregate braced. A declarator with an initializer cannot follow a colon.",
    });
    const semi = this.scanSemi(brace);
    const end = semi < 0 ? this.match(brace, "{", "}") : semi;
    return { type: "text", text: this.source.slice(this.code[start]!.start, this.code[end]!.end) };
  }

  mixNodes(items: Mix[]): Node[] {
    if (items.length === 0) return [];
    if (items.length === 1 && items[0]?.kind === "text") return [{ type: "text", text: items[0].text }];
    const nodes: Node[] = [];
    let prefix = "";
    for (let n = 0; n < items.length; n++) {
      const item = items[n];
      if (!item) continue;
      if (item.kind === "text") {
        if (items[n + 1]?.kind === "sexpr") prefix = item.text;
        else if (prefix && nodes.at(-1)?.type === "sexpr") {
          const prev = nodes[nodes.length - 1];
          if (prev && prev.type === "sexpr") prev.suffix = item.text.trim();
          prefix = "";
        } else nodes.push({ type: "text", text: item.text });
      } else {
        nodes.push({ type: "sexpr", prefix: prefix.trim(), body: item.body, suffix: "" });
        prefix = "";
      }
    }
    return nodes;
  }

  render(node: Node, indent: number): string[] {
    switch (node.type) {
      case "blank":
        return [""];
      case "raw":
        return reindent(node.text, indent);
      case "comment":
        return reindent(node.text, indent);
      case "text":
        return reindent(node.text, indent);
      case "group":
        return node.nodes.flatMap((child) => this.render(child, indent));
      case "keep":
        return [
          `${" ".repeat(indent)}{`,
          ...node.body.flatMap((child) => this.render(child, indent + 4)),
          `${" ".repeat(indent)}}`,
        ];
      case "sexpr": {
        const pre = reindent(node.prefix, indent);
        if (pre.length === 0) pre.push(" ".repeat(indent));
        pre[pre.length - 1] = `${pre[pre.length - 1]!.replace(/[ \t]+$/, "")} ({`;
        const close = `${" ".repeat(indent)}})${node.suffix ? ` ${node.suffix.trim()}` : ""}`;
        return [...pre, ...node.body.flatMap((child) => this.render(child, indent + 4)), close];
      }
      case "colon": {
        const lines = reindent(node.header, indent, "first");
        if (lines.length === 0) lines.push(" ".repeat(indent));
        const lead = (node.lead ?? []).flatMap((child) => this.render(child, indent));
        if (lead.length === 0 && node.where === "inline") lines[lines.length - 1] = `${lines[lines.length - 1]!.replace(/[ \t]+$/, "")}:`;
        else if (lead.length > 0 || node.where === "own") lines.push(...lead, `${" ".repeat(indent)}:`);
        const body = node.body.flatMap((child) => this.render(child, indent + 4));
        const tail = node.tail ? reindent(node.tail, indent) : [];
        const follow = node.follow ? reindent(node.follow, indent) : [];
        return [...lines, ...body, ...tail, ...follow];
      }
      default:
        return [];
    }
  }

  pullTrivia(until: number): Node[] {
    if (until < this.mark) return [];
    const nodes: Node[] = [];
    while (this.triviaAt < this.tokens.length && this.tokens[this.triviaAt]!.start < this.mark) this.triviaAt++;
    let cursor = this.mark;
    const pushGap = (gap: string): void => {
      if (/\n[ \t]*\n/.test(gap)) nodes.push({ type: "blank" });
    };
    while (this.triviaAt < this.tokens.length && this.tokens[this.triviaAt]!.start < until) {
      const tok = this.tokens[this.triviaAt]!;
      this.triviaAt++;
      if (tok.kind !== "comment" && tok.kind !== "directive") continue;
      pushGap(this.source.slice(cursor, tok.start));
      if (tok.kind === "directive") nodes.push({ type: "raw", text: tok.text.replace(/[ \t]+$/g, "") });
      else nodes.push({ type: "comment", text: tok.text });
      cursor = tok.end;
    }
    pushGap(this.source.slice(cursor, until));
    this.mark = until;
    return nodes;
  }

  braceRole(index: number): "init" | "expr" | "colon" | "keep" {
    const prev = this.code[index - 1];
    if (!prev) return "keep";
    if (prev.text === "=" || prev.text === "," || prev.text === "{") return "init";
    if (prev.text === "=>") return "colon";
    if (prev.text === "(") return "expr";
    if (prev.text === "]" && this.code[index - 2]?.text === "]") return "colon";
    if (prev.text === "else" || prev.text === "do" || prev.text === "try" || prev.text === "finally") return "colon";
    if (AGG.has(prev.text)) return "colon";
    if (prev.text === ")" ) {
      const open = this.matchBack(index - 1, ")", "(");
      const before = open > 0 ? this.code[open - 1] : undefined;
      if (before && (before.text === "__attribute__" || before.text === "__attribute" || before.text === "__declspec" || before.text === "alignas" || before.text === "_Alignas")) {
        return "colon";
      }
      if (!before || before.text === "=" || before.text === "," || before.text === "return" || before.text === "(") return "init";
      if (before.text === "]" || AGG.has(before.text)) return "init";
      if (before.kind === "keyword" && !CONTROL.has(before.text) && before.text !== "else" && before.text !== "do") return "init";
      return "colon";
    }
    if (this.looksLikeAggregate(index)) return "colon";
    return "keep";
  }

  looksLikeAggregate(brace: number): boolean {
    let paren = 0;
    let bracket = 0;
    for (let k = brace - 1; k >= 0; k--) {
      const tok = this.code[k];
      if (!tok) continue;
      if (tok.text === ")") paren++;
      else if (tok.text === "(") {
        if (paren === 0) return false;
        paren--;
      } else if (tok.text === "]") bracket++;
      else if (tok.text === "[") {
        if (bracket === 0) return false;
        bracket--;
      } else if (paren === 0 && bracket === 0) {
        if (tok.text === "=" || tok.text === ";" || tok.text === "{" || tok.text === "}") return false;
        if (AGG.has(tok.text)) return true;
      }
    }
    return false;
  }

  hasAggregate(from: number, brace: number): boolean {
    let paren = 0;
    let bracket = 0;
    let seen = false;
    for (let k = from; k < brace; k++) {
      const tok = this.code[k];
      if (!tok) continue;
      if (tok.text === "(") {
        if (paren === 0 && seen) return false;
        paren++;
      } else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0 && AGG.has(tok.text)) seen = true;
    }
    return seen;
  }

  hasKeyword(from: number, to: number, word: string): boolean {
    let paren = 0;
    let bracket = 0;
    for (let k = from; k < to; k++) {
      const tok = this.code[k];
      if (!tok) continue;
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0 && tok.text === word) return true;
    }
    return false;
  }

  aggregateHasTag(from: number, brace: number): boolean {
    let paren = 0;
    let bracket = 0;
    for (let k = from; k < brace; k++) {
      const tok = this.code[k];
      if (!tok) continue;
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (paren === 0 && bracket === 0 && AGG.has(tok.text) && this.code[k + 1]?.kind === "ident") return true;
    }
    return false;
  }

  tailIsSimple(tail: string): boolean {
    return !/[=?:.(){}]|->/.test(tail);
  }

  isAttributeTail(tail: string): boolean {
    const text = tail.trim();
    return text.startsWith("__attribute__") || text.startsWith("__attribute") || text.startsWith("__declspec") || text.startsWith("alignas") || text.startsWith("_Alignas") || text.startsWith("[[");
  }

  krBody(fnClose: number, brace: number): boolean {
    for (let k = fnClose + 1; k < brace; k++) {
      if (this.code[k]?.text === "=") return false;
    }
    return true;
  }

  /** Tokens between a `)` and a `;` are a K&R parameter declaration, not `call() + 2`. */
  krDecl(from: number, to: number): boolean {
    for (let k = from + 1; k < to; k++) {
      const tok = this.code[k];
      if (!tok) return false;
      if (tok.text === "(") return false;
      if (tok.kind === "ident" || tok.text === "*" || tok.text === "," || tok.text === ";" || tok.text === "[" || tok.text === "]") continue;
      if (KR_DECL.has(tok.text)) continue;
      return false;
    }
    return true;
  }

  /** A `{` follows, with only K&R parameter declarations between here and it. */
  krContinues(semi: number): boolean {
    let bracket = 0;
    for (let k = semi + 1; k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) break;
      if (tok.text === "(" || tok.text === ")" || tok.text === "=" || tok.text === "}") return false;
      if (tok.text === "[") bracket++;
      else if (tok.text === "]") {
        if (bracket === 0) return false;
        bracket--;
      } else if (tok.text === "{") return bracket === 0;
      else if (tok.text === ";" || tok.text === "*" || tok.text === "," || tok.kind === "ident" || KR_DECL.has(tok.text)) continue;
      else return false;
    }
    return false;
  }

  colonWhere(brace: number, header: string): "inline" | "own" {
    const prev = this.code[brace - 1];
    if (prev && (prev.text === ")" || prev.text === "=>")) return "inline";
    const last = header.split("\n").pop() ?? "";
    return HEADER_WORD.test(last) ? "inline" : "own";
  }

  isLabel(i: number): boolean {
    const tok = this.code[i];
    const colon = this.code[i + 1];
    return !!tok && tok.kind === "ident" && colon?.text === ":";
  }

  findCaseColon(i: number): number {
    let paren = 0;
    let bracket = 0;
    let ternary = 0;
    for (let k = i; k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) break;
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (tok.text === "?") ternary++;
      else if (tok.text === ":") {
        if (ternary > 0) ternary--;
        else if (paren === 0 && bracket === 0) return k;
      } else if (tok.text === ";" && paren === 0 && bracket === 0) return -1;
    }
    return -1;
  }

  findParen(i: number): number {
    for (let k = i; k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) break;
      if (tok.text === "(") return k;
      if (tok.text === ";" || tok.text === "{") return -1;
    }
    return -1;
  }

  /** `#if` / `#else` arms that each open a brace share one close. Copy that region verbatim. */
  private spans: { start: number; end: number }[] | null = null;

  directiveRole(text: string): "open" | "close" | "mid" | "other" {
    const body = text.trim().toLowerCase();
    if (/^#\s*(if|ifdef|ifndef)\b/.test(body)) return "open";
    if (/^#\s*endif\b/.test(body)) return "close";
    if (/^#\s*(else|elifdef|elifndef|elif)\b/.test(body)) return "mid";
    return "other";
  }

  if0(text: string): boolean {
    return /^#\s*if\s+0\b/.test(text.trim().toLowerCase());
  }

  elif0(text: string): boolean {
    return /^#\s*elif\s+0\b/.test(text.trim().toLowerCase());
  }

  /** Net `{` minus `}` in the next preprocessor arm. A `#if 0` arm does not count; its `#else` does. */
  branchNet(from: number): number {
    let net = 0;
    const stack: { if0: boolean; dead: boolean }[] = [];
    const dead = (): boolean => stack.some((frame) => frame.dead);
    for (let t = from; t < this.tokens.length; t++) {
      const tok = this.tokens[t];
      if (!tok) break;
      if (tok.kind === "directive") {
        const role = this.directiveRole(tok.text);
        if (role === "open") stack.push({ if0: this.if0(tok.text), dead: dead() || this.if0(tok.text) });
        else if (role === "close") {
          if (stack.length === 0) break;
          stack.pop();
        } else if (role === "mid") {
          if (stack.length === 0) break;
          const frame = stack[stack.length - 1]!;
          const outer = stack.slice(0, -1).some((item) => item.dead);
          if (frame.if0 && !outer) frame.dead = this.elif0(tok.text);
        }
        continue;
      }
      if (dead() || tok.kind === "comment") continue;
      if (tok.text === "{") net++;
      else if (tok.text === "}") net--;
    }
    return net;
  }

  braceSpans(): { start: number; end: number }[] {
    if (this.spans) return this.spans;
    const spans: { start: number; end: number }[] = [];
    const stack: { base: number; start: number; unbalanced: boolean; if0: boolean; dead: boolean }[] = [];
    const pending: { start: number; base: number }[] = [];
    let depth = 0;
    const dead = (): boolean => stack.some((frame) => frame.dead);
    for (let t = 0; t < this.tokens.length; t++) {
      const tok = this.tokens[t];
      if (!tok) break;
      if (tok.kind === "directive") {
        const role = this.directiveRole(tok.text);
        if (role === "open") {
          const zero = this.if0(tok.text);
          stack.push({ base: depth, start: tok.start, unbalanced: false, if0: zero, dead: dead() || zero });
        } else if (role === "mid" && stack.length > 0) {
          const frame = stack[stack.length - 1]!;
          const outer = stack.slice(0, -1).some((item) => item.dead);
          if (frame.if0 && !outer) {
            if (frame.dead) spans.push({ start: frame.start, end: tok.start });
            frame.dead = this.elif0(tok.text);
            frame.start = tok.start;
          } else if (!dead() && this.branchNet(t + 1) > 0 && depth !== frame.base) {
            frame.unbalanced = true;
            depth = frame.base;
          }
        } else if (role === "close" && stack.length > 0) {
          const frame = stack.pop()!;
          const outer = dead();
          if (frame.if0 && frame.dead && !outer) spans.push({ start: frame.start, end: tok.end });
          else if (!frame.dead && !outer && depth > frame.base && pending.length === 0) {
            pending.push({ start: frame.start, base: frame.base });
          }
        }
        continue;
      }
      if (dead() || tok.kind === "comment") continue;
      if (tok.text === "{") depth++;
      else if (tok.text === "}") {
        depth--;
        while (pending.length > 0 && depth <= pending[pending.length - 1]!.base) {
          const done = pending.pop()!;
          spans.push({ start: done.start, end: tok.end });
        }
      }
    }
    for (const left of pending) spans.push({ start: left.start, end: this.source.length });
    spans.sort((a, b) => a.start - b.start || b.end - a.end);
    const kept: { start: number; end: number }[] = [];
    for (const span of spans) {
      if (kept.some((have) => have.start <= span.start && have.end >= span.end)) continue;
      kept.push(span);
    }
    this.spans = kept;
    return kept;
  }

  /** A preprocessor line sits between two source offsets. */
  private hasDirectiveBetween(from: number, to: number): boolean {
    for (const tok of this.tokens) {
      if (tok.start < from) continue;
      if (tok.start >= to) break;
      if (tok.kind === "directive") return true;
    }
    return false;
  }

  spanCovering(start: number): { start: number; end: number } | null {
    for (const span of this.braceSpans()) {
      if (start >= span.start && start < span.end) return span;
    }
    return null;
  }

  indexOfCode(start: number): number {
    let lo = 0;
    let hi = this.code.length - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      const at = this.code[mid]!.start;
      if (at === start) return mid;
      if (at < start) lo = mid + 1;
      else hi = mid - 1;
    }
    return -1;
  }

  match(open: number, left: string, right: string): number {
    if (left === "{") return this.matchBrace(open);
    let depth = 0;
    for (let k = open; k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) break;
      if (tok.text === left) depth++;
      else if (tok.text === right) {
        depth--;
        if (depth === 0) return k;
      }
    }
    return -1;
  }

  /** Brace match that treats `{` opened in both arms of an `#if` as one level. */
  matchBrace(open: number): number {
    const origin = this.code[open]!.start;
    let depth = 0;
    const stack: { depth: number; if0: boolean; dead: boolean }[] = [];
    const dead = (): boolean => stack.some((frame) => frame.dead);
    for (let t = 0; t < this.tokens.length; t++) {
      const tok = this.tokens[t];
      if (!tok || tok.start < origin) continue;
      if (tok.kind === "directive") {
        const role = this.directiveRole(tok.text);
        if (role === "open") {
          const zero = this.if0(tok.text);
          stack.push({ depth, if0: zero, dead: dead() || zero });
        } else if (role === "mid" && stack.length > 0) {
          const frame = stack[stack.length - 1]!;
          const outer = stack.slice(0, -1).some((item) => item.dead);
          if (frame.if0 && !outer) frame.dead = this.elif0(tok.text);
          else if (!dead() && this.branchNet(t + 1) > 0) depth = frame.depth;
        } else if (role === "close") stack.pop();
        continue;
      }
      if (dead() || tok.kind === "comment") continue;
      if (tok.text === "{") depth++;
      else if (tok.text === "}") {
        depth--;
        if (depth === 0) return this.indexOfCode(tok.start);
      }
    }
    return -1;
  }

  matchBack(close: number, right: string, left: string): number {
    let depth = 0;
    for (let k = close; k >= 0; k--) {
      const tok = this.code[k];
      if (!tok) continue;
      if (tok.text === right) depth++;
      else if (tok.text === left) {
        depth--;
        if (depth === 0) return k;
      }
    }
    return -1;
  }

  scanSemi(i: number): number {
    let paren = 0;
    let bracket = 0;
    let brace = 0;
    for (let k = i; k < this.code.length; k++) {
      const tok = this.code[k];
      if (!tok) break;
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (tok.text === "{") brace++;
      else if (tok.text === "}" && brace > 0) brace--;
      else if (tok.text === "}" && brace === 0 && paren === 0) return -1;
      else if (tok.text === ";" && paren === 0 && bracket === 0 && brace === 0) return k;
    }
    return -1;
  }

  onlyTrivia(from: number, semi: number): boolean {
    return from >= semi;
  }

  inlineAfter(i: number): string {
    const semi = this.scanSemi(i);
    if (semi < 0 || semi === i) return "";
    return this.stripSemis(i, semi - 1);
  }

  stripSemis(from: number, to: number): string {
    if (from > to || from < 0 || to >= this.code.length) return "";
    let out = "";
    let pos = this.code[from]!.start;
    let paren = 0;
    let bracket = 0;
    let brace = 0;
    for (let k = from; k <= to; k++) {
      const tok = this.code[k];
      if (!tok) continue;
      out += this.source.slice(pos, tok.start);
      const skip = tok.text === ";" && paren === 0 && bracket === 0 && brace === 0;
      if (!skip) out += tok.text;
      if (tok.text === "(") paren++;
      else if (tok.text === ")" && paren > 0) paren--;
      else if (tok.text === "[") bracket++;
      else if (tok.text === "]" && bracket > 0) bracket--;
      else if (tok.text === "{") brace++;
      else if (tok.text === "}" && brace > 0) brace--;
      pos = tok.end;
    }
    const next = this.code[to + 1];
    let anchor = this.code[to]!;
    if (next?.text === ";" && next.start >= pos) {
      const gap = this.source.slice(pos, next.start);
      if (/\S/.test(gap)) out += gap.replace(/[ \t]+$/g, "");
      anchor = next;
    }
    const trail = this.commentOnLineAfter(anchor);
    if (trail && trail.start >= pos && trail.line === anchor.line) {
      out = `${out.replace(/[ \t]+$/, "")} ${trail.text}`;
    }
    return out;
  }

  headerText(from: number, brace: number): string {
    return this.stripSemis(from, brace - 1);
  }

  leadIn(brace: number): Node[] {
    const prev = this.code[brace - 1];
    const open = this.code[brace];
    if (!prev || !open) return [];
    let from = prev.end;
    const glued = this.commentOnLineAfter(prev);
    if (glued && glued.start < open.start) from = glued.end;
    this.mark = Math.max(this.mark, from);
    const nodes = this.pullTrivia(open.start);
    this.mark = Math.max(this.mark, open.end);
    return nodes;
  }

  flat(from: number, to: number): string {
    const parts: string[] = [];
    for (let k = from; k <= to; k++) {
      const tok = this.code[k];
      if (tok) parts.push(tok.text);
    }
    return parts.join(" ");
  }

  commentOnLineAfter(tok: Tok): Tok | undefined {
    let lo = 0;
    let hi = this.tokens.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this.tokens[mid]!.start < tok.end) lo = mid + 1;
      else hi = mid;
    }
    const cand = this.tokens[lo];
    if (!cand || cand.line !== tok.line || cand.kind !== "comment") return undefined;
    return cand;
  }

  offsetAfter(next: number): number {
    if (next <= 0) return this.mark;
    const prev = this.code[next - 1];
    if (!prev) return this.mark;
    const trail = this.commentOnLineAfter(prev);
    return trail ? trail.end : prev.end;
  }
}

type Mix = { kind: "text"; text: string } | { kind: "sexpr"; body: Node[] };

function flatten(node: Node): Node[] {
  if (node.type === "group") return node.nodes.flatMap(flatten);
  return [node];
}

function prefixHeader(node: Node, prefix: string): Node {
  if (node.type === "colon") return { ...node, header: prefix + node.header };
  if (node.type === "group") {
    const [first, ...rest] = node.nodes;
    if (!first) return node;
    return { type: "group", nodes: [prefixHeader(first, prefix), ...rest] };
  }
  return node;
}

function isEmpty(node: Node): boolean {
  return node.type === "text" && node.text.trim() === "";
}

function reindent(text: string, indent: number, anchor: "min" | "first" = "min"): string[] {
  const cleaned = text.replace(/\t/g, "    ").replace(/[ \t]+$/g, "").replace(/^\n+/, "").replace(/\s+$/g, "");
  if (!cleaned.trim()) return [];
  const lines = cleaned.split("\n");
  const widths = lines.map((line) => (line.trim() ? (line.match(/^[ ]*/) ?? [""])[0].length : null));
  const present = widths.filter((n): n is number => n !== null);
  let base = present[0] ?? 0;
  if (anchor !== "first") {
    for (const width of present) if (width < base) base = width;
  }
  return lines.map((line) => {
    if (!line.trim()) return "";
    const width = (line.match(/^[ ]*/) ?? [""])[0].length;
    return `${" ".repeat(indent + Math.max(0, width - base))}${line.trimStart()}`;
  });
}

function finish(lines: string[]): string {
  const out: string[] = [];
  for (const line of lines) {
    if (line === "" && (out.length === 0 || out[out.length - 1] === "")) continue;
    out.push(line.replace(/[ \t]+$/g, ""));
  }
  while (out.at(-1) === "") out.pop();
  if (out.length === 0) return "";
  return `${out.join("\n")}\n`;
}
