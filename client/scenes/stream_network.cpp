/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#include "stream.h"

#include "application.h"
#include "utils/i18n.h"
#include "utils/named_thread.h"

#include <set>
#include <spdlog/spdlog.h>
#include <uni_algo/case.h>

void scenes::stream::process_packets()
{
#ifdef __ANDROID__
	application::instance().setup_jni();
#endif
	while (state_ != state::shutdown)
	{
#ifdef __ANDROID__
		// Independent of rendering: keeps working when the headset is idle.
		scan_cameras();
#endif

		try
		{
			network_session->poll(*this, std::chrono::milliseconds(500));
		}
		catch (std::exception & e)
		{
			spdlog::info("Exception in network thread, exiting: {}", e.what());
			exit();
		}
	}
}

void scenes::stream::operator()(to_headset::server_message && message)
{
	switch (message.kind)
	{
		case to_headset::server_message::kind::toast:
		case to_headset::server_message::kind::toast_urgent: {
			auto toast = gui_toast.lock();
			toast->emplace(message.msg, message.kind == to_headset::server_message::kind::toast_urgent);

			gui_status_last_change = instance.now();
			break;
		}

		case to_headset::server_message::kind::error: {
			auto queue = stream_error_queue.lock();
			queue->emplace(std::move(message.msg));

			break;
		}
	}
}

void scenes::stream::operator()(to_headset::video_stream_data_shard && shard)
{
	// No decoder_mutex here: setup() and this handler both run on the network
	// thread, and the render thread only reads the decoders under the lock.
	uint8_t idx = shard.stream_item_idx;
	if (idx >= decoders.size())
	{
		// We don't know (yet?) about this stream, ignore packet
		return;
	}
	decoders[idx].decoder->push_shard(std::move(shard));
}

void scenes::stream::operator()(to_headset::feature_control && control)
{
	switch (control.f)
	{
		case wivrn::to_headset::feature_control::hid_input:
			hid_forwarding = control.state;
			if (not control.state and (application::get_config().forward_keyboard or application::get_config().forward_mouse or application::get_config().forward_gamepad))
			{
				auto toast = gui_toast.lock();
				toast->emplace(_("The server does not allow forwarded input devices"), true);
				gui_status_last_change = instance.now();
			}
			return;
		case wivrn::to_headset::feature_control::microphone:
			if (audio_handle)
				audio_handle->set_mic_state(control.state);
			return;
	}
}

void scenes::stream::operator()(to_headset::audio_stream_description && desc)
{
	audio_handle.emplace(desc, *network_session, instance);
}

void scenes::stream::operator()(to_headset::camera_assignment && assignment)
{
	std::lock_guard lock(camera_assignment_mutex);
	camera_assignment = std::move(assignment.cameras);
}

