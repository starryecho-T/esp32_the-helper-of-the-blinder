/**
 * protocol.js — BLE 协议解析器（纯函数，可单元测试）
 * ---------------------------------------------------------------
 * 依据 protocol/ble-protocol.md（v2.0）与 docs/演示说明.md §5 实现：
 *
 * 盲杖 → 手机（TX/NOTIFY）
 *   周期上报（每 500ms，字段可能只有部分出现）：
 *     DIST:85,HIGH:120,TYPE:LARGE,LEVEL:1,MODE:0,BATT:85,ALARM:NORMAL,
 *     FALLST:0,ENC:42,ACC:1.02,MPU:1,LAT:30.123,LNG:120.654,LIGHT:无
 *   事件消息：
 *     MODE:NORMAL / MODE:SILENT / MODE:NIGHT
 *     CAMERA:CAPTURE
 *     barrierdetect / BARRIER:DETECT    障碍物检测请求（大小写不限）
 *     ALARM:MANUAL / ALARM:CANCEL
 *     FALL:1 / FALL:CANCELLED / FALL:CONFIRMED
 *
 * 手机 → 盲杖（RX/WRITE）
 *     MODE:0 / MODE:1 / MODE:2          切换三种模式
 *     ALARM:CANCEL                      远程取消报警
 *     CAMERA:CAPTURE                    触发拍照
 *     RED / YELLOW / GREEN / NONE       交通灯识别结果
 *     BARRIER:PED2,VEH1 / BARRIER:NONE  障碍物检测结果（行人/车辆/动物/设施数量）
 *     STATUS                            查询状态
 */
