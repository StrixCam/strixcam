import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'src')))

from adapters.capture.picamera2 import Picamera2Adapter
from domain.video.calibration import calibrator


def test_calibration() -> None:
	cam0 = Picamera2Adapter(0)
	cam1 = Picamera2Adapter(1)
	calibrator.calibrate_camera_from_images([cam0, cam1])


if __name__ == '__main__':
	test_calibration()
