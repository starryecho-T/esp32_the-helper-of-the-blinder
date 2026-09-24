/**
 * 前哨主控 ESP32-S3 CAM 完整固件（云端推流版 · 已修复 HTTP -3）
 *
 * 功能：
 *   1. 超声波高位测距 → ESP-NOW 发给盲杖
 *   2. 接收盲杖拍照命令 → 预热摄像头
 *   3. 摄像头 Web Server（手机可直接访问 /capture 拍照、/stream 实时）
 *   4. 每 2 秒主动 POST 一帧到云服务器，App 调 GET /detect 拿红绿灯识别结果
 *
 * 接线：
 *   HC-SR04:  Trig→GPIO1, Echo→GPIO14, VCC→5V, GND→GND
 *     （GPIO9被摄像头Y3占用；GPIO2有板载LED下拉导致Echo读不到高电平，改用GPIO14）
 *   摄像头:   板载，不用接线
 *
 * 摄像头占用的引脚（Freenove ESP32-S3 WROOM，不能用）：
 *   4,5,6,7,8,9,10,11,12,13,15,16,17,18
 *   超声波用 GPIO1(Trig) 和 GPIO14(Echo)，不冲突
 *
 * ============================================================
 * 推流间隔：2 秒一帧（UPLOAD_INTERVAL_MS = 2000）
 * ------------------------------------------------------------
 * 云端 FRAME_MAX_AGE 当前是 45 秒，2 秒一帧时帧龄最多 2~3 秒，
 * 余量足够，服务器不用改。
 * 注意：27KB 一帧约要发 2 秒，间隔 2 秒意味着几乎"发完立刻发下一帧"，
 * 上传期间（阻塞约 2 秒）超声波和 ESP-NOW 会停更 —— 若盲杖端的距离
 * 报警明显变迟钝，把间隔调到 3000~5000 或把 jpeg_quality 调到 15。
 * ============================================================
 * 针对 "HTTP -3"（send payload failed）的最终方案
 * ------------------------------------------------------------
 * 现象：5 字节能发、3072 字节能发、超过约 3KB 就卡住约 10 秒后 -3。
 * 原因：http.POST(buf, len) 把整块 19KB 一次性丢给 NetworkClient::write()，
 *       写不完就失败。
 * 解决：新增 CameraBufferStream，把 framebuffer 包装成 Stream，
 *       改用 http.sendRequest("POST", &stream, len) —— HTTPClient 内部
 *       按约 1460 字节（TCP 发送缓冲）分块发送并重试短写。
 * 顺带修掉的隐患：
 *   · esp_camera_fb_return(fb) 之后仍访问 fb->len（野指针）→ 改用 imageLen
 *   · readDistance() 里的 noInterrupts() 最长屏蔽 30ms 中断，会掐断
 *     WiFi/ESP-NOW/HTTP → 去掉
 *   · WiFi.setSleep(false) 保留（连上 WiFi 与重连后各关一次省电）
 * ============================================================
 */

// 摄像头型号：ESP32-S3-EYE（Freenove ESP32-S3 WROOM 用此型号，引脚与官方例程一致）
#define CAMERA_MODEL_ESP32S3_EYE

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <esp_camera.h>
#include "esp_http_server.h"
#include "camera_pins.h"

// ====================== 引脚 ======================
#define TRIG_PIN 1     // GPIO1 空闲
#define ECHO_PIN 14   // GPIO14 空闲（GPIO2有板载LED下拉，导致Echo读不到高电平）

// ====================== 盲杖 MAC ======================
uint8_t caneMac[] = {0x94, 0xA9, 0x90, 0xCA, 0xAE, 0x64};

// ====================== WiFi ======================
const char* ssid = "starry";       // 改成你的 WiFi
const char* password = "iloveyouso";  // 改成你的 WiFi 密码

// ====================== 云端识别（前哨主动推流） ======================
// 架构：前哨 POST 一帧 JPEG → 云端缓存 → App 调 GET /detect 拿识别结果
// 云端在公网，前哨只要能上网即可，不再要求三者在同一局域网。
const char* CLOUD_HOST  = "http://39.106.216.80:8000";
const char* CLOUD_TOKEN = "YFOpzRWJxw6G2dl4jkNUMX7h";  // 与服务器 DEVICE_TOKEN 一致
const unsigned long UPLOAD_INTERVAL_MS = 2000;          // 2 秒推一帧
// 注意：云端 FRAME_MAX_AGE=45 秒已够用，不用改服务器
// 若盲杖距离报警变迟钝（上传阻塞 2 秒），改回 3000~5000
// 注意：云端 server.py 的 FRAME_MAX_AGE 必须 ≥ 45，否则帧会被判过期（见文件头说明）

