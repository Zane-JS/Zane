# Zane Coding Guidelines

This document specifies the C++ coding standards for the Zane project to ensure consistency, performance, and maintainability.

## 1. Code Formatting

The Zane project uses the traditional **K&R (Kernighan & Ritchie)** style.

### Basic Rules:

- **Opening Braces `{`**: Placed on the same line as the control statement (`if`, `while`, `for`, `switch`) or function declaration.
- **Closing Braces `}`**: Placed on a separate line, indented to match the corresponding control statement.
- **Indentation**: Use **4 spaces**. The use of Tab characters for indentation is **STRICTLY PROHIBITED** to ensure consistent display across all editors.
- **`else` Keyword**: Placed on the same line as the closing brace of the preceding `if` block.

### Example:

```cpp
void ExampleFunction(int32_t value) {
    if (value > 0) {
        // Logic here
        DoSomething();
    } else {
        // Other logic
        DoOtherThing();
    }

    for (int32_t i = 0; i < 10; i++) {
        printf("%d\n", i);
    }
}
```

---

## 2. Integer Types

To ensure consistency across platforms (Windows/Linux/macOS) and prevent unexpected overflow issues, Zane **always** uses fixed-width integer types from the `<cstdint>` library.

### Rules:

- **Never use** basic C++ integer types like `int`, `long`, `short`, or `unsigned long`.
- Always include the `#include <cstdint>` header.
- Use the following corresponding types:

| Old Type             | New Standard Type (`stdint`) | Purpose                                         |
| :------------------- | :--------------------------- | :---------------------------------------------- |
| `int`                | `int32_t`                    | 32-bit signed integer                           |
| `unsigned int`       | `uint32_t`                   | 32-bit unsigned integer                         |
| `long long`          | `int64_t`                    | 64-bit signed integer (for file sizes, offsets) |
| `unsigned long long` | `uint64_t`                   | 64-bit unsigned integer                         |
| `short`              | `int16_t`                    | 16-bit signed integer                           |
| `unsigned char`      | `uint8_t`                    | Byte / Binary data                              |
| `NULL`               | `nullptr`                    | Null pointer (C++11)                            |

### Additional Safety Rules:

- **Always use `nullptr`**: Never use `NULL` or `0` for pointers.
- **Minimize `void*`**: Avoid using `void*` unless absolutely necessary for interfacing with low-level C libraries. Prefer `std::variant`, `templates`, or polymorphism.
- **Minimize `char*` and `char[]`**: Prefer `std::string` for text and `std::vector<uint8_t>` or `std::array` for data buffers. This helps prevent buffer overflow vulnerabilities.

---

## 3. Naming Conventions

Zane applies a naming system that combines Modern C++ style with safety standards from NASA and Google to optimize code readability.

| Entity                     | Rule                  | Example                        |
| :------------------------- | :-------------------- | :----------------------------- |
| **Class / Struct**         | `PascalCase`          | `FileStream`, `TaskRunner`     |
| **Function / Method**      | `camelCase`           | `openFile()`, `readFileSync()` |
| **Namespace**              | `snake_case`          | `zane::module_fs`                |
| **Local Variable**         | `snake_case`          | `buffer_size`, `bytes_read`    |
| **Member Variable**        | `m_snake_case`        | `m_file_handle`, `m_is_open`   |
| **Pointer Variable (Raw)** | `p_snake_case`        | `p_isolate`, `p_context`       |
| **Unique Pointer**         | `up_snake_case`       | `up_task`, `up_buffer`         |
| **Shared Pointer**         | `sp_snake_case`       | `sp_config`, `sp_node`         |
| **Weak Pointer**           | `wp_snake_case`       | `wp_parent`                    |
| **Constant / Macro**       | `SNAKE_CASE_ALL_CAPS` | `MAX_BUFFER_SIZE`, `O_RDONLY`  |

### Example:

```cpp
namespace zane::module_fs {

class FileHandler {
private:
    int32_t m_file_descriptor; // Member variable
    uint8_t* p_data_buffer;    // Member pointer variable

public:
    void processData(int32_t length) { // camelCase method
        int32_t local_count = 0;       // snake_case local variable
        uint8_t* p_local_ptr = nullptr; // Local pointer variable
        auto up_task = std::make_unique<Task>(); // Smart pointer
        auto up_buf = std::make_unique<uint8_t[]>(length); // Unique pointer

        // ... logic
    }
};

}
```

---

## 4. Third-party Libraries

To ensure Peak Performance and complete Control over Memory Management, Zane follows a rule of strictly limiting dependencies on external libraries.

### Rules:

- **Prioritize Self-implementation**: For common features, prefer using the modern `Standard Template Library (STL)` or self-implementing optimized data structures for Zane.
- **Why?**: External libraries often come with redundant features (bloatware), increasing the executable size and potentially containing memory management mechanisms incompatible with V8.
- **Exceptions**:
  - Libraries implementing extremely complex algorithms or industry standards (e.g., `zlib` for compression, `OpenSSL` for security).
  - Mandatory embedded libraries (such as `V8` itself).

---

## 5. Namespace and Aliases

To avoid name collisions and keep the source code scope explicit.

### Rules:

- **PROHIBITED use of `using namespace ...;`**: Never write `using namespace std;` or any other namespace in the global scope or header files. This helps avoid namespace pollution.
- **Allowed `using name = type;`**: Encouraged use of the `using` syntax (C++11) instead of `typedef` for creating aliases for complex types. This makes the code more concise and understandable.

### Example:

```cpp
// WRONG
using namespace std;

// CORRECT
using Buffer = std::vector<uint8_t>;
using ResolverMap = std::map<int32_t, v8::Global<v8::Promise::Resolver>>;

void process() {
    Buffer internal_buf; // Explicit and clean
}
```

---

## 6. Performance Rules

To ensure Zane remains a high-performance JavaScript engine.

### Rules:

- **Overhead Limit**: Every bug fix or new feature **must not** increase performance overhead by more than **1%** compared to the previous version.
- **Mandatory Benchmarking**: Comparative benchmarks (e.g., using `hyperfine` or `bench_io.js`) must be run before merging code.

---

## 7. Rationale

1.  **Consistency**: The source code looks clean and is easy to read when all team members follow the same style.
2.  **Reliability**: Knowing exactly how many bits a variable occupies eliminates "undefined behavior" when compiling across different operating systems.
3.  **Performance**: Helps the compiler optimize better when data limits are clearly defined.
4.  **Safety**: Minimizes memory errors and security vulnerabilities by leveraging Modern C++ safety features.
