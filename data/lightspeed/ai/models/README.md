# Models of the Tonelark AI helper

- `face_detection_yunet_2023mar.onnx`: YuNet face detector, from the OpenCV Model Zoo, MIT license (LICENSE_yunet).
- `facial_expression_recognition_mobilefacenet_2022july.onnx`: facial expression recognition (smiles), from the
  OpenCV Model Zoo, Apache 2.0 license (LICENSE_fer).
- `arcfaceresnet100-8.onnx`: ArcFace ResNet100 face recognition (the People panel), from the ONNX Model Zoo,
  Apache 2.0 license (LICENSE_arcface). At 261 MB it is too big for git: `python tools/lightspeed/fetch_models.py`
  downloads it here and checks its SHA-256 before a build.