// ====================== 数据结构 ======================
// 前哨 → 盲杖：高位距离
typedef struct {
  float distance;
  int level;
} ScoutData;

// 盲杖 → 前哨：命令
typedef struct {
  int cmd;  // 1=拍照
} CaneCmd;

ScoutData sendData;
CaneCmd recvCmd;

// ====================== 超声波测距 ======================
float readDistance() {
  // 不再用 noInterrupts()：最长 30ms 屏蔽全部中断会掐断 WiFi/ESP-NOW/HTTP，
  // 而且 pulseIn 本身误差可接受。上传大包时尤其不能关中断。
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 30000);
  if (d == 0) return -1.0;
  return d * 0.0343 / 2.0;
}

// 超声波诊断：直接打印 Trig/Echo 电平和 pulseIn 原始值
void diagnoseUltrasonic() {
  Serial.println("\n---------- 超声波诊断 ----------");
  Serial.printf("  Trig脚(GPIO%d) 静态电平: %d\n", TRIG_PIN, digitalRead(TRIG_PIN));
  Serial.printf("  Echo脚(GPIO%d) 静态电平: %d\n", ECHO_PIN, digitalRead(ECHO_PIN));
  Serial.println("  发送10us触发脉冲...");
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(5);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(15);
  digitalWrite(TRIG_PIN, LOW);
  long d = pulseIn(ECHO_PIN, HIGH, 50000);
  Serial.printf("  pulseIn 原始值: %ld (0=超时没收到回波)\n", d);
  if (d > 0) {
    Serial.printf("  换算距离: %.1f cm\n", d * 0.0343 / 2.0);
  } else {
    Serial.println("  没收到回波！检查：");
    Serial.println("    1. VCC 是否接 5V（不是3.3V）");
    Serial.println("    2. GND 是否接好");
    Serial.println("    3. Trig/Echo 是否接反");
    Serial.println("    4. 跳线帽是否已拔掉（GPIO模式）");
    Serial.println("    5. 前方30cm-2m内是否有障碍物");
  }
  Serial.println("------------------------------");
}

int getLevel(float dist) {
  if (dist < 0) return 0;
  if (dist < 30) return 3;
  if (dist < 80) return 2;
  if (dist < 150) return 1;
  return 0;
}

// ====================== 摄像头初始化 ======================
bool cameraInit() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_QVGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 2;

  if (psramFound()) {
    config.jpeg_quality = 10;      // 30s 才一帧，用回高画质（数值越小画质越高）
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("摄像头初始化失败: 0x%x\n", err);
    return false;
  }

  // 翻转画面（根据实际安装方向调整）
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_brightness(s, 1);

  Serial.println("摄像头初始化成功");
  return true;
}

// ====================== 拍照 ======================
camera_fb_t* takePhoto() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb != NULL) {
    Serial.printf("[拍照] %dx%d %d字节\n", fb->width, fb->height, fb->len);
  } else {
    Serial.println("[拍照] 失败");
  }
  return fb;
}

// ======================================================
// 把摄像头 framebuffer 包装成 Stream
//   原来的 http.POST(buf, len) 走"整块 payload"路径，把整个 19KB
//   一次性交给 NetworkClient::write()，写不完就报 -3。
//   改成 Stream 后，HTTPClient 内部按约 1460 字节（TCP 发送缓冲区）
//   分块发送并对"短写"重试，大图就能稳定发完。
// ======================================================
class CameraBufferStream : public Stream {
private:
  const uint8_t* _data;
  size_t _length;
  size_t _position;

public:
  CameraBufferStream(const uint8_t* data, size_t length)
    : _data(data), _length(length), _position(0) {}

  int available() override { return (int)(_length - _position); }

  int read() override {
    if (_position >= _length) return -1;
    return _data[_position++];
  }

  int peek() override {
    if (_position >= _length) return -1;
    return _data[_position];
  }

  void flush() override {}

  size_t write(uint8_t) override { return 0; }
};

