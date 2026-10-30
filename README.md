# offside

Version 0.1.0-alpha.1. This is alpha.

offside is a spaced dialect of C and Pascal. A colon and an indent stand where the host language wants braces or `begin`/`end`. The same tool reads host source into that dialect and writes the dialect back out. The host compiler is the test that it worked.

Semicolons stay. `offside` is the command.

## Why alpha, not beta

Beta would mean the shape of the dialect is frozen and only bugs are left. It is not. Each new body of real source still turns up a placement the converter had not seen. What is below is what this alpha has already compiled, not a promise about the next unit.

## What this alpha has compiled

Pascal, with Free Pascal 3.2.2:

- Lazarus LCL, `alllclunits`, round-tripped and compiled. 232198 lines, 0 errors. GTK, Qt, Cocoa, Win32, and the other widgetset bindings are not in that round trip. JPEG, TIFF, and GIF stay disabled.
- IDEIntf, already in host form, compiled against that LCL. 279174 lines, 0 errors. That is the interface package, not the IDE.

C, with gcc `-std=gnu2x`:

- The SQLite amalgamation 3.54.1 round-trips with 0 errors and 0 warnings. The C target is ISO C89 through C23, plus the GNU dialect that gcc and clang share. MSVC, embedded compilers, Objective-C, and C++ are not targets.

Java and JavaScript use the same layout engine. They are not the proof.

## Page

[tpqwz.github.io/offside](https://tpqwz.github.io/offside/) is this repository, bundled and published by GitHub Pages on each push to `main`. The page runs the converter in the browser. It does not compile.

## Releases

A published release compiles standalone `offside` binaries from [`c/`](c/) and attaches them to that release. That C is written to the SEI CERT C Coding Standard (the rules Robert C. Seacord published for CERT, the same rules behind ISO/IEC TS 17961): bounded copies, no `system()`, no `strcpy`. It is not a Bun runtime with the script attached.

| Pack | Machine |
| --- | --- |
| `offside-linux-x64.zip` | Linux x64 |
| `offside-linux-arm64.zip` | Linux arm64 |
| `offside-macos-x64.zip` | macOS Intel |
| `offside-macos-arm64.zip` | macOS Apple silicon |
| `offside-windows-x64.zip` | Windows x64 |

The binary is the command. It still calls Free Pascal, gcc, clang, or tcc. Those are not in the zip. A push to `main` also builds the five binaries as Actions artifacts.

The TypeScript converter stays. The page is built from it, and `./bin/offside` still runs on Node 22. The native command is `make -C c`.

## Use

Node 22 or newer runs the TypeScript command. The native command needs a C11 compiler.

```bash
git clone https://github.com/tpqwz/offside.git
cd offside
make -C c
./c/build/offside --version
./bin/offside --version
```

Pascal needs Free Pascal (`ppcx64`). `FPC` or `OFFSIDE_FPC` picks the compiler. C uses `tcc` (`-std=c99`) or gcc/clang (`-std=gnu2x`). `OFFSIDE_CC`, `TCC`, or `CC` picks the C compiler.

```bash
offside space hello.c          # host to spaced
offside emit hello.csp         # spaced to host
offside run hello.csp -- -O2   # spaced or host, then the compiler
offside run demo.spas -c       # compile and stop
offside run demo.spas -o demo  # name the binary, then run it
```

`space` and `emit` refuse compiler flags. Put those on `run`, after `--`.

| Extension | Source |
| --- | --- |
| `.csp` `.sjava` `.sjs` `.spas` | spaced |
| `.c` `.h` | host C |
| `.pas` `.pp` `.lpr` `.dpr` `.inc` | host Pascal |
| `.java` `.js` | host Java, JavaScript |

A spaced C fragment:

```c
struct Point:
    int x
    int y

int main(void):
    struct Point p = {1, 2}
    if (p.x > 0):
        return p.y
    else:
        return 0
```

Host C that cannot be written that way stays braced. A trailing comma in a C initializer is kept. Pascal array constructors do not get one.

## Tests

```bash
node --experimental-strip-types --test src/lib/layout/*.test.ts src/cli/offside.test.ts
```

The Pascal tests compile with FPC. The C tests compile with the C compiler on `PATH`.

`scripts/pascal-corpus.ts` walks a Lazarus tree and checks that a second trip through the dialect does not move tokens (other than `begin`/`end`) or compiler directives.

## License

MIT. See [LICENSE](LICENSE).
