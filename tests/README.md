# vidir Tests

This directory contains test suites for the vidir C implementation.

## Test Files

- `test_vidir.py` - Comprehensive test suite covering:
  - Simple file renaming
  - File deletion
  - Directory operations
  - Complex multi-file operations
  - Cycle detection and resolution
  - Nested directory handling

## Running Tests

### Basic Usage

To run the test suite against vidir implementation:

```bash
cd tests

# Test with relative path to vidir.exe (recommended)
python test_vidir.py --vidir=../vidir.exe --python=python

# Test with absolute path
python test_vidir.py --vidir=c:/path.to/vidir.exe --python=python

# Test with system vidir (if available in PATH)
python test_vidir.py --python=python
```

### Command Line Options

- `--vidir=command` - Specify the vidir executable to test (supports relative and absolute paths)
- `--python=command` - Specify the Python command to use (default: `python3`, use `python` on Windows)

### Examples

```bash
# Windows with local vidir.exe build
python test_vidir.py --vidir=../vidir.exe --python=python

# Linux/Unix with system vidir
python test_vidir.py --vidir=vidir --python=python3

# Test specific vidir binary
python test_vidir.py --vidir=/usr/local/bin/vidir --python=python3
```

## How It Works

The test suite creates temporary test directories and cleans them up automatically.
It uses a fake editor script to simulate user edits for automated testing.

Each test:
1. Creates a temporary test directory with initial files
2. Generates a Python script that acts as a fake editor
3. Sets the `EDITOR` environment variable to use the fake editor
4. Runs vidir with the specified command
5. Verifies the expected file operations were performed
6. Cleans up the temporary directory

## Test Coverage

The test suite validates:
- ✅ File renaming and moving
- ✅ Directory creation (auto-creates missing directories) 
- ✅ File deletion (by removing from temp file)
- ✅ Cycle detection and resolution (using stash mechanism)
- ✅ Complex multi-step operations
- ✅ Proper directory handling
- ✅ Nested directory operations
- ✅ Mixed file and directory operations

All tests pass with the current vidir C implementation! ✅
