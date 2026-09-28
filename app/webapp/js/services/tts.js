/**
 * tts.js — 语音播报服务（对应原 App 的 TextToSpeech 组件）
 * ---------------------------------------------------------------
 * 使用浏览器 SpeechSynthesis，中文播报交通灯结果与报警提示。
 * 语音由页面统一经 bus VOICE 事件触发，服务层不关心业务。
 */
(function (global) {
  'use strict';

  var config = global.SmartCane.config;
  var bus = null;   // 延迟取用：speak 失败时向页面发通知（bus.js 先于本文件加载）

  function isSupported() {
    return !!(global.speechSynthesis && global.SpeechSynthesisUtterance);
  }

  /**
   * 解锁语音合成（移动端必需）：iOS Safari / 微信内置浏览器等要求
   * speak() 首次必须发生在用户手势（点击/触摸）调用栈内，否则此后
   * 蓝牙回调/定时器里的后台播报会被静默吞掉、完全无声。
   * 页面首次触摸与「测试语音」按钮都会调用；幂等，可重复调用。
   */
  var unlocked = false;
  function unlock() {
    if (unlocked || !isSupported()) return;
    unlocked = true;
    try {
      global.speechSynthesis.speak(new global.SpeechSynthesisUtterance(' '));
    } catch (e) { /* 个别浏览器对空白文本抛错，忽略 */ }
  }

  // ================= 音频兜底（预录 WAV） =================
  // 微信内置浏览器 / 部分国产浏览器不带 speechSynthesis，无法在线合成语音；
  // 此时改播 app/webapp/audio/ 下的预录文件（Windows Huihui 生成，文案与播报一一对应）。
  // 障碍播报是动态词组（"前方障碍：行人、狗"），拆成 引导句 + 类词 逐段链播（见 speakSequence）。
  var AUDIO_DIR = 'audio/';
  var AUDIO_MAP = {
    '语音功能正常，手机将代读盲杖播报': 'voice-ok',
    '正常模式': 'mode-normal',
    '安静模式': 'mode-quiet',
    '夜间模式': 'mode-night',
    '已报警': 'alarmed',
    '电池电量低': 'low-battery',
    '您似乎跌倒了，三十秒后自动报警，拨动旋钮取消': 'fall',
    '检测到红灯，请停止前进': 'light-red',
    '检测到黄灯，请注意': 'light-yellow',
    '检测到绿灯，可以通行': 'light-green',
    '未检测到交通灯': 'light-none',
    '未检测到障碍物': 'barrier-none',
    // —— 障碍物具体名称（COCO 类别 → 预录词，APK 无在线合成时逐词链播）——
    '注意，前方障碍': 'barrier-intro',
    '行人': 'ob-person',
    '自行车': 'ob-bicycle',
    '汽车': 'ob-car',
    '摩托车': 'ob-motorcycle',
    '公交车': 'ob-bus',
    '卡车': 'ob-truck',
    '鸟': 'ob-bird',
    '猫': 'ob-cat',
    '狗': 'ob-dog',
    '马': 'ob-horse',
    '羊': 'ob-sheep',
    '牛': 'ob-cow',
    '大象': 'ob-elephant',
    '熊': 'ob-bear',
    '斑马': 'ob-zebra',
    '长颈鹿': 'ob-giraffe',
    '消防栓': 'ob-fire-hydrant',
    '停车标志': 'ob-stop-sign',
    '停车计时器': 'ob-parking-meter',
    '长椅': 'ob-bench',
    '椅子': 'ob-chair',
    '沙发': 'ob-couch',
    '盆栽': 'ob-potted-plant',
    '床': 'ob-bed',
    '餐桌': 'ob-dining-table',
    '马桶': 'ob-toilet',
    '车辆': 'ob-vehicle',
    '动物': 'ob-animal',
    '静态设施': 'ob-facility'
  };
  var audioEl = null;
  function speakAudio(text) {
    var key = AUDIO_MAP[text];
    if (!key && /^前方障碍：/.test(text)) key = 'barrier';
    if (!key) {
      console.warn('[tts] 无预录音频，跳过：' + text);
      return false;
    }
    try {
      if (audioEl) audioEl.pause();
      audioEl = new global.Audio(AUDIO_DIR + key + '.wav');
      audioEl.play().catch(function (e) { console.warn('[tts] 音频播放失败：', e); });
      return true;
    } catch (e) {
      console.warn('[tts] 音频播放异常：', e);
      return false;
    }
  }

  /**
   * 依次播放一串预录音频（链式：一个 onended 接下一个）。
   * 任意时刻 speakAudio() 触发新的单段播放会 pause 当前实例，链自然中断 → 新播报优先。
   */
  function playAudioChain(keys) {
    var i = 0;
    function next() {
      if (i >= keys.length) return;
      var k = keys[i++];
      try {
        var el = new global.Audio(AUDIO_DIR + k + '.wav');
        audioEl = el;                       // 登记，供 speakAudio 打断
        el.onended = next;
        el.onerror = next;                  // 个别词文件缺失 → 跳过继续
        var p = el.play();
        if (p && p.catch) p.catch(next);    // play 被拒（应极少：APK 已放开手势）→ 跳下一个
      } catch (e) { next(); }
    }
    next();
  }

  /**
   * 分段播报（双检场景：先灯色后障碍）。
   * 在线合成环境：交给 speechSynthesis 自带队列连续朗读；
   * 预录环境：把每段拆解成 引导句 + 类词 的音频链依次播放。
   * @param {string[]} parts 如 ['检测到红灯，请停止前进', '前方障碍：行人、狗']
   */
  function speakSequence(parts) {
    if (!parts || !parts.length) return;
    if (!config.get().tts.enabled) return;
    if (isSupported()) {
      parts.forEach(function (text, idx) { speak(text, { force: idx === 0 }); });
      return;
    }
    var keys = [];
    parts.forEach(function (text) {
      if (AUDIO_MAP[text]) { keys.push(AUDIO_MAP[text]); return; }
      var m = /^前方障碍：(.+)$/.exec(text);
      if (m) {
        keys.push('barrier-intro');
        m[1].split('、').forEach(function (w) {
          if (AUDIO_MAP[w]) keys.push(AUDIO_MAP[w]);
          else console.warn('[tts] 障碍词无预录音频，跳过：' + w);
        });
        return;
      }
      console.warn('[tts] 无预录音频，跳过：' + text);
    });
    if (keys.length) playAudioChain(keys);
  }

  /**
   * 播报一段中文文本。
   * @param {string} text
   * @param {object} [opts] { rate: 语速 0.5~2, pitch: 音调, force: 打断前一条 }
   */
  function speak(text, opts) {
    opts = opts || {};
    if (!config.get().tts.enabled) return;
    if (!isSupported()) {          // 浏览器无语音合成 → 播预录音频
      speakAudio(text);
      return;
    }
    if (opts.force) cancel();

    var u = new global.SpeechSynthesisUtterance(text);
    u.lang = 'zh-CN';
    u.rate = opts.rate != null ? opts.rate : 1.0;
    u.pitch = opts.pitch != null ? opts.pitch : 1.0;
    // 播报失败上报页面（如 not-allowed / synthesis-failed），便于现场排查无声问题；
    // 失败时再兜底播预录音频（微信等浏览器合成被禁用/失败的场景）
    u.onerror = function (ev) {
      var code = (ev && ev.error) ? ev.error : 'unknown';
      console.warn('[tts] 语音播报失败(' + code + ')：' + text);
      speakAudio(text);
      if (bus === null) bus = (global.SmartCane && global.SmartCane.bus) || false;
      if (bus) bus.emit('ui:notify', {
        message: '在线语音失败(' + code + ')，已尝试播放预录语音',
        type: 'warning'
      });
    };

    // 尽量选择中文音色
    var voices = global.speechSynthesis.getVoices() || [];
    var zh = voices.filter(function (v) { return /^zh([-_]|$)/i.test(v.lang); });
    if (zh.length) {
      var preferred = zh.filter(function (v) { return /mandarin|普通|中文|China/i.test(v.name); });
      u.voice = (preferred[0] || zh[0]);
    }

    global.speechSynthesis.speak(u);
  }

  function cancel() {
    if (isSupported()) global.speechSynthesis.cancel();
  }

  // Chrome 首次调用 getVoices 为空，触发一次加载
  if (isSupported()) global.speechSynthesis.getVoices();

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.tts = {
    isSupported: isSupported,
    unlock: unlock,
    speak: speak,
    speakSequence: speakSequence,
    cancel: cancel
  };
})(window);
