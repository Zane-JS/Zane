#!/usr/bin/env python3
"""
Zane Coding Style Checker
Automatically validates C++ code against Zane coding standards.

Usage:
    python check_style.py [path]
    
    If no path is provided, checks all C++ files in src/
"""

import re
import sys
from pathlib import Path
from typing import List, Set, Tuple


# ---------------------------------------------------------------------------
# Helpers for naming-convention checks
# ---------------------------------------------------------------------------

def _is_snake_case(name: str) -> bool:
    """Return True if *name* is pure snake_case (lowercase letters, digits, underscores)."""
    return bool(re.fullmatch(r'[a-z][a-z0-9_]*', name))


def _is_camel_case(name: str) -> bool:
    """Return True if *name* starts with a lowercase letter (camelCase)."""
    return bool(re.fullmatch(r'[a-z][a-zA-Z0-9]*', name))


def _is_pascal_case(name: str) -> bool:
    """Return True if *name* is PascalCase."""
    return bool(re.fullmatch(r'[A-Z][a-zA-Z0-9]*', name))


def _is_upper_snake(name: str) -> bool:
    """Return True if *name* is UPPER_SNAKE_CASE (constants / macros)."""
    return bool(re.fullmatch(r'[A-Z][A-Z0-9_]*', name))


# Prefixed pointer/smart-pointer helpers
def _check_prefixed(name: str, prefix: str) -> bool:
    """Return True if *name* starts with *prefix* and the rest is snake_case."""
    if not name.startswith(prefix):
        return False
    rest = name[len(prefix):]
    return _is_snake_case(rest)


# ---------------------------------------------------------------------------
# Token / line-level parsing utilities
# ---------------------------------------------------------------------------

def _strip_strings_and_comments(line: str) -> str:
    """Remove string literals and line comments so we don't false-positive on them."""
    # Remove // comments
    result = re.sub(r'//.*', '', line)
    # Remove double-quoted strings (simplified, ignores escaped quotes inside)
    result = re.sub(r'"[^"]*"', '""', result)
    # Remove single-quoted char literals
    result = re.sub(r"'[^']*'", "''", result)
    return result

def _strip_angle_brackets(text: str) -> str:
    """
    Remove <...> content recursively (for nested templates).
    This prevents flagging 'int' inside 'std::vector<int>' or 'static_cast<int>'.
    """
    while '<' in text:
        # Match the innermost <...>
        new_text = re.sub(r'<[^<>]+>', '<>', text)
        if new_text == text:
            break
        text = new_text
    return text

# ---------------------------------------------------------------------------
# Context tracker: detect whether we're inside a class/struct body
# ---------------------------------------------------------------------------

class ScopeTracker:
    """Tracks brace depth and whether we entered a class/struct scope."""

    def __init__(self):
        self._stack: List[str] = []  # 'class' | 'func' | 'other'

    def process_line(self, line: str):
        clean = _strip_strings_and_comments(line)
        
        # Detect class/struct header (may be without opening brace yet)
        # Handle cases like "class Runtime {" or "struct Foo\n{"
        if re.search(r'\b(class|struct)\s+\w+', clean):
            self._pending_marker = 'class'
        elif re.search(r'(?:[\w:<>*&]+\s+)*\w+\s*\([^;{]*\)\s*(const)?\s*\{?', clean):
            # Likely a function/method definition
            if not any(kw in clean for kw in ('if', 'while', 'for', 'switch', 'return')):
                self._pending_marker = 'func'
            else:
                self._pending_marker = None
        else:
            self._pending_marker = None

        for ch in clean:
            if ch == '{':
                self._stack.append(self._pending_marker if hasattr(self, '_pending_marker') and self._pending_marker else 'other')
                self._pending_marker = None
            elif ch == '}':
                if self._stack:
                    self._stack.pop()

    def in_class(self) -> bool:
        """True if we are anywhere inside a class/struct (including its methods)."""
        return 'class' in self._stack

    def directly_in_class(self) -> bool:
        """True if the IMMEDIATE enclosing scope is a class/struct (for member declarations)."""
        return bool(self._stack) and self._stack[-1] == 'class'

    def in_function_body(self) -> bool:
        """True when we're inside a function/lambda body."""
        return 'func' in self._stack or (bool(self._stack) and self._stack[-1] == 'other')

    def depth(self) -> int:
        return len(self._stack)


# ---------------------------------------------------------------------------
# Main checker
# ---------------------------------------------------------------------------

