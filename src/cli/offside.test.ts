import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { describe, it } from "node:test";
import { parseArgs } from "./offside.ts";
import { layoutSource } from "../lib/layout/layout.ts";
import { renderHost } from "../lib/layout/host.ts";

const CLI = path.resolve("src/cli/offside.ts");

function offside(args: string[], cwd = process.cwd()) {
  return spawnSync("node", ["--experimental-strip-types", CLI, ...args], {
    cwd,
    encoding: "utf8",
  });
}

describe("offside cli", () => {
  it("parses compiler flags and program args", () => {
    const parsed = parseArgs(["run", "hello.csp", "in.txt", "--", "-O2", "--", "more"]);
    assert.equal(parsed.ok, true);
    if (!parsed.ok) return;
    assert.equal(parsed.plan.file, "hello.csp");
    assert.deepEqual(parsed.plan.ccFlags, ["-O2"]);
    assert.deepEqual(parsed.plan.programArgs, ["in.txt", "more"]);
  });

  it("does not break a backslash continuation with #line", () => {
    const result = layoutSource("#define ADD(a, b) \\\n    ((a) + (b))\nint main(void):\n    return ADD(1, 2)\n", "c");
    const host = renderHost(result, "/tmp/add.csp", true);
    assert.match(host.code, /#define ADD\(a, b\) \\\n {4}\(\(a\) \+ \(b\)\)/);
    assert.doesNotMatch(host.code, /\\\n#line/);
  });

  it("runs a spaced C file and maps a diagnostic to the spaced line", () => {
    const dir = mkdtempSync(path.join(tmpdir(), "offside-cli-"));
    const file = path.join(dir, "hello.csp");
    writeFileSync(
      file,
      `int a(void):
    return 1

int main(int argc, char **argv):
    if (argc > 1):
        puts(argv[1])
    else:
        puts("ok")
    return 0
`,
    );
    const ran = offside(["run", file]);
    assert.equal(ran.status, 0, ran.stderr);
    assert.equal(ran.stdout, "ok\n");

    const flagged = offside(["run", file, "hi", "--", "-DUNUSED=1"]);
    assert.equal(flagged.status, 0, flagged.stderr);
    assert.equal(flagged.stdout, "hi\n");

    const bad = path.join(dir, "bad.csp");
    writeFileSync(
      bad,
      `int a(void):
    return 1

int main(void):
    return missing
`,
    );
    const failed = offside(["run", bad]);
    assert.notEqual(failed.status, 0);
    assert.ok(failed.stderr.includes(`${bad}:5:`), failed.stderr);
    assert.doesNotMatch(failed.stderr, /offside-.*\.c:6:/);

    const emitted = offside(["emit", "--plain", file]);
    assert.equal(emitted.status, 0, emitted.stderr);
    assert.match(emitted.stdout, /int main\(int argc, char \*\*argv\) \{/);
    assert.doesNotMatch(emitted.stdout, /#line/);
  });

  it("runs spaced Java and JavaScript", () => {
    const dir = mkdtempSync(path.join(tmpdir(), "offside-cli-"));
    const java = path.join(dir, "Greeter.sjava");
    writeFileSync(
      java,
      `class Greeter:
    static int a():
        return 1

    public static void main(String[] args):
        System.out.println(args.length > 0 ? args[0] : "ok")
`,
    );
    const javaRun = offside(["run", java]);
    assert.equal(javaRun.status, 0, javaRun.stderr);
    assert.equal(javaRun.stdout, "ok\n");

    const javaBad = path.join(dir, "Broken.sjava");
    writeFileSync(
      javaBad,
      `class Broken:
    static int a():
        return 1

    public static void main(String[] args):
        System.out.println(missing)
`,
    );
    const javaFail = offside(["run", javaBad]);
    assert.notEqual(javaFail.status, 0);
    assert.match(javaFail.stderr, /Broken\.sjava:6:/);
    assert.doesNotMatch(javaFail.stderr, /Broken\.java:/);

    const js = path.join(dir, "greet.sjs");
    writeFileSync(
      js,
      `function a():
    return 1

function b():
    return "ok"

console.log(b())
`,
    );
    const jsRun = offside(["run", js]);
    assert.equal(jsRun.status, 0, jsRun.stderr);
    assert.equal(jsRun.stdout, "ok\n");

    const jsBad = path.join(dir, "bad.sjs");
    writeFileSync(
      jsBad,
      `function a():
    return 1

function b():
    return nope

b()
`,
    );
    const jsFail = offside(["run", jsBad]);
    assert.notEqual(jsFail.status, 0);
    assert.match(jsFail.stderr, /bad\.sjs:5:/);
    assert.doesNotMatch(jsFail.stderr, /bad\.js:/);
  });

  it("turns host C into the spaced dialect and can run it", () => {
    const dir = mkdtempSync(path.join(tmpdir(), "offside-cli-"));
    const file = path.join(dir, "hello.c");
    writeFileSync(
      file,
      `#include <stdio.h>
int main(void) {
    if (1) {
        puts("ok");
    } else {
        puts("no");
    }
    return 0;
}
`,
    );
    const spaced = offside(["space", file]);
    assert.equal(spaced.status, 0, spaced.stderr);
    assert.match(spaced.stdout, /int main\(void\):/);
    assert.match(spaced.stdout, /if \(1\):/);
    assert.doesNotMatch(spaced.stdout, /puts\("ok"\);/);

    const ran = offside(["run", file]);
    assert.equal(ran.status, 0, ran.stderr);
    assert.equal(ran.stdout, "ok\n");

    const emitted = offside(["emit", file]);
    assert.equal(emitted.status, 2);
    assert.match(emitted.stderr, /offside space/);
  });
});
