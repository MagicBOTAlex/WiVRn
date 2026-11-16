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

#include "application.h"
#include "stream.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <ranges>
#include <spdlog/spdlog.h>

namespace
{
using wivrn::device_id;

std::optional<device_id> frame_gamepad_input(device_id id)
{
	switch (id)
	{
		case device_id::LEFT_MENU_CLICK:
			return device_id::GAMEPAD_VIEW_CLICK;
		case device_id::RIGHT_MENU_CLICK:
			return device_id::GAMEPAD_MENU_CLICK;
		case device_id::A_CLICK:
			return device_id::GAMEPAD_A_CLICK;
		case device_id::B_CLICK:
			return device_id::GAMEPAD_B_CLICK;
		case device_id::RIGHT_X_CLICK:
			return device_id::GAMEPAD_X_CLICK;
		case device_id::RIGHT_Y_CLICK:
			return device_id::GAMEPAD_Y_CLICK;
		case device_id::DPAD_UP_CLICK:
			return device_id::GAMEPAD_DPAD_UP_CLICK;
		case device_id::DPAD_LEFT_CLICK:
			return device_id::GAMEPAD_DPAD_LEFT_CLICK;
		case device_id::LEFT_X_CLICK:
			return device_id::GAMEPAD_DPAD_DOWN_CLICK;
		case device_id::LEFT_Y_CLICK:
			return device_id::GAMEPAD_DPAD_RIGHT_CLICK;
		case device_id::LEFT_BUMPER_CLICK:
			return device_id::GAMEPAD_SHOULDER_LEFT_CLICK;
		case device_id::RIGHT_BUMPER_CLICK:
			return device_id::GAMEPAD_SHOULDER_RIGHT_CLICK;
		case device_id::LEFT_THUMBSTICK_CLICK:
			return device_id::GAMEPAD_THUMBSTICK_LEFT_CLICK;
		case device_id::RIGHT_THUMBSTICK_CLICK:
			return device_id::GAMEPAD_THUMBSTICK_RIGHT_CLICK;
		case device_id::LEFT_TRIGGER_VALUE:
			return device_id::GAMEPAD_TRIGGER_LEFT_VALUE;
		case device_id::RIGHT_TRIGGER_VALUE:
			return device_id::GAMEPAD_TRIGGER_RIGHT_VALUE;
		case device_id::LEFT_THUMBSTICK_X:
			return device_id::GAMEPAD_THUMBSTICK_LEFT_X;
		case device_id::RIGHT_THUMBSTICK_X:
			return device_id::GAMEPAD_THUMBSTICK_RIGHT_X;
		default:
			return std::nullopt;
	}
}

constexpr std::array frame_gamepad_ids = {
        device_id::GAMEPAD_MENU_CLICK,
        device_id::GAMEPAD_VIEW_CLICK,
        device_id::GAMEPAD_A_CLICK,
        device_id::GAMEPAD_B_CLICK,
        device_id::GAMEPAD_X_CLICK,
        device_id::GAMEPAD_Y_CLICK,
        device_id::GAMEPAD_DPAD_UP_CLICK,
        device_id::GAMEPAD_DPAD_DOWN_CLICK,
        device_id::GAMEPAD_DPAD_LEFT_CLICK,
        device_id::GAMEPAD_DPAD_RIGHT_CLICK,
        device_id::GAMEPAD_SHOULDER_LEFT_CLICK,
        device_id::GAMEPAD_SHOULDER_RIGHT_CLICK,
        device_id::GAMEPAD_THUMBSTICK_LEFT_CLICK,
        device_id::GAMEPAD_THUMBSTICK_RIGHT_CLICK,
        device_id::GAMEPAD_TRIGGER_LEFT_VALUE,
        device_id::GAMEPAD_TRIGGER_RIGHT_VALUE,
        device_id::GAMEPAD_THUMBSTICK_LEFT_X,
        device_id::GAMEPAD_THUMBSTICK_LEFT_Y,
        device_id::GAMEPAD_THUMBSTICK_RIGHT_X,
        device_id::GAMEPAD_THUMBSTICK_RIGHT_Y,
};

// Query cache for read_actions().
//
// input_actions holds the inputs of every supported interaction profile (~120
// entries), but a given profile only binds a fraction of them (~25 on Touch). The
// other ones cost a xrGetActionState* call per frame each, for a result that is
// always isActive == false. We remember which actions are bound and stop querying
// the others, and query everything again when the interaction profile changes or
// periodically, so that an input can never be silently dropped.
//
// The bookkeeping only matters to read_actions(), so it lives here instead of in
// the scene header. A new stream instance gets a fresh cache: the owner and the
// action list storage identify the instance the bits belong to.
struct action_query_state
{
	const void * owner = nullptr;
	const void * actions = nullptr;
	bool active = true;          // whether read_actions() queries the action
	uint8_t inactive_frames = 0; // consecutive frames the action was not active
};

// An action is only dropped after it has been inactive for a while: a trigger at
// rest is still active in OpenXR terms, but a transient failure (or a session that
// briefly loses focus) must not make an input disappear from the packet.
constexpr uint8_t action_inactive_frames_limit = 90; // ~1 s at 90 Hz
constexpr uint32_t action_rescan_period = 300;       // ~3.3 s at 90 Hz

std::vector<action_query_state> action_states;
const void * action_states_owner = nullptr;
const void * action_states_actions = nullptr;
std::array<interaction_profile, 3> action_states_profiles{};
uint32_t action_frames_since_rescan = 0;
std::chrono::steady_clock::time_point last_action_stats_log{};
} // namespace

