from fastapi import FastAPI, Request, HTTPException, Header
from fastapi.responses import PlainTextResponse, Response
from ultralytics import YOLO
import cv2
import numpy as np
import requests
import os
import threading
import time

app = FastAPI(title='Smart Cane Traffic Light Detector')

# ---------------------------------------------------------------------------
# CORS：手机 App（WebView / 浏览器）通过 fetch 调用 /detect、/snapshot，
# 没有下面这段会被浏览器跨域策略拦截（App Inventor 原生 HTTP 不受影响）。
# ---------------------------------------------------------------------------
from fastapi.middleware.cors import CORSMiddleware  # noqa: E402

app.add_middleware(
    CORSMiddleware,
    allow_origins=['*'],        # 演示环境放开；生产建议改成具体域名
    allow_methods=['*'],
    allow_headers=['*'],
)


# COCO-trained YOLO model. It detects the object "traffic light".
# The color is classified from the detected traffic-light crop.
MODEL_PATH = os.environ.get('YOLO_MODEL', 'yolov8n.pt')
model = YOLO(MODEL_PATH)
TRAFFIC_LIGHT_CLASS = 9  # COCO: traffic light

# ---------------------------------------------------------------------------
# 帧获取方式
#
# 部署到云服务器后，ESP32-CAM 在家/校园内网里，服务器无法反向访问它。
# 因此主流程改为「设备主动推送」：
#   ESP32-CAM --POST /upload--> 云服务器（缓存最新一帧） <--GET /detect-- 手机 App
# 只有当 cam 参数被显式传入（服务器与摄像头同网时）才走老的拉取模式。
# ---------------------------------------------------------------------------
CAMERA_DEFAULT_URL = os.environ.get('CAMERA_URL', '')   # 可选：能直连时才配
CAMERA_TIMEOUT = float(os.environ.get('CAMERA_TIMEOUT', '5'))  # seconds

# 设备上传鉴权：设置后，/upload 必须带 ?token=xxx 或 X-Device-Token 头
DEVICE_TOKEN = os.environ.get('DEVICE_TOKEN', '')
# 家属端查看画面的鉴权：留空则任何人都能看，建议设一个（与 DEVICE_TOKEN 不同）
VIEW_TOKEN = os.environ.get('VIEW_TOKEN', '')
# 缓存帧最长可用时间（秒）。超过则认为设备离线/断流
# 必须 > 前哨推流间隔（当前固件 30 秒），否则 /detect、/snapshot 会在帧过期窗口报错
FRAME_MAX_AGE = float(os.environ.get('FRAME_MAX_AGE', '45'))
# 缓存帧落盘路径，便于调试查看（重启后仍保留最后一帧）
FRAME_PATH = os.environ.get('FRAME_PATH', 'latest.jpg')

_frame_lock = threading.Lock()
_latest_frame = {'data': b'', 'ts': 0.0}


def remember_frame(data: bytes) -> None:
    """保存设备上传的最新一帧（内存缓存 + 落盘）。"""
    with _frame_lock:
        _latest_frame['data'] = data
        _latest_frame['ts'] = time.time()
    try:
        with open(FRAME_PATH, 'wb') as fh:
            fh.write(data)
    except Exception:
        pass  # 落盘失败不影响主流程


def cached_frame(max_age: float = FRAME_MAX_AGE):
    """返回 (bytes, age)；帧过旧或不存在时返回 (None, age|None)。"""
    with _frame_lock:
        data, ts = _latest_frame['data'], _latest_frame['ts']
    if not data or ts <= 0:
        return None, None
    age = time.time() - ts
    if age > max_age:
        return None, age
    return data, age


def color_score(crop: np.ndarray):
    if crop is None or crop.size == 0:
        return None, 0.0

    hsv = cv2.cvtColor(crop, cv2.COLOR_BGR2HSV)
    h, s, v = cv2.split(hsv)
    bright = v > 70
    saturated = s > 60
    mask = bright & saturated

    scores = {}
    # Red wraps around HSV hue 0/179.
    scores['RED'] = float(np.mean(mask & ((h < 10) | (h > 170))))
    scores['YELLOW'] = float(np.mean(mask & (h >= 18) & (h <= 40)))
    scores['GREEN'] = float(np.mean(mask & (h >= 40) & (h <= 95)))

    # Prefer the color with the strongest evidence. Require a small margin
    # so random colored pixels are less likely to be reported as a signal.
    color = max(scores, key=scores.get)
    value = scores[color]
    ordered = sorted(scores.values(), reverse=True)
    margin = ordered[0] - ordered[1] if len(ordered) > 1 else ordered[0]
    confidence = min(0.99, max(0.20, 0.50 + 4.0 * value + 2.0 * margin))
    return color, confidence


