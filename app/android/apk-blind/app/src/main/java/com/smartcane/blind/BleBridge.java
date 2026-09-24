package com.smartcane.blind;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.webkit.JavascriptInterface;

import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.LinkedList;
import java.util.Queue;
import java.util.UUID;

/**
 * 原生 BLE 桥：WebView 不支持 Web Bluetooth，这里用系统 BluetoothGatt
 * 实现 Nordic UART (NUS) 扫描 / 连接 / 订阅 / 写入，通过 evaluateJavascript
 * 回调 window.__nativeBle.*（由 webapp/js/services/ble.js 注册）。
 *
 * 对应原 App Inventor 的 BluetoothLE 扩展行为：
 *   扫描并连接 → RegisterForStrings(TX NOTIFY) → WriteStrings(RX)
 */
public class BleBridge {

    /** JS 侧通过 window.SmartCaneNative 访问本对象 */
    public static final String JS_NAME = "SmartCaneNative";

    private static final UUID CCC = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final long SCAN_TIMEOUT_MS = 15000;

    private final Activity activity;
    private final Handler main = new Handler(Looper.getMainLooper());

    // 连接参数（由 JS 传入，取自 webapp 配置）
    private String serviceUuid, rxUuid, txUuid, namePrefix;

    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rxChar; // 手机 → 盲杖
    private BluetoothGattCharacteristic txChar; // 盲杖 → 手机
    private volatile boolean connected = false;
    private boolean scanning = false;

    // 串行写队列（GATT 同时只能有一个写请求在途）
    private final Queue<byte[]> writeQueue = new LinkedList<>();
    private volatile boolean writing = false;

    public BleBridge(Activity activity) {
        this.activity = activity;
    }

    // ------------------------------------------------------------------
    // JS 可调用接口（@JavascriptInterface 运行在 JS 桥线程，一律切主线程）
    // ------------------------------------------------------------------

    /** 开始扫描并自动连接第一个匹配设备（等价原 ListPicker 选择 + Connect） */
    @JavascriptInterface
    public void connect(String serviceUuid, String rxUuid, String txUuid, String namePrefix) {
        this.serviceUuid = lower(serviceUuid);
        this.rxUuid = lower(rxUuid);
        this.txUuid = lower(txUuid);
        this.namePrefix = namePrefix == null ? "" : namePrefix.trim();
        main.post(this::startScan);
    }

    /** 发送一行文本（自动补换行，等价原 WriteStrings） */
    @JavascriptInterface
    public void write(final String text) {
        final byte[] data = (text + "\n").getBytes(StandardCharsets.UTF_8);
        main.post(() -> {
            if (!connected || rxChar == null) {
                emitError("请先连接智能盲杖");
                return;
            }
            writeQueue.add(data);
            pumpWrite();
        });
    }

    /** 断开连接 */
    @JavascriptInterface
    public void disconnect() {
        main.post(this::closeGatt);
    }

    @JavascriptInterface
    public boolean isConnected() {
        return connected;
    }

    // ------------------------------------------------------------------
    // 扫描
    // ------------------------------------------------------------------

