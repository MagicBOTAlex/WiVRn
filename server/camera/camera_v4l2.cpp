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

#include "camera_v4l2.h"

#include "util/u_logging.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/videodev2.h>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace wivrn
{
namespace
{
// v4l2loopback devices are the only ones that advertise a video *output* queue.
bool is_loopback_device(int index)
{
	const std::string path = "/dev/video" + std::to_string(index);
	const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return false;

	v4l2_capability cap = {};
	const bool ok = ioctl(fd, VIDIOC_QUERYCAP, &cap) >= 0 and
	                (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT) and
	                (cap.capabilities & V4L2_CAP_STREAMING);
	close(fd);
	return ok;
}

bool is_supported_format(int format)
{
	switch (format)
	{
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUVJ420P:
		case AV_PIX_FMT_YUV422P:
		case AV_PIX_FMT_YUVJ422P:
		case AV_PIX_FMT_YUV444P:
		case AV_PIX_FMT_YUVJ444P:
			return true;
		default:
			return false;
	}
}

// MJPEG decodes to full-range (JPEG) YUV. YUV420 consumers assume limited range
// unless told otherwise, and several ignore the quantization tag, so convert the
// samples once instead of relying on metadata.
struct limited_range_lut
{
	uint8_t y[256];
	uint8_t c[256];

	limited_range_lut()
	{
		for (int i = 0; i < 256; i++)
		{
			int yv = (16 * 255 + 219 * i + 127) / 255;
			y[i] = uint8_t(std::clamp(yv, 16, 235));

			int cv = 128 + ((i - 128) * 224 + (i >= 128 ? 127 : -127)) / 255;
			c[i] = uint8_t(std::clamp(cv, 16, 240));
		}
	}
};

const limited_range_lut & limited_range()
{
	static const limited_range_lut lut;
	return lut;
}

class v4l2_camera
{
public:
	v4l2_camera(uint8_t id, std::string name, const char * device, int device_index) :
	        id(id), name(std::move(name)), device(device), device_index_value(device_index)
	{
	}

	~v4l2_camera()
	{
		stop_worker();
		stop_stream();
		if (packet)
			av_packet_free(&packet);
		if (frame)
			av_frame_free(&frame);
		if (fd >= 0)
			close(fd);
		if (codec)
			avcodec_free_context(&codec);
	}

	int device_index() const
	{
		return device_index_value;
	}

	uint8_t camera_id() const
	{
		return id;
	}

	const std::string & camera_name() const
	{
		return name;
	}

	bool dead() const
	{
		return dead_flag;
	}

	bool init()
	{
		if (not codec)
		{
			const AVCodec * decoder = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
			if (not decoder)
			{
				U_LOG_E("No MJPEG decoder available");
				return false;
			}
			codec = avcodec_alloc_context3(decoder);
			if (not codec or avcodec_open2(codec, decoder, nullptr) < 0)
			{
				U_LOG_E("Failed to open MJPEG decoder");
				return false;
			}
			packet = av_packet_alloc();
			frame = av_frame_alloc();
			if (not packet or not frame)
			{
				U_LOG_E("Failed to allocate MJPEG decode buffers");
				return false;
			}
		}

		if (not open_device())
			return false;

		start_worker();
		return true;
	}

	// Called when the v4l2 node disappeared (module reload, device removed).
	bool reopen()
	{
		const auto now = std::chrono::steady_clock::now();
		if (now < reopen_at)
			return false;
		reopen_at = now + std::chrono::seconds(5);

		if (open_device())
		{
			dead_flag = false;
			return true;
		}
		return false;
	}

	// Called from the session thread: only enqueues, never decodes.
	void process_frame(from_headset::camera_frame && frame)
	{
		if (frame.payload.empty() or frame.payload.size() > (16u << 20))
			return;

		{
			std::lock_guard lock(queue_mutex);
			if (not worker_running)
				return;
			// Drop the oldest frame: a slow consumer must not build a backlog.
			if (queue.size() >= 2)
				queue.pop_front();
			queue.push_back(std::move(frame));
		}
		queue_cv.notify_one();
	}

private:
	void start_worker()
	{
		if (worker_running.exchange(true))
			return;
		worker = std::thread([this] { worker_loop(); });
	}

	void stop_worker()
	{
		{
			std::lock_guard lock(queue_mutex);
			if (not worker_running.exchange(false))
				return;
			queue.clear();
		}
		queue_cv.notify_all();
		if (worker.joinable())
			worker.join();
	}

	void worker_loop()
	{
		while (true)
		{
			from_headset::camera_frame f;
			{
				std::unique_lock lock(queue_mutex);
				queue_cv.wait(lock, [this] { return not worker_running or not queue.empty(); });
				if (queue.empty())
				{
					if (not worker_running)
						return;
					continue;
				}
				f = std::move(queue.front());
				queue.pop_front();
			}
			decode_and_write(std::move(f));
		}
	}

	// True when the v4l2 node has a free buffer, i.e. somebody is consuming.
	// With no reader this stays false and we skip the JPEG decode entirely.
	bool can_write() const
	{
		if (not streaming or fd < 0)
			return true; // first frames: decode so configure() can run
		pollfd p{.fd = fd, .events = POLLOUT, .revents = 0};
		return poll(&p, 1, 0) > 0 and (p.revents & (POLLOUT | POLLERR));
	}

	void decode_and_write(from_headset::camera_frame && f)
	{
		if (not can_write())
			return;

		if (dead_flag and not reopen())
			return;

		av_packet_unref(packet);
		if (av_new_packet(packet, f.payload.size()) < 0)
			return;
		std::memcpy(packet->data, f.payload.data(), f.payload.size());

		const int send_result = avcodec_send_packet(codec, packet);
		av_packet_unref(packet);
		if (send_result < 0)
		{
			if (send_result == AVERROR_INVALIDDATA)
			{
				// Not an MJPEG frame (or a truncated one): resynchronise instead of
				// leaving the decoder stuck.
				avcodec_flush_buffers(codec);
				if (decode_errors++ % 300 == 0)
					U_LOG_W("Camera %u: cannot decode frame (not MJPEG?)", unsigned(id));
			}
			return;
		}

		// Drain every frame the packet produced, otherwise the decoder stays
		// "output pending" and the next send returns EAGAIN forever.
		while (true)
		{
			av_frame_unref(frame);
			const int result = avcodec_receive_frame(codec, frame);
			if (result == AVERROR(EAGAIN) or result == AVERROR_EOF)
				break;
			if (result < 0)
			{
				avcodec_flush_buffers(codec);
				break;
			}

			handle_frame(frame);
		}
	}

private:
	struct buffer
	{
		void * start = nullptr;
		size_t length = 0;
	};

	bool open_device()
	{
		if (fd >= 0)
		{
			close(fd);
			fd = -1;
		}

		// Non-blocking: VIDIOC_DQBUF blocks when a consumer holds the node open and
		// stops dequeuing, which would freeze the whole session thread.
		fd = open(device.c_str(), O_RDWR | O_NONBLOCK);
		if (fd < 0)
		{
			U_LOG_E("Cannot open %s: %s", device.c_str(), strerror(errno));
			return false;
		}

		v4l2_capability cap = {};
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
		{
			U_LOG_E("%s: VIDIOC_QUERYCAP failed: %s", device.c_str(), strerror(errno));
			close(fd);
			fd = -1;
			return false;
		}

		const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
		if (not(caps & V4L2_CAP_VIDEO_OUTPUT) or not(caps & V4L2_CAP_STREAMING))
		{
			U_LOG_E("%s is not a v4l2 output device", device.c_str());
			close(fd);
			fd = -1;
			return false;
		}

		U_LOG_I("Camera %u: v4l2 output device %s (%s)", unsigned(id), device.c_str(), cap.card);
		return true;
	}

	void handle_frame(const AVFrame * frame)
	{
		const int w = frame->width;
		const int h = frame->height;

		if (w < 16 or h < 16 or w > 4096 or h > 4096 or (w & 1) or (h & 1))
		{
			if (bad_dimensions++ % 300 == 0)
				U_LOG_W("Camera %u: refusing %dx%d frame", unsigned(id), w, h);
			return;
		}

		if (not is_supported_format(frame->format) or not frame->data[0] or not frame->data[1] or not frame->data[2])
		{
			if (unsupported_format++ % 300 == 0)
				U_LOG_W("Camera %u: unsupported decoded pixel format %d", unsigned(id), frame->format);
			return;
		}

		if (w != width or h != height)
		{
			if (not configure(w, h))
				return;
		}

		if (streaming)
			write_frame(frame);
	}

	void stop_stream()
	{
		if (fd >= 0 and streaming)
		{
			v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			ioctl(fd, VIDIOC_STREAMOFF, &type);
		}
		streaming = false;

		if (fd >= 0 and not buffers.empty())
		{
			// v4l2loopback refuses VIDIOC_S_FMT while buffers are allocated.
			v4l2_requestbuffers request = {};
			request.count = 0;
			request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			request.memory = V4L2_MEMORY_MMAP;
			ioctl(fd, VIDIOC_REQBUFS, &request);
		}

		for (auto & b: buffers)
			if (b.start and b.start != MAP_FAILED)
				munmap(b.start, b.length);
		buffers.clear();

		width = 0;
		height = 0;
		bytesperline = 0;
		sizeimage = 0;
	}

	bool configure(int w, int h)
	{
		stop_stream();

		v4l2_format format = {};
		format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		format.fmt.pix.width = w;
		format.fmt.pix.height = h;
		format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
		format.fmt.pix.field = V4L2_FIELD_NONE;
		format.fmt.pix.bytesperline = w;
		format.fmt.pix.sizeimage = size_t(w) * h * 3 / 2;
		// The samples are converted to limited range below, so tag them as such.
		// BT.601 limited range is the default assumption of every consumer.
		format.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
		format.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_601;
		format.fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
		format.fmt.pix.xfer_func = V4L2_XFER_FUNC_DEFAULT;

		if (ioctl(fd, VIDIOC_S_FMT, &format) < 0)
		{
			U_LOG_E("Camera %u: VIDIOC_S_FMT failed: %s", unsigned(id), strerror(errno));
			return false;
		}

		// The driver may clamp the size or change the pixel format: never assume it
		// accepted the request, or the writes below go past the mapping.
		v4l2_format actual = {};
		actual.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		if (ioctl(fd, VIDIOC_G_FMT, &actual) < 0)
		{
			U_LOG_E("Camera %u: VIDIOC_G_FMT failed: %s", unsigned(id), strerror(errno));
			return false;
		}

		const int stride = actual.fmt.pix.bytesperline ? int(actual.fmt.pix.bytesperline) : w;
		const size_t frame_size = size_t(stride) * h + 2 * (size_t(stride) / 2) * (size_t(h) / 2);

		if (actual.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420 or
		    actual.fmt.pix.width != unsigned(w) or
		    actual.fmt.pix.height != unsigned(h) or
		    stride < w or
		    actual.fmt.pix.sizeimage < frame_size)
		{
			U_LOG_E("Camera %u: driver gave %ux%u %08x size %u, wanted %dx%d I420",
			        unsigned(id),
			        actual.fmt.pix.width,
			        actual.fmt.pix.height,
			        actual.fmt.pix.pixelformat,
			        actual.fmt.pix.sizeimage,
			        w,
			        h);
			return false;
		}

		v4l2_requestbuffers request = {};
		request.count = 4;
		request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		request.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd, VIDIOC_REQBUFS, &request) < 0 or request.count == 0)
		{
			U_LOG_E("Camera %u: VIDIOC_REQBUFS failed: %s", unsigned(id), strerror(errno));
			return false;
		}

		for (unsigned i = 0; i < request.count; i++)
		{
			v4l2_buffer buffer = {};
			buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
			buffer.memory = V4L2_MEMORY_MMAP;
			buffer.index = i;
			if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0)
				break;

			void * start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
			if (start == MAP_FAILED)
				break;

			buffers.push_back({start, buffer.length});
			ioctl(fd, VIDIOC_QBUF, &buffer);
		}

		if (buffers.size() != request.count)
		{
			U_LOG_E("Camera %u: only %zu of %u v4l2 buffers could be mapped", unsigned(id), buffers.size(), request.count);
			stop_stream();
			return false;
		}

		v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
		{
			U_LOG_E("Camera %u: VIDIOC_STREAMON failed: %s", unsigned(id), strerror(errno));
			stop_stream();
			return false;
		}

		width = w;
		height = h;
		bytesperline = stride;
		sizeimage = frame_size;
		streaming = true;
		U_LOG_I("Camera %u: streaming %dx%d I420 (stride %d) to %s", unsigned(id), w, h, stride, device.c_str());
		return true;
	}

	// MJPEG decodes to full-range 4:2:0, 4:2:2 or 4:4:4. The v4l2 device wants
	// I420, so subsample the chroma. vf/hf are the number of source samples per
	// output chroma pixel, vertically and horizontally.
	void copy_chroma_plane(const AVFrame * frame, int plane, uint8_t * dst, int cstride, int vf, int hf) const
	{
		const int pw = width / 2;
		const int ph = height / 2;
		const uint8_t * src = frame->data[plane];
		const int ls = frame->linesize[plane];

		for (int y = 0; y < ph; y++)
		{
			uint8_t * row = dst + size_t(y) * cstride;
			const uint8_t * s0 = src + size_t(y * vf) * ls;
			const uint8_t * s1 = src + size_t(y * vf + (vf > 1 ? 1 : 0)) * ls;

			for (int x = 0; x < pw; x++)
			{
				int sum = s0[x * hf];
				int count = 1;
				if (hf > 1)
				{
					sum += s0[x * hf + 1];
					count++;
				}
				if (vf > 1)
				{
					sum += s1[x * hf];
					count++;
					if (hf > 1)
					{
						sum += s1[x * hf + 1];
						count++;
					}
				}
				row[x] = limited_range().c[sum / count];
			}
		}
	}

	void mark_dead(const char * what)
	{
		U_LOG_W("Camera %u: v4l2 device %s lost (%s)", unsigned(id), device.c_str(), what);
		dead_flag = true;
		stop_stream();
		if (fd >= 0)
		{
			close(fd);
			fd = -1;
		}
	}

	void write_frame(const AVFrame * frame)
	{
		v4l2_buffer buffer = {};
		buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buffer.memory = V4L2_MEMORY_MMAP;

		if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0)
		{
			if (errno == EAGAIN or errno == EINTR)
				return; // consumer is slow, drop this frame
			mark_dead(strerror(errno));
			return;
		}

		if (buffer.index >= buffers.size() or buffers[buffer.index].length < sizeimage)
		{
			U_LOG_W("Camera %u: v4l2 buffer %u too small, dropping", unsigned(id), buffer.index);
			if (buffer.index < buffers.size())
				ioctl(fd, VIDIOC_QBUF, &buffer);
			return;
		}

		auto * dst = static_cast<uint8_t *>(buffers[buffer.index].start);
		uint8_t * dst_y = dst;
		uint8_t * dst_u = dst_y + size_t(bytesperline) * height;
		uint8_t * dst_v = dst_u + (size_t(bytesperline) / 2) * (size_t(height) / 2);

		const auto & lut = limited_range();
		for (int y = 0; y < height; y++)
		{
			uint8_t * row = dst_y + size_t(y) * bytesperline;
			const uint8_t * src = frame->data[0] + size_t(y) * frame->linesize[0];
			for (int x = 0; x < width; x++)
				row[x] = lut.y[src[x]];
		}

		int vf = 1;
		int hf = 1;
		switch (frame->format)
		{
			case AV_PIX_FMT_YUV422P:
			case AV_PIX_FMT_YUVJ422P:
				vf = 2;
				break;
			case AV_PIX_FMT_YUV444P:
			case AV_PIX_FMT_YUVJ444P:
				vf = 2;
				hf = 2;
				break;
			default:
				break;
		}

		const int cstride = bytesperline / 2;
		copy_chroma_plane(frame, 1, dst_u, cstride, vf, hf);
		copy_chroma_plane(frame, 2, dst_v, cstride, vf, hf);

		buffer.bytesused = sizeimage;
		buffer.field = V4L2_FIELD_NONE;
		if (ioctl(fd, VIDIOC_QBUF, &buffer) < 0)
		{
			if (errno != EAGAIN and errno != EINTR)
				mark_dead(strerror(errno));
		}
	}

	uint8_t id;
	std::string name;
	std::string device;
	int device_index_value = -1;
	int fd = -1;
	int width = 0;
	int height = 0;
	int bytesperline = 0;
	size_t sizeimage = 0;
	bool streaming = false;
	bool dead_flag = false;
	std::chrono::steady_clock::time_point reopen_at{};
	uint64_t decode_errors = 0;
	uint64_t unsupported_format = 0;
	uint64_t bad_dimensions = 0;
	AVCodecContext * codec = nullptr;
	AVPacket * packet = nullptr;
	AVFrame * frame = nullptr;
	std::vector<buffer> buffers;

	// Decoding happens on this thread so the session thread only enqueues.
	std::thread worker;
	std::atomic<bool> worker_running{false};
	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	std::deque<from_headset::camera_frame> queue;
};

