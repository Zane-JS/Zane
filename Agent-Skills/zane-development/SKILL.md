---
name: zane-development
description: Guidelines and instructions for developing the Zane runtime. Includes coding standards, build processes, and testing workflows.
license: Apache-2.0
metadata:
  author: Zane V8 Authors
  version: "1.0.0"
---

# Zane Development Skill

This skill provides instructions for maintaining and extending the Zane JavaScript runtime.

## Coding Standards

All C++ code in the Zane project should adhere to the following standards:

- **Methods**: Use `camelCase` (e.g., `readFile`, `setTimeout`).
- **Member Variables**: Prefix with `m_` (e.g., `m_count`, `m_isRunning`).
- **Pointers**: Prefix with `p_` (e.g., `p_isolate`, `p_context`).
- **Static Variables**: Prefix with `s_` (e.g., `s_instance`).
- **Integer Types**: Use `int32_t` instead of `int`.
- **Reference Docs**: Agent **MUST** read and follow `docs/CODING_STYLE_en.md`, `docs/OPTIMIZATION.md`, và `docs/TOOLS_GUIDE_vi.md` to understand the project deeply.

## Build Process

The project is built using a PowerShell script:

- Run `.\build.ps1` from the `Zane-app` directory.
- For a clean build, you may need to remove `.obj` files.

## Testing & Validation

After implementing new features or making changes, you **MUST** re-test to ensure stability and performance.

Before committing changes, ensure the following checks pass:

- **Style Check**: Run `python tools/check_style.py`.
- **Benchmark**: Run `.\zane.exe ..\TEST\benchmark_fs.js` to verify performance consistency.
- **FS Validation**: Run `.\zane.exe ..\TEST\validate_fs.js` for deeper I/O checks.
- **Git Hooks**: Ensure `python tools/install_hooks.py` has been run to enable pre-commit checks.

## Repository Structure

- `src/`: Core C++ source files.
- `tools/`: Development and build support tools.
- `docs/`: Documentation and coding style guides.
- `Agent-Skills/`: AI agent capability definitions (this directory).
