/**
 * tts.js — 语音播报服务（对应原 App 的 TextToSpeech 组件）
 * ---------------------------------------------------------------
 * 使用浏览器 SpeechSynthesis，中文播报交通灯结果与报警提示。
 * 语音由页面统一经 bus VOICE 事件触发，服务层不关心业务。
 */
(function (global) {
  'use strict';

  var config = global.SmartCane.config;

  function isSupported() {
    return !!(global.speechSynthesis && global.SpeechSynthesisUtterance);
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
    speak: speak,
    cancel: cancel
  };
})(window);
