package com.smartcane.family;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.view.KeyEvent;
import android.webkit.GeolocationPermissions;
import android.webkit.JsResult;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * 家属端壳：全屏 WebView 加载打包在 assets 里的 webapp/family.html。
 * 纯网络轮询（Firebase + 识别服务器 + 高德地图），不需要原生 BLE 桥。
 */
public class MainActivity extends Activity {

    public static final int REQ_PERMS = 1001;

    private WebView web;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        web = new WebView(this);
        setContentView(web);

        setupWebView();
        requestLocationPermission();
        web.loadUrl("file:///android_asset/webapp/family.html");
    }

    private void setupWebView() {
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);                 // localStorage：配置持久化
        s.setGeolocationEnabled(true);                // 地图定位
        s.setMediaPlaybackRequiresUserGesture(false); // 报警提示音/语音
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW); // 直连 http 识别服务器画面

        web.setWebViewClient(new WebViewClient());
        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onGeolocationPermissionsShowPrompt(String origin,
                    GeolocationPermissions.Callback callback) {
                boolean ok = hasLocationPermission();
                callback.invoke(origin, ok, false);
                if (!ok) requestLocationPermission();
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

    private boolean hasLocationPermission() {
        return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                == PackageManager.PERMISSION_GRANTED;
    }

    private void requestLocationPermission() {
        if (!hasLocationPermission()) {
            requestPermissions(new String[]{
                    Manifest.permission.ACCESS_FINE_LOCATION,
                    Manifest.permission.ACCESS_COARSE_LOCATION}, REQ_PERMS);
        }
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
