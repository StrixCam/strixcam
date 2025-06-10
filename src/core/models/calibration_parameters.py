from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Any, TypeVar


# === Base Interface ===
class BaseCalibrationParameters(ABC):
	@abstractmethod
	def to_dict(self) -> dict[str, Any]:
		raise NotImplementedError

	@classmethod
	@abstractmethod
	def from_dict(cls, data: dict[str, Any]) -> 'BaseCalibrationParameters':
		raise NotImplementedError


# === Specific Calibration Types ===


@dataclass
class CameraIntrinsics:
	camera_matrix: list[list[float]]
	dist_coeffs: list[list[float]]


@dataclass
class CameraCalibrationParameters(BaseCalibrationParameters):
	parameters: dict[str, CameraIntrinsics]

	def to_dict(self) -> dict[str, Any]:
		return {
			k: {
				'camera_matrix': v.camera_matrix,
				'dist_coeffs': v.dist_coeffs,
			}
			for k, v in self.parameters.items()
		}

	@classmethod
	def from_dict(cls, data: dict[str, Any]) -> 'CameraCalibrationParameters':
		return cls(
			parameters={
				k: CameraIntrinsics(
					camera_matrix=v['camera_matrix'],
					dist_coeffs=v['dist_coeffs'],
				)
				for k, v in data.items()
			}
		)


# === Union Type and Factory ===

T = TypeVar('T', bound=BaseCalibrationParameters)


@dataclass
class CalibrationContainer:
	"""
	Main container to hold any type of calibration.
	"""

	target_id: str  # e.g., "camera", "lidar", "imu"
	target_data: BaseCalibrationParameters

	def to_dict(self) -> dict[str, Any]:
		return {
			'target_id': self.target_id,
			'target_data': self.target_data.to_dict(),
		}

	@staticmethod
	def from_dict(data: dict[str, Any]) -> 'CalibrationContainer':
		target_id = data['target_id']
		raw = data['target_data']

		if target_id == 'camera':
			return CalibrationContainer(
				target_id=target_id,
				target_data=CameraCalibrationParameters.from_dict(raw),
			)
		# Future: add 'elif target_id == "lidar"' etc.

		raise ValueError(f'Unsupported target_id: {target_id}')
