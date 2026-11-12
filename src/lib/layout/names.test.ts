import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { classifyFile } from "./host.ts";
import { classifyName, outputName, spacedExampleName } from "./names.ts";

const KNOWN = [
  "hello.sc",
  "engine.h.sc",
  "hello.c",
  "engine.h",
  "Greeter.sjava",
  "Greeter.java",
  "app.sjs",
  "app.js",
  "app.mjs",
  "demo.spas",
  "demo.pas",
  "demo.pp",
  "point.szig",
  "point.zig",
  "Box.skt",
  "Box.kt",
];

describe("file names", () => {
  it("agrees with the command about every claimed extension", () => {
    for (const file of KNOWN) {
      const named = classifyName(file);
      const kind = classifyFile(file);
      assert.ok(named, file);
      assert.equal(named.lang, kind.lang, file);
      assert.equal(named.host, kind.host, file);
    }
  });

  it("leaves an unknown extension unnamed", () => {
    assert.equal(classifyName("notes.txt"), null);
    assert.equal(classifyName("Makefile"), null);
  });

  it("pairs spaced C with host C, and a header with itself", () => {
    assert.equal(outputName("hello.sc", "c", true, ""), "hello.c");
    assert.equal(outputName("hello.c", "c", false, ""), "hello.sc");
    assert.equal(outputName("engine.h.sc", "c", true, ""), "engine.h");
    assert.equal(outputName("engine.h", "c", false, ""), "engine.h.sc");
    assert.equal(outputName("Hello.SC", "c", true, ""), "Hello.c");
  });

  it("pairs the other five dialects", () => {
    assert.equal(outputName("app.sjs", "javascript", true, ""), "app.js");
    assert.equal(outputName("app.mjs", "javascript", false, ""), "app.sjs");
    assert.equal(outputName("demo.spas", "pascal", true, ""), "demo.pas");
    assert.equal(outputName("demo.pp", "pascal", false, ""), "demo.spas");
    assert.equal(outputName("point.szig", "zig", true, ""), "point.zig");
    assert.equal(outputName("Box.skt", "kotlin", true, ""), "Box.kt");
    assert.equal(outputName("Box.kt", "kotlin", false, ""), "Box.skt");
  });

  it("names Java from the class, then the stem", () => {
    const host = "public final class App {\n}\n";
    assert.equal(outputName("Greeter.sjava", "java", true, host), "App.java");
    assert.equal(outputName("App.java", "java", false, host), "App.sjava");
    assert.equal(outputName("Greeter.sjava", "java", true, "int x;\n"), "Greeter.java");
  });

  it("names an example button from its id", () => {
    assert.equal(spacedExampleName("hello", "c"), "hello.sc");
    assert.equal(spacedExampleName("program", "c"), "program.sc");
    assert.equal(spacedExampleName("unit", "pascal"), "unit.spas");
  });
});
