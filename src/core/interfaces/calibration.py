from abc import ABC, abstractmethod
from typing import Any


class ICalibration(ABC):
	@abstractmethod
	def calibrate(self, *args, **kwargs) -> None:
		"""Ejecuta el proceso de calibración (genérico)."""
		pass

	@abstractmethod
	def load(self, target_id: str) -> Any:
		"""Carga los parámetros de calibración de un identificador específico."""
		pass

	@abstractmethod
	def remove(self, target_id: str) -> None:
		"""Elimina los parámetros de calibración de un identificador específico."""
		pass
