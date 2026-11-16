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

#include "android_decoder.h"
#include "application.h"
#include "scenes/stream.h"
#include "utils/named_thread.h"
#include <android/hardware_buffer.h>
#include <cassert>
#include <magic_enum.hpp>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <mutex>
#include <ranges>
#include <spdlog/spdlog.h>
#include <thread>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_android.h>
#include <vulkan/vulkan_raii.hpp>

DEUGLIFY(AMediaFormat)

struct wivrn::android::decoder::mapped_hardware_buffer
{
	vk::raii::DeviceMemory memory = nullptr;
	vk::raii::Image vimage = nullptr;
	vk::Extent2D extent{};
	vk::raii::ImageView image_view = nullptr;
	vk::ImageLayout layout = vk::ImageLayout::eUndefined;
};

namespace
{
const char * mime(wivrn::video_codec codec)
{
	using c = wivrn::video_codec;
	switch (codec)
	{
		case c::h264:
			return "video/avc";
		case c::h265:
			return "video/hevc";
		case c::av1:
			return "video/av01";
		case c::raw:
			break;
	}
	assert(false);
	__builtin_unreachable();
}

void check(media_status_t status, const char * msg)
{
	if (status != AMEDIA_OK)
	{
		spdlog::error("{}: MediaCodec error {}", msg, (int)status);
		throw std::runtime_error("MediaCodec error");
	}
}

struct android_blit_handle : public decoder::blit_handle
{
	std::shared_ptr<wivrn::android::decoder::mapped_hardware_buffer> vk_data;
	std::shared_ptr<AImageReader> image_reader;
	AImage_ptr aimage;
	android_blit_handle(
	        const wivrn::from_headset::feedback & feedback,
	        const wivrn::to_headset::video_stream_data_shard::view_info_t & view_info,
	        vk::ImageView image_view,
	        vk::Image image,
	        vk::Extent2D extent,
	        vk::ImageLayout & current_layout,
	        decltype(vk_data) vk_data,
	        decltype(image_reader) image_reader,
	        decltype(aimage) aimage) :
	        decoder::blit_handle(feedback, view_info, image_view, image, extent, current_layout),
	        vk_data(vk_data),
	        image_reader(image_reader),
	        aimage(std::move(aimage)) {}
};

} // namespace

