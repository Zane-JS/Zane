import os
import sys
import urllib.request
import zipfile
import tarfile
import shutil
import time

# Thư mục chứa các thư viện phụ thuộc
DEPS_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "deps")

# Danh sách các thư viện cần tải cho Zane
DEPENDENCIES = {
    "zlib": {
        "url": "https://github.com/madler/zlib/releases/download/v1.3.2/zlib132.zip",
        "folder": "zlib",
        "is_source": True
    },
    "trantor": {
        "url": "https://github.com/an-tao/trantor/archive/refs/tags/v1.5.26.zip",
        "folder": "trantor",
        "is_source": True
    },
    "brotli": {
        "url": "https://github.com/google/brotli/archive/refs/tags/v1.2.0.zip",
        "folder": "brotli",
        "is_source": True
    },
    "zstd": {
        "url": "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz",
        "folder": "zstd",
        "is_source": True
    }
}

def download_file(url, target_path):
    print(f"📥 Đang tải: {url}...")
    try:
        urllib.request.urlretrieve(url, target_path)
        print(f"✅ Đã tải xong: {target_path}")
    except Exception as e:
        print(f"❌ Lỗi khi tải: {e}")
        return False
    return True

def extract_file(file_path, extract_to):
    print(f"📦 Đang giải nén: {file_path}...")
    try:
        if file_path.endswith('.zip'):
            with zipfile.ZipFile(file_path, 'r') as zip_ref:
                zip_ref.extractall(extract_to)
        elif file_path.endswith(('.tar.gz', '.tgz')):
            with tarfile.open(file_path, 'r:gz') as tar_ref:
                tar_ref.extractall(extract_to)
        else:
            print(f"❌ Định dạng file không hỗ trợ: {file_path}")
            return False
        print(f"✅ Giải nén xong vào: {extract_to}")
    except Exception as e:
        print(f"❌ Lỗi giải nén: {e}")
        return False
    return True

def setup_dependencies():
    if not os.path.exists(DEPS_DIR):
        os.makedirs(DEPS_DIR)
        print(f"📂 Đã tạo thư mục deps tại: {DEPS_DIR}")

    for name, info in DEPENDENCIES.items():
        target_folder = os.path.join(DEPS_DIR, info["folder"])
        
        # Tạo thư mục cha nếu cần
        parent_dir = os.path.dirname(target_folder)
        if parent_dir and not os.path.exists(parent_dir):
            os.makedirs(parent_dir)
        
        # Nếu thư mục đã tồn tại, bỏ qua
        if os.path.exists(target_folder):
            print(f"⏭️  Thư viện {name} đã tồn tại, bỏ qua.")
            continue

        ext = ".tar.gz" if info["url"].endswith(".tar.gz") else ".zip"
        temp_file = os.path.join(DEPS_DIR, f"{name}{ext}")
        
        if download_file(info["url"], temp_file):
            if extract_file(temp_file, DEPS_DIR):
                # Xử lý trường hợp archive chứa thư mục con lồng nhau
                if info.get("is_source"):
                    base_folder = os.path.basename(info["folder"])
                    
                    extracted_dirs = [d for d in os.listdir(DEPS_DIR) 
                                    if os.path.isdir(os.path.join(DEPS_DIR, d)) 
                                    and d.lower().startswith(base_folder.lower())
                                    and d.lower() != base_folder.lower()
                                    and not d.startswith('.')]
                    
                    if extracted_dirs:
                        source_dir = os.path.join(DEPS_DIR, extracted_dirs[0])
                        print(f"🚚 Di chuyển {extracted_dirs[0]} -> {info['folder']}...")
                        if os.path.exists(target_folder):
                            shutil.rmtree(target_folder)
                        
                        if os.name == 'nt':
                            time.sleep(0.5)
                        shutil.move(source_dir, target_folder)
                
                if os.path.exists(temp_file):
                    os.remove(temp_file)
                print(f"🎉 Hoàn tất thiết lập {name}!")

if __name__ == "__main__":
    print("🚀 Bắt đầu thiết lập các thư viện phụ thuộc cho Zane...")
    setup_dependencies()
    print("\n✨ Tất cả thư viện đã sẵn sàng trong thư mục /deps")
