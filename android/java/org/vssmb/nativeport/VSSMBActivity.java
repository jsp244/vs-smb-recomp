package org.vssmb.nativeport;

import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.Display;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

import org.libsdl.app.SDLActivity;

/**
 * The game runs in native code (libmain.so) on SDL. This activity adds what needs Java: the
 * system document picker for choosing your dump, the display mode, the phone edition's
 * on-screen controls, and the Updates tab's network requests and installation.
 */
public class VSSMBActivity extends SDLActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        preferNesRefreshRate();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        onUpdateStatus(intent);
    }

    /* The Vs. System shows 60 frames a second. Ask for the display mode (at the current
       resolution) with the highest refresh rate that is a whole multiple of 60 Hz, up to 240 Hz:
       120 Hz on a 120 / 144 / 165 Hz panel. Every frame then stays on screen equally long, and a
       faster refresh shortens the compositor's queue. It applies only while this app is in front. */
    private void preferNesRefreshRate() {
        if (Build.VERSION.SDK_INT < 23) return;
        try {
            Display d = getWindowManager().getDefaultDisplay();
            Display.Mode cur = d.getMode();
            Display.Mode best = null;
            for (Display.Mode m : d.getSupportedModes()) {
                if (m.getPhysicalWidth() != cur.getPhysicalWidth() || m.getPhysicalHeight() != cur.getPhysicalHeight()) continue;
                float r = m.getRefreshRate();
                int k = Math.round(r / 60.0f);
                if (k < 1 || k > 4 || Math.abs(r - 60.0f * k) > 1.5f * k) continue;
                if (best == null || r > best.getRefreshRate()) best = m;
            }
            if (best == null) return;
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(lp);
        } catch (RuntimeException e) {
            /* keep the system's choice */
        }
    }

    /* The phone edition (manifest meta-data org.vssmb.touch) gets the on-screen controls.
       Command-line options for tests and diagnostics (for example --stats and an absolute file
       path, one per line) can be put in Android/data/<package>/files/args.txt, a folder only adb
       and the app itself can write to. */
    @Override
    protected String[] getArguments() {
        ArrayList<String> args = new ArrayList<>();
        if (touchEdition()) args.add("--touch");
        File dir = getExternalFilesDir(null);
        File f = dir != null ? new File(dir, "args.txt") : null;
        if (f == null || !f.isFile()) return args.toArray(new String[0]);
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (!line.isEmpty()) args.add(line);
            }
        } catch (IOException e) {
            /* the options read so far */
        }
        return args.toArray(new String[0]);
    }

    private boolean touchEdition() {
        try {
            ApplicationInfo ai = getPackageManager().getApplicationInfo(getPackageName(), PackageManager.GET_META_DATA);
            return ai.metaData != null && ai.metaData.getBoolean("org.vssmb.touch", false);
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }
    private static final int PICK_ROM = 0x5754;

    /** Called from native code: show the system document picker. */
    public void pickRom() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                try {
                    startActivityForResult(i, PICK_ROM);
                } catch (Exception e) {
                    // no document picker on this device: the dump can be put in the app's folder instead
                }
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ROM || resultCode != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        new Thread(new Runnable() {
            @Override
            public void run() {
                String path = copyToAppStorage(uri);
                // the native side treats it like a file dropped on the window: check its hash and load it
                if (path != null) SDLActivity.onNativeDropFile(path);
            }
        }, "rom-copy").start();
    }

    /** Copies the chosen document (.nes, or MAME's .zip) into the app's own storage, keeping its
        name (saves are named after it). */
    private String copyToAppStorage(Uri uri) {
        String name = "VS Super Mario Bros.nes";
        Cursor c = null;
        try {
            c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null);
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null && n.length() > 0) name = n.replace('/', '_').replace('\\', '_');
            }
        } catch (Exception e) {
            // keep the default name
        } finally {
            if (c != null) c.close();
        }
        File dir = new File(getFilesDir(), "roms");
        dir.mkdirs();
        File out = new File(dir, name);
        InputStream in = null;
        OutputStream o = null;
        try {
            in = getContentResolver().openInputStream(uri);
            if (in == null) return null;
            o = new FileOutputStream(out);
            byte[] buf = new byte[65536];
            long total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                total += n;
                if (total > (16 << 20)) throw new IOException("larger than any dump of this board");
                o.write(buf, 0, n);
            }
            return out.getAbsolutePath();
        } catch (Exception e) {
            out.delete();
            return null;
        } finally {
            try { if (in != null) in.close(); } catch (IOException e) { /* ignore */ }
            try { if (o != null) o.close(); } catch (IOException e) { /* ignore */ }
        }
    }

    /* ---- the Updates tab (frontend/updater.c) ---- */

    private static final String UPDATE_STATUS = "org.vssmb.nativeport.UPDATE_STATUS";

    /** In native code: what became of an installation (a PackageInstaller status and its message). */
    private static native void nativeUpdateStatus(int status, String message);

    /** Called from native code (frontend/http.c) on a worker thread: an https GET. Returns the
        status as 4 big-endian bytes, then the body; status 0 and an error message when no answer
        came. Redirects are followed only to https addresses (HttpURLConnection never follows one
        to another protocol) and the server's certificate is checked against the system's. */
    public static byte[] httpGet(String url, String accept, String userAgent, int timeoutMs, int maxLen) {
        HttpURLConnection c = null;
        try {
            if (!url.startsWith("https://")) throw new IOException("not an https address");
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setInstanceFollowRedirects(true);
            c.setRequestProperty("User-Agent", userAgent);
            if (accept.length() > 0) c.setRequestProperty("Accept", accept);
            int status = c.getResponseCode();
            if (!"https".equals(c.getURL().getProtocol())) throw new IOException("redirected to an address that is not https");
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            writeInt(out, status);
            InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            if (in != null) {
                byte[] buf = new byte[65536];
                long total = 0;
                int n;
                while ((n = in.read(buf)) > 0) {
                    total += n;
                    if (total > maxLen) throw new IOException("the answer is larger than expected");
                    out.write(buf, 0, n);
                }
                in.close();
            }
            return out.toByteArray();
        } catch (Exception e) {
            ByteArrayOutputStream err = new ByteArrayOutputStream();
            writeInt(err, 0);
            byte[] msg = (e.getMessage() != null ? e.getMessage() : e.toString()).getBytes(StandardCharsets.UTF_8);
            err.write(msg, 0, msg.length);
            return err.toByteArray();
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static void writeInt(ByteArrayOutputStream o, int v) {
        o.write(v >>> 24);
        o.write(v >>> 16);
        o.write(v >>> 8);
        o.write(v);
    }

    /** Called from native code (frontend/updater.c) on a worker thread with a downloaded and
        verified update of this app: hands it to the system's package installer, which asks the
        player to confirm, closes the game and installs it (what happens comes back through
        onNewIntent). Returns "" once that has begun, "permission" when the player must first
        allow this app to install updates (the settings page for that is opened), else what went
        wrong. */
    public static String installUpdate(String path) {
        final SDLActivity a = mSingleton;
        if (a == null) return "the app is not running";
        PackageInstaller.Session session = null;
        try {
            PackageManager pm = a.getPackageManager();
            PackageInfo pi = pm.getPackageArchiveInfo(path, 0);
            if (pi == null || !a.getPackageName().equals(pi.packageName)) return "the download is not an update of this app";
            if (Build.VERSION.SDK_INT >= 26 && !pm.canRequestPackageInstalls()) {
                a.runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + a.getPackageName())));
                        } catch (Exception e) {
                            // the player finds it in the system settings (Apps > Special app access)
                        }
                    }
                });
                return "permission";
            }
            File f = new File(path);
            PackageInstaller installer = pm.getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(a.getPackageName());
            params.setSize(f.length());
            session = installer.openSession(installer.createSession(params));
            InputStream in = new FileInputStream(f);
            OutputStream o = session.openWrite("base.apk", 0, f.length());
            try {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                session.fsync(o);
            } finally {
                o.close();
                in.close();
            }
            // the installer reports to this activity (onNewIntent): first that it needs the player's
            // confirmation, then how the installation ended
            Intent status = new Intent(a, VSSMBActivity.class).setAction(UPDATE_STATUS);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            session.commit(PendingIntent.getActivity(a, 0, status, flags).getIntentSender());
            session.close();
            session = null;
            return "";
        } catch (Exception e) {
            if (session != null) {
                session.abandon();
                session.close();
            }
            return e.getMessage() != null ? e.getMessage() : e.toString();
        }
    }

    /** The package installer's reports (see installUpdate). */
    private void onUpdateStatus(Intent intent) {
        if (intent == null || !UPDATE_STATUS.equals(intent.getAction())) return;
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            try {
                if (confirm != null) startActivity(confirm);
            } catch (Exception e) {
                nativeUpdateStatus(PackageInstaller.STATUS_FAILURE, "the confirmation could not be shown");
            }
            return;
        }
        String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
        nativeUpdateStatus(status, message != null ? message : "");
    }
}
