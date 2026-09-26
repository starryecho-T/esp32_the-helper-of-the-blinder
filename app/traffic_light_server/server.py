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
# 障碍物检测（第二个 YOLO 模型，yolov8s 比 yolov8n 精度更高）
#
# 把 COCO 80 类归并成 App 关心的 4 大类：
#   PEDESTRIAN 行人 / VEHICLE 车辆 / ANIMAL 动物 / FACILITY 静态设施
# 交通灯（class 9）不参与障碍物统计，仍走 /detect 专用链路。
# ---------------------------------------------------------------------------
BARRIER_MODEL_PATH = os.environ.get('BARRIER_MODEL', 'yolov8s.pt')
BARRIER_CONF = float(os.environ.get('BARRIER_CONF', '0.35'))   # 障碍物置信度阈值
MAX_BARRIER_OBJECTS = int(os.environ.get('MAX_BARRIER_OBJECTS', '10'))  # 明细最多返回几个
barrier_model = YOLO(BARRIER_MODEL_PATH)

CATEGORY_ORDER = ['PEDESTRIAN', 'VEHICLE', 'ANIMAL', 'FACILITY']
CATEGORY_GROUPS = {
    # person
    'PEDESTRIAN': {0},
    # bicycle car motorcycle airplane bus train truck boat
    'VEHICLE': {1, 2, 3, 4, 5, 6, 7, 8},
    # bird cat dog horse sheep cow elephant bear zebra giraffe
    'ANIMAL': {14, 15, 16, 17, 18, 19, 20, 21, 22, 23},
    # fire hydrant stop sign parking meter bench chair couch potted plant bed dining table toilet
    'FACILITY': {10, 11, 12, 13, 56, 57, 58, 59, 60, 61},
}
CLASS_TO_CATEGORY = {
    cls_id: cat for cat, ids in CATEGORY_GROUPS.items() for cls_id in ids
}
CATEGORY_ZH = {
    'PEDESTRIAN': '行人', 'VEHICLE': '车辆', 'ANIMAL': '动物', 'FACILITY': '静态设施',
}
CATEGORY_UNIT = {
    'PEDESTRIAN': '名', 'VEHICLE': '辆', 'ANIMAL': '只', 'FACILITY': '处',
}
CATEGORY_SHORT = {
    'PEDESTRIAN': 'PED', 'VEHICLE': 'VEH', 'ANIMAL': 'ANI', 'FACILITY': 'FAC',
}

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


def summarize_barrier(objects_all: list) -> dict:
    """根据全部检测结果生成 4 大类计数、中文摘要与回传盲杖的紧凑格式。"""
    counts = {c: 0 for c in CATEGORY_ORDER}
    for o in objects_all:
        counts[o['category']] += 1

    present = [c for c in CATEGORY_ORDER if counts[c] > 0]
    if not present:
        summary_zh = '未检测到障碍物'
        cane = 'BARRIER:NONE'
    else:
        summary_zh = '、'.join(
            f'{counts[c]}{CATEGORY_UNIT[c]}{CATEGORY_ZH[c]}' for c in present)
        cane = 'BARRIER:' + ','.join(
            f'{CATEGORY_SHORT[c]}{counts[c]}' for c in present)
    return {
        'ok': True,
        'model': BARRIER_MODEL_PATH,
        'counts': counts,
        'total': sum(counts.values()),
        'objects': objects_all[:MAX_BARRIER_OBJECTS],   # 明细截断，计数保留全量
        'summary_zh': summary_zh,
        'cane': cane,
    }


def detect_barrier_from_bytes(data: bytes) -> dict:
    """Run the obstacle pipeline on raw image bytes and return a JSON dict.

    与 /detect（交通灯，纯文本 COLOR|NN%）不同，这里返回结构化 JSON：
      counts      4 大类计数 {PEDESTRIAN, VEHICLE, ANIMAL, FACILITY}
      objects     明细（label 为 COCO 原始英文类名，conf 置信度，box 像素坐标）
      summary_zh  中文摘要（可直接 TTS 播报）
      cane        回传盲杖的紧凑格式，如 BARRIER:PED2,VEH1 / BARRIER:NONE
    """
    if not data:
        raise HTTPException(status_code=400, detail='empty image')

    arr = np.frombuffer(data, dtype=np.uint8)
    image = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if image is None:
        raise HTTPException(status_code=400, detail='invalid image')

    results = barrier_model.predict(image, verbose=False, conf=BARRIER_CONF)

    objects_all = []
    for result in results:
        boxes = result.boxes
        if boxes is None or len(boxes) == 0:
            continue
        xyxy = boxes.xyxy.cpu().numpy()   # (n, 4)
        confs = boxes.conf.cpu().numpy()  # (n,)
        clss = boxes.cls.cpu().numpy()    # (n,)

        for i in range(len(xyxy)):
            cls_id = int(clss[i])
            category = CLASS_TO_CATEGORY.get(cls_id)
            if category is None:
                continue   # 与障碍物无关的类别（含 traffic light）
            x1, y1, x2, y2 = xyxy[i].tolist()
            objects_all.append({
                'category': category,
                'label': str(barrier_model.names.get(cls_id, cls_id)),
                'conf': round(float(confs[i]), 3),
                'box': [round(x1), round(y1), round(x2), round(y2)],
            })

    objects_all.sort(key=lambda o: o['conf'], reverse=True)
    return summarize_barrier(objects_all)


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
    return (f'OK {state} light_model={MODEL_PATH} '
            f'barrier_model={BARRIER_MODEL_PATH} barrier_conf={BARRIER_CONF}')


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