namespace wivrn::android
{

decoder::decoder(
        vk::raii::Device & device,
        vk::raii::PhysicalDevice & physical_device,
        const wivrn::to_headset::video_stream_description & description,
        uint8_t stream_index,
        std::weak_ptr<scenes::stream> weak_scene,
        shard_accumulator * accumulator) :
        stream_index(stream_index), device(device), weak_scene(weak_scene), accumulator(accumulator)
{
	spdlog::info("hbm_mutex.native_handle() = {}", (void *)hbm_mutex.native_handle());

	auto width = description.width;
	auto height = description.height / (stream_index == 2 ? 2 : 1);

	AImageReader * ir;
	check(AImageReader_newWithUsage(
	              width,
	              height,
	              AIMAGE_FORMAT_PRIVATE,
	              AHARDWAREBUFFER_USAGE_CPU_READ_NEVER | AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
	              scenes::stream::image_buffer_size + 4 /* maxImages */,
	              &ir),
	      "AImageReader_newWithUsage");
	image_reader.reset(ir, AImageReader_deleter{});

	AImageReader_ImageListener listener{this, on_image_available};
	check(AImageReader_setImageListener(ir, &listener), "AImageReader_setImageListener");

	vkGetAndroidHardwareBufferPropertiesANDROID =
	        application::get_vulkan_proc<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
	                "vkGetAndroidHardwareBufferPropertiesANDROID");
	{
		AMediaFormat_ptr format(AMediaFormat_new());
		AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, mime(description.codec[stream_index]));
		AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, width);
		AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, height);
		AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_OPERATING_RATE, std::ceil(description.frame_rate));
		AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_PRIORITY, 0);
		// Low-latency mode: the default decoder keeps extra internal buffering and
		// sometimes a reorder pipeline, which costs about a frame.
		AMediaFormat_setInt32(format.get(), "low-latency", 1);
		AMediaFormat_setInt32(format.get(), "vendor.qti-ext-dec-low-latency.enable", 1);
		AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, width * height * 3 / 2);

		media_codec.reset(AMediaCodec_createDecoderByType(mime(description.codec[stream_index])));

		if (not media_codec)
			throw std::runtime_error(std::string("Cannot create decoder for MIME type ") + mime(description.codec[stream_index]));

		char * codec_name;
		check(AMediaCodec_getName(media_codec.get(), &codec_name), "AMediaCodec_getName");
		spdlog::info("Created MediaCodec decoder \"{}\"", codec_name);
		AMediaCodec_releaseName(media_codec.get(), codec_name);

		ANativeWindow * window;

		check(AImageReader_getWindow(image_reader.get(), &window), "AImageReader_getWindow");

		AMediaCodecOnAsyncNotifyCallback callback{
		        .onAsyncInputAvailable = decoder::on_media_input_available,
		        .onAsyncOutputAvailable = decoder::on_media_output_available,
		        .onAsyncFormatChanged = decoder::on_media_format_changed,
		        .onAsyncError = decoder::on_media_error,
		};
		check(AMediaCodec_setAsyncNotifyCallback(media_codec.get(), callback, this),
		      "AMediaCodec_setAsyncNotifyCallback");

		check(AMediaCodec_configure(media_codec.get(), format.get(), window, nullptr /* crypto */, 0 /* flags */),
		      "AMediaCodec_configure");

		check(AMediaCodec_start(media_codec.get()), "AMediaCodec_start");
	}
	worker = utils::named_thread(
	        "decoder-" + std::to_string(stream_index),
	        [this]() {
		        while (true)
		        {
			        try
			        {
				        if (jobs.pop()())
					        return;
			        }
			        catch (const utils::sync_queue_closed & e)
			        {
				        return;
			        }
			        catch (const std::exception & e)
			        {
				        spdlog::error("error in decoder thread: {}", e.what());
			        }
		        }
	        });
}

decoder::~decoder()
{
	if (image_reader)
		AImageReader_setImageListener(image_reader.get(), nullptr);
	if (media_codec)
	{
		AMediaCodec_stop(media_codec.get());
		jobs.push([]() { return true; });
		if (worker.joinable())
			worker.join();
	}
	input_buffers.close();
	jobs.close();

	if (worker.joinable())
		worker.join();

	spdlog::info("decoder::~decoder");
}

bool decoder::push_data(std::span<std::span<const uint8_t>> data, uint64_t frame_index, bool partial)
{
	// Once a frame is dropped, discard the rest of it: submitting the tail of a
	// bitstream as if it were a whole frame corrupts the decoder's reference
	// state, which is unrecoverable with an infinite GOP.
	if (frame_index == dropped_frame)
		return false;
	if (dropped_frame != UINT64_MAX)
		dropped_frame = UINT64_MAX;

	last_frame_index = frame_index;

	if (current_input_buffer.data == nullptr)
	{
		// Wait briefly for the decoder to return a buffer. Dropping the frame is
		// far more expensive than a few ms of backpressure: the server has to
		// send a keyframe, and a keyframe is the frame most likely to be lost
		// again under load (a keyframe storm). If the wait times out the frame is
		// dropped and reported, which is correct but should be rare.
		auto buffer = input_buffers.pop_for(std::chrono::milliseconds(20));
		if (not buffer)
		{
			dropped_frame = frame_index;
			auto n = frames_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
			if (n == 1 or n % 90 == 0)
				spdlog::warn("decoder {}: no input buffer, dropped frame {} ({} dropped so far)",
				             stream_index,
				             frame_index,
				             n);
			return false;
		}
		current_input_buffer = std::move(*buffer);
	}
	else if (current_input_buffer.frame_index != frame_index)
	{
		// Reuse the input buffer, discard existing data
		current_input_buffer.data_size = 0;
	}
	current_input_buffer.frame_index = frame_index;

	for (const auto & sub_data: data)
	{
		if (current_input_buffer.data_size + sub_data.size() > current_input_buffer.capacity)
		{
			auto n = frames_oversize.fetch_add(1, std::memory_order_relaxed) + 1;
			if (n == 1 or n % 90 == 0)
				spdlog::error("decoder {}: frame {} is larger than the decoder buffer ({} > {}), dropping ({} so far)",
				              stream_index,
				              frame_index,
				              current_input_buffer.data_size + sub_data.size(),
				              current_input_buffer.capacity,
				              n);
			// Discard the whole frame rather than queueing a frame with a hole.
			current_input_buffer.data_size = 0;
			dropped_frame = frame_index;
			return false;
		}

		memcpy(current_input_buffer.data + current_input_buffer.data_size, sub_data.data(), sub_data.size());
		current_input_buffer.data_size += sub_data.size();
	}

	if (partial)
		return true;

	queue_current();
	return true;
}

