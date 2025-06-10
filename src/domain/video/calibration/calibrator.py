import json
import os
import time

import cv2
import numpy as np

from adapters.capture import picamera2
from config.settings import settings
from core.interfaces.camera import ICamera


def calibrate_camera_from_images(cameras: list[ICamera], board_size: tuple = (9, 7)) -> None:
	pattern_size: tuple = (board_size[0] - 1, board_size[1] - 1)
	save_dir: str = 'calibration'
	frames_to_capture: int = 20

	objp = np.zeros((pattern_size[1] * pattern_size[0], 3), np.float32)
	objp[:, :2] = np.mgrid[0 : pattern_size[0], 0 : pattern_size[1]].T.reshape(-1, 2)

	os.makedirs(save_dir, exist_ok=True)
	calibration_data = {}
	frames_capture = np.zeros((len(cameras), frames_to_capture), dtype=object)

	for cam_index, cam in enumerate(cameras):
		cam.start()

		print(f'📷 Previewing camera {cam_index}. Press "y" to start capturing frames...')
		cv2.namedWindow(f'Camera {cam_index} Frame', cv2.WINDOW_NORMAL)
		cv2.resizeWindow(f'Camera {cam_index} Frame', *settings.camera.resolution)

		# Wait until user presses 'y'
		while True:
			frame = cam.get_frame()
			raw_frame = frame.data
			sized = cv2.resize(raw_frame, settings.camera.resolution)
			gray = cv2.cvtColor(sized, cv2.COLOR_BGR2GRAY)
			cv2.imshow(f'Camera {cam_index} Frame', gray)
			key = cv2.waitKey(1) & 0xFF
			if key == ord('y'):
				print(f'📷 Capturing {frames_to_capture} frames for camera {cam_index}...')
				break
			elif key == ord('q'):
				print('❌ Capture cancelled.')
				cv2.destroyAllWindows()
				cam.stop()
				return

		for i in range(frames_to_capture):
			frame = cam.get_frame()
			gray = cv2.cvtColor(frame.data, cv2.COLOR_BGR2GRAY)
			message = f'Frame {i + 1}/{frames_to_capture}'
			cv2.putText(gray, message, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, 255, 2)
			cv2.imshow(f'Camera {cam_index} Frame', gray)
			frames_capture[cam_index, i] = gray

			if cv2.waitKey(1) & 0xFF == ord('q'):
				print('❌ Capture interrupted.')
				cam.stop()
				cv2.destroyAllWindows()
				return

			time.sleep(1.0)

		cam.stop()
		cv2.destroyAllWindows()

	# Process calibration per camera
	for cam_index, _ in enumerate(cameras):
		print(f'📐 Processing calibration for camera {cam_index}...')
		objpoints, imgpoints = [], []
		frame_to_analize_shape = None

		for i in range(frames_to_capture):
			frame = frames_capture[cam_index, i]
			ret, corners = cv2.findChessboardCorners(frame, pattern_size, None)
			if ret:
				objpoints.append(objp)
				imgpoints.append(corners)
				if frame_to_analize_shape is None:
					frame_to_analize_shape = frame.shape[::-1]
			else:
				print(f'⚠️ Frame {i + 1} of Camera {cam_index}: Chessboard not detected')

		if not objpoints or frame_to_analize_shape is None:
			print(f'❌ Not enough valid frames for camera {cam_index}')
			continue

		ret, mtx, dist, rvecs, tvecs = cv2.calibrateCamera(
			objpoints, imgpoints, frame_to_analize_shape, None, None
		)

		calibration_data[str(cam_index)] = {
			'camera_matrix': mtx.tolist(),
			'dist_coeffs': dist.tolist(),
		}

	output_path = os.path.join(save_dir, 'calibration.json')
	with open(output_path, 'w') as f:
		json.dump({'calibration': calibration_data}, f, indent=2)

	print(f'✅ Calibration saved to {output_path}')
