# Shell Scripts

EYN-OS includes a built-in shell script interpreter for `.shell` files.  Scripts
are line-oriented text files that are read from the filesystem and executed
line-by-line through the normal shell command dispatch path.  This means every
program in `/binaries/`, every alias, and pipeline syntax all work transparently
inside scripts.

## Running a Script

Scripts can be executed in several ways:

```
# By name -- if the file lives in /binaries/ and starts with '#' it is
# auto-detected as a script (no extension needed):
build_doom

# By explicit path:
run /shell/myscript.shell

# With arguments:
build_doom arg1 arg2
```

Scripts stored as extensionless files in `/binaries/` are auto-detected by
checking the first byte of the file.  If it is `#` (comment / shebang), the
file is run through the script interpreter instead of as a UELF binary.

Files with an explicit `.shell` extension are always dispatched to the
interpreter.

## Language Reference

### Comments

Lines beginning with `#` (after optional whitespace) are comments and are
ignored.  Inline comments are **not** supported -- the `#` must be the first
non-whitespace character on the line.

```shell
# This is a comment
echo hello   # this is NOT a comment -- the '#' is passed to the command
```

### Variables

Variables are defined with `set` and referenced with `$NAME` or `${NAME}`:

```shell
set GREETING=hello
set SUBJECT=world
echo $GREETING $SUBJECT        # prints: hello world
echo ${GREETING}_${SUBJECT}    # prints: hello_world
```

Variable names may contain letters, digits, and underscores.  Values are
everything after the `=` to the end of the line (no quoting needed).

**Script arguments** are available as `$0` (the script path), `$1`, `$2`, ...
up to `$9`:

```shell
# If invoked as:  myscript foo bar
echo Script: $0    # /binaries/myscript
echo First: $1     # foo
echo Second: $2    # bar
```

### Command Substitution

`$(command args)` captures the standard output of a command and substitutes it
inline:

```shell
set FILES=$(ls /binaries)
echo Available commands: $FILES
```

Nested substitution is supported: `$(echo $(cat /config/name))`.

### Conditionals

```shell
if CONDITION
    # commands when true
elif CONDITION
    # alternative
else
    # fallback
endif
```

`elif` and `else` are optional.  Nesting is supported up to 16 levels.

**Condition syntax:**

| Form | Meaning |
|------|---------|
| `A == B` | String equality (after variable expansion) |
| `A != B` | String inequality |
| `-e PATH` | File or directory exists |
| `-f PATH` | File exists (not a directory) |
| `WORD` | True if non-empty and not `"0"` |

Example:

```shell
if -f /binaries/doom
    echo DOOM binary exists
else
    echo DOOM binary not found -- building...
    build_doom
endif
```

### While Loops

```shell
set COUNT=3
while $COUNT != 0
    echo Countdown: $COUNT
    # (no arithmetic yet -- decrement manually or use an external tool)
    set COUNT=$(decrement $COUNT)
endwhile
```

The condition is re-evaluated on each iteration.  Use `exit` inside when
needed to break out early.

### Echo

`echo` is a built-in that prints its arguments followed by a newline:

```shell
echo Hello, world!
echo Build starting for $TARGET...
```

### Exit

`exit` terminates the script immediately:

```shell
if -f /error.flag
    echo Aborting due to error flag
    exit
endif
```

### Commands

Any line that is not a keyword (`set`, `echo`, `exit`, `if`, `elif`, `else`,
`endif`, `while`, `endwhile`) is dispatched through the shell's normal command
handler.  This means:

- All programs in `/binaries/` work: `chibicc`, `doom`, `ls`, `cat`, etc.
- Aliases are expanded.
- Pipelines work: `ls | grep doom`.
- `|` is backed by kernel pipe FDs; shell stages are launched in-order with
    stdin/stdout remapped to pipe ends.
- File redirection: commands using `>` behave the same as in interactive mode.

```shell
# Compile and run
chibicc -m32 -I/DOOM -S -o /tmp.s /DOOM/doom_unity.c
chibicc --as /tmp.s -o /binaries/doom_chibicc
doom_chibicc
```

## Limits

| Constant | Value | Notes |
|----------|-------|-------|
| Max variables | 64 | Per script invocation |
| Max line length | 512 | After variable expansion |
| Max nesting depth | 16 | Combined if/while depth |
| Max script size | 64 KB | File size on disk |
| Max lines | 2048 | Per script |
| Max arguments | 10 | `$0` through `$9` |

## Example: build_doom

A complete example script that compiles DOOM from source using chibicc:

```shell
# build_doom -- Compile DOOM from source using chibicc on EYN-OS
set CC=chibicc
set SRC=/DOOM/doom_unity.c
set ASM=/doom_compiled.s
set OUT=/binaries/doom_chibicc

echo [1/2] Compiling C to assembly...
$CC -m32 -I/DOOM -I/include -DNORMALUNIX -DLINUX -USNDSERV -S -o $ASM $SRC

if -f $ASM
    echo [2/2] Assembling and linking...
    $CC --as $ASM -o $OUT
    if -f $OUT
        echo Build complete: $OUT
    else
        echo Error: Linking failed
    endif
else
    echo Error: Compilation failed
endif
```
