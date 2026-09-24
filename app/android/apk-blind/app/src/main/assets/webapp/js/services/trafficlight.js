/**
 * trafficlight.js — 交通灯识别服务
 * ---------------------------------------------------------------
 * 对接 app/traffic_light_server/server.py：
 *   GET  /detect   → 文本 "COLOR|NN%"（RED/YELLOW/GREEN/NONE）
 *   GET  /snapshot?t=<ms>&token=xxx → 最新一帧 JPEG（家属端实时画面）
 * 前哨(ESP32-CAM)每 30s 主动推帧到云服务器，App 无需直连摄像头。
 */
(function (global) {
  'use strict';

  var bus = global.SmartCane.bus;
  var EVENTS = global.SmartCane.EVENTS;
  var config = global.SmartCane.config;

  /**
   * 请求一次识别。
   * @returns {Promise<{color:string, confidence:string}>}
   */
  function detect() {
    var cfg = config.get().server;
    var url = cfg.detectBaseUrl.replace(/\/+$/, '') + '/detect';
    return global.fetch(url, { cache: 'no-store' })
      .then(function (res) {
        if (!res.ok) {
          return res.text().then(function (t) {
            throw new Error('HTTP ' + res.status + ' ' + t.slice(0, 120));
          });
        }
        return res.text();
      })
      .then(function (text) {
        var result = parseDetectResponse(text);
        bus.emit(EVENTS.LIGHT_RESULT, result);
        return result;
      })
      .catch(function (err) {
        bus.emit(EVENTS.LIGHT_ERROR, { message: err.message || String(err) });
        throw err;
      });
  }

  /** 解析 "GREEN|87%" 风格响应；容错处理纯色名或 JSON。 */
  function parseDetectResponse(text) {
    var raw = (text || '').trim();
    var m = raw.match(/^(RED|YELLOW|GREEN|NONE)\s*\|\s*([0-9]+)\s*%?$/i);
    if (m) {
      return { color: m[1].toUpperCase(), confidence: m[2] + '%' };
    }
    // 容错：裸色名
    var up = raw.toUpperCase();
    if (up === 'RED' || up === 'YELLOW' || up === 'GREEN' || up === 'NONE') {
      return { color: up, confidence: '--' };
    }
    // 容错：JSON（预留）
    try {
      var j = JSON.parse(raw);
      if (j && j.color) return { color: String(j.color).toUpperCase(), confidence: j.confidence || '--' };
    } catch (e) { /* not json */ }
    throw new Error('无法解析识别结果：' + raw.slice(0, 80));
  }

  /** 家属端实时画面 URL（带时间戳防缓存，对应原 Image.Picture） */
  function snapshotUrl() {
    var cfg = config.get().server;
    var base = cfg.detectBaseUrl.replace(/\/+$/, '') + '/snapshot';
    return base + '?token=' + encodeURIComponent(cfg.viewToken) + '&t=' + Date.now();
  }

  /** 服务器连通性检查 */
  function health() {
    var cfg = config.get().server;
    return global.fetch(cfg.detectBaseUrl.replace(/\/+$/, '') + '/health', { cache: 'no-store' })
      .then(function (res) { return res.ok; })
      .catch(function () { return false; });
  }

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.trafficLight = {
    detect: detect,
    parseDetectResponse: parseDetectResponse,
    snapshotUrl: snapshotUrl,
    health: health
  };
})(window);
