#!/usr/bin/env python3
"""
Pre-commit hook for Zane project.
Automatically checks coding style before allowing commits.

Installation:
    Copy this file to .git/hooks/pre-commit (without .py extension)
    Or run: python tools/install_hooks.py
"""

import subprocess
import sys
from pathlib import Path

def main():
    # Get the root directory
    root = Path(__file__).parent.parent
    checker_script = root / 'tools' / 'check_style.py'
    
    if not checker_script.exists():
        print("⚠️  Warning: Style checker not found, skipping check")
        return 0
    
    # Get list of staged C++ files
    result = subprocess.run(
        ['git', 'diff', '--cached', '--name-only', '--diff-filter=ACM'],
        capture_output=True,
        text=True,
        cwd=root
    )
    
    if result.returncode != 0:
        return 0
    
    cpp_files = [
        f for f in result.stdout.strip().split('\n')
        if f.endswith(('.cpp', '.h')) and f
    ]
    
    if not cpp_files:
        return 0
    
    print("🔍 Checking coding style for staged files...")
    
    # Check each file
    has_errors = False
    for filepath in cpp_files:
        full_path = root / filepath
        if full_path.exists():
            result = subprocess.run(
                [sys.executable, str(checker_script), str(full_path)],
                cwd=root
            )
            if result.returncode != 0:
                has_errors = True
    
    if has_errors:
        print("\n❌ Commit rejected due to coding style violations.")
        print("💡 Fix the errors above or use 'git commit --no-verify' to skip this check.")
        return 1
    
    print("✅ Coding style check passed!")
    return 0

if __name__ == '__main__':
    sys.exit(main())
