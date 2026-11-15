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

package org.meumeu.wivrn;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.util.Log;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * Access to UVC cameras through Android's USB host API.
 *
 * Horizon OS ships no uvcvideo driver, so the cameras never show up in Camera2
 * or /dev/video*: they are driven from userspace (libusb) on a usbfs fd.
 *
 * Opening a UVC device requires both android.permission.CAMERA and
 * horizonos.permission.USB_CAMERA, otherwise UsbManager denies silently.
 *
 * Everything is keyed by camera identity (USB serial, else vid:pid), never by
 * enumeration order: getDeviceList() is a HashMap and its order changes when a
 * camera is plugged or unplugged.
 */
public class UsbCamera
{
	private static final String TAG = "WiVRnUsbCamera";
	private static final String ACTION_USB_PERMISSION = "org.meumeu.wivrn.USB_PERMISSION";

	private final Context context;

	// Guards all the static state below.
	private static final Object lock = new Object();

	// identity -> device, rebuilt on every enumeration
	private static final Map<String, UsbDevice> devices = new LinkedHashMap<>();
	// identity -> open connection (one per streaming camera)
	private static final Map<String, UsbDeviceConnection> connections = new HashMap<>();
	// usbfs paths we already asked a permission for, so a denied camera does not
	// pop a dialog on every scan
	private static final Set<String> requested = new HashSet<>();

	private static boolean receiver_registered = false;