// ====================== 云端推流 ======================
// 拍一帧 JPEG，POST 到云服务器 /upload。云端缓存最新帧，App 再调 /detect 识别。
// 复用已初始化的摄像头（cameraInit()），不重复定义引脚/初始化。
bool uploadFrame() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[upload] WiFi 未连接");
    return false;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("[upload] 取帧失败"); return false; }

  // 必须先把长度存下来：后面 fb 会被归还，再访问 fb->len 就是野指针
  const size_t imageLen = fb->len;

  Serial.printf("[upload] 开始上传 JPEG：%u 字节，RSSI=%d，可用内存=%u\n",
                imageLen, WiFi.RSSI(), ESP.getFreeHeap());

  char url[160];
  snprintf(url, sizeof(url), "%s/upload?token=%s", CLOUD_HOST, CLOUD_TOKEN);

  HTTPClient http;
  if (!http.begin(url)) {
    Serial.println("[upload] HTTP begin 失败");
    esp_camera_fb_return(fb);
    return false;
  }
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(15000);   // Stream 分块发送，留足时间

  CameraBufferStream stream(fb->buf, imageLen);

  Serial.println("[upload] 开始 Stream POST...");
  int code = http.sendRequest("POST", &stream, imageLen);
  String errText = (code < 0) ? http.errorToString(code) : "";

  if (code > 0) {
    String response = http.getString();
    Serial.printf("[upload] HTTP %d，服务器返回：%s\n", code, response.c_str());
  } else {
    Serial.printf("[upload] HTTP %d %s\n", code, errText.c_str());
  }

  http.end();
  esp_camera_fb_return(fb);   // 归还放在所有访问 fb 之后

  Serial.printf("[upload] 完成：%u 字节，HTTP=%d，RSSI=%d，可用内存=%u\n",
                imageLen, code, WiFi.RSSI(), ESP.getFreeHeap());
  return (code == 200);
}

// ====================== HTTP /capture 接口 ======================
// 手机浏览器访问 http://前哨IP/capture 可以拿到一张 JPEG 照片
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// HTTP /stream 接口（实时视频流）
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=123456789000000000000987654321");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }

    size_t hlen = snprintf(part_buf, 64, "--123456789000000000000987654321\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, "\r\n", 2);
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
  }
  return res;
}

// 根路径 "/" 索引页：打开就能确认服务器活着
static esp_err_t index_handler(httpd_req_t *req) {
  const char html[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>前哨 Scout</title></head>"
    "<body style='font-family:sans-serif;max-width:600px;margin:40px auto;text-align:center'>"
    "<h2>前哨 Scout 在线</h2>"
    "<p><a href='/capture'><button style='padding:20px 40px;font-size:18px'>拍照</button></a></p>"
    "<p><a href='/stream'><button style='padding:10px 30px;font-size:15px'>实时画面</button></a></p>"
    "<p style='color:#888'>拍照=返回一张 JPEG，实时=连续视频流</p>"
    "</body></html>";
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, html, strlen(html));
}

// 启动 Web 服务器
void startCameraServer() {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
  };

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
  };

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
  };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &capture_uri);
    httpd_register_uri_handler(server, &stream_uri);
    Serial.println("Web 服务器启动: / (主页) /capture (拍照) /stream (实时)");
  }
}

// ====================== ESP-NOW ======================
// 盲杖→前哨：拍照命令。前哨本身不主动上传照片，照片由手机通过 /capture 拉取。
// 这里收到命令后预热摄像头（取一帧即归还），让下次 /capture 响应更快。
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  memcpy(&recvCmd, data, sizeof(recvCmd));
  if (recvCmd.cmd == 1) {
    Serial.println("[ESP-NOW] 收到拍照命令，预热摄像头");
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
}