// Optional per-camera assignment, so a physical camera always lands on the same
// /dev/videoN:
//   ~/.config/wivrn/cameras.json
//   { "cameras": [ { "identity": "A007240223000154", "device": 10, "label": "left eye" } ] }
struct camera_config
{
	std::map<std::string, int> devices;
	std::map<std::string, std::string> labels;

	int device_for(const std::string & identity, const std::set<int> & used) const
	{
		auto it = devices.find(identity);
		if (it != devices.end() and
		    it->second >= camera_device_first and it->second <= camera_device_last and
		    not used.contains(it->second) and
		    is_loopback_device(it->second))
			return it->second;

		for (int i = camera_device_first; i <= camera_device_last; i++)
			if (not used.contains(i) and is_loopback_device(i))
				return i;
		return -1;
	}

	std::string label_for(const std::string & identity, const std::string & fallback) const
	{
		auto it = labels.find(identity);
		return it == labels.end() ? fallback : it->second;
	}
};

std::string config_path()
{
	if (const char * xdg = getenv("XDG_CONFIG_HOME"); xdg and *xdg)
		return std::string(xdg) + "/wivrn/cameras.json";
	if (const char * home = getenv("HOME"); home and *home)
		return std::string(home) + "/.config/wivrn/cameras.json";
	return {};
}

