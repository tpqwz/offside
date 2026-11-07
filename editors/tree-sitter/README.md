# Highlighting grammars

These are Tree-sitter grammars for the spaced dialects. They color keywords, strings, comments, and colon-blocks in an editor. They are not the compiler, and they do not check that a program is legal. The host compiler, reached through `offside`, remains the test.

Alpha, same as the dialect. A colon opens a highlighted block when the next line that contains code is indented strictly deeper than the line the colon is on. `::`, `:=`, and `?:` stay operators. A colon with more code after it on the same line stays punctuation.

Known limits, on purpose:

- A label whose next line is deeper is shown as a block. C `error:` and Zig `blk:` are the usual cases. The compiler still treats a two-token label as a label.
- `?:` at the end of a line, with a deeper next line, can look like a block.
- Lambdas, `by lazy { }`, and other shapes the dialect leaves braced stay braces. `{` and `}` are punctuation.
- JavaScript regular-expression literals are not recognized.
- Kotlin string interpolation (`$name`, `${name}`) stays inside the string. Nested block comments are one level deep.
- Pascal keywords are case-insensitive. A longer word such as `WriteLn` is an identifier, not the keyword `write`.
- The grammars do not know types.
- A file is assumed to start at column 0. Tabs are stops of 8, matching the converter.
- A comment-only line does not close a block.

| Dialect | Grammar | Extensions | Scope |
|---|---|---|---|
| C | `offsidec` | `.csp` | `source.offside.c` |
| Java | `offsidejava` | `.sjava` | `source.offside.java` |
| JavaScript | `offsidejavascript` | `.sjs` | `source.offside.javascript` |
| Pascal | `offsidepascal` | `.spas` | `source.offside.pascal` |
| Zig | `offsidezig` | `.szig` | `source.offside.zig` |
| Kotlin | `offsidekotlin` | `.skt` | `source.offside.kotlin` |

Each grammar directory has `src/parser.c` (ABI 15), `src/scanner.c`, and `queries/highlights.scm`. The scanner logic is shared in `common/scanner.c`. Keep that directory beside the six grammars; the scanners include it.

Samples live in `samples/`. From a grammar directory, with the Tree-sitter CLI 0.25.10:

```bash
tree-sitter parse ../samples/hello.csp
tree-sitter query queries/highlights.scm ../samples/hello.csp
```

To regenerate after editing `build-grammars.mjs` or `common/scanner.c`:

```bash
node build-grammars.mjs
for d in offsidec offsidejava offsidejavascript offsidepascal offsidezig offsidekotlin; do
  (cd "$d" && tree-sitter generate --abi 15)
done
```

## Neovim

Compile one shared library per dialect. `src/scanner.c` includes `common/scanner.c`, so build from the grammar directory:

```bash
cd editors/tree-sitter/offsidec
cc -shared -fPIC -I src -o "$HOME/.local/share/nvim/offsidec.so" src/parser.c src/scanner.c
```

Repeat for the other five names. Queries must be on the runtime path as `queries/<grammar>/highlights.scm` (and `folds.scm` if you want folds). Copy each `queries/` directory to `~/.config/nvim/queries/offsidec/` and so on.

```lua
vim.filetype.add({
  extension = {
    csp = "offsidec",
    sjava = "offsidejava",
    sjs = "offsidejavascript",
    spas = "offsidepascal",
    szig = "offsidezig",
    skt = "offsidekotlin",
  },
})

local parsers = {
  offsidec = "offsidec.so",
  offsidejava = "offsidejava.so",
  offsidejavascript = "offsidejavascript.so",
  offsidepascal = "offsidepascal.so",
  offsidezig = "offsidezig.so",
  offsidekotlin = "offsidekotlin.so",
}
local dir = vim.fn.expand("~/.local/share/nvim/")
for lang, file in pairs(parsers) do
  vim.treesitter.language.register(lang, lang)
  vim.api.nvim_create_autocmd("FileType", {
    pattern = lang,
    callback = function()
      vim.treesitter.language.add(lang, { path = dir .. file })
      vim.treesitter.start()
    end,
  })
end
```

If you use nvim-treesitter's older `get_parser_configs()`, point `install_info.url` at the grammar directory, set `files` to `{ "src/parser.c", "src/scanner.c" }`, and keep `common/` where the include expects it. Generate is not required; `src/parser.c` is already produced.

## Helix

In `languages.toml`:

```toml
[[language]]
name = "offsidec"
scope = "source.offside.c"
file-types = ["csp"]
roots = []
comment-tokens = "//"
indent = { tab-width = 4, unit = "    " }
grammar = "offsidec"

[[grammar]]
name = "offsidec"
source = { path = "/absolute/path/to/offside/editors/tree-sitter/offsidec" }
```

Repeat for `offsidejava` (`.sjava`), `offsidejavascript` (`.sjs`), `offsidepascal` (`.spas`), `offsidezig` (`.szig`), and `offsidekotlin` (`.skt`). Pascal line comments are `//`; `{ }` and `(* *)` are also comments. Then:

```bash
hx --grammar build
```

Helix reads `queries/highlights.scm` from the grammar directory.

## Zed

A grammar crate or extension points at one directory under `editors/tree-sitter/`. Local development looks like this in the extension's `extension.toml` (one block per dialect):

```toml
[grammars.offsidec]
path = "offsidec"
```

with the grammar sources copied or submodule-linked so `common/scanner.c` still resolves from `src/scanner.c`. Highlight queries are the `queries/highlights.scm` file Zed already expects.

## Emacs

Emacs 29 or later, with `treesit`. Install from the grammar directory (`src/parser.c` and `src/scanner.c` are the sources):

```elisp
(add-to-list 'treesit-language-source-alist
             '(offsidec :url "file:///absolute/path/to/offside"
                        :source-dir "editors/tree-sitter/offsidec"))
(treesit-install-language-grammar 'offsidec)
```

Repeat for the other five language symbols. `treesit-install-language-grammar` compiles `src/parser.c` and `src/scanner.c`. Load `queries/highlights.scm` with `treesit-font-lock-rules`, mapping `(keyword)` to `font-lock-keyword-face`, `(string)` and `(template)` and `(line_string)` to `font-lock-string-face`, `(comment)` to `font-lock-comment-face`, `(number)` to `font-lock-number-face`, and `(block_colon)`, `(operator)`, and `(punctuation)` to `font-lock-delimiter-face` or `font-lock-operator-face`. File associations: `.csp` to `offsidec`, `.sjava` to `offsidejava`, `.sjs` to `offsidejavascript`, `.spas` to `offsidepascal`, `.szig` to `offsidezig`, `.skt` to `offsidekotlin`.

## VS Code

VS Code loads a WebAssembly grammar, not the C parser. This tree was not built to wasm here (that needs `emcc`, Docker, or Podman). From a grammar directory, with the Tree-sitter CLI and Emscripten:

```bash
tree-sitter build --wasm
```

An extension then registers the `.wasm` file, the file extensions above, and `queries/highlights.scm`. Until that extension exists, Neovim, Helix, Emacs, and Zed are the editors these grammars actually load in.
