from dataclasses import dataclass, field


@dataclass
class CameraSettings:
	resolution: tuple[int, int] = (2304, 1296)
	frame_rate: int = 56


@dataclass
class StreamSettings:
	stream_url: str
	stream_key: str
	bitrate: str = '4000k'
	preset: str = 'veryfast'


@dataclass
class Settings:
	camera: CameraSettings = field(default_factory=CameraSettings)
	stream: StreamSettings = field(default_factory=lambda: StreamSettings('', ''))


settings = Settings()
