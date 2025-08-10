# vidir

A C implementation of vidir from [moreutils](https://joeyh.name/code/moreutils/). Supports editing filenames in a visual manner.

## Features

- **Windows XP Compatible**: Uses Win32 API calls compatible with Windows XP and later.
- **Unicode Support**: Full Unicode support. Editor must support UTF-8.
- **No CRT Dependency**: Links only against kernel32.dll and shell.dll.
- **Unity Build System**: Only a single .c file to compile .
- **Public Domain**: Dedicated to the public domain.

## Building

### Using GCC (MinGW-w64)
```sh
gcc main_windows.c -o vidir.exe -nostdlib -nostartfiles -lkernel32 -lshell32
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
3. Falls back to `notepad.exe`

## TODO:
Make above comment true.
Port platform layer to libc.
Port platform layer to Linux syscalls.
Don't always run editors through busybox sh.
Test on Windows XP. (no theoretical reason this shouldn't work - I just haven't done it.).
Change relevant asserts to actual failures.