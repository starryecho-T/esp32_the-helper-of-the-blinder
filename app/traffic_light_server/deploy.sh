#!/usr/bin/env bash
# 在阿里云 ECS (Ubuntu 22.04) 上部署交通灯识别服务
#
# 用法：
#   1. 先在本地把整个 traffic_light_server 目录（含 server.py, requirements.txt,
#      yolov8n.pt, traffic-light-server.service）上传到服务器，例如：
#        scp -r app/traffic_light_server root@<ECS公网IP>:/opt/traffic_light_server
#   2. 在服务器上执行：
#        cd /opt/traffic_light_server && bash deploy.sh
#
# Python 环境已经配好的话，跳过装环境，直接指定解释器：
#   PYTHON_BIN=/usr/bin/python3   bash deploy.sh     # 用系统 python
#   PYTHON_BIN=/opt/venv/bin/python bash deploy.sh   # 用你自己的 venv
#   PYTHON_BIN=$(which python3)   bash deploy.sh     # 用当前 shell 的 python
# 只要给了 PYTHON_BIN，就会自动跳过 apt/pip/venv 那几步（除非你的 python 里
# 缺 fastapi / uvicorn / ultralytics，脚本会检测到并只补装缺的包）。
#
# 注意：阿里云安全组必须另外在控制台放行 8000 端口，脚本管不到。

set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_NAME="traffic-light-server"
PORT="${PORT:-8000}"
PY_MIRROR="https://mirrors.aliyun.com/pypi/simple/"
PYTHON_BIN="${PYTHON_BIN:-}"

if [ -n "$PYTHON_BIN" ]; then
  echo "[1/6] 使用已有 Python: $PYTHON_BIN（跳过装环境）"
else
  echo "[1/6] 安装系统依赖并创建虚拟环境"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq python3-venv python3-pip > /dev/null
  python3 -m venv "$APP_DIR/venv"
  PYTHON_BIN="$APP_DIR/venv/bin/python"
fi

command -v "$PYTHON_BIN" > /dev/null || { echo "[!] 找不到解释器 $PYTHON_BIN"; exit 1; }
BIN_DIR="$(dirname "$PYTHON_BIN")"
UVICORN="$BIN_DIR/uvicorn"
echo "     python 版本: $("$PYTHON_BIN" -V 2>&1)"

echo "[2/6] 检查依赖"
MISSING=""
for pkg in fastapi uvicorn ultralytics cv2 numpy requests; do
  "$PYTHON_BIN" -c "import $pkg" 2> /dev/null || MISSING="$MISSING $pkg"
done
if [ -n "$MISSING" ]; then
  echo "     缺少:$MISSING → 用阿里云镜像补装（cv2 对应 opencv-python-headless）"
  "$PYTHON_BIN" -m pip install -r "$APP_DIR/requirements.txt" -i "$PY_MIRROR"
else
  echo "     fastapi / uvicorn / ultralytics / cv2 / numpy / requests 齐全"
fi
command -v "$UVICORN" > /dev/null || { echo "     补装 uvicorn"; "$PYTHON_BIN" -m pip install "uvicorn[standard]" -i "$PY_MIRROR"; }

if [ ! -f "$APP_DIR/yolov8n.pt" ]; then
  echo "[!] 未找到 yolov8n.pt，请把它放到 $APP_DIR 后重新运行本脚本"
  exit 1
fi
echo "[3/6] 模型文件 yolov8n.pt 已就位"

echo "[4/6] 生成设备上传 token 并写服务文件"
TOKEN="$(tr -dc 'a-zA-Z0-9' < /dev/urandom | head -c 24)"
sed -i "s#^\(WorkingDirectory=\).*#\1$APP_DIR#" "$APP_DIR/traffic-light-server.service"
sed -i "s#^\(Environment=\"DEVICE_TOKEN=\).*#\1$TOKEN\"#" "$APP_DIR/traffic-light-server.service"
sed -i "s#^\(Environment=\"YOLO_MODEL=\).*#\1$APP_DIR/yolov8n.pt\"#" "$APP_DIR/traffic-light-server.service"
sed -i "s#^\(Environment=\"FRAME_PATH=\).*#\1$APP_DIR/latest.jpg\"#" "$APP_DIR/traffic-light-server.service"
sed -i "s#^ExecStart=.*#ExecStart=$UVICORN server:app --host 0.0.0.0 --port $PORT --workers 1#" "$APP_DIR/traffic-light-server.service"
sed -i "s#^User=.*#User=$(id -un)#" "$APP_DIR/traffic-light-server.service"
echo "     DEVICE_TOKEN = $TOKEN"
echo "     ^^^^^^^^^^^ 这个 token 要填进 ESP32 固件的 DEVICE_TOKEN"

echo "[5/6] 注册 systemd 服务"
cp "$APP_DIR/traffic-light-server.service" "/etc/systemd/system/$SERVICE_NAME.service"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME" > /dev/null
systemctl restart "$SERVICE_NAME"

echo "[6/6] 等待服务就绪并检查"
sleep 5
systemctl --no-pager status "$SERVICE_NAME" | head -12
echo "--- /health ---"
curl -s "http://127.0.0.1:$PORT/health" || echo "(无响应，看日志：journalctl -u $SERVICE_NAME -f)"
echo
echo "--- /status ---"
curl -s "http://127.0.0.1:$PORT/status"
echo
echo
echo "完成。别忘了："
echo "  1. 阿里云控制台 → 安全组 → 入方向放行 TCP $PORT"
echo "  2. 本地测试：curl http://<ECS公网IP>:$PORT/health"
echo "  3. ESP32 固件里 SERVER_HOST 改成 http://<ECS公网IP>:$PORT ，DEVICE_TOKEN 填上面的值"