// ====================== 初始化 ======================
void setup() {
  Serial.begin(115200);
  Serial.println("\n==============================================");
  Serial.println("     Scout-CAM  智能盲杖前哨（云端推流版）");
  Serial.println("==============================================");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // 摄像头
  bool camOk = cameraInit();
  Serial.printf("  [摄像头]   %s\n", camOk ? "OK" : "失败");

  // WiFi：连手机热点。ESP-NOW 信道必须和 WiFi 信道一致，所以连上后读实际信道。
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  Serial.print("  [WiFi]     连接热点中");
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 20) {
    delay(500); Serial.print("."); wifiTimeout++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n  [WiFi]     OK  IP: %s\n", WiFi.localIP().toString().c_str());
    // ★ 关键修复（针对 HTTP -3 send payload failed）：
    //   ESP32 默认开 modem 省电睡眠，射频在两次收发之间"打盹"，
    //   发十几 KB 图片时容易中途断流 → TCP 写失败 → HTTP -3。
    //   关掉省电后上传立刻稳定。这是 ESP32-CAM 上传不稳的头号原因。
    WiFi.setSleep(false);
    Serial.printf("  [WiFi]     已关闭省电睡眠，信号 RSSI=%d dBm（-50 很好 / -70 一般 / -80 很差）\n", WiFi.RSSI());
    // mDNS：以后访问 http://scout.local/capture，IP 变了也不用管
    if (MDNS.begin("scout")) {
      Serial.println("  [mDNS]     OK  可用 http://scout.local/capture");
    }
  } else {
    Serial.println("\n  [WiFi]     失败  ESP-NOW 将用默认信道");
  }
  Serial.print("     本机 MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("     对端(盲杖) MAC: ");
  for (int i = 0; i < 6; i++) { if (caneMac[i] < 0x10) Serial.print("0"); Serial.print(caneMac[i], HEX); if (i < 5) Serial.print(":"); }
  Serial.println();

  // 读取 WiFi 实际信道，ESP-NOW 必须用同一信道，否则 peer channel 报错
  uint8_t primaryChan = 1;
  wifi_second_chan_t secChan = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&primaryChan, &secChan);
  Serial.printf("  [信道]     WiFi 实际信道=%d，ESP-NOW 将用此信道\n", primaryChan);

  // 启动 HTTP 服务器（手机访问 http://<前哨IP>/capture 拉照片）
  startCameraServer();
  Serial.println("  [HTTP]     /(主页) /capture(拍照) /stream(实时) 就绪");

  // ESP-NOW（和盲杖通信）。用 WiFi 实际信道，与盲杖对齐
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, caneMac, 6);
    peer.channel = primaryChan;   // 用实际信道，不写死
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    Serial.printf("  [ESP-NOW]  就绪 (信道%d)\n", primaryChan);
  } else {
    Serial.println("  [ESP-NOW]  失败");
  }

  Serial.println("==============================================");
  Serial.println("初始化完成。");
  Serial.println("  · 本地拍照: http://scout.local/capture （IP: http://" + WiFi.localIP().toString() + "/capture）");
  Serial.println("  · 云端推流: 每2s POST → " + String(CLOUD_HOST) + "/upload");
  Serial.println("串口: dist(超声波) / upload(手动推一帧) / status(查云端) / help");
}

// ====================== 串口命令 ======================
void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "dist") {
    diagnoseUltrasonic();
  } else if (cmd == "upload") {
    Serial.println("手动推一帧到云端…");
    uploadFrame();
  } else if (cmd == "status") {
    Serial.printf("云端地址: %s   推流间隔: %lums\n", CLOUD_HOST, UPLOAD_INTERVAL_MS);
    Serial.println("浏览器打开 " + String(CLOUD_HOST) + "/status 看帧年龄");
  } else if (cmd == "help") {
    Serial.println("命令: dist(超声波诊断) / upload(手动推一帧) / status(云端信息)");
  }
}

// ====================== 主循环 ======================
void loop() {
  handleSerial();

  // WiFi 掉线自动重连（掉线后网页会打不开，必须重连）
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] 掉线，重连中…");
      WiFi.disconnect();
      WiFi.reconnect();
      // 重连后省电设置会复位，必须重新关掉，否则上传又会开始报 -3
      WiFi.setSleep(false);
    }
  }

  // 每 500ms 测高位距离，ESP-NOW 发给盲杖
  static unsigned long lastSend = 0;
  static int lastLevel = -1;
  if (millis() - lastSend >= 500) {
    lastSend = millis();
    sendData.distance = readDistance();
    sendData.level = getLevel(sendData.distance);

    // 等级变化时打印，避免刷屏
    if (sendData.level != lastLevel) {
      lastLevel = sendData.level;
      Serial.printf("[前哨] 高位=%.0fcm 等级=%d\n", sendData.distance, sendData.level);
    }

    esp_now_send(caneMac, (uint8_t *)&sendData, sizeof(sendData));
  }

  // 每 2s 推一帧到云端（云识别），用 millis 定时，不阻塞测距
  // 2000 是 500 的整数倍，会和超声波节拍对齐；影响不大（错开最多半拍），
  // 若介意可改成 2200 这类非整数倍。
  // 初值 = millis()-UPLOAD_INTERVAL_MS → 上电后立刻推第一帧
  static unsigned long lastUpload = millis() - UPLOAD_INTERVAL_MS;
  if (millis() - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = millis();
    uploadFrame();
  }
}
