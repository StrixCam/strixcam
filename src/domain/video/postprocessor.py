import time

import cv2
import numpy as np

from config.settings import Settings
from core.models.frame import Frame

_last_frame_time = 0
_target_fps = 60
_target_frame_duration = 1.0 / _target_fps


def postprocess_frame(frame: Frame) -> Frame:
	global _last_frame_time

	now = time.time()
	elapsed = now - _last_frame_time

	if elapsed < _target_frame_duration:
		time.sleep(_target_frame_duration - elapsed)

	_last_frame_time = time.time()

	return Frame(data=frame.data, timestamp=_last_frame_time)