void save_camera_config(const camera_config & config)
{
	const std::string path = config_path();
	if (path.empty())
		return;

	try
	{
		nlohmann::json json;
		json["cameras"] = nlohmann::json::array();
		for (const auto & [identity, device]: config.devices)
		{
			nlohmann::json entry;
			entry["identity"] = identity;
			entry["device"] = device;
			auto label = config.labels.find(identity);
			if (label != config.labels.end())
				entry["label"] = label->second;
			json["cameras"].push_back(entry);
		}

		const std::filesystem::path target(path);
		std::filesystem::create_directories(target.parent_path());

		// Write then rename: a crash mid-write must not leave a truncated file that
		// loses every assignment.
		const std::string tmp = path + ".tmp";
		{
			std::ofstream file(tmp);
			if (not file)
			{
				U_LOG_E("Cannot write %s", tmp.c_str());
				return;
			}
			// The identity comes from the headset and may not be valid UTF-8.
			file << json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
			if (not file)
			{
				U_LOG_E("Cannot write %s", tmp.c_str());
				return;
			}
		}
		std::filesystem::rename(tmp, target);
		U_LOG_I("Saved camera assignment to %s", path.c_str());
	}
	catch (const std::exception & e)
	{
		U_LOG_E("Cannot save camera assignment: %s", e.what());
	}
}

