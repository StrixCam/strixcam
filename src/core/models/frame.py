from dataclasses import dataclass

import numpy as np


@dataclass
class Frame:
	data: np.ndarray
	timestamp: float
