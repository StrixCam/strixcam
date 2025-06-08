from dataclasses import dataclass, field


@dataclass
class CameraSettings:
	resolution: tuple[int, int] = (1920, 1080)
	frame_rate: int = 60
	format: str = 'RGB888'


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
