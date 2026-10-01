# CLAUDE.md

Guidance for Claude Code (and similar agents) working in this repository.

## What this repo is

Workspace gồm 3 dự án con, cùng chủ đề đếm vật thể qua camera/video bằng YOLO:

```
AIStuff/            # coco.names + yolov8n.onnx — asset dùng chung
FishCounter/         # C++/MSVC, đếm cá qua camera (camera_integrated.obj, opencv2/ vendor)
StreamCounter1/       # Python, CMake — đếm qua stream, dùng Ultralytics YOLO (yolov8n.pt)
  src/                # camera_capture.cpp, usb_enum.cpp
  Code.ipynb          # notebook thử nghiệm
```

## Đã dọn ngày 2026-10-01

Các thư mục sau đã có rule trong `.gitignore` từ trước nhưng bị commit nhầm trước đó — đã untrack: `.vs/`, `FishCounter/x64/`, `StreamCounter1/build/`, `StreamCounter1/out/`, `StreamCounter1/Video.mp4`. Thêm mới rule `runs/` (thư mục output chuẩn của Ultralytics YOLO, 69MB/306 file bị track nhầm, đã untrack).

## Ràng buộc

- `FishCounter/opencv2/` là header OpenCV vendor trong repo (để build không cần cài OpenCV riêng) — **không xóa/untrack**, code C++ phụ thuộc trực tiếp vào đây.
- `yolov8n.pt`, `AIStuff/yolov8n.onnx` là model nhẹ (pretrained YOLOv8 nano, ~19MB) — không rõ có chủ đích giữ lại để demo nhanh hay không (đã có rule `*.h5`/`*.pkl`/`*.model` cho format khác nhưng không có cho `.pt`/`.onnx`). Hỏi lại người dùng trước khi xóa.
- `FishCounter` là project C++/MSVC (`.sln`, `.vcxproj`) — không build được trong môi trường agent (cần MSVC + OpenCV build sẵn).
- `StreamCounter1` dùng Ultralytics YOLO (Python) — không có GPU/model thật trong môi trường agent, chỉ kiểm tra syntax.

## Kiểm thử an toàn

```bash
python -c "import ast; ast.parse(open('StreamCounter1/src/camera_capture.cpp').read())" 2>&1 || true  # C++ không parse bằng ast, chỉ đọc bằng mắt
```
Không chạy build MSVC/CMake hay script train/inference thật.
