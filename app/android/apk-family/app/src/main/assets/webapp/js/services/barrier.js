/**
 * barrier.js — 障碍物检测服务
 * ---------------------------------------------------------------
 * 对接 app/traffic_light_server/server.py 的 /barrier 接口：
 *   GET /barrier → JSON（4 大类计数 + 明细 + 中文摘要 + 回传盲杖的 cane 串）
 *
 * 触发链路：盲杖 BLE 发 barrierdetect → 本服务 GET /barrier
 *   → 云端第二个 YOLO 模型（yolov8s）检测 行人/车辆/动物/静态设施
 *   → App 播报 summaryZh → BLE 回传 cane（BARRIER:PED2,VEH1 / BARRIER:NONE）
 *
 * 与 trafficlight.js 同帧源：前哨 ESP32-CAM 周期推流，服务器缓存最新一帧。
 */
(function (global) {
  'use strict';

  var bus = global.SmartCane.bus;
  var EVENTS = global.SmartCane.EVENTS;
  var config = global.SmartCane.config;

  var CATEGORY_ORDER = ['PEDESTRIAN', 'VEHICLE', 'ANIMAL', 'FACILITY'];
  var CATEGORY_CN    = { PEDESTRIAN: '行人', VEHICLE: '车辆', ANIMAL: '动物', FACILITY: '静态设施' };
  var CATEGORY_UNIT  = { PEDESTRIAN: '名', VEHICLE: '辆', ANIMAL: '只', FACILITY: '处' };
  var CATEGORY_SHORT = { PEDESTRIAN: 'PED', VEHICLE: 'VEH', ANIMAL: 'ANI', FACILITY: 'FAC' };

  /**
   * 请求一次障碍物检测。
   * @returns {Promise<{counts:Object,total:number,objects:Array,summaryZh:string,cane:string}>}
   */
  function detect() {
    var cfg = config.get().server;
    var url = cfg.detectBaseUrl.replace(/\/+$/, '') + '/barrier';
    return global.fetch(url, { cache: 'no-store' })
      .then(function (res) {
        if (!res.ok) {
          return res.text().then(function (t) {
            throw new Error('HTTP ' + res.status + ' ' + t.slice(0, 120));
          });
        }
        return res.json();
      })
      .then(function (json) {
        var result = parseBarrierResponse(json);
        bus.emit(EVENTS.BARRIER_RESULT, result);
        return result;
      })
      .catch(function (err) {
        bus.emit(EVENTS.BARRIER_ERROR, { message: err.message || String(err) });
        throw err;
      });
  }

  /** 校验/规整服务器 JSON；summary_zh / cane 缺失时按 counts 本地兜底生成。 */
  function parseBarrierResponse(json) {
    if (!json || typeof json !== 'object') {
      throw new Error('响应不是 JSON 对象');
    }
    var counts = {};
    CATEGORY_ORDER.forEach(function (c) {
      counts[c] = Math.max(0, parseInt(json.counts && json.counts[c], 10) || 0);
    });
    var objects = Array.isArray(json.objects) ? json.objects : [];
    var total = parseInt(json.total, 10);
    if (isNaN(total)) {
      total = CATEGORY_ORDER.reduce(function (sum, c) { return sum + counts[c]; }, 0);
    }
    var summaryZh = json.summary_zh || summarizeZh(counts);
    var cane = json.cane || toCane(counts);
    return { counts: counts, total: total, objects: objects, summaryZh: summaryZh, cane: cane };
  }

  /** 本地生成中文摘要（服务器 summary_zh 缺失时的兜底），如「2名行人、1辆车」。 */
  function summarizeZh(counts) {
    var parts = [];
    CATEGORY_ORDER.forEach(function (c) {
      if (counts[c] > 0) parts.push(counts[c] + CATEGORY_UNIT[c] + CATEGORY_CN[c]);
    });
    return parts.length ? parts.join('、') : '未检测到障碍物';
  }

  /** 本地生成回传盲杖的紧凑格式（服务器 cane 缺失时的兜底）。 */
  function toCane(counts) {
    var parts = [];
    CATEGORY_ORDER.forEach(function (c) {
      if (counts[c] > 0) parts.push(CATEGORY_SHORT[c] + counts[c]);
    });
    return parts.length ? 'BARRIER:' + parts.join(',') : 'BARRIER:NONE';
  }

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.barrier = {
    CATEGORY_ORDER: CATEGORY_ORDER,
    CATEGORY_CN: CATEGORY_CN,
    detect: detect,
    parseBarrierResponse: parseBarrierResponse,
    summarizeZh: summarizeZh,
    toCane: toCane
  };
})(window);