bool scenes::stream::chord_state::check(XrTime now)
{
	constexpr XrDuration press_tolerance = 150'000'000; // 150ms between button presses
	constexpr XrDuration click_timeout = 400'000'000;   // 400ms between chord clicks

	auto state_1 = application::read_action_bool(a);
	auto state_2 = application::read_action_bool(b);
	if (not state_1 or not state_2)
	{
		reset();
		return false;
	}

	if (clicks_done != 0 and now - first_click_time > click_timeout)
		clicks_done = 0;

	const bool down_1 = state_1->second;
	const bool down_2 = state_2->second;

	if (not down_1 and not down_2)
	{
		both_released = true;
		return false;
	}

	// require both inputs to be released between taps
	// to avoid accidental activation
	if (not(down_1 and down_2) or not both_released)
		return false;

	// both inputs are down
	both_released = false;
	const XrTime press_1 = state_1->first;
	const XrTime press_2 = state_2->first;
	const XrDuration press_separation = press_1 >= press_2 ? press_1 - press_2 : press_2 - press_1;

	// inputs were not pressed together
	if (press_separation > press_tolerance)
	{
		clicks_done = 0;
		return false;
	}

	if (clicks_done == 0)
		first_click_time = std::max(press_1, press_2);

	++clicks_done;
	if (clicks_done < clicks_needed)
		return false;

	reset();
	return true;
}

