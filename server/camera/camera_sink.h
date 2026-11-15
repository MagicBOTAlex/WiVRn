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

#include "wivrn_packets.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace wivrn
{
// A camera currently attached to the headset.
struct camera_entry
{
	uint8_t id;
	std::string identity; // USB serial, or vid:pid
	std::string name;
};

// Receives camera frames from the headset and exposes them on the PC.
class camera_sink
{
public:
	static std::unique_ptr<camera_sink> create(const from_headset::headset_info_packet & info);

	virtual ~camera_sink() = default;

	virtual void process_frame(from_headset::camera_frame && frame) = 0;
	// Called at connection and whenever cameras are plugged or unplugged.
	virtual void update_cameras(const std::vector<camera_entry> &) {}
	// Move a camera to another /dev/videoN. Returns false if it could not be applied.
	virtual bool set_camera_device(const std::string &, int)
	{
		return false;
	}
	// Current identity -> device assignment.
	virtual std::vector<std::pair<std::string, std::pair<int, std::string>>> assignment() const
	{
		return {};
	}
	virtual void pause() {}
	virtual void resume() {}
};
} // namespace wivrn
