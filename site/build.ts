import { mkdirSync, readFileSync, writeFileSync } from "node:fs";

const version = JSON.parse(readFileSync("package.json", "utf8")).version as string;
const example = (name: string) => readFileSync(`examples/${name}.sc`, "utf8");
mkdirSync("site/dist", { recursive: true });

const built = await Bun.build({
  entrypoints: ["site/main.ts"],
  outdir: "site/dist",
  target: "browser",
  naming: "app.js",
  define: {
    __OFFSIDE_VERSION__: JSON.stringify(version),
    __HELLO__: JSON.stringify(example("hello")),
    __POINT__: JSON.stringify(example("point")),
    __FIZZBUZZ__: JSON.stringify(example("fizzbuzz")),
    __LIST__: JSON.stringify(example("list")),
    __CLASSIFY__: JSON.stringify(example("classify")),
  },
});

if (!built.success) {
  for (const log of built.logs) console.error(log);
  process.exit(1);
}

const html = readFileSync("site/index.html", "utf8").replaceAll("0.1.0-alpha.2", version);
writeFileSync("site/dist/index.html", html);
writeFileSync("site/dist/site.css", readFileSync("site/site.css"));
console.log(`site/dist ready (${version})`);
