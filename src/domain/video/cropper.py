import cv2
import numpy as np

from config.settings import settings
from core.models.frame import Frame


class Cropper:
	def __init__(self, target_size: tuple[int, int] = (1920, 1080)) -> None:
		self.target_size: tuple[int, int] = target_size

	def crop(
		self,
		frames: Frame,
		top_left: tuple[int, int] = (0, 0),
		bottom_right: tuple[int, int] = settings.camera.resolution,
	) -> Frame:
		x1, y1 = top_left
		x2, y2 = bottom_right
		cropped = frames.data[y1:y2, x1:x2]
		resized = cv2.resize(cropped, self.target_size, interpolation=cv2.INTER_AREA)
		return Frame(
			data=resized,
			timestamp=frames.timestamp,
		)
