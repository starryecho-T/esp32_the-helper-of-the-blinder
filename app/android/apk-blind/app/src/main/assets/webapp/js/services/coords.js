/**
 * coords.js — 坐标系转换服务（WGS-84 → GCJ-02）
 * ---------------------------------------------------------------
 * 背景：GPS / 浏览器定位输出的是 WGS-84（地球真实坐标），
 * 而高德（及国内大多数）地图使用 GCJ-02（国测局加偏坐标）。
 * 若不转换直接上图标，位置会系统性偏移几百米；且偏移量随
 * 地点变化，不存在「加固定常数」的正确补偿方式。
 *
 * 本文件采用业界公开的近似算法（误差约 1~2 米，满足本场景），
 * 纯本地计算：不依赖高德 convertFrom 接口、无网络请求、无配额限制。
 * 境外坐标（outOfChina）原样返回，不做偏移。
 *
 * 用法：var gcj = SmartCane.coords.wgs84ToGcj02(lat, lng);
 *       // → { latitude, longitude }，可直接交给高德地图
 */
(function (global) {
  'use strict';

  var PI = 3.14159265358979324;
  var A = 6378245.0;                // 克拉索夫斯基椭球长半轴（米）
  var EE = 0.00669342162296594323;  // 椭球偏心率平方

  /** 判断是否在中国境外（境外无 GCJ-02 加偏，直接用原坐标） */
  function outOfChina(lat, lng) {
    return lng < 72.004 || lng > 137.8347 || lat < 0.8293 || lat > 55.8271;
  }

  function transformLat(x, y) {
    var ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * Math.sqrt(Math.abs(x));
    ret += (20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0 / 3.0;
    ret += (20.0 * Math.sin(y * PI) + 40.0 * Math.sin(y / 3.0 * PI)) * 2.0 / 3.0;
    ret += (160.0 * Math.sin(y / 12.0 * PI) + 320.0 * Math.sin(y * PI / 30.0)) * 2.0 / 3.0;
    return ret;
  }

  function transformLng(x, y) {
    var ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * Math.sqrt(Math.abs(x));
    ret += (20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0 / 3.0;
    ret += (20.0 * Math.sin(x * PI) + 40.0 * Math.sin(x / 3.0 * PI)) * 2.0 / 3.0;
    ret += (150.0 * Math.sin(x / 12.0 * PI) + 300.0 * Math.sin(x / 30.0 * PI)) * 2.0 / 3.0;
    return ret;
  }

  /**
   * WGS-84 → GCJ-02（高德坐标系）。
   * @param {number} lat WGS-84 纬度
   * @param {number} lng WGS-84 经度
   * @returns {{latitude:number, longitude:number}} GCJ-02 坐标（境外原样返回）
   */
  function wgs84ToGcj02(lat, lng) {
    if (outOfChina(lat, lng)) {
      return { latitude: lat, longitude: lng };
    }
    var dLat = transformLat(lng - 105.0, lat - 35.0);
    var dLng = transformLng(lng - 105.0, lat - 35.0);
    var radLat = lat / 180.0 * PI;
    var magic = Math.sin(radLat);
    magic = 1 - EE * magic * magic;
    var sqrtMagic = Math.sqrt(magic);
    dLat = (dLat * 180.0) / ((A * (1 - EE)) / (magic * sqrtMagic) * PI);
    dLng = (dLng * 180.0) / (A / sqrtMagic * Math.cos(radLat) * PI);
    return { latitude: lat + dLat, longitude: lng + dLng };
  }

  global.SmartCane = global.SmartCane || {};
  global.SmartCane.coords = {
    outOfChina: outOfChina,
    wgs84ToGcj02: wgs84ToGcj02
  };
})(window);