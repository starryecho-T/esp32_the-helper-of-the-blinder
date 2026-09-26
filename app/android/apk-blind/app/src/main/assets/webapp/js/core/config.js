/**
 * config.js — 全局配置中心
 * ---------------------------------------------------------------
 * 所有可变配置（服务器地址、UUID、轮询间隔）集中于此，
 * 支持运行时修改并持久化到 localStorage。
 * 原 App Inventor 版本中这些值硬编码在积木块里，难以修改。
 */
(function (global) {
  'use strict';

  var STORAGE_KEY = 'smartcane.webapp.config.v1';

  /** 默认配置（与原 App 保持一致） */
  var DEFAULTS = {
    // —— BLE（Nordic UART Service，见 protocol/ble-protocol.md）——
    ble: {
      deviceNamePrefix: 'SmartCane',                         // 盲杖蓝牙名前缀
      serviceUuid: '6e400001-b5a3-f393-e0a9-e50e24dcca9e',   // NUS 主服务
      rxUuid:      '6e400002-b5a3-f393-e0a9-e50e24dcca9e',   // 手机 → 盲杖（WRITE）
      txUuid:      '6e400003-b5a3-f393-e0a9-e50e24dcca9e',   // 盲杖 → 手机（NOTIFY）
      scanTimeoutMs: 15000                                   // 扫描超时
    },
    // —— 云端识别服务器（YOLOv8 交通灯识别，见 app/traffic_light_server/server.py）——
    server: {
      detectBaseUrl: 'http://39.106.216.80:8000',  // /detect /snapshot 所在服务器
      viewToken: 'Jxt7HvzaZVfhkF1D'                // 家属端查看画面的 VIEW_TOKEN
    },
    // —— 双端通信（自建 /fb 接口，格式与 Firebase RTDB REST 完全兼容）——
    // 原来走境外 Firebase 经常连不上（Error 1101），现改用自有云服务器。
    // firebase.js 只认 baseUrl + blindId，故无需改任何其他代码。
    firebase: {
      baseUrl: 'http://39.106.216.80:8000/fb',
      blindId: 'blind001'                          // 盲人节点 ID
    },
    // —— 轮询间隔（毫秒）——
    intervals: {
      gpsUploadMs: 10000,      // 盲人端：GPS 定时上传（原 gps计时器 10s）
      familyPollMs: 5000,      // 家属端：Firebase 轮询（原 计时器1 5s）
      snapshotMs: 2000         // 家属端：实时画面刷新（原 画面刷新 2s）
    },
    // —— 交通灯语音播报文案（与原 App 一致）——
    tts: {
      enabled: true,
      redText: '检测到红灯，请停止前进',
      yellowText: '检测到黄灯，请注意',
      greenText: '检测到绿灯，可以通行',
      noneText: '未检测到交通灯',
      // —— 障碍物检测播报（barrier.js / blind 端 runBarrierDetect）——
      barrierPrefix: '前方障碍：',          // 有障碍时前缀 + 摘要 + 后缀
      barrierSuffix: '，请注意避让',
      barrierNoneText: '未检测到障碍物'     // 无障碍时整句播报
    }
  };

  /** 深合并工具（不引入任何第三方库） */
  function deepMerge(base, override) {
    var out = {};
    Object.keys(base).forEach(function (k) {
      var b = base[k];
      var o = override ? override[k] : undefined;
      if (b && typeof b === 'object' && !Array.isArray(b)) {
        out[k] = deepMerge(b, o && typeof o === 'object' ? o : {});
      } else {
        out[k] = (o === undefined || o === null) ? b : o;
      }
    });
    return out;
  }

  function readStorage() {
    try {
      var raw = global.localStorage && global.localStorage.getItem(STORAGE_KEY);
      return raw ? JSON.parse(raw) : {};
    } catch (e) { return {}; }
  }

  var config = deepMerge(DEFAULTS, readStorage());

  var configApi = {
    /** 读取配置（返回副本，防止外部误改） */
    get: function () {
      return JSON.parse(JSON.stringify(config));
    },

    /** 批量更新配置（传入与 DEFAULTS 同构的部分对象）并持久化 */
    update: function (partial) {
      config = deepMerge(config, partial);
      try {
        global.localStorage.setItem(STORAGE_KEY, JSON.stringify(config));
      } catch (e) { /* 隐私模式下可能失败，忽略 */ }
      return configApi.get();
    },

    /** 恢复默认配置 */
    reset: function () {
      config = JSON.parse(JSON.stringify(DEFAULTS));
      try { global.localStorage.removeItem(STORAGE_KEY); } catch (e) {}
      return configApi.get();
    },

    /** 便捷取值：Config.path('server.detectBaseUrl') */
    path: function (dotPath) {
      return dotPath.split('.').reduce(function (obj, key) {
        return obj ? obj[key] : undefined;
      }, config);
    }
  };

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.config = configApi;
})(window);
