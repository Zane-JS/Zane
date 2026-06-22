# Zane


**Zane** is a high-performance, lightweight JavaScript runtime built on top of Google's V8 engine. Written in pure C++ for maximum stability and performance on Windows.

## ✨ Features

- **Pure C++17/20**: Zero overhead, built with the latest MSVC toolchain.
- **Monolithic Build**: Easy to distribute, no complex DLL dependencies.
- **Zane Context**: Optimized default environment with essential APIs like `console.log`.
- **Temporal Enabled**: Ready for next-generation JavaScript date and time handling.

## 🏗 Project Structure

- `src/`: Core C++ source code.
- `include/`: V8 and project headers.
- `libs/`: Pre-built V8 monolithic libraries.
- `tools/`: Utility scripts for build maintenance.

## 🛠 Prerequisites

- **Visual Studio 2022/2026** with "Desktop development with C++" workload.
- **Python 3.x** (for shim extraction).
- **V8 Artifacts**: Get them from [v8-zane](https://github.com/Zane-JS/v8-zane).

## 🚀 Building Zane

1. Open **Developer PowerShell for Visual Studio**.
2. Run the build script:
   ```powershell
   .\build.ps1
   ```
3. The resulting `zane.exe` will be created in the root directory.

## 🏁 Running

Make sure `icudtl.dat` is in the same directory as `zane.exe`.

```powershell
.\zane.exe
```

## 📜 Example

```bash
zane main.js
```

---

**Zane: V8 - A Gift from God.**


