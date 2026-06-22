# Zane Development Tools

This directory contains development tools for the Zane project.

## 📋 Available Tools

### 1. **check_style.py** - Coding Style Checker

Automatically validates C++ code against Zane coding standards.

**Usage:**

```bash
# Check all files in src/
python tools/check_style.py

# Check specific file
python tools/check_style.py src/module/console.cpp

# Check specific directory
python tools/check_style.py src/module
```

**What it checks:**

- ✅ Methods use camelCase (not PascalCase)
- ✅ Member variables use `m_` prefix
- ✅ Pointer parameters use `p_` prefix
- ✅ Use `int32_t` instead of `int`
- ✅ Static variables use `s_` prefix

**Exit codes:**

- `0` - All checks passed
- `1` - Style violations found

---

### 2. **pre-commit.py** - Pre-commit Hook

Automatically checks coding style before allowing commits.

**Installation:**

```bash
python tools/install_hooks.py
```

**Features:**

- 🔍 Automatically runs on `git commit`
- 📝 Only checks staged C++ files
- ⚡ Fast - only validates changed files
- 🚫 Blocks commits with style violations

**Skip the check (when needed):**

```bash
git commit --no-verify
```

---

### 3. **install_hooks.py** - Hook Installer

Installs Git hooks for the project.

**Usage:**

```bash
python tools/install_hooks.py
```

---

### 4. **extract_shims.py** - Temporal Shims Extractor

Extracts Temporal API shims from V8 library.

---

## 🚀 Quick Start

### First Time Setup

1. **Install Git hooks:**

   ```bash
   python tools/install_hooks.py
   ```

2. **Check your code:**

   ```bash
   python tools/check_style.py
   ```

3. **Fix any violations** reported by the checker

4. **Commit your changes:**
   ```bash
   git add .
   git commit -m "Your message"
   ```
   The pre-commit hook will automatically validate your code!

---

## 📖 Coding Standards

For detailed coding standards, see:

- [CODING_STYLE_en.md](../docs/CODING_STYLE_en.md) - English
- [CODING_STYLE_vi.md](../docs/CODING_STYLE_vi.md) - Tiếng Việt

### Quick Reference

**Naming Conventions:**

- Methods: `camelCase` (e.g., `readFile`, `setTimeout`)
- Member variables: `m_` prefix (e.g., `m_count`, `m_data`)
- Pointer parameters: `p_` prefix (e.g., `p_isolate`, `p_context`)
- Static variables: `s_` prefix (e.g., `s_instance`)
- Shared pointers: `sp_` prefix (e.g., `sp_task`)
- Unique pointers: `up_` prefix (e.g., `up_timer`)

**Type System:**

- Use `int32_t` instead of `int`
- Use `uint32_t` instead of `unsigned int`
- Be explicit with integer sizes

---

## 🔧 Continuous Integration

The style checker can be integrated into CI/CD pipelines:

```yaml
# Example GitHub Actions workflow
- name: Check Coding Style
  run: python tools/check_style.py
```

---

## 💡 Tips

1. **Run the checker frequently** during development
2. **Install the pre-commit hook** to catch issues early
3. **Use `--no-verify` sparingly** - only when you have a good reason
4. **Keep the coding standards document** open while coding

---

## 🐛 Troubleshooting

**Hook not running?**

- Make sure you installed it: `python tools/install_hooks.py`
- Check `.git/hooks/pre-commit` exists

**Too many warnings?**

- Focus on errors first (marked with ❌)
- Warnings (⚠️) are suggestions, not requirements

**False positives?**

- The checker uses heuristics and may have false positives
- Report issues or improve the checker script

---

## 📝 Contributing

To improve these tools:

1. Edit the tool script
2. Test thoroughly
3. Update this README if needed
4. Submit a pull request

---

**Last Updated:** 2026-01-29  
**Maintainer:** Zane Team
