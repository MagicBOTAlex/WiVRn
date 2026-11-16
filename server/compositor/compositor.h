/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.

#pragma once

#include "util/comp_base.h"
#include "util/u_logging.h"

#include "encoder/encoder_settings.h"
#include "foveation.h"
#include "layer_squasher.h"
#include "pacer.h"
#include "utils/wivrn_vk_bundle.h"

#include "main/comp_compositor.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace wivrn
{

class wivrn_session;
class video_encoder;

class compositor : public comp_base
{
public:
	struct image
	{
		std::atomic<bool> busy = false;
		image_allocation image;
		vk::raii::ImageView view_y;
		vk::raii::ImageView view_cbcr;
		to_headset::video_stream_data_shard::view_info_t view_info{};
		uint64_t frame_index;
	};

private:
	struct timings
	{
		std::array<float, 50> values{};
		int index = 0;
		u_var_timing var{
		        .values = {
		                .data = values.data(),
		                .index_ptr = &index,
		                .length = int(values.size()),
		        },
		        .range = 1000,
		        .dynamic_rescale = true,
		        .unit = "µs",
		};

		void add(float us);
	};

	const u_logging_level log_level;
	timings squasher_times;
	timings foveation_times;
	wivrn_session & session;
	vk_bundle vk;
	// Two frames in flight: the app thread must not wait for the GPU at the end
	// of layer_commit (xrEndFrame is a synchronous IPC round trip).
	std::array<vk::raii::CommandPool, 2> cmd_pools;
	std::array<vk::raii::QueryPool, 2> query_pools;
	std::array<vk::raii::CommandBuffer, 2> cmds;
	uint8_t frame_parity = 0;
	uint64_t pending_sem_value = 0;
	bool have_pending_frame = false;

	std::array<encoder_settings, 3> settings;

	std::array<image, 2> images;
	vk::raii::Semaphore sem;
	uint64_t sem_value = 0;

	std::atomic<float> requested_refresh_rate;
	std::atomic<float> frame_rate;
	wivrn::pacer pacer;

	layer_squasher squasher;
	wivrn::foveation foveation;

	std::array<std::unique_ptr<video_encoder>, 3> encoders;

	// One encode worker per encoder. Two NVENC sessions encode the two eyes in
	// parallel (~1.4 ms off the two-eye path), and a failure in one stream can
	// no longer starve the others (the old single try/catch around the whole
	// loop left sibling slots busy forever).
	struct encode_job
	{
		uint64_t epoch;
		int image;
		uint64_t frame_index;
		to_headset::video_stream_data_shard::view_info_t view_info;
		std::atomic<int> remaining{0};
	};
	std::mutex job_mutex;
	std::condition_variable job_cv;
	std::deque<std::shared_ptr<encode_job>> jobs;
	uint64_t next_job_epoch = 1;
	std::array<std::jthread, 3> encoder_threads;

	bool submit_encode_job(int image);
	void encoder_worker(std::stop_token, size_t index);

	struct
	{
		comp_frame waited{.id = -1};
		comp_frame rendering{.id = -1};
	} frame;

	xrt_result_t begin_session(const xrt_begin_session_info * info)
	{
		return XRT_SUCCESS;
	};
	xrt_result_t end_session()
	{
		return XRT_SUCCESS;
	};

	xrt_result_t predict_frame(int64_t * out_frame_id,
	                           int64_t * out_wake_time_ns,
	                           int64_t * out_predicted_gpu_time_ns,
	                           int64_t * out_predicted_display_time_ns,
	                           int64_t * out_predicted_display_period_ns);

	xrt_result_t mark_frame(int64_t frame_id,
	                        xrt_compositor_frame_point point,
	                        int64_t when_ns);

	xrt_result_t begin_frame(int64_t frame_id)
	{
		return XRT_SUCCESS;
	}
	xrt_result_t discard_frame(int64_t frame_id)
	{
		return XRT_SUCCESS;
	};

	xrt_result_t layer_commit(xrt_graphics_sync_handle_t sync_handle);

	xrt_result_t get_display_refresh_rate(float * out_display_refresh_rate_hz);

public:
	xrt_result_t request_display_refresh_rate(float display_refresh_rate_hz);

	static xrt_result_t get_view_config(xrt_compositor_native *,
	                                    xrt_view_type view_type,
	                                    xrt_view_config * out_view_config);

private:
	void destroy()
	{
		// do nothing, actually owned by wivrn_session object
	}

	int acquire_image();

	void send_video_stream_description();

public:
	using base_t = xrt_compositor;
	compositor(wivrn_session &);
	~compositor();

	xrt_system_compositor_info sys_info() const;

	int64_t get_frame_duration() const
	{
		return pacer.get_frame_duration();
	}

	float get_requested_refresh_rate() const
	{
		return requested_refresh_rate;
	}

	float get_framerate() const
	{
		return frame_rate;
	}
	void set_framerate(float hz);

	void set_bitrate(uint32_t);
	void update_foveation(const from_headset::foveation &);

	void resume();

	void on_feedback(const from_headset::feedback &, const clock_offset &);
};

} // namespace wivrn