camera_config load_camera_config()
{
	camera_config config;

	const std::string path = config_path();
	if (path.empty())
		return config;

	std::ifstream file(path);
	if (not file)
		return config;

	try
	{
		auto json = nlohmann::json::parse(file);
		for (const auto & entry: json.value("cameras", nlohmann::json::array()))
		{
			try
			{
				const std::string identity = entry.value("identity", "");
				if (identity.empty() or identity.size() > 256)
					continue;

				if (entry.contains("device"))
				{
					const int device = entry["device"].get<int>();
					if (device < camera_device_first or device > camera_device_last)
					{
						U_LOG_W("cameras.json: ignoring invalid device %d for %s", device, identity.c_str());
					}
					else
					{
						config.devices[identity] = device;
					}
				}
				if (entry.contains("label"))
					config.labels[identity] = entry["label"].get<std::string>();
			}
			catch (const std::exception & e)
			{
				U_LOG_W("cameras.json: ignoring invalid entry: %s", e.what());
			}
		}
		U_LOG_I("Loaded %zu camera assignment(s) from cameras.json", config.devices.size());
	}
	catch (const std::exception & e)
	{
		U_LOG_E("Invalid cameras.json: %s", e.what());
	}

	return config;
}

class v4l2_camera_sink : public camera_sink
{
public:
	explicit v4l2_camera_sink(const from_headset::headset_info_packet & info) :
	        config(load_camera_config())
	{
		std::vector<camera_entry> cameras;
		for (const auto & cam: info.cameras)
			cameras.push_back({.id = cam.id, .identity = cam.identity, .name = cam.name});
		update_cameras(cameras);
	}

