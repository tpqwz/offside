import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, it } from "node:test";
import { layoutSource } from "./layout.ts";
import { toSpaced } from "./spaced.ts";

const TCC = process.env.TCC ?? "/workspace/.tools/tcc/bin/tcc";

const PROGRAM = `#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "fail %s\\n", #cond); return 1; } } while (0)
#define DOUBLE(x) ((x) + (x))

typedef struct Point:
    int x
    int y
Point

typedef enum Color:
    RED
    GREEN = 2
    BLUE
Color

union Num:
    int i
    float f

struct Flags:
    unsigned a : 1
    unsigned b : 3
    unsigned split :
        1

struct Node:
    int v
    struct Node *next

#if 0
static int skipped(void):
    return 1
#else
static int kept(void):
    return 2
#endif

int add(int a, int b):
    return a + b

int apply(int (*fp)(int, int), int a, int b):
    return fp(a, b)

int load(int *restrict p):
    return *p

int varargs(int n, ...):
    va_list ap
    va_start(ap, n)
    int s = 0
    for (int i = 0; i < n; i++):
        s += va_arg(ap, int)
    va_end(ap)
    return s

int jumps(void):
    int n = 0
    goto skip
    n = 1
    skip:
        n += 2
    return n

int repeats(void):
    int i = 0
    do:
        i++
    while (i < 3)
    return i

int classify(int n):
    switch (n):
        case 1:
            return 10
        case 2:
            return 20
        default:
            return 0

int sign(int n):
    if (n < 0):
        return -1
    else if (n > 0):
        return 1
    else:
        return 0

static int counter = 0

int bump(void):
    counter++
    return counter

int first(void):
    return 1

#include <stdlib.h>

int second(void):
    return abs(-2)

int main(void):
    Point p = {.x = 3, .y = 4}
    Point q = (Point){5, 6}
    int xs[] = {1, 2, 3, 4}
    int ys[2][2] = {
        {1, 2},
        {3, 4}
    }
    const char *msg =
        "hel"
        "lo"
    int n = 4
    int vla[n]
    int sum = 0
    struct:
        int x
    spot
    struct Flags bits = {0}
    union Num num
    struct Node a
    struct Node b
    int a1 = 1, b1 = 2
    long long big = 1LL
    int z = (1, 2, 3)
    volatile int sink = 0

    CHECK(add(2, 3) == 5)
    CHECK(apply(add, 2, 3) == 5)
    CHECK(p.x == 3 && p.y == 4)
    CHECK(q.x == 5 && q.y == 6)
    CHECK(xs[3] == 4)
    CHECK(ys[1][1] == 4)
    CHECK(strcmp(msg, "hello") == 0)
    CHECK(GREEN == 2 && BLUE == 3 && RED == 0)
    for (int i = 0; i < n; i++):
        vla[i] = i
        sum += vla[i]
    CHECK(sum == 6)
    CHECK(varargs(3, 1, 2, 3) == 6)
    CHECK(jumps() == 2)
    CHECK(repeats() == 3)
    CHECK(classify(1) == 10 && classify(2) == 20 && classify(9) == 0)
    CHECK(sign(-4) == -1 && sign(0) == 0 && sign(8) == 1)
    CHECK(DOUBLE(4) == 8)
    CHECK(kept() == 2)
    CHECK(sizeof(Point) == 2 * sizeof(int))
    CHECK((int)1.9 == 1)
    CHECK(0xFF == 255)
    CHECK('A' == 65 && '\\n' == 10)
    CHECK(z == 3)
    CHECK(a1 + b1 == 3)
    CHECK(big == 1)
    CHECK(first() == 1 && second() == 2)
    spot.x = 4
    CHECK(spot.x == 4)
    bits.a = 1
    bits.b = 2
    bits.split = 1
    CHECK(bits.a == 1 && bits.b == 2 && bits.split == 1)
    num.i = 42
    CHECK(num.i == 42)
    a.v = 1
    b.v = 2
    a.next = &b
    b.next = 0
    CHECK(a.next->v == 2)
    {
        int local = 7
        CHECK(load(&local) == 7)
    }
    if (0):
        (void)0
    sink = 1
    CHECK(sink == 1)
    CHECK(bump() == 1 && bump() == 2)
    /* multi
       line */
    puts("ok")
    return 0
`;

