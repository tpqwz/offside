#!/usr/bin/env -S node --experimental-strip-types
import { spawn } from "node:child_process";
import { accessSync, constants, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { classifyFile, hostFileName, remapDiagnostics, renderHost } from "../lib/layout/host.ts";
import { layoutSource } from "../lib/layout/layout.ts";
import { toSpaced } from "../lib/layout/spaced.ts";
import type { LangId, LayoutNote, LayoutResult } from "../lib/layout/types.ts";

export const VERSION = "0.1.0-alpha.1";

const HELP = `offside ${VERSION}

offside run <file> [program args] [-- <compiler args>] [-- <program args>]
offside emit <file> [--plain]
offside space <file>

  .csp .sjava .sjs .spas     spaced source
  .c .h .java .js            host C, Java, JavaScript
  .pas .pp .lpr .dpr .inc    host Pascal

  offside run hello.csp -- -O2
  offside emit hello.csp
  offside space hello.c
  offside run demo.spas

  --lang c|java|javascript|pascal   ignore the extension
  --cc <compiler>            C compiler, else tcc or cc
  --keep                     leave the host file on disk
  --plain                    emit host code without #line
  --version                  print ${VERSION}

  emit turns spaced source into host code.
  space turns host code into the spaced dialect.
  run accepts either, then compiles.

  C: tcc defaults to -std=c99. gcc and clang default to -std=gnu2x.
  Pascal: Free Pascal (ppcx64). FPC or OFFSIDE_FPC picks the compiler.
  OFFSIDE_CC or TCC or CC overrides the C compiler.
`;

export interface Plan {
  command: "run" | "emit" | "space" | "help";
  file?: string;
  lang?: string;
  cc?: string;
  keep: boolean;
  plain: boolean;
  ccFlags: string[];
  programArgs: string[];
}

export function parseArgs(args: string[]): { ok: true; plan: Plan } | { ok: false; error: string } {
  if (args.length === 0 || args[0] === "--help" || args[0] === "-h") {
    return { ok: true, plan: emptyPlan("help") };
  }
  const command = args[0];
  if (command !== "run" && command !== "emit" && command !== "space") {
    return { ok: false, error: `Unknown command ${command}.` };
  }
  const groups: string[][] = [[]];
  for (const arg of args.slice(1)) {
    if (arg === "--" && groups.length < 3) groups.push([]);
    else groups[groups.length - 1]?.push(arg);
  }
  const group = groups[0] ?? [];
  const ccFlags = groups[1] ?? [];
  const trailed = groups.slice(2).flat();
  const plan = emptyPlan(command);
  plan.ccFlags = ccFlags;
  const positionals: string[] = [];
  for (let i = 0; i < group.length; i++) {
    const arg = group[i];
    if (!arg) continue;
    if (arg === "--help" || arg === "-h") return { ok: true, plan: emptyPlan("help") };
    if (arg === "--keep" || arg === "-k") {
      plan.keep = true;
      continue;
    }
    if (arg === "--plain") {
      plan.plain = true;
      continue;
    }
    if (arg === "--lang" || arg === "-l") {
      const value = group[++i];
      if (!value) return { ok: false, error: `${arg} needs c, java, javascript, or pascal.` };
      plan.lang = value;
      continue;
    }
    if (arg === "--cc") {
      const value = group[++i];
      if (!value) return { ok: false, error: "--cc needs a compiler path." };
      plan.cc = value;
      continue;
    }
    if (arg.startsWith("-")) return { ok: false, error: `Unknown option ${arg}.` };
    positionals.push(arg);
  }
  plan.file = positionals[0];
  plan.programArgs = [...positionals.slice(1), ...trailed];
  if (!plan.file) return { ok: false, error: "Missing a source file." };
  if ((command === "emit" || command === "space") && (ccFlags.length > 0 || plan.programArgs.length > 0)) {
    return { ok: false, error: `${command} only writes source. Compiler flags belong on run.` };
  }
  return { ok: true, plan };
}

function emptyPlan(command: Plan["command"]): Plan {
  return { command, keep: false, plain: false, ccFlags: [], programArgs: [] };
}

function commandExists(file: string): boolean {
  try {
    accessSync(file, constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

function which(cmd: string): string | null {
  if (cmd.includes("/")) return commandExists(cmd) ? cmd : null;
  const found = spawnSyncWhich(cmd);
  return found;
}

function spawnSyncWhich(cmd: string): string | null {
  const pathEnv = process.env.PATH ?? "";
  for (const dir of pathEnv.split(path.delimiter)) {
    if (!dir) continue;
    const candidate = path.join(dir, cmd);
    if (commandExists(candidate)) return candidate;
  }
  return null;
}

export function defaultCCompiler(): string {
  const fromEnv = process.env.OFFSIDE_CC || process.env.TCC;
  if (fromEnv) return fromEnv;
  return which("tcc") || process.env.CC || which("cc") || which("gcc") || "cc";
}

function isTcc(compiler: string): boolean {
  return path.basename(compiler) === "tcc";
}

function implicitCStd(compiler: string, ccFlags: string[]): string[] {
  if (ccFlags.some((flag) => flag === "-std" || flag.startsWith("-std="))) return [];
  if (isTcc(compiler)) return ["-std=c99"];
  const base = path.basename(compiler);
  if (base === "gcc" || base === "g++" || base === "clang" || base.startsWith("clang")) return ["-std=gnu2x"];
  return ["-std=c99"];
}

function statusOf(code: number | null, signal: NodeJS.Signals | null): number {
  if (signal) return 1;
  return code ?? 1;
}

function spawnMapped(
  cmd: string,
  args: string[],
  remap: (text: string) => string,
  cwd = process.cwd(),
): Promise<number> {
  return new Promise((resolve) => {
    const child = spawn(cmd, args, { cwd, stdio: ["inherit", "pipe", "pipe"] });
    let pending = "";
    const writeErr = (chunk: string) => {
      pending += chunk;
      const lines = pending.split("\n");
      pending = lines.pop() ?? "";
      for (const line of lines) process.stderr.write(remap(`${line}\n`));
    };
    child.stdout?.on("data", (chunk: Buffer) => process.stdout.write(chunk));
    child.stderr?.on("data", (chunk: Buffer) => writeErr(chunk.toString("utf8")));
    child.on("error", (error) => {
      process.stderr.write(`${cmd}: ${error.message}\n`);
      resolve(127);
    });
    child.on("close", (code, signal) => {
      if (pending) process.stderr.write(remap(pending.endsWith("\n") ? pending : `${pending}\n`));
      resolve(statusOf(code, signal));
    });
  });
}

function printNotes(file: string, notes: readonly LayoutNote[]): void {
  for (const note of notes) {
    process.stderr.write(`${file}:${note.line}: note: ${note.message}\n`);
  }
}

async function runC(
  compiler: string,
  sourcePath: string,
  result: LayoutResult,
  ccFlags: string[],
  programArgs: string[],
  keep: boolean,
  report: boolean,
): Promise<number> {
  const dir = mkdtempSync(path.join(tmpdir(), "offside-"));
  const hostName = hostFileName("c", sourcePath, result.code);
  const hostPath = path.join(dir, hostName);
  try {
    const rendered = renderHost(result, sourcePath, report);
    writeFileSync(hostPath, rendered.code);
    const remap = report
      ? (text: string) => remapDiagnostics(text, hostPath, sourcePath, rendered.lineMap)
      : (text: string) => text;
    const std = implicitCStd(compiler, ccFlags);
    const include = ["-I", path.dirname(sourcePath)];
    const compileOnly = ccFlags.some((flag) => flag === "-c" || flag === "-E" || flag === "-S");
    const hasOut = ccFlags.includes("-o") || ccFlags.some((flag) => flag.startsWith("-o"));
    if (isTcc(compiler) && !compileOnly && !hasOut) {
      return await spawnMapped(compiler, [...std, ...include, ...ccFlags, "-run", hostPath, ...programArgs], remap);
    }
    const binary = path.join(dir, path.basename(hostName, ".c"));
    const produce = [...std, ...include, ...ccFlags, hostPath];
    if (!compileOnly && !hasOut) produce.push("-o", binary);
    const compiled = await spawnMapped(compiler, produce, remap);
    if (compiled !== 0 || compileOnly) return compiled;
    const explicit = hasOut ? outputPath(ccFlags) : binary;
    if (!explicit) return compiled;
    return await spawnMapped(explicit, programArgs, remap);
  } finally {
    if (keep) process.stderr.write(`kept ${hostPath}\n`);
    else rmSync(dir, { recursive: true, force: true });
  }
}

function outputPath(flags: string[]): string | null {
  const index = flags.indexOf("-o");
  if (index >= 0) return flags[index + 1] ?? null;
  const joined = flags.find((flag) => flag.startsWith("-o") && flag.length > 2);
  return joined ? joined.slice(2) : null;
}

const DEFAULT_FPC = "/workspace/.tools/fpc/bin/ppcx64";
const DEFAULT_FPC_UNITS = "/workspace/.tools/fpc/lib/fpc/3.2.2/units/x86_64-linux/rtl";

export function defaultPascalCompiler(): string {
  return process.env.OFFSIDE_FPC || process.env.FPC || (commandExists(DEFAULT_FPC) ? DEFAULT_FPC : "") || which("ppcx64") || which("fpc") || "fpc";
}

function pascalUnits(): string {
  if (process.env.FPC_UNITS) return process.env.FPC_UNITS;
  const compiler = defaultPascalCompiler();
  const sibling = path.resolve(path.dirname(compiler), "../lib/fpc/3.2.2/units/x86_64-linux/rtl");
  if (commandExists(compiler) && sibling.startsWith("/")) {
    try {
      accessSync(sibling);
      return sibling;
    } catch {
      /* fall through */
    }
  }
  return DEFAULT_FPC_UNITS;
}

async function runPascal(
  compiler: string,
  sourcePath: string,
  result: LayoutResult,
  ccFlags: string[],
  programArgs: string[],
  keep: boolean,
  report: boolean,
): Promise<number> {
  const dir = mkdtempSync(path.join(tmpdir(), "offside-"));
  const hostName = hostFileName("pascal", sourcePath, result.code);
  const hostPath = path.join(dir, hostName);
  try {
    const rendered = renderHost(result, sourcePath, false);
    writeFileSync(hostPath, rendered.code);
    const remap = report
      ? (text: string) => remapDiagnostics(text, hostPath, sourcePath, rendered.lineMap)
      : (text: string) => text;
    const compileOnly = ccFlags.includes("-c");
    const hasOut = ccFlags.includes("-o") || ccFlags.some((flag) => flag.startsWith("-o"));
    const binary = path.join(dir, path.basename(hostName, ".pas"));
    const args = [`-Fu${pascalUnits()}`, `-Fu${path.dirname(sourcePath)}`, ...ccFlags, hostPath];
    if (!compileOnly && !hasOut) args.push(`-o${binary}`);
    const compiled = await spawnMapped(compiler, args, remap, dir);
    if (compiled !== 0 || compileOnly) return compiled;
    const explicit = hasOut ? outputPath(ccFlags) : binary;
    if (!explicit) return compiled;
    return await spawnMapped(explicit, programArgs, remap, dir);
  } finally {
    if (keep) process.stderr.write(`kept ${hostPath}\n`);
    else rmSync(dir, { recursive: true, force: true });
  }
}

async function runJava(
  sourcePath: string,
  result: LayoutResult,
  ccFlags: string[],
  programArgs: string[],
  keep: boolean,
  report: boolean,
): Promise<number> {
  const dir = mkdtempSync(path.join(tmpdir(), "offside-"));
  const hostName = hostFileName("java", sourcePath, result.code);
  const hostPath = path.join(dir, hostName);
  const className = hostName.replace(/\.java$/, "");
  try {
    const rendered = renderHost(result, sourcePath, false);
    writeFileSync(hostPath, rendered.code);
    const remap = report
      ? (text: string) => remapDiagnostics(text, hostPath, sourcePath, rendered.lineMap)
      : (text: string) => text;
    const compiled = await spawnMapped("javac", [...ccFlags, hostPath], remap);
    if (compiled !== 0 || ccFlags.includes("-c")) return compiled;
    return await spawnMapped("java", ["-cp", dir, className, ...programArgs], remap);
  } finally {
    if (keep) process.stderr.write(`kept ${hostPath}\n`);
    else rmSync(dir, { recursive: true, force: true });
  }
}

async function runJavaScript(
  sourcePath: string,
  result: LayoutResult,
  ccFlags: string[],
  programArgs: string[],
  keep: boolean,
  report: boolean,
): Promise<number> {
  const dir = mkdtempSync(path.join(tmpdir(), "offside-"));
  const hostPath = path.join(dir, hostFileName("javascript", sourcePath, result.code));
  try {
    const rendered = renderHost(result, sourcePath, false);
    writeFileSync(hostPath, rendered.code);
    const remap = report
      ? (text: string) => remapDiagnostics(text, hostPath, sourcePath, rendered.lineMap)
      : (text: string) => text;
    return await spawnMapped("node", [...ccFlags, hostPath, ...programArgs], remap);
  } finally {
    if (keep) process.stderr.write(`kept ${hostPath}\n`);
    else rmSync(dir, { recursive: true, force: true });
  }
}

export async function runPlan(plan: Plan): Promise<number> {
  if (plan.command === "help" || !plan.file) {
    process.stdout.write(HELP);
    return 0;
  }
  const sourcePath = path.resolve(plan.file);
  let kind: { lang: LangId; host: boolean };
  try {
    kind = classifyFile(sourcePath, plan.lang);
  } catch (error) {
    process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    return 2;
  }
  let source: string;
  try {
    source = readFileSync(sourcePath, "utf8");
  } catch (error) {
    process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    return 1;
  }
  if (plan.command === "space") {
    const spaced = toSpaced(source, kind.lang);
    printNotes(sourcePath, spaced.notes);
    process.stdout.write(spaced.code);
    return 0;
  }
  if (kind.host && plan.command === "emit") {
    process.stderr.write(`${sourcePath} is host source. offside space writes the spaced form.\n`);
    return 2;
  }
  let spacedNotes: LayoutNote[] = [];
  if (kind.host) {
    const spaced = toSpaced(source, kind.lang);
    spacedNotes = spaced.notes;
    source = spaced.code;
  }
  const result = layoutSource(source, kind.lang);
  printNotes(sourcePath, [...spacedNotes, ...result.notes]);
  if (plan.command === "emit") {
    process.stdout.write(renderHost(result, sourcePath, !plan.plain).code);
    return 0;
  }
  if (kind.lang === "c") {
    return runC(plan.cc ?? defaultCCompiler(), sourcePath, result, plan.ccFlags, plan.programArgs, plan.keep, !kind.host);
  }
  if (kind.lang === "java") return runJava(sourcePath, result, plan.ccFlags, plan.programArgs, plan.keep, !kind.host);
  if (kind.lang === "pascal") {
    return runPascal(plan.cc ?? defaultPascalCompiler(), sourcePath, result, plan.ccFlags, plan.programArgs, plan.keep, !kind.host);
  }
  return runJavaScript(sourcePath, result, plan.ccFlags, plan.programArgs, plan.keep, !kind.host);
}

export async function main(args: string[]): Promise<number> {
  if (args.length === 1 && (args[0] === "--version" || args[0] === "-V")) {
    process.stdout.write(`${VERSION}\n`);
    return 0;
  }
  const parsed = parseArgs(args);
  if (!parsed.ok) {
    process.stderr.write(`${parsed.error}\n${HELP}`);
    return 2;
  }
  return runPlan(parsed.plan);
}

const invoked = process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (invoked) {
  main(process.argv.slice(2))
    .then((code) => process.exit(code))
    .catch((error: unknown) => {
      process.stderr.write(`${error instanceof Error ? error.stack ?? error.message : String(error)}\n`);
      process.exit(1);
    });
}