# System / OS structs whose names are fixed by the platform ABI — exempt from
# PascalCase checks.
_SYSTEM_STRUCT_NAMES: Set[str] = {
    # POSIX / Linux
    'stat', 'stat64', 'fstat', 'dirent', 'tm', 'timespec', 'timeval',
    'sockaddr', 'sockaddr_in', 'sockaddr_in6', 'sockaddr_storage',
    'addrinfo', 'iovec', 'pollfd', 'epoll_event', 'rlimit', 'rusage',
    'sigaction', 'passwd', 'group', 'utsname', 'termios', 'winsize',
    'itimerval', 'siginfo_t',
    # Windows
    'OVERLAPPED', 'SECURITY_ATTRIBUTES', 'STARTUPINFO', 'STARTUPINFOW',
    'PROCESS_INFORMATION', 'SYSTEM_INFO', 'MEMORYSTATUSEX', 'FILETIME',
    'SYSTEMTIME', 'BY_HANDLE_FILE_INFORMATION', 'WIN32_FIND_DATA',
    'CRITICAL_SECTION', 'CONDITION_VARIABLE', 'SRWLOCK',
}

# Additional prefix patterns for system structs
_SYSTEM_STRUCT_PREFIXES = ('_', 'Win32', 'WNDCLASS')

# System macros that are allowed to be lowercase / non-UPPER_SNAKE
_SYSTEM_MACROS: Set[str] = {
    'fileno', 'isatty', 'ssize_t', 'stdin', 'stdout', 'stderr',
    'EOF', 'BUFSIZ', 'FILENAME_MAX', 'FOPEN_MAX', 'L_tmpnam', 'NULL',
    'offsetof', 'assert', 'errno', 'O_RDONLY', 'O_WRONLY', 'O_RDWR',
    'O_CREAT', 'O_TRUNC', 'O_APPEND', 'pread', 'pwrite', 'read', 'write',
}

# Windows API type names - exempt from snake_case / m_ checks
_WIN32_TYPES: Set[str] = {
    'HANDLE', 'DWORD', 'BOOL', 'WORD', 'BYTE', 'LONG', 'ULONG',
    'HMODULE', 'HINSTANCE', 'HWND', 'HDC', 'HPEN', 'HBRUSH',
    'LPCSTR', 'LPSTR', 'LPCWSTR', 'LPWSTR', 'LPVOID', 'LPDWORD',
    'UINT', 'ULONGLONG', 'LONGLONG', 'INT', 'SIZE_T', 'SSIZE_T',
    'SOCKET', 'WSADATA', 'SOCKADDR', 'FILETIME',
}


