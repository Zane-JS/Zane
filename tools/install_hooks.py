#!/usr/bin/env python3
"""
Install Git hooks for Zane project.

This script installs the pre-commit hook that automatically checks
coding style before allowing commits.

Usage:
    python tools/install_hooks.py
"""

import os
import shutil
import sys
from pathlib import Path

def main():
    root = Path(__file__).parent.parent
    git_hooks_dir = root / '.git' / 'hooks'
    
    if not git_hooks_dir.exists():
        print("❌ Error: .git/hooks directory not found")
        print("   Make sure you're running this from the Zane-app directory")
        return 1
    
    # Install pre-commit hook
    source = root / 'tools' / 'pre-commit.py'
    target = git_hooks_dir / 'pre-commit'
    
    if not source.exists():
        print(f"❌ Error: {source} not found")
        return 1
    
    # Copy the hook
    shutil.copy2(source, target)
    
    # Make it executable (on Unix-like systems)
    if os.name != 'nt':  # Not Windows
        os.chmod(target, 0o755)
    
    print("✅ Git hooks installed successfully!")
    print(f"   Pre-commit hook: {target}")
    print("\n📝 The pre-commit hook will now check coding style before each commit.")
    print("   To skip the check, use: git commit --no-verify")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
