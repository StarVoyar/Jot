# Jot

A compiled programming language written in C. `jotc` lexes, parses, and generates NASM x86-64 assembly, then assembles and links it into a runnable program.

## Current Status

Working end-to-end compiler: lexer (tokens) → parser (AST) → codegen (`.asm` in `build/bin/generated/`) → `nasm` + `gcc` → executable. Verified on Windows (`win64`); Unix targets `elf64`.

## Language Features

- **Keywords**: `fn`, `return`, `if`, `else`, `while`, `for`, `print`, `num`, `bool`, `string`, `array`
- **Literals**: 64-bit integer literals, 64-bit floating point literals, string literals
- **Operators**: Arithmetic (`+`, `-`, `*`, `/`, `%` ints only), comparison (`==`, `!=`, `<`, `>`, `<=`, `>=`), assignment (`=`, `+=`, `-=`). Precedence is C-like: `*`/`/`/`%` over `+`/`-` over comparisons. Mixed int/float promotes to float (`+`, `-`, `*`, `/` use `addsd`/`subsd`/`mulsd`/`divsd`, comparisons use `ucomisd`)
- **Separators**: Semicolons, parentheses, braces, brackets, commas, dot (for `self.arg`)
- **Control Flow**: if/else statements (plus else-if), while loops, `for (x in arr)` array iteration
- **Data Types**: num (holds ints or doubles; int vars promote to float on float assign, int operands promote via `cvtsi2sd`), bool, string, plus number arrays (float arrays convert ints). Values are category-checked (numbers vs strings); mismatches are errors. `%` on floats is an error
- **Functions**: Definitions with typed or untyped params (any count: first four use registers, the rest spill to the stack). Nothing runs until called — top-level statements are the entry point, `fn main` is an ordinary function invoked with `main();`. Inside a body, parameters are read as `self.name` (bare use warns); locals stay bare. Shadowing a parameter warns. Duplicate params, calls to undefined functions, and arity mismatches are errors with locations
- **Member access**: `self.arg` for parameters (also inside `{...}` print interpolation). Other objects and member assignment are not supported
- **Visibility**: `fn public name` / `fn private name`. Missing visibility warns and defaults to private (`main` defaults to public). Only public functions can be imported
- **Imports**: `from [file.jot] import [a, b];` merges the file's functions (paths resolve from where `jotc` runs). `import [*]` takes all public functions; names alongside `*` warn as redundant. Dependencies travel with imports (a merged function pulls what it calls, including private helpers). Re-imports resolve once (diamonds safe); cycles, duplicates, private or missing names are errors, and non-function top-level statements in imported files are ignored. Empty lists warn; `import` without `from` is an error
- **Print**: `print(x);` for values, `print("x={x}\n");` with `{name}` interpolation
- **Input**: `input()` reads an integer from stdin (`input` is reserved, takes no arguments). Non-integer input aborts with `invalid input: expected integer`
- **Return**: `return(v);` returns from a function (`rax`), top-level `return(v);` exits the process with code `v`
- **Comments**: Single-line comments (`//`)
- **Diagnostics**: clang-style errors (red `Error:`, `file:line:col`, source snippet, `^` / red `~~~`) and yellow `Warning:`s (unreachable code, missing visibility, redundant imports, shadowing, unused locals — all still compile). A missing delimiter points where the token belongs when the offender starts a new line, otherwise at the offender like gcc
- **Runtime checks**: signed arithmetic with integer-overflow, division-by-zero, and stack-overflow traps (message plus exit code 3)

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
./build/bin/jotc <file.jot> [-o output] [--debug]
```

The `-o` flag specifies the output file (e.g., `main.exe`, `main.o`, or `main`). The compiler validates that the output directory exists and warns if the file already exists. Without `-o`, the assembly goes to `build/bin/generated/<name>.asm`. Without `--debug` only errors/warnings print.

## Example

`test/test.jot`:

```jot
fn main() {
  num x = 2;
  num y = x + 4;
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
fn add(num var1, num var2) {
  num sum = self.var1 + self.var2;
  print("Sum: {sum}\n");
  return(sum);
}

add(10, 20);
```

prints `Sum: 30`.

Splitting across files (`math.jot`):

```jot
fn public add(num var1, num var2) {
  num sum = self.var1 + self.var2;
  return(sum);
}
```

```jot
from [math.jot] import [add];

fn public main() {
  int r = add(20, 22);
  print("{r}\n");
  return(0);
}

main();
```

prints `42`.

## Implementation

- Written in C (`-Wall -Wextra` clean)
- `src/lexer/` — tokenizes source (`line`/`col` on every token)
- `src/parser/` — recursive descent to AST, statements chained via `right`
- `src/codegen/` — AST to NASM (stack-machine expressions, `rbp`-relative locals, `labelN`/`loopN` jumps, `printf`/`exit` runtime)
- `src/terminal/` — terminal detection + ANSI colors for diagnostics
- `src/jotc.c` — driver: `Lexer` → `Parser` → `GenerateAssembly`
