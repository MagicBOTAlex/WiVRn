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

#include "android/camera.h"
#include "android/uvc.h"
#include "application.h"
#include "jni.h"

#include <mutex>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace android_camera
{
namespace
{
std::mutex & enumeration_mutex()
{
	static std::mutex m;
	return m;
}

std::vector<info> & enumeration()
{
	static std::vector<info> e;
	return e;
}

// Native code calls into Java from threads that have no Java frame of their own
// (the network thread). Local references created there are never released
// automatically, so every block is wrapped in a local frame.
struct local_frame
{
	JNIEnv & env;
	explicit local_frame(JNIEnv & env) :
	        env(env)
	{
		env.PushLocalFrame(64);
	}
	~local_frame()
	{
		env.PopLocalFrame(nullptr);
	}
	local_frame(const local_frame &) = delete;
	local_frame & operator=(const local_frame &) = delete;
};

// The camera class lives in the APK, so it cannot be found with FindClass from a
// native thread (that uses the system class loader). Resolve it through the
// activity's class loader and keep global refs.
struct camera_jni
{
	jclass cls = nullptr;
	jmethodID ctor = nullptr;
	jmethodID list = nullptr;
	jmethodID open = nullptr;
	jmethodID close = nullptr;
};

void throw_on_exception(JNIEnv & env, const char * what)
{
	if (not env.ExceptionCheck())
		return;
	env.ExceptionDescribe();
	env.ExceptionClear();
	throw std::runtime_error(what);
}

jobject get_context(JNIEnv & env)
{
	jni::object<""> act(application::native_app()->activity->clazz);
	auto app = act.call<jni::object<"android/app/Application">>("getApplication");
	auto ctx = app.call<jni::object<"android/content/Context">>("getApplicationContext");
	return env.NewLocalRef(ctx.self.get());
}

// Thread-safe, all-or-nothing initialisation: a partially initialised class is
// never published, and a pending Java exception is always cleared.
camera_jni & camera_class(JNIEnv & env)
{
	static camera_jni jni = [&env]() {
		camera_jni result;

		jni::object<""> act(application::native_app()->activity->clazz);
		auto loader = act.call<jni::object<"java/lang/ClassLoader">>("getClassLoader");
		auto cls = loader.call<jni::object<"java/lang/Class">>("loadClass", jni::string("org.meumeu.wivrn.UsbCamera"));
		throw_on_exception(env, "UsbCamera class not found");

		auto local = static_cast<jclass>(cls.self.get());
		result.ctor = env.GetMethodID(local, "<init>", "(Landroid/content/Context;)V");
		result.list = env.GetMethodID(local, "listCameras", "()V");
		result.open = env.GetMethodID(local, "openUsb", "(Ljava/lang/String;)I");
		result.close = env.GetMethodID(local, "closeUsb", "(Ljava/lang/String;)V");

		if (env.ExceptionCheck() or not result.ctor or not result.list or not result.open or not result.close)
		{
			env.ExceptionDescribe();
			env.ExceptionClear();
			throw std::runtime_error("UsbCamera methods not found");
		}

		result.cls = static_cast<jclass>(env.NewGlobalRef(local));
		return result;
	}();
	return jni;
}

// Returns a local reference, valid for the caller's local frame.
jobject make_object(JNIEnv & env)
{
	auto & jni = camera_class(env);
	jobject ctx = get_context(env);
	jobject obj = env.NewObject(jni.cls, jni.ctor, ctx);
	env.DeleteLocalRef(ctx);
	throw_on_exception(env, "UsbCamera creation failed");
	return obj;
}
} // namespace

std::optional<std::vector<info>> list_cameras()
{
	{
		std::lock_guard lock(enumeration_mutex());
		enumeration().clear();
	}

	auto & env = jni::jni_thread::env();

	try
	{
		local_frame frame(env);
		auto & jni = camera_class(env);
		jobject obj = make_object(env);
		env.CallVoidMethod(obj, jni.list);
		throw_on_exception(env, "Camera enumeration failed");
	}
	catch (const std::exception & e)
	{
		spdlog::warn("Camera enumeration failed: {}", e.what());
		return std::nullopt;
	}

	std::lock_guard lock(enumeration_mutex());
	return enumeration();
}

struct capture::impl
{
	jobject obj = nullptr;
	int id = 0;
	std::string identity;
	std::unique_ptr<uvc::capture> stream;

	~impl()
	{
		// Stop streaming before closing the USB device
		stream.reset();

		if (not obj)
			return;

		JNIEnv * env = jni::jni_thread::try_env();
		if (not env)
		{
			// Attach the thread rather than leaking the open USB connection.
			if (auto * app = application::native_app(); app and app->activity)
			{
				jni::jni_thread::setup_thread(app->activity->vm);
				env = jni::jni_thread::try_env();
			}
		}
		if (not env)
		{
			// Cannot touch JNI here; leaking one global ref is better than crashing.
			spdlog::error("Camera {}: no JNI environment at teardown", id);
			return;
		}

		{
			local_frame frame(*env);
			try
			{
				auto & jni = camera_class(*env);
				jni::string identity_str(identity.c_str());
				env->CallVoidMethod(obj, jni.close, static_cast<jstring>(identity_str));
				if (env->ExceptionCheck())
				{
					env->ExceptionDescribe();
					env->ExceptionClear();
				}
			}
			catch (const std::exception & e)
			{
				spdlog::warn("Camera {}: failed to close the USB device: {}", id, e.what());
			}
		}

		env->DeleteGlobalRef(obj);
		obj = nullptr;
	}
};

capture::capture(std::unique_ptr<impl> && p) :
        p(std::move(p))
{
}

bool capture::is_running() const
{
	return p and p->stream and p->stream->is_running();
}

capture::~capture() = default;

std::unique_ptr<capture> capture::start(const info & camera, int width, int height, int fps, callback cb)
{
	auto & env = jni::jni_thread::env();
	auto impl_ptr = std::make_unique<impl>();
	impl_ptr->id = camera.id;
	impl_ptr->identity = camera.identity;

	jint fd = -1;
	{
		local_frame frame(env);
		auto & jni = camera_class(env);
		jobject obj = make_object(env);
		jni::string identity(camera.identity.c_str());

		fd = env.CallIntMethod(obj, jni.open, static_cast<jstring>(identity));
		if (env.ExceptionCheck())
		{
			env.ExceptionDescribe();
			env.ExceptionClear();
			throw std::runtime_error("openUsb failed");
		}
		if (fd < 0)
			throw std::runtime_error("no file descriptor for camera " + camera.name);

		// The global reference is created before the local frame is popped; on any
		// later failure ~impl closes the connection and releases it.
		impl_ptr->obj = env.NewGlobalRef(obj);
	}

	impl_ptr->stream = uvc::capture::start(
	        fd,
	        width,
	        height,
	        fps,
	        [id = camera.id, counter = uint64_t(0), cb = std::move(cb)](uvc::frame && f) mutable {
		        frame out{
		                .camera_id = id,
		                .frame_idx = counter++,
		                .timestamp_ns = f.timestamp_ns,
		                // Every MJPEG frame is independently decodable.
		                .keyframe = true,
		                .data = std::move(f.data),
		        };
		        cb(std::move(out));
	        });

	spdlog::info("Camera {} streaming: {}x{} @ {} fps ({}), MJPEG={}",
	             camera.id,
	             impl_ptr->stream->width(),
	             impl_ptr->stream->height(),
	             impl_ptr->stream->fps(),
	             camera.name,
	             impl_ptr->stream->is_mjpeg());

	return std::unique_ptr<capture>(new capture(std::move(impl_ptr)));
}
} // namespace android_camera