	void update_cameras(const std::vector<camera_entry> & cameras) override
	{
		std::lock_guard lock(mutex);

		// Drop cameras that are gone
		for (auto it = nodes.begin(); it != nodes.end();)
		{
			const bool present = std::any_of(cameras.begin(), cameras.end(), [&](const camera_entry & c) {
				return c.identity == it->first;
			});
			if (present)
			{
				++it;
			}
			else
			{
				U_LOG_I("Camera %s detached", it->first.c_str());
				it = nodes.erase(it);
			}
		}

		std::set<int> used;
		for (const auto & [identity, node]: nodes)
			used.insert(node->device_index());

		id_to_identity.clear();
		for (const auto & cam: cameras)
		{
			id_to_identity[cam.id] = cam.identity;
			if (nodes.contains(cam.identity))
				continue;

			const int device = config.device_for(cam.identity, used);
			if (device < 0)
			{
				U_LOG_E("No v4l2loopback device available for camera %s", cam.identity.c_str());
				continue;
			}
			used.insert(device);

			const std::string label = config.label_for(cam.identity, cam.name);
			const std::string path = "/dev/video" + std::to_string(device);
			auto node = std::make_unique<v4l2_camera>(cam.id, label, path.c_str(), device);
			if (node->init())
			{
				U_LOG_I("Camera %s (%s) -> %s", cam.identity.c_str(), label.c_str(), path.c_str());
				nodes[cam.identity] = std::move(node);
			}
		}
	}