void scenes::stream::read_actions()
{
	from_headset::inputs inputs;
	const bool emulating_gamepad = using_frame_gamepad();
	const bool focused = application::is_focused();

	// The set of bound actions changes with the interaction profile. The profile
	// values are updated on this thread, so comparing them catches the change even
	// if the notification has already been consumed by the tracking thread. The
	// flag is only read here (the tracking thread consumes it), so that both can
	// react to the event.
	const std::array<interaction_profile, 3> profiles = {
	        interaction_profiles[0].load(),
	        interaction_profiles[1].load(),
	        interaction_profiles[2].load(),
	};

	const bool rescan = action_states.size() != input_actions.size() or
	                    action_states_owner != this or
	                    action_states_actions != input_actions.data() or
	                    action_states_profiles != profiles or
	                    interaction_profile_changed;

	// Also rescan periodically: a missed profile change must not leave an input
	// ignored forever.
	if (rescan or ++action_frames_since_rescan >= action_rescan_period)
	{
		action_states.assign(input_actions.size(), {});
		action_states_owner = this;
		action_states_actions = input_actions.data();
		action_states_profiles = profiles;
		action_frames_since_rescan = 0;
	}

	size_t queried = 0;
	size_t active = 0;

	if (not is_gui_interactable())
	{
		for (size_t i = 0; i < input_actions.size(); ++i)
		{
			auto & query = action_states[i];
			if (not query.active)
				continue; // Known to be unbound by the current profile

			const auto & [id, action, action_type] = input_actions[i];

			device_id output_id = id;
			if (emulating_gamepad)
			{
				auto mapped_id = frame_gamepad_input(id);
				if (not mapped_id)
					continue; // No VR inputs in gamepad mode
				output_id = *mapped_id;
			}

			// Feed the result of the query back into the cache. While the session is
			// not focused every action reports inactive without even reaching the
			// runtime, so nothing is learned in that case.
			auto update_query_state = [&](bool is_active) {
				if (is_active)
				{
					query.active = true;
					query.inactive_frames = 0;
					++active;
				}
				else if (focused and ++query.inactive_frames >= action_inactive_frames_limit)
					query.active = false;
			};

			switch (action_type)
			{
				case XR_ACTION_TYPE_BOOLEAN_INPUT: {
					auto value = application::read_action_bool(action);
					++queried;
					update_query_state(value.has_value());
					if (value)
						inputs.values.push_back({output_id, (float)value->second, value->first});
				}
				break;

				case XR_ACTION_TYPE_FLOAT_INPUT: {
					auto value = application::read_action_float(action);
					++queried;
					update_query_state(value.has_value());
					if (value)
						inputs.values.push_back({output_id, value->second, value->first});
				}
				break;

				case XR_ACTION_TYPE_VECTOR2F_INPUT: {
					auto value = application::read_action_vec2(action);
					++queried;
					update_query_state(value.has_value());
					if (value)
					{
						inputs.values.push_back({output_id, value->second.x, value->first});
						float y = value->second.y;
						if (emulating_gamepad &&
						    (id == device_id::LEFT_THUMBSTICK_X || id == device_id::RIGHT_THUMBSTICK_X))
							y = -y;
						inputs.values.push_back({(device_id)((int)output_id + 1), y, value->first});
					}
				}
				break;

				case XR_ACTION_TYPE_POSE_INPUT:
				default:
					break;
			}
		}
	}

	// Rate limited to once per second so that it can stay enabled while debugging.
	const auto now = std::chrono::steady_clock::now();
	if (now - last_action_stats_log >= std::chrono::seconds(1))
	{
		last_action_stats_log = now;
		spdlog::debug("Input actions: {} total, {} queried, {} active", input_actions.size(), queried, active);
	}

	if (emulating_gamepad)
	{
		for (auto id: frame_gamepad_ids)
		{
			if (std::ranges::none_of(inputs.values, [id](const auto & value) { return value.id == id; }))
				inputs.values.push_back({id, 0, 0});
		}
	}

	try
	{
		network_session->send_stream(std::move(inputs));
	}
	catch (std::exception & e)
	{
		spdlog::warn("Exception while sending inputs packet: {}", e.what());
	}
}

void scenes::stream::operator()(to_headset::haptics && haptics)
{
	if (using_frame_gamepad())
	{
		switch (haptics.id)
		{
			case device_id::GAMEPAD_HAPTIC_LEFT:
			case device_id::GAMEPAD_HAPTIC_LEFT_TRIGGER:
				haptics.id = device_id::LEFT_CONTROLLER_HAPTIC;
				break;
			case device_id::GAMEPAD_HAPTIC_RIGHT:
			case device_id::GAMEPAD_HAPTIC_RIGHT_TRIGGER:
				haptics.id = device_id::RIGHT_CONTROLLER_HAPTIC;
				break;
			default:
				break;
		}
	}
	auto range = haptics_actions.equal_range(haptics.id);
	for (auto it = range.first; it != range.second; ++it)
	{
		XrAction action = it->second.action;
		float old = it->second.amplitude;
		it->second.amplitude = haptics.amplitude;
		// Some runtimes may be slow to process actions
		// Skip it if not necessary
		if (old == 0 and haptics.amplitude == 0)
			continue;

		if (haptics.amplitude > 0)
			application::haptic_start(action, XR_NULL_PATH, haptics.duration.count(), haptics.frequency, std::min(1.0f, haptics.amplitude));
		else
			application::haptic_stop(action, XR_NULL_PATH);
	}
}
