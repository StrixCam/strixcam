from abc import ABC, abstractmethod
from typing import Any


class IStorage(ABC):
	@abstractmethod
	def save(self, data: dict, filename: str, ext: str, dir: str) -> None:
		pass

	@abstractmethod
	def load(self, filename: str, ext: str, dir: str) -> dict:
		pass