(function (global) {
  'use strict';

  /** 消息种类 */
  var KIND = {
    STATUS: 'status',   // 周期状态包
    EVENT:  'event',    // 事件消息
    UNKNOWN: 'unknown'
  };

  /** 事件类型（cane:event 载荷里的 type 字段） */
  var EVENT_TYPE = {
    MODE_SWITCH:     'mode-switch',      // { type, mode: 'NORMAL'|'SILENT'|'NIGHT' }
    CAMERA_CAPTURE:  'camera-capture',   // { type }
    BARRIER_DETECT:  'barrier-detect',   // { type } 盲杖请求障碍物检测
    ALARM_MANUAL:    'alarm-manual',     // { type }
    ALARM_CANCEL:    'alarm-cancel',     // { type }
    FALL_DETECTED:   'fall-detected',    // { type }  30s 倒计时开始
    FALL_CANCELLED:  'fall-cancelled',   // { type }
    FALL_CONFIRMED:  'fall-confirmed'    // { type }  30s 到，确认报警
  };

  /** 模式数字 ↔ 名称 */
  var MODE_BY_INDEX = ['NORMAL', 'SILENT', 'NIGHT'];   // MODE:0/1/2（演示说明 §5.2）
  var MODE_INDEX_BY_NAME = { NORMAL: 0, SILENT: 1, NIGHT: 2 };

  /**
   * 解析一行盲杖消息。
   * @param {string} line 单行文本（不带换行符）
   * @returns {{kind:string, status?:object, event?:object, raw:string}}
   */
  function parseLine(line) {
    var raw = (line || '').trim();
    if (!raw) return { kind: KIND.UNKNOWN, raw: raw };

    // —— 1. 事件消息（带冒号的关键字，或纯灯色回显）——
    var evt = parseEvent(raw);
    if (evt) return { kind: KIND.EVENT, event: evt, raw: raw };

    // —— 2. 周期状态包（KEY:VALUE 逗号分隔）——
    var status = parseStatus(raw);
    if (status) return { kind: KIND.STATUS, status: status, raw: raw };

    return { kind: KIND.UNKNOWN, raw: raw };
  }

  /** 识别事件消息；不是事件返回 null */
  function parseEvent(text) {
    // —— 障碍物检测请求：固件可能发 barrierdetect / BARRIER:DETECT 等变体（大小写不限）——
    var up = text.toUpperCase();
    if (up === 'BARRIERDETECT' || up === 'BARRIER:DETECT' || up === 'BARRIER:REQ') {
      return { type: EVENT_TYPE.BARRIER_DETECT };
    }
    switch (text) {
      case 'MODE:NORMAL':  return { type: EVENT_TYPE.MODE_SWITCH, mode: 'NORMAL' };
      case 'MODE:SILENT':  return { type: EVENT_TYPE.MODE_SWITCH, mode: 'SILENT' };
      case 'MODE:NIGHT':   return { type: EVENT_TYPE.MODE_SWITCH, mode: 'NIGHT' };
      case 'CAMERA:CAPTURE': return { type: EVENT_TYPE.CAMERA_CAPTURE };
      case 'ALARM:MANUAL': return { type: EVENT_TYPE.ALARM_MANUAL };
      case 'ALARM:CANCEL': return { type: EVENT_TYPE.ALARM_CANCEL };
      case 'FALL:1':       return { type: EVENT_TYPE.FALL_DETECTED };
      case 'FALL:CANCELLED':  return { type: EVENT_TYPE.FALL_CANCELLED };
      case 'FALL:CONFIRMED':  return { type: EVENT_TYPE.FALL_CONFIRMED };
      default: return null;
    }
  }

  /** 解析 KEY:VALUE,... 状态包；不像状态包返回 null */
  function parseStatus(text) {
    if (text.indexOf(':') < 0) return null;
    var parts = text.split(',');
    var map = {};
    var hits = 0;
    parts.forEach(function (p) {
      var seg = p.trim();
      var i = seg.indexOf(':');
      if (i <= 0) return;
      var key = seg.slice(0, i).trim().toUpperCase();
      var val = seg.slice(i + 1).trim();
      if (key) { map[key] = val; hits++; }
    });
    if (!hits) return null;

    var s = {
      dist:  num(map.DIST),
      high:  num(map.HIGH),
      type:  map.TYPE || '',
      level: num(map.LEVEL),
      mode:  normalizeMode(map.MODE),
      batt:  num(map.BATT),
      alarm: map.ALARM || '',
      fallst: num(map.FALLST),
      acc:   float(map.ACC),
      lat:   float(map.LAT),
      lng:   float(map.LNG),
      light: map.LIGHT || ''
    };
    return s;
  }

  /** MODE 字段可能是 0/1/2 或名称 */
  function normalizeMode(v) {
    if (v === undefined || v === null || v === '') return '';
    if (MODE_INDEX_BY_NAME[v] !== undefined) return v;
    var idx = parseInt(v, 10);
    return MODE_BY_INDEX[idx] !== undefined ? MODE_BY_INDEX[idx] : v;
  }

  function num(v) {
    if (v === undefined || v === null || v === '') return null;
    var n = parseInt(v, 10);
    return isNaN(n) ? null : n;
  }
  function float(v) {
    if (v === undefined || v === null || v === '') return null;
    var f = parseFloat(v);
    return isNaN(f) ? null : f;
  }

  /**
   * 将 notify 收到的字节/文本按行拆分（保留不完整行等待下次拼接）。
   * @param {TextDecoderLike|null} decoder 见 ble.js 内部说明，这里只处理字符串
   * @param {string} buffer 之前未消费的残包
   * @param {string} chunk 本次新收到的文本
   * @returns {{lines:string[], rest:string}} 完整行数组 + 剩余残包
   */
  function splitLines(buffer, chunk) {
    var data = (buffer || '') + (chunk || '');
    var lines = data.split(/\r\n|\n|\r/);
    var rest = lines.pop() || '';   // 最后一段可能不完整
    return { lines: lines.filter(function (l) { return l.trim() !== ''; }), rest: rest };
  }

  // —— 手机 → 盲杖指令构造 ——
  function cmdSetMode(indexOrName) {
    if (typeof indexOrName === 'number') return 'MODE:' + indexOrName;
    var idx = MODE_INDEX_BY_NAME[indexOrName];
    return 'MODE:' + (idx !== undefined ? idx : indexOrName);
  }
  function cmdAlarmCancel()   { return 'ALARM:CANCEL'; }
  function cmdCameraCapture() { return 'CAMERA:CAPTURE'; }
  function cmdLightResult(color) { return color; }        // RED / YELLOW / GREEN / NONE
  function cmdBarrierResult(caneText) { return caneText || 'BARRIER:NONE'; }  // BARRIER:PED2,VEH1
  function cmdQueryStatus()   { return 'STATUS'; }

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.protocol = {
    KIND: KIND,
    EVENT_TYPE: EVENT_TYPE,
    MODE_BY_INDEX: MODE_BY_INDEX,
    MODE_INDEX_BY_NAME: MODE_INDEX_BY_NAME,
    parseLine: parseLine,
    parseEvent: parseEvent,
    parseStatus: parseStatus,
    splitLines: splitLines,
    cmdSetMode: cmdSetMode,
    cmdAlarmCancel: cmdAlarmCancel,
    cmdCameraCapture: cmdCameraCapture,
    cmdLightResult: cmdLightResult,
    cmdBarrierResult: cmdBarrierResult,
    cmdQueryStatus: cmdQueryStatus
  };
})(window);
