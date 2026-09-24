/**
 * family/app.js — 家属端页面控制器
 * ---------------------------------------------------------------
 * 与原 MIT App Inventor 版（smartcanefamily）功能 1:1 对应：
 *  - 每 5 秒拉取 Firebase /blind001.json（原 计时器1）
 *  - SOS 状态显示（紧急求助/没有报警，颜色变化）
 *  - 经纬度显示 + 高德地图实时定位（原 WebViewer + WebViewString）
 *  - 每 2 秒刷新前哨实时画面（原 画面刷新 Clock + Image.Picture）
 */
(function (global) {
  'use strict';

  var S = global.SmartCane;
  var cfg = S.config, fb = S.firebase, light = S.trafficLight;

  var $ = function (id) { return document.getElementById(id); };
  var pollTimer = null, snapTimer = null;
  var map = null, marker = null;

  function log(msg) {
    var v = $('logView');
    if (!v) return;
    var t = new Date().toLocaleTimeString('zh-CN', { hour12: false });
    v.textContent = '[' + t + '] ' + msg + '\n' + v.textContent;
  }

  // ================= Firebase 轮询（原 计时器1 5s） =================
  function poll() {
    fb.fetchBlind().then(function (data) {
      if (!data) {
        $('lastUpdate').textContent = '无数据';
        log('Firebase：盲人节点不存在');
        return;
      }
      // —— SOS 状态（原 sos 字段判断）——
      var sosActive = data.sos === true;
      var badge = $('sosBadge');
      badge.textContent = sosActive ? '紧急求助' : '没有报警';
      badge.className = 'badge ' + (sosActive ? 'danger' : 'ok');

      // —— 位置（原 location.latitude / longitude）——
      var loc = data.location || {};
      if (typeof loc.latitude === 'number' && typeof loc.longitude === 'number') {
        $('famLat').textContent = loc.latitude.toFixed(6);
        $('famLng').textContent = loc.longitude.toFixed(6);
        updateMap(loc.latitude, loc.longitude);
      }
      $('lastUpdate').textContent = new Date().toLocaleTimeString('zh-CN', { hour12: false });
      if (sosActive) log('SOS 报警中！位置 ' + loc.latitude + ',' + loc.longitude);
    }).catch(function (err) {
      $('lastUpdate').textContent = '读取失败';
      log('Firebase 读取失败：' + err.message);
    });
  }

  // ================= 高德地图（原 family_map HTML） =================
  function initMap() {
    if (typeof AMap === 'undefined') {
      log('高德地图脚本未加载（无网络？），地图暂不可用');
      $('mapContainer').textContent = '地图加载失败，请检查网络';
      return;
    }
    // 默认中心：北京（原代码误留东京坐标，已修正；收到定位后自动跳转到实际位置）
    map = new AMap.Map('mapContainer', { zoom: 15, center: [116.4074, 39.9042] });
    marker = new AMap.Marker({ position: [116.4074, 39.9042], title: '家人当前位置' });
    marker.setMap(map);
  }

  function updateMap(lat, lng) {
    if (!map || !marker) return;
    var pos = [lng, lat];              // 高德使用 [经度, 纬度]
    marker.setPosition(pos);
    map.setCenter(pos);
  }

  // ================= 实时画面（原 画面刷新 2s） =================
  function refreshSnapshot() {
    var img = $('snapshotImg');
    img.onerror = function () {
      img.className = 'snapshot offline';
      img.alt = '画面暂不可用（设备可能离线）';
      $('snapState').textContent = '暂不可用';
    };
    img.onload = function () {
      img.className = 'snapshot';
      $('snapState').textContent = '每 2 秒刷新';
    };
    img.src = light.snapshotUrl();   // ?token=...&t=时间戳 防缓存
  }

  // ================= 启动 / 停止 =================
  function start() {
    var iv = cfg.get().intervals;
    stop();
    initMap();
    poll();                                   // 立即拉一次
    pollTimer = setInterval(poll, iv.familyPollMs);      // 5s
    snapTimer = setInterval(refreshSnapshot, iv.snapshotMs); // 2s
    refreshSnapshot();
    log('家属端就绪 v1.0（WebApp 重构版）');
  }
  function stop() {
    if (pollTimer) clearInterval(pollTimer);
    if (snapTimer) clearInterval(snapTimer);
    pollTimer = snapTimer = null;
  }

  document.addEventListener('DOMContentLoaded', start);
  global.addEventListener('pagehide', stop);
})(window);
