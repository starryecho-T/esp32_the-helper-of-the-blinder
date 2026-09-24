/**
 * store.js — 极简响应式状态管理
 * ---------------------------------------------------------------
 * 集中保存两端的运行时状态；UI 通过 subscribe 订阅变更后自行刷新。
 * 数据流：服务 → store.set() → 通知订阅者 → DOM 更新（单向）。
 */
(function (global) {
  'use strict';

  var state = {
    // —— 连接 ——
    ble: {
      scanning: false,
      devices: [],          // 扫描到的设备 [{id, name}]
      connecting: false,
      connected: false,
      deviceName: '',
      lastError: ''
    },
    // —— 盲杖实时状态（来自周期上报，protocol.js 解析）——
    cane: {
      dist: null,           // 低位距离 cm
      high: null,           // 高位距离 cm
      type: '',             // SAFE / LARGE / LOW / HIGH
      level: null,          // 0安全 1提醒 2警告 3危险
      mode: '',             // NORMAL / SILENT / NIGHT（或 0/1/2）
      batt: null,           // 电量 %
      alarm: 'NORMAL',      // NORMAL / MANUAL / FALL
      lat: null,
      lng: null,
      updatedAt: 0
    },
    // —— 交通灯识别 ——
    light: {
      color: '',            // RED / YELLOW / GREEN / NONE
      confidence: '',
      busy: false,
      lastResultText: '识别结果：等待图片',
      lastConfidenceText: '置信度：--'
    },
    // —— GPS ——
    geo: {
      latitude: null,
      longitude: null,
      accuracy: null,
      updatedAt: 0
    },
    // —— SOS（盲人端上报状态 / 家属端观察状态）——
    sos: {
      active: false,
      reason: '',           // FALL / MANUAL / ''
      updatedAt: 0
    }
  };

  var subscribers = [];

  var store = {
    /** 读取 state（直接引用，只读约定） */
    get: function () { return state; },

    /**
     * 局部更新：store.set('cane', { batt: 85 })
     * @param {string} section 顶层键名
     * @param {object} partial 要合并的字段
     */
    set: function (section, partial) {
      if (!state[section]) state[section] = {};
      Object.keys(partial).forEach(function (k) {
        state[section][k] = partial[k];
      });
      store._notify(section, partial);
    },

    /** 订阅变更：返回取消订阅函数 */
    subscribe: function (fn) {
      subscribers.push(fn);
      return function () {
        var i = subscribers.indexOf(fn);
        if (i >= 0) subscribers.splice(i, 1);
      };
    },

    _notify: function (section, changed) {
      subscribers.slice().forEach(function (fn) {
        try { fn(section, changed, state); }
        catch (e) { console.error('[store] subscriber error:', e); }
      });
    }
  };

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.store = store;
})(window);
