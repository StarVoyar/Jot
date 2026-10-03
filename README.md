# Jot

A compiled programming language written in C.

## Current Status

Jot is in early development. Currently implements a lexer that tokenizes source code.

## Features

- **Keywords**: `return`
- **Literals**: Integer literals
- **Separators**: Semicolons, parentheses

## Building

```bash
make all
```

Or build manually:

```bash
make build
make link
make run
```

The Makefile supports both Windows and Unix-like systems.

## Usage

```bash
./build/bin/jotc <file.jot>
```

Or use the Makefile (uses `test/test.jot` by default):

```bash
make run
```

## Example

A simple Jot program:

```jot
return(0);
```

This tokenizes into:
- Return keyword
- Open parenthesis
- Integer literal (0)
- Close parenthesis
- Semicolon

## Implementation

- Written in C
- Compiler: `jotc`
- Lexer reads source files and outputs token information
- Cross-platform build system via Makefile
