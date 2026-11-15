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
#include <vector>

// Minimal UVC (USB Video Class) host implementation on top of libusb, for
// platforms without kernel uvcvideo support (Meta Horizon OS).
namespace uvc
{
struct frame
{
	// MJPEG (a complete JPEG image) or raw YUYV frame
	std::vector<uint8_t> data;
	uint8_t frame_id;
	int64_t timestamp_ns;
};

class capture
{
public:
	using callback = std::function<void(frame &&)>;

	// fd: usbfs file descriptor obtained from UsbDeviceConnection.getFileDescriptor()
	static std::unique_ptr<capture> start(int fd, int width, int height, int fps, callback cb);
	~capture();

	capture(const capture &) = delete;
	capture & operator=(const capture &) = delete;

	int width() const;
	int height() const;
	int fps() const;
	bool is_mjpeg() const;
	// False once the device is gone or transfers stopped.
	bool is_running() const;

private:
	struct impl;
	explicit capture(std::unique_ptr<impl> &&);
	std::unique_ptr<impl> p;
};
} // namespace uvc
