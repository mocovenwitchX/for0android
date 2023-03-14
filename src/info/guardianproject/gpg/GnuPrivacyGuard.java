package info.guardianproject.gpg;

import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.ActivityNotFoundException;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager.NameNotFoundException;
import android.os.AsyncTask;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Message;
import android.os.Messenger;
import android.preference.PreferenceManager;
import android.support.v4.app.FragmentActivity;
import android.util.Log;
import android.view.Menu;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.View.OnClickListener;
import android.view.View.OnCreateContextMenuListener;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import info.guardianproject.gpg.apg_compat.Apg;
import info.guardianproject.gpg.apg_compat.Id;
import info.guardianproject.gpg.ui.FileDialogFragment;

import java.io.File;
import java.io.OutputStream;

public class GnuPrivacyGuard extends FragmentActivity implements OnCreateContextMenuListener {
	public static final String TAG = "GnuPrivacyGuard";

    public static final String PACKAGE_NAME = "info.guardianproject.gpg";
    public static String VERSION = null;

	private ScrollView consoleScroll;
	private TextView consoleText;

	public static final String LOG_UPDATE = "LOG_UPDATE";
	public static final String COMMAND_FINISHED = "COMMAND_FINISHED";

	private CommandThread commandThread;
	private BroadcastReceiver logUpdateReceiver;
	private BroadcastReceiver commandFinishedReceiver;
	public String command;

	private FileDialogFragment mFileDialog;

	/** Called when the activity is first created. */
	@Override
	public void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
//		NativeHelper.setup(getApplicationContext());

		// this also sets up GnuPG.context in onPostExecute()
//		new InstallAndSetupTask(this).execute();

		setContentView(R.layout.main);
		consoleScroll = (ScrollView) findViewById(R.id.consoleScroll);
		consoleText = (TextView) findViewById(R.id.consoleText);

