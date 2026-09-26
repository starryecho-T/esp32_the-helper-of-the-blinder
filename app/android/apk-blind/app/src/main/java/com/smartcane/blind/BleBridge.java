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
 *
 * 自动重连（重连状态仅在主线程读写，Binder 回调一律 post 到主线程）：
 *   - 连接建立后意外掉线（用户未主动断开）→ 指数退避重连 1s→2s→4s→…封顶 15s，无限重试；
 *     优先对 lastDevice 直连，每 RESCAN_EVERY 次插入一轮重新扫描兜底。
 *   - 首次连接（从未连上过）失败 → 重试 INIT_MAX_ATTEMPTS 次后放弃并报错。
 *   - JS 主动 disconnect() / Activity 销毁 → 立即停止一切重连。
 *   - 单轮扫描 / 单次 GATT 连接均有超时，保证重连循环不会被卡死。
 */
public class BleBridge {

    /** JS 侧通过 window.SmartCaneNative 访问本对象 */
    public static final String JS_NAME = "SmartCaneNative";

    private static final UUID CCC = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    private static final long SCAN_TIMEOUT_MS = 15000;     // 单轮扫描超时
    private static final long CONNECT_TIMEOUT_MS = 12000;  // 单次 GATT 连接（到订阅完成）超时
    private static final long RECONNECT_FIRST_MS = 1000;   // 重连退避起点
    private static final long RECONNECT_MAX_MS = 15000;    // 重连退避上限
    private static final int INIT_MAX_ATTEMPTS = 3;        // 首次连接最多尝试次数
    private static final int RESCAN_EVERY = 3;             // 每 N 次重连插入一轮重新扫描

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
    private volatile boolean scanning = false;

    // —— 自动重连状态（仅在主线程读写）——
    private volatile boolean wantConnected = false;  // 用户意图：connect() 后为真，disconnect()/shutdown() 后为假
    private boolean everConnected = false;           // 本次会话是否成功连上过（决定无限重连还是有限重试）
    private BluetoothDevice lastDevice = null;       // 上次（尝试）连接的设备，重连优先直连
    private int attempt = 0;                         // 已连续失败的尝试次数（连接成功后清零）
    private volatile boolean destroyed = false;      // Activity 已销毁，拦截一切 JS 回调

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
        main.post(() -> {
            cancelPending();      // 掐掉可能还在跑的旧扫描 / 旧重连任务
            closeGatt();
            wantConnected = true;
            everConnected = false;
            attempt = 0;
            startScan();          // 首次连接仍从扫描开始（等价原「选择设备并连接」）
        });
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

    /** 断开连接并停止自动重连（onDisconnected 恰好回调一次，供 JS 复位 UI） */
    @JavascriptInterface
    public void disconnect() {
        main.post(() -> {
            wantConnected = false;
            cancelPending();
            closeGatt();
            // closeGatt 已置空 gatt，Binder 迟到的断开回调会被 handleStateChange
            // 的过期回调守卫拦下，因此在这里同步通知（恰好一次）
            emitDisconnected("disconnected");
        });
    }

    /** Activity onDestroy 调用：静默关闭蓝牙并停止一切任务（不再回调 JS） */
    public void shutdown() {
        destroyed = true;
        main.post(() -> {
            wantConnected = false;
            cancelPending();
            closeGatt();
        });
    }

    @JavascriptInterface
    public boolean isConnected() {
        return connected;
    }

    // ------------------------------------------------------------------
    // 扫描
    // ------------------------------------------------------------------

