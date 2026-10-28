import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, it } from "node:test";
import { layoutSource } from "./layout.ts";
import { lex } from "./lex.ts";
import { PROFILES } from "./languages.ts";
import { toSpaced } from "./spaced.ts";

const FPC = process.env.FPC ?? "/workspace/.tools/fpc/bin/ppcx64";
const FU = process.env.FPC_UNITS ?? "/workspace/.tools/fpc/lib/fpc/3.2.2/units/x86_64-linux/rtl";

function fpc(code: string, fileName: string, run: boolean): void {
  const dir = mkdtempSync(join(tmpdir(), "offside-pas-"));
  const file = join(dir, fileName);
  writeFileSync(file, code);
  const binary = join(dir, "prog");
  const compiled = spawnSync(FPC, [`-Fu${FU}`, file, ...(run ? [`-o${binary}`] : [])], {
    encoding: "utf8",
    cwd: dir,
  });
  if (compiled.status !== 0) {
    assert.fail(`fpc status ${compiled.status}\n${compiled.stdout}\n${compiled.stderr}\n${code}`);
  }
  if (!run) return;
  const exec = spawnSync(binary, [], { encoding: "utf8" });
  if (exec.status !== 0 || !exec.stdout.includes("ok")) {
    assert.fail(`run ${exec.status}\n${exec.stdout}\n${exec.stderr}\n${code}`);
  }
}

function codeWords(source: string): string[] {
  return lex(source, PROFILES.pascal).tokens
    .filter((tok) => tok.kind !== "comment" && tok.kind !== "directive")
    .map((tok) => tok.text.toLowerCase());
}

function spacedProgram(mode: string, body: string, header = "program P:"): string {
  return `{$mode ${mode}}\n\n${header}\n${body}`;
}

const OBJ = `{$mode objfpc}{$H+}

program Demo:

    uses SysUtils

    type
        TPoint = record:
            X: Integer
            Y: Integer

    function Sign(N: Integer): Integer:
        if N < 0 then:
            Sign := -1
        else:
            Sign := 1

    function Nested: Integer:
        function Inner: Integer:
            Inner := 5
        Nested := Inner

    procedure UseTry:
        var
            N: Integer
        try:
            try:
                N := 1
            except:
                on E: Exception do:
                    N := 2
        finally:
            if N = 0 then:
                N := 3
        if N <> 1 then:
            Halt(1)

    procedure UseAsm:
        asm:
            nop

    var
        P: TPoint
        I: Integer
        N: Integer

    P.X := 3
    P.Y := Sign(-P.X)
    for I := 1 to 3 do:
        N := I
    N := 0
    repeat:
        N := N + 1
    until N = 2
    case N of:
        1:
            Halt(1)
        2:
            N := Nested
        else:
            Halt(1)
    while N > 0 do:
        N := N - 1
    UseTry
    UseAsm
    if P.Y <> -1 then:
        Halt(1)
    WriteLn('ok')
`;

