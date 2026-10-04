# Jot

A compiled programming language written in C. `jotc` lexes, parses, and generates NASM x86-64 assembly, then assembles and links it into a runnable program.

## Current Status

Working end-to-end compiler: lexer (tokens) → parser (AST) → codegen (`.asm` in `build/bin/generated/`) → `nasm` + `gcc` → executable. Verified on Windows (`win64`); Unix targets `elf64`.

## Language Features

- **Keywords**: `fn`, `return`, `if`, `else`, `while`, `for`, `print`, `int`, `bool`, `string`, `char`, `array`
- **Literals**: Integer literals, string literals
- **Operators**: Arithmetic (`+`, `-`, `*`, `/`, `%`), comparison (`==`, `!=`, `<`, `>`, `<=`, `>=`), assignment (`=`)
- **Separators**: Semicolons, parentheses, braces, brackets, commas
- **Control Flow**: if/else statements (plus else-if), while loops
- **Data Types**: int, bool, string, char (arrays parse but codegen rejects them for now, same for `for` loops)
- **Functions**: Definitions with typed or untyped params (any count: first four use registers, the rest spill to the stack). Nothing runs until called — top-level statements are the entry point, `fn main` is an ordinary function invoked with `main();`. Inside a body, parameters are read as `self.name` (bare parameter names are rejected); locals stay bare
- **Member access**: `self.arg` for parameters (also inside `{...}` print interpolation). Other objects and member assignment are not supported
- **Print**: `print(x);` for values, `print("x={x}\n");` with `{name}` interpolation
- **Return**: `return(v);` returns from a function (`rax`), top-level `return(v);` exits the process with code `v`
- **Comments**: Single-line comments (`//`)
- **Diagnostics**: clang-style errors (red `Error:`, `file:line:col`, source snippet, `^` / red `~~~`) and yellow `Warning:`s (e.g. unreachable code after `return`, which still compiles). A missing delimiter points where the token belongs when the offender starts a new line, otherwise at the offender like gcc

## Building

Requires `gcc`, `nasm`, and `make`.

```bash
make all
```

Or step by step:

```bash
make build
make link
make debug
make run
```

Targets: `build` compiles `src/**/*.c`, `link` links `build/bin/jotc`, `debug` dumps tokens + AST + asm path, `run` compiles `test/test.jot` to asm, assembles, links, and executes it, `clean` deletes `build/`. Any step failing deletes `build/`. The Makefile supports both Windows and Unix-like systems.

## Usage

```bash
./build/bin/jotc <file.jot> [output.asm] [--debug]
```

Without `output.asm` the assembly goes to `build/bin/generated/<name>.asm`. Without `--debug` only errors/warnings print.

## Example

`test/test.jot`:

```jot
fn main() {
  int x = 2;
  int y = x + 4;
  print("Y: {y} \n");
  return(1);
}

main();
```

```bash
make run
```

prints `Y: 6 ` (the `1` is `main`'s return value to its caller; the process exits `0`).

Parameters use `self`:

```jot
fn add(int var1, int var2) {
  int sum = self.var1 + self.var2;
  print("Sum: {sum}\n");
  return(sum);
}

add(10, 20);
```

prints `Sum: 30`.

## Implementation

- Written in C (`-Wall -Wextra` clean)
- `src/lexer/` — tokenizes source (`line`/`col` on every token)
- `src/parser/` — recursive descent to AST, statements chained via `right`
- `src/codegen/` — AST to NASM (stack-machine expressions, `rbp`-relative locals, `labelN`/`loopN` jumps, `printf`/`exit` runtime)
- `src/terminal/` — terminal detection + ANSI colors for diagnostics
- `src/jotc.c` — driver: `Lexer` → `Parser` → `GenerateAssembly`
