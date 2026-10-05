# Jot

A compiled programming language written in C. `jotc` lexes, parses, and generates NASM x86-64 assembly, then assembles and links it into a runnable program.

## Current Status

Working end-to-end compiler: lexer (tokens) → parser (AST) → codegen (`.asm` in `build/bin/generated/`) → `nasm` + `gcc` → executable. Verified on Windows (`win64`); Unix targets `elf64`.

## Language Features

- **Keywords**: `fn`, `return`, `if`, `else`, `while`, `for`, `print`, `num`, `bool`, `string`, `array`, `struct`, `class`, `new`
- **Literals**: 64-bit integer literals, 64-bit floating point literals, string literals
- **Operators**: Arithmetic (`+`, `-`, `*`, `/`, `%` ints only; `+` also concatenates strings), comparison (`==`, `!=`, `<`, `>`, `<=`, `>=`, lexicographic for strings via `strcmp`), assignment (`=`, `+=`, `-=`; `+=` concatenates strings). Precedence is C-like: `*`/`/`/`%` over `+`/`-` over comparisons. Mixed int/float promotes to float (`+`, `-`, `*`, `/` use `addsd`/`subsd`/`mulsd`/`divsd`, comparisons use `ucomisd`)
- **Separators**: Semicolons, parentheses, braces, brackets, commas, dot (for `self.arg`, `obj.field`, `obj.method()`)
- **Control Flow**: if/else statements (plus else-if), while loops, `for (x in arr)` array iteration
- **Data Types**: num (holds ints or doubles; int vars promote to float on float assign, int operands promote via `cvtsi2sd`), bool, string, plus number arrays (float arrays convert ints). Values are category-checked (numbers vs strings); mismatches are errors. `%` on floats is an error
- **Strings**: `s[i]` returns the num character code (`s[0]` is first), `len(s)` returns the num length, `s1 + s2` concatenates (new heap string), comparisons are lexicographic. Mixing strings with numbers is an error, as is `-`, `*`, `/`, `%` on strings
- **Structs**: `struct private object { num size; string label; }` declares fields (`num`, `bool`, `string`; no nesting, no methods). `object car = new object(200, "fast");` heap-allocates an instance with positional field values. Read `car.size`, write `car.size = 300;` (C-like, plus `+=`/`-=`; `num` fields store doubles, so `200` prints as `200`). Whole-instance copy works (`object b = car;`, `car = new object(1, "x");`)
- **Classes**: `class public dog { num age; string bark() { ... } }` holds fields (like a struct) plus methods written as `type name(params) { ... }` (no `fn`, always private to the class). `dog d = new dog(3);` creates an instance (args match fields), `d.bark("hi");` calls a method, and calls nest in expressions (`string s = d.bark();`). Inside a method the instance arrives as hidden first argument: `self.field` reads/writes fields (parameters win on name clashes), and `self.other()` calls sibling methods
- **Functions**: Definitions with typed or untyped params (any count: first four use registers, the rest spill to the stack; struct/class-typed params take instances by pointer). Nothing runs until called — top-level statements are the entry point, `fn main` is an ordinary function invoked with `main();`. Inside a body, parameters are read as `self.name` (bare use warns); locals stay bare. Shadowing a parameter warns. Duplicate params, calls to undefined functions, and arity mismatches are errors with locations. `input` and `len` are reserved (builtins, not definable)
- **Member access**: `self.arg` for parameters (also inside `{...}` print interpolation), `car.size` for struct/class fields, `d.bark()` for methods. Field writes use `car.size = v;` (also `+=`/`-=`); chained access like `a.b.c` is an error. Method definitions belong to their class and are invisible outside it: bare `bark()` and `import [bark]` are errors telling you to call `instance.bark(...)`
- **Visibility**: `fn` / `struct` / `class` take `public` / `private`. Missing visibility warns and defaults to private (`main` defaults to public). Only public functions, structs, and classes can be imported
- **Imports**: `from [file.jot] import [a, b];` merges the file's functions, structs, and classes (paths resolve from where `jotc` runs). `import [*]` takes all public definitions; names alongside `*` warn as redundant. Dependencies travel with imports (a merged function pulls what it calls, including private helpers). Re-imports resolve once (diamonds safe); cycles, duplicates, private or missing names are errors, and non-function top-level statements in imported files are ignored. Empty lists warn; `import` without `from` is an error. Importing a method by name explains it is private to its class
- **Print**: `print(x);` for values, `print("x={x}\n");` with `{name}` interpolation (also `{car.size}` for fields; instances themselves cannot print)
- **Input**: `input()` reads an integer from stdin, `input("Age: ")` prints the string prompt first (Python-style, no trailing newline, flushed before blocking). `input` is reserved and takes at most 1 string argument. Non-integer input aborts with `invalid input: expected integer`. `len(s)` returns the num length of a string (exactly 1 string argument)
- **Return**: `return(v);` returns from a function (`rax`), top-level `return(v);` exits the process with code `v`
- **Comments**: Single-line comments (`//`)
- **Diagnostics**: clang-style errors (red `Error:`, `file:line:col`, source snippet, `^` / red `~~~`) and yellow `Warning:`s (unreachable code, missing visibility, redundant imports, shadowing — params, fields by params, locals — unused locals). **The compiler never stops at the first problem**: the parser and code generator recover from each faulty statement and keep going, so every error and warning in the file prints before `jotc` exits with status 1. **Any error means nothing is compiled** — no assembly is written (a partial file is deleted), so warnings still build while errors do not. Diagnostics in imported files point at that file, not the entry file. Error coverage: string misuse (indexing non-strings, non-numeric index, `len` arity/type, mixed concat/compare, arithmetic on strings, `-=` on strings), structs/classes (duplicates, unknown types/fields/methods, `new` arity/type errors, methods in structs, member visibility modifiers, bare or imported method calls, bad method returns, iterating/printing instances, chained access, reserved `this`), and `input` misuse (arity, non-string prompt). A missing delimiter points where the token belongs when the offender starts a new line, otherwise at the offender like gcc
- **Runtime checks**: signed arithmetic with integer-overflow, division-by-zero, and stack-overflow traps, plus string index out of bounds, out of memory, and null instance access (message plus exit code 3)

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