		wireTestButtons();
	}

	@Override
	protected void onResume() {
		super.onResume();
		registerReceivers();
		startGpgAgent();
	}

	@Override
	protected void onPause() {
		super.onPause();
		unregisterReceivers();
	}

	 @Override
    public boolean onCreateOptionsMenu(Menu menu) {
	    MenuInflater inflater = getMenuInflater();
		inflater.inflate(R.menu.options_menu, menu);
		return true;
	}

	@Override
	protected void onActivityResult (int requestCode, int resultCode, Intent data) {
		Log.i(TAG, "Activity Result: " + requestCode + " " + resultCode);
		if (resultCode == RESULT_CANCELED || data == null) return;

		switch( requestCode ) {
		    case ApgId.FILENAME: {
	            if (resultCode == RESULT_OK) {
	                try {
	                    String path = data.getData().getPath();
	                    Log.d(TAG, "path=" + path);

	                    // set filename used in export/import dialogs
	                    mFileDialog.setFilename(path);
	                } catch (NullPointerException e) {
	                    Log.e(TAG, "Nullpointer while retrieving path!", e);
	                }
	            }
	            return;
	        }
		}
		Bundle extras = data.getExtras();
		if (extras != null) {
			String text = "RESULT: ";
			switch (requestCode) {
			case ApgId.SELECT_SECRET_KEY:
				long keyId = extras.getLong(Apg.EXTRA_KEY_ID);
				String userId = extras.getString(Apg.EXTRA_USER_ID);
				text += userId + " " + Long.toHexString(keyId);
				break;
			case ApgId.SELECT_PUBLIC_KEYS:
				long[] selectedKeyIds = extras.getLongArray(Apg.EXTRA_SELECTION);
				String[] userIds = extras.getStringArray(Apg.EXTRA_USER_IDS);
				if (selectedKeyIds != null && userIds != null)
					for (int i = 0; i < selectedKeyIds.length && i < userIds.length; i++) {
						text += userIds[i] + " " + Long.toHexString(selectedKeyIds[i]) + " ";
						Log.i(TAG, "received: " + userIds[i] + " " + Long.toHexString(selectedKeyIds[i]));
					}
				break;
			case Id.dialog.import_keys:
				break;
			default:
				text += "unknown intent";
			}
			Toast.makeText(this, text, Toast.LENGTH_LONG).show();
		}
	}

	@Override
	public boolean onOptionsItemSelected(MenuItem item) {
		// Set a popup EditText view to get user input
		AlertDialog.Builder alert = new AlertDialog.Builder(this);
		final EditText input = new EditText(this);
		alert.setView(input);

		switch (item.getItemId()) {
	    case R.id.menu_settings_key:
	        startActivity(new Intent(this, GPGPreferenceActivity.class));
	        return true;
		case R.id.menu_list_keys:
			startActivity(new Intent(this, KeyListActivity.class));
			return true;
		case R.id.menu_search_keys:
			alert.setTitle("Search Keys");
			alert.setPositiveButton("Search", new DialogInterface.OnClickListener() {
				@Override
                public void onClick(DialogInterface dialog, int whichButton) {
					Intent intent = new Intent(getApplicationContext(), SearchKeysActivity.class);
					intent.putExtra(Intent.EXTRA_TEXT, input.getText().toString());
					startActivity(intent);
				}
			});
			alert.show();
			return true;
		case R.id.menu_receive_key:
			final Context c = getApplicationContext();
			alert.setTitle("Receive Key");
			alert.setPositiveButton("Receive", new DialogInterface.OnClickListener() {
				@Override
                public void onClick(DialogInterface dialog, int whichButton) {
			        SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(c);
			        String ks = prefs.getString(GPGPreferenceActivity.PREF_KEYSERVER, "200.144.121.45");
					command = NativeHelper.gpg2
							+ " --keyserver " + ks + " --recv-keys " + input.getText().toString();
					commandThread = new CommandThread();
					commandThread.start();
				}
			});
			alert.show();
			return true;
		case R.id.menu_run_test:
			command = NativeHelper.app_opt + "/tests/run-tests.sh";
			commandThread = new CommandThread();
			commandThread.start();
			Log.i(TAG, "finished " + command);
			return true;
		case R.id.menu_import_key_from_file:
			final String defaultFilename = (NativeHelper.app_opt.getAbsolutePath()
					+ "/tests/pinentry/secret-keys.gpg");
			showImportFromFileDialog(defaultFilename);
			return true;
		case R.id.menu_share_log:
			shareTestLog();
			Log.i(TAG, "finished menu_share_log");
			return true;
		}
		return false;
	}

    /**
     * Show to dialog from where to import keys
     */
    public void showImportFromFileDialog(final String defaultFilename) {
        // Message is received after file is selected
        Handler returnHandler = new Handler() {
            @Override
            public void handleMessage(Message message) {
                if (message.what == FileDialogFragment.MESSAGE_OKAY) {
                    Bundle data = message.getData();
                    String importFilename = new File(data.getString(FileDialogFragment.MESSAGE_DATA_FILENAME)).getAbsolutePath();
                    boolean deleteAfterImport = data.getBoolean(FileDialogFragment.MESSAGE_DATA_CHECKED);


                    Log.d(TAG, "importFilename: " + importFilename);
                    Log.d(TAG, "deleteAfterImport: " + deleteAfterImport);
                    command = NativeHelper.gpg2 + " --import " + importFilename;
                    commandThread = new CommandThread();
                    commandThread.start();
                    if(deleteAfterImport) new File(importFilename).delete();
                }
            }
        };

        // Create a new Messenger for the communication back
        final Messenger messenger = new Messenger(returnHandler);

        new Runnable() {
            @Override
            public void run() {
                mFileDialog = FileDialogFragment.newInstance(messenger,
                        getString(R.string.title_import_keys),
                        getString(R.string.dialog_specify_import_file_msg), defaultFilename,
                        null, ApgId.FILENAME);

                mFileDialog.show(getSupportFragmentManager(), "fileDialog");
            }
        }.run();
    }

	private void updateLog() {
		final String logContents = NativeHelper.log.toString();
		if (logContents != null && logContents.trim().length() > 0)
			consoleText.setText(logContents);
		consoleScroll.scrollTo(0, consoleText.getHeight());
	}

	class CommandThread extends Thread {
		private LogUpdate logUpdate;

		@Override
		public void run() {
			logUpdate = new LogUpdate();
			try {
				File dir = new File(NativeHelper.app_opt, "bin");
				Process sh = Runtime.getRuntime().exec("/system/bin/sh",
						NativeHelper.envp, dir);
				OutputStream os = sh.getOutputStream();

				StreamThread it = new StreamThread(sh.getInputStream(), logUpdate);
				StreamThread et = new StreamThread(sh.getErrorStream(), logUpdate);

				it.start();
				et.start();

				Log.i(TAG, command);
				writeCommand(os, command);
				writeCommand(os, "exit");

				sh.waitFor();
				Log.i(TAG, "Done!");
			} catch (Exception e) {
				Log.e(TAG, "Error!!!", e);
			} finally {
				synchronized (GnuPrivacyGuard.this) {
					commandThread = null;
				}
				sendBroadcast(new Intent(COMMAND_FINISHED));
			}
		}
	}

	class LogUpdate extends StreamThread.StreamUpdate {

		StringBuffer sb = new StringBuffer();

		@Override
		public void update(String val) {
			NativeHelper.log.append(val);
			sendBroadcast(new Intent(LOG_UPDATE));
		}
	}

	public static void writeCommand(OutputStream os, String command) throws Exception {
		os.write((command + "\n").getBytes("ASCII"));
	}

	private void startGpgAgent() {
		File gpgAgentSocket = new File(NativeHelper.app_home, "S.gpg-agent");
		// gpg-agent is not running, start it
		if (!gpgAgentSocket.exists()) {
			Intent service = new Intent(this, GpgAgentService.class);
			startService(service);
		}
	}

	private void registerReceivers() {
		logUpdateReceiver = new BroadcastReceiver() {
			@Override
			public void onReceive(Context context, Intent intent) {
				updateLog();
			}
		};
		registerReceiver(logUpdateReceiver, new IntentFilter(GnuPrivacyGuard.LOG_UPDATE));

		commandFinishedReceiver = new BroadcastReceiver() {
			@Override
			public void onReceive(Context context, Intent intent) {
			}
		};
		registerReceiver(commandFinishedReceiver, new IntentFilter(
				GnuPrivacyGuard.COMMAND_FINISHED));
	}

	private void unregisterReceivers() {
		if (logUpdateReceiver != null)
			unregisterReceiver(logUpdateReceiver);

		if (commandFinishedReceiver != null)
			unregisterReceiver(commandFinishedReceiver);
	}

    public static String getVersionString(Context context) {
        if (VERSION != null) {
            return VERSION;
        }
        try {
            PackageInfo pi = context.getPackageManager().getPackageInfo(PACKAGE_NAME, 0);
            VERSION = "gpgcli v" + pi.versionName;
            return VERSION;
        } catch (NameNotFoundException e) {
            // unpossible!
            return "v0.0.0";
        }
    }

    public static class ApgId {
        public static final String VERSION = "1";

        public static final String EXTRA_INTENT_VERSION = "intentVersion";

        // must us only lowest 16 bits, otherwise you get (not sure under which conditions exactly)
        // java.lang.IllegalArgumentException: Can only use lower 16 bits for requestCode
        public static final int DECRYPT = 0x00007001;
        public static final int ENCRYPT = 0x00007002;
        public static final int SELECT_PUBLIC_KEYS = 0x00007003;
        public static final int SELECT_SECRET_KEY = 0x00007004;
        public static final int GENERATE_SIGNATURE = 0x00007005;
        public static final int FILENAME = 0x00007006;
    }

    private void setOnClick(Button button, final String intentName, final int intentId) {
		button.setOnClickListener(new OnClickListener() {
            @Override
            public void onClick(View view) {
                Intent intent = new android.content.Intent(intentName);
                intent.putExtra(ApgId.EXTRA_INTENT_VERSION, ApgId.VERSION);
                try {
                    startActivityForResult(intent, intentId);
                    Toast.makeText(view.getContext(),
                            "started " + intentName + " " + intentId,
                            Toast.LENGTH_SHORT).show();
                    Log.i(TAG, "started " + intentName + " " + intentId);
                } catch (ActivityNotFoundException e) {
                    Toast.makeText(view.getContext(),
                                   R.string.error_activity_not_found,
                                   Toast.LENGTH_SHORT).show();
                }
            }
        });
    }

	private void wireTestButtons() {
		Button selectPublicKeysButton = (Button) findViewById(R.id.select_public_keys);
		setOnClick(selectPublicKeysButton, Apg.Intent.SELECT_PUBLIC_KEYS, ApgId.SELECT_PUBLIC_KEYS);

		Button selectSecretKeyButton = (Button) findViewById(R.id.select_secret_key);
		setOnClick(selectSecretKeyButton, Apg.Intent.SELECT_SECRET_KEY, ApgId.SELECT_SECRET_KEY);

		Button encryptButton = (Button) findViewById(R.id.encrypt);
		setOnClick(encryptButton, Apg.Intent.ENCRYPT, ApgId.ENCRYPT);

		Button encryptFileButton = (Button) findViewById(R.id.encrypt_file);
		setOnClick(encryptFileButton, Apg.Intent.ENCRYPT_FILE, ApgId.ENCRYPT);

		Button decryptButton = (Button) findViewById(R.id.decrypt);
		setOnClick(decryptButton, Apg.Intent.DECRYPT, ApgId.DECRYPT);

		Button decryptFileButton = (Button) findViewById(R.id.decrypt_file);
		setOnClick(decryptFileButton, Apg.Intent.DECRYPT_FILE, ApgId.DECRYPT);

		Button generateSignatureButton = (Button) findViewById(R.id.generate_signature);
		setOnClick(generateSignatureButton, Apg.Intent.GENERATE_SIGNATURE, ApgId.GENERATE_SIGNATURE);
	}

	public class InstallAndSetupTask extends AsyncTask<Void, Void, Void> {
		private ProgressDialog dialog;
		private boolean doInstall;

		private final Context context = getApplicationContext();
		private final Handler handler = new Handler() {
			@Override
			public void handleMessage(Message msg) {
				dialog.setMessage(msg.getData().getString("message"));
			}
		};

		private void showProgressMessage(int resId) {
			String messageText = getString(resId);
			if (messageText == null) messageText = "(null)";
			if (dialog == null) {
				Log.e(TAG, "installDialog is null!");
				return;
			}
			dialog.setMessage(messageText);
			if (!dialog.isShowing())
				dialog.show();
		}

		private void hideProgressDialog() {
			dialog.dismiss();
		}

		public InstallAndSetupTask(Context c) {
			dialog = new ProgressDialog(c);
			dialog.setProgressStyle(ProgressDialog.STYLE_SPINNER);
			dialog.setTitle(R.string.dialog_installing_title);
			dialog.setCancelable(false);
		}

		@Override
		protected void onPreExecute() {
			doInstall = NativeHelper.installOrUpgradeAppOpt(context);
			if (doInstall)
				showProgressMessage(R.string.dialog_installing_msg);
		}

		@Override
		protected Void doInBackground(Void... params) {
			if (doInstall)
				NativeHelper.unpackAssets(context, handler);
			return null;
		}

		@Override
		protected void onPostExecute(Void result) {
			hideProgressDialog();

            // these need to be loaded before System.load("gnupg-for-java"); and in
            // the right order, since they have interdependencies.
            System.load(NativeHelper.app_opt + "/lib/libgpg-error.so.0");
            System.load(NativeHelper.app_opt + "/lib/libassuan.so.0");
            System.load(NativeHelper.app_opt + "/lib/libgpgme.so.11");

            Intent intent = new Intent(GnuPrivacyGuard.this, GpgAgentService.class);
            startService(intent);
            intent = new Intent(GnuPrivacyGuard.this, SharedDaemonsService.class);
            startService(intent);
            GnuPG.createContext();
        }
	}

	protected void shareTestLog() {
		Intent i = new Intent(android.content.Intent.ACTION_SEND);
		i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_WHEN_TASK_RESET);
		i.setType("text/plain");
		i.putExtra(Intent.EXTRA_SUBJECT, "test log from " + getString(R.string.app_name));
		i.putExtra(Intent.EXTRA_TEXT,
				"Attached is an log sent by " + getString(R.string.app_name)
						+ ".  For more info, see:\n"
						+ "https://github.com/guardianproject/gnupg-for-android\n\n"
						+ "manufacturer: " + Build.MANUFACTURER + "\n"
						+ "model: " + Build.MODEL + "\n"
						+ "product: " + Build.PRODUCT + "\n"
						+ "brand: " + Build.BRAND + "\n"
						+ "device: " + Build.DEVICE + "\n"
						+ "board: " + Build.BOARD + "\n"
						+ "ID: " + Build.ID + "\n"
						+ "CPU ABI: " + Build.CPU_ABI + "\n"
						+ "release: " + Build.VERSION.RELEASE + "\n"
						+ "incremental: " + Build.VERSION.INCREMENTAL + "\n"
						+ "codename: " + Build.VERSION.CODENAME + "\n"
						+ "SDK: " + Build.VERSION.SDK_INT + "\n"
						+ "\n\nlog:\n----------------------------------\n"
						+ consoleText.getText().toString()
						);
		startActivity(Intent.createChooser(i, "How do you want to share?"));
	}
}
