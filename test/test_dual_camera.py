import os
import sys

import cv2

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '../src')))
from adapters.capture.picamera2 import Picamera2Adapter


def test_dual_camera():
	cam0 = Picamera2Adapter(0)
	cam1 = Picamera2Adapter(1)

	cam0.start()
	cam1.start()

	try:
		while True:
			frame0 = cam0.get_frame().data
			frame1 = cam1.get_frame().data

			combined = cv2.hconcat((frame0, frame1))
			resized = cv2.resize(combined, (3840, 1080))
			colored = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
			cv2.imshow('Dual Feed', colored)

			if cv2.waitKey(1) & 0xFF == ord('q'):
				break
	finally:
		cam0.stop()
		cam1.stop()
		cv2.destroyAllWindows()


if __name__ == '__main__':
	test_dual_camera()
