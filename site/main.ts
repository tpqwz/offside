import { layoutSource } from "../src/lib/layout/layout.ts";
import { LANGS, PROFILES } from "../src/lib/layout/languages.ts";
import { toSpaced } from "../src/lib/layout/spaced.ts";
import type { LangId, LayoutEvent, LayoutNote, OutLine } from "../src/lib/layout/types.ts";

declare const __HELLO__: string;
declare const __POINT__: string;
declare const __FIZZBUZZ__: string;
declare const __LIST__: string;
declare const __CLASSIFY__: string;

type Direction = "to-host" | "to-spaced";
type Panel = "source" | "result" | "decisions";

const sourceEl = document.querySelector<HTMLTextAreaElement>("#source")!;
const sourceGutter = document.querySelector<HTMLElement>("#source-gutter")!;
const outputEl = document.querySelector<HTMLElement>("#output")!;
const outGutter = document.querySelector<HTMLElement>("#out-gutter")!;
const notesEl = document.querySelector<HTMLElement>("#notes")!;
const eventsEl = document.querySelector<HTMLElement>("#events")!;
const langsEl = document.querySelector<HTMLElement>("#langs")!;
const examplesEl = document.querySelector<HTMLElement>("#examples")!;
const sourceLabel = document.querySelector<HTMLElement>("#source-label")!;
const outName = document.querySelector<HTMLElement>("#out-name")!;
const lineCount = document.querySelector<HTMLElement>("#line-count")!;
const outMeta = document.querySelector<HTMLElement>("#out-meta")!;
const copyBtn = document.querySelector<HTMLButtonElement>("#copy")!;
const saveBtn = document.querySelector<HTMLButtonElement>("#save")!;

let lang: LangId = "c";
let direction: Direction = "to-host";
let resultText = "";

const SPACED: Record<LangId, string> = {
  c: "out.csp",
  java: "Out.sjava",
  javascript: "out.sjs",
  pascal: "out.spas",
  zig: "out.szig",
  kotlin: "out.skt",
};

const C_PROGRAMS = [
  { id: "hello", label: "Hello", source: __HELLO__ },
  { id: "point", label: "Point", source: __POINT__ },
  { id: "fizzbuzz", label: "Fizzbuzz", source: __FIZZBUZZ__ },
  { id: "list", label: "List", source: __LIST__ },
  { id: "classify", label: "Classify", source: __CLASSIFY__ },
];

function spacedName(id: LangId): string {
  return SPACED[id];
}

function fileName(): string {
  return direction === "to-host" ? PROFILES[lang].filename : spacedName(lang);
}

function examplesFor(id: LangId) {
  return id === "c" ? [...C_PROGRAMS, ...PROFILES.c.examples] : PROFILES[id].examples;
}

function paintPressed(root: ParentNode, attr: string, value: string): void {
  for (const button of root.querySelectorAll<HTMLButtonElement>(`button[${attr}]`)) {
    const on = button.getAttribute(attr) === value;
    button.setAttribute(attr === "data-panel" ? "aria-selected" : "aria-pressed", on ? "true" : "false");
  }
}

function gutter(el: HTMLElement, numbers: number[]): void {
  el.replaceChildren(
    ...numbers.map((n) => {
      const span = document.createElement("span");
      span.className = "code-num";
      span.textContent = String(n);
      return span;
    }),
  );
}

function plainLines(code: string): OutLine[] {
  const parts = code.length === 0 ? [""] : code.split("\n");
  if (parts.length > 1 && parts[parts.length - 1] === "") parts.pop();
  return parts.map((text, index) => ({ segments: [{ text, inserted: false }], sourceLine: index + 1 }));
}

function convert(text: string): { code: string; notes: LayoutNote[]; events: LayoutEvent[]; lines: OutLine[] } {
  try {
    if (direction === "to-host") return layoutSource(text, lang);
    const spaced = toSpaced(text, lang);
    return { code: spaced.code, notes: spaced.notes, events: [], lines: plainLines(spaced.code) };
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    return { code: "", notes: [{ line: 1, message }], events: [], lines: [] };
  }
}