	public UsbCamera(Context context)
	{
		this.context = context;

		synchronized (lock)
		{
			if (receiver_registered)
				return;

			// One receiver for the whole process: native code creates a UsbCamera
			// object for every scan, and a receiver per object is never released.
			BroadcastReceiver receiver = new BroadcastReceiver() {
				@Override
				public void onReceive(Context ctx, Intent intent)
				{
					if (!ACTION_USB_PERMISSION.equals(intent.getAction()))
						return;
					UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
					boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
					Log.i(TAG, "usb permission " + (granted ? "granted" : "denied") + " for "
					                  + (device == null ? "?" : device.getDeviceName()));
				}
			};

			IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
			if (Build.VERSION.SDK_INT >= 33)
				context.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED);
			else
				context.registerReceiver(receiver, filter);

			receiver_registered = true;
		}
	}

	private static boolean isUvc(UsbDevice dev)
	{
		for (int i = 0; i < dev.getInterfaceCount(); i++)
			if (dev.getInterface(i).getInterfaceClass() == UsbConstants.USB_CLASS_VIDEO)
				return true;
		return false;
	}

	/** Stable identity: USB serial number, else vid:pid. */
	private static String identity_of(UsbDevice dev)
	{
		try
		{
			String serial = dev.getSerialNumber();
			if (serial != null && !serial.isEmpty())
				return serial;
		}
		catch (SecurityException ignored)
		{
			// serial is only readable with USB permission
		}
		return String.format(Locale.ROOT, "%04x:%04x", dev.getVendorId(), dev.getProductId());
	}

	/**
	 * Two identical cameras (same hardcoded serial, or no serial at all) would
	 * otherwise share an identity and be merged into one stream. Disambiguate
	 * with the usbfs path, which is unique while both are plugged in.
	 */
	private static String unique_identity(UsbDevice dev, Set<String> used)
	{
		String identity = identity_of(dev);
		if (used.contains(identity))
			identity = identity + "@" + dev.getDeviceName();
		used.add(identity);
		return identity;
	}

	private static String name_of(UsbDevice dev)
	{
		String name = dev.getProductName();
		if (name == null || name.isEmpty())
			name = String.format(Locale.ROOT, "USB camera %04x:%04x", dev.getVendorId(), dev.getProductId());
		return name;
	}

	/** Enumerate UVC devices; each one is reported through reportCamera(). */
	public void listCameras()
	{
		UsbManager manager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
		if (manager == null)
		{
			onError(-1, "no UsbManager");
			return;
		}

		List<UsbDevice> found = new ArrayList<>();
		for (UsbDevice dev : manager.getDeviceList().values())
			if (isUvc(dev))
				found.add(dev);

		// getDeviceList() is a HashMap keyed by a monotonically increasing id, so
		// sort by usbfs path to keep the reported ids stable.
		found.sort((a, b) -> a.getDeviceName().compareTo(b.getDeviceName()));

		Map<String, UsbDevice> next = new LinkedHashMap<>();
		Set<String> used = new HashSet<>();
		int index = 0;
		for (UsbDevice dev : found)
		{
			if (!manager.hasPermission(dev))
			{
				// Without permission the serial number is unreadable, so the identity
				// would change the moment the user taps "Allow". Do not advertise it
				// until the identity is trustworthy.
				requestPermission(manager, dev);
				continue;
			}

			String identity = unique_identity(dev, used);
			next.put(identity, dev);
			Log.i(TAG, "camera " + index + ": " + name_of(dev) + " " + identity + " " + dev.getDeviceName());
			reportCamera(index, identity, name_of(dev), dev.getVendorId(), dev.getProductId(), 1);
			index++;
		}

		synchronized (lock)
		{
			// A replugged device gets a new usbfs path: allow asking again.
			Set<String> present = new HashSet<>();
			for (UsbDevice dev : found)
				present.add(dev.getDeviceName());
			requested.retainAll(present);

			devices.clear();
			devices.putAll(next);

			// Close connections whose camera is gone. A replugged camera gets a new
			// device path, so its stale connection must be dropped.
			Iterator<Map.Entry<String, UsbDeviceConnection>> it = connections.entrySet().iterator();
			while (it.hasNext())
			{
				Map.Entry<String, UsbDeviceConnection> entry = it.next();
				if (!next.containsKey(entry.getKey()))
				{
					entry.getValue().close();
					it.remove();
					Log.i(TAG, "closed stale connection for " + entry.getKey());
				}
			}
		}

		Log.i(TAG, next.size() + " UVC device(s)");
	}

	private void requestPermission(UsbManager manager, UsbDevice dev)
	{
		synchronized (lock)
		{
			if (!requested.add(dev.getDeviceName()))
				return; // already asked, do not pop a dialog on every scan
		}

		Log.i(TAG, "requesting usb permission for " + dev.getDeviceName());
		Intent intent = new Intent(ACTION_USB_PERMISSION).setPackage(context.getPackageName());
		// Stable request code per device, otherwise two devices share the pending
		// intent and the second dialog is never shown.
		PendingIntent pi = PendingIntent.getBroadcast(context, dev.getDeviceName().hashCode(), intent,
		        PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
		manager.requestPermission(dev, pi);
	}

	/** Open a camera by identity and return its usbfs file descriptor (-1 on failure). */
	public int openUsb(String identity)
	{
		synchronized (lock)
		{
			UsbDevice dev = devices.get(identity);
			if (dev == null)
			{
				onError(-1, "unknown camera " + identity);
				return -1;
			}

			UsbDeviceConnection existing = connections.get(identity);
			if (existing != null)
				return existing.getFileDescriptor();

			UsbManager manager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
			if (manager == null || !manager.hasPermission(dev))
			{
				onError(-1, "no USB permission for " + dev.getDeviceName());
				return -1;
			}

			UsbDeviceConnection connection = manager.openDevice(dev);
			if (connection == null)
			{
				onError(-1, "openDevice failed for " + dev.getDeviceName());
				return -1;
			}

			// Claim only the video interfaces: claiming the audio/HID interfaces of a
			// composite camera would detach them from the system.
			for (int i = 0; i < dev.getInterfaceCount(); i++)
			{
				UsbInterface intf = dev.getInterface(i);
				if (intf.getInterfaceClass() != UsbConstants.USB_CLASS_VIDEO)
					continue;
				if (!connection.claimInterface(intf, true))
					Log.w(TAG, "claimInterface failed for " + dev.getDeviceName() + " interface " + intf.getId());
			}

			connections.put(identity, connection);
			Log.i(TAG, "opened " + dev.getDeviceName() + " fd=" + connection.getFileDescriptor());
			return connection.getFileDescriptor();
		}
	}

	public void closeUsb(String identity)
	{
		UsbDeviceConnection connection;
		synchronized (lock)
		{
			connection = connections.remove(identity);
		}
		if (connection != null)
		{
			connection.close();
			Log.i(TAG, "closed camera " + identity);
		}
	}

	public native void reportCamera(int id, String identity, String name, int vendorId, int productId, int granted);

	public native void onError(int id, String message);
}
