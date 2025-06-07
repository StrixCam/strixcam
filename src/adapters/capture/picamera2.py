import time

from numpy import uint8
from numpy.typing import NDArray
from picamera2 import Picamera2

from config.settings import settings
from core.interfaces.camera import ICamera
from core.models.frame import Frame


class Picamera2Adapter(ICamera):
	def __init__(self, camera_index: int = 0) -> None:
		self.picam = Picamera2(camera_num=camera_index, verbose_console=0)
		video_config = self.picam.create_video_configuration(
			main={"size": settings.camera.resolution},
      controls={ "FrameRate": settings.camera.frame_rate }
    )
		self.picam.configure(video_config)

	def start(self) -> None:
		self.picam.start()

	def get_frame(self) -> Frame:
		data: NDArray[uint8] = self.picam.capture_array()
		return Frame(data=data, timestamp=time.time())

	def stop(self) -> None:
		self.picam.stop()