void decoder::queue_current()
{
	input_buffer buf = current_input_buffer;
	current_input_buffer = input_buffer{};
	if (buf.data == nullptr)
		return;

	jobs.push([this, buf, mc = media_codec.get()]() mutable {
		uint64_t timestamp = buf.frame_index * 10'000;
		auto status = AMediaCodec_queueInputBuffer(mc, buf.idx, 0, buf.data_size, timestamp, 0);
		if (status != AMEDIA_OK)
		{
			spdlog::error("AMediaCodec_queueInputBuffer: MediaCodec error {}({})",
			              int(status),
			              std::string(magic_enum::enum_name(status)).c_str());
			buf.data_size = 0;
			buf.frame_index = 0;
			input_buffers.push(std::move(buf));
		}
		return false;
	});
}

void decoder::frame_completed(const wivrn::from_headset::feedback & feedback, const wivrn::to_headset::video_stream_data_shard::view_info_t & view_info)
{
	if (not media_codec)
	{
		// If media_codec is not initialized, frame processing ends here
		if (auto scene = weak_scene.lock())
			scene->send_feedback(feedback);
	}

	// nothing required for decoder, mediacodec will callback when done
	frame_infos.push(frame_info{
	        .feedback = feedback,
	        .view_info = view_info,
	});
}

void decoder::on_image_available(void * context, AImageReader * reader)
{
	try
	{
		static_cast<decoder *>(context)->on_image_available(reader);
	}
	catch (std::exception & e)
	{
		spdlog::error("Exception in decoder::on_image_available: {}", e.what());
	}
}

void decoder::on_image_available(AImageReader * reader)
{
	assert(reader == image_reader.get());
	// Executed on image reader thread

	decltype(android_blit_handle::aimage) image;
	try
	{
		AImage * tmp;
		check(AImageReader_acquireLatestImage(image_reader.get(), &tmp), "AImageReader_acquireLatestImage");
		image.reset(tmp);

		int64_t fake_timestamp_ns;
		check(AImage_getTimestamp(image.get(), &fake_timestamp_ns), "AImage_getTimestamp");
		uint64_t frame_index = (fake_timestamp_ns + 5'000'000) / (10'000'000);

		frame_infos.drop_until([frame_index](auto & x) { return x.feedback.frame_index >= frame_index; });

		// pop_if waits for a non-empty queue with no timeout. If the matching
		// entry was dropped (drop_until above) and the stream then pauses, the
		// image-reader thread blocks forever, which stops image delivery and
		// stalls the codec. Wait briefly, then give up and drop the frame; a
		// non-matching front entry is left in place for its own image.
		auto info = frame_infos.pop_if_for(
		        [frame_index](auto & x) { return x.feedback.frame_index == frame_index; },
		        std::chrono::milliseconds(20));

		if (!info)
		{
			spdlog::warn("No frame info for frame {}, dropping frame", frame_index);
			return;
		}

		assert(info->feedback.frame_index == frame_index);

		auto vk_data = map_hardware_buffer(image.get());

		auto handle = std::make_shared<android_blit_handle>(
		        info->feedback,
		        info->view_info,
		        *vk_data->image_view,
		        *vk_data->vimage,
		        vk_data->extent,
		        vk_data->layout,
		        std::move(vk_data),
		        image_reader,
		        std::move(image));

		if (auto scene = weak_scene.lock())
			scene->push_blit_handle(accumulator, std::move(handle));
	}
	catch (...)
	{
	}
}

