/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Capture of external (USB/UVC) cameras on Android.
// Horizon OS has no uvcvideo driver, so frames come from a userspace UVC stack
// (libusb) and are MJPEG or raw YUYV.
namespace android_camera
{
struct info
{
	int id;
	std::string identity;
	std::string name;
	uint16_t vendor_id;
	uint16_t product_id;
	bool granted;
};

struct frame
{
	int camera_id;
	uint64_t frame_idx;
	int64_t timestamp_ns;
	bool keyframe;
	std::vector<uint8_t> data;
};

// Enumerate the external cameras currently attached to the headset.
// Returns nullopt when enumeration failed (as opposed to an empty list when there
// are simply no cameras), so callers do not tear down a working stream.
std::optional<std::vector<info>> list_cameras();

class capture
{
public:
	using callback = std::function<void(frame &&)>;

	static std::unique_ptr<capture> start(const info & camera, int width, int height, int fps, callback cb);
	~capture();

	bool is_running() const;

	capture(const capture &) = delete;
	capture & operator=(const capture &) = delete;
	capture(capture &&) = delete;
	capture & operator=(capture &&) = delete;

private:
	struct impl;
	explicit capture(std::unique_ptr<impl> &&);

	std::unique_ptr<impl> p;
};
} // namespace android_camera
