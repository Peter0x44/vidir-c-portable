# vidir Tests

This directory contains test suites for the vidir C implementation.

## Test Files

- `test_vidir_simple.py` - Comprehensive test suite covering:
  - Simple file renaming
  - File deletion
  - Directory operations
  - Complex multi-file operations
  - Cycle detection and resolution
  - Nested directory handling

## Running Tests

To run the test suite:

```bash
cd tests
python test_vidir_simple.py
```

The test suite creates temporary test directories and cleans them up automatically.
It uses a fake editor script to simulate user edits for automated testing.

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
