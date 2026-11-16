/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
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

#include <cstdint>
#include <mutex>
#include <variant>

namespace wivrn
{

class idr_handler
{
public:
	virtual ~idr_handler();
	virtual void on_feedback(const from_headset::feedback &) = 0;
	virtual void reset() = 0;
	virtual bool should_skip(uint64_t frame_id, uint8_t stream_idx) = 0;
};

// handler for unknown P-frames
// any lost frame triggers an I-frame
// skip frames until th I frame is received
class default_idr_handler : public idr_handler
{
	std::mutex mutex;
	struct need_idr
	{};
	struct wait_idr_feedback
	{
		uint64_t idr_id;
	};
	struct running
	{
		uint64_t last_ack;
	};
	std::variant<need_idr, wait_idr_feedback, running> state;
	// Do not retry a keyframe before this frame index. A keyframe is the largest
	// frame on the wire, so under load it is the most likely one to be lost;
	// retrying on the very next frame turns that into a keyframe storm.
	uint64_t next_idr_allowed = 0;
	// NB: parentheses, not braces: {512, -1} is a two-element vector.
	std::vector<uint64_t> non_ref_frames = std::vector<uint64_t>(512, uint64_t(-1));

public:
	enum class frame_type
	{
		i,
		p,
	};

	void on_feedback(const from_headset::feedback &) override;
	void reset() override;
	bool should_skip(uint64_t frame_id, uint8_t stream_idx) override;
	void set_non_ref(uint64_t frame_index);
	// The transport dropped a datagram of this frame: it cannot be decoded, so
	// request a keyframe without waiting for the client's feedback.
	void on_local_drop(uint64_t frame_index, uint8_t stream_idx);
	bool is_non_ref_frame(uint64_t frame_index);
	frame_type get_type(uint64_t frame_index, uint8_t stream_idx);
};
} // namespace wivrn
