/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2024  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "pacer.h"

#include "driver/clock_offset.h"

#include "util/u_var.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "os/os_time.h"
#include "util/u_debug.h"

DEBUG_GET_ONCE_FLOAT_OPTION(client_margin_ms, "WIVRN_CLIENT_MARGIN_MS", 1.f)

namespace wivrn
{

static const int64_t margin_ns = 3'000'000;
static const int64_t slop_ns = 500'000;

// Largest catch-up applied to a single frame's prediction, in display periods.
// Beyond that the schedule itself is stale (long stall, suspend, reset, clock
// jump) rather than merely one late frame, and predict() re-anchors it instead.
static const int64_t max_bump_periods = 2;

// Length of the percentile window used by the safe_present_to_decoded worker, in
// frames. Overridable at runtime (WIVRN_PACER_WINDOW) to A/B the estimator
// against a shorter history; the default keeps the historical 5000 frames
// (~55 s at 90 Hz). Clamped so a bad value can neither starve the estimator nor
// allocate absurd amounts of memory. Read once, on first use.
static size_t pacer_window_frames()
{
	static const size_t frames = [] {
		size_t value = 5000;
		if (const char * env = getenv("WIVRN_PACER_WINDOW"))
		{
			char * end = nullptr;
			long parsed = strtol(env, &end, 10);
			if (end != env and parsed > 0)
				value = parsed;
		}
		return std::clamp<size_t>(value, 300, 5000);
	}();
	return frames;
}

pacer::pacer(uint64_t frame_duration) :
        frame_duration_ns(frame_duration),
        client_margin_ms{
                .val = debug_get_float_option_client_margin_ms(),
                .step = 0.1,
                .min = 0,
                .max = 20,
        },
        frame_times(pacer_window_frames()),
        frame_times_compute(frame_times),
        worker([this](std::stop_token t) {
	        std::vector<XrDuration> samples;
	        while (not t.stop_requested())
	        {
		        samples.clear();
		        {
			        std::unique_lock lock(compute_mutex);
			        compute_cv.wait(lock);
			        for (const auto & time: frame_times_compute)
			        {
				        if (time.decoded > time.present)
					        samples.push_back(time.decoded - time.present);
			        }
		        }
		        if (samples.empty())
			        continue;
		        auto it = samples.begin() + (samples.size() * 995) / 1000;
		        std::ranges::nth_element(samples, it);

		        std::unique_lock lock(mutex);
		        safe_present_to_decoded_ns = *it + client_margin_ms.val * U_TIME_1MS_IN_NS;
	        }
        })
{
	u_var_add_root(this, "Pacer", false);
	u_var_add_draggable_f32(this, &client_margin_ms, "headset margin (ms)");
}

pacer::~pacer()
{
	u_var_remove_root(this);
	worker.request_stop();
	compute_cv.notify_all();
}

uint64_t pacer::get_frame_duration() const
{
	std::lock_guard lock(mutex);
	return frame_duration_ns;
}

void pacer::set_frame_duration(uint64_t frame_duration_ns)
{
	std::lock_guard lock(mutex);
	this->frame_duration_ns = frame_duration_ns;
}

void pacer::predict(
        int64_t & frame_id,
        int64_t & out_wake_up_time_ns,
        int64_t & out_desired_present_time_ns,
        int64_t & out_present_slop_ns,
        int64_t & out_predicted_display_time_ns)
{
	std::lock_guard lock(mutex);
	frame_id = this->frame_id++;
	auto now = os_monotonic_get_ns();

	// The nominal schedule advances by exactly one period per frame. If the
	// bumped value were stored back into last_ns, a single stall would shift
	// the whole schedule for the rest of the session: the phase loop in
	// on_feedback only ever sees last_ns modulo the period, so it could never
	// give the offset back.
	int64_t nominal_client_render = last_ns + frame_duration_ns;
	last_ns = nominal_client_render;

	// Catch up the current frame only. The bump is recomputed from now on every
	// frame, so it self-clears as soon as the schedule is on time again.
	int64_t predicted_client_render = nominal_client_render;
	int64_t earliest_render_ns = now + mean_wake_up_to_present_ns + safe_present_to_decoded_ns;
	int64_t deficit_ns = earliest_render_ns - nominal_client_render;
	if (frame_duration_ns > 0 and deficit_ns > 0)
	{
		int64_t periods = deficit_ns / frame_duration_ns;

		if (periods > max_bump_periods)
		{
			U_LOG_W("pacer: schedule %.1f ms behind, bumping by %ld periods instead of %ld",
			        deficit_ns / 1e6,
			        long(max_bump_periods),
			        long(periods));

			// More than a couple of periods behind is not a late frame but a
			// stale schedule (suspend, reset, clock jump). Catching up a couple
			// of periods per frame would never be enough, as last_ns advances
			// at the same rate as the clock: re-anchor the nominal schedule on
			// now and present this frame as early as possible, which is what
			// the old unbounded bump did in that case.
			last_ns = earliest_render_ns - frame_duration_ns;
			predicted_client_render = earliest_render_ns;
		}
		else
			predicted_client_render += periods * frame_duration_ns;
	}

	out_predicted_display_time_ns = predicted_client_render + mean_render_to_display_ns;
	out_desired_present_time_ns = predicted_client_render - safe_present_to_decoded_ns;
	out_wake_up_time_ns = out_desired_present_time_ns - mean_wake_up_to_present_ns + margin_ns; // we should be awoken early by the application
	last_wake_up_ns = out_wake_up_time_ns;

	in_flight_frames[frame_id % in_flight_frames.size()] = {
	        .frame_id = frame_id,
	        .present_ns = out_desired_present_time_ns,
	        .predicted_display_time = out_predicted_display_time_ns,
	};

	out_present_slop_ns = slop_ns;

	// The schedule is a p99.5 percentile controller over a long window (5000
	// frames by default), so a stage win does not move the median; these numbers
	// are what actually decide the latency. deficit is what the bump above had
	// to absorb, so a non-zero value means the schedule is late. Opt-in, 1 Hz.
	static const bool pacer_stats = getenv("WIVRN_PACER_STATS") != nullptr;
	if (pacer_stats and frame_id % 90 == 0)
	{
		U_LOG_I("pacer: frame %ld lead %.1f ms (predicted_render-now), safe_present_to_decoded %.1f ms, wake_to_present %.1f ms, render_to_display %.1f ms, display_delay %.1f ms, deficit %.1f ms",
		        long(frame_id),
		        (predicted_client_render - now) / 1e6,
		        safe_present_to_decoded_ns / 1e6,
		        mean_wake_up_to_present_ns / 1e6,
		        mean_render_to_display_ns / 1e6,
		        (out_predicted_display_time_ns - out_desired_present_time_ns) / 1e6,
		        deficit_ns / 1e6);
	}
}

void pacer::on_feedback(const wivrn::from_headset::feedback & feedback, const clock_offset & offset)
{
	if (feedback.times_displayed > 1 or not feedback.blitted)
		return;

	std::lock_guard lock(mutex);
	auto & when = in_flight_frames[feedback.frame_index % in_flight_frames.size()];
	if (when.frame_id != feedback.frame_index)
		return;

	auto & times = frame_times[feedback.frame_index % frame_times.size()];
	if (times.frame_id != feedback.frame_index)
	{
		times.frame_id = feedback.frame_index;
		times.present = when.present_ns;
		times.decoded = 0;
	}
	times.decoded = std::max(times.decoded, offset.from_headset(feedback.received_from_decoder));

	if (feedback.stream_index == 0)
	{
		if (feedback.frame_index % 100 == 0)
		{
			std::unique_lock lock(compute_mutex);
			std::swap(frame_times, frame_times_compute);
			compute_cv.notify_all();
		}
		// adjust phase
		auto d = (frame_duration_ns / 2 + offset.from_headset(feedback.blitted) - last_ns) % frame_duration_ns;
		if (d < 0)
			d += frame_duration_ns;
		d -= frame_duration_ns / 2;
		last_ns += d / 10;
	}

	if (feedback.displayed and feedback.displayed > feedback.blitted and feedback.displayed < feedback.blitted + 100'000'000)
	{
		int64_t sample_ns = feedback.displayed - feedback.blitted;
		// Seed from the first sample: lerping from 0 would make
		// view_info.display_time up to 20 ms early for the first ~30 frames of
		// a session, which is exactly when a bad estimate is most visible.
		if (mean_render_to_display_ns == 0)
			mean_render_to_display_ns = sample_ns;
		else
			mean_render_to_display_ns = std::lerp(mean_render_to_display_ns, sample_ns, 0.1);
	}
}
void pacer::mark_timing_point(
        comp_target_timing_point point,
        int64_t frame_id,
        int64_t when_ns)
{
	switch (point)
	{
		//! Woke up after sleeping in wait frame.
		case COMP_TARGET_TIMING_POINT_WAKE_UP:
			return;

		//! Began CPU side work for GPU.
		case COMP_TARGET_TIMING_POINT_BEGIN:
			return;

		//! Just before submitting work to the GPU.
		case COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN:
			return;

		//! Just after submitting work to the GPU.
		case COMP_TARGET_TIMING_POINT_SUBMIT_END:
			if (when_ns > last_wake_up_ns and when_ns < last_wake_up_ns + 100'000'000)
			{
				std::lock_guard lock(mutex);
				int64_t sample_ns = when_ns - last_wake_up_ns;
				// Same as render_to_display: seed from the first sample rather
				// than lerping from a guess, so the estimate does not spend the
				// first ~30 frames of a session converging.
				if (mean_wake_up_to_present_ns == 0)
					mean_wake_up_to_present_ns = sample_ns;
				else
					mean_wake_up_to_present_ns = std::lerp(mean_wake_up_to_present_ns, sample_ns, 0.1);
			}
	}
}

pacer::frame_info pacer::present_to_info(int64_t present)
{
	std::lock_guard lock(mutex);
	for (const auto & info: in_flight_frames)
	{
		if (info.present_ns == present)
			return info;
	}
	assert(false);
	return {};
}

void pacer::reset()
{
	std::lock_guard lock(mutex);
	std::ranges::fill(frame_times, frame_time{});

	// Forget everything learned from the previous connection or refresh rate: a
	// stale offset would make the first frames of the new session be predicted
	// early or late, and there is no way for the phase loop to recover it.
	// 0 is the "no sample yet" value for the means, they are seeded by the
	// first measurement.
	safe_present_to_decoded_ns = 0;
	mean_render_to_display_ns = 0;
	mean_wake_up_to_present_ns = 0;
	last_ns = 0;
	last_wake_up_ns = 0;
	for (auto & info: in_flight_frames)
		info = frame_info{.frame_id = -1, .present_ns = 0, .predicted_display_time = 0};
}
} // namespace wivrn
