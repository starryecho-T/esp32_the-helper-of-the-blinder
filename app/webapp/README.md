# 智能盲杖 WebApp（重构版）

> 由 MIT App Inventor 工程重构而来的**零依赖原生 Web 应用**。
> 功能与原版 1:1 对应，架构全面升级为「核心层 / 服务层 / 页面控制器」三层结构。
> 原工程备份：`app/备份/`（保持不动）+ 本目录 `assets/legacy/`（双保险）。

---

## 一、目录结构

```
app/webapp/
├── index.html              # 入口：选择盲人端 / 家属端
├── blind.html              # 盲人端页面
├── family.html             # 家属端页面
├── css/app.css             # 设计系统（大字体/高对比度/无障碍）
├── js/
│   ├── core/               # —— 核心层（与 UI 无关，可复用可测试）——
│   │   ├── config.js       #   配置中心 + localStorage 持久化
│   │   ├── bus.js          #   事件总线（发布/订阅，解耦服务与 UI）
│   │   ├── store.js        #   响应式状态管理
│   │   └── protocol.js     #   BLE 协议解析/指令构造（纯函数）
│   ├── services/           # —— 服务层（每类外部能力一个模块）——
│   │   ├── ble.js          #   Web Bluetooth（Nordic UART，连盲杖）
│   │   ├── firebase.js     #   Firebase RTDB REST（位置/SOS）
│   │   ├── trafficlight.js #   交通灯识别 /detect、实时画面 /snapshot
│   │   ├── geo.js          #   GPS 定位
│   │   └── tts.js          #   中文语音播报
│   ├── blind/app.js        # —— 页面控制器：盲人端业务流
│   └── family/app.js       # —— 页面控制器：家属端业务流
├── assets/legacy/          # 原 App Inventor .aia 工程备份
├── tests/protocol.test.html# 协议解析单元测试（浏览器打开即跑）
└── README.md
```

**数据流（单向）**：

```
盲杖 BLE ──→ ble.js ──→ bus(ble:data) ──→ protocol.parseLine()
                                          ├─ status → store.cane → 页面刷新
                                          └─ event  → blind/app.js
                                                 ├─ CAMERA:CAPTURE → trafficlight.detect()
                                                 │      → UI + tts.speak() + 回传色值
                                                 └─ ALARM/FALL → firebase.putSos/putLocation
手机 GPS ──→ geo.js ──→ store.geo ──→ 10s 定时 → firebase.putLocation()
家属端   ←── firebase.fetchBlind()（5s）──→ SOS 徽章 + 地图 marker
         ←── trafficlight.snapshotUrl()（2s）──→ 实时画面 <img>
```

---

## 二、功能映射（原 App Inventor → WebApp）

### 盲人端（原 cane_controller__2.aia）

| 原 App 功能 | 原实现 | 新实现 |
|------------|--------|--------|
| 扫描/选择设备 | 扫描按钮 + ListPicker | 系统蓝牙选择器（点「连接盲杖」弹出，按 `SmartCane` 前缀过滤）|
| 连接/断开 | BluetoothLE 扩展 | Web Bluetooth（NUS 6E400001/2/3）|
| 状态显示 | 计时刷新 Label | `protocol.parseLine` → store → DOM |
| 模式切换 | 三按钮 MODE:0/1/2 | 相同指令，三按钮 |
| 电池/报警/障碍/距离 | 周期状态包解析 | 相同（含 TYPE/LEVEL/FALLST/ENC/ACC/MPU/LAT/LNG/LIGHT 扩展字段）|
| 交通灯识别 | CAMERA:CAPTURE → GET /detect | 相同闭环：识别 → 显示+置信度 → TTS 播报 → 回传 RED/YELLOW/GREEN/NONE |
| SOS 上报 | PutText sos.json true/false | 相同（ALARM:MANUAL/CANCEL、FALL:CONFIRMED/CANCELLED 触发）|
| GPS 上传 | LocationSensor + 10s 计时器 | Geolocation watchPosition + 10s setInterval + 事件即时上传 |
| 语音播报 | TextToSpeech 组件 | Web Speech API（zh-CN）|
| 通知 | Notifier ShowAlert | toast + aria-live（TalkBack 可读）|

**新增便利功能（不破坏原流程）**：
- 「手动识别一次」按钮（原只能盲杖长按触发）
- 「远程取消报警」按钮（协议 §5.2 定义、演示流程 6:20 需要的能力）
- 调试日志面板（BLE 收发可见）

### 家属端（原 smartcanefamily.aia）

| 原 App 功能 | 新实现 |
|------------|--------|
| 5s 轮询 /blind001.json | `firebase.fetchBlind()` + setInterval(5s) |
| SOS 徽章（紧急求助/没有报警）| 相同文案 + 红绿徽章 |
| 经纬度显示 | 相同 |
| 高德地图定位 | AMap JS API 2.0（沿用原 key/安全码），直接更新 marker |
| 2s 刷新实时画面 | `/snapshot?token=...&t=<时间戳>`（沿用原 token）|

---

## 三、如何运行

### 场景 A：日常开发预览（电脑）
直接双击 `index.html` 即可预览界面；协议测试双击 `tests/protocol.test.html`。
（此方式无蓝牙/定位能力，页面会给出提示。）

### 场景 B：手机完整功能（推荐）
Web Bluetooth 与 Geolocation 要求**安全上下文**（HTTPS 或 localhost）：