void scenes::stream::operator()(to_headset::camera_stream_description && desc)
{
#ifdef __ANDROID__
	std::lock_guard lock(camera_mutex);

	if (not desc.cameras.empty())
		start_camera_sender();

	// Captures are owned per identity, not per id: ids renumber when a camera is
	// unplugged, and re-opening a device that is still captured would claim it
	// twice and then close it from under the new capture.
	std::map<std::string, std::unique_ptr<android_camera::capture>> existing;
	for (auto & [id, capture]: camera_captures)
	{
		auto identity = camera_capture_identity.find(id);
		if (identity != camera_capture_identity.end() and not identity->second.empty())
			existing[identity->second] = std::move(capture);
	}
	camera_captures.clear();
	camera_capture_identity.clear();

	std::set<std::string> wanted;
	for (const auto & cam: desc.cameras)
	{
		auto it = camera_identities.find(cam.id);
		if (it != camera_identities.end())
			wanted.insert(it->second);
	}

	// Stop the captures of cameras that are gone before starting new ones.
	for (auto it = existing.begin(); it != existing.end();)
	{
		if (not wanted.contains(it->first))
			it = existing.erase(it);
		else
			++it;
	}

	const auto now = std::chrono::steady_clock::now();

	for (const auto & cam: desc.cameras)
	{
		auto identity_it = camera_identities.find(cam.id);
		if (identity_it == camera_identities.end())
			continue; // id the headset did not advertise
		const std::string identity = identity_it->second;

		auto found = existing.find(identity);
		if (found != existing.end() and found->second and found->second->is_running())
		{
			camera_captures[cam.id] = std::move(found->second);
			camera_capture_identity[cam.id] = identity;
			existing.erase(found);
			continue;
		}
		if (found != existing.end())
			existing.erase(found); // dead: release it before restarting

		// Back off after a failure instead of tearing the USB device down again
		// a couple of seconds later.
		if (now < camera_retry_at)
			continue;

		android_camera::info info{
		        .id = cam.id,
		        .identity = identity,
		        .name = std::to_string(cam.id),
		        .vendor_id = 0,
		        .product_id = 0,
		        .granted = true,
		};

		try
		{
			camera_captures[cam.id] = android_camera::capture::start(
			        info,
			        static_cast<int>(cam.width),
			        static_cast<int>(cam.height),
			        static_cast<int>(cam.frame_rate),
			        [this](android_camera::frame && frame) { send_camera_frame(std::move(frame)); });
			camera_capture_identity[cam.id] = identity;
		}
		catch (const std::exception & e)
		{
			spdlog::error("Failed to start camera {}: {}", cam.id, e.what());
			camera_retry_at = now + std::chrono::seconds(3);
		}
	}

	spdlog::info("Streaming {} camera(s) to the server", camera_captures.size());
#else
	(void)desc;
#endif
}

#ifdef __ANDROID__
void scenes::stream::start_camera_sender()
{
	std::lock_guard lock(camera_send_mutex);
	if (camera_send_queue)
		return;

	auto queue = std::make_shared<camera_sender>();
	queue->running = true;
	camera_send_queue = queue;

	queue->thread = utils::named_thread("camera_sender", [this, queue]() {
		uint64_t errors = 0;
		while (true)
		{
			from_headset::camera_frame frame;
			{
				std::unique_lock lock(queue->mutex);
				queue->cv.wait(lock, [&queue] { return not queue->running or not queue->frames.empty(); });
				if (queue->frames.empty())
				{
					if (not queue->running)
						return;
					continue;
				}
				frame = std::move(queue->frames.front());
				queue->frames.pop_front();
			}

			try
			{
				network_session->send_control(std::move(frame));
			}
			catch (const std::exception & e)
			{
				if (errors++ % 100 == 0)
					spdlog::error("Failed to send camera frame: {}", e.what());
			}
		}
	});
}

void scenes::stream::stop_camera_sender()
{
	std::shared_ptr<camera_sender> queue;
	{
		std::lock_guard lock(camera_send_mutex);
		queue = std::move(camera_send_queue);
	}
	if (not queue)
		return;

	{
		std::lock_guard lock(queue->mutex);
		queue->running = false;
		queue->frames.clear();
	}
	queue->cv.notify_all();
	if (queue->thread.joinable())
		queue->thread.join();
}