void decoder::create_sampler(const AHardwareBuffer_Desc & buffer_desc, vk::AndroidHardwareBufferFormatPropertiesANDROID & ahb_format)
{
	assert(ahb_format.externalFormat != 0);
	spdlog::info("AndroidHardwareBufferProperties");
	spdlog::info("  Vulkan format: {}", vk::to_string(ahb_format.format));
	spdlog::info("  External format: {:#x}", ahb_format.externalFormat);
	spdlog::info("  Format features: {}", vk::to_string(ahb_format.formatFeatures));
	spdlog::info("  samplerYcbcrConversionComponents: ({}, {}, {}, {})",
	             vk::to_string(ahb_format.samplerYcbcrConversionComponents.r),
	             vk::to_string(ahb_format.samplerYcbcrConversionComponents.g),
	             vk::to_string(ahb_format.samplerYcbcrConversionComponents.b),
	             vk::to_string(ahb_format.samplerYcbcrConversionComponents.a));
	spdlog::info("  Suggested YCbCr model: {}", vk::to_string(ahb_format.suggestedYcbcrModel));
	spdlog::info("  Suggested YCbCr range: {}", vk::to_string(ahb_format.suggestedYcbcrRange));
	spdlog::info("  Suggested X chroma offset: {}", vk::to_string(ahb_format.suggestedXChromaOffset));
	spdlog::info("  Suggested Y chroma offset: {}", vk::to_string(ahb_format.suggestedYChromaOffset));

	vk::Filter yuv_filter;
	if (ahb_format.formatFeatures & vk::FormatFeatureFlagBits::eSampledImageYcbcrConversionLinearFilter)
		yuv_filter = vk::Filter::eLinear;
	else
		yuv_filter = vk::Filter::eNearest;

	// Create VkSamplerYcbcrConversion
	vk::StructureChain ycbcr_create_info{
	        vk::SamplerYcbcrConversionCreateInfo{
	                .format = vk::Format::eUndefined,
	                .ycbcrModel = ahb_format.suggestedYcbcrModel,
	                .ycbcrRange = ahb_format.suggestedYcbcrRange,
	                .components = ahb_format.samplerYcbcrConversionComponents,
	                .xChromaOffset = ahb_format.suggestedXChromaOffset,
	                .yChromaOffset = ahb_format.suggestedYChromaOffset,
	                .chromaFilter = yuv_filter,
	        },
	        vk::ExternalFormatANDROID{
	                .externalFormat = ahb_format.externalFormat,
	        },
	};

	// suggested values from decoder don't actually read the metadata, so it's garbage
	ycbcr_create_info.get<vk::SamplerYcbcrConversionCreateInfo>().ycbcrRange = vk::SamplerYcbcrRange::eItuFull;
	ycbcr_create_info.get<vk::SamplerYcbcrConversionCreateInfo>().ycbcrModel = vk::SamplerYcbcrModelConversion::eYcbcr709;

	ycbcr_conversion = vk::raii::SamplerYcbcrConversion(device, ycbcr_create_info.get<vk::SamplerYcbcrConversionCreateInfo>());

	// Create VkSampler
	vk::StructureChain sampler_info{
	        vk::SamplerCreateInfo{
	                .magFilter = yuv_filter,
	                .minFilter = yuv_filter,
	                .mipmapMode = vk::SamplerMipmapMode::eNearest,
	                .addressModeU = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeV = vk::SamplerAddressMode::eClampToEdge,
	                .addressModeW = vk::SamplerAddressMode::eClampToEdge,
	                .mipLodBias = 0.0f,
	                .anisotropyEnable = VK_FALSE,
	                .maxAnisotropy = 1,
	                .compareEnable = VK_FALSE,
	                .compareOp = vk::CompareOp::eNever,
	                .minLod = 0.0f,
	                .maxLod = 0.0f,
	                .borderColor = vk::BorderColor::eFloatOpaqueWhite, // TODO TBC
	                .unnormalizedCoordinates = VK_FALSE,
	        },
	        vk::SamplerYcbcrConversionInfo{
	                .conversion = *ycbcr_conversion,
	        },
	};

	ycbcr_sampler = vk::raii::Sampler(device, sampler_info.get());
}

