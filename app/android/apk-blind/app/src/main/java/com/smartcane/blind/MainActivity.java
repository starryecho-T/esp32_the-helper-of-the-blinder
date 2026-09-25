package com.smartcane.blind;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.view.KeyEvent;
import android.webkit.GeolocationPermissions;
import android.webkit.JsResult;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * 盲人端壳：全屏 WebView 加载打包在 assets 里的 webapp/blind.html。
 * - 混合内容放开：file:// 页面可直接访问 http://39.106.216.80:8000 识别服务器
 * - 注入原生 BLE 桥（SmartCaneNative）：WebView 不支持 Web Bluetooth
 * - 支持 Geolocation（GPS 上报）与 alert/confirm 对话框
 */
public class MainActivity extends Activity {

    public static final int REQ_PERMS = 1001;

    private WebView web;
    private BleBridge ble;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        web = new WebView(this);
        setContentView(web);

        setupWebView();
        requestRuntimePermissions();
        web.loadUrl("file:///android_asset/webapp/blind.html");
    }

    private void setupWebView() {
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);                 // localStorage：配置持久化
        s.setGeolocationEnabled(true);                // GPS 上报
        s.setMediaPlaybackRequiresUserGesture(false); // 语音播报无需手势
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);

        web.setWebViewClient(new WebViewClient());    // 站内跳转不弹系统浏览器
        ble = new BleBridge(this);
        web.addJavascriptInterface(ble, BleBridge.JS_NAME);
        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onGeolocationPermissionsShowPrompt(String origin,
                    GeolocationPermissions.Callback callback) {
                boolean ok = hasPermissions(needLocation());
                callback.invoke(origin, ok, false);
                if (!ok) requestRuntimePermissions(); // 授权后下次定位即可成功
            }

            @Override
            public boolean onJsAlert(WebView view, String url, String message, JsResult result) {
                new AlertDialog.Builder(MainActivity.this)
                        .setMessage(message)
                        .setPositiveButton("确定", (d, w) -> result.confirm())
                        .setOnCancelListener(d -> result.cancel())
                        .show();
                return true;
            }

            @Override
            public boolean onJsConfirm(WebView view, String url, String message, JsResult result) {
                new AlertDialog.Builder(MainActivity.this)
                        .setMessage(message)
                        .setPositiveButton("确定", (d, w) -> result.confirm())
                        .setNegativeButton("取消", (d, w) -> result.cancel())
                        .setOnCancelListener(d -> result.cancel())
                        .show();
                return true;
            }
        });
    }

    // ---------------- 权限 ----------------

    /** 按系统版本返回需要运行时申请的权限 */
    private String[] needLocation() {
        if (Build.VERSION.SDK_INT >= 31) {
            return new String[]{
                    Manifest.permission.ACCESS_FINE_LOCATION,
                    Manifest.permission.ACCESS_COARSE_LOCATION,
                    Manifest.permission.BLUETOOTH_SCAN,
                    Manifest.permission.BLUETOOTH_CONNECT};
        }
        return new String[]{
                Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.ACCESS_COARSE_LOCATION};
    }

    private boolean hasPermissions(String[] perms) {
        for (String p : perms) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) return false;
        }
        return true;
    }

    private void requestRuntimePermissions() {
        String[] perms = needLocation();
        if (!hasPermissions(perms)) {
            requestPermissions(perms, REQ_PERMS);
        }
    }

    // ---------------- WebView 工具（供 BleBridge 回调 JS） ----------------

    /** 在 UI 线程执行 JS 片段 */
    public void postJs(final String js) {
        runOnUiThread(() -> {
            if (web != null) {
                web.evaluateJavascript(js, null);
            }
        });
    }

    // ---------------- 生命周期与返回键 ----------------

    @Override
    protected void onPause() {
        if (web != null) web.onPause();
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (web != null) web.onResume();
    }

    @Override
    protected void onDestroy() {
        if (ble != null) ble.shutdown(); // 停止蓝牙与自动重连（静默，不再回调 JS）
        if (web != null) web.destroy();
        super.onDestroy();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK && web != null && web.canGoBack()) {
            web.goBack();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }
}

