import { mkdir, writeFile, unlink, rm } from 'node:fs/promises';
import { join } from 'node:path';

async function runTest(fileCount) {
    const testDir = './test_folder';
    const content = "Test dữ liệu"; // Chuỗi string đơn giản

    try {
        // 1. Tạo thư mục
        await mkdir(testDir, { recursive: true });
        console.log(`>>> Dang test với ${fileCount} files...`);

        const start = Date.now();

        // 2. Tạo file (Dùng vòng lặp chờ đợi để tránh crash nếu Zane chưa xử lý tốt Promise.all)
        for (let i = 0; i < fileCount; i++) {
            await writeFile(join(testDir, `f_${i}.txt`), content);
        }
        
        const mid = Date.now();
        console.log(`- Tao file: ${mid - start}ms`);

        // 3. Xóa file
        for (let i = 0; i < fileCount; i++) {
            await unlink(join(testDir, `f_${i}.txt`));
        }

        // 4. Xóa thư mục
        await rm(testDir, { recursive: true, force: true });
        
        const end = Date.now();
        console.log(`- Xoa file: ${end - mid}ms`);
        console.log(`>>> Tong cong: ${end - start}ms`);

    } catch (err) {
        console.log("Loi: ", err.message);
    }
}

runTest(1000); // Thử với 500 file trước để xem độ ổn định của Zane