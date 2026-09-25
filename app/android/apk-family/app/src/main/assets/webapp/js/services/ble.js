/**
 * ble.js — Web Bluetooth 服务（Nordic UART，对应原 App 的 BluetoothLE 扩展）
 * ---------------------------------------------------------------
 * 连接 → 订阅 TX(NOTIFY) → 按行拆分抛 ble:data → writeLine 发送指令。
 * Web Bluetooth 的 requestDevice 由用户手势触发，系统选择列表等价于原 ListPicker。
 */
(function (global) {
  'use strict';

  var bus = global.SmartCane.bus;
  var EVENTS = global.SmartCane.EVENTS;
  var config = global.SmartCane.config;
  var protocol = global.SmartCane.protocol;

  // Android APK 壳注入的原生 BLE 桥（WebView 不支持 Web Bluetooth 时由 Java 层实现）
  var native = (typeof global.SmartCaneNative !== 'undefined') ? global.SmartCaneNative : null;

  function isNativeAvailable() {
    return !!(native && typeof native.connect === 'function');
  }

  /** 原生桥回调接收器：BleBridge.java 通过 evaluateJavascript 调用以下方法 */
  global.__nativeBle = {
    onScanning: function () {
      bus.emit(EVENTS.BLE_CONNECTING, { name: '正在扫描盲杖…' });
    },
    onConnecting: function (name) {
      bus.emit(EVENTS.BLE_CONNECTING, { name: name || '盲杖' });
    },
    onReconnecting: function (attempt, delayMs) {
      state.nativeReconnecting = true;
      bus.emit(EVENTS.BLE_CONNECTING, {
        name: '重连中（第 ' + attempt + ' 次）…',
        reconnecting: true,
        attempt: attempt,
        delayMs: delayMs
      });
    },
    onConnected: function (name) {
      state.nativeConnected = true;
      state.nativeReconnecting = false;
      state.lineBuf = '';
      bus.emit(EVENTS.BLE_CONNECTED, { name: name || '盲杖', id: 'native' });
    },
    onLine: function (text) {
      var r = protocol.splitLines(state.lineBuf, String(text == null ? '' : text));
      state.lineBuf = r.rest;
      r.lines.forEach(function (line) { bus.emit(EVENTS.BLE_DATA, { line: line }); });
    },
    onDisconnected: function (payload) {
      state.nativeConnected = false;
      state.nativeReconnecting = false;
      state.lineBuf = '';
      bus.emit(EVENTS.BLE_DISCONNECTED, { reason: (payload && payload.reason) || 'disconnected' });
    },
    onError: function (msg) {
      var m = String(msg || '原生蓝牙错误');
      bus.emit(EVENTS.BLE_ERROR, { message: m });
      // 原生错误同样走 NOTIFY 弹 toast（与浏览器路径 fail() 行为一致，盲人端靠听觉感知）
      bus.emit(EVENTS.NOTIFY, { message: m, type: 'danger' });
    }
  };

  var state = {
    device: null, server: null,
    rxChar: null,      // 手机 → 盲杖
    txChar: null,      // 盲杖 → 手机
    lineBuf: '', textDecoder: null,
    nativeConnected: false,     // 原生桥（APK 内）连接标志
    nativeReconnecting: false   // 原生桥自动重连进行中
  };

  function isSupported() {
    return isNativeAvailable() ||
           !!(global.navigator && global.navigator.bluetooth);
  }

  function isSecureContext() {
    if (isNativeAvailable()) return true; // 原生桥不受浏览器安全上下文限制
    return global.isSecureContext === true ||
           global.location.protocol === 'https:' ||
           global.location.hostname === 'localhost' ||
           global.location.hostname === '127.0.0.1';
  }


  /** 打开系统蓝牙选择器并连接（必须在点击等用户手势中调用） */
  function connect() {
    // —— 路径 A：APK 内走原生 BLE 桥（WebView 不支持 Web Bluetooth）——
    if (isNativeAvailable()) {
      var cfgN = config.get().ble;
      bus.emit(EVENTS.BLE_CONNECTING, { name: '正在扫描盲杖…' });
      try {
        native.connect(
          (cfgN.serviceUuid || '').trim(),
          (cfgN.rxUuid || '').trim(),
          (cfgN.txUuid || '').trim(),
          cfgN.deviceNamePrefix || ''
        );
        return Promise.resolve();
      } catch (e) {
        return fail('原生蓝牙启动失败：' + ((e && e.message) || e));
      }
    }

    // —— 路径 B：浏览器走 Web Bluetooth ——
    if (!isSupported()) {
      return fail('当前浏览器不支持 Web Bluetooth，请用 Android 版 Chrome 打开（HTTPS 页面）。');
    }
    if (!isSecureContext()) {
      return fail('蓝牙功能需要 HTTPS（或 localhost）页面，请按 README「部署」说明访问。');
    }

    var cfg = config.get().ble;
    var uuid = (cfg.serviceUuid || '').trim().toLowerCase();

    bus.emit(EVENTS.BLE_CONNECTING, { name: '' });

    return global.navigator.bluetooth.requestDevice({
      filters: [
        { services: [uuid] },
        { namePrefix: cfg.deviceNamePrefix }
      ],
      optionalServices: [uuid]
    }).then(function (device) {
      state.device = device;
      device.addEventListener('gattserverdisconnected', onDisconnected);
      bus.emit(EVENTS.BLE_CONNECTING, { name: device.name || '未知设备' });
      return device.gatt.connect();
    }).then(function (server) {
      state.server = server;
      return server.getPrimaryService(uuid);
    }).then(function (service) {
      return Promise.all([
        service.getCharacteristic((cfg.rxUuid || '').toLowerCase()),
        service.getCharacteristic((cfg.txUuid || '').toLowerCase())
      ]);
    }).then(function (chars) {
      state.rxChar = chars[0];
      state.txChar = chars[1];
      state.txChar.addEventListener('characteristicvaluechanged', onNotify); // 原 RegisterForStrings
      return state.txChar.startNotifications();
    }).then(function () {
      bus.emit(EVENTS.BLE_CONNECTED, {
        name: (state.device && state.device.name) || '未知设备',
        id: state.device && state.device.id
      });
    }).catch(function (err) {
      if (err && /cancel|User cancelled|用户取消/i.test(err.message || '')) {
        bus.emit(EVENTS.BLE_DISCONNECTED, { reason: 'user-cancelled' });
        return;
      }
      bus.emit(EVENTS.BLE_ERROR, { message: err.message || String(err) });
      throw err;
    });
  }

  function fail(msg) {
    bus.emit(EVENTS.BLE_ERROR, { message: msg });
    bus.emit(EVENTS.NOTIFY, { message: msg, type: 'danger' });
    return Promise.reject(new Error(msg));
  }

  /** 断开连接（对应原 Disconnect） */
  function disconnect() {
    if (isNativeAvailable()) {
      // 原生层会恰好回调一次 onDisconnected（含本来就没连上的情况），此处不再重复抛事件
      try {
        native.disconnect();
      } catch (e) {
        state.nativeConnected = false;
        bus.emit(EVENTS.BLE_DISCONNECTED, { reason: 'disconnected' });
      }
      return;
    }
    if (state.device && state.device.gatt && state.device.gatt.connected) {
      state.device.gatt.disconnect(); // 触发 onDisconnected
    } else {
      onDisconnected();
    }
  }

  function onDisconnected() {
    var had = !!state.device;
    if (state.txChar) {
      try { state.txChar.removeEventListener('characteristicvaluechanged', onNotify); } catch (e) {}
    }
    state.device = null; state.server = null;
    state.rxChar = null; state.txChar = null; state.lineBuf = '';
    if (had) bus.emit(EVENTS.BLE_DISCONNECTED, { reason: 'disconnected' });
  }

  /** NOTIFY 数据 → 文本 → 按行拆分 → 逐行 emit */
  function onNotify(event) {
    var text = decode(event.target.value);
    var r = protocol.splitLines(state.lineBuf, text);
    state.lineBuf = r.rest;
    r.lines.forEach(function (line) { bus.emit(EVENTS.BLE_DATA, { line: line }); });
  }

  function decode(dataView) {
    if (typeof TextDecoder !== 'undefined') {
      if (!state.textDecoder) state.textDecoder = new TextDecoder('utf-8');
      return state.textDecoder.decode(dataView);
    }
    var out = '';
    for (var i = 0; i < dataView.byteLength; i++) out += String.fromCharCode(dataView.getUint8(i));
    try { return decodeURIComponent(escape(out)); } catch (e) { return out; }
  }

  /** 手机 → 盲杖 发送一行指令（带换行，对应原 WriteStrings） */
  function writeLine(text) {
    if (isNativeAvailable()) {
      if (!state.nativeConnected) {
        var msg = state.nativeReconnecting
          ? '盲杖连接已断开，正在自动重连，请稍候…'
          : '请先连接智能盲杖';
        bus.emit(EVENTS.NOTIFY, { message: msg, type: 'warning' });
        return Promise.reject(new Error('BLE not connected'));
      }
      try {
        native.write(text);
        return Promise.resolve();
      } catch (e) {
        bus.emit(EVENTS.BLE_ERROR, { message: '指令发送失败：' + ((e && e.message) || e) });
        return Promise.reject(e);
      }
    }
    if (!state.rxChar) {
      bus.emit(EVENTS.NOTIFY, { message: '请先连接智能盲杖', type: 'warning' });
      return Promise.reject(new Error('BLE not connected'));
    }
    return state.rxChar.writeValue(encodeUtf8(text + '\n')).catch(function (err) {
      bus.emit(EVENTS.BLE_ERROR, { message: '指令发送失败：' + (err.message || err) });
      throw err;
    });
  }

  function encodeUtf8(str) {
    if (typeof TextEncoder !== 'undefined') return new TextEncoder().encode(str);
    var bytes = [];
    for (var i = 0; i < str.length; i++) {
      var code = str.charCodeAt(i);
      if (code < 0x80) bytes.push(code);
      else if (code < 0x800) bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
      else bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    }
    return new Uint8Array(bytes);
  }

  function isConnected() {
    if (isNativeAvailable()) return !!state.nativeConnected;
    return !!(state.device && state.device.gatt && state.device.gatt.connected);
  }

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.ble = {
    isSupported: isSupported,
    isSecureContext: isSecureContext,
    connect: connect,
    disconnect: disconnect,
    writeLine: writeLine,
    isConnected: isConnected
  };
})(window);
