import os
import sys
import time

import cv2

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'src')))
from adapters.capture.picamera2 import Picamera2Adapter
from domain.video.processor.cropper import Cropper
from domain.video.processor.postprocessor import postprocess_frame
from domain.video.processor.preprocessor import preprocess_frame


def test_single_camera():
	cam = Picamera2Adapter(0)  # test only camera 0

	cam.start()

	frame_count = 0
	start_time = time.time()

	try:
		while True:
			frame = cam.get_frame()
			frame = preprocess_frame(frame)
			top_left = (0, 0)
			bottom_right = (1920, 1080)
			frame = Cropper().crop(frame, top_left, bottom_right)
			frame = postprocess_frame(frame)

			cv2.imshow('Single Camera Feed', frame.data)

			frame_count += 1
			elapsed = time.time() - start_time

			if elapsed >= 1.0:
				fps = frame_count / elapsed
				print(f'[INFO] Frame Size: {frame.data.shape[1]}x{frame.data.shape[0]}')
				print(f'[INFO] Frame FPS: {fps:.2f}')
				frame_count = 0
				start_time = time.time()

			if cv2.waitKey(1) & 0xFF == ord('q'):
				break

	finally:
		cam.stop()
		cv2.destroyAllWindows()


if __name__ == '__main__':
	test_single_camera()
