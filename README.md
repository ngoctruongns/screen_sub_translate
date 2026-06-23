### 🎬 ScreenSubTranslator (SST)

**ScreenSubTranslator** là một ứng dụng máy tính mã nguồn mở, hiệu năng cao được phát triển bằng **C++** và **Qt6**. Phần mềm hoạt động như một công cụ hỗ trợ thời gian thực (Real-time Overlay), tự động quét phụ đề tiếng Trung trên màn hình thông qua các thuật toán xử lý ảnh của **OpenCV**, nhận diện ký tự bằng **Tesseract OCR** và dịch sang tiếng Việt để người dùng có thể xem phim trực tiếp trên các nền tảng Trung Quốc một cách chủ động.

* * *

### ✨ Tính năng nổi bật

*   **⚡ Siêu tốc & Độ trễ thấp:** Lõi C++ xử lý đa luồng (Multi-threading), tối ưu hóa tốc độ chụp màn hình và nhận diện ở mức mili-giây.
*   **👁️ Cửa sổ Overlay thông minh:** Giao diện dạng khung viền trong suốt, luôn nằm trên cùng (`Stays on Top`), cho phép hiển thị Vietsub đè trực tiếp lên trình phát video.
*   **🖼️ Tiền xử lý ảnh nâng cao:** Tích hợp OpenCV để nhị phân hóa (Binary Thresholding) và khử nhiễu vùng chữ, giúp tăng tỷ lệ chính xác của bộ OCR lên tối đa.
*   **📉 Tối ưu hóa hiệu năng (Hash Matching):** Sử dụng thuật toán so sánh sự khác biệt giữa các khung hình (`cv::absdiff`). Hệ thống chỉ chạy OCR và gọi API dịch khi phụ đề thực sự thay đổi, giảm tải 90% CPU và tài nguyên mạng.
*   **🌐 Dịch thuật bất đồng bộ:** Gọi API dịch thuật ngầm (`QNetworkAccessManager`), đảm bảo trải nghiệm xem phim mượt mà, hoàn toàn không gây đứng hay khựng giao diện.

* * *

### 🛠️ Yêu cầu hệ thống & Kiến trúc

*   **Hệ điều hành:** Ubuntu 22.04 LTS (Khuyến khích chạy trên môi trường X11/Xorg).
*   **Ngôn ngữ lập trình:** C++17 trở lên.
*   **Framework & Thư viện:**
    *   Qt6 (Widgets, Network)
    *   OpenCV 4.x
    *   Tesseract OCR (Yêu cầu gói ngôn ngữ giản thể `chi_sim`)

* * *

### 🚀 Hướng dẫn cài đặt và Build dự án

### 1\. Cài đặt các thư viện phụ thuộc

Mở Terminal trên Ubuntu 22.04 và chạy lệnh sau để cài đặt môi trường:

bash

    sudo apt update
    sudo apt install build-essential cmake git -y
    sudo apt install qt6-base-dev qt6-base-private-dev -y
    sudo apt install libopencv-dev -y
    sudo apt install libtesseract-dev tesseract-ocr-chi-sim -y
    

### 2\. Tải mã nguồn và Build bằng CMake

bash

    # Di chuyển vào thư mục dự án
    cd ScreenSubTranslator
    
    # Tạo và di chuyển vào thư mục build
    mkdir build && cd build
    
    # Cấu hình dự án bằng CMake
    cmake ..
    
    # Biên dịch mã nguồn với tất cả lõi CPU khả dụng
    make -j$(nproc)
    

* * *

### 📂 Cấu trúc thư mục dự án

text

    ScreenSubTranslator/
    │
    ├── CMakeLists.txt              # File cấu hình build dự án
    ├── README.md                   # Tài liệu hướng dẫn này
    ├── main.cpp                    # Khởi chạy ứng dụng Qt
    │
    └── src/
        ├── overlay_window.h        # Giao diện hiển thị Vietsub trong suốt
        ├── overlay_window.cpp
        ├── capture_worker.h        # Luồng nền chụp màn hình & xử lý OpenCV
        ├── capture_worker.cpp
        ├── ocr_engine.h            # Bộ xử lý nhận diện chữ Tesseract (chi_sim)
        ├── ocr_engine.cpp
        ├── translate_client.h      # Trình gọi API dịch thuật bất đồng bộ
        └── translate_client.cpp
    

* * *

### 📖 Hướng dẫn sử dụng

1.  Khởi chạy ứng dụng từ thư mục `build`: `./ScreenSubTranslator`.
2.  Một cửa sổ có viền đỏ trong suốt sẽ xuất hiện trên màn hình của bạn.
3.  Di chuyển và co giãn cửa sổ viền đỏ này sao cho nó bao trọn **vùng hiển thị phụ đề tiếng Trung** trên trình duyệt web (Youtube, Bilibili, Tencent Video, iQIYI...).
4.  Bật phim và thưởng thức. Tiếng Việt dịch từ phụ đề gốc sẽ tự động hiển thị mượt mà ngay tại trung tâm khung hình.

* * *

### 🎯 Lộ trình phát triển tương lai (Roadmap)

*   Tích hợp tính năng **Text-to-Speech (TTS)** bằng `Edge-TTS` để tự động đọc lời thoại tiếng Việt.
*   Hỗ trợ kéo thả thay đổi kích thước vùng quét (Resizable Box) trực tiếp bằng chuột thay vì fix cứng kích thước cửa sổ.
*   Thử nghiệm nâng cấp bộ não OCR lên **PaddleOCR C++** hoặc **ONNX Runtime** để nhận diện được các loại font chữ phim nghệ thuật phức tạp.
*   Đóng gói ứng dụng thành một thiết bị phần cứng Edge AI độc lập (sử dụng Jetson Nano hoặc Raspberry Pi 5 kết hợp camera ngoài).
