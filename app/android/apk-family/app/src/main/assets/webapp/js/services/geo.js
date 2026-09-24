/**
 * geo.js — GPS 定位服务（对应原 App 的 LocationSensor）
 * ---------------------------------------------------------------
 * 盲人端使用：持续监听位置变化，定时/事件触发时上传 Firebase。
 * 注：浏览器 Geolocation 在 HTTPS 下可用；精度略低于原生，满足演示需求。
 */
(function (global) {
  'use strict';

  var bus = global.SmartCane.bus;
  var EVENTS = global.SmartCane.EVENTS;

  var watchId = null;
  var last = null; // { latitude, longitude, accuracy, updatedAt }

  function isSupported() {
    return !!(global.navigator && global.navigator.geolocation);
  }

  /** 开始持续监听（原 LocationChanged 事件） */
  function start() {
    if (!isSupported()) {
      bus.emit(EVENTS.GEO_ERROR, { message: '当前浏览器不支持定位' });
      return;
    }
    if (watchId !== null) return;
    watchId = global.navigator.geolocation.watchPosition(
      function (pos) {
        last = {
          latitude: pos.coords.latitude,
          longitude: pos.coords.longitude,
          accuracy: pos.coords.accuracy,
          updatedAt: Date.now()
        };
        bus.emit(EVENTS.GEO_POSITION, last);
      },
      function (err) {
        bus.emit(EVENTS.GEO_ERROR, { message: err.message || '定位失败', code: err.code });
      },
      { enableHighAccuracy: true, maximumAge: 3000, timeout: 15000 }
    );
  }

  function stop() {
    if (watchId !== null && isSupported()) {
      global.navigator.geolocation.clearWatch(watchId);
      watchId = null;
    }
  }

  /** 最近一次位置（可能为 null） */
  function current() {
    return last ? JSON.parse(JSON.stringify(last)) : null;
  }

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.geo = {
    isSupported: isSupported,
    start: start,
    stop: stop,
    current: current
  };
})(window);