	bool set_camera_device(const std::string & identity, int device) override
	{
		std::lock_guard lock(mutex);

		auto it = nodes.find(identity);
		if (it != nodes.end() and it->second->device_index() == device)
			return true; // no-op: do not tear the node down
		if (it == nodes.end() and config.devices.contains(identity) and config.devices[identity] == device)
			return true;

		if (device < camera_device_first or device > camera_device_last or not is_loopback_device(device))
		{
			U_LOG_W("Camera %s: /dev/video%d is not a v4l2loopback output device", identity.c_str(), device);
			return false;
		}

		for (const auto & [other_identity, node]: nodes)
			if (other_identity != identity and node->device_index() == device)
				return false; // already taken

		if (it == nodes.end())
		{
			// Camera not attached: remember the choice, it applies when it is.
			config.devices[identity] = device;
			save_camera_config(config);
			return true;
		}

		const auto entry_id = it->second->camera_id();
		const std::string label = config.label_for(identity, it->second->camera_name());
		const std::string path = "/dev/video" + std::to_string(device);

		// Build the replacement before destroying the working node: a failed move
		// must not leave the camera without a device.
		auto node = std::make_unique<v4l2_camera>(entry_id, label, path.c_str(), device);
		if (not node->init())
		{
			U_LOG_E("Failed to move camera %s to %s", identity.c_str(), path.c_str());
			return false;
		}

		nodes.erase(it);
		nodes[identity] = std::move(node);

		config.devices[identity] = device;
		save_camera_config(config);
		U_LOG_I("Camera %s moved to %s", identity.c_str(), path.c_str());
		return true;
	}

	std::vector<std::pair<std::string, std::pair<int, std::string>>> assignment() const override
	{
		std::lock_guard lock(mutex);
		std::vector<std::pair<std::string, std::pair<int, std::string>>> result;
		for (const auto & [identity, node]: nodes)
			result.push_back({identity, {node->device_index(), node->camera_name()}});
		return result;
	}

	void process_frame(from_headset::camera_frame && frame) override
	{
		std::lock_guard lock(mutex);
		auto id_it = id_to_identity.find(frame.camera_id);
		if (id_it == id_to_identity.end())
			return;
		auto node = nodes.find(id_it->second);
		if (node != nodes.end())
			node->second->process_frame(std::move(frame));
	}

private:
	mutable std::mutex mutex;
	camera_config config;
	std::map<std::string, std::unique_ptr<v4l2_camera>> nodes; // identity -> node
	std::map<uint8_t, std::string> id_to_identity;
};
} // namespace

int find_v4l2_camera_device(int start, int count)
{
	for (int i = start; i < start + count; i++)
	{
		if (is_loopback_device(i))
			return i;
	}
	return -1;
}

std::unique_ptr<camera_sink> create_v4l2_camera_sink(const from_headset::headset_info_packet & info)
{
	return std::make_unique<v4l2_camera_sink>(info);
}
} // namespace wivrn
