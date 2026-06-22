# Quy tắc viết mã nguồn (Coding Guidelines) cho Zane

Tài liệu này quy định các tiêu chuẩn viết mã nguồn C++ trong dự án Zane để đảm bảo tính đồng nhất, hiệu suất và khả năng bảo trì cao.

## 1. Định dạng mã nguồn (Code Formatting)

Dự án Zane sử dụng phong cách **K&R (Kernighan & Ritchie)** truyền thống.

### Quy tắc cơ bản:

- **Dấu ngoặc nhọn `{`**: Đặt trên cùng một dòng với câu lệnh điều khiển (`if`, `while`, `for`, `switch`) hoặc khai báo hàm.
- **Dấu ngoặc nhọn đóng `}`**: Đặt trên một dòng riêng biệt, thụt đầu dòng khớp với câu lệnh điều khiển tương ứng.
- **Thụt đầu dòng (Indentation)**: Sử dụng **4 khoảng trắng (spaces)**. Tuyệt đối **CẤM/KHÔNG SỬ DỤNG** ký tự Tab để thụt đầu dòng nhằm đảm bảo mã nguồn hiển thị nhất quán trên mọi trình soạn thảo.
- **Từ khóa `else`**: Đặt trên cùng một dòng với dấu ngoặc nhọn đóng của khối `if` trước đó.

### Ví dụ:

```cpp
void ExampleFunction(int32_t value) {
    if (value > 0) {
        // Logic ở đây
        DoSomething();
    } else {
        // Logic khác
        DoOtherThing();
    }

    for (int32_t i = 0; i < 10; i++) {
        printf("%d\n", i);
    }
}
```

---

## 2. Kiểu dữ liệu số (Integer Types)

Để đảm bảo tính nhất quán trên các nền tảng (Windows/Linux/macOS) và tránh các lỗi tràn số không mong muốn, dự án Zane **luôn luôn** sử dụng các kiểu số xác định kích thước từ thư viện `<cstdint>`.

### Quy tắc:

- Tuyệt đối **không sử dụng** các kiểu số cơ bản của C++ như `int`, `long`, `short`, `unsigned long`.
- Luôn bao gồm thư viện `#include <cstdint>`.
- Sử dụng các kiểu tương ứng sau:

| Kiểu cũ              | Kiểu chuẩn mới (`stdint`) | Mục đích                                              |
| :------------------- | :------------------------ | :---------------------------------------------------- |
| `int`                | `int32_t`                 | Số nguyên 32-bit có dấu                               |
| `unsigned int`       | `uint32_t`                | Số nguyên 32-bit không dấu                            |
| `long long`          | `int64_t`                 | Số nguyên 64-bit có dấu (cho kích thước file, offset) |
| `unsigned long long` | `uint64_t`                | Số nguyên 64-bit không dấu                            |
| `short`              | `int16_t`                 | Số nguyên 16-bit                                      |
| `unsigned char`      | `uint8_t`                 | Dữ liệu byte / nhị phân                               |
| `NULL`               | `nullptr`                 | Con trỏ rỗng (C++11)                                  |

### Quy tắc bổ sung về an toàn con trỏ và mảng:

- **Luôn sử dụng `nullptr`**: Tuyệt đối không sử dụng `NULL` hoặc `0` cho con trỏ.
- **Hạn chế tối đa `void*`**: Tránh sử dụng `void*` trừ khi bắt buộc phải giao tiếp với các thư viện C cấp thấp. Ưu tiên sử dụng `std::variant`, `templates` hoặc tính đa hình.
- **Hạn chế `char*` và `char[]`**: Ưu tiên sử dụng `std::string` cho chuỗi văn bản và `std::vector<uint8_t>` hoặc `std::array` cho đệm dữ liệu. Điều này giúp tránh các lỗi tràn bộ đệm (buffer overflow).

### Ví dụ:

```cpp
#include <cstdint>

class FileProcessor {
public:
    // Sử dụng int32_t cho File Descriptor
    int32_t open_file(const char* path);

    // Sử dụng int64_t cho kích thước file lớn
    int64_t get_file_size(int32_t fd);

    // Sử dụng uint8_t cho buffer dữ liệu
    void process_data(uint8_t* buffer, uint32_t length);
};
```

---

## 3. Quy tắc đặt tên (Naming Conventions)

Zane áp dụng hệ thống đặt tên kết hợp giữa phong cách Modern C++ và các tiêu chuẩn an toàn từ NASA/Google để tối ưu hóa khả năng đọc mã nguồn.

| Đối tượng                      | Quy tắc               | Ví dụ                          |
| :----------------------------- | :-------------------- | :----------------------------- |
| **Class / Struct**             | `PascalCase`          | `FileStream`, `TaskRunner`     |
| **Hàm / Phương thức**          | `camelCase`           | `openFile()`, `readFileSync()` |
| **Namespace**                  | `snake_case`          | `zane::module_fs`                |
| **Biến cục bộ**                | `snake_case`          | `buffer_size`, `bytes_read`    |
| **Biến thành viên (Member)**   | `m_snake_case`        | `m_file_handle`, `m_is_open`   |
| **Biến con trỏ (Raw Pointer)** | `p_snake_case`        | `p_isolate`, `p_context`       |
| **Unique Pointer**             | `up_snake_case`       | `up_task`, `up_buffer`         |
| **Shared Pointer**             | `sp_snake_case`       | `sp_config`, `sp_node`         |
| **Weak Pointer**               | `wp_snake_case`       | `wp_parent`                    |
| **Hằng số / Macro**            | `SNAKE_CASE_ALL_CAPS` | `MAX_BUFFER_SIZE`, `O_RDONLY`  |

