import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, it } from "node:test";
import { layoutSource } from "./layout.ts";
import { toSpaced } from "./spaced.ts";

const GCC = process.env.CC ?? "/usr/bin/gcc";

function runGcc(code: string, extra: string[] = []): void {
  const dir = mkdtempSync(join(tmpdir(), "offside-cstd-"));
  const file = join(dir, "std.c");
  writeFileSync(file, code);
  const run = spawnSync(GCC, ["-std=gnu2x", "-Wall", "-Wextra", ...extra, file, "-o", join(dir, "std")], {
    encoding: "utf8",
  });
  if (run.status !== 0) {
    assert.fail(`gcc status ${run.status}\n${run.stdout}\n${run.stderr}\n${code}`);
  }
  const exec = spawnSync(join(dir, "std"), [], { encoding: "utf8" });
  if (exec.status !== 0 || !exec.stdout.includes("ok")) {
    assert.fail(`run ${exec.status}\n${exec.stdout}\n${exec.stderr}\n${code}`);
  }
}

const MODERN = `#include <stdio.h>

#define FEATURE 1
#define ID(x, ...) ((x) __VA_OPT__(+) __VA_ARGS__)

struct __attribute__((packed)) Pair:
    char tag
    int value

[[nodiscard]]
static __attribute__((cold)) int id(int x):
    return x

#if 0
int skipped = 1
#elifdef FEATURE
int kept = 3
#else
int other = 4
#endif

int main(void):
    _Static_assert(sizeof(int) >= 2, "int")
    struct Pair p = { .tag = 1, .value = 2 }
    int empty[2] = {}
    empty[0] = 7
    int n = 1'000 + 0b11
    int g = _Generic(n, int: 5, default: 9)
    int e = n ?: 4
    int s = ({
        int t
        t = 6
        t
    })
    int inner(int y):
        return y + 1
    switch (empty[0]):
        case 1 ... 6:
            [[fallthrough]]
        case 7:
            break
        default:
            return 3
    _Alignas(8) int aligned = 8
    _Atomic int atom = 1
    __auto_type q = 2
    const char *u = u8"Z"
    asm volatile ("nop")
    int mac = ID(3, 4)
    if (n != 1003 || g != 5 || e != 1003 || s != 6 || inner(1) != 2):
        return 1
    if (id(1) != 1 || p.tag != 1 || p.value != 2 || aligned != 8 || atom != 1 || q != 2):
        return 2
    if (u[0] != 'Z' || kept != 3 || mac != 7 || empty[0] != 7):
        return 3
    puts("ok")
    return 0
`;

