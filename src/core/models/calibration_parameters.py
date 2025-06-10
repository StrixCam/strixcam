from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Any, TypeVar, Union


class BaseCalibrationParameters(ABC):
	@abstractmethod
	def to_dict(self) -> dict[str, Any]:
		pass

	@classmethod
	@abstractmethod
	def from_dict(cls, data: dict[str, Any]) -> 'BaseCalibrationParameters':
		pass


@dataclass
class CameraCalibrationParameters(BaseCalibrationParameters):
	parameters: dict[str, dict[str, list[list[float]]]]

	def to_dict(self) -> dict[str, Any]:
		return self.parameters

	@classmethod
	def from_dict(cls, data: dict[str, Any]) -> 'CameraCalibrationParameters':
		return cls(parameters=data)


@dataclass
class CalibrationParameter:
	target_id: str  # "camera", "lidar", etc.
	target_data: BaseCalibrationParameters | dict[str, Any]

	def to_dict(self) -> dict[str, Any]:
		data = (
			self.target_data.to_dict()
			if isinstance(self.target_data, BaseCalibrationParameters)
			else self.target_data
		)
		return {
			'target_id': self.target_id,
			'target_data': data,
		}

	@staticmethod
	def from_dict(data: dict[str, Any]) -> 'CalibrationParameter':
		target_id = data['target_id']
		raw_data = data['target_data']

		if target_id == 'camera':
			return CalibrationParameter(
				target_id=target_id, target_data=CameraCalibrationParameters.from_dict(raw_data)
			)
		# futuros tipos...

		raise ValueError(f'Unsupported target_id: {target_id}')