    private void startScan() {
        if (!hasBlePermissions()) {
            emitError("缺少蓝牙/定位权限，请到系统设置中授予后重试");
            return;
        }
        BluetoothManager bm = (BluetoothManager) activity.getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = bm != null ? bm.getAdapter() : null;
        if (adapter == null || !adapter.isEnabled()) {
            emitError("手机蓝牙未开启，请先打开蓝牙");
            return;
        }
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            emitError("蓝牙不可用（扫描器获取失败）");
            return;
        }
        closeGatt();
        scanning = true;
        emit("window.__nativeBle&&window.__nativeBle.onScanning&&window.__nativeBle.onScanning()");
        // 不加服务过滤：部分固件不广播 128 位服务 UUID，按「名字前缀或广播含 NUS」匹配
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        try {
            scanner.startScan(null, settings, scanCallback);
        } catch (Exception e) {
            scanning = false;
            emitError("扫描启动失败：" + e.getMessage());
        }
    }

    private void stopScan() {
        scanning = false;
        try {
            if (scanner != null) scanner.stopScan(scanCallback);
        } catch (Exception ignored) {
        }
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            if (!scanning) return;
            BluetoothDevice dev = result.getDevice();
            String name = dev.getName() != null ? dev.getName() : "";
            boolean byName = namePrefix.isEmpty()
                    || name.toLowerCase().startsWith(namePrefix.toLowerCase());
            boolean byUuid = false;
            if (!byName && serviceUuid != null) {
                ScanRecord rec = result.getScanRecord();
                if (rec != null && rec.getServiceUuids() != null) {
                    for (ParcelUuid u : rec.getServiceUuids()) {
                        if (serviceUuid.equals(u.getUuid().toString())) {
                            byUuid = true;
                            break;
                        }
                    }
                }
            }
            if (byName || byUuid) {
                stopScan();
                connectGatt(dev);
            }
        }

        @Override
        public void onScanFailed(int errorCode) {
            scanning = false;
            emitError("扫描失败，错误码 " + errorCode);
        }
    };

    // ------------------------------------------------------------------
    // GATT 连接与数据
    // ------------------------------------------------------------------

    private void connectGatt(BluetoothDevice dev) {
        String name = dev.getName() != null ? dev.getName() : "未知设备";
        emit("window.__nativeBle&&window.__nativeBle.onConnecting&&window.__nativeBle.onConnecting("
                + quote(name) + ")");
        try {
            gatt = dev.connectGatt(activity, false, gattCallback);
        } catch (SecurityException e) {
            emitError("连接被系统拒绝（缺少蓝牙权限）：" + e.getMessage());
        }
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            if (newState == android.bluetooth.BluetoothProfile.STATE_CONNECTED) {
                try {
                    g.discoverServices();
                } catch (SecurityException e) {
                    emitError("发现服务失败（缺少蓝牙权限）");
                }
            } else if (newState == android.bluetooth.BluetoothProfile.STATE_DISCONNECTED) {
                boolean was = connected;
                connected = false;
                writeQueue.clear();
                writing = false;
                closeGattQuietly(g);
                emit("window.__nativeBle&&window.__nativeBle.onDisconnected&&window.__nativeBle.onDisconnected({reason:"
                        + (was ? "'disconnected'" : "'connect-failed'") + "})");
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                emitError("发现服务失败，status=" + status);
                return;
            }
            BluetoothGattService svc = g.getService(UUID.fromString(serviceUuid));
            if (svc == null) {
                emitError("设备未提供 NUS 服务（固件不匹配？）");
                closeGatt();
                return;
            }
            rxChar = svc.getCharacteristic(UUID.fromString(rxUuid));
            txChar = svc.getCharacteristic(UUID.fromString(txUuid));
            if (rxChar == null || txChar == null) {
                emitError("未找到 NUS 收发特征");
                closeGatt();
                return;
            }
            // 订阅 TX NOTIFY（等价原 RegisterForStrings）
            g.setCharacteristicNotification(txChar, true);
            BluetoothGattDescriptor d = txChar.getDescriptor(CCC);
            if (d != null) {
                try {
                    d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    g.writeDescriptor(d);
                } catch (SecurityException e) {
                    emitError("订阅通知失败（缺少蓝牙权限）");
                }
            } else {
                onReady(g);
            }
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            if (d != null && CCC.equals(d.getUuid())) {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    onReady(g);
                } else {
                    emitError("订阅通知失败，status=" + status);
                }
            }
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            writing = false;
            pumpWrite();
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            // API 33 起有新签名，旧签名仍会回调到此处；此处只关心 TX 通知
            if (c == null || c.getUuid() == null || !c.getUuid().toString().equals(txUuid)) return;
            String text = new String(value == null ? new byte[0] : value, StandardCharsets.UTF_8);
            emit("window.__nativeBle&&window.__nativeBle.onLine&&window.__nativeBle.onLine(" + quote(text) + ")");
        }
    };

    /** 订阅成功 → 通知 JS 已连接（行拆分在 JS 侧 protocol.splitLines 完成） */
    private void onReady(BluetoothGatt g) {
        connected = true;
        String name = "盲杖";
        try {
            BluetoothDevice dev = g.getDevice();
            if (dev != null && dev.getName() != null) name = dev.getName();
        } catch (SecurityException ignored) {
        }
        emit("window.__nativeBle&&window.__nativeBle.onConnected&&window.__nativeBle.onConnected("
                + quote(name) + ")");
        pumpWrite();
    }

    // ---------------- 写队列（GATT 写必须串行） ----------------

    private synchronized void pumpWrite() {
        if (writing || !connected || gatt == null || rxChar == null) return;
        byte[] data = writeQueue.poll();
        if (data == null) return;
        writing = true;
        try {
            rxChar.setValue(data);
            rxChar.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            if (!gatt.writeCharacteristic(rxChar)) {
                writing = false;
                emitError("指令发送失败（写入被拒绝）");
            }
        } catch (SecurityException e) {
            writing = false;
            emitError("发送失败（缺少蓝牙权限）");
        }
    }

    // ---------------- 清理与工具 ----------------

    private void closeGatt() {
        closeGattQuietly(gatt);
        gatt = null;
        rxChar = null;
        txChar = null;
        connected = false;
        writeQueue.clear();
        writing = false;
    }

    private void closeGattQuietly(BluetoothGatt g) {
        if (g == null) return;
        try {
            g.disconnect();
            g.close();
        } catch (Exception ignored) {
        }
    }

    private boolean hasBlePermissions() {
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            return granted(Manifest.permission.BLUETOOTH_SCAN)
                    && granted(Manifest.permission.BLUETOOTH_CONNECT);
        }
        return granted(Manifest.permission.ACCESS_FINE_LOCATION);
    }

    private boolean granted(String perm) {
        return activity.checkSelfPermission(perm) == PackageManager.PERMISSION_GRANTED;
    }

    private static String lower(String s) {
        return s == null ? null : s.trim().toLowerCase();
    }

    /** JSON 字符串转义（org.json 自带，保证换行/引号安全注入 JS） */
    private static String quote(String s) {
        return JSONObject.quote(s == null ? "" : s);
    }

    private void emit(final String js) {
        ((MainActivity) activity).postJs(js);
    }

    private void emitError(String msg) {
        emit("window.__nativeBle&&window.__nativeBle.onError&&window.__nativeBle.onError(" + quote(msg) + ")");
    }
}




