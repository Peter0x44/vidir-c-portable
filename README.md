# vidir

A C implementation of vidir from [moreutils](https://joeyh.name/code/moreutils/). Supports editing filenames in a visual manner.

## Features

- **Cross-platform**: Works on Windows (XP+) and POSIX systems (Linux, macOS, BSD)
- **Unicode Support**: Full Unicode support. Editor must support UTF-8.
- **No CRT Dependency**: Windows version links only against kernel32.dll and shell32.dll.
- **Unity Build System**: Only a single .c file to compile.
- **Public Domain**: Dedicated to the public domain.

## Building

### POSIX
cc -o vidir main_posix.c
```

### Windows (MinGW-w64)
```sh
x86_64-w64-mingw32-gcc main_windows.c -o vidir.exe -nostdlib -nostartfiles -lkernel32 -lshell32
```

## Usage

```sh
vidir [--verbose] [directory|file|-]...
```

- `vidir` - Edit current directory
- `vidir somedir` - Edit contents of somedir
- `vidir file1 file2` - Edit specific files
- `vidir -` - Read file list from stdin
- `vidir --verbose` - Show verbose output

## Editor Configuration

The program respects the following environment variables (in order of preference):
1. `VISUAL`
2. `EDITOR`
3. Falls back to build-time DEFAULT_EDITOR (by default, `vi` on POSIX systems, `notepad` on Windows)

## TODO:
* Test on Windows XP. (no theoretical reason this shouldn't work - I just haven't done it.)