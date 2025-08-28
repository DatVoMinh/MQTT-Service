### 1) Tạo service file
```ini
[Unit]
Description=MQTT Monitor Service
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=dat
Group=dat
WorkingDirectory=/home/dat/denso_ws/FAKER
ExecStart=/home/dat/denso_ws/FAKER/run_mqtt_monitor.sh
Restart=always
RestartSec=5
# Đảm bảo PATH có python/pip
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# Thời gian chờ khi dừng
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
```

### 2) Cài đặt và chạy
```bash
# 1) Đảm bảo script chạy được
sudo chmod +x /home/dat/denso_ws/FAKER/run_mqtt_monitor.sh

# 2) Tạo unit file
sudo tee /etc/systemd/system/mqtt-monitor.service > /dev/null <<'EOF'
[Unit]
Description=MQTT Monitor Service
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=dat
Group=dat
WorkingDirectory=/home/dat/denso_ws/FAKER
ExecStart=/home/dat/denso_ws/FAKER/run_mqtt_monitor.sh
Restart=always
RestartSec=5
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
EOF

# 3) Reload, enable, start
sudo systemctl daemon-reload
sudo systemctl enable mqtt-monitor.service
sudo systemctl start mqtt-monitor.service
```

### 3) Lệnh quản trị
```bash
# Kiểm tra trạng thái
systemctl status mqtt-monitor.service

# Xem log (theo dõi realtime)
journalctl -u mqtt-monitor.service -f

# Xem log gần đây
journalctl -u mqtt-monitor.service --since "1 hour ago"

# Dừng / chạy / khởi động lại
sudo systemctl stop mqtt-monitor.service
sudo systemctl start mqtt-monitor.service
sudo systemctl restart mqtt-monitor.service

# Tắt auto-start lúc boot
sudo systemctl disable mqtt-monitor.service

# Gỡ service (nếu cần)
sudo systemctl stop mqtt-monitor.service
sudo systemctl disable mqtt-monitor.service
sudo rm -f /etc/systemd/system/mqtt-monitor.service
sudo systemctl daemon-reload
```

- Nếu user/group không phải `dat`, sửa các dòng `User=` và `Group=` tương ứng.
- Service tự động restart nếu tiến trình chết; log xem qua `journalctl`.