std::shared_ptr<decoder::mapped_hardware_buffer> decoder::map_hardware_buffer(AImage * image)
{
	AHardwareBuffer * hardware_buffer;
	check(AImage_getHardwareBuffer(image, &hardware_buffer), "AImage_getHardwareBuffer");

	std::unique_lock lock(hbm_mutex);

	// Cache hit first: the properties query is a driver round trip, and this
	// runs on every frame. Comparing the whole driver-filled struct could also
	// trip on padding and rebuild the conversion + clear the map every frame.
	if (auto it = hardware_buffer_map.find(hardware_buffer); it != hardware_buffer_map.end())
		return it->second;

	AHardwareBuffer_Desc buffer_desc{};
	AHardwareBuffer_describe(hardware_buffer, &buffer_desc);

	auto [properties, format_properties] = device.getAndroidHardwareBufferPropertiesANDROID<vk::AndroidHardwareBufferPropertiesANDROID, vk::AndroidHardwareBufferFormatPropertiesANDROID>(*hardware_buffer);

	if (!*ycbcr_sampler || ahb_format.externalFormat != format_properties.externalFormat)
	{
		if (*ycbcr_sampler)
		{
			spdlog::warn("decoder {}: output format changed ({} -> {}), rebuilding the YCbCr conversion",
			             stream_index,
			             ahb_format.externalFormat,
			             format_properties.externalFormat);
			// Keep the old conversion alive: image views created with it are
			// still in use.
			old_conversions.push_back(std::move(ycbcr_conversion));
		}
		memcpy(&ahb_format, &format_properties, sizeof(format_properties));
		spdlog::info("decoded image size: {}x{}", buffer_desc.width, buffer_desc.height);
		create_sampler(buffer_desc, ahb_format);
		// Do not clear hardware_buffer_map: those handles are still referenced
		// by frames in flight, and their images stay valid.
	}

	vk::StructureChain img_info{
	        vk::ImageCreateInfo{
	                .flags = {},
	                .imageType = vk::ImageType::e2D,
	                .format = vk::Format::eUndefined,
	                .extent = {buffer_desc.width, buffer_desc.height, 1},
	                .mipLevels = 1,
	                .arrayLayers = 1,
	                .samples = vk::SampleCountFlagBits::e1,
	                .tiling = vk::ImageTiling::eOptimal,
	                .usage = vk::ImageUsageFlagBits::eSampled,
	                .sharingMode = vk::SharingMode::eExclusive,
	                .initialLayout = vk::ImageLayout::eUndefined,
	        },
	        vk::ExternalMemoryImageCreateInfo{
	                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID,
	        },
	        vk::ExternalFormatANDROID{
	                .externalFormat = format_properties.externalFormat,
	        },
	};

	vk::raii::Image vimage(device, img_info.get());

	assert(properties.memoryTypeBits != 0);
	vk::StructureChain mem_info{
	        vk::MemoryAllocateInfo{
	                .allocationSize = properties.allocationSize,
	                .memoryTypeIndex = (uint32_t)(ffs(properties.memoryTypeBits) - 1),
	        },
	        vk::MemoryDedicatedAllocateInfo{
	                .image = *vimage,
	        },
	        vk::ImportAndroidHardwareBufferInfoANDROID{
	                .buffer = hardware_buffer,
	        },
	};

	vk::raii::DeviceMemory memory(device, mem_info.get());

	vimage.bindMemory(*memory, 0);

	vk::StructureChain iv_info{
	        vk::ImageViewCreateInfo{
	                .image = *vimage,
	                .viewType = vk::ImageViewType::e2D,
	                .format = vk::Format::eUndefined,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .baseMipLevel = 0,
	                        .levelCount = 1,
	                        .baseArrayLayer = 0,
	                        .layerCount = 1,
	                },
	        },
	        vk::SamplerYcbcrConversionInfo{
	                .conversion = *ycbcr_conversion,
	        },
	};

	application::ignore_debug_reports_for(*vimage);
	vk::raii::ImageView image_view(device, iv_info.get());
	application::unignore_debug_reports_for(*vimage);

	auto handle = std::make_shared<mapped_hardware_buffer>();
	handle->vimage = std::move(vimage);
	handle->image_view = std::move(image_view);
	handle->memory = std::move(memory);
	handle->extent = vk::Extent2D{
	        .width = buffer_desc.width,
	        .height = buffer_desc.height,
	};

	hardware_buffer_map[hardware_buffer] = handle;
	return handle;
}

