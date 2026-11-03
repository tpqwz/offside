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
});
