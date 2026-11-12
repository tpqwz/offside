import type { LangId } from "./types.ts";

/** Spaced extensions. Keep these in step with classifyFile in host.ts. */
const SPACED_EXT: Record<string, LangId> = {
  ".sc": "c",
  ".sjava": "java",
  ".sjs": "javascript",
  ".spas": "pascal",
  ".szig": "zig",
  ".skt": "kotlin",
};

/** Host extensions. A header stays a header: engine.h pairs with engine.h.sc. */
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
  ".zig": "zig",
  ".kt": "kotlin",
};

const HOST_SUFFIX: Record<LangId, string> = {
  c: ".c",
  java: ".java",
  javascript: ".js",
  pascal: ".pas",
  zig: ".zig",
  kotlin: ".kt",
};

const SPACED_SUFFIX: Record<LangId, string> = {
  c: ".sc",
  java: ".sjava",
  javascript: ".sjs",
  pascal: ".spas",
  zig: ".szig",
  kotlin: ".skt",
};

export interface NamedSource {
  lang: LangId;
  /** True when the file is ordinary host source, not the spaced dialect. */
  host: boolean;
}

export function baseName(file: string): string {
  const slash = Math.max(file.lastIndexOf("/"), file.lastIndexOf("\\"));
  return slash >= 0 ? file.slice(slash + 1) : file;
}

function splitExt(file: string): { base: string; stem: string; ext: string } {
  const base = baseName(file);
  const dot = base.lastIndexOf(".");
  if (dot <= 0) return { base, stem: base, ext: "" };
  return { base, stem: base.slice(0, dot), ext: base.slice(dot).toLowerCase() };
}

/** Null when the extension is not one this dialect claims. */
export function classifyName(file: string): NamedSource | null {
  const { ext } = splitExt(file);
  const spaced = SPACED_EXT[ext];
  if (spaced) return { lang: spaced, host: false };
  const host = HOST_EXT[ext];
  if (host) return { lang: host, host: true };
  return null;
}

export function spacedExampleName(id: string, lang: LangId): string {
  return `${id}${SPACED_SUFFIX[lang]}`;
}

function javaClassName(code: string, fallback: string): string {
  const named = code.match(/\b(?:public\s+(?:final\s+|abstract\s+)?)?class\s+([A-Za-z_$][\w$]*)/);
  return named?.[1] ?? fallback;
}

/**
 * Name of the file the other direction would save.
 * `toHost` means the source is spaced and the result is host.
 * `hostCode` is the host-language text, used to name a Java class.
 */
export function outputName(sourceName: string, lang: LangId, toHost: boolean, hostCode: string): string {
  const { base, stem, ext } = splitExt(sourceName);
  if (toHost) {
    if (base.toLowerCase().endsWith(".h.sc")) return base.slice(0, -3);
    if (lang === "java") return `${javaClassName(hostCode, stem)}.java`;
    return `${stem}${HOST_SUFFIX[lang]}`;
  }
  if (ext === ".h") return `${base}.sc`;
  if (lang === "java") return `${javaClassName(hostCode, stem)}.sjava`;
  return `${stem}${SPACED_SUFFIX[lang]}`;
}
