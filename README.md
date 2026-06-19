# 🐘 STM32 Multi-Device Dashboard (PostgreSQL Version)

Hệ thống đã được nâng cấp từ SQLite lên PostgreSQL để tăng hiệu năng và khả năng mở rộng.

## 🚀 Cách khởi động nhanh

### 1. Khởi động Database (Docker)
Nếu bạn có Docker, hãy chạy lệnh sau để bật Postgres:
```bash
docker-compose up -d
```

### 2. Cài đặt thư viện
```bash
npm install
```

### 3. Chạy Server
```bash
npm start
```

## ⚙️ Cấu hình (File .env)
Bạn có thể điều chỉnh thông số kết nối trong file `.env`:
- `PGHOST`: Địa chỉ Database (mặc định localhost)
- `PGUSER`: iov_user
- `PGPASSWORD`: iot_password
- `PGDATABASE`: iot_db

## 📊 Tính năng mới
- **Concurrency**: Hỗ trợ ghi dữ liệu từ hàng trăm thiết bị cùng lúc.
- **Data Integrity**: Kiểu dữ liệu `TIMESTAMPTZ` đảm bảo thời gian chính xác theo múi giờ.
- **Scalability**: Sẵn sàng tích hợp các công cụ phân tích dữ liệu lớn.

---
*Lưu ý: Nếu bạn muốn chuyển dữ liệu từ `sensors.db` cũ sang Postgres, hãy cho tôi biết để tôi viết script migrate.*
