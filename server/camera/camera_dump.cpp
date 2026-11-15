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

#include "camera_sink.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "util/u_logging.h"

#include "camera_v4l2.h"

namespace wivrn
{
namespace
{
// Debug sink: writes the raw MJPEG stream of each camera to a file.
// Enabled with WIVRN_CAMERA_DUMP=/tmp/wivrn-camera (one file per camera id).
class camera_dump : public camera_sink
{
	std::string prefix;
	std::map<uint8_t, FILE *> files; // nullptr caches an open failure
	uint64_t write_errors = 0;

	FILE * file_for(uint8_t id)
	{
		auto it = files.find(id);
		if (it != files.end())
			return it->second;

		std::string path = prefix + "-" + std::to_string(unsigned(id)) + ".mjpg";
		FILE * f = fopen(path.c_str(), "wb");
		if (not f)
		{
			U_LOG_E("Failed to open %s", path.c_str());
			files[id] = nullptr;
			return nullptr;
		}
		U_LOG_I("Dumping camera %u to %s", unsigned(id), path.c_str());
		files[id] = f;
		return f;
	}

public:
	explicit camera_dump(std::string prefix) :
	        prefix(std::move(prefix))
	{
	}

	~camera_dump() override
	{
		for (auto & [id, f]: files)
			if (f)
				fclose(f);
	}

	void process_frame(from_headset::camera_frame && frame) override
	{
		FILE * f = file_for(frame.camera_id);
		if (not f)
			return;
		if (fwrite(frame.payload.data(), 1, frame.payload.size(), f) != frame.payload.size())
		{
			if (write_errors++ % 300 == 0)
				U_LOG_E("Camera %u: write failed (disk full?)", unsigned(frame.camera_id));
			return;
		}
		fflush(f);
	}
};
} // namespace

std::unique_ptr<camera_sink> camera_sink::create(const from_headset::headset_info_packet & info)
{
	const char * dump = getenv("WIVRN_CAMERA_DUMP");
	if (dump and *dump)
	{
		U_LOG_I("Camera frames will be dumped to %s-<id>.mjpg", dump);
		return std::make_unique<camera_dump>(dump);
	}

	// Create the sink even when no camera is attached: cameras plugged in later
	// must work without reconnecting (the headset sends camera_list on hotplug).
	if (find_v4l2_camera_device() >= 0)
		return create_v4l2_camera_sink(info);

	U_LOG_I("Camera streaming disabled: no v4l2loopback device (set WIVRN_CAMERA_DUMP to dump frames)");
	return nullptr;
}
} // namespace wivrn
