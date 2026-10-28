import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, it } from "node:test";
import { layoutSource } from "./layout.ts";
import { toSpaced } from "./spaced.ts";

const TCC = process.env.TCC ?? "/workspace/.tools/tcc/bin/tcc";

describe("host to spaced", () => {
  it("turns a C function, struct, and if into colons", () => {
    const { code, notes } = toSpaced(
      `#include <stdio.h>

struct Point {
    int x;
    int y;
};

int main(void) {
    struct Point p = {1, 2};
    if (p.x > 0) {
        printf("%d\\n", p.x);
    } else {
        printf("zero\\n");
    }
    return 0;
}
`,
      "c",
    );
    assert.deepEqual(notes, []);
    assert.equal(
      code,
      `#include <stdio.h>

struct Point:
    int x
    int y

int main(void):
    struct Point p = {1, 2}
    if (p.x > 0):
        printf("%d\\n", p.x)
    else:
        printf("zero\\n")
    return 0
`,
    );
    assert.match(layoutSource(code, "c").code, /struct Point \{/);
  });

  it("keeps a typedef enum tail and indents a label inside the function", () => {
    const { code } = toSpaced(
      `typedef enum { RED, GREEN = 2 } Color;
int main(void) {
    goto skip;
    return 1;
skip:
    return GREEN;
}
`,
      "c",
    );
    assert.match(code, /typedef enum:\n {4}RED\n {4}GREEN = 2\nColor\n/);
    assert.match(code, /\n {4}skip:\n {4}return GREEN\n/);
  });

  it("turns Java and JavaScript blocks into colons", () => {
    const java = toSpaced(
      `class Greeter {
    public static void main(String[] args) {
        for (String s : args) {
            System.out.println(s);
        }
    }
}
`,
      "java",
    );
    assert.match(java.code, /class Greeter:\n {4}public static void main\(String\[\] args\):\n {8}for \(String s : args\):/);
    assert.match(layoutSource(java.code, "java").code, /class Greeter \{/);

    const js = toSpaced(
      `const summarize = (items) => {
    let n = 0;
    for (const item of items) {
        n += item.length;
    }
    return n;
};
`,
      "javascript",
    );
    assert.match(js.code, /const summarize = \(items\) =>:\n {4}let n = 0\n {4}for \(const item of items\):\n {8}n \+= item\.length\n {4}return n\n/);
    assert.match(layoutSource(js.code, "javascript").code, /const summarize = \(items\) => \{/);
  });

  it("puts a whole switch arm under the case and keeps a braced scope", () => {
    const host = `int fall(int n) {
    int x = 0;
    switch (n) {
    case 1:
        x += 1;
        x += 2;
        break;
    case 2: {
        int y = 3;
        x = y;
        break;
    }
    default:
        x = 7;
        break;
    }
    return x;
}
`;
    const { code } = toSpaced(host, "c");
    assert.match(code, /case 1:\n {12}x \+= 1\n {12}x \+= 2\n {12}break\n {8}case 2:\n {12}\{\n {16}int y = 3\n/);
    assert.match(code, /default:\n {12}x = 7\n {12}break\n/);

    const scoped = `#include <stdio.h>
int main(void) {
    int n = 1;
    switch (n) {
    case 1: {
        int x = 4;
        n = x;
        break;
    }
    case 2: {
        int x = 9;
        n = x;
        break;
    }
    default:
        n = 0;
        break;
    }
    return n == 4 ? 0 : 1;
}
`;
    const spaced = toSpaced(scoped, "c");
    const again = layoutSource(spaced.code, "c");
    const dir = mkdtempSync(join(tmpdir(), "offside-case-"));
    const file = join(dir, "case.c");
    writeFileSync(file, again.code);
    const run = spawnSync(TCC, ["-std=c99", "-run", file], { encoding: "utf8" });
    assert.equal(run.status, 0, `${run.stderr}\n${spaced.code}\n---\n${again.code}`);
  });

  it("keeps a comment that sat next to a removed semicolon", () => {
    const after = toSpaced(`int main(void) {\n    return 0; /* done */\n}\n`, "c");
    assert.match(after.code, /return 0 \/\* done \*\//);
    const before = toSpaced(`int main(void) {\n    return 0 /* done */;\n}\n`, "c");
    assert.match(before.code, /return 0 \/\* done \*\//);
    const between = toSpaced(
      `int main(void)\n    /* entry */\n{\n    return 0;\n}\n`,
      "c",
    );
    assert.match(between.code, /int main\(void\):\n {4}\/\* entry \*\//);
  });

  it("leaves an anonymous aggregate braced when the declarator has an initializer", () => {
    const { code, notes } = toSpaced(`struct { int x; } s = {1}, *p;\n`, "c");
    assert.match(code, /struct \{ int x; \} s = \{1\}, \*p;/);
    assert.equal(notes.length, 1);
    assert.match(notes[0]?.message ?? "", /initializer/);
  });
});
