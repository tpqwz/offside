import { readdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { join, relative } from "node:path";
import { layoutSource } from "../src/lib/layout/layout.ts";
import { lex } from "../src/lib/layout/lex.ts";
import { PROFILES } from "../src/lib/layout/languages.ts";
import { toSpaced } from "../src/lib/layout/spaced.ts";

const ROOT = process.argv[2] ?? "/tmp/lazarus";
const MAX = Number(process.env.CORPUS_MAX_BYTES ?? 2_000_000);
const DIRS = ["components/lazutils", "lcl"];
const FILES = ["fpmake.pp", "fpmake_add.inc", "fpmake_proc.inc"];

const SKIP_KEYWORD = new Set(["begin", "end"]);

function walk(dir: string, out: string[]): void {
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    const stat = statSync(path);
    if (stat.isDirectory()) {
      walk(path, out);
      continue;
    }
    if (/\.(pas|pp|inc)$/i.test(name)) out.push(path);
  }
}

function significant(source: string, keepDirectives: boolean): string[] {
  const { tokens } = lex(source, PROFILES.pascal);
  return tokens
    .filter((tok) => tok.kind !== "comment" && (keepDirectives || tok.kind !== "directive"))
    .map((tok) => tok.text.toLowerCase());
}

function keywordCounts(tokens: string[]): Map<string, number> {
  const counts = new Map<string, number>();
  for (const text of tokens) {
    if (!PROFILES.pascal.keywords.has(text) || SKIP_KEYWORD.has(text)) continue;
    counts.set(text, (counts.get(text) ?? 0) + 1);
  }
  return counts;
}

function countDelta(before: string[], after: string[]): string[] {
  const left = keywordCounts(before);
  const right = keywordCounts(after);
  const keys = new Set([...left.keys(), ...right.keys()]);
  const deltas: string[] = [];
  for (const key of keys) {
    const a = left.get(key) ?? 0;
    const b = right.get(key) ?? 0;
    if (a !== b) deltas.push(`${key} ${a}->${b}`);
  }
  return deltas.sort();
}

function firstMismatch(left: string[], right: string[]): string {
  const n = Math.min(left.length, right.length);
  for (let i = 0; i < n; i++) {
    if (left[i] !== right[i]) {
      const around = left.slice(Math.max(0, i - 4), i + 6).join(" ");
      return `token ${i}: '${left[i]}' vs '${right[i]}' near ${around}`;
    }
  }
  if (left.length !== right.length) return `length ${left.length} vs ${right.length}`;
  return "";
}

function bucket(message: string, deltas: string[], notes: string[]): string {
  const blob = `${message}\n${deltas.join(" ")}\n${notes.join(" ")}`.toLowerCase();
  if (blob.includes("variant record")) return "variant-record";
  if (blob.includes("skipped a pascal")) return "skipped-token";
  if (blob.includes("no indented body")) return "empty-body";
  if (/\b(else|otherwise)\b/.test(blob) && deltas.some((d) => /^(else|otherwise) /.test(d))) return "else";
  if (deltas.some((d) => /^(except|finally|try) /.test(d))) return "try";
  if (deltas.some((d) => /^(procedure|function|constructor|destructor|operator) /.test(d))) return "routine";
  if (deltas.some((d) => /^(record|class|interface|object|dispinterface) /.test(d))) return "type-body";
  if (deltas.some((d) => /^(property|var|const|type) /.test(d))) return "decl";
  if (blob.includes("{$") || blob.includes("directive")) return "directive";
  if (message.includes("length") || message.includes("';'")) return "separator";
  return "other";
}

const files: string[] = [];
for (const dir of DIRS) walk(join(ROOT, dir), files);
for (const file of FILES) {
  const path = join(ROOT, file);
  try {
    if (statSync(path).isFile()) files.push(path);
  } catch {
    // optional root file
  }
}
files.sort((a, b) => statSync(a).size - statSync(b).size || a.localeCompare(b));

const buckets = new Map<string, number>();
const samples = new Map<string, string[]>();
let clean = 0;
let skipped = 0;
let thrown = 0;
const rows: { file: string; bucket: string; detail: string }[] = [];

const started = Date.now();
for (const file of files) {
  const size = statSync(file).size;
  const rel = relative(ROOT, file);
  if (size > MAX) {
    skipped++;
    rows.push({ file: rel, bucket: "too-big", detail: `${size}` });
    continue;
  }
  process.stderr.write(`${rel} ${size}\n`);
  let source: string;
  try {
    source = readFileSync(file, "utf8");
  } catch (error) {
    thrown++;
    rows.push({ file: rel, bucket: "read", detail: error instanceof Error ? error.message : String(error) });
    continue;
  }
  try {
    const spaced = toSpaced(source, "pascal");
    const host = layoutSource(spaced.code, "pascal");
    const again = toSpaced(host.code, "pascal");
    const originalTokens = significant(source, false);
    const hostTokens = significant(host.code, false);
    const spacedTokens = significant(spaced.code, false);
    const againTokens = significant(again.code, false);
    const deltas = countDelta(originalTokens, hostTokens);
    const drift = firstMismatch(spacedTokens, againTokens);
    const directiveDrift = firstMismatch(significant(spaced.code, true), significant(again.code, true));
    const notes = [...spaced.notes, ...host.notes, ...again.notes].map((note) => note.message);
    if (deltas.length === 0 && !drift) {
      clean++;
      if (directiveDrift) {
        buckets.set("directive-only", (buckets.get("directive-only") ?? 0) + 1);
      }
      continue;
    }
    const detail = [drift, deltas.join(", "), notes.filter((m, i, all) => all.indexOf(m) === i).slice(0, 3).join(" | ")]
      .filter(Boolean)
      .join(" :: ");
    const kind = drift && deltas.length === 0 ? "unstable" : bucket(detail, deltas, notes);
    buckets.set(kind, (buckets.get(kind) ?? 0) + 1);
    const list = samples.get(kind) ?? [];
    if (list.length < 8) {
      list.push(`${rel}: ${detail.slice(0, 400)}`);
      samples.set(kind, list);
    }
    rows.push({ file: rel, bucket: kind, detail: detail.slice(0, 800) });
  } catch (error) {
    thrown++;
    const message = error instanceof Error ? `${error.name}: ${error.message}` : String(error);
    buckets.set("throw", (buckets.get("throw") ?? 0) + 1);
    const list = samples.get("throw") ?? [];
    if (list.length < 8) {
      list.push(`${rel}: ${message}`);
      samples.set("throw", list);
    }
    rows.push({ file: rel, bucket: "throw", detail: message });
  }
}

const summary = {
  files: files.length,
  clean,
  dirty: files.length - clean - skipped,
  skipped,
  thrown,
  seconds: Math.round((Date.now() - started) / 100) / 10,
  buckets: Object.fromEntries([...buckets.entries()].sort((a, b) => b[1] - a[1])),
  samples: Object.fromEntries(samples),
};

writeFileSync("/tmp/pascal-corpus.json", JSON.stringify({ summary, rows }, null, 2));
console.log(JSON.stringify(summary, null, 2));