void decoder::on_media_error(AMediaCodec *, void * userdata, media_status_t error, int32_t actionCode, const char * detail)
{
	spdlog::warn("Mediacodec error: {} (code {})", detail, int(error));

	// Any codec error leaves the decoder's reference state undefined. Flush it
	// and ask for a keyframe: the feedback must carry a real frame index,
	// otherwise the server's IDR handler ignores it (it only acts on
	// frame_index >= last_ack).
	auto self = (decoder *)userdata;
	if (auto scene = self->weak_scene.lock())
		scene->send_feedback(
		        wivrn::from_headset::feedback{
		                .frame_index = self->last_frame_index,
		                .stream_index = self->stream_index,
		                .sent_to_decoder = 0});

	// Flush on the job thread: it owns all MediaCodec calls that can race with
	// queueInputBuffer. The flush discards the corrupted reference state.
	self->jobs.push([self]() {
		if (auto mc = self->media_codec.get())
			AMediaCodec_flush(mc);
		return false;
	});
}
void decoder::on_media_format_changed(AMediaCodec *, void * userdata, AMediaFormat *)
{
	spdlog::info("Mediacodec format changed");
}
void decoder::on_media_input_available(AMediaCodec * media_codec, void * userdata, int32_t index)
{
	auto self = (decoder *)userdata;
	size_t size;
	uint8_t * buffer = AMediaCodec_getInputBuffer(media_codec, index, &size);
	self->input_buffers.push({
	        .idx = index,
	        .capacity = size,
	        .data = buffer,
	});
}
void decoder::on_media_output_available(AMediaCodec * media_codec, void * userdata, int32_t index, AMediaCodecBufferInfo * bufferInfo)
{
	auto self = (decoder *)userdata;
	self->jobs.push([=]() {
		auto status = AMediaCodec_releaseOutputBuffer(media_codec, index, true);
		// will trigger on_image_available through ImageReader
		if (status != AMEDIA_OK)
			spdlog::error("AMediaCodec_releaseOutputBuffer: MediaCodec error {}({})",
			              int(status),
			              std::string(magic_enum::enum_name(status)).c_str());
		return false;
	});
}

static bool hardware_accelerated(AMediaCodec * media_codec)
{
	// MediaCodecInfo has isHardwareAccelerated, but this does not exist in NDK.
	char * name;
	AMediaCodec_getName(media_codec, &name);
	auto release = [&]() {
		AMediaCodec_releaseName(media_codec, name);
	};
	for (const char * prefix: {
	             "OMX.google",
	             "c2.android",
	     })
	{
		if (std::string_view(name).starts_with(prefix))
		{
			release();
			return false;
		}
	}
	release();
	return true;
}

void decoder::supported_codecs(std::vector<wivrn::video_codec> & result)
{
	// Make sure we update this code when codecs are changed
	static_assert(magic_enum::enum_count<wivrn::video_codec>() == 4);

	// In order or preference, from preferred to least preferred
	for (auto codec: {
	             wivrn::video_codec::av1,
	             wivrn::video_codec::h265,
	             wivrn::video_codec::h264,
	     })
	{
		AMediaCodec_ptr media_codec(AMediaCodec_createDecoderByType(mime(codec)));

		bool supported = media_codec and hardware_accelerated(media_codec.get());
		if (supported)
			result.push_back(codec);

		spdlog::info("video codec {}: {}supported", magic_enum::enum_name(codec), supported ? "" : "NOT ");
	}
}

} // namespace wivrn::android
