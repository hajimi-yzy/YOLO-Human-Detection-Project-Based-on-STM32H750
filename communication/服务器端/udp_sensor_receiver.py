"""
UDP 传感器数据接收器 + 控制指令回传
从 BW21 接收 JSON 传感器数据并广播到 WebSocket 客户端
同时可向 BW21 回传控制指令

数据格式: {"temperature":25.3,"humidity":60.1,"altitude":120.5,"pressure":1013.2,"lat":39.9042,"lng":116.4074,"satellites":12}
"""
import json
import socket
import asyncio
import threading
from ws_sensor import broadcast_sensor_data, update_sensor_data
from ws_gps import broadcast_gps_data, update_gps_data

_sensor_data = {
    "temperature": None,
    "humidity": None,
    "altitude": None,
    "pressure": None,
    "timestamp": 0,
}

# 记录 BW21 的来源地址（用于回传控制指令）
_bw21_addr = None
_sock = None
_sock_lock = threading.Lock()


def get_sensor_data():
    """获取最新传感器数据（线程安全）"""
    return dict(_sensor_data)


def send_command_to_bw21(cmd_dict):
    """
    向 BW21 发送控制指令（线程安全）
    cmd_dict 示例: {"cmd": "move", "direction": "forward", "speed": 100}
    """
    global _bw21_addr, _sock
    with _sock_lock:
        if _sock is None or _bw21_addr is None:
            print(f'[SENSOR-UDP] 无法发送指令：BW21 地址未知', flush=True)
            return False
        try:
            payload = json.dumps(cmd_dict, separators=(',', ':')).encode('utf-8')
            n = _sock.sendto(payload, _bw21_addr)
            print(f'[SENSOR-UDP] →BW21 {n}B cmd={cmd_dict.get("cmd")}', flush=True)
            return n > 0
        except Exception as e:
            print(f'[SENSOR-UDP] 发送指令失败: {e}', flush=True)
            return False


def _udp_sensor_thread(port, loop):
    global _sensor_data, _bw21_addr, _sock
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', port))
    sock.settimeout(1.0)

    with _sock_lock:
        _sock = sock

    print(f'[SENSOR-UDP] listening :{port}', flush=True)
    fc = 0

    try:
        while True:
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break

            # 记录 BW21 来源地址
            _bw21_addr = addr

            try:
                msg = data.decode('utf-8', errors='replace').strip()
                parsed = json.loads(msg)

                # 更新全局传感器数据
                _sensor_data = {
                    "temperature": parsed.get("temperature"),
                    "humidity": parsed.get("humidity"),
                    "altitude": parsed.get("altitude"),
                    "pressure": parsed.get("pressure"),
                    "gas": parsed.get("gas", {}),
                    "person_detected": parsed.get("person_detected", 0),
                    "timestamp": parsed.get("timestamp", 0),
                }

                # 通过 WebSocket 广播给所有客户端
                loop.call_soon_threadsafe(
                    lambda d=dict(_sensor_data): asyncio.ensure_future(broadcast_sensor_data(d))
                )

                # ── 提取 GPS 数据并广播 ──
                gps_lat = parsed.get("lat")
                gps_lng = parsed.get("lng")
                if gps_lat is not None and gps_lng is not None:
                    gps_data = {
                        "lat": gps_lat,
                        "lng": gps_lng,
                        "satellites": parsed.get("satellites", 0),
                        "speed": parsed.get("speed", 0),
                        "heading": parsed.get("heading", 0),
                        "timestamp": parsed.get("timestamp", 0),
                    }
                    loop.call_soon_threadsafe(
                        lambda d=dict(gps_data): asyncio.ensure_future(broadcast_gps_data(d))
                    )

                fc += 1
                if fc <= 5 or fc % 50 == 0:
                    gps_info = ""
                    if gps_lat is not None:
                        gps_info = f' GPS=({gps_lat:.6f},{gps_lng:.6f}) sat={gps_data["satellites"]}'
                    print(f'[SENSOR-UDP] #{fc} T={_sensor_data["temperature"]}°C '
                          f'H={_sensor_data["humidity"]}% A={_sensor_data["altitude"]}m '
                          f'P={_sensor_data["pressure"]}hPa{gps_info}', flush=True)

            except json.JSONDecodeError:
                if fc < 3:
                    print(f'[SENSOR-UDP] invalid JSON: {msg[:80]}', flush=True)
            except Exception as e:
                if fc < 3:
                    print(f'[SENSOR-UDP] parse err: {e}', flush=True)

    except Exception as e:
        print(f'[SENSOR-UDP] err: {e}', flush=True)
    finally:
        with _sock_lock:
            _sock = None
            _bw21_addr = None
        sock.close()
        print(f'[SENSOR-UDP] stopped, {fc} packets', flush=True)


async def start_udp_sensor_receiver(port=9093):
    """启动 UDP 传感器数据接收器"""
    t = threading.Thread(
        target=_udp_sensor_thread,
        args=(port, asyncio.get_running_loop()),
        daemon=True,
    )
    t.start()
    try:
        while t.is_alive():
            await asyncio.sleep(1)
    except asyncio.CancelledError:
        pass
