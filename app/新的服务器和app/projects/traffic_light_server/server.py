from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import PlainTextResponse
from ultralytics import YOLO
import cv2
import numpy as np
import requests

app = FastAPI(title='Smart Cane Traffic Light Detector')

# COCO-trained YOLO model. It detects the object "traffic light".
# The color is classified from the detected traffic-light crop.
model = YOLO('yolov8n.pt')
TRAFFIC_LIGHT_CLASS = 9  # COCO: traffic light

# Head-mounted ESP32-CAM snapshot URL. Override with ?cam=<url>.
CAMERA_DEFAULT_URL = 'http://192.168.43.248/capture'
CAMERA_TIMEOUT = 5  # seconds


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
        for box in boxes:
            cls = int(box.cls[0].item())
            if cls != TRAFFIC_LIGHT_CLASS:
                continue
            det_conf = float(box.conf[0].item())
            x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
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
    # Keep two decimal places for the App Inventor client.
    return f'{color}|{det_conf * 100:.0f}%'


@app.get('/health', response_class=PlainTextResponse)
def health():
    return 'OK'


@app.get('/detect', response_class=PlainTextResponse)
def detect_from_camera(cam: str = CAMERA_DEFAULT_URL):
    """GET /detect?cam=<snapshot-url>: server pulls the image from the
    ESP32-CAM by itself, so the app never has to upload a file."""
    try:
        r = requests.get(cam, timeout=CAMERA_TIMEOUT)
        r.raise_for_status()
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f'camera unreachable: {exc}')
    return detect_from_bytes(r.content)


@app.post('/detect', response_class=PlainTextResponse)
async def detect(request: Request):
    data = await request.body()
    return detect_from_bytes(data)

