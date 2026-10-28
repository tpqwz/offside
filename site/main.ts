import { layoutSource } from "../src/lib/layout/layout.ts";
import { LANGS, PROFILES } from "../src/lib/layout/languages.ts";
import { toSpaced } from "../src/lib/layout/spaced.ts";
import type { LangId, LayoutNote } from "../src/lib/layout/types.ts";

declare const __OFFSIDE_VERSION__: string;

type Direction = "to-host" | "to-spaced";

const sourceEl = document.querySelector<HTMLTextAreaElement>("#source")!;
const outputEl = document.querySelector<HTMLElement>("#output")!;
const notesEl = document.querySelector<HTMLElement>("#notes")!;
const langsEl = document.querySelector<HTMLElement>("#langs")!;
const examplesEl = document.querySelector<HTMLElement>("#examples")!;
const sourceLabel = document.querySelector<HTMLElement>("#source-label")!;
const outName = document.querySelector<HTMLElement>("#out-name")!;
const lineCount = document.querySelector<HTMLElement>("#line-count")!;
const copyBtn = document.querySelector<HTMLButtonElement>("#copy")!;
const saveBtn = document.querySelector<HTMLButtonElement>("#save")!;

let lang: LangId = "c";
let direction: Direction = "to-host";

document.querySelector(".kicker")!.textContent = `${__OFFSIDE_VERSION__} · layout pass, not a compiler`;

function spacedName(id: LangId): string {
  if (id === "c") return "out.csp";
  if (id === "java") return "Out.sjava";
  if (id === "pascal") return "out.spas";
  return "out.sjs";
}

function convert(text: string): { code: string; notes: LayoutNote[] } {
  try {
    return direction === "to-host" ? layoutSource(text, lang) : toSpaced(text, lang);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    return { code: "", notes: [{ line: 1, message }] };
  }
}

function render(): void {
  const result = convert(sourceEl.value);
  outputEl.textContent = result.code;
  notesEl.replaceChildren(
    ...result.notes.map((note) => {
      const item = document.createElement("li");
      item.textContent = `${note.line}: ${note.message}`;
      return item;
    }),
  );
  const lines = sourceEl.value.length === 0 ? 1 : sourceEl.value.split("\n").length;
  lineCount.textContent = `${lines} line${lines === 1 ? "" : "s"}`;
  sourceLabel.textContent = direction === "to-host" ? "Spaced source" : "Host source";
  outName.textContent = direction === "to-host" ? PROFILES[lang].filename : spacedName(lang);
}

function paintPressed(root: ParentNode, attr: string, value: string): void {
  for (const button of root.querySelectorAll<HTMLButtonElement>(`button[${attr}]`)) {
    button.setAttribute("aria-pressed", button.getAttribute(attr) === value ? "true" : "false");
  }
}

function loadExample(id: string): void {
  const example = PROFILES[lang].examples.find((item) => item.id === id) ?? PROFILES[lang].examples[0];
  if (!example) return;
  direction = "to-host";
  sourceEl.value = example.source;
  paintPressed(document, "data-direction", direction);
  paintPressed(examplesEl, "data-example", example.id);
  render();
}

function drawExamples(): void {
  examplesEl.replaceChildren(
    ...PROFILES[lang].examples.map((example) => {
      const button = document.createElement("button");
      button.type = "button";
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
      button.dataset.lang = id;
      button.textContent = PROFILES[id].label;
      button.setAttribute("aria-pressed", id === lang ? "true" : "false");
      button.addEventListener("click", () => {
        lang = id;
        paintPressed(langsEl, "data-lang", lang);
        drawExamples();
        loadExample(PROFILES[lang].examples[0]?.id ?? "");
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

sourceEl.addEventListener("input", () => {
  paintPressed(examplesEl, "data-example", "");
  render();
});

copyBtn.addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText(outputEl.textContent ?? "");
    copyBtn.textContent = "Copied";
  } catch {
    copyBtn.textContent = "Copy failed";
  }
  window.setTimeout(() => {
    copyBtn.textContent = "Copy";
  }, 1200);
});

saveBtn.addEventListener("click", () => {
  const blob = new Blob([outputEl.textContent ?? ""], { type: "text/plain;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = outName.textContent ?? "out.txt";
  link.click();
  URL.revokeObjectURL(url);
});

drawLangs();
drawExamples();
loadExample("program");
