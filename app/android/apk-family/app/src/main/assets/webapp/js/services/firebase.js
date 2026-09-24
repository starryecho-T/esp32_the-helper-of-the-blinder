/**
 * firebase.js — Firebase 实时数据库 REST 封装
 * ---------------------------------------------------------------
 * 对应原 App 的 Web 组件（PutText / Get + JsonTextDecode）。
 * 数据结构（与原 App 完全一致，家属端可直接读取）：
 *   /blind001/location.json = { "latitude": 30.xx, "longitude": 120.xx }
 *   /blind001/sos.json      = true | false
 */
(function (global) {
  'use strict';

  var bus = global.SmartCane.bus;
  var EVENTS = global.SmartCane.EVENTS;
  var config = global.SmartCane.config;

  function base() {
    var cfg = config.get().firebase;
    return cfg.baseUrl.replace(/\/+$/, '') + '/' + cfg.blindId;
  }

  /** 上报 SOS：true=报警中 false=已恢复 */
  function putSos(active) {
    return put('/sos.json', active)
      .then(function () { bus.emit(EVENTS.FB_UPLOADED, { path: '/sos.json', ok: true }); })
      .catch(function (err) {
        bus.emit(EVENTS.FB_UPLOADED, { path: '/sos.json', ok: false, error: err.message });
        throw err;
      });
  }

  /** 上报位置（对应原「立即上传GPS」） */
  function putLocation(latitude, longitude) {
    var body = JSON.stringify({ latitude: latitude, longitude: longitude });
    return put('/location.json', body)
      .then(function () { bus.emit(EVENTS.FB_UPLOADED, { path: '/location.json', ok: true }); })
      .catch(function (err) {
        bus.emit(EVENTS.FB_UPLOADED, { path: '/location.json', ok: false, error: err.message });
        throw err;
      });
  }

  /** 读取整个盲人节点：{ sos, location: { latitude, longitude } } */
  function fetchBlind() {
    return global.fetch(base() + '.json')
      .then(function (res) {
        if (!res.ok) throw new Error('HTTP ' + res.status);
        return res.json();
      });
  }

  function put(path, body) {
    return global.fetch(base() + path, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: typeof body === 'string' ? body : JSON.stringify(body)
    }).then(function (res) {
      if (!res.ok) throw new Error('HTTP ' + res.status);
      return res.json();
    });
  }

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.firebase = {
    putSos: putSos,
    putLocation: putLocation,
    fetchBlind: fetchBlind
  };
})(window);
