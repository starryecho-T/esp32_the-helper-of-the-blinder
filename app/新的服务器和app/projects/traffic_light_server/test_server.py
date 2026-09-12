# -*- coding: utf-8 -*-
"""Offline test for the traffic-light server.

Starts a fake ESP32-CAM HTTP server that serves a synthetic JPEG, then checks:
  1. GET  /health                -> 200 'OK'
  2. GET  /detect?cam=<fake url> -> 200, body matches COLOR|NN% or NONE|0
  3. POST /detect (upload bytes) -> 200, same format (old flow still works)
  4. GET  /detect?cam=<dead url> -> 502 camera unreachable
  5. POST /detect (invalid data) -> 400 invalid image
"""
import re
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import cv2
import numpy as np
from fastapi.testclient import TestClient

import server as srv

# --- build a synthetic "camera" JPEG (dark street + red light blob) ---------
img = np.full((480, 640, 3), 40, np.uint8)
cv2.circle(img, (320, 120), 35, (0, 0, 255), -1)   # BGR red
cv2.rectangle(img, (312, 150), (328, 300), (60, 60, 60), -1)  # pole
ok, buf = cv2.imencode('.jpg', img)
assert ok
JPEG = buf.tobytes()
print(f'synthetic JPEG bytes: {len(JPEG)}')


class CamHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'image/jpeg')
        self.send_header('Content-Length', str(len(JPEG)))
        self.end_headers()
        self.wfile.write(JPEG)

    def log_message(self, *args):
        pass


cam_srv = HTTPServer(('127.0.0.1', 0), CamHandler)
threading.Thread(target=cam_srv.serve_forever, daemon=True).start()
CAM_URL = f'http://127.0.0.1:{cam_srv.server_address[1]}/capture'
print(f'fake camera at {CAM_URL}')

PAT = re.compile(r'^(RED|YELLOW|GREEN|NONE)\|\d+%$|^NONE\|0$')

c = TestClient(srv.app)

r1 = c.get('/health')
print(f"1) GET  /health            -> {r1.status_code} '{r1.text}'")
assert r1.status_code == 200 and r1.text == 'OK'

r2 = c.get('/detect', params={'cam': CAM_URL})
print(f"2) GET  /detect?cam=fake   -> {r2.status_code} '{r2.text}'")
assert r2.status_code == 200 and PAT.match(r2.text), 'bad body for camera flow'

r2b = c.get('/detect?cam=' + CAM_URL)  # raw unencoded query exactly like App Inventor Web.Get()
print(f"2b) GET  raw unencoded query -> {r2b.status_code} '{r2b.text}'")
assert r2b.status_code == 200 and PAT.match(r2b.text), 'raw query form failed'

r3 = c.post('/detect', content=JPEG)
print(f"3) POST /detect (upload)   -> {r3.status_code} '{r3.text}'")
assert r3.status_code == 200 and PAT.match(r3.text), 'upload flow broken'

r4 = c.get('/detect', params={'cam': 'http://127.0.0.1:9/capture'})
print(f"4) GET  /detect?cam=dead   -> {r4.status_code} '{r4.text[:60]}'")
assert r4.status_code == 502, 'dead camera should be 502'

r5 = c.post('/detect', content=b'this-is-not-a-jpeg')
print(f"5) POST /detect (invalid)  -> {r5.status_code}")
assert r5.status_code == 400, 'invalid upload should be 400'

print()
print('ALL SERVER TESTS PASSED')
