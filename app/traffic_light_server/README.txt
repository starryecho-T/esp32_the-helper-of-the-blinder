智能盲杖交通灯识别后端

1. 安装 Python 3.10/3.11。
2. 在此目录运行：
   pip install -r requirements.txt
3. 启动：
   python -m uvicorn server:app --host 0.0.0.0 --port 8000

App Inventor 默认接口：
http://192.168.43.248:8000/detect

如果运行服务器的电脑/树莓派 IP 不是 192.168.43.248，需要在 App Inventor 的 Blocks 中把全局 TrafficURL 改成：
http://你的设备IP:8000/detect

浏览器可测试：
http://你的设备IP:8000/health
返回 OK 即表示服务器正常。

接口说明：

【POST /detect】手动上传图片识别
  输入：HTTP POST 原始图片二进制
  输出：
  RED|96%
  YELLOW|91%
  GREEN|95%
  NONE|0

【GET /detect?cam=<摄像头地址】摄像头直识（服务器自己从头戴 ESP32-CAM 拉图，App 无需上传）
  示例：
  http://你的设备IP:8000/detect?cam=http://192.168.43.248/capture
  cam 参数不填时默认 http://192.168.43.248/capture
  输出格式与 POST 完全相同。
  摄像头不可达时返回 HTTP 502。

App 端（新版 .aia）里「摄像头识别」按钮自动调用：
  Web.Url = TrafficURL + "?cam=" + CameraURL，然后 Web.Get()

注意：YOLO 使用 COCO 的 traffic light 类别，再根据检测框中的颜色判断红/黄/绿。对于夜间、严重遮挡、远距离或强反光图片，建议后续用你自己的交通灯数据集训练专用模型以提高可靠性。

【同局域网模式】的网络要求（老的 /detect?cam= 直连方式）：
只有在这种模式下，服务器才必须能直接访问 ESP32-CAM 的 IP
（默认 http://192.168.43.248/capture），即手机、ESP32-CAM、服务器需在同一局域网
或同一手机热点下，三者互通方案才成立。
⚠️ 云服务器部署时请看下面的「设备主动推送」模式，该模式不要求任何设备在同一网络。

=========================================================
云服务器部署（阿里云 ECS）：改用「设备主动推送」模式
=========================================================
云服务器在公网，ESP32-CAM 在内网，服务器拉不到摄像头。因此主流程改为：

  ESP32-CAM --POST /upload--> 云服务器（缓存最新一帧） <--GET /detect-- 手机 App

【网络要求（该模式下）】三者可以处于完全不同的网络，无需同一 WiFi：
  · ESP32-CAM：任意能上网的 2.4GHz WiFi / 手机热点（ESP32 不支持 5GHz）
  · 云服务器：公网 IP，安全组入方向放行 8000（阿里云控制台设置）
  · 手机 App ：4G/5G/WiFi 均可，只要能上网
  唯一要求是 ESP32-CAM 所在网络允许「向外访问 TCP 8000」。
  需要网页认证的网络（校园网、酒店 WiFi）ESP32 无法通过认证，请用手机热点。

【POST /upload?token=<token>】ESP32-CAM 主动上传一帧
  输入：HTTP POST 原始 JPEG 二进制（body）
  鉴权：环境变量 DEVICE_TOKEN 非空时，必须带 ?token=xxx 或 X-Device-Token 头
  输出：OK
  固件示例见 firmware/scout-c3/cloud_upload/cloud_upload.ino

【GET /detect】无 cam 参数时用设备推送上来的最新一帧做识别
  帧超过 FRAME_MAX_AGE（默认 20 秒）没更新 → 返回 HTTP 503
  仍传 ?cam=<url> 则走老的直连拉取模式（服务器与摄像头同网时可用）

【GET /status】调试用，查看最新帧字节数和年龄，例如 "OK frame bytes=24513 age=0.8s"

【GET /snapshot】返回设备最新一帧 JPEG 图片（给家属端看画面用）
  浏览器直接打开 http://<服务器>:8000/latest.jpg 也能看
  家属端 App（Image 组件）的 Picture 填：
    http://<服务器>:8000/snapshot?token=<VIEW_TOKEN>&t=<毫秒时间戳>
  · token：环境变量 VIEW_TOKEN 非空时必须带（防止陌生人看画面）
  · t：每次换个值（如 Clock.SystemTime），用来绕开 App Inventor 的图片缓存
  · 设备超过 FRAME_MAX_AGE 没推帧时返回 HTTP 503
  定时刷新（Clock 组件 2000ms）即可实现家属端近实时查看。

环境变量（见 traffic-light-server.service）：
  YOLO_MODEL       模型路径，默认 yolov8n.pt
  CAMERA_URL       能直连摄像头时才配；云上部署留空
  DEVICE_TOKEN     上传鉴权 token，建议设为随机字符串
  VIEW_TOKEN       家属端看画面的 token，留空则不校验（不建议）
  FRAME_MAX_AGE    缓存帧最长可用时间（秒），默认 20
  FRAME_PATH       缓存帧落盘路径，默认 latest.jpg
  CAMERA_TIMEOUT   直连拉取超时（秒），默认 5

注意：uvicorn 必须 --workers 1，最新一帧缓存在内存里，多进程会各存一份。
