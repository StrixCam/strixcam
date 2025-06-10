import json
import os

from core.interfaces.storage import IStorage


class JSONStorage(IStorage):
	def save(self, data: dict, filename: str, ext: str = 'json', dir: str = 'storage') -> None:
		os.makedirs(dir, exist_ok=True)
		path = os.path.join(dir, f'{filename}.{ext}')
		with open(path, 'w') as f:
			json.dump(data, f, indent=2)
		print(f'✅ Data saved to {path}')

	def load(self, filename: str, ext: str = 'json', dir: str = 'storage') -> dict:
		path = os.path.join(dir, f'{filename}.{ext}')
		if not os.path.exists(path):
			raise FileNotFoundError(f'❌ File not found: {path}')
		with open(path) as f:
			return json.load(f)
