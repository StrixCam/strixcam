import cv2
import numpy as np

from config.settings import Settings
from core.models.frame import Frame


def preprocess_frame(frame: Frame) -> Frame:
	# resized = cv2.resize(frame.data, (1920, 1080), interpolation=cv2.INTER_LINEAR)
	# colored = cv2.cvtColor(frame.data, cv2.COLOR_BGR2GRAY)
	processed_frame = frame.data
	return Frame(data=processed_frame, timestamp=frame.timestamp)