@app.get('/barrier')
def barrier_from_camera(cam: str = ''):
    """GET /barrier —— 障碍物检测（第二个 YOLO 模型 yolov8s.pt）

    与 GET /detect 同样的取帧逻辑：
    默认使用 ESP32-CAM 推送上来的最新一帧；
    显式传 ?cam=<snapshot-url> 且服务器能直连时临时去拉一张。
    返回 JSON（见 detect_barrier_from_bytes）。
    """
    if cam:
        try:
            r = requests.get(cam, timeout=CAMERA_TIMEOUT)
            r.raise_for_status()
        except Exception as exc:
            raise HTTPException(status_code=502, detail=f'camera unreachable: {exc}')
        return detect_barrier_from_bytes(r.content)

    data, age = cached_frame()
    if data is None:
        raise HTTPException(
            status_code=503,
            detail=f'no fresh frame from device (age={age if age else "n/a"})')
    return detect_barrier_from_bytes(data)


@app.post('/barrier')
async def barrier(request: Request):
    """POST /barrier —— 手机直接上传图片做障碍物识别（不经过摄像头缓存）。"""
    data = await request.body()
    return detect_barrier_from_bytes(data)


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


# ---------------------------------------------------------------------------
# 自建轻量"实时数据库"（/fb/*）—— 替代 Firebase RTDB 的双端通信
#
# 背景：Firebase（Google 境外服务）在国内时断时续（App 端曾报 Error 1101），
# 而双端 App 只用它中转两样东西：盲人 GPS 位置 + SOS 求救。
# 这里在自有服务器上实现一套 REST 格式与 Firebase 完全兼容的接口，
# 前端仅需把 config.js 里 firebase.baseUrl 改成 http://<本服务器>/fb 即可，
# firebase.js / blind 端 / family 端的代码一行都不用动：
#
#   PUT /fb/<node>/<key>.json   写入一个键（盲人端：上报位置 / SOS）
#   GET /fb/<node>.json         读整个节点（家属端轮询；空节点返回 null，同 Firebase）
#   GET /fb/<node>/<key>.json   读单个键
#
# 示例（blind001 为盲人节点 ID）：
#   PUT /fb/blind001/location.json  body={"latitude":30.0,"longitude":120.0}
#   PUT /fb/blind001/sos.json       body=true
#   GET /fb/blind001.json           → {"location":{...},"sos":true}
#
# 说明：数据存内存（重启即清空，重新上报即恢复）；演示环境暂不加鉴权，
# 日后可仿照 /upload 的 token 机制补一层。
# ---------------------------------------------------------------------------
_fb_lock = threading.Lock()
_fb_nodes = {}   # {node: {key: value, ...}}


@app.put('/fb/{node}/{key}.json')
async def fb_put(node: str, key: str, request: Request):
    """写入一个键（与 Firebase RTDB 的 PUT 语义一致，返回写入的值）。"""
    try:
        value = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail='invalid json')
    with _fb_lock:
        _fb_nodes.setdefault(node, {})[key] = value
    return value


@app.get('/fb/{node}/{key}.json')
def fb_get_key(node: str, key: str):
    """读取单个键；不存在时与 Firebase 一致返回 404。"""
    with _fb_lock:
        node_data = _fb_nodes.get(node, {})
    if key not in node_data:
        raise HTTPException(status_code=404, detail='not found')
    return node_data[key]


@app.get('/fb/{node}.json')
def fb_get_node(node: str):
    """读取整个节点；节点不存在时返回 null（与 Firebase 一致）。"""
    with _fb_lock:
        node_data = dict(_fb_nodes.get(node, {}))
    return node_data or None
