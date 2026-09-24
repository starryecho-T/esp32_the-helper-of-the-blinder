/**
 * blind/app.js — 盲人端页面控制器
 * ---------------------------------------------------------------
 * 与原 MIT App Inventor 版（cane_controller__2）功能 1:1 对应：
 *  - BLE 连接管理（选择/连接/断开）
 *  - 周期状态解析显示（模式/电量/报警/障碍/距离）
 *  - 模式切换（MODE:0/1/2）
 *  - 交通灯识别闭环（CAMERA:CAPTURE → /detect → 播报 → 回传色值）
 *  - 报警联动（ALARM:MANUAL/CANCEL、FALL:CONFIRMED/CANCELLED → Firebase SOS + 上传 GPS）
 *  - GPS 定时上传（10s）+ 事件即时上传
 */
(function (global) {
  'use strict';

  var S = global.SmartCane;
  var bus = S.bus, EVENTS = S.EVENTS, store = S.store, protocol = S.protocol;
  var ble = S.ble, fb = S.firebase, light = S.trafficLight, geo = S.geo, tts = S.tts;
  var cfg = S.config;

  var $ = function (id) { return document.getElementById(id); };
  var MODE_TEXT = { NORMAL: '日常模式', SILENT: '安静模式', NIGHT: '夜间模式' };
  var ALARM_TEXT = { NORMAL: '正常', MANUAL: '主动报警', FALL: '跌倒报警' };
  var TYPE_TEXT = { SAFE: '安全', LARGE: '大型障碍', LOW: '低位障碍', HIGH: '悬空障碍' };

  // ================= 通知 / 日志 =================
  function toast(msg, type) {
    var el = document.createElement('div');
    el.className = 'toast ' + (type || 'info');
    el.textContent = msg;
    $('toastArea').appendChild(el);
    setTimeout(function () { el.remove(); }, 4000);
  }
  function log(msg) {
    var v = $('logView');
    if (!v) return;
    var t = new Date().toLocaleTimeString('zh-CN', { hour12: false });
    v.textContent = '[' + t + '] ' + msg + '\n' + v.textContent;
  }
  bus.on(EVENTS.NOTIFY, function (n) { toast(n.message, n.type); });

  // ================= BLE 连接区 =================
  function setBleUi(connected, connecting, name) {
    $('bleState').textContent = connecting ? '正在连接…' : (connected ? '已连接' : '未连接');
    $('bleState').className = 'value ' + (connected ? 'ok' : 'muted');
    $('bleDevice').textContent = name || (connected ? '未知设备' : '未选择');
    $('bleDevice').className = 'value ' + (connected ? '' : 'muted');
    $('btnConnect').disabled = connected || connecting;
    $('btnDisconnect').disabled = !connected;
    ['btnMode0', 'btnMode1', 'btnMode2', 'btnCapture', 'btnAlarmCancel'].forEach(function (id) {
      $(id).disabled = !connected;
    });
  }

  $('btnConnect').addEventListener('click', function () {
    ble.connect().catch(function () { /* 已通过事件通知 */ });
  });
  $('btnDisconnect').addEventListener('click', function () {
    toast('正在断开……', 'info');
    ble.disconnect();
  });

  bus.on(EVENTS.BLE_CONNECTED, function (e) {
    setBleUi(true, false, e.name);
    store.set('ble', { connected: true, deviceName: e.name });
    toast('已连接：' + e.name, 'success');
    ble.writeLine(protocol.cmdQueryStatus());   // 连上先查一次状态
  });
  bus.on(EVENTS.BLE_DISCONNECTED, function () {
    setBleUi(false, false, '');
    store.set('ble', { connected: false, deviceName: '' });
    toast('已断开连接', 'warning');
  });
  bus.on(EVENTS.BLE_CONNECTING, function (e) { setBleUi(false, true, e.name); });

  // ================= 模式切换（原三个模式按钮） =================
  function setMode(idx) {
    if (!ble.isConnected()) { toast('请先连接智能盲杖', 'warning'); return; }
    ble.writeLine(protocol.cmdSetMode(idx));
    showMode(protocol.MODE_BY_INDEX[idx]);
    toast('已发送：' + MODE_TEXT[protocol.MODE_BY_INDEX[idx]], 'info');
  }
  function showMode(mode) {
    $('caneMode').textContent = MODE_TEXT[mode] || mode || '--';
    $('caneMode').className = 'value ' + (mode ? '' : 'muted');
  }

  // ================= 盲杖数据流 =================
  bus.on(EVENTS.BLE_DATA, function (e) {
    log('← ' + e.line);
    var p = protocol.parseLine(e.line);
    if (p.kind === protocol.KIND.STATUS) onCaneStatus(p.status);
    else if (p.kind === protocol.KIND.EVENT) onCaneEvent(p.event);
    else log('未知消息：' + p.raw);
  });

  function onCaneStatus(s) {
    store.set('cane', Object.assign({ updatedAt: Date.now() }, s));
    if (s.mode) showMode(s.mode);
    if (s.batt !== null && s.batt !== undefined) {
      var b = $('caneBatt');
      b.textContent = s.batt + '%';
      b.className = 'value ' + (s.batt > 20 ? 'ok' : 'danger');
    }
    if (s.alarm) {
      var a = $('caneAlarm');
      a.textContent = ALARM_TEXT[s.alarm] || s.alarm;
      a.className = 'value ' + (s.alarm === 'NORMAL' ? 'ok' : 'danger');
    }
    if (s.type) {
      var o = $('caneObstacle');
      o.textContent = TYPE_TEXT[s.type] || s.type;
      o.className = 'value ' + (s.type === 'SAFE' ? 'ok' : (s.level >= 2 ? 'danger' : 'warn'));
    }
    if (s.dist !== null || s.high !== null) {
      $('caneDist').textContent = (s.dist === null ? '--' : s.dist) + ' / ' +
                                  (s.high === null ? '--' : s.high) + ' cm';
    }
  }

  function onCaneEvent(evt) {
    bus.emit(EVENTS.CANE_EVENT, evt);
    switch (evt.type) {
      case protocol.EVENT_TYPE.CAMERA_CAPTURE: runDetect(); break;
      case protocol.EVENT_TYPE.MODE_SWITCH: showMode(evt.mode); break;
      case protocol.EVENT_TYPE.ALARM_MANUAL: sosReport(true, '收到主动报警，当前位置已上传'); break;
      case protocol.EVENT_TYPE.ALARM_CANCEL: sosClear('报警已解除，已恢复正常状态'); break;
      case protocol.EVENT_TYPE.FALL_CONFIRMED: sosReport(true, '用户跌倒，当前位置已上传'); break;
      case protocol.EVENT_TYPE.FALL_CANCELLED: sosClear('跌倒报警已解除'); break;
      case protocol.EVENT_TYPE.FALL_DETECTED: log('收到 FALL:1（30 秒倒计时，端上可取消）'); break;
    }
  }

  // —— SOS 上报（对应原 PutText sos.json + 立即上传GPS）——
  function sosReport(active, message) {
    fb.putSos(active).then(function () {
      return uploadGpsNow();
    }).then(function () {
      $('sosState').textContent = '报警中';
      $('sosState').className = 'value danger';
      toast(message, 'danger');
      log('SOS=' + active + ' 已上传 Firebase');
    }).catch(function (err) {
      toast('SOS 上传失败：' + err.message, 'danger');
    });
  }
  function sosClear(message) {
    fb.putSos(false).then(function () {
      $('sosState').textContent = '正常';
      $('sosState').className = 'value ok';
      toast(message, 'success');
    }).catch(function (err) {
      toast('SOS 状态更新失败：' + err.message, 'danger');
    });
  }

  // ================= 交通灯识别闭环 =================
  var LIGHT_CN = { RED: '红灯', YELLOW: '黄灯', GREEN: '绿灯', NONE: '未检测到交通灯' };
  var LIGHT_BOX_CLS = { RED: 'red', YELLOW: 'yellow', GREEN: 'green' };

  function runDetect() {
    $('lightColor').textContent = '正在识别……';
    $('lightConf').textContent = '置信度：--';
    light.detect().then(function (r) {
      showLight(r);
      var t = cfg.get().tts;
      var voice = r.color === 'RED' ? t.redText : r.color === 'YELLOW' ? t.yellowText :
                  r.color === 'GREEN' ? t.greenText : t.noneText;
      bus.emit(EVENTS.VOICE, { message: voice });
      tts.speak(voice, { force: true });
      if (ble.isConnected()) ble.writeLine(protocol.cmdLightResult(r.color));  // 回传盲杖
      log('识别 ' + r.color + '|' + r.confidence + ' → 已播报/回传');
    }).catch(function (err) {
      $('lightColor').textContent = '识别失败';
      $('lightConf').textContent = String(err.message).slice(0, 60);
      toast('识别失败：' + err.message, 'danger');
    });
  }
  function showLight(r) {
    $('lightColor').textContent = LIGHT_CN[r.color] || r.color;
    $('lightConf').textContent = '置信度：' + r.confidence;
    $('lightBox').className = 'light-box ' + (LIGHT_BOX_CLS[r.color] || '');
  }
  $('btnCapture').addEventListener('click', runDetect);

  // ================= GPS（原 LocationSensor + gps计时器） =================
  bus.on(EVENTS.GEO_POSITION, function (p) {
    store.set('geo', p);
    $('geoLat').textContent = p.latitude.toFixed(6);
    $('geoLng').textContent = p.longitude.toFixed(6);
  });
  bus.on(EVENTS.GEO_ERROR, function (e) { log('GPS：' + e.message); });

  /** 对应原过程「立即上传GPS」 */
  function uploadGpsNow() {
    var p = geo.current();
    if (!p) { log('暂无 GPS 定位，跳过上传'); return Promise.resolve(); }
    return fb.putLocation(p.latitude, p.longitude)
      .then(function () {
        log('GPS 已上传 ' + p.latitude.toFixed(5) + ',' + p.longitude.toFixed(5));
      });
  }

  $('btnAlarmCancel').addEventListener('click', function () {
    if (!ble.isConnected()) return;
    ble.writeLine(protocol.cmdAlarmCancel());
    sosClear('已远程取消盲杖报警');
  });

  // ================= 启动 =================
  function init() {
    setBleUi(false, false, '');
    if (!ble.isSupported()) {
      $('bleState').textContent = '浏览器不支持';
      toast('当前浏览器不支持 Web Bluetooth，请用 Android Chrome 打开', 'warning');
    }
    if (!ble.isSecureContext()) {
      toast('蓝牙需 HTTPS 页面，当前地址不可用蓝牙（其他功能不受影响）', 'warning');
    }
    geo.start();
    setInterval(uploadGpsNow, cfg.get().intervals.gpsUploadMs);   // 原 gps计时器 10s
    log('盲人端就绪 v1.0（WebApp 重构版）');
  }
  document.addEventListener('DOMContentLoaded', init);
})(window);

