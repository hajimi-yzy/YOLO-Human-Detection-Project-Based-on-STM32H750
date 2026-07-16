============================================================
  BW21 远程机器人控制系统 — 项目打包
============================================================

目录结构:
  服务器端/         Python 后端 (aiohttp + WebSocket + UDP)
  前端/             Vue3 前端 (需要 npm install && npm run build)
  固件/             BW21 嵌入式固件

============================================================
一、数据格式总览
============================================================

[1] STM32 → BW21 (Serial1, 9600 baud)
    传感器: T:25.3,H:60.1,A:120.5,P:1013.2,G:10.0,I:0
    GPS新:  G:39.904200,116.407400,12
    GPS旧:  Time: 12:30:45  Lat: N 39.904200  Lon: E 116.407400  Sat: 12

    T=温度(°C) H=湿度(%) A=海拔(m) P=气压(hPa)
    G=可燃气体(%LEL)  I=人体识别(1=有人)

[2] BW21 → 服务器 (UDP :9093 JSON)
    {"temperature":25.3,"humidity":60.1,"altitude":120.5,"pressure":1013.2,
     "gas":{"concentration":10.0,"alarm":false},"person_detected":0,
     "lat":39.904200,"lng":116.407400,"satellites":12}

[3] 服务器 → BW21 (UDP :9093 JSON)
    {"cmd":"move","direction":"forward","speed":100}
    {"cmd":"stop"}

[4] BW21 → STM32 (Serial1 TX)
    F/B/L/R = 前进/后退/左转/右转
    S = 停止 (含500ms超时自动发送)

[5] 视频帧 → 服务器 (UDP :9091)
    [H.264 Annex B NAL][0x64 0x00 0x00 0x00]

[6] 服务器 → 前端 (WebSocket)
    /ws/sensor  传感器数据
    /ws/gps     GPS定位数据
    /ws/control 远程控制指令

============================================================
二、端口清单
============================================================

  8765  HTTP/WebSocket (aiohttp)
  9091  UDP H.264 视频流
  9093  UDP 传感器数据 + 控制指令 (双向)

============================================================
三、部署步骤
============================================================

服务器:
  pip3.8 install aiohttp
  cd 服务器端
  export ROBOT_ADMIN_USER=admin
  export ROBOT_ADMIN_PASSWORD=请替换为强密码
  nohup python3.8 server.py > server.log 2>&1 &

前端:
  cd 前端 && npm install && npm run build
  (dist/ 部署到 nginx 或宝塔网站目录)

固件:
  Arduino IDE 打开 bw21_firmware.ino
  修改 SERVER_IP 为服务器 IP
  编译烧录到 BW21 (AMB82-MINI)

============================================================
四、硬件接线
============================================================

  BW21 IOA3(RX) ← STM32 TX  (传感器 + GPS, 共线)
  BW21 IOA2(TX) → STM32 RX  (控制标志位)
  GND ↔ GND
