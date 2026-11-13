import assert from "node:assert/strict";
import { describe, it } from "node:test";
import { highlight } from "./highlight.ts";
import { PROFILES } from "./languages.ts";

describe("highlight", () => {
  it("colors a keyword, a string, a comment, and a block colon", () => {
    const source = 'int main(void):\n    puts("hello"); // done\n';
    const hues = highlight(source, PROFILES.c);
    assert.equal(hues.map((hue) => hue.text).join(""), source);
    assert.ok(hues.some((hue) => hue.text === "int" && hue.cls === "tok-key"));
    assert.ok(hues.some((hue) => hue.text === ":" && hue.cls === "tok-colon"));
    assert.ok(hues.some((hue) => hue.text === '"hello"' && hue.cls === "tok-str"));
    assert.ok(hues.some((hue) => hue.cls === "tok-cmt" && hue.text.includes("done")));
  });

  it("leaves a ternary colon on the same line uncolored", () => {
    const hues = highlight("int n = a ? 1 : 0;\n", PROFILES.c);
    assert.equal(hues.some((hue) => hue.cls === "tok-colon"), false);
    assert.ok(hues.some((hue) => hue.text.includes(":") && hue.cls === undefined));
  });

  it("keeps an inserted brace out of the keyword color", () => {
    const source = "int main(void) {\n";
    const brace = source.indexOf("{");
    const hues = highlight(source, PROFILES.c, [[brace, brace + 1]]);
    assert.equal(hues.map((hue) => hue.text).join(""), source);
    const key = hues.find((hue) => hue.text === "int");
    const mark = hues.find((hue) => hue.text === "{");
    assert.equal(key?.cls, "tok-key");
    assert.equal(key?.inserted, undefined);
    assert.equal(mark?.inserted, true);
    assert.equal(mark?.cls, undefined);
  });

  it("colors a preprocessor line as one directive", () => {
    const hues = highlight("#include <stdio.h>\n", PROFILES.c);
    assert.ok(hues.some((hue) => hue.cls === "tok-dir" && hue.text.startsWith("#include")));
  });
});