Targets: `build` compiles `src/**/*.c`, `link` links `build/bin/jotc`, `debug` dumps tokens + AST + asm path, `run` compiles `src/main.jot` to asm, assembles, links, and executes it, `clean` deletes `build/`. Any step failing deletes `build/`. The Makefile supports both Windows and Unix-like systems. More runnable programs live under `examples/` (one folder per topic, file names say what they demo).

## Usage

```bash
./build/bin/jotc <file.jot> [-o output] [--debug]
```

The `-o` flag specifies the output file (e.g., `main.exe`, `main.o`, or `main`). The compiler validates that the output directory exists and warns if the file already exists. Without `-o`, the assembly goes to `build/bin/generated/<name>.asm`. Without `--debug` only errors/warnings print. Exit status is `1` when anything failed to compile, `0` otherwise (warnings still compile).

## Example

`src/main.jot` (what `make run` compiles):

```jot
fn public main() {
  num x = 2;
  num y = x + 4;
  print("Y: {y} \n");
  string greeting = "hello" + " jot";
  print(greeting);
  print("\n");
  return(0);
}

main();
```

```bash
make run
```

prints `Y: 6 ` followed by `hello jot` (the process exits `0`).

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

Strings, structs, classes, and prompted input (`examples/strings/`, `examples/structs/`, `examples/classes/`, `examples/input/`):

```jot
string a = "hello";
print(a + " world");
print(len(a));
print(a[1]);

struct private object {
  num size;
}

object car = new object(200);
car.size += 50;
print(car.size);

class public dog {
  string bark() {
    return("BARK!");
  }
}

dog d = new dog();
d.bark();

num age = input("Age: ");
```

## Implementation

- Written in C (`-Wall -Wextra` clean)
- `src/lexer/` — tokenizes source (`line`/`col` on every token)
- `src/parser/` — recursive descent to AST, statements chained via `right`
- `src/codegen/` — AST to NASM (stack-machine expressions, `rbp`-relative locals, `labelN`/`loopN` jumps, `printf`/`exit` runtime)
- `src/terminal/` — terminal detection + ANSI colors for diagnostics
- `src/jotc.c` — driver: `Lexer` → `Parser` → `GenerateAssembly`
