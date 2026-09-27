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

  /**
   * 播报一段中文文本。
   * @param {string} text
   * @param {object} [opts] { rate: 语速 0.5~2, pitch: 音调, force: 打断前一条 }
   */
  function speak(text, opts) {
    opts = opts || {};
    if (!isSupported()) {
      console.warn('[tts] 浏览器不支持语音合成，跳过：' + text);
      return;
    }
    if (!config.get().tts.enabled) return;
    if (opts.force) cancel();

    var u = new global.SpeechSynthesisUtterance(text);
    u.lang = 'zh-CN';
    u.rate = opts.rate != null ? opts.rate : 1.0;
    u.pitch = opts.pitch != null ? opts.pitch : 1.0;
    // 播报失败上报页面（如 not-allowed / synthesis-failed），便于现场排查无声问题
    u.onerror = function (ev) {
      var code = (ev && ev.error) ? ev.error : 'unknown';
      console.warn('[tts] 语音播报失败(' + code + ')：' + text);
      if (bus === null) bus = (global.SmartCane && global.SmartCane.bus) || false;
      if (bus) bus.emit('ui:notify', {
        message: '语音播报失败(' + code + ')，请点一次「测试语音」解锁后重试',
        type: 'danger'
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
    cancel: cancel
  };
})(window);