describe("Lazarus Pascal modes", () => {
  it("compiles the spaced objfpc program", () => {
    const laid = layoutSource(OBJ, "pascal");
    fpc(laid.code, "demo.pas", true);
  });

  it("compiles the playground program and unit", () => {
    fpc(layoutSource(PROFILES.pascal.examples[0]?.source ?? "", "pascal").code, "demo.pas", true);
    fpc(layoutSource(PROFILES.pascal.examples[1]?.source ?? "", "pascal").code, "Boxes.pas", false);
  });

  it("compiles a small program in every Lazarus mode", () => {
    const modes = ["objfpc", "delphi", "delphiunicode", "fpc", "tp", "macpas", "iso", "extendedpascal"];
    for (const mode of modes) {
      const source = spacedProgram(
        mode,
        `    WriteLn('ok')\n`,
        mode === "iso" || mode === "extendedpascal" ? "program P(output):" : "program P:",
      );
      fpc(layoutSource(source, "pascal").code, "p.pas", true);
    }
  });

  it("compiles macpas otherwise and delphi generics from spaced source", () => {
    const mac = spacedProgram(
      "macpas",
      `    var
        N: Integer
    N := 2
    case N of:
        1:
            N := 1
        otherwise:
            N := 9
    if N <> 9 then:
        Halt(1)
    WriteLn('ok')
`,
    );
    fpc(layoutSource(mac, "pascal").code, "mac.pas", true);

    const delphi = `{$mode delphi}

program Gen:

    type
        TBox<T> = class:
            Value: T
            procedure SetValue(V: T)

    procedure TBox<T>.SetValue(V: T):
        Value := V

    var
        B: TBox<Integer>

    B := TBox<Integer>.Create
    B.SetValue(3)
    if B.Value <> 3 then:
        Halt(1)
    WriteLn('ok')
`;
    fpc(layoutSource(delphi, "pascal").code, "gen.pas", true);

    const generic = `{$mode objfpc}

program Gen:

    type
        generic TBox<T> = class:
            Value: T

    var
        B: specialize TBox<Integer>

    B := specialize TBox<Integer>.Create
    B.Value := 4
    if B.Value <> 4 then:
        Halt(1)
    WriteLn('ok')
`;
    fpc(layoutSource(generic, "pascal").code, "gen.pas", true);
  });

  it("round-trips host Pascal through the spaced dialect", () => {
    const hosts = [
      `{$mode objfpc}
program P;
procedure Work(var S: ansistring); {$IF FPC_FULLVERSION > 030300} noinline; {$ENDIF}
begin
  S := 'ok';
end;
var
  S: ansistring;
begin
  Work(S);
  if S <> 'ok' then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
type
  TObj = class
    procedure Work; virtual; overload;
  end;
procedure TObj.Work;
begin
end;
function Twice(N: Integer): Integer; inline;
begin
  Twice := N * 2;
end;
function Missing: Integer; cdecl; external name 'missing';
begin
  if Twice(3) <> 6 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
type
  TObj = class
    procedure Enter; {$IFDEF CPUX86_64}virtual;{$ELSE}overload;{$ENDIF}
  end;
procedure TObj.Enter;
begin
end;
function Q(constref G: Pointer; out Obj): LongInt; {$IFDEF WINDOWS}stdcall{$ELSE}cdecl{$ENDIF};
begin
  Result := 0;
end;
function Align(const P: Pointer): Pointer; inline;
begin
{$IFDEF CPUX86_64}
  Result := P;
{$ELSE}
  Result := P;
{$ENDIF}
end;
begin
  if Align(nil) <> nil then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Pick(const Env: string): string;
begin
  if Env = 'GNOME' then
    Result := 'nautilus'
  else if Env = 'KDE' then
    Result := 'dolphin'
  else if Env = 'XFCE' then
    Result := 'thunar' // trailing note
  else if Env = 'NEMO' then
    // sitting between the arms
    Result := 'nemo'
  else
    Result := 'xdg';
end;
begin
  if Pick('GNOME') <> 'nautilus' then Halt(1);
  if Pick('KDE') <> 'dolphin' then Halt(1);
  if Pick('NEMO') <> 'nemo' then Halt(1);
  if Pick('other') <> 'xdg' then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}{$modeswitch typehelpers}
program P;
type
  TKind = (One, Two);
  TKindHelper = type helper for TKind
    function Name: string;
  end;
function TKindHelper.Name: string;
begin
  if Self = One then Result := 'one' else Result := 'two';
end;
var
  K: TKind;
begin
  K := One;
  if K.Name <> 'one' then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
type
  TRef = object
    FRef: record
      case Byte of
        0: (Ptr: Pointer);
        1: (Handle: PtrUInt);
    end;
  end;
  TBox = class
  private const
    Marker = 1;
  public
    function Value: Integer;
  end;
function TBox.Value: Integer;
begin
  Result := Marker;
end;
procedure Outer;
var
  Cmp: function(A, B: Integer): Integer of object;
  N: Integer;
  procedure Inner;
  begin
    N := 3;
  end;
begin
  N := 0;
  Inner;
  if N <> 3 then Halt(1);
end;
var
  B: TBox;
begin
  Outer;
  B := TBox.Create;
  if B.Value <> 1 then Halt(1);
  B.Free;
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Sign(N: Integer): Integer;
begin
  if N < 0 then
    Sign := -1
  else
    Sign := 1;
end;
begin
  if Sign(-2) <> -1 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode delphi}
program P;
type
  TBox<T> = class
    Value: T;
    procedure SetValue(V: T);
  end;
procedure TBox<T>.SetValue(V: T);
begin
  Value := V;
end;
var
  B: TBox<Integer>;
begin
  B := TBox<Integer>.Create;
  B.SetValue(3);
  if B.Value <> 3 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}{$H+}
program P;
type
  generic TBox<T> = class
  protected
    type
      TNode = class
      public
        Data: T;
      end;
    function Make: TNode;
  public
    Value: T;
  end;
function TBox.Make: TNode;
var
  continue: boolean;
begin
  continue := False;
  if continue then Halt(1);
  Result := TNode.Create;
  Result.Data := Default(T);
end;
begin
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
var
  N: Integer;
begin
  N := 1;
  case N of
    1: begin end;
    2: N := 0;
    4: begin
      N := 7;
    end;
  end;
  if N <> 1 then Halt(1);
  N := 4;
  case N of
    4: begin
      N := 7;
    end;
  end;
  if N <> 7 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
var
  K, N: Integer;
begin
  K := 0;
  N := 0;
  case K of
    0: begin
      N := 1;
      case N of
        1: N := 5;
        2: N := 9;
      end;
    end;
    1: N := 4;
  end;
  if N <> 5 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
const
  Fonts: array[0..1] of record
    Value: Integer;
    Name: string;
  end = (
    (Value: 1; Name: 'a'),
    (Value: 2; Name: 'b')
  );
begin
  if Fonts[1].Value <> 2 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
var
  C: Char;
  N: Integer;
begin
  C := ' ';
  N := 0;
  case C of
    ' ', #9: ;
    'a': N := 1;
  else
    N := 2;
  end;
  if N <> 0 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
begin
  if ('ok ' + {$I %FPCVERSION%}) = '' then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
type
  TRec = record
    case Integer of
      0: (A: Byte);
      1: (B: Word);
  end;
var
  R: TRec;
begin
  R.A := 1;
  if R.A <> 1 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Compare(const A, B: Integer): Integer;
begin
  if A > B then
    Result := 1
  else if A < B then
    Result := -1
  else
    Result := 0;
end;
begin
  if Compare(2, 1) <> 1 then Halt(1);
  if Compare(1, 2) <> -1 then Halt(1);
  if Compare(2, 2) <> 0 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
procedure Outer;
  procedure Inner;
  begin
  end;
begin
end;
var N: Integer;
begin
  N := 0;
  repeat
    N := N + 1
  until N = 2;
  try
    try
      N := 1;
    except
      N := 0;
    end;
  finally
    if N <> 1 then Halt(1);
  end;
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
procedure Alpha;
begin
end;

 procedure Beta;
begin
  writeln('ok');
end;

function Kind(N: Integer): Integer;
begin
  case Abs(N) of
    0: Kind := 0;
  else
  end;
  { outer { inner Halt(9); } still a comment }
  Kind := 1;
end;

begin
  Beta;
  if Kind(0) <> 1 then Halt(1);
end.
`,
      `{$mode objfpc}
program P;
function Pick(ReadBackslash: boolean): string;
begin
  if ReadBackslash then
    begin
    if false then
    begin
      Result := 'a';
    end else begin
      // next character is already a normal character
    end;
  end else begin
    Result := '\\';
  end;
  if false then
    // comment only
  else
    Result := Result;
end;
begin
  if Pick(false) <> '\\' then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
const
  S: array[0..1] of string = (#194#128, 'a'#10'b');
begin
  if S[0] <> #194#128 then Halt(1);
  if S[1] <> 'a'#10'b' then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
type
  PInt = ^Integer;
const
  S = 'MaskEdit Internal Error'^m'Range';
var
  N: integer;
  Ptr: PInt;
function Kind(K: char): integer;
begin
  case K of
    ^C: Kind := 1;
    ^H, #32..#126: Kind := 2;
  else
    Kind := 0;
  end;
end;
begin
  if Pos(#13, S) <> 24 then Halt(1);
  if Pos('Range', S) <> 25 then Halt(1);
  if Kind(^C) <> 1 then Halt(1);
  if Kind(^H) <> 2 then Halt(1);
  if Kind('A') <> 2 then Halt(1);
  N := 4;
  Ptr := @N;
  if Ptr^ <> 4 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
type
  TRec = class
  public
    A: Integer;
    {$IFDEF NEVER}
    B: Integer;
    {$ENDIF}
  end;
var
  R: TRec;
begin
  R := TRec.Create;
  R.A := 1;
  if R.A <> 1 then Halt(1);
  R.Free;
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
type
  TGraphicsColor = {$ifdef UseSystemUITypes}System.UITypes.TColor{$else}-$7FFFFFFF-1..$7FFFFFFF{$endif};
var
  C: TGraphicsColor;
begin
  C := 1;
  if C <> 1 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Check(p: char): boolean;
begin
  if {$IFDEF UseWideString}
     (p = 'A')
     {$ELSE}
     (p = 'B')
     {$ENDIF}
     or (p = 'C') then
  begin
    Result := True;
  end
  {$IFDEF UseWideString}
  else if p = 'D' then
  begin
    Result := True;
  end
  {$ENDIF}
  else
  begin
    Result := False;
  end;
end;
begin
  if not Check('B') then Halt(1);
  if not Check('C') then Halt(1);
  if Check('A') then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Find(Kind: integer): integer;
var
  I: integer;
begin
  case Kind of
    1:
      for I := 1 to 1 do
      begin
        Find := 2;
      end;
  else
    Find := 3;
  end;
end;
begin
  if Find(1) <> 2 then Halt(1);
  if Find(9) <> 3 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
type
  TOld = object
  end deprecated;
var
  O: TOld;
begin
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
{$IFDEF NEVER}
procedure OnlyDummy;
begin
end;
{$ELSE}
function Pick(g1, g2: boolean): boolean;
begin
  if g1 then
    Pick := g2
  else
    Pick := not g2;
end;
{$ENDIF}
begin
  if Pick(false, true) then Halt(1);
  if not Pick(true, true) then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function CodePointSize(p: char): integer;
begin
  {$IFDEF ReallyUseUTF16}
  if p = 'A' then
    CodePointSize := 2
  else
    CodePointSize := 1
  {$ELSE}
  CodePointSize := 3;
  {$ENDIF}
end;
begin
  if CodePointSize('B') <> 3 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function AbsPath(const Filename, Base: string): string;
begin
  if Filename = '' then
    Result := Filename
  {$IFDEF Windows}
  else if Filename = '\\' then
    Result := Base + Filename
  {$ENDIF}
  else
    Result := Base + Filename;
end;
begin
  if AbsPath('a', '/tmp/') <> '/tmp/a' then Halt(1);
  if AbsPath('', '/tmp/') <> '' then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Threads: integer;
{$IF defined(windows)}
begin
  Threads := 2;
end;
{$ELSEIF defined(linux)}
begin
  Threads := 4;
end;
{$ELSE}
begin
  Threads := 1;
end;
{$ENDIF}
begin
  if Threads <> 4 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
{$DEFINE UseWideString}
function Decode(var OutCnt: integer): integer; stdcall;
{$IFDEF UseUTF8}
var
  cnt: integer;
begin
  cnt := OutCnt;
  Decode := cnt;
end;
{$ENDIF}
{$IFDEF UseWideString}
begin
  Decode := OutCnt + 1;
end;
{$ENDIF}
var
  n: integer;
begin
  n := 3;
  if Decode(n) <> 4 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
procedure Paint(p: pointer);
{$IFDEF CPUI386} assembler; {$ASMMODE INTEL}
asm
  nop
end;
{$ELSE}
begin
  if p = nil then Halt(1);
end;
{$ENDIF}
begin
  Paint(pointer(1));
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Resolve(const Name: string): string;
{$ifdef windows}
  function Drive(const Path: string): boolean; inline;
  begin
    Drive := false;
  end;
{$endif}
begin
  Resolve := Name;
end;
begin
  if Resolve('a') <> 'a' then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Conv(x: integer): integer;
{$IFDEF windows}
stdcall;
{$ELSE}
cdecl;
{$ENDIF}
begin
  Conv := x + 1;
end;
begin
  if Conv(1) <> 2 then Halt(1);
  writeln('ok');
end.
`,
    ];
    for (const host of hosts) {
      const spaced = toSpaced(host, "pascal");
      const again = layoutSource(spaced.code, "pascal");
      const back = toSpaced(again.code, "pascal");
      assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
      fpc(again.code, "back.pas", true);
    }
  });

  it("keeps an else statement outside the ifdef that ends before it", () => {
    const hosts = [
      `{$mode objfpc}
program P;
function Adjust(Source: integer): integer;
begin
  if Source <> 0 then
    begin
{$ifdef storevariant}
    if Source < 0 then
      Source := 0
    else
{$endif}
      Source := Source;
    if Source = -5 then Halt(1);
    end;
  Adjust := Source;
end;
begin
  if Adjust(1) <> 1 then Halt(1);
{$ifdef storevariant}
  if Adjust(-3) <> 0 then Halt(1);
{$else}
  if Adjust(-3) <> -3 then Halt(1);
{$endif}
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Pick(N: integer): integer;
begin
{$ifdef darwin}
  if N < 0 then
    N := 1
  else
{$endif}
    N := N + 1;
  Pick := N;
end;
begin
{$ifdef darwin}
  if Pick(-4) <> 1 then Halt(1);
{$endif}
  if Pick(2) <> 3 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function IndexOf(Color: integer): integer;
begin
  if Color <> 0 then
  begin
{$ifdef DefineCLXColors}
    case Color of
      1: IndexOf := 10;
    else
{$endif}
      IndexOf := Color;
{$ifdef DefineCLXColors}
    end;
{$endif}
  end else
    IndexOf := -1;
end;
begin
  if IndexOf(0) <> -1 then Halt(1);
{$ifdef DefineCLXColors}
  if IndexOf(1) <> 10 then Halt(1);
{$endif}
  if IndexOf(5) <> 5 then Halt(1);
  writeln('ok');
end.
`,
      `{$mode objfpc}
program P;
function Adjust(Source: integer): integer;
begin
  if Source <> 0 then
  begin
{$ifdef storevariant}
    if Source < 0 then
      Source := 0
    else
{$endif}
    begin
      Source := Source;
      if Source = 0 then Halt(1);
    end;
  end;
  Adjust := Source;
end;
begin
  if Adjust(1) <> 1 then Halt(1);
{$ifdef storevariant}
  if Adjust(-3) <> 0 then Halt(1);
{$else}
  if Adjust(-3) <> -3 then Halt(1);
{$endif}
  writeln('ok');
end.
`,
    ];
    for (const host of hosts) {
      const spaced = toSpaced(host, "pascal");
      const again = layoutSource(spaced.code, "pascal");
      const back = toSpaced(again.code, "pascal");
      assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
      assert.match(again.code, /else\s*\{\$endif\}/i, again.code);
      assert.doesNotMatch(again.code, /else\s+begin\s*\{\$endif\}/i, again.code);
      fpc(again.code, "off.pas", true);
      fpc(`{$define storevariant}{$define darwin}{$define DefineCLXColors}\n${again.code}`, "on.pas", true);
    }
  });

  it("keeps both arms of a then-statement that is split by ifdef", () => {
    const host = `{$mode objfpc}
program P;
function Pick(NilValue: boolean): string;
begin
  if NilValue then
{$ifdef storevariant}
    Result := 'N'
{$else}
    Result := 'E'
{$endif}
  else
    Result := 'V';
end;
begin
{$ifdef storevariant}
  if Pick(true) <> 'N' then Halt(1);
{$else}
  if Pick(true) <> 'E' then Halt(1);
{$endif}
  if Pick(false) <> 'V' then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /then begin\s*\{\$ifdef storevariant\}\s*Result := 'N';\s*\{\$else\}\s*Result := 'E';\s*\{\$endif\}/i, again.code);
    assert.doesNotMatch(again.code, /Result := 'N' Result/, again.code);
    fpc(again.code, "off.pas", true);
    fpc(`{$define storevariant}\n${again.code}`, "on.pas", true);
  });

  it("keeps a semicolon-terminated then arm and its else arm in one if", () => {
    const host = `{$mode objfpc}
program P;
function Shift(Meta: boolean): string;
begin
  Result := '';
  if Meta then
    {$IFDEF LCLcarbon}
    Result := 'cmd';
    {$ELSE}
    Result := 'meta';
    {$ENDIF}
  if not Meta then Result := Result + 'x';
end;
begin
{$ifdef LCLcarbon}
  if Shift(true) <> 'cmd' then Halt(1);
{$else}
  if Shift(true) <> 'meta' then Halt(1);
{$endif}
  if Shift(false) <> 'x' then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /\{\$endif\}\s*end;/i, again.code);
    assert.doesNotMatch(again.code, /end;\s*\{\$else\}\s*Result := 'meta'/i, again.code);
    fpc(again.code, "off.pas", true);
    fpc(`{$define LCLcarbon}\n${again.code}`, "on.pas", true);
  });

  it("keeps an extdecl calling convention on the routine that has the body", () => {
    const host = `{$mode objfpc}{$macro on}
{$define extdecl := cdecl}
program P;
function Add(A, B: integer): integer; extdecl;
begin
  Add := A + B;
end;
begin
  if Add(1, 2) <> 3 then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    assert.match(again.code, /extdecl;\s*begin/i, again.code);
    assert.match(spaced.code, /extdecl:/i, spaced.code);
    fpc(again.code, "extdecl.pas", true);
  });

  it("keeps an else-body endif inside the else, and an outer endif outside it", () => {
    const warn = `{$mode objfpc}
program P;
procedure Touch(Updating: boolean);
begin
  if not Updating then begin
    {$ifdef dbgDBGrid}writeln('start');{$endif}
    writeln('lock');
  end
  else
    {$ifdef dbgDBGrid}writeln('warn');{$endif}
end;
begin
  Touch(false);
  Touch(true);
  writeln('ok');
end.
`;
    const arms = `{$mode objfpc}
program P;
function Home(Skip: boolean): string;
begin
  if Skip then
    Home := 'missing'
  else
{$ifdef unix}
    Home := 'home'
            + '/app';
{$else}
    Home := 'app.json';
{$endif}
end;
begin
{$ifdef unix}
  if Home(false) <> 'home/app' then Halt(1);
{$else}
  if Home(false) <> 'app.json' then Halt(1);
{$endif}
  if Home(true) <> 'missing' then Halt(1);
  writeln('ok');
end.
`;
    const outer = `{$mode objfpc}
program P;
begin
{$ifdef OUTER}
  if false then
    writeln('no')
  else
    {$ifdef INNER}writeln('in');{$endif}
{$endif}
  writeln('ok');
end.
`;
    const warnSpaced = toSpaced(warn, "pascal");
    const warnAgain = layoutSource(warnSpaced.code, "pascal");
    assert.deepEqual(codeWords(toSpaced(warnAgain.code, "pascal").code), codeWords(warnSpaced.code), warnSpaced.code + "\n---\n" + warnAgain.code);
    assert.match(warnAgain.code, /else begin\s*\{\$ifdef dbgDBGrid\}\s*writeln\s*\(\s*'warn'\s*\);\s*\{\$endif\}\s*end;/i, warnAgain.code);
    assert.doesNotMatch(warnAgain.code, /writeln\s*\(\s*'warn'\s*\);\s*end;\s*\{\$endif\}/i, warnAgain.code);
    fpc(warnAgain.code, "off.pas", true);
    fpc(`{$define dbgDBGrid}\n${warnAgain.code}`, "on.pas", true);

    const armsSpaced = toSpaced(arms, "pascal");
    const armsAgain = layoutSource(armsSpaced.code, "pascal");
    assert.deepEqual(codeWords(toSpaced(armsAgain.code, "pascal").code), codeWords(armsSpaced.code), armsSpaced.code + "\n---\n" + armsAgain.code);
    assert.match(armsAgain.code, /else begin[\s\S]*\{\$ifdef unix\}[\s\S]*\{\$else\}[\s\S]*\{\$endif\}\s*end;/i, armsAgain.code);
    fpc(armsAgain.code, "off.pas", true);
    fpc(`{$define unix}\n${armsAgain.code}`, "on.pas", true);

    const outerSpaced = toSpaced(outer, "pascal");
    const outerAgain = layoutSource(outerSpaced.code, "pascal");
    assert.deepEqual(codeWords(toSpaced(outerAgain.code, "pascal").code), codeWords(outerSpaced.code), outerSpaced.code + "\n---\n" + outerAgain.code);
    assert.match(outerAgain.code, /\{\$endif\}\s*end\s*\{\$endif\};/i, outerAgain.code);
    assert.doesNotMatch(outerAgain.code, /writeln\s*\(\s*'in'\s*\);\s*\{\$endif\}\s*\{\$endif\}/i, outerAgain.code);
    fpc(outerAgain.code, "off.pas", true);
    fpc(`{$define OUTER}{$define INNER}\n${outerAgain.code}`, "on.pas", true);
  });

  it("keeps a case arm's endif inside that arm", () => {
    const host = `{$mode objfpc}
program P;
function Cell(Id: integer): string;
begin
  case Id of
    1:
      Cell := 'name';
    2:
      {$if defined(USE_UNIX)}
      Cell := 'unix';
      {$else}
      Cell := 'other';
      {$endif}
    3:
      Cell := 'date';
  end;
end;
function Last(Id: integer): string;
begin
  case Id of
    1:
      Last := 'name';
    2:
      {$if defined(USE_UNIX)}
      Last := 'unix';
      {$else}
      Last := 'other';
      {$endif}
  end;
end;
begin
{$if defined(USE_UNIX)}
  if Cell(2) <> 'unix' then Halt(1);
  if Last(2) <> 'unix' then Halt(1);
{$else}
  if Cell(2) <> 'other' then Halt(1);
  if Last(2) <> 'other' then Halt(1);
{$endif}
  if Cell(1) <> 'name' then Halt(1);
  if Cell(3) <> 'date' then Halt(1);
  if Last(1) <> 'name' then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /2:\s*begin\s*\{\$if defined\(USE_UNIX\)\}[\s\S]*?\{\$endif\}\s*end;\s*3:/i, again.code);
    assert.doesNotMatch(again.code, /end;\s*\{\$endif\}\s*3:/i, again.code);
    assert.match(again.code, /Last := 'other';\s*\{\$endif\}\s*end;/i, again.code);
    fpc(again.code, "off.pas", true);
    fpc(`{$define USE_UNIX}\n${again.code}`, "on.pas", true);
  });

  it("keeps an include's interface declarations out of implementation", () => {
    const inc = `{%MainUnit process.pp}
type
  TFoo = class
    procedure Bar;
  end;
procedure Run;

implementation

procedure TFoo.Bar;
begin
  Run;
end;

procedure Run;
begin
end;
`;
    const spaced = toSpaced(inc, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /procedure Run;\s*implementation/i, again.code);
    assert.doesNotMatch(again.code, /begin\s*end;\s*begin\s*end;\s*implementation/i, again.code);
    const unit = `{$mode objfpc}{$H+}
unit u;
interface
${again.code}
end.
`;
    fpc(unit, "u.pas", false);
  });

  it("keeps a version check inside the expression it guards", () => {
    const host = `{$mode objfpc}
program P;
function OpenIt: integer;
begin
  Result := Pos('a', 'ab' {$IF FPC_FULLVERSION >= 30301} + Missing {$ENDIF});
end;
begin
  if OpenIt <> 1 then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /'ab'\s*\{\$IF FPC_FULLVERSION >= 30301\}\s*\+\s*Missing\s*\{\$ENDIF\}/i, again.code);
    fpc(again.code, "ver.pas", true);
  });

  it("keeps both semicolons of a conditional const inside the declaration", () => {
    const host = `{$mode objfpc}
program P;
function Pick: longint;
const
  cBreak = {$IFDEF UNICODE} $000D000A; {$ELSE} $0D0A; {$ENDIF}
begin
  Result := cBreak;
end;
type
  PtrPair = {$IFDEF UNICODE} ^LongInt; {$ELSE} ^Word; {$ENDIF}
begin
{$IFDEF UNICODE}
  if Pick <> $000D000A then Halt(1);
{$ELSE}
  if Pick <> $0D0A then Halt(1);
{$ENDIF}
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /\{\$ENDIF\}\s*begin/i, again.code);
    assert.doesNotMatch(again.code, /\{\$ELSE\}\s*begin/i, again.code);
    fpc(again.code, "off.pas", true);
    fpc(`{$define UNICODE}\n${again.code}`, "on.pas", true);
  });

  it("keeps the if semicolon after an endif that wraps only the else", () => {
    const host = `{$mode objfpc}
program P;
procedure DebugLn(const S: string);
begin
  if S = '' then Halt(1);
end;
function NextToken: Boolean;
var
  FDeepest: Integer;
begin
  Result := False;
  if not Result then
    FDeepest := 1
  {$IFDEF TMATE_MATCHING} else DebugLn('advance') {$ENDIF}
  ;
  Result := True;
  NextToken := Result;
end;
begin
  if not NextToken then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /end\s*\{\$ENDIF\};/i, again.code);
    assert.doesNotMatch(again.code, /end;\s*\{\$ENDIF\}/i, again.code);
    fpc(again.code, "off.pas", true);
    fpc(`{$define TMATE_MATCHING}\n${again.code}`, "on.pas", true);
  });

  it("does not leave a semicolon in a case when the else arm is compiled out", () => {
    const host = `{$mode objfpc}
program P;
function Cell(Kind: integer): integer;
begin
  case Kind of
    1:
      Cell := 1;
    {$IFDEF WITH_REGEX_ASSERT}
    else
      Cell := 9;
    {$ENDIF}
  end;
end;
begin
  if Cell(1) <> 1 then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.doesNotMatch(again.code, /\{\$ENDIF\};/i, again.code);
    fpc(again.code, "off.pas", true);
    fpc(`{$define WITH_REGEX_ASSERT}\n${again.code}`, "on.pas", true);
  });

  it("keeps a long array constructor off a single line", () => {
    const values = Array.from({ length: 39 }, (_, i) => `${i + 1}, // n${i}`);
    const host = `{$mode objfpc}
program P;
const
  A: array[0..39] of Byte = (
    ${values.join("\n    ")}
    40
  );
begin
  if A[0] <> 1 then Halt(1);
  if A[39] <> 40 then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.ok(!again.code.split("\n").some((line) => line.length > 200), again.code.slice(0, 400));
    fpc(again.code, "arr.pas", true);
  });

  it("closes an abstract object nested in a class type section", () => {
    const host = `{$mode objfpc}{$H+}
program P;
uses fgl;
type
  TFile = class
  end;
  TList = class(specialize TFPGList<TFile>)
  public type
    TEnum = object abstract
    protected
      FList: TList;
      function GetCurrent: TFile; virtual; abstract;
    public
      function MoveNext: Boolean; virtual; abstract;
      property Current: TFile read GetCurrent;
    end;
    TEnum2 = class abstract
    public
      function Done: Boolean; virtual; abstract;
    end;
  public
    function Count: integer; virtual; abstract;
  end;
begin
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /object abstract\s+protected/i, again.code);
    assert.match(again.code, /class abstract\s+public/i, again.code);
    assert.doesNotMatch(again.code, /abstract\s*:/i, again.code);
    fpc(again.code, "abs.pas", true);
  });

  it("keeps a uses semicolon after an endif that only adds a unit", () => {
    const host = `{$mode objfpc}
program P;
uses
  SysUtils
  {$IFDEF EXTRA}, Math {$ENDIF};
begin
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /\{\$ENDIF\};/i, again.code);
    fpc(again.code, "off.pas", true);
    fpc(`{$define EXTRA}\n${again.code}`, "on.pas", true);
  });

  it("keeps an endif that wraps the last then-statement inside the if", () => {
    const host = `{$mode objfpc}
program P;
function Visual(AllEqual: boolean): string;
begin
  if AllEqual then
  begin
    Result := 'v';
    {$IFDEF LCLCarbon}
    Result := Result + 'c'
    {$ENDIF}
  end
  else
    Result := '';
  Visual := Result;
end;
begin
  if Visual(false) <> '' then Halt(1);
  if (Visual(true) <> 'v') and (Visual(true) <> 'vc') then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /\{\$ENDIF\}\s*end\s+else\b/i, again.code);
    assert.doesNotMatch(again.code, /end\s*\{\$ENDIF\}/i, again.code);
    fpc(again.code, "off.pas", true);
    fpc(`{$define LCLCarbon}\n${again.code}`, "on.pas", true);
  });

  it("keeps an except-end before the outer finally", () => {
    const host = `{$mode objfpc}
program P;
procedure Restore;
begin
  try
    with TObject.Create do
      try
        try
          if False then Halt(1);
        except
          { ignore any exceptions }
          // still fine
        end;
      finally
        Free;
      end;
  except
    Halt(1);
  end;
  writeln('ok');
end;
begin
  Restore;
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /except[\s\S]*\{\s*ignore any exceptions\s*\}[\s\S]*end;\s*finally/i, again.code);
    assert.doesNotMatch(again.code, /except[\s\S]*finally[\s\S]*end;\s*Free/i, again.code);
    fpc(again.code, "try.pas", true);
  });

  it("keeps a for-loop end outside an endian else that has no semicolon", () => {
    const host = `{$mode objfpc}
program P;
procedure ReadData;
var
  I, J: integer;
begin
  for I := 0 to 0 do
  begin
    for J := 0 to 0 do
    begin
      {$ifndef FPC_BIG_ENDIAN}
        if J = 0 then Write('');
      {$else}
        if J = 0 then Write('')
      {$endif FPC_BIG_ENDIAN}
    end;
  end;
  for I := 0 to 0 do
    if I = 0 then writeln('ok');
end;
begin
  ReadData;
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /\{\$endif FPC_BIG_ENDIAN\};\s*end;\s*end;/i, again.code);
    assert.doesNotMatch(again.code, /end;\s*end\s*\{\$endif FPC_BIG_ENDIAN\}/i, again.code);
    fpc(again.code, "le.pas", true);
    fpc(`{$define FPC_BIG_ENDIAN}\n${again.code}`, "be.pas", true);
  });

  it("keeps a case else off an if that already ended", () => {
    const host = `{$mode objfpc}
program P;
function Pick(Kind: integer): integer;
begin
  case Kind of
    1:
      if Kind = 1 then
      begin
        Pick := 1;
      end;
    else
      if Kind < 0 then
        Pick := 2
      else
        Pick := 3;
      if Kind = 0 then
        Pick := 4;
  end;
end;
begin
  if Pick(1) <> 1 then Halt(1);
  if Pick(0) <> 4 then Halt(1);
  if Pick(-1) <> 2 then Halt(1);
  if Pick(5) <> 3 then Halt(1);
  writeln('ok');
end.
`;
    const spaced = toSpaced(host, "pascal");
    const again = layoutSource(spaced.code, "pascal");
    const back = toSpaced(again.code, "pascal");
    assert.deepEqual(codeWords(back.code), codeWords(spaced.code), spaced.code + "\n---\n" + again.code);
    assert.match(again.code, /Pick := 1;\s*end;\s*end\s+else\b/i, again.code);
    assert.match(again.code, /else begin[\s\S]*Pick := 4/i, again.code);
    fpc(again.code, "caseelse.pas", true);
  });
});

