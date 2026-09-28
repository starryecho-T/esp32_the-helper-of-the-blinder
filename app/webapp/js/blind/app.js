/**
 * blind/app.js — 盲人端页面控制器
 * ---------------------------------------------------------------
 * 与原 MIT App Inventor 版（cane_controller__2）功能 1:1 对应：
 *  - BLE 连接管理（选择/连接/断开）
 *  - 周期状态解析显示（模式/电量/报警/障碍/距离）
 *  - 模式切换（MODE:0/1/2）
 *  - 交通灯识别闭环（CAMERA:CAPTURE → /detect → 播报 → 回传色值）
 *  - 障碍物检测闭环（barrierdetect → /barrier → 播报 → 回传 BARRIER:xxx）
 *  - 报警联动（ALARM:MANUAL/CANCEL、FALL:CONFIRMED/CANCELLED → Firebase SOS + 上传 GPS）
 *  - GPS 定时上传（10s）+ 事件即时上传
 *  - 手机代读语音（纯软件，无需改固件）：盲杖语音模块异常时，
 *    依据 MODE/FALL/ALARM/BATT 事件用手机 TTS 朗读原播报文案
 */
(function (global) {
  'use strict';

  var S = global.SmartCane;
  var bus = S.bus, EVENTS = S.EVENTS, store = S.store, protocol = S.protocol;
  var ble = S.ble, fb = S.firebase, light = S.trafficLight, barrier = S.barrier,
      geo = S.geo, tts = S.tts, coords = S.coords;
  var cfg = S.config;

  var $ = function (id) { return document.getElementById(id); };
  var MODE_TEXT = { NORMAL: '日常模式', SILENT: '安静模式', NIGHT: '夜间模式' };
  // 手机代读语音文案（与盲杖原 SYN6288 播报一致）
  var MODE_VOICE = { NORMAL: '正常模式', SILENT: '安静模式', NIGHT: '夜间模式' };
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

  // ================= 手机代读语音（纯软件方案，无需改固件） =================
  // 盲杖 SYN6288 语音模块异常时，由手机 TTS 代读：盲杖本就会把 MODE:xxx /
  // FALL:1 / ALARM:MANUAL 事件和状态包 BATT 字段发过来，据此触发朗读即可。
  // 同一句 3 秒内只播一次：防 FALL:1 每 3 秒重发等造成的重复朗读，
  // 也兼容将来固件改发 SAY:<文本>（两路同文本会被去重合并，不双播）。
  var lastSpokenAt = {};
  var lowBattAnnounced = false;   // 低电量语音一次性标志（与固件逻辑一致）
  var fallVoicePlayed = false;    // 一次跌倒只播一遍（FALL:1 会每 3 秒重发）
  function announce(text) {
    if (!text) return;
    var now = Date.now();
    if (lastSpokenAt[text] && now - lastSpokenAt[text] < 3000) return;
    lastSpokenAt[text] = now;
    bus.emit(EVENTS.VOICE, { message: text });
    tts.speak(text, { force: true });
    log('语音播报：' + text);
  }

  // ================= BLE 连接区 =================
  function setBleUi(connected, connecting, name, statusText) {
    $('bleState').textContent = statusText || (connecting ? '正在连接…' : (connected ? '已连接' : '未连接'));
    $('bleState').className = 'value ' + (connected ? 'ok' : 'muted');
    $('bleDevice').textContent = name || (connected ? '未知设备' : '未选择');
    $('bleDevice').className = 'value ' + (connected ? '' : 'muted');
    $('btnConnect').disabled = connected || connecting;
    $('btnDisconnect').disabled = !connected && !connecting; // 连接中/重连中也可手动取消
    ['btnMode0', 'btnMode1', 'btnMode2', 'btnCapture', 'btnBarrier', 'btnAlarmCancel',
     'btnLightRed', 'btnLightYellow', 'btnLightGreen'].forEach(function (id) {
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
  bus.on(EVENTS.BLE_DISCONNECTED, function (e) {
    var lost = !!(e && e.reason === 'lost');
    setBleUi(false, false, '');
    // 意外掉线时保留设备名，供「重连中」界面继续显示
    store.set('ble', {
      connected: false,
      deviceName: lost ? (store.get().ble.deviceName || '') : ''
    });
    toast(lost ? '与盲杖的连接已断开' : '已断开连接', 'warning');
  });
  bus.on(EVENTS.BLE_CONNECTING, function (e) {
    // e.name 兼作状态文案：如「正在扫描盲杖…」「重连中（第 n 次）…」
    var devName = e.reconnecting
      ? (store.get().ble.deviceName || '盲杖')
      : (e.name || '');
    setBleUi(false, true, devName, e.name || '正在连接…');
    if (e.reconnecting) {
      log('自动重连：第 ' + e.attempt + ' 次尝试将于 ' +
          Math.round((e.delayMs || 0) / 1000) + ' 秒后进行');
      if (e.attempt === 1) toast('正在自动重连盲杖…', 'warning');
    }
  });

  // ================= 模式切换（原三个模式按钮） =================
  function setMode(idx) {
    if (!ble.isConnected()) { toast('请先连接智能盲杖', 'warning'); return; }
    ble.writeLine(protocol.cmdSetMode(idx));
    showMode(protocol.MODE_BY_INDEX[idx]);
    announce(MODE_VOICE[protocol.MODE_BY_INDEX[idx]]);   // 手机代读（旋钮切换走 MODE: 事件，去重防双播）
    toast('已发送：' + MODE_TEXT[protocol.MODE_BY_INDEX[idx]], 'info');
  }
  function showMode(mode) {
    $('caneMode').textContent = MODE_TEXT[mode] || mode || '--';
    $('caneMode').className = 'value ' + (mode ? '' : 'muted');
  }

  // 绑定三个模式按钮（BUGFIX：此前只定义了 setMode 却从未绑定 click，点按钮无任何反应）
  $('btnMode0').addEventListener('click', function () { setMode(0); });   // 日常模式
  $('btnMode1').addEventListener('click', function () { setMode(1); });   // 安静模式
  $('btnMode2').addEventListener('click', function () { setMode(2); });   // 夜间模式

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
      // 低电量语音（手机代读）：阈值与一次性播报逻辑和固件一致（<20 播一次，恢复重置）
      if (s.batt >= 0 && s.batt < 20) {
        if (!lowBattAnnounced) { lowBattAnnounced = true; announce('电池电量低'); }
      } else {
        lowBattAnnounced = false;
      }
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
      case protocol.EVENT_TYPE.TTS_SPEAK:          // 预留：将来固件改发 SAY:<文本> 时自动生效
        announce(evt.text);
        break;
      // 盲杖按钮长按 1.5s 发 CAMERA:CAPTURE / barrierdetect → 一次同时识别交通灯 + 障碍物
      case protocol.EVENT_TYPE.CAMERA_CAPTURE: runComboDetect(); break;
      case protocol.EVENT_TYPE.BARRIER_DETECT: runComboDetect(); break;
      case protocol.EVENT_TYPE.MODE_SWITCH:
        showMode(evt.mode);
        announce(MODE_VOICE[evt.mode]);            // 手机代读原 SYN6288 的模式播报
        break;
      case protocol.EVENT_TYPE.ALARM_MANUAL:
        announce('已报警');
        sosReport(true, '收到主动报警，当前位置已上传');
        break;
      case protocol.EVENT_TYPE.ALARM_CANCEL: sosClear('报警已解除，已恢复正常状态'); break;
      case protocol.EVENT_TYPE.FALL_CONFIRMED:
        fallVoicePlayed = false;
        announce('已报警');
        sosReport(true, '用户跌倒，当前位置已上传');
        break;
      case protocol.EVENT_TYPE.FALL_CANCELLED:
        fallVoicePlayed = false;
        sosClear('跌倒报警已解除');
        break;
      case protocol.EVENT_TYPE.FALL_DETECTED:
        if (!fallVoicePlayed) {                    // FALL:1 每 3 秒重发，一次跌倒只播一遍
          fallVoicePlayed = true;
          announce('您似乎跌倒了，三十秒后自动报警，拨动旋钮取消');
        }
        log('收到 FALL:1（30 秒倒计时，端上可取消）');
        break;
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

  // ================= 双检闭环（交通灯 + 障碍物一次同测） =================
  // 触发：盲杖按钮长按 1.5s（CAMERA:CAPTURE / barrierdetect）或页面「双检」按钮长按 1 秒。
  // 播报规则：未识别到交通灯 → 不播灯色；无障碍 → 不播障碍；
  //           有障碍 → 播具体名称（"前方障碍：行人、狗"）；两者皆无 → 不出声，仅界面显示。
  var comboBusy = false;
  function runComboDetect() {
    if (comboBusy) { toast('正在检测，请稍候', 'warning'); return; }
    comboBusy = true;
    $('lightColor').textContent = '正在识别……';
    $('lightConf').textContent = '置信度：--';
    $('barrierSummary').textContent = '正在检测……';
    $('barrierDetail').textContent = '';
    // 两路并发；任何一路失败不影响另一路（错误包装成值传递）
    function wrap(p) {
      return p.then(function (v) { return { ok: v }; },
                     function (e) { return { err: e }; });
    }
    Promise.all([wrap(light.detect()), wrap(barrier.detect())]).then(function (rs) {
      comboBusy = false;
      $('btnCapture').textContent = HOLD_LABEL;
      var l = rs[0], b = rs[1];
      var parts = [];                       // 语音分段：先灯色，后障碍
      if (l.ok) {
        showLight(l.ok);
        if (ble.isConnected()) ble.writeLine(protocol.cmdLightResult(l.ok.color));  // 回传盲杖
        if (l.ok.color !== 'NONE') {        // 未识别到交通灯 → 不播报（仅界面显示）
          var t = cfg.get().tts;
          parts.push(l.ok.color === 'RED' ? t.redText :
                     l.ok.color === 'YELLOW' ? t.yellowText : t.greenText);
        }
      } else {
        $('lightColor').textContent = '识别失败';
        $('lightConf').textContent = String(l.err.message || l.err).slice(0, 60);
      }
      if (b.ok) {
        showBarrier(b.ok);
        if (ble.isConnected()) ble.writeLine(protocol.cmdBarrierResult(b.ok.cane));  // 回传盲杖
        if (b.ok.total > 0) parts.push('前方障碍：' + barrierNamesZh(b.ok.objects).join('、'));
      } else {
        $('barrierSummary').textContent = '检测失败';
        $('barrierDetail').textContent = String(b.err.message || b.err).slice(0, 60);
      }
      if (parts.length) {
        bus.emit(EVENTS.VOICE, { message: parts.join('，') });
        tts.speakSequence(parts);           // 分段播报：在线合成排队读；预录音频逐段链播
        log('双检播报：' + parts.join('，'));
      } else {
        log('双检完成：无交通灯、无障碍物（不播报）');
      }
      if (l.err && b.err) toast('交通灯与障碍物检测均失败：' + (l.err.message || l.err), 'danger');
    });
  }
  function showLight(r) {
    $('lightColor').textContent = LIGHT_CN[r.color] || r.color;
    $('lightConf').textContent = '置信度：' + r.confidence;
    $('lightBox').className = 'light-box ' + (LIGHT_BOX_CLS[r.color] || '');
  }

  // —— 页面按钮长按 1 秒触发双检（防误触；不足 1 秒松手提示）——
  var HOLD_LABEL = '长按1秒·灯色+障碍双检';
  var holdTimer = null;
  function holdStart() {
    if ($('btnCapture').disabled) return;
    $('btnCapture').textContent = '按住…';
    holdTimer = setTimeout(function () {
      holdTimer = null;
      $('btnCapture').textContent = '检测中…';
      runComboDetect();
    }, 1000);
  }
  function holdEnd() {
    if (holdTimer) {
      clearTimeout(holdTimer);
      holdTimer = null;
      $('btnCapture').textContent = HOLD_LABEL;
      toast('请长按满 1 秒再松开', 'info');
    }
  }
  $('btnCapture').addEventListener('pointerdown', holdStart);
  $('btnCapture').addEventListener('pointerup', holdEnd);
  $('btnCapture').addEventListener('pointerleave', holdEnd);
  $('btnCapture').addEventListener('pointercancel', holdEnd);
  // 移动端长按按钮默认会弹系统菜单/选中文字，双检长按必须屏蔽
  $('btnCapture').addEventListener('contextmenu', function (e) { e.preventDefault(); });

  // ================= 手动灯色（演示） =================
  // 不走云端识别，直接把所选灯色回传盲杖（RED/YELLOW/GREEN）。
  // 盲杖固件 setTrafficLight() 收到后语音播报（红灯请等待等），
  // 并在周期状态包回显 LIGHT:红灯。注意：仅灯色变化时盲杖才播报，
  // 连续点同一颜色不会重复播。
  function sendManualLight(color) {
    if (!ble.isConnected()) { toast('请先连接智能盲杖', 'warning'); return; }
    showLight({ color: color, confidence: '手动' });
    var t = cfg.get().tts;
    var voice = color === 'RED' ? t.redText : color === 'YELLOW' ? t.yellowText : t.greenText;
    bus.emit(EVENTS.VOICE, { message: voice });
    tts.speak(voice, { force: true });
    ble.writeLine(protocol.cmdLightResult(color));   // 回传盲杖
    toast('已发送：' + LIGHT_CN[color], 'success');
    log('手动灯色 ' + color + ' → 已回传盲杖');
  }
  $('btnLightRed').addEventListener('click', function () { sendManualLight('RED'); });
  $('btnLightYellow').addEventListener('click', function () { sendManualLight('YELLOW'); });
  $('btnLightGreen').addEventListener('click', function () { sendManualLight('GREEN'); });

  // 「测试语音」：手动校验手机 TTS。点击是用户手势，同时完成移动端语音解锁
  // （iOS Safari / 微信内置浏览器不解锁时，蓝牙事件触发的后台播报会无声）。
  // 不支持在线合成的浏览器（部分微信/国产内核）会自动改播预录音频。
  $('btnVoiceTest').addEventListener('click', function () {
    if (tts.unlock) tts.unlock();
    if (!tts.isSupported()) {
      log('此浏览器不支持在线语音合成，改用预录音频');
      toast('此浏览器不支持在线语音，已改用预录音频播放', 'warning');
    }
    tts.speak('语音功能正常，手机将代读盲杖播报', { force: true });
    log('手动语音测试已播放');
    if (tts.isSupported()) toast('已播放测试语音，若无声请调大媒体音量后重试', 'info');
  });

  // ================= 障碍物检测闭环 =================
  // 触发：盲杖 BLE 发 barrierdetect（protocol 解析为 BARRIER_DETECT 事件）或点「手动检测」。
  // 流程：GET /barrier → 云端 yolov8s 检测 → 播报具体障碍名称
  //       → BLE 回传 cane 串（BARRIER:PED2,VEH1 / BARRIER:NONE）给盲杖。
  // —— COCO 具体类别 → 中文：障碍播报用具体名称（行人/狗/椅子…），
  //    未收录的 label 回退 4 大类中文名（barrier.CATEGORY_CN）。——
  var COCO_CN = {
    person: '行人', bicycle: '自行车', car: '汽车', motorcycle: '摩托车',
    bus: '公交车', truck: '卡车',
    bird: '鸟', cat: '猫', dog: '狗', horse: '马', sheep: '羊', cow: '牛',
    elephant: '大象', bear: '熊', zebra: '斑马', giraffe: '长颈鹿',
    'fire hydrant': '消防栓', 'stop sign': '停车标志', 'parking meter': '停车计时器',
    bench: '长椅', chair: '椅子', couch: '沙发', 'potted plant': '盆栽',
    bed: '床', 'dining table': '餐桌', toilet: '马桶'
  };
  function cocoZh(label) {
    return COCO_CN[String(label || '').toLowerCase()] || '';
  }
  /** 障碍明细 → 具体中文名列表（按出现顺序去重；未知 label 回退大类名）。 */
  function barrierNamesZh(objects) {
    var names = [];
    (objects || []).forEach(function (o) {
      var zh = cocoZh(o.label) || barrier.CATEGORY_CN[o.category] || o.label;
      if (names.indexOf(zh) < 0) names.push(zh);
    });
    return names;
  }
  function runBarrierDetect() {
    $('barrierSummary').textContent = '正在检测……';
    $('barrierDetail').textContent = '';
    barrier.detect().then(function (r) {
      showBarrier(r);
      var voice = r.total > 0
        ? ('前方障碍：' + barrierNamesZh(r.objects).join('、'))
        : cfg.get().tts.barrierNoneText;
      bus.emit(EVENTS.VOICE, { message: voice });
      tts.speakSequence([voice]);           // 预录环境自动拆成 引导句+类词 链播
      if (ble.isConnected()) ble.writeLine(protocol.cmdBarrierResult(r.cane));  // 回传盲杖
      log('障碍物 ' + barrierNamesZh(r.objects).join('、') + ' → 已播报/回传 ' + r.cane);
    }).catch(function (err) {
      $('barrierSummary').textContent = '检测失败';
      $('barrierDetail').textContent = String(err.message).slice(0, 60);
      toast('障碍物检测失败：' + err.message, 'danger');
    });
  }
  function showBarrier(r) {
    $('barrierSummary').textContent = r.total > 0
      ? ('前方：' + barrierNamesZh(r.objects).join('、')) : '未检测到障碍物';
    var counts = [];
    barrier.CATEGORY_ORDER.forEach(function (c) {
      counts.push(barrier.CATEGORY_CN[c] + ' ' + (r.counts[c] || 0));
    });
    $('barrierCounts').textContent = counts.join('　');
    var detail = r.objects.slice(0, 5).map(function (o) {
      var zh = cocoZh(o.label) || barrier.CATEGORY_CN[o.category] || o.label;
      return zh + ' ' + Math.round((o.conf || 0) * 100) + '%';
    }).join('、');
    $('barrierDetail').textContent = detail || '--';
    $('barrierBox').className = 'barrier-box ' + (r.total > 0 ? 'hit' : 'clear');
  }
  $('btnBarrier').addEventListener('click', runBarrierDetect);

  // ================= GPS（原 LocationSensor + gps计时器） =================
  bus.on(EVENTS.GEO_POSITION, function (p) {
    store.set('geo', p);
    $('geoLat').textContent = p.latitude.toFixed(6);
    $('geoLng').textContent = p.longitude.toFixed(6);
    $('geoAcc').textContent = p.accuracy ? ('±' + Math.round(p.accuracy) + ' 米') : '--';
  });
  bus.on(EVENTS.GEO_ERROR, function (e) { log('GPS：' + e.message); });

  /** 对应原过程「立即上传GPS」——上报前把 WGS-84 原始坐标转成 GCJ-02（高德坐标系） */
  function uploadGpsNow() {
    var p = geo.current();
    if (!p) { log('暂无 GPS 定位，跳过上传'); return Promise.resolve(); }
    var g = coords.wgs84ToGcj02(p.latitude, p.longitude);
    return fb.putLocation(g.latitude, g.longitude)
      .then(function () {
        log('GPS 已上传 ' + g.latitude.toFixed(5) + ',' + g.longitude.toFixed(5) +
            '（GCJ-02；WGS-84 原始 ' + p.latitude.toFixed(5) + ',' + p.longitude.toFixed(5) + '）');
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
    // 首次任意触摸/点击即解锁语音合成（幂等）：保证之后蓝牙事件触发的播报能出声
    document.addEventListener('touchend', function () { tts.unlock(); }, { once: true, passive: true });
    document.addEventListener('click', function () { tts.unlock(); }, { once: true });
    log('盲人端就绪 v1.0（WebApp 重构版）');
  }
  document.addEventListener('DOMContentLoaded', init);
})(window);