### Ví dụ minh họa:

```cpp
namespace zane::module_fs {

class FileHandler {
private:
    int32_t m_file_descriptor; // Biến thành viên
    uint8_t* p_data_buffer;    // Biến con trỏ thành viên

public:
    void processData(int32_t length) { // Phương thức camelCase
        int32_t local_count = 0;       // Biến cục bộ snake_case
        uint8_t* p_local_ptr = nullptr; // Biến con trỏ cục bộ
        auto up_task = std::make_unique<Task>(); // Smart pointer

        // ... logic
    }
};

}
```

---

## 4. Hạn chế sử dụng thư viện ngoài (Third-party Libraries)

Để đảm bảo hiệu suất tối đa (Peak Performance) và kiểm soát hoàn toàn bộ nhớ (Memory Management), Zane tuân thủ quy tắc hạn chế tối đa việc phụ thuộc vào các thư viện bên ngoài.

### Quy tắc:

- **Ưu tiên tự triển khai**: Đối với các tính năng thông dụng, hãy ưu tiên sử dụng `Standard Template Library (STL)` hiện đại hoặc tự triển khai các cấu trúc dữ liệu tối ưu cho Zane.
- **Tại sao?**: Các thư viện ngoài thường đi kèm với nhiều tính năng dư thừa (bloatware), làm tăng kích thước file thực thi và có thể chứa các cơ chế quản lý bộ nhớ không tương thích với V8.
- **Trường hợp ngoại lệ**:
  - Các thư viện thực hiện thuật toán cực kỳ phức tạp hoặc mang tính tiêu chuẩn ngành (ví dụ: `zlib` cho nén/giải nén, `OpenSSL` cho bảo mật).
  - Các thư viện nhúng bắt buộc (như chính `V8`).

### Lưu ý:

Mọi ý định tích hợp thêm bất kỳ thư viện ngoài nào vào mã nguồn phải được thảo luận và thẩm định kỹ lưỡng về mặt hiệu suất.

---

## 5. Sử dụng Namespace và Alias

Để tránh xung đột tên (name collision) và giữ cho phạm vi mã nguồn luôn tường minh.

### Quy tắc:

- **CẤM sử dụng `using namespace ...;`**: Tuyệt đối không viết `using namespace std;` hay bất kỳ namespace nào khác ở phạm vi toàn cục hoặc trong file header. Điều này giúp tránh làm ô nhiễm không gian tên (namespace pollution).
- **Cho phép `using name = type;`**: Khuyến khích sử dụng cú pháp `using` (C++11) thay cho `typedef` để tạo bí danh cho các kiểu dữ liệu phức tạp. Điều này giúp mã nguồn ngắn gọn và dễ hiểu hơn.

### Ví dụ:

```cpp
// SAI
using namespace std;

// ĐÚNG
using Buffer = std::vector<uint8_t>;
using ResolverMap = std::map<int32_t, v8::Global<v8::Promise::Resolver>>;

void process() {
    Buffer internal_buf; // Tường minh và gọn gàng
}
```

---

## 6. Quy tắc Hiệu năng (Performance Rules)

Để đảm bảo Zane luôn giữ vững vị thế là một engine JavaScript hiệu năng cao.

### Quy tắc:

- **Giới hạn Overhead**: Mọi bản sửa lỗi hoặc tính năng mới **không được phép** làm giảm hiệu năng quá **1%** so với phiên bản trước đó.
- **Kiểm chuẩn bắt buộc**: Phải chạy benchmark so sánh (ví dụ: dùng `hyperfine` hoặc `bench_io.js`) trước khi merge code.

---

## 7. Tại sao chúng ta sử dụng các quy tắc này?

1.  **Tính nhất quán**: Mã nguồn trông sạch sẽ và dễ đọc khi tất cả các thành viên trong nhóm tuân thủ cùng một phong cách.
2.  **Độ tin cậy**: Việc biết chính xác một biến chiếm bao nhiêu bit giúp loại bỏ hoàn toàn các lỗi "undefined behavior" khi biên dịch mã nguồn trên các hệ điều hành khác nhau (ví dụ: `long` trên Windows là 32-bit nhưng trên Linux x64 lại là 64-bit).
3.  **Hiệu suất**: Giúp trình biên dịch tối ưu hóa tốt hơn khi biết rõ giới hạn của dữ liệu.
4.  **An toàn (Safety)**: Giảm thiểu rủi ro lỗi bộ nhớ và lỗ hổng bảo mật bằng các kiểu dữ liệu an toàn của Modern C++.
