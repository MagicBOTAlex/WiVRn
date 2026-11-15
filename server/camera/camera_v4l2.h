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

#include "camera_sink.h"

#include <memory>

namespace wivrn
{
// Exposes the headset cameras as v4l2loopback devices (/dev/video<N>), which every
// application (browsers, OBS, Zoom, ...) can use as a webcam.
//
// The loopback module must be loaded with enough devices. The sink uses
// /dev/video10..17 (see camera_device_first/last in wivrn_packets.h), e.g.
//   options v4l2loopback video_nr=10,11,12 card_label="WiVRn camera 0,WiVRn camera 1,WiVRn camera 2" exclusive_caps=1,1,1
std::unique_ptr<camera_sink> create_v4l2_camera_sink(const from_headset::headset_info_packet & info);

// First /dev/videoN device provided by v4l2loopback, or -1.
int find_v4l2_camera_device(int start = camera_device_first, int count = camera_device_last - camera_device_first + 1);
} // namespace wivrn
