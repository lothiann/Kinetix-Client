package org.ddnet.client;

import android.app.NativeActivity;
import android.content.*;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.*;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

import androidx.core.content.ContextCompat;

import org.libsdl.app.SDLActivity;

public class ClientActivity extends SDLActivity {

	private static final int COMMAND_RESTART_APP = SDLActivity.COMMAND_USER + 1;
	private static final int COMMAND_PICK_FILE = SDLActivity.COMMAND_USER + 2;

	private static final int FILE_PICK_REQUEST_SCRIPT = 47110;
	private static final int FILE_PICK_REQUEST_TAS = 47111;

	private String[] launchArguments = new String[0];

	private final Object serverServiceMonitor = new Object();
	private Messenger serverServiceMessenger = null;
	private final ServiceConnection serverServiceConnection = new ServiceConnection() {
		@Override
		public void onServiceConnected(ComponentName name, IBinder service) {
			synchronized(serverServiceMonitor) {
				serverServiceMessenger = new Messenger(service);
			}
		}

		@Override
		public void onServiceDisconnected(ComponentName name) {
			synchronized(serverServiceMonitor) {
				serverServiceMessenger = null;
			}
		}
	};

	@Override
	protected String[] getLibraries() {
		return new String[] {
			"DDNet",
		};
	}

	@Override
	public void onCreate(Bundle savedInstanceState) {
		setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);

		Intent intent = getIntent();
		if(intent != null) {
			String gfxBackend = intent.getStringExtra("gfx-backend");
			if(gfxBackend != null) {
				if(gfxBackend.equals("Vulkan")) {
					launchArguments = new String[] {"gfx_backend Vulkan"};
				} else if(gfxBackend.equals("GLES")) {
					launchArguments = new String[] {"gfx_backend GLES"};
				}
			}
		}

		super.onCreate(savedInstanceState);
	}

	@Override
	protected void onDestroy() {
		super.onDestroy();
		synchronized(serverServiceMonitor) {
			if(serverServiceMessenger != null) {
				unbindService(serverServiceConnection);
			}
		}
	}

	@Override
	protected String[] getArguments() {
		return launchArguments;
	}

	@Override
	protected boolean onUnhandledMessage(int command, Object param) {
		switch(command) {
		case COMMAND_RESTART_APP:
			restartApp();
			return true;
		case COMMAND_PICK_FILE:
			pickFile((Integer)param);
			return true;
		}
		return false;
	}

	private void pickFile(int kind) {
		Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
		intent.setType("*/*");
		intent.addCategory(Intent.CATEGORY_OPENABLE);
		try {
			startActivityForResult(intent, kind == 1 ? FILE_PICK_REQUEST_TAS : FILE_PICK_REQUEST_SCRIPT);
		} catch(Exception e) {
			nativeOnFilePicked(kind, null);
		}
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		super.onActivityResult(requestCode, resultCode, data);

		int kind = -1;
		if(requestCode == FILE_PICK_REQUEST_SCRIPT) {
			kind = 0;
		} else if(requestCode == FILE_PICK_REQUEST_TAS) {
			kind = 1;
		}
		if(kind < 0) {
			return;
		}

		String path = null;
		if(resultCode == RESULT_OK && data != null && data.getData() != null) {
			path = copyPickedFile(data.getData(), kind);
		}
		nativeOnFilePicked(kind, path);
	}

	private String copyPickedFile(Uri uri, int kind) {
		String name = "picked";
		Cursor cursor = getContentResolver().query(uri, null, null, null, null);
		if(cursor != null) {
			try {
				int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
				if(nameIndex >= 0 && cursor.moveToFirst() && cursor.getString(nameIndex) != null) {
					name = cursor.getString(nameIndex);
				}
			} finally {
				cursor.close();
			}
		}
		name = name.replaceAll("[/\\\\]", "_");
		if(name.isEmpty()) {
			name = "picked";
		}

		File dir = new File(getExternalFilesDir(null), "user/" + (kind == 1 ? "tas" : "scripts"));
		dir.mkdirs();
		File destination = new File(dir, name);

		InputStream in = null;
		OutputStream out = null;
		try {
			in = getContentResolver().openInputStream(uri);
			out = new FileOutputStream(destination);
			byte[] buffer = new byte[65536];
			int read;
			while((read = in.read(buffer)) > 0) {
				out.write(buffer, 0, read);
			}
			return destination.getAbsolutePath();
		} catch(Exception e) {
			return null;
		} finally {
			try { if(in != null) in.close(); } catch(Exception e) { }
			try { if(out != null) out.close(); } catch(Exception e) { }
		}
	}

	private static native void nativeOnFilePicked(int kind, String path);

	private void restartApp() {
		Intent restartIntent =
			Intent.makeRestartActivityTask(
				getPackageManager().getLaunchIntentForPackage(
					getPackageName()
				).getComponent()
			);
		restartIntent.setPackage(getPackageName());
		startActivity(restartIntent);
	}

	// Called from native code, see android_main.cpp
	public void startServer(String[] arguments) {
		synchronized(serverServiceMonitor) {
			if(serverServiceMessenger != null) {
				return;
			}
			Intent startIntent = ServerService.createStartIntent(this, arguments);
			ContextCompat.startForegroundService(this, startIntent);
			bindService(startIntent, serverServiceConnection, 0);
		}
	}

	// Called from native code, see android_main.cpp
	public void executeCommand(String command) {
		synchronized(serverServiceMonitor) {
			if(serverServiceMessenger == null) {
				return;
			}
			try {
				serverServiceMessenger.send(ServerService.createExecuteCommandMessage(command));
			} catch (RemoteException e) {
				// Connection broken
				unbindService(serverServiceConnection);
			}
		}
	}

	// Called from native code, see android_main.cpp
	public boolean isServerRunning() {
		synchronized(serverServiceMonitor) {
			return serverServiceMessenger != null;
		}
	}
}
