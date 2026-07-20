# External Dependencies

Zane is built on top of several open-source libraries. This document lists all external dependencies and their respective licenses.

All dependencies listed below permit **standalone binary distribution** — Zane's runtime does not impose additional restrictions beyond what each license allows.

---

## Core Runtime

| Dependency | Repository | License |
|---|---|---|
| **Google V8** | [https://chromium.googlesource.com/v8/v8.git/](https://chromium.googlesource.com/v8/v8.git/) | V8's BSD-style license |
| **brotli** | [https://github.com/google/brotli](https://github.com/google/brotli) | MIT |

## HTTP

| Dependency | Repository | License |
|---|---|---|
| **Zane-HTTPParser** (submodule `libs/http`) | [https://github.com/Zane-JS/Zane-HTTPParser](https://github.com/Zane-JS/Zane-HTTPParser) | MIT |

## Networking Layer — trantor

| Dependency | Repository | License |
|---|---|---|
| **trantor** | [https://github.com/an-tao/trantor](https://github.com/an-tao/trantor) | BSD 3-Clause |

### trantor's dependencies

| Dependency | Repository | License |
|---|---|---|
| **c-ares** | [https://github.com/c-ares/c-ares](https://github.com/c-ares/c-ares) | MIT |
| **spdlog** | [https://github.com/gabime/spdlog](https://github.com/gabime/spdlog) | MIT |
| **Botan** | [https://github.com/randombit/botan](https://github.com/randombit/botan) | BSD-2-Clause |
| **wepoll** | [https://github.com/piscisaureus/wepoll](https://github.com/piscisaureus/wepoll) | Custom license (IOCP-based, Windows only) |

### spdlog's dependency

| Dependency | Repository | License |
|---|---|---|
| **{fmt}** | [https://github.com/fmtlib/fmt](https://github.com/fmtlib/fmt) | MIT |

## Compression

| Dependency | Repository | License |
|---|---|---|
| **zlib** | [https://github.com/madler/zlib](https://github.com/madler/zlib) | Zlib |
| **zstd** | [https://github.com/facebook/zstd](https://github.com/facebook/zstd) | BSD (Zane's choice — permits standalone binary) |

---

## License Compatibility Notes

All dependencies listed above allow Zane to be distributed as a **standalone binary** without requiring source code disclosure. Specifically:

- **MIT, BSD, Zlib** licenses are permissive and impose no restrictions on binary distribution.
- **zstd** offers a dual license (BSD / GPL); Zane uses it under the **BSD** terms.
- **wepoll** uses a custom permissive license that permits redistribution in binary form.

If you have any questions about licensing, please open an issue on the [Zane repository](https://github.com/Zane-JS/Zane).