describe("C99 via tcc", () => {
  it("keeps an include between functions and commas enumerators", () => {
    const { code } = layoutSource(
      `int first(void):
    return 1

#include <stdlib.h>

int second(void):
    return 2
`,
      "c",
    );
    const close = code.indexOf("}");
    const inc = code.indexOf("#include <stdlib.h>");
    const second = code.indexOf("int second");
    assert.ok(close !== -1 && close < inc && inc < second, code);

    const enumerated = layoutSource(
      `typedef enum Color:
    RED
    GREEN = 2
    BLUE
Color
`,
      "c",
    );
    assert.equal(
      enumerated.code,
      `typedef enum Color {
    RED,
    GREEN = 2,
    BLUE,
} Color;
`,
    );
  });

  it("compiles and runs a C99 program with tcc", () => {
    const { code, notes } = layoutSource(PROGRAM, "c");
    const dir = mkdtempSync(join(tmpdir(), "offside-"));
    const file = join(dir, "corpus.c");
    writeFileSync(file, code);
    const run = spawnSync(TCC, ["-std=c99", "-run", file], { encoding: "utf8" });
    if (run.status !== 0 || !run.stdout.includes("ok")) {
      assert.fail(
        `tcc status ${run.status}\n${run.stdout}\n${run.stderr}\nnotes ${JSON.stringify(notes)}\n${code}`,
      );
    }
  });

  it("compiles the rest of C99: splits, K&R, fallthrough, initializers, macros", () => {
    const { code, notes } = layoutSource(REST, "c");
    const dir = mkdtempSync(join(tmpdir(), "offside-"));
    const file = join(dir, "rest.c");
    writeFileSync(file, code);
    const run = spawnSync(TCC, ["-std=c99", "-run", file], { encoding: "utf8" });
    if (run.status !== 0 || !run.stdout.includes("ok")) {
      assert.fail(
        `tcc status ${run.status}\n${run.stdout}\n${run.stderr}\nnotes ${JSON.stringify(notes)}\n${code}`,
      );
    }
  });

  it("round-trips host C back through the spaced dialect", () => {
    for (const source of [PROGRAM, REST]) {
      const host = layoutSource(source, "c").code;
      const spaced = toSpaced(host, "c");
      const again = layoutSource(spaced.code, "c");
      const dir = mkdtempSync(join(tmpdir(), "offside-back-"));
      const file = join(dir, "back.c");
      writeFileSync(file, again.code);
      const run = spawnSync(TCC, ["-std=c99", "-run", file], { encoding: "utf8" });
      if (run.status !== 0 || !run.stdout.includes("ok")) {
        assert.fail(
          `tcc status ${run.status}\n${run.stdout}\n${run.stderr}\nspace ${JSON.stringify(spaced.notes)}\nlayout ${JSON.stringify(again.notes)}\n${spaced.code}\n---\n${again.code}`,
        );
      }
    }
  });
});

