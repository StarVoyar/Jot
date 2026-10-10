# Jot

A compiled programming language written in C. `jotc` lexes, parses, semantically analyzes, lowers to a typed three-address IR, optimizes, and generates NASM x86-64 assembly, then assembles and links it into a runnable program.

## Current Status

Working end-to-end compiler: lexer (tokens) → parser (AST) → semantic analysis → Jot IR → optimization (`-O0`/`-O1`/`-O2`) → target backend with linear-scan register allocation (`.asm` in `build/bin/generated/`) → `nasm` + `gcc` → executable. Targets: Windows x86-64 (`win64`) and Linux x86-64 (`elf64`), each with its own calling convention; verified by executing all examples on Windows and on Linux (via WSL).

## Language Features

- **Keywords**: `char`, `fn`, `return`, `if`, `else`, `while`, `for`, `break`, `continue`, `print`, `num`, `bool`, `str`, `arr`, `struct`, `class`, `inherit`, `new`, `null`, `global`
- **Literals**: 64-bit integer literals, 64-bit floating point literals, string literals (`"text"`) with escapes `\"`, `\\`, `\n`, `\t`, `\r`, character literals (`'A'`) with escapes `\'`, `\\`, `\n`, `\t`, `\r`, `\0` (a char is its numeric code: `'A'` is 65; empty, multi-character, and unterminated literals are errors)
- **Operators**: Arithmetic (`+`, `-`, `*`, `/`, `%` ints only; `+` also concatenates strings), comparison (`==`, `!=`, `<`, `>`, `<=`, `>=`, lexicographic for strings via `strcmp`), assignment (`=`, `+=`, `-=`; `+=` concatenates strings). Precedence is C-like: `*`/`/`/`%` over `+`/`-` over comparisons. Mixed int/float promotes to float (`+`, `-`, `*`, `/` use `addsd`/`subsd`/`mulsd`/`divsd`, comparisons use `ucomisd`)
- **Separators**: Semicolons, parentheses, braces, brackets, commas, dot (for `self.arg`, `obj.field`, `obj.method()`)
- **Control Flow**: if/else statements (plus else-if), while loops, `for (x in arr)` array iteration, `break` / `continue` (both need an enclosing loop; `continue` in a `while` re-tests the condition)
- **Data Types**: num (holds ints or doubles; a num becomes a float the first time it is assigned a float — locals, globals, params, and returns all follow, converting existing int values), bool, char (a num with character meaning: `char c = 'A';` or `char c = 65;`), str, plus arrays (below). Values are category-checked (numbers vs strings); mismatches are errors. `%` on floats is an error
- **Arrays**: `arr nums = [10, 20, 30];` heap-allocates a length-prefixed block (count at `[ptr]`, elements at `[ptr + 8 + i*8]`). Elements share one category: all numbers (floats win, ints promote), all strings, or instances of one struct/class. Read `a[i]` / write `a[i] = v` (also `+=` / `-=`, including `str` element concat) with runtime null and out-of-bounds traps. Iterate `for (x in arr)` over any element type. Arrays assign (`arr b = a;` aliases the block), pass to `fn f(arr xs)`, and return from functions. Struct/class elements bind by copy (`pair q = pts[0];` copies; the array keeps its own), while `pts[0].x = 1` writes the stored element in place and `pts[0].name()` calls methods on it. Mixed literals, kind mismatches, and bad element assigns are compile errors (`examples/arrays/`)
- **Strings**: `s[i]` returns the num character code (`s[0]` is first), `len(s)` returns the num length, `s1 + s2` concatenates (new heap string), comparisons are lexicographic. Mixing strings with numbers is an error, as is `-`, `*`, `/`, `%` on strings
- **Conversions**: `tostr(n)` renders a num as a heap string, `tonum(s)` parses a string into a num (`tostr(3.5)` is `3.5`, `tonum("2.75")` is `2.75`). Unparsable text is a runtime abort. Both are reserved builtins
- **File I/O**: `readFile("path")` returns the whole file as a str, `writeFile("path", text)` writes text and returns the num of bytes written. Both are reserved builtins
- **Null**: `null` is the null reference for str, struct, and class types. `p == null` / `p != null` compare the pointer (no other operator is allowed with null), and assigning `null` clears the variable
- **Structs**: `struct private object { num size; str label; }` declares fields (`num`, `bool`, `str`, or other struct/class instances — nesting works in any depth). `object car = new object(200, "fast");` heap-allocates an instance with positional field values. Read `car.size`, write `car.size = 300;` (C-like, plus `+=`/`-=`; `num` fields store doubles, so `200` prints as `200`). Whole-instance copy works (`object b = car;`, `car = new object(1, "x");`) and nested fields read as `l.start.x` (`examples/nested/`). Instance-typed values bind by copy on declaration, assignment, and `new` arguments, so the copy's own fields can change without touching the original
- **Classes**: `class public dog { num age; str bark() { ... } }` holds fields (like a struct) plus methods written as `type name(params) { ... }` (no `fn`, always private to the class). `dog d = new dog(3);` creates an instance (args match fields), `d.bark("hi");` calls a method, and calls nest in expressions (`str s = d.bark();`). Inside a method the instance arrives as hidden first argument: `self.field` reads/writes fields (parameters win on name clashes), and `self.other()` calls sibling methods. A class may `inherit` one base (`class dog inherit animal { ... }`): base fields come first in `new` arguments, inherited methods are usable as-is, and overriding a method changes behavior everywhere — a base-typed variable holding a subclass instance still calls the subclass method (dynamic dispatch through the instance's class id). Subclasses can be passed or assigned wherever the base is expected (`examples/inheritance/`)
- **Functions**: Definitions with typed or untyped params (any count: first four use registers, the rest spill to the stack; struct/class-typed params take instances by pointer) and an optional return type — `fn add(...) -> num { ... }` with `num`, `str`, `arr`, `char`, or `void`. `-> void` functions return without a value (`return;`); using their result is an error (`noop() + 1`), calling them as a statement is fine, and `return(v);` in a void function is an error. Outside a void function, bare `return;` is an error, and empty `return()` warns (use `return;` only for void). Nothing runs until called — top-level statements are the entry point, `fn main` is an ordinary function invoked with `main();`. Inside a body, parameters are read as `self.name` (bare use warns); locals stay bare. Shadowing a parameter warns. Duplicate params, calls to undefined functions, and arity mismatches are errors with locations. `input` and `len` are reserved (builtins, not definable)
- **Member access**: `self.arg` for parameters (also inside `{...}` print interpolation), `car.size` for struct/class fields, `d.bark()` for methods. Field writes use `car.size = v;` (also `+=`/`-=`). Chains work: `l.start.x`, `pts[1].y += 10`, `d.name.render()`, and `print("{b.edge.start.x}\n")` all resolve through nested instances. Method definitions belong to their class and are invisible outside it: bare `bark()` and `import [bark]` are errors telling you to call `instance.bark(...)`
- **Globals**: `global num counter = 0;` declares a top-level variable visible in every statement and function (no passing needed). Globals take a type and optional initializer, may be reassigned (`counter += 1;`), promote from int to float exactly like locals when given a float, and duplicate names are errors. Inside functions they are read and written like any variable
- **Visibility**: `fn` / `struct` / `class` take `public` / `private`. Missing visibility warns and defaults to private (`main` defaults to public). Only public functions, structs, and classes can be imported
- **Imports**: `from [file.jot] import [a, b];` merges the file's functions, structs, and classes (paths resolve from where `jotc` runs). `import [*]` takes all public definitions; names alongside `*` warn as redundant. Dependencies travel with imports (a merged function pulls what it calls, including private helpers). Re-imports resolve once (diamonds safe); cycles, duplicates, private or missing names are errors, and non-function top-level statements in imported files are ignored. Empty lists warn; `import` without `from` is an error. Importing a method by name explains it is private to its class
- **Print**: `print(x);` for values, `print("x={x}\n");` with `{name}` interpolation (also `{car.size}` for fields; instances themselves cannot print)
- **Input**: `input()` reads an integer from stdin, `input("Age: ")` prints the string prompt first (Python-style, no trailing newline, flushed before blocking). `input` is reserved and takes at most 1 string argument. Non-integer input aborts with `invalid input: expected integer`. `len(s)` returns the num length of a string (exactly 1 string argument)
- **Return**: `return(v);` returns a value from a function (typed by the declared return type; missing types fall back to what the returns actually contain), `return;` returns from a `-> void` function, and a top-level `return(v);` exits the process with code `v`
- **Comments**: Single-line comments (`//`)
- **Diagnostics**: clang-style errors (red `Error:`, `file:line:col`, source snippet, `^` / red `~~~`) and yellow `Warning:` (unreachable code, missing visibility, redundant imports, shadowing — params, fields by params, locals — unused locals). **The compiler never stops at the first problem**: the parser and semantic analyzer recover from each faulty statement and keep going, so every error and warning in the file prints before `jotc` exits with status 1. **Any error means nothing is compiled** — no assembly is written (a partial file is deleted), so warnings still build while errors do not. Diagnostics in imported files point at that file, not the entry file. Error coverage: string misuse (indexing non-strings, non-numeric index, `len` arity/type, mixed concat/compare, arithmetic on strings, `-=` on strings), structs/classes (duplicates, unknown types/fields/methods, `new` arity/type errors, methods in structs, member visibility modifiers, bare or imported method calls, bad method returns, iterating/printing instances, reserved `this`), inheritance (unknown base class, a class inheriting itself, inheritance cycles, subclass passed where an unrelated class is expected — overrides with different parameters only warn), `input` misuse (arity, non-string prompt), `break`/`continue` outside a loop, `null` used with anything but `==`/`!=`, return misuse (bare `return;` outside `-> void`, a value returned from `-> void`, using a void call's result), bad char literals, duplicate globals, and builtin misuse (`toStr`, `toNum`, `readFile`, `writeFile` arity/type). A missing delimiter points where the token belongs when the offender starts a new line, otherwise at the offender like gcc
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

Targets: `build` compiles `src/**/*.c`, `link` links `build/bin/jotc`, `debug` dumps tokens + AST + asm path, `run` compiles `src/main.jot` to asm, assembles, links, and executes it, `test` checks every `examples/**/*.jot` for byte-identical output at `-O0`/`-O1`/`-O2` plus CRLF and CLI smoke tests, `clean` deletes `build/`. Any step failing deletes `build/`. The Makefile supports both Windows and Unix-like systems. More runnable programs live under `examples/` (one folder per topic, file names say what they demo).

## Usage

```bash
./build/bin/jotc <file.jot> [-o output] [--debug] [-O0|-O1|-O2] [--emit-ir] [--emit-asm] [--target win64|elf64]
```

The `-o` flag specifies the output file (e.g., `main.exe`, `main.o`, or `main`). The compiler validates that the output directory exists and warns if the file already exists. Without `-o`, the assembly goes to `build/bin/generated/<name>.asm`. Without `--debug` only errors/warnings print. Exit status is `1` when anything failed to compile, `0` otherwise (warnings still compile).

Compiler options: `-O0` emits straightforward unoptimized code, `-O1` (default) enables safe basic optimizations, `-O2` adds local common-subexpression elimination. `--emit-ir` prints the optimized three-address IR to stdout and stops without writing assembly. `--emit-asm` prints the generated NASM assembly to stdout and stops without writing a file (byte-identical to the file output). `--target` selects `win64` (default on Windows) or `elf64` (default elsewhere); assemble `win64` output with `nasm -f win64` and `elf64` output with `nasm -f elf64`, then link with the platform `gcc`. `--help` (or `-h`, or no arguments) prints usage and exits `0`.

## Example

`src/main.jot` (what `make run` compiles):

```jot
fn public main() {
  num x = 2;
  num y = x + 4;
  print("Y: {y} \n");
  str greeting = "hello" + " jot";
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
fn add(num var1, num var2) -> num {
  num sum = self.var1 + self.var2;
  print("Sum: {sum}\n");
  return(sum);
}

add(10, 20);
```

prints `Sum: 30`.

Splitting across files (`math.jot`):

```jot
fn public add(num var1, num var2) -> num {
  num sum = self.var1 + self.var2;
  return(sum);
}
```

```jot
from [math.jot] import [add];

fn public main() {
  num r = add(20, 22);
  print("{r}\n");
  return(0);
}

main();
```

prints `42`.

Strings, structs, classes, and prompted input (`examples/strings/`, `examples/structs/`, `examples/classes/`, `examples/input/`):

```jot
str a = "hello";
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
  str bark() {
    return("BARK!");
  }
}

dog d = new dog();
d.bark();

num age = input("Age: ");
```

Arrays, nested instances, inheritance, imports, and everything above (`examples/arrays/`, `examples/nested/`, `examples/inheritance/`, `examples/imports/`):

```jot
arr nums = [10, 20, 30];
nums[1] = 99;
print(nums[1]);   // 99
for (n in nums) {
  print(n);
}
```

```jot
struct private point { num x; num y; }
struct private line { point start; point end; }

line l = new line(new point(1, 2), new point(3, 4));
print(l.start.x);   // 1
```

```jot
class public animal {
  str name;
  str speak() { return("generic sound"); }
}
class public dog inherit animal {
  str speak() { return(self.name + " barks"); }
}

dog d = new dog("Rex");
animal a = d;
print(a.speak());   // "Rex barks": dispatch uses the instance's class
```

```jot
from [helpers.jot] import [add, greet];

print(add(20, 22));   // 42
greet("jot");         // hello jot
```

Conversions, file I/O, null, and loop control:

```jot
print(toStr(3.5));       // 3.5
num n = toNum("2.75");   // 2.75

writeFile("out.txt", "hi\n");
str text = readFile("out.txt");
print(text);

str s = null;
if (s == null) {
  print("empty\n");
}

num i = 0;
while (i < 10) {
  i += 1;
  if (i == 3) {
    continue;            // skips the rest of the body
  }
  if (i > 5) {
    break;
  }
}
```

## Implementation

- Written in C (`-Wall -Wextra` clean)
- Pipeline: `src/lexer/` tokenizes source (`line`/`col` on every token); `src/parser/` parses (recursive descent to AST, statements chained via `right`, plus import merging and call validation); `src/sem/` resolves meaning (symbols, types, fields, methods, inheritance, conversions, returns — diagnostics keep their messages and locations); `src/ir/` lowers the checked program to typed three-address IR (basic blocks, explicit conversions, resolved calls/fields); `src/opt/` optimizes the IR; `src/codegen/` emits NASM x86-64 purely from IR; `src/terminal/` handles terminal detection + ANSI colors; `src/jotc.c` drives `Lexer` → `Parser` → `SemAnalyze` → `ir_build` → `opt_run` → `GenerateAssembly`
- IR: target-independent three-address code with enum opcodes (arithmetic, comparisons, conversions, loads/stores, jumps, calls, method dispatch, arrays, strings, instances, runtime builtins). `jotc --emit-ir file.jot` dumps it; it is internal and never part of the language
- Optimization: independently callable passes — constant folding (never folds trapping integer ops or non-finite floats; folds `len` of constant strings), constant/copy propagation, dead code and dead store elimination, unreachable block removal, algebraic identities (exact float ones only), branch/block comparison simplification, safe within-block load/store elimination, and local common-subexpression elimination (`-O2`). `-O0` runs no passes, `-O1` (default) runs the safe set. Every pass preserves traps and observable behavior: all 39 examples produce byte-identical output at `-O0`, `-O1`, and `-O2` on Windows, and at `-O1` on Linux
- Backend: the x86-64 generator consumes IR only (no AST) through instruction selection parameterized by operand locations, with compare-and-branch fusion (NaN-safe float forms included), jump-to-next elision, same-address read-modify-write forms, and lean direct-to-register call setup. `src/codegen/target.h`/`target.c` describe each target (argument registers, shadow space, caller/callee-saved classes, varargs rules, PLT calls); `src/codegen/regalloc.c` runs linear-scan allocation over real live intervals (caller-saved registers for short ranges, callee-saved with prologue saves across calls, frame-slot spills); `src/codegen/x86.c` selects register, immediate, and memory instruction forms; `src/codegen/codegen.c` drives per-function allocation and emission. No other architecture backends are claimed — the abstraction exists so they can plug in later
- Register allocation: temps (not variables) are allocated; scratch (`rax`, `rbx`, `r10`, `r11`, `xmm0`-`xmm3`) is never allocated, and every runtime sequence clobbers only scratch plus argument registers it spilled first, so values in saved registers and spill slots survive calls. For-loop array loads skip bounds checks (indices are in range by construction); everything else keeps its traps
- Runtime and memory: ownership and aliasing are unchanged (copies still happen exactly where the language requires them: instance binding, loop variables, concat results). Compile-time wins only: `len` of constants folds, dead stores vanish, `for` bounds checks on provably in-bounds indices are skipped. Deliberately not done: concat-chain fusion and `tostr` folding need target-specific runtime/format behavior; null-check elimination needs alias analysis; array-length specialization needs size tracking in IR
- Compiler speed: symbol tables are hash maps (recorded types, slots, function names), function tables grow dynamically (no 256-function cliff), and full-pipeline compile time matches the old single-pass compiler at every size measured (about 0.1s at 30–120 functions — both compilers overlap inside process-spawn noise there — and about 1.5s at 480, dominated by shared parsing and fixpoint scans)
- Testing: `make test` runs the committed runners (`tests/run_win.ps1` on Windows, `tests/run_unix.sh` elsewhere; both need `nasm` + `gcc` in `PATH`): every program under `examples/` (including `examples/opt/` for folding, propagation, dead code, stores, algebra, branches, comparisons incl. NaN, calls, strings, floats, control flow, and register pressure) plus `tests/stress/` (300 chained functions past the old 256 limit, 20-deep block nesting with long arithmetic chains, heavy mixed int/float/array/concat/conversion ops) must produce byte-identical output at `-O0`, `-O1`, and `-O2`, plus a CRLF source behaves like its LF original and CLI checks (`--help` exits 0, `--emit-ir`/`--emit-asm` work, bogus `--target` and missing files fail). A 76-case corpus checks diagnostics are byte-identical too. Compiling all examples takes a few seconds (about 0.1s per file, spawn-dominated). Measured code: `-O1` assembly is about the same size as the previous backend across all examples (about 99% of its bytes in total, LF-only vs the old CRLF output; about 96% of `-O0`). Measured runtime: outputs are identical at every level and against the old backend; timing is parity within run-to-run noise on sub-second micro-benchmarks (loop, array, concat, and recursive fib cases all agree exactly, recursion-heavy code runs about the same)
- Portability: sources are read and assembly written in binary mode (`"rb"`/`"wb"`), so `CRLF` input lexes exactly like `LF` (carriage returns are whitespace, snippets strip a trailing `\r`) and emitted `.asm` is LF-only on every host. Frame offsets are `int` with `%d` throughout (no `long` size assumptions); 64-bit values use `long long` with `%lld`/`%llx`. `_WIN32` ifdefs stay at three justified sites only: `src/jotc.c` (directory creation/stat), `src/terminal/terminal.c` (console color detection), `src/codegen/target.c` (host target default) — target logic itself lives in `target.h`/`target.c`, never scattered through the emitter