1. **GitHub Pages 部署（最简单）**
   - 把本目录推到 GitHub 仓库 → Settings → Pages → 开启
   - 手机 Chrome 打开 `https://<用户名>.github.io/<仓库>/webapp/`
   - ⚠️ 注意：HTTPS 页面调用 `http://39.106.216.80:8000` 会被「混合内容」策略拦截。
     解决办法二选一：
     - 给云服务器加 HTTPS 反代（如 Caddy：`detect.example.com { reverse_proxy localhost:8000 }`），
       然后在 `js/core/config.js` 改 `server.detectBaseUrl` 为 https 域名；
     - 或用场景 C。

2. **局域网 + Chrome 白名单（演示快速路径）**
   - 电脑上：`python -m http.server 8080`（在 webapp 目录下）
   - 手机 Chrome 打开 `http://<电脑IP>:8080/blind.html`
   - 手机 Chrome 地址栏进入 `chrome://flags/#unsafely-treat-insecure-origin-as-secure`，
     填入 `http://<电脑IP>:8080`，启用并重启浏览器
   - 此法同时解决安全上下文与混合内容两个问题，**演示日推荐**

### 场景 C：打包成 APK（已实现 ✅ 最终交付形态）

**两个独立手机 App 已经就绪**，本机**不需要**安装 Android Studio / JDK / Node.js——
用 GitHub Actions 云端免费编译：

```
app/android/
├── apk-blind/    智慧盲杖（盲人端）
│   └── WebView 壳 + 原生 BLE 桥（WebView 不支持 Web Bluetooth，由 Java 实现 NUS 扫描/连接/收发）
└── apk-family/   智慧盲杖·家属端
    └── WebView 壳（Firebase 轮询 + 高德地图 + 实时画面，无需桥）
```

**打包步骤（3 步）：**

1. 把仓库（含 `app/webapp/`、`app/android/`、`.github/workflows/build-apk.yml`）推到 GitHub
2. 打开仓库页面 → **Actions** 标签 → `Build Android APKs` 工作流会自动运行
   （约 3~5 分钟；也可手动点 *Run workflow* 触发）
3. 运行完成后，在该次运行页面底部 **Artifacts** 下载：
   - `SmartCane-blind-apk` → 盲人端 APK
   - `SmartCane-family-apk` → 家属端 APK
   - 传到手机直接安装（debug 签名，安装时允许「未知来源」即可）

**APK 相比浏览器版的优势：**

| 能力 | 浏览器版 | APK 版 |
|------|---------|--------|
| BLE 连接盲杖 | 需 HTTPS + Chrome + 系统选择器 | 原生蓝牙自动扫描连接（点一下即连） |
| 访问 http://39.106.216.80:8000 | 受混合内容策略限制 | 壳内已放开，**直连无障碍** |
| 离线打开界面 | 不行 | 界面打包在 APK 内，无网也能打开 |
| 桌面图标/独立 App | PWA 体验 | ✅ 两个独立图标，就是两个 App |

**技术说明：** APK 内 `WebView` 通过 `window.SmartCaneNative` 注入原生 BLE；
`ble.js` 检测到注入即走原生桥，浏览器打开则自动回落 Web Bluetooth，
**同一份 webapp 代码两栖通用**。识别服务器 `server.py` 已加 CORS 支持（APK/Web 的 fetch 必需）。

改了 `app/webapp/` 里的代码后重新推送，Actions 会自动把最新页面同步进 APK 重新编译。


---

## 四、重构收益

| 维度 | 原 App Inventor | 新 WebApp |
|------|----------------|-----------|
| 版本管理 | .aia 是二进制包，git 无法 diff | 纯文本，git diff / 代码评审友好 |
| 可测试性 | 无法单测积木 | `protocol.js` 纯函数，自带测试页 |
| 配置管理 | URL/IP 硬编码在积木里 | `config.js` 集中管理 + localStorage |
| 无障碍 | 默认小字体堆叠 | 大字体/大按钮/aria-live，TalkBack 友好 |
| 双端复用 | 两个独立工程 | 同一核心层，首页一键切换 |
| 修改协议 | 在 75KB 积木 XML 里找块 | 只改 `protocol.js` 一处 |

## 五、已知差异（如实说明）

1. **BLE 扫描方式**：
   - 浏览器版：Web Bluetooth 出于安全只允许「用户点击 → 系统选择器」；
   - APK 版：原生桥自动扫描 `SmartCane` 前缀设备并连接（接近原 App Inventor 体验）。
2. **浏览器 Geolocation 精度**略低于 App Inventor 原生 LocationSensor（APK 内同为系统定位，演示足够）。
3. **HTTPS 混合内容**（仅浏览器版）：若部署在 HTTPS，识别服务器需同样支持 HTTPS（见场景 B）；
   APK 版已在壳内放开限制，可直接访问 http 识别服务器。
4. `FALL:1`（30 秒倒计时）原 App 未做界面处理，本版同样只记录日志（保持一致）。
5. **识别服务器需重新部署一次**：`server.py` 新增了 CORS 支持（手机 App/浏览器 fetch 必需），
   云服务器上记得 `git pull` 并重启服务。

## 六、配置修改

`js/core/config.js` 顶部 `DEFAULTS`：
- 换服务器地址 → `server.detectBaseUrl`
- 换 Firebase → `firebase.baseUrl`
- 换轮询间隔 → `intervals`
- 换语音文案 → `tts`

改完刷新页面即生效（无需构建）。