extern "C" void Java_org_meumeu_wivrn_UsbCamera_reportCamera(
        JNIEnv * env,
        jobject,
        jint id,
        jstring identity,
        jstring name,
        jint vendor_id,
        jint product_id,
        jint granted)
{
	jni::jni_thread::setup_thread(env);

	const char * id_chars = env->GetStringUTFChars(identity, nullptr);
	std::string camera_identity = id_chars ? id_chars : "";
	if (id_chars)
		env->ReleaseStringUTFChars(identity, id_chars);
	else if (env->ExceptionCheck())
		env->ExceptionClear();

	const char * chars = env->GetStringUTFChars(name, nullptr);
	std::string camera_name = chars ? chars : "";
	if (chars)
		env->ReleaseStringUTFChars(name, chars);
	else if (env->ExceptionCheck())
		env->ExceptionClear();

	std::lock_guard lock(android_camera::enumeration_mutex());
	android_camera::enumeration().push_back({
	        .id = id,
	        .identity = camera_identity,
	        .name = camera_name,
	        .vendor_id = static_cast<uint16_t>(vendor_id),
	        .product_id = static_cast<uint16_t>(product_id),
	        .granted = granted != 0,
	});
}

extern "C" void Java_org_meumeu_wivrn_UsbCamera_onError(JNIEnv * env, jobject, jint id, jstring message)
{
	jni::jni_thread::setup_thread(env);

	const char * chars = env->GetStringUTFChars(message, nullptr);
	std::string msg = chars ? chars : "";
	if (chars)
		env->ReleaseStringUTFChars(message, chars);
	else if (env->ExceptionCheck())
		env->ExceptionClear();

	spdlog::error("Camera {}: {}", id, msg);
}
