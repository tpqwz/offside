import assert from "node:assert/strict";
import { existsSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import { describe, it } from "node:test";
import { layoutSource } from "./layout.ts";
import { PROFILES } from "./languages.ts";
import { toSpaced } from "./spaced.ts";

const ZIG = process.env.OFFSIDE_ZIG ?? "/workspace/.tools/zig-x86_64-linux-0.14.1/zig";

const SHAPES = `const std = @import("std")

const Color = enum:
    red,
    green,
    blue,

const Fail = error:
    Boom,
    Nope,

pub fn clamp(n: i32) i32:
    if (n < 0):
        return 0
    else if (n > 9):
        return 9
    else:
        return n

pub fn sum(items: []const i32) i32:
    var total: i32 = 0
    for (items) |x|:
        total += x
    return total

pub fn maybe(a: ?i32) i32:
    const n = a orelse return 0
    return n + 2

fn boom() Fail!void:
    return error.Boom

pub fn recover() void:
    defer _ = 1
    boom() catch |err|:
        _ = err
        return

pub fn main() void:
    const c = Color.red
    _ = c
    _ = clamp(3)
    _ = sum(&[_]i32{ 1, 2, 3 })
    _ = maybe(6)
    recover()
    _ = std

test "clamp":
    try std.testing.expect(clamp(12) == 9)
    try std.testing.expect(sum(&[_]i32{ 1, 2, 3 }) == 6)
    try std.testing.expect(maybe(null) == 0)
`;

const WIDER = `const std = @import("std")

const Packed = packed struct:
    a: u1
    b: u7

const Ext = extern struct:
    n: i32

const Slot = union(enum):
    int: i32
    none

const Hole = opaque:
    pub fn tag() u8:
        return 1

pub fn abs(n: i32) i32:
    const v = if (n < 0) -n else n
    return v

pub fn walk(items: []const i32) i32:
    var total: i32 = 0
    for (items, 0..) |item, i|:
        if (i == 0):
            continue
        total += item
    else:
        total += 1
    return total

pub fn scan(items: []const i32) i32:
    var i: usize = 0
    blk: while (i < items.len) : (i += 1):
        if (items[i] < 0):
            break :blk
    else:
        return 0
    return 1

pub fn pick(n: i32) i32:
    return switch (n):
        0, 1 => 0,
        2 => blk:
            break :blk 2
        3 =>:
            return 3
        else => n,

pub fn maybe(a: ?i32) i32:
    return a orelse:
        return 0

pub fn boom() error{Fail}!void:
    errdefer:
        _ = 1
    return error.Fail

pub fn guard(a: ?i32) i32:
    if (a) |v|:
        return v
    return 0

pub fn triples() i32:
    comptime var total: i32 = 0
    inline for (.{ 1, 2, 3 }) |x|:
        total += x
    return total

pub fn main() void:
    _ = Packed{ .a = 1, .b = 2 }
    _ = Ext{ .n = 3 }
    _ = Slot{ .int = 4 }
    _ = Hole.tag()
    _ = abs(-3)
    _ = walk(&[_]i32{ 4, 5 })
    _ = scan(&[_]i32{ 1, 2 })
    _ = pick(2)
    _ = maybe(null)
    _ = guard(4)
    _ = triples()
    boom() catch |err|:
        _ = err
        return
    _ = std

test "wider":
    try std.testing.expect(abs(-3) == 3)
    try std.testing.expect(walk(&[_]i32{ 4, 5 }) == 6)
    try std.testing.expect(scan(&[_]i32{ 1, 2 }) == 0)
    try std.testing.expect(scan(&[_]i32{ -1 }) == 1)
    try std.testing.expect(pick(2) == 2)
    try std.testing.expect(pick(3) == 3)
    try std.testing.expect(maybe(null) == 0)
    try std.testing.expect(guard(4) == 4)
    try std.testing.expect(triples() == 6)
`;

/** Shapes Zig 0.14.1 accepted after layout. Commas are omitted where the pass must insert them. */
const MORE: { name: string; source: string }[] = [
  {
    name: "align-decl",
    source: `const Aligned align(4) = struct:
    x: i32 align(4)

fn foo() align(1) void:
    return

export fn bar() callconv(.C) i32:
    return 1

export fn baz() linksection(".text") void:
    return

var g: i32 addrspace(.generic) = 1

const n: usize = 1
const zero align(16) = [_]u64{0} ** n

pub fn main() void:
    _ = Aligned{ .x = 1 }
    foo()
    _ = bar()
    baz()
    _ = g
    _ = zero
`,
  },
  {
    name: "else-payload",
    source: `fn fail() error{Fail}!i32:
    return error.Fail

pub fn use(a: error{Fail}!i32) i32:
    if (a) |v|:
        return v
    else |err|:
        _ = err
        return 0

pub fn use2(a: error{Fail}!i32) i32:
    if (a) |v|:
        return v
    else |err| switch (err):
        error.Fail => return 0,

pub fn main() void:
    _ = use(fail())
    _ = use2(error.Fail)
`,
  },
  {
    name: "usingnamespace",
    source: `const Inner = struct:
    pub const n: i32 = 1

const Outer = struct:
    pub usingnamespace Inner

pub fn main() void:
    _ = Outer.n
`,
  },
  {
    name: "errdefer-payload",
    source: `fn boom() error{Fail}!void:
    errdefer |err|:
        _ = err
    return error.Fail

pub fn main() void:
    boom() catch |err|:
        _ = err
        return
`,
  },
  {
    name: "enum-int",
    source: `const E = enum(u2):
    a
    b = 2

const P = packed union:
    a: u8
    b: u8

const Tup = struct:
    i32
    u8

const Def = struct:
    x: i32 = 1
    y: i32

pub fn main() void:
    _ = E.b
    _ = P{ .a = 1 }
    const t: Tup = .{ 1, 2 }
    const d: Def = .{ .y = 3 }
    _ = t
    _ = d
`,
  },
  {
    name: "switch-range",
    source: `const Slot = union(enum):
    int: i32
    none

pub fn band(n: u8) u8:
    return switch (n):
        0...2, 5, 7...9 => 1,
        else => 0,

pub fn val(s: Slot) i32:
    return switch (s):
        .int => |v| v,
        .none => 0,

const Flag = enum:
    a
    b

pub fn name(e: Flag) u8:
    return switch (e):
        inline else => |v| @intFromEnum(v),

pub fn main() void:
    _ = band(8)
    _ = val(.{ .int = 4 })
    _ = name(.a)
`,
  },
  {
    name: "for-ptr",
    source: `pub fn bump(items: []i32) void:
    outer: for (items) |*item|:
        if (item.* < 0):
            continue :outer
        item.* += 1

pub fn main() void:
    var items = [_]i32{ -1, 2 }
    bump(&items)
`,
  },
  {
    name: "value-blocks",
    source: `pub fn f(n: i32) i32:
    const x = blk:
        break :blk n
    const v = if (n < 0) -n else n
    var i: i32 = 0
    const w = while (i < n) : (i += 1):
        if (i == 3):
            break i
    else 0
    return x + v + w

pub fn main() void:
    _ = f(4)
`,
  },
  {
    name: "comptime-errors",
    source: `const A = error:
    A

const B = error:
    B

threadlocal var tls: i32 = 0

fn merge() (A || B)!void:
    return error.A

fn id(comptime T: type, value: T) T:
    return value

pub fn stuff(a: anyerror!void) void:
    comptime:
        _ = @sizeOf(i32)
        _ = @alignOf(u32)
    _ = a catch:
        return
    _ = tls
    _ = id(i32, 1)
    merge() catch:
        return

pub fn main() void:
    stuff({})
`,
  },
];

const HOSTS: { name: string; source: string }[] = [
  {
    name: "pointers",
    source: `extern "c" fn strlen(s: [*:0]const u8) callconv(.C) usize;

pub fn demo() void {
    const s: [:0]const u8 = "hi";
    const p: [*:0]const u8 = s;
    const q: [*c]u8 = null;
    var bytes: [4]u8 align(4) = .{ 0, 0, 0, 0 };
    const a: *align(4) u32 = @ptrCast(&bytes);
    const z: *allowzero u8 = @ptrFromInt(0);
    _ = p;
    _ = q;
    _ = a;
    _ = z;
    _ = strlen;
    _ = @as(i32, 1_000);
    const @"weird name" = 3;
    _ = @"weird name";
}
`,
  },
  {
    name: "docs",
    source: `//! module note

/// item note
pub fn idle() void {
    nosuspend {
        _ = 1;
    }
}
`,
  },
];

const OFFSIDE = process.env.OFFSIDE_BIN ?? "/workspace/c/build/offside";

function cStdout(args: string[], fileName: string, text: string): string {
  const dir = mkdtempSync(join(tmpdir(), "offside-c-"));
  const file = join(dir, fileName);
  writeFileSync(file, text);
  const run = spawnSync(OFFSIDE, [...args, file], { encoding: "utf8" });
  assert.equal(run.status, 0, `${run.stderr}\n${text}`);
  return run.stdout;
}

function zigTest(code: string, name: string): void {
  const dir = mkdtempSync(join(tmpdir(), "offside-zig-"));
  const file = join(dir, `${name}.zig`);
  writeFileSync(file, code);
  const run = spawnSync(ZIG, ["test", file], { encoding: "utf8" });
  if (run.status !== 0) {
    assert.fail(`zig test ${run.status}\n${run.stdout}\n${run.stderr}\n${code}`);
  }
}

describe("zig", () => {
  it("lays the sample out as Zig the compiler can read", () => {
    const { code, notes } = layoutSource(PROFILES.zig.examples[0]?.source ?? "", "zig");
    assert.deepEqual(notes, []);
    assert.match(code, /pub fn add\(a: i32, b: i32\) i32 \{/);
    assert.match(code, /const Point = struct \{/);
    assert.match(code, /x: i32,/);
    assert.match(code, /while \(i < 3\) : \(i \+= 1\) \{/);
    assert.match(code, /\\\\hello\n\s+\\\\zig\n\s+;/);
    assert.match(code, /test "add" \{/);
  });

  it("keeps a label colon and comma-separates a switch", () => {
    const { code, notes } = layoutSource(PROFILES.zig.examples[1]?.source ?? "", "zig");
    assert.deepEqual(notes, []);
    assert.match(code, /const x = blk: \{/);
    assert.match(code, /return switch \(n\) \{/);
    assert.match(code, /else => n,/);
    assert.match(code, /\};/);
  });

  it("round-trips host Zig through the spaced dialect", () => {
    for (const example of PROFILES.zig.examples) {
      const laid = layoutSource(example.source, "zig");
      const spaced = toSpaced(laid.code, "zig");
      const again = layoutSource(spaced.code, "zig");
      assert.equal(again.code, laid.code, spaced.code);
    }
  });

  it("does not treat ? as a ternary or /* as a comment", () => {
    const { code } = layoutSource(
      `pub fn bare(a: ?i32) i32:
    const n = a orelse 0
    const q = n / 2 * 3
    return q
`,
      "zig",
    );
    assert.match(code, /pub fn bare\(a: \?i32\) i32 \{/);
    assert.match(code, /const q = n \/ 2 \* 3;/);
  });

  it("closes an enum after a trailing comma and round-trips the shape sample", () => {
    const laid = layoutSource(SHAPES, "zig");
    assert.deepEqual(laid.notes, []);
    assert.match(laid.code, /blue,\n\};/);
    assert.match(laid.code, /Nope,\n\};/);
    assert.match(laid.code, /for \(items\) \|x\| \{/);
    assert.match(laid.code, /catch \|err\| \{\n\s+_ = err;\n\s+return;\n\s+\};/);
    assert.match(laid.code, /defer _ = 1;/);
    const again = layoutSource(toSpaced(laid.code, "zig").code, "zig");
    assert.equal(again.code, laid.code);
  });

  it("compiles the laid-out samples with zig", (t) => {
    if (!existsSync(ZIG)) {
      t.skip("zig compiler is not installed");
      return;
    }
    for (const example of PROFILES.zig.examples) {
      zigTest(layoutSource(example.source, "zig").code, example.id);
    }
    zigTest(layoutSource(SHAPES, "zig").code, "shapes");
    zigTest(layoutSource(WIDER, "zig").code, "wider");
    for (const sample of MORE) zigTest(layoutSource(sample.source, "zig").code, sample.name);
    for (const sample of HOSTS) zigTest(sample.source, sample.name);
  });

  it("keeps labels, payloads, error sets, and bare union fields", () => {
    const laid = layoutSource(WIDER, "zig");
    assert.deepEqual(laid.notes, []);
    assert.match(laid.code, /none,/);
    assert.match(laid.code, /for \(items, 0\.\.\) \|item, i\| \{/);
    assert.match(laid.code, /blk: while \(i < items\.len\) : \(i \+= 1\) \{/);
    assert.match(laid.code, /2 => blk: \{\n\s+break :blk 2;\n\s+\},/);
    assert.match(laid.code, /3 => \{\n\s+return 3;\n\s+\},/);
    assert.match(laid.code, /0, 1 => 0,/);
    assert.match(laid.code, /return a orelse \{\n\s+return 0;\n\s+\};/);
    assert.match(laid.code, /pub fn boom\(\) error\{Fail\}!void \{/);
    assert.match(laid.code, /errdefer \{\n\s+_ = 1;\n\s+\}/);
    assert.match(laid.code, /inline for \(\.\{ 1, 2, 3 \}\) \|x\| \{/);
    assert.doesNotMatch(laid.code, /errdefer _ = 1,/);
    const again = layoutSource(toSpaced(laid.code, "zig").code, "zig");
    assert.equal(again.code, laid.code);
  });

  it("keeps an inline struct in a return type", () => {
    const host = "pub fn point() struct { x: i32 } {\n    return .{ .x = 1 };\n}\n";
    const spaced = toSpaced(host, "zig");
    assert.match(spaced.code, /pub fn point\(\) struct \{ x: i32 \}:/);
    const back = layoutSource(spaced.code, "zig");
    assert.equal(back.code, host);
  });

  it("lays out the shapes Zig 0.14 accepts", () => {
    const expect: Record<string, RegExp[]> = {
      "align-decl": [
        /const Aligned align\(4\) = struct \{/,
        /x: i32 align\(4\),/,
        /fn foo\(\) align\(1\) void \{/,
        /export fn bar\(\) callconv\(\.C\) i32 \{/,
        /export fn baz\(\) linksection\("\.text"\) void \{/,
        /var g: i32 addrspace\(\.generic\) = 1;/,
        /const zero align\(16\) = \[_\]u64\{0\} \*\* n;/,
      ],
      "else-payload": [/else \|err\| \{/, /else \|err\| switch \(err\) \{/],
      usingnamespace: [/pub usingnamespace Inner;/],
      "errdefer-payload": [/errdefer \|err\| \{/],
      "enum-int": [/const E = enum\(u2\) \{/, /\n {4}a,/, /\n {4}b = 2,/, /const Tup = struct \{/, /\n {4}i32,/],
      "switch-range": [/0\.\.\.2, 5, 7\.\.\.9 => 1,/, /\.int => \|v\| v,/, /inline else => \|v\|/],
      "for-ptr": [/outer: for \(items\) \|\*item\| \{/, /continue :outer;/],
      "value-blocks": [/\} else 0;/, /const x = blk: \{/],
      "comptime-errors": [/fn merge\(\) \(A \|\| B\)!void \{/, /threadlocal var tls: i32 = 0;/, /comptime \{/],
    };
    for (const sample of MORE) {
      const laid = layoutSource(sample.source, "zig");
      assert.deepEqual(laid.notes, [], sample.name);
      for (const pattern of expect[sample.name] ?? []) assert.match(laid.code, pattern, sample.name);
      assert.equal(layoutSource(toSpaced(laid.code, "zig").code, "zig").code, laid.code, sample.name);
    }
    for (const sample of HOSTS) {
      const spaced = toSpaced(sample.source, "zig");
      assert.equal(layoutSource(spaced.code, "zig").code, sample.source, sample.name);
    }
  });

  it("preserves an align suffix that Zig 0.14 rejects", () => {
    const laid = layoutSource("const Aligned = struct:\n    x: i32\nalign(4)\n", "zig");
    assert.match(laid.code, /\} align\(4\);/);
    assert.equal(layoutSource(toSpaced(laid.code, "zig").code, "zig").code, laid.code);
  });

  it("matches the C port", (t) => {
    if (!existsSync(OFFSIDE)) {
      t.skip("native offside is not built");
      return;
    }
    const spaced = [
      ...PROFILES.zig.examples.map((example) => example.source),
      SHAPES,
      WIDER,
      ...MORE.map((sample) => sample.source),
      "const Aligned = struct:\n    x: i32\nalign(4)\n",
    ];
    for (const source of spaced) {
      const laid = layoutSource(source, "zig");
      const again = toSpaced(laid.code, "zig").code;
      assert.equal(cStdout(["emit", "--plain"], "in.szig", source), laid.code);
      assert.equal(cStdout(["space"], "in.zig", laid.code), again);
    }
    for (const sample of HOSTS) {
      const spacedHost = toSpaced(sample.source, "zig").code;
      assert.equal(cStdout(["space"], "in.zig", sample.source), spacedHost);
      assert.equal(cStdout(["emit", "--plain"], "in.szig", spacedHost), sample.source);
    }
  });
});
