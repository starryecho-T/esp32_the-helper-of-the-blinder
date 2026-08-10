# ESP32 The Helper of the Blinder

用于智能盲杖（ESP32-S3）、探路设备（ESP32-C3）和 MIT App Inventor 手机应用协同开发的项目仓库。

## 目录说明

- `docs/`：需求、计划和项目文档。
- `firmware/`：ESP32-S3、ESP32-C3 及共享固件代码。
- `app/`：MIT App Inventor 的当前工程、发布版本和资源。
- `protocol/`：ESP-NOW、BLE、Wi-Fi 等通信协议。
- `hardware/`：引脚定义、接线、原理图和物料清单。
- `mechanical/`：盲杖、探路设备及装配相关的结构文件。
- `algorithms/`：障碍物、跌倒和交通灯识别算法资料或代码。
- `tests/`：硬件、通信、App 与系统集成测试。

## 协作约定

开始工作前先同步 `main`，每项任务使用独立分支并通过 Pull Request 合并。不要提交密钥、令牌、个人数据或本地构建产物。

## 快速开始

1. 在 `firmware/cane-s3/` 或 `firmware/scout-c3/` 中开发对应设备固件。
2. 在 `protocol/` 中先约定设备与 App 的消息格式。
3. 将 App Inventor 主工程放在 `app/current/`，发布包放在 `app/releases/`。
4. 将接线图、BOM 和测试记录分别放入对应目录。
