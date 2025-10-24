#!/usr/bin/env python3
"""
Simple vidir test suite using relative paths and a temporary test directory.
This avoids all Windows path conversion issues by working entirely with relative paths.

Usage:
    python3 test_vidir.py [--vidir=vidir_command] [--python=python_command]
    
Examples:
    python3 test_vidir.py                                 # Use system vidir with python
    python3 test_vidir.py --vidir=./vidir.exe             # Use Windows C implementation with python
    python3 test_vidir.py --vidir=/path/to/vidir          # Use specific vidir binary with python
    python3 test_vidir.py --python=python                 # Use system vidir with python
    python3 test_vidir.py --vidir=./vidir.exe --python=py # Use specific vidir with py command
"""

import os
import subprocess
import tempfile
import shutil
import sys
import re

def create_fake_editor(test_dir, operations_code, python_command="python"):
    """Create a simple fake editor script that performs the specified operations."""
    editor_script = os.path.join(test_dir, "fake_editor.py")
    
    with open(editor_script, "w") as f:
        f.write(f"""#!/usr/bin/env python3
import sys
import re

# Read the temp file
with open(sys.argv[1], 'r') as f:
    content = f.read()

# Apply operations
{operations_code}

# Write back
with open(sys.argv[1], 'w') as f:
    f.write(content)
""")
    
    # Convert to forward slashes for busybox compatibility
    editor_script_unix = editor_script.replace("\\", "/")
    return f"{python_command} {editor_script_unix}"

def run_vidir_test(test_name, setup_files, editor_operations, expected_files, vidir_args=None, vidir_command="vidir", python_command="python3", expected_contents=None):
    """Run a single vidir test."""
    print(f"\n=== Testing: {test_name} ===")
    
    # Create test directory
    test_dir = f"test_{test_name.lower().replace(' ', '_')}"
    if os.path.exists(test_dir):
        shutil.rmtree(test_dir)
    os.makedirs(test_dir)
    
    try:
        # Change to test directory
        original_cwd = os.getcwd()
        os.chdir(test_dir)
        
        # Create initial files
        for filename, content in setup_files.items():
            if "/" in filename:  # Create subdirectories
                os.makedirs(os.path.dirname(filename), exist_ok=True)
            with open(filename, "w") as f:
                f.write(content)
        
        # Create fake editor
        editor_cmd = create_fake_editor(".", editor_operations, python_command)
        
        # Run vidir
        env = os.environ.copy()
        env["EDITOR"] = editor_cmd
        
        # Use specified args or default to current directory
        if vidir_args is None:
            vidir_args = ["."]
        
        # Use the specified vidir command
        if isinstance(vidir_command, str):
            cmd = [vidir_command] + vidir_args
        else:
            cmd = vidir_command + vidir_args
        
        result = subprocess.run(cmd, env=env, capture_output=True, text=True)
        
        print(f"Return code: {result.returncode}")
        if result.stdout:
            print(f"Stdout: {result.stdout}")
        if result.stderr:
            print(f"Stderr: {result.stderr}")
        
        # Check results
        actual_files = set()
        for root, dirs, files in os.walk("."):
            for file in files:
                if file != "fake_editor.py":  # Exclude our test script
                    path = os.path.relpath(os.path.join(root, file), ".")
                    path = path.replace("\\", "/")  # Normalize path separators
                    actual_files.add(path)
        
        expected_set = set(expected_files)
        
        # First check if files exist as expected
        if actual_files != expected_set:
            print(f"✗ FAIL: {test_name} - File structure mismatch")
            print(f"  Expected: {sorted(expected_set)}")
            print(f"  Actual:   {sorted(actual_files)}")
            return False
            
        # Then check file contents if expected_contents is provided
        if expected_contents:
            for filename, expected_content in expected_contents.items():
                if filename in actual_files:
                    try:
                        with open(filename, 'r') as f:
                            actual_content = f.read()
                        if actual_content != expected_content:
                            print(f"✗ FAIL: {test_name} - Content mismatch in {filename}")
                            print(f"  Expected content: {repr(expected_content)}")
                            print(f"  Actual content:   {repr(actual_content)}")
                            return False
                    except Exception as e:
                        print(f"✗ FAIL: {test_name} - Could not read {filename}: {e}")
                        return False
                else:
                    print(f"✗ FAIL: {test_name} - Expected file {filename} not found")
                    return False
        
        print(f"✓ PASS: {test_name}")
        return True
            
    finally:
        # Clean up
        os.chdir(original_cwd)
        if os.path.exists(test_dir):
            shutil.rmtree(test_dir)