def detect_from_bytes(data: bytes) -> str:
    """Run the traffic-light pipeline on raw image bytes and return 'COLOR|NN%'."""
    if not data:
        raise HTTPException(status_code=400, detail='empty image')

    arr = np.frombuffer(data, dtype=np.uint8)
    image = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if image is None:
        raise HTTPException(status_code=400, detail='invalid image')

    results = model.predict(image, verbose=False, conf=0.25)
    best = None

    for result in results:
        boxes = result.boxes
        if boxes is None:
            continue
        # 直接取整个数组再逐行处理，避免不同 ultralytics 版本里 Box 对象的维度差异
        if len(boxes) == 0:
            continue
        xyxy = boxes.xyxy.cpu().numpy()   # (n, 4)
        confs = boxes.conf.cpu().numpy()  # (n,)
        clss = boxes.cls.cpu().numpy()    # (n,)

        for i in range(len(xyxy)):
            if int(clss[i]) != TRAFFIC_LIGHT_CLASS:
                continue
            det_conf = float(confs[i])
            x1, y1, x2, y2 = map(int, xyxy[i].tolist())
            x1, y1 = max(0, x1), max(0, y1)
            x2, y2 = min(image.shape[1], x2), min(image.shape[0], y2)
            crop = image[y1:y2, x1:x2]
            color, color_conf = color_score(crop)
            if color is None:
                continue
            combined = 0.65 * det_conf + 0.35 * color_conf
            if best is None or combined > best[0]:
                best = (combined, color, det_conf)

    if best is None:
        return 'NONE|0'

    _, color, det_conf = best
    # Return the format expected by the App Inventor client: COLOR|NN%.
    return f'{color}|{det_conf * 100:.0f}%'


@app.get('/health', response_class=PlainTextResponse)
def health():
    return 'OK'


@app.get('/status', response_class=PlainTextResponse)
def status():
    """调试用：查看设备是否还在推送帧、最新帧有多旧。"""
    data, age = cached_frame()
    if data is None:
        state = f'no-frame age={age if age else "n/a"}'
    else:
        state = f'frame bytes={len(data)} age={age:.1f}s'
    return f'OK {state} model={MODEL_PATH}'


@app.post('/upload', response_class=PlainTextResponse)
async def upload(request: Request, token: str = '',
                 x_device_token: str = Header(default='')):
    """ESP32-CAM 主动上传一帧 JPEG：POST /upload?token=<token>，body 为图片二进制。

    只要设备能上网（不需要公网 IP、不需要端口映射）就能把画面送到云端；
    服务器只保存最新一帧，随后由 GET /detect 消费。
    """
    if DEVICE_TOKEN and token != DEVICE_TOKEN and x_device_token != DEVICE_TOKEN:
        raise HTTPException(status_code=401, detail='bad token')

    data = await request.body()
    if not data:
        raise HTTPException(status_code=400, detail='empty image')

    remember_frame(data)
    return 'OK'


@app.get('/detect', response_class=PlainTextResponse)
def detect_from_camera(cam: str = ''):
    """GET /detect

    默认使用 ESP32-CAM 推送上来的最新一帧；
    若显式传 ?cam=<snapshot-url> 且服务器能直连该地址，则临时去拉一张。
    """
    if cam:
        try:
            r = requests.get(cam, timeout=CAMERA_TIMEOUT)
            r.raise_for_status()
        except Exception as exc:
            raise HTTPException(status_code=502, detail=f'camera unreachable: {exc}')
        return detect_from_bytes(r.content)

    data, age = cached_frame()
    if data is None:
        if CAMERA_DEFAULT_URL:
            try:
                r = requests.get(CAMERA_DEFAULT_URL, timeout=CAMERA_TIMEOUT)
                r.raise_for_status()
                return detect_from_bytes(r.content)
            except Exception as exc:
                raise HTTPException(status_code=502, detail=f'camera unreachable: {exc}')
        raise HTTPException(
            status_code=503,
            detail=f'no fresh frame from device (age={age if age else "n/a"})')

    return detect_from_bytes(data)


@app.post('/detect', response_class=PlainTextResponse)
async def detect(request: Request):
    """手机直接上传图片做识别（不经过摄像头缓存）。"""
    data = await request.body()
    return detect_from_bytes(data)


@app.get('/snapshot')
def snapshot(t: str = '', token: str = ''):
    """返回设备上传的最新一帧 JPEG，供家属端 App 直接当图片显示。

    用法：Image 组件的 Picture 填
        http://<服务器>/snapshot?t=<毫秒时间戳>
    带上变化的 t 参数是为了绕开 App Inventor / 系统的图片缓存。
    VIEW_TOKEN 非空时还需要 ?token=xxx。
    """
    if VIEW_TOKEN and token != VIEW_TOKEN:
        raise HTTPException(status_code=401, detail='bad token')

    data, age = cached_frame()
    if data is None:
        raise HTTPException(status_code=503, detail=f'no fresh frame (age={age})')

    return Response(
        content=data,
        media_type='image/jpeg',
        headers={'Cache-Control': 'no-store'},
    )


@app.get('/latest.jpg')
def latest_jpg(token: str = ''):
    """/snapshot 的别名，方便直接在浏览器里打开查看。"""
    return snapshot(token=token)