function render(): void {
  const result = convert(sourceEl.value);
  resultText = result.code;
  const sourceLines = sourceEl.value.length === 0 ? [""] : sourceEl.value.split("\n");
  if (sourceLines.length > 1 && sourceLines[sourceLines.length - 1] === "") sourceLines.pop();
  gutter(sourceGutter, sourceLines.map((_, index) => index + 1));
  lineCount.textContent = `${sourceLines.length} line${sourceLines.length === 1 ? "" : "s"}`;
  sourceLabel.textContent = direction === "to-host" ? "Spaced source" : "Host source";

  const lines = result.lines.length > 0 ? result.lines : plainLines(result.code);
  const empty = result.code.length === 0;
  outputEl.replaceChildren(
    ...lines.map((line) => {
      const row = document.createElement("span");
      row.className = "code-line";
      if (line.segments.length === 0) row.textContent = " ";
      for (const segment of line.segments) {
        if (!segment.inserted) {
          row.append(segment.text);
          continue;
        }
        const mark = document.createElement("span");
        mark.className = "code-mark";
        mark.textContent = segment.text;
        row.append(mark);
      }
      return row;
    }),
  );
  gutter(outGutter, lines.map((line) => line.sourceLine || 1));
  const insertions = lines.reduce((sum, line) => sum + line.segments.filter((segment) => segment.inserted && segment.text !== "").length, 0);
  outName.textContent = direction === "to-host" ? "Host result" : "Spaced result";
  outMeta.replaceChildren();
  if (empty) {
    outMeta.textContent = "Empty";
  } else if (direction === "to-host") {
    outMeta.append(
      document.createTextNode(
        `${fileName()} · ${lines.length} line${lines.length === 1 ? "" : "s"} · ${insertions} insertion${insertions === 1 ? "" : "s"}`,
      ),
    );
    const oxide = document.createElement("span");
    oxide.className = "oxide";
    oxide.textContent = " · oxide is new";
    outMeta.append(oxide);
  } else {
    outMeta.textContent = `${fileName()} · ${lines.length} line${lines.length === 1 ? "" : "s"} · colons replace braces`;
  }

  notesEl.replaceChildren();
  if (result.notes.length === 0) {
    const none = document.createElement("p");
    none.textContent = direction === "to-host"
      ? "None. Labels, ternaries, and preprocessor lines were left alone."
      : "None. Statement braces became colons. Initializers kept theirs.";
    notesEl.append(none);
  } else {
    for (const note of result.notes) {
      const item = document.createElement("li");
      const where = document.createElement("span");
      where.textContent = `Line ${note.line}. `;
      item.append(where, note.message);
      notesEl.append(item);
    }
  }
  if (result.events.length === 0) {
    const none = document.createElement("p");
    none.textContent = direction === "to-host" ? "No blocks opened." : "The reverse pass does not record events.";
    eventsEl.replaceChildren(none);
  } else eventsEl.replaceChildren(
    ...result.events.map((event) => {
      const item = document.createElement("li");
      const line = document.createElement("span");
      line.className = "ln";
      line.textContent = String(event.line);
      const message = document.createElement("span");
      message.textContent = event.message;
      item.append(line, message);
      return item;
    }),
  );

  const policy = PROFILES[lang].policy;
  document.querySelector("#policy-cue")!.textContent = policy.cue;
  document.querySelector("#policy-terminator")!.textContent = policy.terminator;
  document.querySelector("#policy-close")!.textContent = policy.semiClose;
  document.querySelector("#policy-through")!.textContent = policy.transparent;
}

function loadExample(id: string): void {
  const example = examplesFor(lang).find((item) => item.id === id) ?? examplesFor(lang)[0];
  if (!example) return;
  direction = "to-host";
  sourceEl.value = example.source;
  paintPressed(document, "data-direction", direction);
  paintPressed(examplesEl, "data-example", example.id);
  render();
}

function drawExamples(): void {
  examplesEl.replaceChildren(
    ...examplesFor(lang).map((example) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "plaque";
      button.dataset.example = example.id;
      button.textContent = example.label;
      button.addEventListener("click", () => loadExample(example.id));
      return button;
    }),
  );
}

function drawLangs(): void {
  langsEl.replaceChildren(
    ...LANGS.map((id) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "plaque";
      button.dataset.lang = id;
      button.textContent = PROFILES[id].label;
      button.setAttribute("aria-pressed", id === lang ? "true" : "false");
      button.addEventListener("click", () => {
        lang = id;
        paintPressed(langsEl, "data-lang", lang);
        drawExamples();
        loadExample(examplesFor(lang)[0]?.id ?? "");
      });
      return button;
    }),
  );
}

for (const button of document.querySelectorAll<HTMLButtonElement>("button[data-direction]")) {
  button.addEventListener("click", () => {
    const next = button.dataset.direction as Direction;
    if (next === direction) return;
    sourceEl.value = convert(sourceEl.value).code;
    direction = next;
    paintPressed(document, "data-direction", direction);
    paintPressed(examplesEl, "data-example", "");
    render();
  });
}

for (const button of document.querySelectorAll<HTMLButtonElement>("button[data-panel]")) {
  button.addEventListener("click", () => {
    const panel = button.dataset.panel as Panel;
    document.body.dataset.panel = panel;
    paintPressed(document.querySelector(".tabs")!, "data-panel", panel);
  });
}

sourceEl.addEventListener("input", () => {
  paintPressed(examplesEl, "data-example", "");
  render();
});
sourceEl.addEventListener("scroll", () => {
  sourceGutter.scrollTop = sourceEl.scrollTop;
});
outputEl.addEventListener("scroll", () => {
  outGutter.scrollTop = outputEl.scrollTop;
});

copyBtn.addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText(resultText);
    copyBtn.textContent = "Copied";
  } catch {
    copyBtn.textContent = "Copy failed";
  }
  window.setTimeout(() => {
    copyBtn.textContent = "Copy";
  }, 1200);
});

saveBtn.addEventListener("click", () => {
  const blob = new Blob([resultText], { type: "text/plain;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = fileName();
  link.click();
  URL.revokeObjectURL(url);
});

drawLangs();
drawExamples();
loadExample("program");
