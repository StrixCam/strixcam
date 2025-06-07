from abc import ABC, abstractmethod

from core.models.frame import Frame


class ICamera(ABC):
	@abstractmethod
	def start(self) -> None: ...

	@abstractmethod
	def get_frame(self) -> Frame: ...

	@abstractmethod
	def stop(self) -> None: ...