void scenes::stream::scan_cameras()
{
	const auto now = std::chrono::steady_clock::now();
	if (now - last_camera_scan < std::chrono::seconds(2))
		return;
	last_camera_scan = now;

	std::vector<android_camera::info> cameras;
	try
	{
		auto listed = android_camera::list_cameras();
		if (not listed)
			return; // enumeration failed: keep the current topology
		cameras = std::move(*listed);
	}
	catch (const std::exception & e)
	{
		spdlog::warn("Camera scan failed: {}", e.what());
		return;
	}

	std::map<uint8_t, std::string> current;
	for (const auto & cam: cameras)
		current[static_cast<uint8_t>(cam.id)] = cam.identity;

	// What the settings page shows, independent of whether a capture is running.
	{
		std::lock_guard lock(camera_assignment_mutex);
		camera_attached.clear();
		for (const auto & cam: cameras)
			camera_attached.push_back(cam.identity);
	}

	bool need_update = false;
	{
		std::lock_guard lock(camera_mutex);

		// Drop captures that stopped delivering: keeping them would leave the
		// camera dead for the rest of the session.
		for (auto it = camera_captures.begin(); it != camera_captures.end();)
		{
			if (not it->second or not it->second->is_running())
			{
				const auto id = it->first;
				spdlog::warn("Camera {} stopped delivering", id);
				it = camera_captures.erase(it);
				camera_capture_identity.erase(id);
				camera_retry_at = now + std::chrono::seconds(3);
				need_update = true;
			}
			else
			{
				++it;
			}
		}

		if (current != camera_identities)
		{
			camera_identities = current;
			need_update = true;
		}

		if (camera_list_pending)
			need_update = true;

		// Cameras attached but not streaming (a failed or backed-off start): ask
		// the server again once the cooldown expired.
		if (not need_update and now >= camera_retry_at)
		{
			for (const auto & [id, identity]: camera_identities)
			{
				if (not camera_captures.contains(id))
				{
					need_update = true;
					break;
				}
			}
		}
	}

	if (not need_update)
		return;
	camera_list_pending = false;

	from_headset::camera_list list;
	for (const auto & cam: cameras)
		list.cameras.push_back({
		        .id = static_cast<uint8_t>(cam.id),
		        .identity = cam.identity,
		        .name = cam.name,
		});

	spdlog::info("Camera list changed: {} camera(s)", list.cameras.size());
	try
	{
		network_session->send_control(std::move(list));
	}
	catch (const std::exception & e)
	{
		spdlog::warn("Failed to send camera list: {}", e.what());
	}
}

void scenes::stream::send_camera_frame(android_camera::frame && frame)
{
	auto buffer = std::make_shared<uint8_t[]>(frame.data.size());
	std::copy(frame.data.begin(), frame.data.end(), buffer.get());

	std::shared_ptr<camera_sender> queue;
	{
		std::lock_guard lock(camera_send_mutex);
		queue = camera_send_queue;
	}
	if (not queue)
		return;

	{
		std::lock_guard lock(queue->mutex);
		if (not queue->running)
			return;
		// Drop the oldest frame: a slow link must not make the headset buffer
		// seconds of video, and the UVC thread must never block on the network.
		if (queue->frames.size() >= 2)
			queue->frames.pop_front();
		queue->frames.push_back(from_headset::camera_frame{
		        .camera_id = static_cast<uint8_t>(frame.camera_id),
		        .frame_idx = frame.frame_idx,
		        .timestamp = frame.timestamp_ns,
		        .keyframe = frame.keyframe,
		        .payload = {buffer.get(), frame.data.size()},
		        .data = {buffer},
		});
	}
	queue->cv.notify_one();
}
#endif

void scenes::stream::operator()(to_headset::video_stream_description && desc)
{
	setup(desc);

	if (not tracking_thread)
	{
		tracking_thread = utils::named_thread("tracking_thread", &stream::tracking, this);
	}
}

void scenes::stream::operator()(to_headset::refresh_rate_change && rate)
{
	spdlog::info("refresh rate change request: {}", rate.hz);
	session.set_refresh_rate(rate.hz);
	std::shared_lock lock(decoder_mutex);
	if (video_stream_description)
		video_stream_description->refresh_rate = rate.hz;
}

void scenes::stream::operator()(to_headset::stream_tab_change && tab)
{
	next_gui_status = tab.tab;
}

void scenes::stream::operator()(to_headset::timesync_query && query)
{
	network_session->send_stream(from_headset::timesync_response{
	        .query = query.query,
	        .response = instance.now(),
	});
}

void scenes::stream::operator()(audio_data && data)
{
	if (audio_handle)
		(*audio_handle)(std::move(data));
}

void scenes::stream::send_feedback(const wivrn::from_headset::feedback & feedback)
{
	try
	{
		network_session->send_control(wivrn::from_headset::feedback{feedback});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Exception while sending feedback packet: {}", e.what());
	}
}

void scenes::stream::operator()(to_headset::application_list && l)
{
	apps(std::move(l));
}

void scenes::stream::operator()(to_headset::application_icon && icon)
{
	apps(std::move(icon));
}

void scenes::stream::operator()(to_headset::running_applications && apps)
{
	*running_applications.lock() = std::move(apps);
}

void scenes::stream::start_application(std::string appid)
{
	network_session->send_control(wivrn::from_headset::start_app{
	        .app_id = std::move(appid),
	});
}