    private void startScan() {
        // UUID 来自 webapp 配置，非法时后续 getService/fromString 会在主线程抛
        // IllegalArgumentException 直接闪退，这里提前拦下并给出可读提示
        if (!uuidOk(serviceUuid) || !uuidOk(rxUuid) || !uuidOk(txUuid)) {
            giveUp("蓝牙 UUID 配置无效，请到配置页检查后重试");
            return;
        }
        if (!hasBlePermissions()) {
            giveUp("缺少蓝牙/定位权限，请到系统设置中授予后重试");
            return;
        }
        BluetoothManager bm = (BluetoothManager) activity.getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = bm != null ? bm.getAdapter() : null;
        if (adapter == null || !adapter.isEnabled()) {
            onAttemptFailed("手机蓝牙未开启");   // 用户可能正要开蓝牙，静默重试即可
            return;
        }
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            onAttemptFailed("蓝牙不可用（扫描器获取失败）");
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
            main.postDelayed(scanTimeout, SCAN_TIMEOUT_MS);
        } catch (Exception e) {
            scanning = false;
            giveUp("扫描启动失败：" + e.getMessage());
        }
    }

    private void stopScan() {
        scanning = false;
        main.removeCallbacks(scanTimeout);
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
            String name = safeName(dev);
            // 注意：周围常有无广播名的 BLE 设备（耳机/手环/信标），getName() 会合法地返回
            // null（safeName 只拦 SecurityException），判空前缀必须空安全，否则 NPE 直接闪退
            boolean byName = namePrefix.isEmpty()
                    || (name != null && name.toLowerCase().startsWith(namePrefix.toLowerCase()));
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
                // Binder 线程 → 主线程串行处理，防止同一设备触发两次连接
                main.post(() -> {
                    if (!scanning) return;
                    stopScan();
                    connectDevice(dev);
                });
            }
        }

        @Override
        public void onScanFailed(int errorCode) {
            main.post(() -> {
                scanning = false;
                main.removeCallbacks(scanTimeout);
                giveUp("蓝牙扫描失败（错误码 " + errorCode + "），请关闭再打开手机蓝牙后重试");
            });
        }
    };

    // ------------------------------------------------------------------
    // 自动重连引擎（全部在主线程执行）
    // ------------------------------------------------------------------

    /** 扫描超时：本轮没扫到匹配设备 → 记一次失败并按策略重试 */
    private final Runnable scanTimeout = new Runnable() {
        @Override
        public void run() {
            if (!scanning) return;
            stopScan();
            onAttemptFailed("扫描超时：附近没有发现盲杖");
        }
    };

    /** 连接超时：connectGatt 可能长时间无回调，防止重连循环被卡死 */
    private final Runnable connectTimeout = new Runnable() {
        @Override
        public void run() {
            if (connected || gatt == null) return;
            BluetoothGatt g = gatt;
            gatt = null;
            closeGattQuietly(g);
            onAttemptFailed("连接超时");
        }
    };

    /** 到点的重连动作：优先直连上次设备，每 RESCAN_EVERY 次重新扫描一轮 */
    private final Runnable reconnectTask = new Runnable() {
        @Override
        public void run() {
            if (!wantConnected || connected) return;
            if (lastDevice != null && attempt % RESCAN_EVERY != 0) {
                connectDevice(lastDevice);
            } else {
                startScan();
            }
        }
    };

    /** 单次尝试失败的统一入口（静默计数，达到放弃条件才报错，避免重试刷屏） */
    private void onAttemptFailed(String why) {
        if (!wantConnected || connected) return;
        attempt++;
        if (!everConnected && attempt >= INIT_MAX_ATTEMPTS) {
            giveUp(why + "（已尝试 " + attempt + " 次），请确认盲杖已开机并在附近");
            return;
        }
        scheduleNextAttempt();
    }

    /** 按指数退避安排下一次尝试：1s → 2s → 4s → … 封顶 15s */
    private void scheduleNextAttempt() {
        if (!wantConnected || connected) return;
        long delay = RECONNECT_FIRST_MS;
        for (int i = 1; i < attempt && delay < RECONNECT_MAX_MS; i++) delay *= 2;
        if (delay > RECONNECT_MAX_MS) delay = RECONNECT_MAX_MS;
        emit("window.__nativeBle&&window.__nativeBle.onReconnecting&&window.__nativeBle.onReconnecting("
                + attempt + "," + delay + ")");
        main.postDelayed(reconnectTask, delay);
    }

    /** 放弃本次连接会话：停任务 + 报错 + 通知 JS 复位 UI */
    private void giveUp(String msg) {
        wantConnected = false;
        cancelPending();
        closeGatt();
        emitError(msg);
        emitDisconnected("connect-failed");
    }

    /** 取消所有挂起任务（扫描超时 / 连接超时 / 重连任务） */
    private void cancelPending() {
        main.removeCallbacks(scanTimeout);
        main.removeCallbacks(connectTimeout);
        main.removeCallbacks(reconnectTask);
        if (scanning) stopScan();
    }

    // ------------------------------------------------------------------
    // GATT 连接与数据
    // ------------------------------------------------------------------

    private void connectDevice(BluetoothDevice dev) {
        main.removeCallbacks(connectTimeout);
        lastDevice = dev;
        String name = safeName(dev);
        name = (name == null || name.isEmpty()) ? "未知设备" : name;
        emit("window.__nativeBle&&window.__nativeBle.onConnecting&&window.__nativeBle.onConnecting("
                + quote(name) + ")");
        try {
            gatt = dev.connectGatt(activity, false, gattCallback);
            if (gatt == null) {
                onAttemptFailed("连接建立失败");
                return;
            }
            main.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS);
        } catch (SecurityException e) {
            gatt = null;
            giveUp("连接被系统拒绝（缺少蓝牙权限）：" + e.getMessage());
        }
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            main.post(() -> handleStateChange(g, newState));
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            main.post(() -> handleServicesDiscovered(g, status));
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            main.post(() -> handleDescriptorWrite(g, d, status));
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            writing = false;
            pumpWrite();
        }

        /** 统一的 TX 通知处理（新旧两个签名共用；行拆分在 JS 侧 protocol.splitLines 完成） */
        private void handleNotify(BluetoothGattCharacteristic c, byte[] value) {
            if (c == null || c.getUuid() == null || txUuid == null
                    || !c.getUuid().toString().equals(txUuid)) return;
            String text = new String(value == null ? new byte[0] : value, StandardCharsets.UTF_8);
            emit("window.__nativeBle&&window.__nativeBle.onLine&&window.__nativeBle.onLine(" + quote(text) + ")");
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            // API 33+ 走此签名（本机一加 Android 16 即此路径）
            handleNotify(c, value);
        }

        @SuppressWarnings("deprecation")
        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            // API 32 及以下走旧签名（不带 value 参数），数据从特征里取；
            // 不补这个重写在旧系统上会「连得上但收不到任何数据」
            handleNotify(c, c == null ? null : c.getValue());
        }
    };

    /** 连接状态变化（主线程，由 Binder 回调转发） */
    private void handleStateChange(BluetoothGatt g, int newState) {
        if (gatt == null || g != gatt) return;   // 过期回调：连接已被替换或清理
        if (newState == android.bluetooth.BluetoothProfile.STATE_CONNECTED) {
            // 超时继续守到「订阅完成」（onReady 才取消），这里只推进服务发现
            main.removeCallbacks(connectTimeout);
            main.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS);
            try {
                g.discoverServices();
            } catch (SecurityException e) {
                giveUp("发现服务失败（缺少蓝牙权限）");
            }
        } else if (newState == android.bluetooth.BluetoothProfile.STATE_DISCONNECTED) {
            main.removeCallbacks(connectTimeout);
            boolean was = connected;
            connected = false;
            writeQueue.clear();
            writing = false;
            closeGattQuietly(g);
            if (gatt == g) gatt = null;
            if (!wantConnected) {
                // 用户已主动断开（或已放弃）：只通知 JS，不再重试
                emitDisconnected(was ? "disconnected" : "connect-failed");
                return;
            }
            if (was) {
                // 连接建立后意外掉线 → 自动重连（无限次，退避封顶 15s）
                emitDisconnected("lost");
                attempt = 1;              // 掉线本身算第 1 次失败，退避从 1s 开始
                scheduleNextAttempt();
            } else {
                onAttemptFailed("连接失败");
            }
        }
    }

    /** 服务发现完成（主线程，由 Binder 回调转发） */
    private void handleServicesDiscovered(BluetoothGatt g, int status) {
        if (gatt == null || g != gatt) return;
        if (status != BluetoothGatt.GATT_SUCCESS) {
            closeGattQuietly(g);
            gatt = null;
            onAttemptFailed("发现服务失败，status=" + status);
            return;
        }
        BluetoothGattService svc = g.getService(UUID.fromString(serviceUuid));
        if (svc == null) {
            // 连上了但不是盲杖固件 → 重试无意义，直接放弃
            giveUp("设备未提供 NUS 服务（固件不匹配？）");
            return;
        }
        rxChar = svc.getCharacteristic(UUID.fromString(rxUuid));
        txChar = svc.getCharacteristic(UUID.fromString(txUuid));
        if (rxChar == null || txChar == null) {
            giveUp("未找到 NUS 收发特征");
            return;
        }
        try {
            // 订阅 TX NOTIFY（等价原 RegisterForStrings）
            g.setCharacteristicNotification(txChar, true);
            BluetoothGattDescriptor d = txChar.getDescriptor(CCC);
            if (d != null) {
                d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                g.writeDescriptor(d);
            } else {
                onReady(g);
            }
        } catch (SecurityException e) {
            giveUp("订阅通知失败（缺少蓝牙权限）");
        }
    }

    /** CCC 描述符写入完成（主线程，由 Binder 回调转发） */
    private void handleDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int status) {
        if (gatt == null || g != gatt || d == null || !CCC.equals(d.getUuid())) return;
        if (status == BluetoothGatt.GATT_SUCCESS) {
            onReady(g);
        } else {
            closeGattQuietly(g);
            gatt = null;
            onAttemptFailed("订阅通知失败，status=" + status);
        }
    }

    /** 订阅成功 → 通知 JS 已连接（行拆分在 JS 侧 protocol.splitLines 完成） */
    private void onReady(BluetoothGatt g) {
        connected = true;
        everConnected = true;      // 从此掉线进入「无限自动重连」
        attempt = 0;               // 清零失败计数，下次掉线退避从 1s 重新开始
        lastDevice = g.getDevice();
        main.removeCallbacks(connectTimeout);
        String name = "盲杖";
        String n = safeName(g.getDevice());
        if (n != null && !n.isEmpty()) name = n;
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
        main.removeCallbacks(connectTimeout);
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

    /** UUID 合法性校验（空串 / 非法格式均返回 false） */
    private static boolean uuidOk(String s) {
        if (s == null || s.isEmpty()) return false;
        try {
            UUID.fromString(s);
            return true;
        } catch (IllegalArgumentException e) {
            return false;
        }
    }

    /** JSON 字符串转义（org.json 自带，保证换行/引号安全注入 JS） */
    private static String quote(String s) {
        return JSONObject.quote(s == null ? "" : s);
    }

    /** 读设备名（Android 12+ 缺 BLUETOOTH_CONNECT 时 getName 会抛 SecurityException） */
    private static String safeName(BluetoothDevice dev) {
        try {
            return dev == null ? null : dev.getName();
        } catch (SecurityException e) {
            return null;
        }
    }

    private void emit(final String js) {
        if (destroyed) return;   // WebView 已销毁，不再注入 JS
        ((MainActivity) activity).postJs(js);
    }

    private void emitError(String msg) {
        emit("window.__nativeBle&&window.__nativeBle.onError&&window.__nativeBle.onError(" + quote(msg) + ")");
    }

    private void emitDisconnected(String reason) {
        emit("window.__nativeBle&&window.__nativeBle.onDisconnected&&window.__nativeBle.onDisconnected({reason:"
                + quote(reason) + "})");
    }
}