class StyleChecker:
    def __init__(self):
        self.errors: List[str] = []
        self.warnings: List[str] = []
        self.files_checked = 0

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def check_file(self, filepath: Path) -> bool:
        """Check a single file for style violations."""
        if filepath.suffix not in ('.cpp', '.hpp'):
            return True

        # Skip external files
        skip_patterns = ['temporal_shims.cpp', 'v8', 'libs/']
        if any(pattern in str(filepath) for pattern in skip_patterns):
            return True

        self.files_checked += 1
        content = filepath.read_text(encoding='utf-8', errors='ignore')
        lines = content.split('\n')

        has_errors = False
        scope = ScopeTracker()

        # Pre-scan: collect namespace aliases defined in this file
        _NS_ALIAS_RE = re.compile(r'^\s*namespace\s+(\w+)\s*=\s*[\w:]+\s*;')
        ns_aliases: Set[str] = set()
        for raw_line in lines:
            alias_m = _NS_ALIAS_RE.match(raw_line)
            if alias_m:
                ns_aliases.add(alias_m.group(1))  # e.g. 'fs'

        for i, line in enumerate(lines, 1):
            scope.process_line(line)
            clean = _strip_strings_and_comments(line)

            # ── 1. Methods: must be camelCase ──────────────────────────────
            method_m = re.match(
                r'^(?:[\w:<>*&\s]+?\s+)?([A-Z]\w*)::([A-Za-z_]\w*)\s*\(', clean)
            if method_m and not self._is_external_ns(clean, ns_aliases):
                owner = method_m.group(1)
                mname = method_m.group(2)
                # Skip destructors, operators
                if not mname.startswith('~') and not mname.startswith('operator'):
                    if mname != owner:  # not a constructor
                        if not _is_camel_case(mname):
                            self.errors.append(
                                f"{filepath}:{i}: Method/function '{mname}' "
                                f"should use camelCase"
                            )
                            has_errors = True

            # ── 2. Member variables ────────────────────────────────────────
            if scope.directly_in_class():
                self._check_member_variable(filepath, i, clean, ns_aliases)

            # ── 3. Local variables ─────────────────────────────────────────
            if scope.in_function_body() and not scope.directly_in_class():
                self._check_local_variable(filepath, i, clean, ns_aliases)

            # ── 4. Constants / Macros (#define) ───────────────────────────
            define_m = re.match(r'^\s*#define\s+([A-Za-z_]\w*)', clean)
            if define_m:
                cname = define_m.group(1)
                if (not cname.startswith('_')
                        and cname not in _SYSTEM_MACROS
                        and not _is_upper_snake(cname)):
                    self.warnings.append(
                        f"{filepath}:{i}: Macro '{cname}' should be "
                        f"UPPER_SNAKE_CASE"
                    )

            # ── 5. Class / Struct names: PascalCase ────────────────────────
            class_m = re.search(r'\b(class|struct)\s+([A-Za-z_]\w*)', clean)
            if class_m:
                cname = class_m.group(2)
                if '::' not in clean[class_m.start():class_m.end() + 20]:
                    if (cname not in _SYSTEM_STRUCT_NAMES
                            and not any(cname.startswith(pfx)
                                        for pfx in _SYSTEM_STRUCT_PREFIXES)
                            and cname not in _WIN32_TYPES
                            and not _is_pascal_case(cname)):
                        self.warnings.append(
                            f"{filepath}:{i}: Class/Struct '{cname}' should "
                            f"use PascalCase"
                        )

            # ── 6. Raw integer types (int, long, short, unsigned) ──────────
            if not clean.strip().startswith('#define'):
                self._check_integer_types(filepath, i, clean)

            # ── 7. Forbidden: using namespace ... ─────────────────────────
            if re.search(r'\busing\s+namespace\b', clean):
                self.errors.append(
                    f"{filepath}:{i}: 'using namespace' is PROHIBITED "
                    f"(see coding style §5)"
                )
                has_errors = True

            # ── 8. NULL instead of nullptr ─────────────────────────────────
            if re.search(r'\bNULL\b', clean):
                self.warnings.append(
                    f"{filepath}:{i}: Use 'nullptr' instead of 'NULL'"
                )

        return not has_errors

    # ------------------------------------------------------------------
    # Naming convention helpers
    # ------------------------------------------------------------------

    _MEMBER_TYPE_RE = re.compile(
        r'^\s+(?P<type>'
        r'(?:(?:const|static|mutable|volatile|inline)\s+)*'
        r'(?:(?:u?int(?:8|16|32|64)_t|bool|double|float|char|auto|size_t)'
        r'|(?:std::\w+(?:<[^>]*>)?)'
        r'|(?:v8::\w+(?:<[^>]*>)?)'
        r'|(?:[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:<[^>]*>)?))'
        r'(?:\s*[*&]+)?)\s+'
        r'(?P<name>[A-Za-z_]\w*)'
        r'\s*(?:[=;{]|$)'
    )

    _LOCAL_TYPE_RE = re.compile(
        r'^\s{4,}(?P<type>'
        r'(?:(?:const|static|volatile|inline)\s+)*'
        r'(?:(?:u?int(?:8|16|32|64)_t|bool|double|float|char|auto|size_t)'
        r'|(?:std::\w+(?:<[^>]*>)?)'
        r'|(?:v8::\w+(?:<[^>]*>)?)'
        r'|(?:[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*(?:<[^>]*>)?))'
        r'(?:\s*[*&]+)?)\s+'
        r'(?P<name>[A-Za-z_]\w*)'
        r'\s*(?:[=;{]|$)'
    )

    _IGNORED_NAMES = {
        'return', 'if', 'else', 'for', 'while', 'switch', 'case', 'break',
        'continue', 'true', 'false', 'nullptr', 'new', 'delete', 'this',
        'override', 'final', 'const', 'static', 'inline', 'virtual',
        'explicit', 'noexcept', 'typename', 'template', 'class', 'struct',
        'namespace', 'operator', 'sizeof', 'decltype', 'typedef', 'using',
        'auto', 'void', 'argc', 'argv',
        'i', 'j', 'k', 'n', 'x', 'y', 'z',
    }

    @staticmethod
    def _is_v8_related(line: str, type_str: str = '') -> bool:
        if type_str.lstrip().startswith('v8::'):
            return True
        if 'v8::' in line:
            return True
        return False

    @staticmethod
    def _is_external_ns(line: str, ns_aliases: Set[str] = frozenset()) -> bool:
        if 'v8::' in line or 'std::' in line:
            return True
        for alias in ns_aliases:
            if f'{alias}::' in line:
                return True
        return False

    @staticmethod
    def _is_win32_type(type_str: str) -> bool:
        base = type_str.replace('const', '').replace('*', '').replace('&', '').strip()
        return base in _WIN32_TYPES

    def _is_pointer_type(self, type_str: str) -> bool:
        stripped = _strip_angle_brackets(type_str)
        return '*' in stripped or stripped.strip().endswith('*')

    def _classify_smart_ptr(self, type_str: str):
        t = type_str.replace(' ', '')
        if 'unique_ptr' in t: return 'up_'
        if 'shared_ptr' in t: return 'sp_'
        if 'weak_ptr' in t: return 'wp_'
        return None

    def _check_member_variable(self, filepath: Path, lineno: int,
                                clean: str, ns_aliases: Set[str] = frozenset()):
        if re.search(r'^\s*(struct|class)\s+\w+\s*\{', clean):
            return
        if clean.rstrip().endswith(',') or clean.rstrip().endswith(');'):
            return

        m = self._MEMBER_TYPE_RE.match(clean)
        if not m: return
        name = m.group('name')
        type_str = m.group('type')
        if name in self._IGNORED_NAMES:
            return
        if self._is_win32_type(type_str):
            return

        smart_prefix = self._classify_smart_ptr(type_str)
        if smart_prefix:
            if not name.startswith(smart_prefix) or not _is_snake_case(name[len(smart_prefix):]):
                self.warnings.append(f"{filepath}:{lineno}: Member smart-pointer '{name}' should use prefix '{smart_prefix}' + snake_case")
        elif self._is_pointer_type(type_str):
            if not _check_prefixed(name, 'p_'):
                self.warnings.append(f"{filepath}:{lineno}: Member raw-pointer '{name}' should use 'p_' prefix + snake_case")
        else:
            if not _check_prefixed(name, 'm_'):
                self.warnings.append(f"{filepath}:{lineno}: Member variable '{name}' should use 'm_' prefix + snake_case")

    def _check_local_variable(self, filepath: Path, lineno: int,
                               clean: str, ns_aliases: Set[str] = frozenset()):
        m = self._LOCAL_TYPE_RE.match(clean)
        if not m: return
        name = m.group('name')
        type_str = m.group('type')
        if name in self._IGNORED_NAMES:
            return
        if self._is_win32_type(type_str):
            return

        smart_prefix = self._classify_smart_ptr(type_str)
        if smart_prefix:
            if not name.startswith(smart_prefix) or not _is_snake_case(name[len(smart_prefix):]):
                self.warnings.append(f"{filepath}:{lineno}: Local smart-pointer '{name}' should use prefix '{smart_prefix}' + snake_case")
        elif self._is_pointer_type(type_str):
            if not _check_prefixed(name, 'p_'):
                self.warnings.append(f"{filepath}:{lineno}: Local raw-pointer '{name}' should use 'p_' prefix + snake_case")
        else:
            if not _is_snake_case(name) and not name.startswith('m_') and not name.startswith('p_'):
                self.warnings.append(f"{filepath}:{lineno}: Local variable '{name}' should use snake_case")

    def _check_integer_types(self, filepath: Path, lineno: int, clean: str):
        if re.search(r'(?:int\s+main\s*\(|int\s+argc|extern\s+"C")', clean):
            return
        
        stripped = _strip_angle_brackets(clean)
        forbidden = [
            (r'\bint\b(?!\d|_t)', 'int32_t'),
            (r'\bunsigned int\b', 'uint32_t'),
            (r'\blong long\b', 'int64_t'),
            (r'\bunsigned long long\b', 'uint64_t'),
            (r'\bshort\b(?!\s*int)', 'int16_t'),
        ]
        for pattern, replacement in forbidden:
            if re.search(pattern, stripped):
                self.warnings.append(f"{filepath}:{lineno}: Use '{replacement}' instead of bare C type")
                break

    def check_directory(self, directory: Path) -> bool:
        all_good = True
        for filepath in sorted(directory.rglob('*.cpp')):
            if not self.check_file(filepath): all_good = False
        for filepath in sorted(directory.rglob('*.hpp')):
            if not self.check_file(filepath): all_good = False
        return all_good

    def print_report(self) -> bool:
        print(f"\n{'='*70}\nZane Coding Style Check Report\n{'='*70}")
        print(f"Files checked : {self.files_checked}\nErrors        : {len(self.errors)}\nWarnings      : {len(self.warnings)}\n{'='*70}\n")
        if self.errors:
            print("[ERROR] ERRORS:")
            for e in self.errors: print(f"  {e}")
        if self.warnings:
            print("[WARN] WARNINGS:")
            for w in self.warnings[:50]: print(f"  {w}")
            if len(self.warnings) > 50: print(f"  ... and {len(self.warnings)-50} more")
        if not self.errors and not self.warnings:
            print("All checks passed!")
        return len(self.errors) == 0

def main():
    checker = StyleChecker()
    if len(sys.argv) > 1:
        target = Path(sys.argv[1])
    else:
        target = Path(__file__).parent.parent / 'src'
    
    if target.is_file(): checker.check_file(target)
    else: checker.check_directory(target)
    
    checker.print_report()
    sys.exit(0 if len(checker.errors) == 0 else 1)

if __name__ == '__main__':
    main()
