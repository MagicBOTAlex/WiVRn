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

#pragma once

#include "crypto.h"
#include "wivrn_packets.h"

#include "wivrn_sockets.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <poll.h>
#include <thread>
#include <type_traits>

using namespace wivrn;

class handshake_error : public std::exception
{
	std::string message;

public:
	const char * what() const noexcept override;
	handshake_error(std::string_view message);
};

class wivrn_session
{
public:
	using control_socket_t = typed_socket<TCP, to_headset::packets, from_headset::packets>;
	using stream_socket_t = typed_socket<UDP, to_headset::packets, from_headset::packets>;

private:
	control_socket_t control;
	stream_socket_t stream;

	std::atomic<uint64_t> bytes_sent_ = 0;
	std::atomic<uint64_t> bytes_received_ = 0;

	// Control traffic is written by a dedicated thread. The TCP socket is
	// blocking (5 s SO_SNDTIMEO) and its mutex is shared by the render, image
	// reader, tracking and camera threads; sending directly from those threads
	// meant one congested link froze the render thread for seconds. The handshake
	// still writes synchronously, before the writer exists.
	std::mutex control_queue_mutex;
	std::condition_variable control_queue_cv;
	std::deque<from_headset::packets> control_queue;
	std::jthread control_writer;
	std::atomic<bool> control_writer_running = false;
	std::atomic<bool> control_failed = false;
	// Queue bound: feedback is coalesced, so this only fills up with control
	// packets, which are rare.
	static constexpr size_t control_queue_max = 64;

	void queue_control(from_headset::packets && packet)
	{
		if (control_failed)
			return;

		{
			std::lock_guard lock(control_queue_mutex);
			// Coalesce latest-wins traffic: the server only cares about the newest
			// feedback per stream and the newest foveation parameters.
			if (auto * feedback = std::get_if<from_headset::feedback>(&packet))
			{
				std::erase_if(control_queue, [&](const auto & queued) {
					auto * q = std::get_if<from_headset::feedback>(&queued);
					return q and q->stream_index == feedback->stream_index;
				});
			}
			else if (std::holds_alternative<from_headset::foveation>(packet))
			{
				std::erase_if(control_queue, [](const auto & queued) {
					return std::holds_alternative<from_headset::foveation>(queued);
				});
			}

			if (control_queue.size() >= control_queue_max)
				control_queue.pop_front();

			control_queue.push_back(std::move(packet));
		}
		control_queue_cv.notify_one();
	}

	void run_control_writer()
	{
		while (true)
		{
			from_headset::packets packet;
			{
				std::unique_lock lock(control_queue_mutex);
				control_queue_cv.wait(lock, [&] {
					return not control_queue.empty() or not control_writer_running;
				});
				if (not control_writer_running)
					return;
				packet = std::move(control_queue.front());
				control_queue.pop_front();
			}

			try
			{
				// typed_socket::send takes one concrete packet type, not the
				// variant, so dispatch on the alternative.
				std::visit(
				        [this](auto && concrete) {
					        bytes_sent_ += control.send(std::forward<decltype(concrete)>(concrete));
				        },
				        std::move(packet));
			}
			catch (const std::exception & e)
			{
				// The socket is gone (or poisoned by a partial write). Stop writing
				// and let the network thread perform the normal teardown.
				control_failed = true;
				return;
			}
		}
	}

	template <typename T>
	void handshake(T address, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter);

public:
	std::variant<in_addr, in6_addr> address;

	wivrn_session(sockaddr_in6 address, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter);
	wivrn_session(sockaddr_in address, bool tcp_only, crypto::key & headset_keypair, std::function<std::string(int fd)> pin_enter);
	wivrn_session(const wivrn_session &) = delete;
	wivrn_session & operator=(const wivrn_session &) = delete;
	~wivrn_session();

	// Depth of the control (TCP) send queue. Camera frames use the same socket
	// as feedback and video keyframes; when it backs up, dropping a camera
	// frame is much cheaper than letting the whole control path stall (a
	// partial write on timeout desynchronises the framing and kills the
	// session).
	size_t control_send_queue_bytes()
	{
		return control.send_queue_bytes();
	}

	template <typename T>
	void send_control(T && packet)
	{
		// Only real packet types go through the queue; batched spans and other
		// serialised forms are written directly (they are not on the render path).
		if constexpr (std::is_constructible_v<from_headset::packets, T &&>)
		{
			if (control_writer_running)
			{
				queue_control(from_headset::packets{std::forward<T>(packet)});
				return;
			}
		}
		// Handshake path (writer not started yet) or a non-packet payload.
		bytes_sent_ += control.send(std::forward<T>(packet));
	}

	template <typename T>
	void send_stream(T && packet)
	{
		if (stream)
		{
			bytes_sent_ += stream.send(std::forward<T>(packet));
			return;
		}
		if constexpr (std::is_constructible_v<from_headset::packets, T &&>)
		{
			if (control_writer_running)
			{
				queue_control(from_headset::packets{std::forward<T>(packet)});
				return;
			}
		}
		bytes_sent_ += control.send(std::forward<T>(packet));
	}

	template <typename T>
	int poll(T && visitor, std::chrono::milliseconds timeout)
	{
		if (control_failed)
			throw std::runtime_error("control writer stopped");

		pollfd fds[2] = {};
		fds[0].events = POLLIN;
		fds[0].fd = stream.get_fd();
		fds[1].events = POLLIN;
		fds[1].fd = control.get_fd();

		while (auto packet = stream.receive_pending(&bytes_received_))
			std::visit(std::forward<T>(visitor), std::move(*packet));
		while (auto packet = control.receive_pending(&bytes_received_))
			std::visit(std::forward<T>(visitor), std::move(*packet));

		int r = ::poll(fds, std::size(fds), timeout.count());
		if (r < 0)
			throw std::system_error(errno, std::system_category());

		if (fds[0].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Error on stream socket");

		if (fds[1].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Error on control socket");

		if (fds[0].revents & POLLIN)
		{
			auto packet = stream.receive(&bytes_received_);
			if (packet)
			{
				std::visit(std::forward<T>(visitor), std::move(*packet));
			}
		}

		if (fds[1].revents & POLLIN)
		{
			auto packet = control.receive(&bytes_received_);
			if (packet)
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}

		return r;
	}

	uint64_t bytes_received() const
	{
		return bytes_received_;
	}

	uint64_t bytes_sent() const
	{
		return bytes_sent_;
	}
};