def main():
    """Run all vidir tests."""
    # Parse command line arguments
    vidir_command = "vidir"
    python_command = "python3"
    
    # Parse arguments
    args = sys.argv[1:]
    
    for arg in args:
        if arg.startswith("--vidir="):
            vidir_command = arg.split("=", 1)[1]
        elif arg.startswith("--python="):
            python_command = arg.split("=", 1)[1]
        else:
            print(f"Error: Unknown argument '{arg}'. Use --vidir=command or --python=command")
            return 1
    
    # Convert relative paths to absolute paths so they work from any directory
    if vidir_command != "vidir" and not os.path.isabs(vidir_command):
        vidir_command = os.path.abspath(vidir_command)
 
    print(f"Testing vidir command: {vidir_command}")
    
    print(f"Using Python command: {python_command}")
    
    tests_passed = 0
    tests_total = 0
    
    # Test 1: Simple rename
    tests_total += 1
    if run_vidir_test(
        "Simple Rename",
        {"file1.txt": "content1", "file2.txt": "content2"},
        'content = content.replace("file1.txt", "renamed.txt")',
        ["renamed.txt", "file2.txt"],
        vidir_command=vidir_command,
        python_command=python_command
    ):
        tests_passed += 1
    
    # Test 2: Delete file
    tests_total += 1
    if run_vidir_test(
        "Delete File", 
        {"file1.txt": "content1", "file2.txt": "content2"},
        'content = "\\n".join(line for line in content.split("\\n") if "file1.txt" not in line)',
        ["file2.txt"],
        vidir_command=vidir_command,
        python_command=python_command
    ):
        tests_passed += 1
    
    # Test 3: Rename with subdirectory
    tests_total += 1
    if run_vidir_test(
        "Rename to Subdirectory",
        {"file1.txt": "content1", "file2.txt": "content2"},
        'content = content.replace("file1.txt", "subdir/file1.txt")',
        ["subdir/file1.txt", "file2.txt"],
        vidir_command=vidir_command,
        python_command=python_command
    ):
        tests_passed += 1
    
    # Test 4: Move from subdirectory
    tests_total += 1
    if run_vidir_test(
        "Move from Subdirectory",
        {"dir1/file1.txt": "content1", "file2.txt": "content2"},
        'content = content.replace("dir1/file1.txt", "moved_file.txt")',
        ["moved_file.txt", "file2.txt"],
        ["dir1", "file2.txt"],  # Tell vidir to process dir1 and file2.txt explicitly
        vidir_command,
        python_command
    ):
        tests_passed += 1
    
    # Test 5: Rename cycle (A->B, B->A)
    tests_total += 1
    if run_vidir_test(
        "Rename Cycle",
        {"fileA.txt": "contentA", "fileB.txt": "contentB"},
        '''
lines = content.strip().split("\\n")
new_lines = []
for line in lines:
    if "fileA.txt" in line:
        new_lines.append(line.replace("fileA.txt", "fileB.txt"))
    elif "fileB.txt" in line:
        new_lines.append(line.replace("fileB.txt", "fileA.txt"))
    else:
        new_lines.append(line)
content = "\\n".join(new_lines) + "\\n"
        ''',
        ["fileA.txt", "fileB.txt"],  # Files should be swapped
        vidir_command=vidir_command,
        python_command=python_command
    ):
        tests_passed += 1
    
    # Test 6: Complex directory operations
    tests_total += 1
    if run_vidir_test(
        "Complex Directory Operations",
        {
            "dir1/file1.txt": "content1",
            "dir1/file2.txt": "content2", 
            "dir2/file3.txt": "content3"
        },
        '''
# Move all files from dir1 to dir2, and rename dir2 to dir3
content = content.replace("dir1/", "dir2/")
content = content.replace("dir2/", "dir3/")
        ''',
        ["dir3/file1.txt", "dir3/file2.txt", "dir3/file3.txt"],
        ["dir1", "dir2"],  # Tell vidir to process dir1 and dir2 explicitly
        vidir_command,
        python_command
    ):
        tests_passed += 1

    # Test 7: Create new directories via rename
    tests_total += 1
    if run_vidir_test(
        "Create New Directories",
        {"file1.txt": "content1", "file2.txt": "content2"},
        'content = content.replace("file1.txt", "newdir/subdir/file1.txt")',
        ["newdir/subdir/file1.txt", "file2.txt"],
        vidir_command=vidir_command,
        python_command=python_command
    ):
        tests_passed += 1

    # Test 8: Multiple file operations
    tests_total += 1
    if run_vidir_test(
        "Multiple Operations",
        {
            "a.txt": "content_a",
            "b.txt": "content_b", 
            "c.txt": "content_c",
            "d.txt": "content_d"
        },
        '''
# Complex multi-file operations: rename, move to subdirs, delete
content = content.replace("a.txt", "renamed_a.txt")
content = content.replace("b.txt", "subdir1/b.txt") 
content = content.replace("c.txt", "subdir2/renamed_c.txt")
# Delete d.txt by removing its line
lines = content.strip().split("\\n")
lines = [line for line in lines if not line.endswith("d.txt")]
content = "\\n".join(lines) + "\\n"
        ''',
        ["renamed_a.txt", "subdir1/b.txt", "subdir2/renamed_c.txt"],
        vidir_command=vidir_command,
        python_command=python_command
    ):
        tests_passed += 1

    # Test 9: Deep directory nesting - vidir shows directory entries when not expanded explicitly
    tests_total += 1
    if run_vidir_test(
        "Deep Directory Nesting",
        {"deep/nested/path/file.txt": "deep_content"},
        'content = content.replace("deep/nested", "very/deeply/nested")',
        ["very/deeply/nested/path/file.txt"],
        ["deep"],
        vidir_command,
        python_command
    ):
        tests_passed += 1

    # Test 10: Mixed file and directory operations - vidir shows directory entries unless expanded explicitly
    tests_total += 1
    if run_vidir_test(
        "Mixed Operations",
        {
            "standalone.txt": "content1",
            "project/src/main.py": "python_code",
            "project/docs/readme.md": "documentation",
            "temp/cache.tmp": "temp_data"
        },
        '''
# Move standalone file into project, reorganize project structure, delete temp
content = content.replace("standalone.txt", "project/standalone.txt")
content = content.replace("project/docs", "project/documentation")
content = content.replace("project/src", "project/code")
# Delete temp directory by removing its line
lines = content.strip().split("\\n")
lines = [line for line in lines if not line.endswith("temp/cache.tmp")]
content = "\\n".join(lines) + "\\n"
        ''',
        [
            "project/standalone.txt",
            "project/code/main.py", 
            "project/documentation/readme.md"
        ],
        ["standalone.txt", "project", "temp"],
        vidir_command,
        python_command
    ):
        tests_passed += 1

    # Test 11: Three-way cycle test (a->b->c->a) to test cycle detection and unstash
    tests_total += 1
    if run_vidir_test(
        "Three-way Cycle",
        {
            "a.txt": "Content of A",
            "b.txt": "Content of B", 
            "c.txt": "Content of C"
        },
        '''
# Create a cycle: a.txt -> b.txt, b.txt -> c.txt, c.txt -> a.txt
content = content.replace("1\\t./a.txt", "1\\t./b.txt")
content = content.replace("2\\t./b.txt", "2\\t./c.txt")
content = content.replace("3\\t./c.txt", "3\\t./a.txt")
        ''',
        ["a.txt", "b.txt", "c.txt"],  # All three files should exist with rotated content
        vidir_command=vidir_command,
        python_command=python_command,
        expected_contents={
            "a.txt": "Content of C",  # a.txt gets content from c.txt
            "b.txt": "Content of A",  # b.txt gets content from a.txt  
            "c.txt": "Content of B"   # c.txt gets content from b.txt
        }
    ):
        tests_passed += 1

    # Test 12: Multiple cycles test (tests multiple stash operations)
    tests_total += 1
    if run_vidir_test(
        "Multiple Cycles",
        {
            "cycle1_a.txt": "Content A1",
            "cycle1_b.txt": "Content B1",
            "cycle1_c.txt": "Content C1",
            "cycle2_x.txt": "Content X2", 
            "cycle2_y.txt": "Content Y2",
            "cycle2_z.txt": "Content Z2"
        },
        '''
# Create two separate cycles:
# Cycle 1: cycle1_a -> cycle1_b -> cycle1_c -> cycle1_a
# Cycle 2: cycle2_x -> cycle2_y -> cycle2_z -> cycle2_x
content = content.replace("1\\t./cycle1_a.txt", "1\\t./cycle1_b.txt")
content = content.replace("2\\t./cycle1_b.txt", "2\\t./cycle1_c.txt")
content = content.replace("3\\t./cycle1_c.txt", "3\\t./cycle1_a.txt")
content = content.replace("4\\t./cycle2_x.txt", "4\\t./cycle2_y.txt")
content = content.replace("5\\t./cycle2_y.txt", "5\\t./cycle2_z.txt")
content = content.replace("6\\t./cycle2_z.txt", "6\\t./cycle2_x.txt")
        ''',
        ["cycle1_a.txt", "cycle1_b.txt", "cycle1_c.txt", "cycle2_x.txt", "cycle2_y.txt", "cycle2_z.txt"],
        vidir_command=vidir_command,
        python_command=python_command,
        expected_contents={
            # Cycle 1: a->b->c->a (a gets c's content, b gets a's content, c gets b's content)
            "cycle1_a.txt": "Content C1",
            "cycle1_b.txt": "Content A1", 
            "cycle1_c.txt": "Content B1",
            # Cycle 2: x->y->z->x (x gets z's content, y gets x's content, z gets y's content)
            "cycle2_x.txt": "Content Z2",
            "cycle2_y.txt": "Content X2",
            "cycle2_z.txt": "Content Y2"
        }
    ):
        tests_passed += 1
    
    # Test: Duplicate Targets (Perl-compatible: last one wins, earlier files moved aside)
    tests_total += 1
    if run_vidir_test(
        "Duplicate Targets",
        {
            "file1.txt": "content1",
            "file2.txt": "content2", 
            "file3.txt": "content3"
        },
        '''
# Rename all three files to the same target
content = content.replace("1\\t./file1.txt", "1\\t./target.txt")
content = content.replace("2\\t./file2.txt", "2\\t./target.txt")
content = content.replace("3\\t./file3.txt", "3\\t./target.txt")
        ''',
        ["target.txt", "target.txt~", "target.txt~1"],  # Expected result files
        ["file1.txt", "file2.txt", "file3.txt"],  # Pass explicit files to vidir
        vidir_command,
        python_command,
        expected_contents={
            # Last one wins - file3 becomes target.txt
            "target.txt": "content3",
            # Earlier files are moved aside with ~ suffixes
            "target.txt~": "content1",
            "target.txt~1": "content2"
        }
    ):
        tests_passed += 1
    
    print(f"\n=== Test Results ===")
    print(f"Passed: {tests_passed}/{tests_total}")
    
    if tests_passed == tests_total:
        print("All tests passed! ✓")
        return 0
    else:
        print(f"Some tests failed. ✗")
        return 1

if __name__ == "__main__":
    sys.exit(main())
