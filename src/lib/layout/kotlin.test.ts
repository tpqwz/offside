import assert from "node:assert/strict";
import { existsSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import { describe, it } from "node:test";
import { layoutSource } from "./layout.ts";
import { lex } from "./lex.ts";
import { PROFILES } from "./languages.ts";
import { toSpaced } from "./spaced.ts";

const KOTLINC = process.env.OFFSIDE_KOTLIN ?? "/workspace/.tools/kotlinc/bin/kotlinc";
const OFFSIDE = process.env.OFFSIDE_BIN ?? "/workspace/c/build/offside";

/** Spaced samples. A few files kotlinc accepts are not a Kotlin corpus. */
const MORE = [
  {
    name: "nullable",
    source: `fun add(a: Int?): Int:
    return (a ?: 0) + 1

fun main():
    println(add(null))
    println(add(2))
`,
  },
  {
    name: "when-block",
    source: `fun label(n: Int): String:
    return when (n):
        0 ->:
            println("zero")
            "empty"
        1 -> "one"
        else -> "many"

fun main():
    println(label(0))
`,
  },
  {
    name: "lambda",
    source: `fun main():
    val f = { x: Int -> x + 1 }
    val g = { x: Int ->
        println(x)
        x + 1
    }
    println(f(1))
    println(g(2))
    listOf(1, 2).forEach { println(it) }
`,
  },
  {
    name: "templates",
    source: `fun main():
    val name = "ada"
    val raw = """
        hello, $name
        still $name
        """.trimIndent()
    println(raw)
    println("sum is \${1 + 1}")
    println(name !is String)
`,
  },
  {
    name: "try-if",
    source: `fun pick(flag: Boolean): Int:
    if (flag):
        return 1
    else:
        return 0

fun read(text: String): Int:
    return try:
        text.toInt()
    catch (e: NumberFormatException):
        0

fun main():
    println(pick(true))
    println(read("4"))
    println(read("no"))
`,
  },
  {
    name: "nullable-return",
    source: `fun label(name: String?): String?:
    return name ?: "none"

fun main():
    println(label(null))
`,
  },
  {
    name: "elvis-break",
    source: `fun pick(name: String?): String:
    return name
        ?: "none"

fun main():
    println(pick(null))
`,
  },
  {
    name: "annotation",
    source: `annotation class Mark

@Mark
fun greet(): String:
    return "ok"

fun main():
    println(greet())
`,
  },
  {
    name: "enum-class",
    source: `enum class Color:
    RED
    GREEN
    BLUE;
    fun paint(): String:
        return name.lowercase()

fun main():
    println(Color.BLUE.paint())
`,
  },
  {
    name: "enum-expr",
    source: `enum class Color:
    RED
    GREEN
    BLUE;
    fun paint() = name.lowercase()

fun main():
    println(Color.RED.paint())
`,
  },
  {
    name: "enum-body",
    source: `enum class Color:
    RED:
        override fun toString(): String:
            return "red"
    GREEN
    BLUE;
    fun paint() = name.lowercase()

fun main():
    println(Color.RED.toString())
    println(Color.BLUE.paint())
`,
  },
  {
    name: "enum-ctor",
    source: `enum class Shade(val n: Int):
    RED(1)
    GREEN(2);
    fun twice() = n * 2

fun main():
    println(Shade.RED.twice())
`,
  },
  {
    name: "types-and-inherit",
    source: `open class Parent(val n: Int):
    open fun doubled() = n * 2

class Child(n: Int) : Parent(n):
    override fun doubled() = n * 3

fun main():
    println(Child(4).doubled())
`,
  },
  {
    name: "members",
    source: `class Box(text: String):
    val shown: Int = text.length
        get() = field
    var tag: String = text
        set(value):
            field = value.trim()
    init:
        println(shown)
    fun bump() = shown

fun main():
    val box = Box("hi")
    box.tag = "  x "
    println(box.tag)
    println(box.bump())
`,
  },
  {
    name: "where-line",
    source: `fun <T> longer(a: T, b: T): T
    where T : Comparable<T>:
        return if (a > b) a else b

fun main():
    println(longer("aa", "b"))
`,
  },
  {
    name: "secondary",
    source: `class Box(val n: Int):
    constructor(text: String) : this(text.length)

fun main():
    println(Box("abcd").n)
`,
  },
  {
    name: "fn-type",
    source: `fun apply(f: (Int) -> Int, n: Int): Int:
    return f(n)

fun main():
    println(apply({ x -> x + 1 }, 3))
`,
  },
  {
    name: "extension",
    source: `class Box(var n: Int)

fun Box.bump():
    n += 1

fun main():
    val box = Box(1)
    box.bump()
    println(box.n)
`,
  },
];

const HOSTS = [
  {
    name: "trailing-lambda",
    source: `fun main() {
    val n = listOf("layout", "tokens").map { it.length }.sum()
    println(n)
}
`,
  },
  {
    name: "object-expr",
    source: `fun main() {
    val o = object {
        var n = 0
        fun bump(): Int {
            n += 1
            return n
        }
    }
    println(o.bump())
}
`,
  },
  {
    name: "nested-comment",
    source: `fun main() {
    /* outer /* inner */ still */
    println(1)
}
`,
  },
  {
    name: "nullable-host",
    source: `fun label(name: String?): String? {
    return name ?: "none"
}

fun main() {
    println(label(null))
}
`,
  },
  {
    name: "annotated-host",
    source: `annotation class Mark

@Mark
fun greet(): String {
    return "ok"
}

fun main() {
    println(greet())
}
`,
  },
  {
    name: "h-enum",
    source: `enum class Color {
    RED, GREEN;
    fun paint() = name.lowercase()
}

fun main() {
    println(Color.GREEN.paint())
}
`,
  },
  {
    name: "h-enum-body",
    source: `enum class Color {
    RED {
        override fun toString() = "red"
    },
    GREEN;
    fun paint() = name.lowercase()
}

fun main() {
    println(Color.RED.toString())
    println(Color.GREEN.paint())
}
`,
  },
  {
    name: "h-inherit",
    source: `open class Parent(val n: Int) {
    open fun doubled() = n * 2
}

class Child(n: Int) : Parent(n) {
    override fun doubled() = n * 3
}

fun main() {
    println(Child(4).doubled())
}
`,
  },
  {
    name: "h-members",
    source: `class Box(text: String) {
    val shown: Int = text.length
        get() = field
    var tag: String = text
        set(value) {
            field = value.trim()
        }
    init {
        println(shown)
    }
    fun bump() = shown
}

fun main() {
    val box = Box("hi")
    box.tag = "  x "
    println(box.tag)
    println(box.bump())
}
`,
  },
  {
    name: "h-where",
    source: `fun <T> longer(a: T, b: T): T
    where T : Comparable<T> {
    return if (a > b) a else b
}

fun main() {
    println(longer(1, 2))
}
`,
  },
  {
    name: "h-lazy",
    source: `fun main() {
    val n: Int by lazy { 1 + 2 }
    println(n)
}
`,
  },
];

function cStdout(args: string[], fileName: string, text: string): string {
  const dir = mkdtempSync(join(tmpdir(), "offside-c-"));
  const file = join(dir, fileName);
  writeFileSync(file, text);
  const run = spawnSync(OFFSIDE, [...args, file], { encoding: "utf8" });
  assert.equal(run.status, 0, `${run.stderr}\n${text}`);
  return run.stdout;
}

function kotlinCompile(code: string, name: string): void {
  const dir = mkdtempSync(join(tmpdir(), "offside-kt-"));
  const file = join(dir, `${name}.kt`);
  writeFileSync(file, code);
  const run = spawnSync(KOTLINC, ["-d", join(dir, "out.jar"), file], { encoding: "utf8" });
  if (run.status !== 0) {
    assert.fail(`kotlinc ${run.status}\n${run.stdout}\n${run.stderr}\n${code}`);
  }
}

describe("kotlin", () => {
  it("lays the sample out as Kotlin the compiler can read", () => {
    const { code, notes } = layoutSource(PROFILES.kotlin.examples[0]?.source ?? "", "kotlin");
    assert.deepEqual(notes, []);
    assert.match(code, /fun greet\(name: String\?\): String \{/);
    assert.match(code, /return "hello, \$name";/);
    assert.match(code, /val n = items\.map \{ it\.length \}\.sum\(\);/);
    assert.match(code, /when \(items\.size\) \{/);
    assert.match(code, /0 -> "empty";/);
    assert.match(code, /else -> "many";/);
  });

  it("keeps elvis, templates, and a value object", () => {
    const { code, notes } = layoutSource(PROFILES.kotlin.examples[1]?.source ?? "", "kotlin");
    assert.deepEqual(notes, []);
    assert.match(code, /class Box \{/);
    assert.match(code, /val on = flag \?: false;/);
    assert.match(code, /val word = "sum is \$\{1 \+ 1\}";/);
    assert.match(code, /val box = object \{/);
    assert.match(code, /\};/);
    assert.match(code, /return box\.bump\(\);/);
  });

  it("does not treat ? as a ternary and keeps a string interpolation whole", () => {
    const { code } = layoutSource("fun add(a: Int?): Int:\n    return a ?: 0\n", "kotlin");
    assert.match(code, /fun add\(a: Int\?\): Int \{/);
    assert.match(code, /return a \?: 0;/);
    const lexed = lex('"sum is ${1 + 1}" /* /* */ */ \'a\'\n', PROFILES.kotlin);
    assert.deepEqual(lexed.notes, []);
    assert.equal(lexed.tokens.filter((tok) => tok.kind !== "comment").map((tok) => tok.text).join(" "), '"sum is ${1 + 1}" \'a\'');
    assert.equal(lexed.tokens.find((tok) => tok.kind === "comment")?.text, "/* /* */ */");
  });

  it("round-trips the spaced samples", () => {
    const sources = [
      ...PROFILES.kotlin.examples.map((example) => example.source),
      ...MORE.map((sample) => sample.source),
    ];
    for (const source of sources) {
      const laid = layoutSource(source, "kotlin");
      assert.deepEqual(laid.notes, []);
      const again = layoutSource(toSpaced(laid.code, "kotlin").code, "kotlin");
      assert.equal(again.code, laid.code);
    }
  });

  it("reads host Kotlin back into a form that lays out stably", () => {
    for (const sample of HOSTS) {
      const spaced = toSpaced(sample.source, "kotlin");
      assert.deepEqual(spaced.notes, []);
      const laid = layoutSource(spaced.code, "kotlin");
      const again = layoutSource(toSpaced(laid.code, "kotlin").code, "kotlin");
      assert.equal(again.code, laid.code, sample.name);
    }
    const lambda = toSpaced(HOSTS[0]!.source, "kotlin");
    assert.match(lambda.code, /map \{ it\.length \}/);
    const objectSpaced = toSpaced(HOSTS[1]!.source, "kotlin");
    assert.match(objectSpaced.code, /object:/);
    assert.match(objectSpaced.code, /fun bump\(\): Int:/);
  });

  it("compiles the laid-out samples with kotlinc", (t) => {
    if (!existsSync(KOTLINC)) {
      t.skip("kotlinc is not installed");
      return;
    }
    for (const example of PROFILES.kotlin.examples) {
      kotlinCompile(layoutSource(example.source, "kotlin").code, example.id);
    }
    for (const sample of MORE) kotlinCompile(layoutSource(sample.source, "kotlin").code, sample.name);
    for (const sample of HOSTS) {
      kotlinCompile(layoutSource(toSpaced(sample.source, "kotlin").code, "kotlin").code, sample.name);
    }
  });

  it("matches the C port", (t) => {
    if (!existsSync(OFFSIDE)) {
      t.skip("native offside is not built");
      return;
    }
    const spaced = [...PROFILES.kotlin.examples.map((example) => example.source), ...MORE.map((sample) => sample.source)];
    for (const source of spaced) {
      const laid = layoutSource(source, "kotlin");
      const again = toSpaced(laid.code, "kotlin").code;
      assert.equal(cStdout(["emit", "--plain"], "in.skt", source), laid.code);
      assert.equal(cStdout(["space"], "in.kt", laid.code), again);
    }
    for (const sample of HOSTS) {
      const spacedHost = toSpaced(sample.source, "kotlin").code;
      assert.equal(cStdout(["space"], "in.kt", sample.source), spacedHost);
      assert.equal(cStdout(["emit", "--plain"], "in.skt", spacedHost), layoutSource(spacedHost, "kotlin").code);
    }
  });
});