const REST = `#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <setjmp.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "fail %s\\n", #cond); return 1; } } while (0)
#define ADD(a, b) \\
    ((a) + (b))
#define FIRST(a, ...) (a)
#define GLUE(a, b) a ## b
#define FLAG 3

#if defined(FLAG)
static int flagged(void):
    return FLAG
#endif
#undef FLAG

#if 0
static int no(void):
    return 0
#elif 1
static int yes(void):
    return 7
#else
static int neither(void):
    return 0
#endif

static int GLUE(pref, ix) = 11

extern int extra(int x)

int extra(int x):
    return x + 1

static inline int
twice(int x):
    return x << 1

int
wide(int a,
    int b):
    return a - b

int legacy(a, b)
    int a
    int b
:
    return a + b

typedef int (*BinOp)(int, int)

typedef int (*Cmp)(const char *,
    const char *)

int mul(int a, int b):
    return a * b

struct Point:
    int x
    int y

struct Flex:
    int n
    int a[]

struct Pair:
    char c
    int n

int take(struct Point p):
    return p.x + p.y

int sum(const int *a, int n):
    int s = 0
    for (int i = 0, j = n; i < j; i++):
        s += a[i]
    return s

int fall(int n):
    int x = 0
    switch (n):
        case 1:
            x += 1
        case 2:
            x += 2
            break
        default:
            x = 7
    return x

int skipped(void):
    int s = 0
    for (int i = 0; i < 5; i++):
        if (i == 2):
            continue
        s += i
    return s

int forever(void):
    int i = 0
    for (;;):
        i++
        if (i == 3):
            break
    return i

int nested(void):
    int i = 0
    int j = 0
    do:
        do:
            j++
        while (j < 2)
        i++
    while (i < 2):
    return j

int jumped(void):
    jmp_buf jb
    if (setjmp(jb) == 0):
        longjmp(jb, 4)
    return 4

int cmp(const void *p, const void *q):
    const int a = *(const int *)p
    const int b = *(const int *)q
    return a - b

int box = 5

int *ref(void):
    return &box

#pragma pack(push, 1)
#pragma pack(pop)

int main(void):
    BinOp op = mul
    struct Point p = {
        .x = 3
        .y = 4
    }
    int designated[] = {
        [0] = 1
        [2] = 9
    }
    int grid[2][2] = {
        {1, 2}
        {3, 4}
    }
    int (*row)[2] = grid
    int nums[] = {10, 20, 30}
    int *cursor = nums
    int stmt = ({
        int n
        n = 4
        n + 1
    })
    int joined = 1 \\
        + 6
    _Bool flag = 2
    float f = 1.5f
    double d = 2.25
    unsigned short wrapped = 65535
    auto int automatic = 1
    register int reg = 2
    int v = 3
    const int *cp = &v
    int *const pc = &v
    enum:
        OFF
        ON
    state
    char text[16]
    int arr[] = {3, 1, 2}
    int m
    int n
    struct Flex *flex = malloc(sizeof(struct Flex) + 2 * sizeof(int))
    int acc = 0
    void *vp = &v

    CHECK(flex != 0)
    flex->n = 2
    flex->a[0] = 4
    flex->a[1] = 5
    CHECK(flex->a[0] + flex->a[1] == 9)
    free(flex)

    wrapped++
    m = n = 4
    sprintf(text, "%d", 42)
    qsort(arr, 3, sizeof arr[0], cmp)
    for (int i = 0, j = 3; i < j; i++, j--):
        acc += i + j

    CHECK(extra(4) == 5)
    CHECK(twice(6) == 12)
    CHECK(wide(9, 2) == 7)
    CHECK(legacy(2, 3) == 5)
    CHECK(op(3, 4) == 12)
    CHECK(flagged() == 3 && yes() == 7 && prefix == 11)
    CHECK(FIRST(4, 5, 6) == 4 && ADD(2, 5) == 7)
    CHECK(fall(1) == 3 && fall(2) == 2 && fall(9) == 7)
    CHECK(skipped() == 8 && forever() == 3 && nested() == 3)
    CHECK(jumped() == 4)
    CHECK(p.x + p.y == 7)
    CHECK(designated[0] == 1 && designated[1] == 0 && designated[2] == 9)
    CHECK(row[1][0] == 3)
    CHECK(take((struct Point){8, 9}) == 17)
    CHECK(sum(nums, 3) == 60)
    CHECK(*(cursor + 2) == 30 && cursor[1] == 20)
    CHECK(stmt == 5 && joined == 7)
    CHECK(flag == 1)
    CHECK(f > 1.0f && d > 2.0)
    CHECK(wrapped == 0)
    CHECK(automatic + reg == 3)
    CHECK(*cp == 3 && *pc == 3)
    state = ON
    CHECK(state == ON && OFF == 0)
    CHECK(strcmp(text, "42") == 0)
    CHECK(arr[0] == 1 && arr[2] == 3)
    CHECK(m == 4 && n == 4 && acc == 6)
    CHECK(*ref() == 5)
    CHECK(*(int *)vp == 3)
    CHECK(offsetof(struct Pair, n) >= 1)
    CHECK('\\0' == 0 && '\\x41' == 'A' && 010 == 8 && 0x10 == 16)
    CHECK((1 && 0) == 0 && (0 || 2) == 1 && !0)
    CHECK(((0xF0 >> 4) == 15) && ((1 << 3) == 8) && ((5 & 3) == 1))
    CHECK((5 | 2) == 7 && (5 ^ 1) == 4 && ~0u > 0)
    CHECK((Cmp)0 == 0)
    if (0):
        (void)0
    else:
        (void)0
    puts("ok")
    return 0
`;

