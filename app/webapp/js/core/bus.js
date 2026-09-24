/**
 * bus.js — 轻量事件总线（发布/订阅）
 * ---------------------------------------------------------------
 * 解耦「服务层」与「UI 层」：服务只管发事件，页面只管订阅。
 * 替代原 App Inventor 中积木块之间的隐式耦合。
 */
(function (global) {
  'use strict';

  var listeners = {}; // { eventName: [fn, ...] }

  var bus = {
    /** 订阅事件，返回取消订阅函数 */
    on: function (eventName, handler) {
      (listeners[eventName] = listeners[eventName] || []).push(handler);
      return function off() {
        var arr = listeners[eventName] || [];
        var idx = arr.indexOf(handler);
        if (idx >= 0) arr.splice(idx, 1);
      };
    },

    /** 发布事件（同步） */
    emit: function (eventName, payload) {
      (listeners[eventName] || []).slice().forEach(function (handler) {
        try {
          handler(payload);
        } catch (e) {
          console.error('[bus] handler error for "' + eventName + '":', e);
        }
      });
    },

    /** 清空所有订阅（测试用） */
    clear: function () { listeners = {}; }
  };

  /** 全局事件名常量，避免魔法字符串 */
  var EVENTS = {
    // BLE 连接生命周期
    BLE_SCAN_STARTED:   'ble:scan-started',
    BLE_DEVICE_FOUND:   'ble:device-found',
    BLE_SCAN_DONE:      'ble:scan-done',
    BLE_CONNECTING:     'ble:connecting',
    BLE_CONNECTED:      'ble:connected',
    BLE_DISCONNECTED:   'ble:disconnected',
    BLE_DATA:           'ble:data',           // 盲杖发来的原始文本（已按行拆分）
    BLE_ERROR:          'ble:error',
    // 盲杖状态（protocol.js 解析后）
    CANE_STATUS:        'cane:status',        // 周期状态包解析结果
    CANE_EVENT:         'cane:event',         // 事件消息（MODE:xx / FALL:xx / ALARM:xx / CAMERA:CAPTURE）
    // 交通灯识别
    LIGHT_RESULT:       'light:result',       // { color, confidence }
    LIGHT_ERROR:        'light:error',
    // GPS
    GEO_POSITION:       'geo:position',       // { latitude, longitude, accuracy }
    GEO_ERROR:          'geo:error',
    // Firebase 上传结果
    FB_UPLOADED:        'fb:uploaded',        // { path, ok }
    // UI 通知
    NOTIFY:             'ui:notify',          // { message, type: 'info|success|warning|danger' }
    VOICE:              'ui:voice'            // { message } 语音播报请求
  };

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.bus = bus;
  global.SmartCane.EVENTS = EVENTS;
})(window);
