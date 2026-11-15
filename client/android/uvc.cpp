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

#include "uvc.h"

#include <libusb.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

namespace uvc
{
namespace
{
// UVC class-specific request codes
constexpr uint8_t SET_CUR = 0x01;
constexpr uint8_t GET_CUR = 0x81;
constexpr uint8_t GET_LEN = 0x85;

constexpr uint8_t VS_PROBE_CONTROL = 0x01;
constexpr uint8_t VS_COMMIT_CONTROL = 0x02;

constexpr uint8_t CS_INTERFACE = 0x24;
constexpr uint8_t VS_INPUT_HEADER = 0x01;
constexpr uint8_t VS_FORMAT_UNCOMPRESSED = 0x04;
constexpr uint8_t VS_FRAME_UNCOMPRESSED = 0x05;
constexpr uint8_t VS_FORMAT_MJPEG = 0x06;
constexpr uint8_t VS_FRAME_MJPEG = 0x07;

// Payload header bits (bmHeaderInfo).
constexpr uint8_t HEADER_FID = 0x01;
constexpr uint8_t HEADER_EOF = 0x02;
constexpr uint8_t HEADER_ERR = 0x40;

// Transfer sizing. Isochronous transfers are a fixed number of microframes; bulk
// transfers use the payload size the device negotiated (see start()), because a
// bulk URB can contain several payloads and must be large enough to hold one.
constexpr int VS_NUM_TRANSFERS = 12;
constexpr int VS_PACKETS_PER_TRANSFER = 32;
constexpr int VS_BULK_TRANSFER_SIZE = 65536;

// A stream that delivers no frame for this long is considered dead, so the
// client can restart it (a replugged camera often just goes silent).
constexpr int64_t STALL_TIMEOUT_NS = 3'000'000'000;

// Frames waiting to be handed to the consumer. The consumer is the network
// sender, which can be slow; dropping the oldest frame is better than stalling
// the USB event thread.
constexpr size_t MAX_QUEUED_FRAMES = 2;

int64_t now_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

uint16_t get_u16(const uint8_t * p)
{
	return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

uint32_t get_u32(const uint8_t * p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

struct frame_desc
{
	uint8_t index;
	uint16_t width;
	uint16_t height;
	uint32_t max_frame_buffer_size = 0;
	std::vector<uint32_t> intervals; // 100ns units
};

struct format_desc
{
	uint8_t index;
	uint8_t type; // VS_FORMAT_MJPEG or VS_FORMAT_UNCOMPRESSED
	std::vector<frame_desc> frames;
};

struct streaming_intf
{
	int interface_number = -1;
	int entity_id = 0;
	int alt_setting = -1;
	int endpoint = -1;
	int max_packet_size = 0;
	bool is_iso = false;
	std::vector<format_desc> formats;
};

// UVC 1.0/1.1 probe/commit structure. UVC 1.5 extends it to 48 bytes; the
// fields we use are all in the first 34 bytes.
#pragma pack(push, 1)
struct probe_commit
{
	uint16_t bmHint;
	uint8_t bFormatIndex;
	uint8_t bFrameIndex;
	uint32_t dwFrameInterval;
	uint16_t wKeyFrameRate;
	uint16_t wPFrameRate;
	uint16_t wCompQuality;
	uint16_t wCompWindowSize;
	uint16_t wDelay;
	uint32_t dwMaxVideoFrameSize;
	uint32_t dwMaxPayloadTransferSize;
	uint32_t dwClockFrequency;
	uint8_t bmFramingInfo;
	uint8_t bPreferredVersion;
	uint8_t bMinVersion;
	uint8_t bMaxVersion;
};
#pragma pack(pop)

static_assert(sizeof(probe_commit) == 34);

void parse_streaming_descriptors(const libusb_interface_descriptor & desc, streaming_intf & out)
{
	const uint8_t * extra = desc.extra;
	const int len = desc.extra_length;

	format_desc * current_format = nullptr;

	int pos = 0;
	while (pos + 2 <= len)
	{
		const int bLength = extra[pos];
		if (bLength < 3 or pos + bLength > len)
			break;

		if (extra[pos + 1] == CS_INTERFACE)
		{
			const uint8_t subtype = extra[pos + 2];
			switch (subtype)
			{
				case VS_INPUT_HEADER:
					// bLength, bDescriptorType, bDescriptorSubtype, bNumFormats,
					// wTotalLength (2), bEndpointAddress, bmInfo, bTerminalLink
					if (bLength >= 9)
						out.entity_id = extra[pos + 8];
					break;

				case VS_FORMAT_MJPEG:
				case VS_FORMAT_UNCOMPRESSED: {
					if (bLength < 5)
						break;
					format_desc f;
					f.index = extra[pos + 3];
					f.type = subtype;
					out.formats.push_back(f);
					current_format = &out.formats.back();
					break;
				}

				case VS_FRAME_MJPEG:
				case VS_FRAME_UNCOMPRESSED: {
					if (not current_format or bLength < 26)
						break;
					frame_desc f;
					f.index = extra[pos + 3];
					f.width = get_u16(extra + pos + 5);
					f.height = get_u16(extra + pos + 7);
					f.max_frame_buffer_size = get_u32(extra + pos + 17);
					const uint32_t default_interval = get_u32(extra + pos + 21);
					const uint8_t interval_type = extra[pos + 25];
					if (interval_type == 0)
					{
						// continuous: min/max/step at 26/30/34
						if (bLength >= 38)
						{
							const uint32_t min_i = get_u32(extra + pos + 26);
							const uint32_t max_i = get_u32(extra + pos + 30);
							f.intervals.push_back(default_interval);
							if (min_i and max_i and min_i <= max_i)
								f.intervals.push_back(min_i);
							if (max_i)
								f.intervals.push_back(max_i);
						}
					}
					else
					{
						for (int i = 0; i < interval_type; i++)
						{
							const int off = pos + 26 + 4 * i;
							if (off + 4 > pos + bLength)
								break;
							f.intervals.push_back(get_u32(extra + off));
						}
					}
					if (f.intervals.empty() and default_interval)
						f.intervals.push_back(default_interval);
					current_format->frames.push_back(f);
					break;
				}

				default:
					break;
			}
		}

		pos += bLength;
	}
}

// Per-microframe (USB2) or per-service-interval (USB3) payload size.
int endpoint_packet_size(const libusb_endpoint_descriptor & ep, bool iso)
{
	const int multiplier = iso ? (1 + ((ep.wMaxPacketSize >> 11) & 0x3)) : 1;
	int packet = (ep.wMaxPacketSize & 0x7ff) * multiplier;

	// SuperSpeed: bits 12:11 are reserved and the budget is in the endpoint
	// companion descriptor (USB_DT_SS_ENDPOINT_COMP, type 0x30).
	for (int pos = 0; pos + 2 <= ep.extra_length;)
	{
		const int len = ep.extra[pos];
		if (len < 2)
			break;
		if (ep.extra[pos + 1] == 0x30 and len >= 6 and pos + 6 <= ep.extra_length)
		{
			const int bytes = int(ep.extra[pos + 4]) | (int(ep.extra[pos + 5]) << 8);
			if (bytes > packet)
				packet = bytes;
		}
		pos += len;
	}
	return packet;
}

int control(libusb_device_handle * devh, uint8_t request, uint8_t cs, uint16_t entity, uint8_t * data, uint16_t size)
{
	const uint16_t value = uint16_t(cs) << 8;
	const uint16_t index = uint16_t(entity);
	return libusb_control_transfer(devh, 0x21, request, value, index, data, size, 1000);
}

int control_in(libusb_device_handle * devh, uint8_t request, uint8_t cs, uint16_t entity, uint8_t * data, uint16_t size)
{
	const uint16_t value = uint16_t(cs) << 8;
	const uint16_t index = uint16_t(entity);
	return libusb_control_transfer(devh, 0xA1, request, value, index, data, size, 1000);
}
} // namespace

struct capture::impl
{
	libusb_context * ctx = nullptr;
	libusb_device_handle * devh = nullptr;
	int dup_fd = -1;
	streaming_intf intf;
	format_desc * format = nullptr;
	frame_desc * frame = nullptr;
	uint32_t interval = 0;
	bool mjpeg = false;

	callback cb;
	std::thread event_thread;
	std::atomic<bool> running{false};
	std::atomic<int64_t> started_ns{0};
	std::atomic<int64_t> last_frame_ns{0};

	// frame reassembly
	std::vector<uint8_t> buffer;
	uint8_t current_fid = 0;
	bool have_fid = false;
	bool frame_bad = false;
	uint64_t frames_dropped = 0;
	uint64_t frames_received = 0;
	uint64_t bytes_received = 0;

	// bulk payload state: a bulk URB may contain several UVC payloads, and a
	// payload may span several URBs.
	bool bulk_in_payload = false;
	bool bulk_eof = false;
	uint32_t bulk_payload_size = 0;
	uint32_t max_payload = VS_BULK_TRANSFER_SIZE;
	size_t max_frame_size = 4 * 1024 * 1024;

	// Frames are handed to the consumer from a dedicated thread so that a slow
	// network write can never stall the libusb event thread, and so that
	// destroying the capture cannot join the thread that is running the
	// callback.
	struct queued_frame
	{
		std::vector<uint8_t> data;
		uint8_t fid;
		int64_t timestamp_ns;
	};
	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	std::deque<queued_frame> queue;
	std::thread delivery_thread;
	std::atomic<bool> delivery_running{false};

	std::vector<libusb_transfer *> transfers;
	std::vector<uint8_t *> transfer_buffers;
	int transfer_size = 0;
	std::atomic<int> in_flight{0};

	~impl()
	{
		stop();
		if (devh)
		{
			if (intf.interface_number >= 0)
				libusb_release_interface(devh, intf.interface_number);
			libusb_close(devh);
		}
		// libusb does not close wrapped system devices (fd_keep), so the dup we
		// made belongs to us.
		if (dup_fd >= 0)
			close(dup_fd);
		if (ctx)
			libusb_exit(ctx);
	}

	void stop()
	{
		// Do not early-return when running is already false: the transfer callback
		// clears it on unplug, and the thread still has to be joined or its
		// destructor calls std::terminate.
		running = false;
		delivery_running = false;
		queue_cv.notify_all();

		for (auto * t: transfers)
			libusb_cancel_transfer(t);

		if (event_thread.joinable())
			event_thread.join();

		if (delivery_thread.joinable())
			delivery_thread.join();

		// If the reaper thread was never started (std::thread construction failed
		// after submitting), drain the completions here before freeing them.
		if (not event_thread.joinable() and in_flight.load() > 0)
		{
			while (in_flight.load() > 0)
			{
				for (auto * t: transfers)
					libusb_cancel_transfer(t);
				timeval tv{.tv_sec = 0, .tv_usec = 50'000};
				libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
			}
		}

		for (auto * t: transfers)
			libusb_free_transfer(t);
		transfers.clear();

		for (auto * b: transfer_buffers)
			delete[] b;
		transfer_buffers.clear();

		if (devh and intf.interface_number >= 0)
			libusb_set_interface_alt_setting(devh, intf.interface_number, 0);
	}

	void append(const uint8_t * data, int len)
	{
		if (len <= 0)
			return;
		buffer.insert(buffer.end(), data, data + len);
		if (buffer.size() > max_frame_size)
		{
			// Runaway frame (or a device ignoring dwMaxVideoFrameSize): drop it
			// and mark the frame bad so its tail is not delivered as complete.
			buffer.clear();
			frame_bad = true;
			have_fid = false;
			frames_dropped++;
		}
	}

	// A new frame starts when the FID bit toggles. Some cameras never set EOF
	// (notably when two streams run at once), so the toggle is also a valid
	// end-of-frame marker: deliver the frame instead of throwing it away.
	void start_frame(uint8_t fid, int64_t timestamp)
	{
		if (not have_fid or fid != current_fid)
		{
			if (have_fid and not buffer.empty())
				deliver(timestamp);
			buffer.clear();
			current_fid = fid;
			have_fid = true;
			frame_bad = false;
		}
	}

	void deliver(int64_t timestamp)
	{
		if (buffer.empty())
			return;
		if (frame_bad)
		{
			buffer.clear();
			frame_bad = false;
			frames_dropped++;
			return;
		}

		frames_received++;
		bytes_received += buffer.size();
		last_frame_ns = timestamp;

		{
			std::lock_guard lock(queue_mutex);
			if (queue.size() >= MAX_QUEUED_FRAMES)
			{
				queue.pop_front();
				frames_dropped++;
			}
			queue.push_back({std::move(buffer), current_fid, timestamp});
			buffer.clear();
		}
		queue_cv.notify_one();
	}

	void delivery_loop()
	{
		while (true)
		{
			queued_frame f;
			{
				std::unique_lock lock(queue_mutex);
				queue_cv.wait(lock, [this] { return not delivery_running or not queue.empty(); });
				if (queue.empty())
				{
					if (not delivery_running)
						return;
					continue;
				}
				f = std::move(queue.front());
				queue.pop_front();
			}

			if (not cb)
				continue;
			try
			{
				cb(uvc::frame{std::move(f.data), f.fid, f.timestamp_ns});
			}
			catch (const std::exception & e)
			{
				spdlog::error("UVC: frame callback failed: {}", e.what());
			}
			catch (...)
			{
				spdlog::error("UVC: frame callback failed");
			}
		}
	}

	// One isochronous packet: it carries exactly one payload, header included.
	void process_iso_payload(const uint8_t * data, int len, int64_t timestamp)
	{
		if (len < 2)
			return;

		const int header_len = data[0];
		if (header_len < 2 or header_len > len)
		{
			frames_dropped++;
			return;
		}

		const uint8_t info = data[1];
		start_frame(info & HEADER_FID, timestamp);
		if (info & HEADER_ERR)
			frame_bad = true;
		append(data + header_len, len - header_len);

		if (info & HEADER_EOF)
		{
			deliver(timestamp);
			buffer.clear();
		}
	}

	// One bulk URB. The first payload in the URB starts with a header; payloads
	// that continue in the next URB are raw data.
	void process_bulk_urb(const uint8_t * data, int len, int64_t timestamp)
	{
		int pos = 0;
		while (pos < len)
		{
			if (not bulk_in_payload)
			{
				if (len - pos < 2)
					return;

				const int header_len = data[pos];
				if (header_len < 2 or header_len > len - pos)
				{
					frames_dropped++;
					bulk_in_payload = false;
					bulk_payload_size = 0;
					return;
				}

				const uint8_t info = data[pos + 1];
				start_frame(info & HEADER_FID, timestamp);
				if (info & HEADER_ERR)
					frame_bad = true;
				bulk_eof = info & HEADER_EOF;
				// dwMaxPayloadTransferSize counts the header too.
				bulk_payload_size = header_len;
				bulk_in_payload = true;
				pos += header_len;
				continue;
			}

			const int available = len - pos;
			const int space = int(max_payload) - int(bulk_payload_size);
			const int take = std::min(available, space);
			if (take > 0)
			{
				append(data + pos, take);
				bulk_payload_size += take;
				pos += take;
			}

			if (bulk_payload_size >= max_payload)
			{
				end_payload(timestamp);
				continue;
			}
			break;
		}

		// A short URB ends the payload even when it did not reach max_payload.
		if (bulk_in_payload and len < transfer_size)
			end_payload(timestamp);
	}

	void end_payload(int64_t timestamp)
	{
		bulk_in_payload = false;
		bulk_payload_size = 0;
		if (bulk_eof)
		{
			deliver(timestamp);
			buffer.clear();
		}
	}

	void on_transfer(libusb_transfer * t)
	{
		in_flight--;

		if (t->status == LIBUSB_TRANSFER_COMPLETED)
		{
			const int64_t ts = now_ns();
			if (intf.is_iso)
			{
				for (int i = 0; i < t->num_iso_packets; i++)
				{
					const auto & p = t->iso_packet_desc[i];
					if (p.status != LIBUSB_TRANSFER_COMPLETED or p.actual_length == 0)
						continue;
					const uint8_t * ptr = libusb_get_iso_packet_buffer_simple(t, i);
					process_iso_payload(ptr, p.actual_length, ts);
				}
			}
			else if (t->actual_length > 0)
			{
				process_bulk_urb(t->buffer, t->actual_length, ts);
			}
		}
		else if (t->status == LIBUSB_TRANSFER_CANCELLED)
		{
			// stopping, do not resubmit
		}
		else if (t->status == LIBUSB_TRANSFER_NO_DEVICE)
		{
			spdlog::warn("UVC: device gone");
			running = false;
			return;
		}
		else
		{
			// TIMED_OUT / OVERFLOW / ERROR are transient; a bulk STALL needs the
			// halt cleared or every subsequent transfer stalls too. Either way the
			// transfer has to be requeued or the slot is lost forever.
			if (t->status == LIBUSB_TRANSFER_STALL and not intf.is_iso and devh)
				libusb_clear_halt(devh, intf.endpoint);
			spdlog::warn("UVC: transfer status {} on endpoint 0x{:02x}", int(t->status), intf.endpoint);
		}

		if (running and libusb_submit_transfer(t) == 0)
			in_flight++;
	}

	static void LIBUSB_CALL transfer_cb(libusb_transfer * t)
	{
		static_cast<impl *>(t->user_data)->on_transfer(t);
	}

	bool on_own_thread() const
	{
		const auto self = std::this_thread::get_id();
		return (event_thread.joinable() and event_thread.get_id() == self) or
		       (delivery_thread.joinable() and delivery_thread.get_id() == self);
	}

	void event_loop()
	{
		while (running or in_flight.load() > 0)
		{
			if (not running)
			{
				// Re-cancel every iteration: a transfer may be resubmitted after the
				// first cancel sweep, and it would then never complete.
				for (auto * t: transfers)
					libusb_cancel_transfer(t);
			}
			timeval tv{.tv_sec = 0, .tv_usec = 50'000};
			libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
		}
	}
};

capture::capture(std::unique_ptr<impl> && p) :
        p(std::move(p))
{
}

capture::~capture()
{
	if (p and p->on_own_thread())
	{
		// Destroying a capture from its own callback cannot be made safe: the
		// thread would keep running on freed memory. Leak it instead.
		spdlog::error("UVC: capture destroyed from its own thread, leaking it");
		p.release();
	}
}

int capture::width() const
{
	return p->frame ? p->frame->width : 0;
}

int capture::height() const
{
	return p->frame ? p->frame->height : 0;
}

int capture::fps() const
{
	return p->interval ? int(10'000'000 / p->interval) : 0;
}

bool capture::is_mjpeg() const
{
	return p->mjpeg;
}

bool capture::is_running() const
{
	if (not p->running)
		return false;
	// A replugged camera often just stops delivering instead of reporting an
	// error, so treat a stalled stream as dead. "No frame yet" is also a stall,
	// otherwise a camera that never delivers is never restarted.
	const int64_t last = p->last_frame_ns;
	const int64_t started = p->started_ns;
	const int64_t reference = last ? last : started;
	if (reference == 0)
		return true;
	return now_ns() - reference < STALL_TIMEOUT_NS;
}

std::unique_ptr<capture> capture::start(int fd, int width, int height, int fps, callback cb)
{
	auto impl_ptr = std::make_unique<impl>();
	impl_ptr->cb = std::move(cb);

	// Do not enumerate /dev/bus/usb: we only ever use the fd we are given, and on
	// Android enumeration needs permissions we do not have.
	libusb_init_option options[] = {
	        {.option = LIBUSB_OPTION_LOG_LEVEL, .value = {.ival = LIBUSB_LOG_LEVEL_WARNING}},
	        {.option = LIBUSB_OPTION_NO_DEVICE_DISCOVERY, .value = {.ival = 1}},
	};
	if (libusb_init_context(&impl_ptr->ctx, options, 2) < 0)
		throw std::runtime_error("libusb_init failed");

	const int dup_fd = dup(fd);
	if (dup_fd < 0)
		throw std::runtime_error("failed to dup usb fd");
	impl_ptr->dup_fd = dup_fd;

	if (libusb_wrap_sys_device(impl_ptr->ctx, intptr_t(dup_fd), &impl_ptr->devh) < 0)
		throw std::runtime_error("libusb_wrap_sys_device failed");

	libusb_device * dev = libusb_get_device(impl_ptr->devh);
	libusb_config_descriptor * config = nullptr;
	if (libusb_get_active_config_descriptor(dev, &config) < 0)
		throw std::runtime_error("no active USB configuration");

	// Find the video streaming interface: prefer the alternate setting with the
	// largest endpoint, which is the one carrying video data.
	streaming_intf best;
	for (int i = 0; i < config->bNumInterfaces; i++)
	{
		const libusb_interface & intf = config->interface[i];

		// The format/frame descriptors live in the alt setting 0 of the video
		// streaming interface; the endpoints live in the other alt settings.
		streaming_intf base;
		for (int a = 0; a < intf.num_altsetting; a++)
		{
			const libusb_interface_descriptor & alt = intf.altsetting[a];
			if (alt.bInterfaceClass != 14 or alt.bInterfaceSubClass != 2)
				continue;
			if (base.interface_number < 0)
			{
				base.interface_number = alt.bInterfaceNumber;
				parse_streaming_descriptors(alt, base);
			}
			else if (base.formats.empty())
			{
				parse_streaming_descriptors(alt, base);
			}
		}
		if (base.interface_number < 0 or base.formats.empty())
			continue;

		// Pick the alt setting with the largest endpoint (the video stream).
		for (int a = 0; a < intf.num_altsetting; a++)
		{
			const libusb_interface_descriptor & alt = intf.altsetting[a];
			if (alt.bInterfaceNumber != base.interface_number or alt.bNumEndpoints < 1)
				continue;

			const libusb_endpoint_descriptor & ep = alt.endpoint[0];
			const bool iso = (ep.bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
			const int packet = endpoint_packet_size(ep, iso);
			if (best.interface_number >= 0 and packet <= best.max_packet_size)
				continue;

			best = base;
			best.alt_setting = alt.bAlternateSetting;
			best.endpoint = ep.bEndpointAddress;
			best.max_packet_size = packet;
			best.is_iso = iso;
		}
	}
	libusb_free_config_descriptor(config);

	if (best.interface_number < 0)
		throw std::runtime_error("no UVC video streaming interface");

	impl_ptr->intf = best;

	// MJPEG only: the server decodes MJPEG and does not implement YUYV.
	format_desc * chosen_format = nullptr;
	for (auto & f: impl_ptr->intf.formats)
		if (f.type == VS_FORMAT_MJPEG)
			chosen_format = &f;
	if (not chosen_format)
		throw std::runtime_error("camera has no MJPEG format");
	impl_ptr->format = chosen_format;
	impl_ptr->mjpeg = true;

	// Pick the frame closest to the requested size, preferring the exact one.
	frame_desc * chosen_frame = nullptr;
	int best_score = INT32_MAX;
	for (auto & f: chosen_format->frames)
	{
		const int score = std::abs(int(f.width) - width) + std::abs(int(f.height) - height);
		if (score < best_score)
		{
			best_score = score;
			chosen_frame = &f;
		}
	}
	if (not chosen_frame)
		throw std::runtime_error("no UVC frame descriptor");
	impl_ptr->frame = chosen_frame;

	// Pick a frame interval: the closest one that is not faster than requested,
	// so we never use twice the bandwidth the server asked for.
	const uint32_t target_interval = fps > 0 ? uint32_t(10'000'000 / fps) : 333333;
	uint32_t chosen_interval = 0;
	for (uint32_t i: chosen_frame->intervals)
	{
		if (i == 0 or i < target_interval)
			continue;
		if (chosen_interval == 0 or i < chosen_interval)
			chosen_interval = i;
	}
	if (chosen_interval == 0)
		chosen_interval = target_interval;
	impl_ptr->interval = chosen_interval;

	spdlog::info("UVC: interface {} alt {} ep 0x{:02x} {} format {} (MJPEG), frame {} {}x{} @ {:.2f} fps",
	             best.interface_number,
	             best.alt_setting,
	             best.endpoint,
	             best.is_iso ? "iso" : "bulk",
	             chosen_format->index,
	             chosen_frame->index,
	             chosen_frame->width,
	             chosen_frame->height,
	             10'000'000.0 / chosen_interval);

	const int claim = libusb_claim_interface(impl_ptr->devh, best.interface_number);
	if (claim < 0)
		throw std::runtime_error(std::string("failed to claim UVC streaming interface: ") + libusb_error_name(claim));

	// Negotiate the format with VS_PROBE/VS_COMMIT. The control length depends on
	// the UVC version (26 bytes for 1.0, 34 for 1.1, 48 for 1.5); ask the device.
	alignas(8) std::array<uint8_t, 48> probe_buffer{};
	auto & probe = *reinterpret_cast<probe_commit *>(probe_buffer.data());

	uint8_t length_buf[2] = {};
	int probe_size = sizeof(probe_commit);
	if (control_in(impl_ptr->devh, GET_LEN, VS_PROBE_CONTROL, best.interface_number, length_buf, 2) == 2)
		probe_size = std::clamp<int>(get_u16(length_buf), 26, probe_buffer.size());

	if (control_in(impl_ptr->devh, GET_CUR, VS_PROBE_CONTROL, best.interface_number, probe_buffer.data(), probe_size) < 0)
		probe = {};

	probe.bmHint = 0x0001; // dwFrameInterval
	probe.bFormatIndex = chosen_format->index;
	probe.bFrameIndex = chosen_frame->index;
	probe.dwFrameInterval = chosen_interval;
	probe.wKeyFrameRate = 0;
	probe.wPFrameRate = 0;
	probe.wCompQuality = 0;
	probe.wCompWindowSize = 0;
	probe.dwMaxVideoFrameSize = chosen_frame->max_frame_buffer_size;
	if (probe.dwMaxVideoFrameSize == 0)
		probe.dwMaxVideoFrameSize = uint32_t(chosen_frame->width) * chosen_frame->height * 2;
	probe.dwMaxPayloadTransferSize = best.is_iso ? uint32_t(best.max_packet_size * VS_PACKETS_PER_TRANSFER) : VS_BULK_TRANSFER_SIZE;
	if (probe_size >= 34)
		probe.bmFramingInfo = 0x03; // we understand FID and EOF

	int set_result = control(impl_ptr->devh, SET_CUR, VS_PROBE_CONTROL, best.interface_number, probe_buffer.data(), probe_size);
	if (set_result < 0)
	{
		spdlog::warn("UVC: VS_PROBE failed ({}), trying without hint", set_result);
		probe.bmHint = 0;
		set_result = control(impl_ptr->devh, SET_CUR, VS_PROBE_CONTROL, best.interface_number, probe_buffer.data(), probe_size);

		// Some UVC 1.0 devices do not implement GET_LEN for the VS interface and
		// STALL the 34-byte request: try the other control sizes.
		for (int candidate: {26, 34, 48})
		{
			if (set_result >= 0)
				break;
			if (candidate == probe_size)
				continue;
			set_result = control(impl_ptr->devh, SET_CUR, VS_PROBE_CONTROL, best.interface_number, probe_buffer.data(), candidate);
			if (set_result >= 0)
				probe_size = candidate;
		}

		if (set_result < 0)
			throw std::runtime_error("UVC: failed to set probe control");
	}

	// Read the probe back: the device may clamp the interval, the payload size or
	// even pick another frame, and everything below depends on the real values.
	if (control_in(impl_ptr->devh, GET_CUR, VS_PROBE_CONTROL, best.interface_number, probe_buffer.data(), probe_size) >= 0)
	{
		if (probe.bFrameIndex != chosen_frame->index)
		{
			for (auto & f: chosen_format->frames)
				if (f.index == probe.bFrameIndex)
				{
					chosen_frame = &f;
					impl_ptr->frame = chosen_frame;
					break;
				}
		}
		if (probe.dwFrameInterval)
			impl_ptr->interval = probe.dwFrameInterval;
	}

	if (control(impl_ptr->devh, SET_CUR, VS_COMMIT_CONTROL, best.interface_number, probe_buffer.data(), probe_size) < 0)
		throw std::runtime_error("UVC: failed to commit streaming parameters");

	// A device that ignores dwMaxPayloadTransferSize would make a bulk URB end in
	// the middle of a payload, so trust the negotiated value (with sane bounds).
	if (not best.is_iso)
	{
		if (probe.dwMaxPayloadTransferSize >= 512 and probe.dwMaxPayloadTransferSize <= (1u << 20))
			impl_ptr->max_payload = probe.dwMaxPayloadTransferSize;
		else
			impl_ptr->max_payload = VS_BULK_TRANSFER_SIZE;
	}

	if (probe.dwMaxVideoFrameSize >= 1024 and probe.dwMaxVideoFrameSize <= (64u << 20))
		impl_ptr->max_frame_size = probe.dwMaxVideoFrameSize;
	else
		impl_ptr->max_frame_size = size_t(chosen_frame->width) * chosen_frame->height * 2;

	spdlog::info("UVC: negotiated {}x{} interval {} (max frame size {}, payload {})",
	             chosen_frame->width,
	             chosen_frame->height,
	             probe.dwFrameInterval,
	             probe.dwMaxVideoFrameSize,
	             probe.dwMaxPayloadTransferSize);

	if (libusb_set_interface_alt_setting(impl_ptr->devh, best.interface_number, best.alt_setting) < 0)
		throw std::runtime_error("failed to select UVC alternate setting");

	// Start transfers
	const int packet_size = best.max_packet_size;
	impl_ptr->transfer_size = best.is_iso ? packet_size * VS_PACKETS_PER_TRANSFER : int(impl_ptr->max_payload);

	for (int i = 0; i < VS_NUM_TRANSFERS; i++)
	{
		libusb_transfer * t = libusb_alloc_transfer(best.is_iso ? VS_PACKETS_PER_TRANSFER : 0);
		if (not t)
			throw std::runtime_error("failed to allocate USB transfer");

		auto * buffer = new uint8_t[impl_ptr->transfer_size];
		impl_ptr->transfer_buffers.push_back(buffer);

		if (best.is_iso)
		{
			libusb_fill_iso_transfer(t, impl_ptr->devh, best.endpoint, buffer, impl_ptr->transfer_size, VS_PACKETS_PER_TRANSFER, impl::transfer_cb, impl_ptr.get(), 0);
			libusb_set_iso_packet_lengths(t, packet_size);
		}
		else
		{
			// A finite timeout keeps a silent device from wedging the event loop.
			libusb_fill_bulk_transfer(t, impl_ptr->devh, best.endpoint, buffer, impl_ptr->transfer_size, impl::transfer_cb, impl_ptr.get(), 1000);
		}
		impl_ptr->transfers.push_back(t);
	}

	impl_ptr->running = true;
	impl_ptr->started_ns = now_ns();

	// Submit before starting the reaper: completions cannot run until events are
	// handled, and this way the thread cannot exit while submission is ongoing.
	int submitted = 0;
	for (auto * t: impl_ptr->transfers)
	{
		if (libusb_submit_transfer(t) < 0)
			spdlog::warn("UVC: failed to submit transfer");
		else
		{
			impl_ptr->in_flight++;
			submitted++;
		}
	}

	if (submitted == 0)
		throw std::runtime_error("UVC: could not submit any transfer");

	impl_ptr->event_thread = std::thread([p = impl_ptr.get()]() { p->event_loop(); });
	impl_ptr->delivery_running = true;
	impl_ptr->delivery_thread = std::thread([p = impl_ptr.get()]() { p->delivery_loop(); });

	spdlog::info("UVC: streaming started");
	return std::unique_ptr<capture>(new capture(std::move(impl_ptr)));
}
} // namespace uvc
