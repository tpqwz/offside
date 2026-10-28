import path from "node:path";
import type { LangId, LayoutResult, OutLine } from "./types.ts";

export function lineText(line: OutLine): string {
  return line.segments.map((segment) => segment.text).join("");
}

/** A backslash in the last column continues the physical line. Whitespace after it does not. */
export function continuesLine(text: string): boolean {
  return text.endsWith("\\");
}

export function cQuote(value: string): string {
  return `"${value.replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;
}

export interface HostFile {
  code: string;
  /** 1-based generated line to 1-based spaced line. Index 0 is unused. */
  lineMap: number[];
}

/**
 * Host source for a compiler. C gets a `#line` before every physical line so
 * diagnostics name the spaced file. A backslash continuation is left intact:
 * a `#line` between those lines would be pasted into the macro.
 */
export function renderHost(result: LayoutResult, sourceName: string, lineDirectives: boolean): HostFile {
  const out: string[] = [];
  const lineMap: number[] = [0];
  const useLine = lineDirectives && result.language === "c";
  let openContinuation = false;
  for (const line of result.lines) {
    const text = lineText(line);
    if (useLine && !openContinuation) {
      out.push(`#line ${line.sourceLine} ${cQuote(sourceName)}`);
      lineMap.push(line.sourceLine);
    }
    out.push(text);
    lineMap.push(line.sourceLine);
    openContinuation = continuesLine(text);
  }
  return { code: out.length === 0 ? "" : `${out.join("\n")}\n`, lineMap };
}

export function remapDiagnostics(
  text: string,
  generatedName: string,
  sourceName: string,
  lineMap: readonly number[],
): string {
  const names = [generatedName, generatedName.split(/[\\/]/).pop() ?? ""].filter((name, index, all) => name && all.indexOf(name) === index);
  let out = text;
  for (const name of names) {
    const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    out = out.replace(new RegExp(`${escaped}:(\\d+)`, "g"), (_match, line: string) => {
      const mapped = lineMap[Number(line)] ?? Number(line);
      return `${sourceName}:${mapped}`;
    });
    out = out.replace(new RegExp(`${escaped}\\((\\d+)`, "g"), (_match, line: string) => {
      const mapped = lineMap[Number(line)] ?? Number(line);
      return `${sourceName}(${mapped}`;
    });
  }
  return out;
}

export function langOf(file: string, override?: string): LangId {
  return classifyFile(file, override).lang;
}

const SPACED_EXT: Record<string, LangId> = {
  ".csp": "c",
  ".sjava": "java",
  ".sjs": "javascript",
  ".spas": "pascal",
};

const HOST_EXT: Record<string, LangId> = {
  ".c": "c",
  ".h": "c",
  ".java": "java",
  ".js": "javascript",
  ".mjs": "javascript",
  ".cjs": "javascript",
  ".pas": "pascal",
  ".pp": "pascal",
  ".lpr": "pascal",
  ".dpr": "pascal",
  ".inc": "pascal",
};

const LANG_IDS = new Set<LangId>(["c", "java", "javascript", "pascal"]);

export interface SourceKind {
  lang: LangId;
  /** True when the file is ordinary host source, not the spaced dialect. */
  host: boolean;
}

export function classifyFile(file: string, override?: string): SourceKind {
  const ext = path.extname(file).toLowerCase();
  const spaced = SPACED_EXT[ext];
  const host = HOST_EXT[ext];
  let lang: LangId;
  if (override) {
    if (!LANG_IDS.has(override as LangId)) {
      throw new Error(`Unknown language ${override}. Use c, java, javascript, or pascal.`);
    }
    lang = override as LangId;
  } else if (spaced) lang = spaced;
  else if (host) lang = host;
  else {
    throw new Error(`Cannot tell the language of ${file}. Use .c/.csp, .java/.sjava, .js/.sjs, .pas/.spas, or pass --lang.`);
  }
  return { lang, host: Boolean(host) && !spaced };
}

export function javaClassName(code: string, fallback: string): string {
  const named = code.match(/\b(?:public\s+(?:final\s+|abstract\s+)?)?class\s+([A-Za-z_$][\w$]*)/);
  return named?.[1] ?? fallback;
}

export function hostFileName(lang: LangId, sourcePath: string, code: string): string {
  const stem = path.basename(sourcePath, path.extname(sourcePath));
  if (lang === "java") return `${javaClassName(code, stem)}.java`;
  if (lang === "javascript") return `${stem}.js`;
  if (lang === "pascal") return `${stem}.pas`;
  return `${stem}.c`;
}