describe("C11 through C23 and common extensions", () => {
  it("compiles spaced C with gcc -std=gnu2x", () => {
    const laid = layoutSource(MODERN, "c");
    assert.equal(laid.notes.length, 0, JSON.stringify(laid.notes));
    runGcc(laid.code);
  });

  it("round-trips that program back through the spaced dialect", () => {
    const host = layoutSource(MODERN, "c").code;
    const spaced = toSpaced(host, "c");
    const again = layoutSource(spaced.code, "c");
    runGcc(again.code);
  });

  it("keeps a host attribute tail on a tagged struct", () => {
    const spaced = toSpaced("struct S { int x; } __attribute__((packed));\n", "c");
    assert.match(spaced.code, /struct S:/);
    assert.match(spaced.code, /__attribute__\(\(packed\)\)/);
    const again = layoutSource(spaced.code, "c").code;
    assert.match(again, /\}\s*__attribute__\(\(packed\)\);/);
    runGcc(`#include <stdio.h>\n${again}\nint main(void) { struct S s = {1}; if (s.x != 1) return 1; puts("ok"); return 0; }\n`);
  });

  it("accepts digraph braces as real blocks", () => {
    const spaced = toSpaced("int main(void) <% puts(\"ok\"); return 0; %>\n", "c");
    const again = layoutSource(spaced.code, "c");
    runGcc(again.code);
  });

  it("keeps a semicolon-less statement macro off the next if", () => {
    const host = `#include <stdio.h>
static int trace = 0;
static void sink(const char *fmt, int v) { (void)fmt; (void)v; }
#define IOTRACE(A) if (trace) { sink A; }
static int zeroJournalHdr(int doTruncate) {
  int rc = 0;
  IOTRACE(("JZEROHDR %p\\n", doTruncate))
  if (doTruncate) {
    rc = 1;
  } else {
    rc = 2;
  }
  for (; rc < 0;) {
    rc = 0;
  }
  return rc;
}
int main(void) {
  if (zeroJournalHdr(1) != 1) return 1;
  if (zeroJournalHdr(0) != 2) return 2;
  puts("ok");
  return 0;
}
`;
    const spaced = toSpaced(host, "c");
    assert.match(spaced.code, /if \(doTruncate\):\n\s+rc = 1/);
    assert.match(spaced.code, /else:\n\s+rc = 2/);
    const again = layoutSource(spaced.code, "c");
    assert.match(again.code, /IOTRACE\(\("JZEROHDR %p\\n", doTruncate\)\);/);
    assert.match(again.code, /if \(doTruncate\) \{\s*rc = 1;\s*\} else \{\s*rc = 2;\s*\}/);
    runGcc(again.code);
  });

  it("keeps a #if 0 / #else function body around the live arm", () => {
    const host = `#include <stdio.h>
static int sqlite3PagerGet(int flags) {
#if 0
  int rc;
  rc = flags;
  if (rc) {
    return rc;
  }
  return rc;
#else
  return flags;
#endif
}
int main(void) {
  if (sqlite3PagerGet(4) != 4) return 1;
  puts("ok");
  return 0;
}
`;
    const spaced = toSpaced(host, "c");
    const again = layoutSource(spaced.code, "c");
    assert.match(again.code, /#if 0[\s\S]*#else\s*return flags;\s*#endif\s*\}/);
    runGcc(again.code);
  });

  it("does not attach an else that is inside #if 0", () => {
    const host = `#include <stdio.h>
static int read_it(int *p) {
  if (*p == 0) {
    *p = 1;
  }
#if 0
  else {
    *p = 2;
  }
#endif
  if (*p != 1) return 0;
  return *p;
}
static int after(int x) { return x + 1; }
int main(void) {
  int n = 0;
  if (read_it(&n) != 1) return 1;
  if (after(3) != 4) return 2;
  puts("ok");
  return 0;
}
`;
    const spaced = toSpaced(host, "c");
    assert.equal((spaced.code.match(/static int after/g) ?? []).length, 1);
    assert.match(spaced.code, /#if 0[\s\S]*else[\s\S]*#endif/);
    assert.doesNotMatch(spaced.code, /else:\n\s+if \(\*p != 1\)/);
    const again = layoutSource(spaced.code, "c");
    runGcc(again.code);
  });

  it("does not semicolon a do-while condition that continues", () => {
    const host = `#include <stdio.h>
static int busy(int rc) {
  do {
    rc--;
  } while (rc > 0 &&
           rc > -3);
#ifdef EXTRA
  rc = 0;
#endif
  return rc;
}
int main(void) {
  if (busy(2) != 0) return 1;
  puts("ok");
  return 0;
}
`;
    const again = layoutSource(toSpaced(host, "c").code, "c");
    assert.match(again.code, /while \(rc > 0 &&\s*rc > -3\);/);
    runGcc(again.code);
  });

  it("keeps a directive between an if and its unbraced body", () => {
    const host = `#include <stdio.h>
static int oom(int ctx) {
  int rc = 0;
#ifdef KEEP
  if (ctx == 0)
#endif
    rc = 1;
  return rc;
}
int main(void) {
  if (oom(1) != 1) return 1;
  puts("ok");
  return 0;
}
`;
    const again = layoutSource(toSpaced(host, "c").code, "c");
    assert.match(again.code, /if \(ctx == 0\)\s*#endif\s*rc = 1;/);
    assert.doesNotMatch(again.code, /if \(ctx == 0\);/);
    runGcc(again.code);
  });

  it("keeps a conditional comma and a semicolon that follows #endif", () => {
    const host = `#include <stdio.h>
static const char *mode(int wal) {
  static const char * const name[] = {
    "memory"
#ifndef OMIT_WAL
    , "wal"
#endif
  };
  return name[wal];
}
static int flags(void) {
  int f = 1
#ifdef EXTRA
    | 2
#endif
  ;
  return f;
}
int main(void) {
  if (mode(0)[0] != 'm') return 1;
  if (flags() != 1) return 2;
  puts("ok");
  return 0;
}
`;
    const again = layoutSource(toSpaced(host, "c").code, "c");
    assert.match(again.code, /"memory"\s*#ifndef OMIT_WAL\s*, "wal"/);
    assert.match(again.code, /#endif\s*;/);
    assert.doesNotMatch(again.code, /\| 2;/);
    runGcc(again.code);
  });

  it("keeps a nested struct declarator on the type", () => {
    const host = `#include <stdio.h>
struct Info {
  struct Item {
    int column;
  } *items;
  struct Flag {
    int bit;
  } fg;
  int n;
};
static struct Stat {
  int now;
  int mx;
} stat = { 0, 0 };
static union Slot {
  int m;
  char pad[8];
} slots[] = {
  { .m = 2 },
  { .m = 3 },
};
int main(void) {
  struct Info info;
  struct Item item = { 4 };
  info.items = &item;
  info.fg.bit = 1;
  info.n = 1;
  if (info.items->column != 4 || info.fg.bit != 1 || info.n != 1) return 1;
  if (stat.now != 0 || slots[1].m != 3) return 2;
  puts("ok");
  return 0;
}
`;
    const spaced = toSpaced(host, "c");
    const again = layoutSource(spaced.code, "c");
    assert.match(again.code, /\} \*items;/);
    assert.match(again.code, /\} fg;/);
    assert.match(again.code, /\} stat = \{ 0, 0 \};/);
    assert.match(again.code, /\} slots\[\] = \{/);
    runGcc(again.code, ["-Wall", "-Wextra", "-Werror"]);
  });

  it("keeps a numeric array declarator, including one continued after a comma", () => {
    const host = `#include <stdio.h>
struct IdxRemCtx {
  int nSlot;
  struct IdxRemSlot {
    int eType;
    char *z;
  } aSlot[1];
};
struct ShellState {
  struct AuxDb {
    int db;
    int mFlgs;
  } aAuxDb[5],
    *pAuxDb;
  struct DotCmdLine {
    int nArg;
  } dot;
};
int main(void) {
  struct ShellState s;
  s.aAuxDb[0].db = 1;
  s.pAuxDb = &s.aAuxDb[0];
  s.dot.nArg = 2;
  struct IdxRemCtx rem;
  rem.nSlot = 1;
  rem.aSlot[0].eType = 3;
  if (s.pAuxDb->db != 1 || s.dot.nArg != 2 || rem.aSlot[0].eType != 3) return 1;
  puts("ok");
  return 0;
}
`;
    const again = layoutSource(toSpaced(host, "c").code, "c");
    assert.match(again.code, /\} aSlot\[1\];/);
    assert.match(again.code, /\} aAuxDb\[5\],/);
    assert.doesNotMatch(again.code, /aAuxDb\[5\],\s*;/);
    assert.match(again.code, /\*pAuxDb;/);
    assert.match(again.code, /\} dot;/);
    runGcc(again.code, ["-Wall", "-Wextra", "-Werror"]);
  });

  it("preserves C23 shapes this gcc cannot compile yet", () => {
    const shapes = [
      "enum E : unsigned char:\n    A\n    B\n",
      "int *p = nullptr\n",
      "constexpr int n = 1\n",
      "typeof(1 + 1) x = 2\n",
      "_BitInt(16) b = 1\n",
      "#embed \"payload.bin\"\n",
    ];
    for (const source of shapes) {
      const host = layoutSource(source, "c");
      assert.equal(host.notes.filter((note) => /unclosed|skipped/i.test(note.message)).length, 0, `${source}\n${host.code}`);
      const back = toSpaced(host.code, "c");
      const again = layoutSource(back.code, "c");
      assert.match(again.code, /enum E|nullptr|constexpr|typeof|_BitInt|#embed/);
    }
    const enumerated = layoutSource("enum E : unsigned char:\n    A\n    B\n", "c").code;
    assert.match(enumerated, /enum E : unsigned char \{/);
    assert.match(enumerated, /\};/);
  });
});
