// SPDX-License-Identifier: GPL-3.0-only

package app.lunacore.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.net.Uri;
import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.telephony.TelephonyManager;
import android.os.Bundle;
import android.os.Build;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ImageView;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    // The sign-in screen in the library's night colours, as on the Mac: white
    // text, lavender details, violet accents on frosted cards.
    private static final int INK = 0xfffaf8ff;
    private static final int MUTED = 0xffc7bdd4;
    private static final int ACCENT = 0xffaa88ff;
    private static final int CARD = 0x1fffffff;
    private static final int CARD_EDGE = 0x40aa88ff;
    private static final int BADGE = 0xff6a4cff;
    private static final ExecutorService ENGINE = Executors.newSingleThreadExecutor();

    static { System.loadLibrary("lunaone_android"); }

    private static native void nativeConfigure(String path);
    private static native String[] nativeMarkets();
    private static native boolean nativeHasRegistration();
    private static native String[] nativeRestore();
    private static native String[] nativeConnect(String market, Job job);
    private static native String[] nativeSearch(String query, Job job);
    private static native String[] nativeCatalog(Job job, int section, boolean explore);
    private static native void nativeSignOut();
    private static native void nativePlay(int index, Surface surface, Job job, boolean canChooseDecoder);
    private static native void nativeInput(int buttons, int left, int right, int lx, int ly, int rx, int ry);

    private static native String nativeText(String key);
    private static native byte[] nativeQr(String url);
    private static native String[] nativePlaybackChoices(boolean canChooseDecoder);
    private static native boolean nativeSelectPlaybackChoice(int row, int choice, boolean canChooseDecoder);
    private static native boolean nativeSimpleLibrary();
    private static native String nativePreferredMarket();
    private static native String nativeCheckUpdates(String installed);
    private static native String nativeUpdateVersion();
    private static native String nativeDownloadUpdate(String path);
    private static native void nativeSetDisplay(int height, int refresh);
    private static native boolean nativeVirtualKeyboard();
    private static native String[] nativeShortcutNotes(int vendor);
    private static native int nativeControllerOptions();
    private static native void nativeLog(String event, String detail);
    private static native String[] nativeExploredCatalog(int section);

    // The activity on screen, for the native exploration thread.
    private static volatile MainActivity current;
    private long lastExplored;
    // A refresh is scheduled; games were found that the screen does not show yet.
    private boolean exploredPending, exploredWaiting;

    // Called by the native exploration thread when Luna's linked pages added games.
    private static void catalogGrown() {
        MainActivity activity = current;
        if (activity != null) activity.runOnUiThread(() -> {
            activity.exploredWaiting = true;
            activity.scheduleExplored();
        });
    }

    // « Tous les jeux » grows at most every 8 s, so that focus does not jump while browsing.
    private void scheduleExplored() {
        if (exploredPending || destroyed) return;
        exploredPending = true;
        long wait = Math.max(0, lastExplored + 8000 - System.currentTimeMillis());
        getWindow().getDecorView().postDelayed(this::refreshExplored, wait);
    }

    private boolean showingAllGames() {
        return screen == Screen.CATALOG && active == null && section == ALL_GAMES && searchText.isEmpty() &&
            settingsScreen == null && catalog != null;
    }

    private void refreshExplored() {
        exploredPending = false;
        lastExplored = System.currentTimeMillis();
        // Found during a game or another section: shown on the next return to « Tous les jeux ».
        if (!showingAllGames()) return;
        exploredWaiting = false;
        ENGINE.execute(() -> {
            String[] items;
            try { items = nativeExploredCatalog(ALL_GAMES); }
            catch (RuntimeException failure) { items = null; }
            String[] result = items;
            runOnUiThread(() -> {
                if (result == null || !showingAllGames()) return;
                String title = selectedGame < catalog.games.size() ? catalog.games.get(selectedGame).title() : "";
                LibraryScreen.Content grown = new LibraryScreen.Content(result);
                if (grown.games.size() <= catalog.games.size()) return;
                catalog = grown;
                for (int i = 0; i < catalog.games.size(); i++)
                    if (catalog.games.get(i).title().equals(title)) selectedGame = i;
                showCatalog("");
            });
        });
    }

    private static final int ALL_GAMES = 2;

    // Called by JNI for explicit decoder choices on Android 10 and later.
    private static String findAvcDecoder(int preference, int width, int height) {
        if (Build.VERSION.SDK_INT < 29) return null;
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (info.isEncoder() || info.isAlias()) continue;
            if (preference == 1 ? !info.isHardwareAccelerated() : !info.isSoftwareOnly()) continue;
            try {
                MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType("video/avc");
                if (caps.getVideoCapabilities().isSizeSupported(width, height)) return info.getName();
            } catch (IllegalArgumentException ignored) {
                // This decoder does not advertise AVC.
            }
        }
        return null;
    }

    private enum Screen { LOGIN, CATALOG, PLAYER }
    private interface Operation { String[] run(Job job); }

    private final class Job {
        volatile boolean cancelled;
        private final Screen screen;

        Job(Screen screen) { this.screen = screen; }

        public void onStatus(String message) {
            runOnUiThread(() -> {
                if (active != this || cancelled || destroyed) return;
                if (screen == Screen.LOGIN && codePanel != null) showCodePanel(false);
                setStatus(message);
            });
        }

        public void onMetrics(String rates) {
            runOnUiThread(() -> {
                if (active != this || cancelled || destroyed ||
                    (loadingScreen != null && loadingScreen.getVisibility() == View.VISIBLE)) return;
                hud.setText(rates.isEmpty() ? "" : rates + connectionAndBattery());
                hud.setVisibility(rates.isEmpty() ? View.GONE : View.VISIBLE);
            });
        }

        public void onCode(String code, String url) {
            runOnUiThread(() -> {
                if (active != this || cancelled || destroyed) return;
                deviceCode = code;
                verificationUrl = url;
                codeValue.setText(code);
                showQr(url);
                if (verificationAddress != null) verificationAddress.setText(url);
                showCodePanel(true);
                setStatus("Confirme ce code sur Amazon, puis reviens dans LunaONE.");
            });
        }
    }

    private final ArrayList<View> actions = new ArrayList<>();
    private Screen screen = Screen.LOGIN;
    private Job active;
    private boolean destroyed;
    private WifiManager.WifiLock wifiLock;
    private boolean surfaceStarted;
    private FrameLayout loadingScreen;
    // « Annuler le lancement du jeu » and its Oui / Non confirmation.
    private Button launchCancel;
    private FrameLayout launchConfirm;
    private final View[] loadingSteps = new View[3];
    private android.animation.ValueAnimator loadingPulse;
    private int loadingStage;
    private boolean leftTriggerKey, rightTriggerKey;
    // L1 + L2 + R1 + R2 in the catalog: shoulder keys and trigger axes.
    private boolean chordL1, chordR1, chordL2, chordR2, chordHeld;
    private int controllerVendor;
    // Bit 1: A and B swapped, also in LunaONE's own screens.
    private int controllerOptions = -1;
    private SettingsScreen settingsScreen;
    private boolean savedDevice;
    private boolean tv;
    private String[] markets = new String[0];
    private LibraryScreen.Content catalog;
    private LibraryScreen library;
    private String deviceCode = "", verificationUrl = "", searchText = "";
    private int marketIndex;
    private int selectedGame;
    private int section = ALL_GAMES;
    private int gamepadId = -1, buttons, hatButtons, left, right, lx, ly, rx, ry;
    private TextView status, codeValue, verificationAddress, hud;
    private ImageView codeQr;
    private LinearLayout body, codePanel, stepsPanel;
    private Button cancel;
    private View preferredFocus;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        current = this;
        tv = TvUi.enabled(this);
        if (tv) setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        try {
            nativeConfigure(new File(getFilesDir(), "engine").getAbsolutePath());
            reportDisplay();
            markets = nativeMarkets();
            for (int i = 0; i < markets.length; i += 2)
                if (markets[i].equals("fr")) marketIndex = i / 2;
            savedDevice = nativeHasRegistration();
            showLogin("");
            if (savedDevice) begin("Chargement du catalogue Luna…", job -> nativeRestore());
        } catch (RuntimeException error) {
            showLogin(error.getMessage());
        }
    }

    @Override protected void onPause() {
        if (screen == Screen.PLAYER) stopPlayer();
        super.onPause();
    }

    @Override protected void onDestroy() {
        destroyed = true;
        if (current == this) current = null;
        holdWifi(false);
        stopLoadingPulse();
        if (active != null) active.cancelled = true;
        super.onDestroy();
    }

    @Override public void onBackPressed() {
        if (screen == Screen.PLAYER) stopPlayer();
        else if (active != null) cancelJob();
        else if (screen == Screen.CATALOG && !searchText.isEmpty()) openCatalog(section, false);
        else super.onBackPressed();
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }

    private String t(String french) { return nativeText(french); }

    private TextView text(String label, int size, int color) {
        TextView view = new TextView(this);
        view.setText(label);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    private Button button(String label, boolean primary, Runnable action) {
        Button view = new Button(this);
        view.setText(t(label));
        view.setAllCaps(false);
        if (tv) TvUi.focusable(view);
        TvUi.button(view, primary, getResources().getDisplayMetrics().density);
        view.setTextSize(tv ? 17 : 15);
        view.setOnClickListener(v -> action.run());
        return view;
    }

    private Button action(String label, boolean primary, Runnable operation) {
        Button view = button(label, primary, operation);
        actions.add(view);
        return view;
    }

    private void base(String subtitle) {
        actions.clear();
        codePanel = null;
        stepsPanel = null;
        verificationAddress = null;
        preferredFocus = null;
        body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setBackground(backdrop());
        if (tv) {
            // A 960 × 540 dp page, scaled to fit inside the TV's safe area:
            // it never leaves the picture, even on a TV that crops its edges.
            body.setPadding(dp(28), dp(22), dp(28), dp(18));
            FitFrame frame = new FitFrame(this, dp(960), dp(540));
            frame.setBackgroundColor(0xff0c0820);
            frame.addView(body);
            setContentView(frame);
        } else {
            body.setOnApplyWindowInsetsListener((view, insets) -> {
                int horizontal = dp(20);
                view.setPadding(horizontal + insets.getSystemWindowInsetLeft(), dp(16) + insets.getSystemWindowInsetTop(),
                                horizontal + insets.getSystemWindowInsetRight(), dp(10) + insets.getSystemWindowInsetBottom());
                return insets;
            });
            // Status/navigation bar insets are routed into catalog margins on Android 15+.
            // The page scrolls when it is taller than the screen: a small phone,
            // a phone on its side, or a car screen.
            ScrollView scroll = new ScrollView(this);
            scroll.setFillViewport(true);
            scroll.setBackgroundColor(0xff0c0820);
            scroll.addView(body, new FrameLayout.LayoutParams(-1, -2));
            setContentView(scroll);
            body.requestApplyInsets();
        }
        // Logo, name and subtitle, then the line that says what LunaONE is.
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.luna_icon);
        logo.setBackground(TvUi.shape(CARD, dp(14), 0, 0));
        logo.setClipToOutline(true);
        int logoSize = dp(tv ? 60 : 52);
        LinearLayout.LayoutParams logoLayout = new LinearLayout.LayoutParams(logoSize, logoSize);
        logoLayout.rightMargin = dp(16);
        header.addView(logo, logoLayout);
        LinearLayout names = new LinearLayout(this);
        names.setOrientation(LinearLayout.VERTICAL);
        TextView title = text("LunaONE", tv ? 36 : 30, INK);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        names.addView(title);
        names.addView(text(t(subtitle), tv ? 17 : 15, ACCENT));
        header.addView(names);
        body.addView(header);
        TextView tagline = text(t("Tes jeux Amazon Luna, sans navigateur, sur tous tes écrans."), tv ? 16 : 14, MUTED);
        LinearLayout.LayoutParams taglineLayout = new LinearLayout.LayoutParams(-1, -2);
        taglineLayout.topMargin = dp(10);
        taglineLayout.bottomMargin = dp(tv ? 16 : 14);
        body.addView(tagline, taglineLayout);
    }

    // The logo's violet deepening to night blue from the top left, with two soft glows.
    private Drawable backdrop() {
        GradientDrawable base = new GradientDrawable(GradientDrawable.Orientation.TL_BR,
            new int[]{0xff3a1f9a, 0xff1b1150, 0xff0c0820});
        float width = tv ? dp(960) : getResources().getDisplayMetrics().widthPixels;
        GradientDrawable glow = new GradientDrawable();
        glow.setGradientType(GradientDrawable.RADIAL_GRADIENT);
        glow.setColors(new int[]{0x707c4dff, 0x007c4dff});
        glow.setGradientCenter(0.82f, 0.18f);
        glow.setGradientRadius(width * 0.45f);
        GradientDrawable low = new GradientDrawable();
        low.setGradientType(GradientDrawable.RADIAL_GRADIENT);
        low.setColors(new int[]{0x40b06bff, 0x00b06bff});
        low.setGradientCenter(0.08f, 0.92f);
        low.setGradientRadius(width * 0.4f);
        return new LayerDrawable(new Drawable[]{base, glow, low});
    }

    private GradientDrawable card() {
        return TvUi.shape(CARD, dp(18), Math.max(1, dp(1)), CARD_EDGE);
    }

    // The steps panel stands where the code appears once there is one.
    private void showCodePanel(boolean visible) {
        if (codePanel != null) codePanel.setVisibility(visible ? View.VISIBLE : View.GONE);
        if (stepsPanel != null) stepsPanel.setVisibility(visible ? View.GONE : View.VISIBLE);
        if (visible && !tv && codePanel != null) {
            // On a phone, scroll the new code into view.
            LinearLayout panel = codePanel;
            panel.post(() -> panel.requestRectangleOnScreen(new Rect(0, 0, panel.getWidth(), panel.getHeight())));
        }
    }

    private void footer(String message) {
        status = text(t(message), tv ? 17 : 14, MUTED);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        status.setPadding(0, dp(8), 0, 0);
        body.addView(status);
        cancel = button("Annuler", false, this::cancelJob);
        cancel.setVisibility(View.GONE);
        body.addView(cancel, new LinearLayout.LayoutParams(tv ? -2 : -1, -2));
    }

    private void showLogin(String message) {
        screen = Screen.LOGIN;
        base("Connexion Amazon");
        // The form, on a frosted card: on the left on a TV, first on a phone.
        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        form.setBackground(card());
        form.setPadding(dp(tv ? 22 : 16), dp(tv ? 16 : 12), dp(tv ? 22 : 16), dp(tv ? 18 : 14));
        LinearLayout panels = null;
        if (tv) {
            panels = new LinearLayout(this);
            body.addView(panels, new LinearLayout.LayoutParams(-1, 0, 1));
            LinearLayout column = new LinearLayout(this);
            column.setOrientation(LinearLayout.VERTICAL);
            column.setPadding(0, 0, dp(28), 0);
            column.addView(form, new LinearLayout.LayoutParams(-1, -2));
            panels.addView(column, new LinearLayout.LayoutParams(0, -1, 1));
        } else {
            body.addView(form, new LinearLayout.LayoutParams(-1, -2));
        }
        if (savedDevice) {
            preferredFocus = action("Réessayer le chargement", true, () -> begin("Chargement du catalogue Luna…",
                                                                                 job -> nativeRestore()));
            spaced(form, preferredFocus);
            spaced(form, action("Se déconnecter", false, this::confirmSignOut));
        } else if (markets.length > 0) {
            form.addView(text(t("Pays du compte"), tv ? 16 : 14, MUTED));
            Spinner spinner = new Spinner(this);
            String[] labels = new String[markets.length / 2];
            for (int i = 0; i < labels.length; i++) labels[i] = markets[2 * i + 1];
            // White text on the dark card; the open list keeps the system colours.
            ArrayAdapter<String> regions = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels) {
                @Override public View getView(int position, View convertView, ViewGroup parent) {
                    View view = super.getView(position, convertView, parent);
                    if (view instanceof TextView label) {
                        label.setTextColor(INK);
                        label.setTextSize(tv ? 18 : 16);
                    }
                    return view;
                }
            };
            regions.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            spinner.setAdapter(regions);
            String preferredMarket = nativePreferredMarket();
            for (int i = 0; i < labels.length; ++i)
                if (markets[2 * i].equals(preferredMarket)) marketIndex = i;
            spinner.setSelection(marketIndex);
            actions.add(spinner);
            spinner.setBackgroundTintList(ColorStateList.valueOf(MUTED));
            if (tv) {
                TvUi.focusable(spinner);
                spinner.setOnFocusChangeListener((view, focused) -> view.setBackgroundTintList(
                    ColorStateList.valueOf(focused ? ACCENT : MUTED)));
            }
            form.addView(spinner, new LinearLayout.LayoutParams(-1, dp(tv ? 58 : 48)));
            preferredFocus = action("Créer un code", true, () -> {
                marketIndex = spinner.getSelectedItemPosition();
                begin("Création du code Amazon…", job -> nativeConnect(markets[2 * marketIndex], job));
            });
            spaced(form, preferredFocus);
        }
        spaced(form, action("Réglages", false, this::videoSettings));

        // The code: the QR on a white plate, which phone cameras read best,
        // then the address and the code itself.
        codePanel = new LinearLayout(this);
        codePanel.setOrientation(LinearLayout.VERTICAL);
        codePanel.setBackground(card());
        codePanel.setPadding(dp(tv ? 22 : 16), dp(tv ? 18 : 12), dp(tv ? 22 : 16), dp(tv ? 18 : 12));
        codeValue = text("", tv ? 40 : 30, INK);
        codeValue.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        codeValue.setGravity(Gravity.CENTER_HORIZONTAL);
        codeQr = new ImageView(this);
        codeQr.setContentDescription("QR");
        codeQr.setScaleType(ImageView.ScaleType.FIT_CENTER);
        FrameLayout plate = new FrameLayout(this);
        plate.setBackground(TvUi.shape(Color.WHITE, dp(14), 0, 0));
        plate.setPadding(dp(10), dp(10), dp(10), dp(10));
        int qrSize = dp(tv ? 150 : 125);
        plate.addView(codeQr, new FrameLayout.LayoutParams(qrSize, qrSize));
        LinearLayout.LayoutParams plateLayout = new LinearLayout.LayoutParams(-2, -2);
        plateLayout.gravity = Gravity.CENTER_HORIZONTAL;
        plateLayout.topMargin = dp(tv ? 12 : 4);
        if (tv) {
            // The QR beside the address and the code, so the card stays low
            // enough for the Cancel button under it.
            TextView heading = text(t("Scanne le QR code et saisis le code Amazon."), 17, INK);
            heading.setTypeface(Typeface.DEFAULT_BOLD);
            codePanel.addView(heading, new LinearLayout.LayoutParams(-1, -2));
            LinearLayout row = new LinearLayout(this);
            row.setGravity(Gravity.CENTER_VERTICAL);
            plateLayout.gravity = Gravity.CENTER_VERTICAL;
            row.addView(plate, plateLayout);
            LinearLayout where = new LinearLayout(this);
            where.setOrientation(LinearLayout.VERTICAL);
            verificationAddress = text("", 17, ACCENT);
            where.addView(verificationAddress, new LinearLayout.LayoutParams(-1, -2));
            codeValue.setGravity(Gravity.START);
            LinearLayout.LayoutParams codeLayout = new LinearLayout.LayoutParams(-1, -2);
            codeLayout.topMargin = dp(8);
            where.addView(codeValue, codeLayout);
            LinearLayout.LayoutParams whereLayout = new LinearLayout.LayoutParams(0, -2, 1);
            whereLayout.leftMargin = dp(20);
            whereLayout.topMargin = dp(12);
            row.addView(where, whereLayout);
            codePanel.addView(row, new LinearLayout.LayoutParams(-1, -2));
            panels.addView(codePanel, new LinearLayout.LayoutParams(0, -2, 1.2f));
            // The steps explain a new sign-in, not a saved account that failed to load.
            if (!savedDevice) {
                stepsPanel = steps();
                panels.addView(stepsPanel, new LinearLayout.LayoutParams(0, -2, 1.2f));
            }
        } else {
            codeValue.setTextIsSelectable(true);
            codePanel.addView(plate, plateLayout);
            codePanel.addView(codeValue, new LinearLayout.LayoutParams(-1, -2));
            spaced(codePanel, button("Copier le code", false, () -> {
                ClipboardManager clipboard = getSystemService(ClipboardManager.class);
                clipboard.setPrimaryClip(ClipData.newPlainText("Code Amazon", deviceCode));
            }));
            spaced(codePanel, button("Ouvrir Amazon", true, () -> {
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(verificationUrl)));
                } catch (ActivityNotFoundException error) {
                    setStatus("Ouvre cette page sur un autre appareil. " + verificationUrl);
                }
            }));
            LinearLayout.LayoutParams panelLayout = new LinearLayout.LayoutParams(-1, -2);
            panelLayout.topMargin = dp(12);
            body.addView(codePanel, panelLayout);
        }
        showCodePanel(false);
        footer(message);
        refreshActions();
    }

    // Adds a control under the previous one, with a gap.
    private void spaced(LinearLayout parent, View view) {
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(-1, -2);
        if (parent.getChildCount() > 0) layout.topMargin = dp(10);
        parent.addView(view, layout);
    }

    // On a TV, before a code: the three steps of the sign-in, as on the Mac.
    private LinearLayout steps() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(card());
        panel.setPadding(dp(24), dp(20), dp(24), dp(20));
        TextView heading = text(t("Comment se connecter"), 20, INK);
        heading.setTypeface(Typeface.DEFAULT_BOLD);
        panel.addView(heading);
        String[] lines = {"Choisis le pays de ton compte Amazon, puis crée un code.",
                          "Scanne le QR code avec ton téléphone ou ouvre la page Amazon.",
                          "Saisis le code et confirme : ta bibliothèque Luna s’ouvre ici."};
        for (int i = 0; i < lines.length; i++) {
            LinearLayout row = new LinearLayout(this);
            row.setGravity(Gravity.CENTER_VERTICAL);
            TextView badge = text(String.valueOf(i + 1), 17, INK);
            badge.setTypeface(Typeface.DEFAULT_BOLD);
            badge.setGravity(Gravity.CENTER);
            badge.setBackground(TvUi.shape(BADGE, dp(18), 0, 0));
            row.addView(badge, new LinearLayout.LayoutParams(dp(36), dp(36)));
            LinearLayout.LayoutParams lineLayout = new LinearLayout.LayoutParams(0, -2, 1);
            lineLayout.leftMargin = dp(14);
            row.addView(text(t(lines[i]), 15, MUTED), lineLayout);
            LinearLayout.LayoutParams rowLayout = new LinearLayout.LayoutParams(-1, -2);
            rowLayout.topMargin = dp(16);
            panel.addView(row, rowLayout);
        }
        TextView note = text(t("Aucun mot de passe n’est saisi dans LunaONE."), 14, ACCENT);
        LinearLayout.LayoutParams noteLayout = new LinearLayout.LayoutParams(-1, -2);
        noteLayout.topMargin = dp(18);
        panel.addView(note, noteLayout);
        return panel;
    }

    private void showCatalog(String message) {
        screen = Screen.CATALOG;
        codePanel = null;
        actions.clear();
        library = new LibraryScreen(this, catalog, selectedGame, section, searchText, tv, nativeSimpleLibrary(),
            this::t, this::connectionAndBattery, new LibraryScreen.Actions() {
                @Override public void selection(int index) { selectedGame = index; }
                @Override public void play(int index) { startPlayer(index); }
                @Override public void section(int index) { openCatalog(index, false); }
                @Override public void search() { showSearch(); }
                @Override public void account() { showAccount(); }
                @Override public void settings() { videoSettings(); }
                @Override public void cancel() { cancelJob(); }
            });
        library.setStatus(t(message));
        setContentView(library);
        library.requestApplyInsets();
        refreshActions();
        if (exploredWaiting) scheduleExplored();
    }

    private void showAccount() {
        if (active != null) return;
        String detail = catalog.name;
        if (!catalog.plan.isEmpty()) detail += (detail.isEmpty() ? "" : "\n") + catalog.plan;
        new AlertDialog.Builder(this).setTitle(t("Compte")).setMessage(detail)
            .setNegativeButton(t("Annuler"), null)
            .setPositiveButton(t("Se déconnecter"), (dialog, choice) -> confirmSignOut()).show();
    }

    private void showSearch() {
        if (active != null) return;
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setImeOptions(EditorInfo.IME_ACTION_SEARCH);
        input.setHint(t("Titre du jeu"));
        input.setText(searchText);
        input.setSelectAllOnFocus(true);
        FrameLayout holder = new FrameLayout(this);
        holder.setPadding(dp(24), 0, dp(24), 0);
        holder.addView(input, new FrameLayout.LayoutParams(-1, -2));
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(t("Rechercher un jeu"))
            .setView(holder).setNegativeButton(t("Annuler"), null)
            .setPositiveButton(t("Rechercher"), null).create();
        Runnable submit = () -> {
            String query = input.getText().toString().trim();
            if (query.isEmpty()) {
                input.setError(t("Saisis le titre d’un jeu."));
                return;
            }
            getSystemService(InputMethodManager.class).hideSoftInputFromWindow(input.getWindowToken(), 0);
            dialog.dismiss();
            begin("Recherche dans le catalogue Luna…", job -> nativeSearch(query, job), () -> searchText = query);
        };
        input.setOnEditorActionListener((view, id, event) -> {
            if (id != EditorInfo.IME_ACTION_SEARCH) return false;
            submit.run();
            return true;
        });
        dialog.setOnShowListener(ignored -> {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> submit.run());
            input.requestFocus();
        });
        dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        dialog.show();
    }

    private void showQr(String url) {
        byte[] cells = nativeQr(url);
        if (cells == null || cells.length == 0) return;
        int side = (int) Math.sqrt(cells.length);
        int scale = 5;
        int[] pixels = new int[side * side * scale * scale];
        for (int y = 0; y < side * scale; y++)
            for (int x = 0; x < side * scale; x++)
                pixels[y * side * scale + x] = cells[y / scale * side + x / scale] == 0 ? Color.WHITE : Color.BLACK;
        codeQr.setImageBitmap(Bitmap.createBitmap(pixels, side * scale, side * scale, Bitmap.Config.ARGB_8888));
    }

    private record Setting(String title, int selected, String[] values) {}

    // The screen's physical size and refresh rate, for the automatic video mode.
    private void reportDisplay() {
        try {
            android.view.Display.Mode mode = getWindowManager().getDefaultDisplay().getMode();
            nativeSetDisplay(mode.getPhysicalHeight(), Math.round(mode.getRefreshRate()));
        } catch (RuntimeException unavailable) {
            // The automatic mode then assumes a 1080p, 60 Hz screen.
        }
    }

    // The settings screen shared by every LunaONE client, over the current screen.
    private void videoSettings() {
        if (settingsScreen != null && settingsScreen.getParent() != null) return;
        settingsScreen = newSettingsScreen();
        addContentView(settingsScreen, new ViewGroup.LayoutParams(-1, -1));
        settingsScreen.requestFocus();
    }

    private SettingsScreen newSettingsScreen() {
        return new SettingsScreen(this, tv, this::t, new SettingsScreen.Source() {
            @Override public String[] choices() { return nativePlaybackChoices(Build.VERSION.SDK_INT >= 29); }
            @Override public String[] notes() { return nativeShortcutNotes(controllerVendor); }
            @Override public String[] account() {
                return catalog == null ? new String[0] : new String[]{catalog.name, catalog.plan};
            }
            @Override public String[] about() {
                String version = "";
                try { version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
                catch (android.content.pm.PackageManager.NameNotFoundException ignored) {}
                return new String[]{"LunaONE  V" + version, t("Crédit") + "  KazaR-RooT",
                    t("Projet non affilié à Amazon ni à Amazon Luna.")};
            }
            @Override public boolean signedIn() { return savedDevice; }
        }, new SettingsScreen.Actions() {
            @Override public void apply(int choice, int value) {
                try {
                    if (!nativeSelectPlaybackChoice(choice, value, Build.VERSION.SDK_INT >= 29))
                        setStatus("Impossible d’enregistrer les réglages vidéo.");
                } catch (RuntimeException failure) {
                    setStatus(failure.getMessage());
                }
                controllerOptions = -1;
            }
            @Override public void checkUpdates() { checkForUpdates(); }
            @Override public void signOut() {
                closeSettings();
                confirmSignOut();
            }
            @Override public void close() { closeSettings(); }
        });
    }

    private void closeSettings() {
        if (settingsScreen == null) return;
        if (settingsScreen.getParent() instanceof ViewGroup parent) parent.removeView(settingsScreen);
        settingsScreen = null;
        // Language and library view may have changed.
        if (screen == Screen.CATALOG && active == null) showCatalog("");
        else if (screen == Screen.LOGIN && active == null) showLogin("");
    }

    private void checkForUpdates() {
        AlertDialog progress = new AlertDialog.Builder(this).setTitle(t("Vérifier les mises à jour"))
            .setMessage(t("Vérification des mises à jour…")).setNegativeButton(t("Annuler"), null).show();
        new Thread(() -> {
            String result;
            try {
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                result = nativeCheckUpdates(version);
            } catch (Exception failure) {
                result = t("Impossible de vérifier les mises à jour sur GitHub.");
            }
            String message = result;
            String update = nativeUpdateVersion();
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed() || !progress.isShowing()) return;
                progress.dismiss();
                AlertDialog.Builder dialog = new AlertDialog.Builder(this).setTitle(t("Vérifier les mises à jour"))
                    .setMessage(message).setNegativeButton("OK", null);
                if (!update.isEmpty())
                    dialog.setPositiveButton(t("Installer la version {}").replace("{}", update), (shown, which) -> installUpdate());
                dialog.show();
            });
        }, "lunaone-updates").start();
    }

    // Downloads the APK checked against the release's SHA-256 list, then opens
    // Android's installer on it. Android asks to confirm, and to allow LunaONE
    // to install apps the first time; its last screen offers to open LunaONE.
    private void installUpdate() {
        AlertDialog progress = new AlertDialog.Builder(this).setTitle(t("Mise à jour de LunaONE"))
            .setMessage(t("Téléchargement de la mise à jour…") + "\n" + t("Android va te demander de confirmer la mise à jour."))
            .setCancelable(false).show();
        new Thread(() -> {
            java.io.File apk = UpdateProvider.file(this);
            java.io.File folder = apk.getParentFile();
            String error;
            if (folder == null || (!folder.isDirectory() && !folder.mkdirs())) {
                error = t("Impossible de télécharger la mise à jour.");
            } else {
                java.io.File[] old = folder.listFiles();
                if (old != null) for (java.io.File file : old) file.delete();
                error = nativeDownloadUpdate(apk.getPath());
            }
            String failure = error;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                progress.dismiss();
                if (failure.isEmpty() && openInstaller()) return;
                new AlertDialog.Builder(this).setTitle(t("Mise à jour impossible"))
                    .setMessage(failure.isEmpty() ? t("Télécharge la nouvelle version sur GitHub.") : failure)
                    .setNegativeButton("OK", null)
                    .setPositiveButton(t("Versions GitHub"), (shown, which) -> openReleases()).show();
            });
        }, "lunaone-update-download").start();
    }

    @SuppressWarnings("deprecation")
    private boolean openInstaller() {
        Intent install = new Intent(Intent.ACTION_INSTALL_PACKAGE).setDataAndType(UpdateProvider.uri(), UpdateProvider.TYPE)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            startActivity(install);
            return true;
        } catch (ActivityNotFoundException missing) {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW).setDataAndType(UpdateProvider.uri(), UpdateProvider.TYPE)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK));
                return true;
            } catch (ActivityNotFoundException alsoMissing) {
                return false;
            }
        }
    }

    private void openReleases() {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/kazar-root/LunaONE/releases")));
        } catch (ActivityNotFoundException ignored) {
            // No browser, as on some TVs: the dialog already said where to look.
        }
    }

    @SuppressWarnings("deprecation")
    private String connectionAndBattery() {
        StringBuilder details = new StringBuilder();
        var manager = getSystemService(ConnectivityManager.class);
        var activeNetwork = manager.getActiveNetwork();
        var capabilities = activeNetwork == null ? null : manager.getNetworkCapabilities(activeNetwork);
        if (capabilities != null) {
            if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) details.append(" · LAN");
            else if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) details.append(" · Wi-Fi");
            else if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
                var line = manager.getActiveNetworkInfo();
                details.append(line != null && line.getSubtype() == TelephonyManager.NETWORK_TYPE_NR ? " · 5G" : " · Mobile");
            }
        }
        if (!tv) {
            int battery = getSystemService(BatteryManager.class).getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY);
            if (battery >= 0 && battery <= 100) details.append(" · ").append(battery).append('%');
        }
        return details.toString();
    }

    private void openCatalog(int category, boolean explore) {
        if (active != null) return;
        begin(explore ? "Exploration du catalogue Luna…" : "Chargement du catalogue…",
            job -> nativeCatalog(job, category, explore), () -> {
                section = category;
                searchText = "";
            });
    }

    private void confirmSignOut() {
        if (active != null) return;
        new AlertDialog.Builder(this).setMessage(t("Déconnecter ce compte de LunaONE ?"))
            .setNegativeButton(t("Annuler"), null)
            .setPositiveButton(t("Se déconnecter"), (dialog, which) -> begin("Déconnexion…", job -> {
                nativeSignOut();
                return null;
            })).show();
    }

    private void begin(String message, Operation operation) {
        begin(message, operation, () -> {});
    }

    private void begin(String message, Operation operation, Runnable applySelection) {
        if (active != null) return;
        Job job = new Job(screen);
        active = job;
        setStatus(message);
        refreshActions();
        ENGINE.execute(() -> {
            String[] result = null;
            String error = "";
            boolean registered = savedDevice;
            try {
                result = operation.run(job);
            } catch (RuntimeException failure) {
                if (!job.cancelled) error = failure.getMessage();
            }
            try {
                registered = nativeHasRegistration();
            } catch (RuntimeException failure) {
                if (error.isEmpty()) error = failure.getMessage();
            }
            String[] items = result;
            String detail = error;
            boolean persisted = registered;
            runOnUiThread(() -> {
                if (active != job || destroyed) return;
                active = null;
                savedDevice = persisted;
                deviceCode = "";
                verificationUrl = "";
                if (items != null) {
                    catalog = new LibraryScreen.Content(items);
                    applySelection.run();
                    selectedGame = 0;
                    showCatalog(detail);
                } else if (screen == Screen.CATALOG && savedDevice) {
                    setStatus(job.cancelled ? "Opération annulée." : detail);
                    refreshActions();
                } else {
                    catalog = null;
                    library = null;
                    section = ALL_GAMES;
                    searchText = "";
                    showLogin(job.cancelled ? "Opération annulée." : detail);
                }
            });
        });
    }

    private void cancelJob() {
        if (active == null) return;
        active.cancelled = true;
        if (codePanel != null) showCodePanel(false);
        setStatus("Annulation…");
        refreshActions();
    }

    private void refreshActions() {
        if (screen == Screen.CATALOG) {
            library.setBusy(active != null, active != null && active.cancelled);
            return;
        }
        boolean available = active == null;
        for (View action : actions) action.setEnabled(available);
        cancel.setVisibility(available ? View.GONE : View.VISIBLE);
        cancel.setEnabled(!available && !active.cancelled);
        if (tv) {
            View focus = available ? preferredFocus : cancel;
            if (focus != null && focus.isEnabled()) {
                focus.requestFocus();
            }
        }
    }

    private void setStatus(String value) {
        if (screen == Screen.CATALOG) {
            library.setStatus(t(value));
            return;
        }
        if (screen == Screen.PLAYER && loadingScreen != null) {
            if (value.isEmpty()) {
                stopLoadingPulse();
                loadingScreen.setVisibility(View.GONE);
                launchConfirm = null;
                return;
            }
            if (loadingScreen.getVisibility() == View.GONE) return;
            int stage = value.equals("WebRTC completed") ? 3 :
                value.startsWith("Requesting a Luna") || value.startsWith("Luna queue") ||
                value.startsWith("Connecting to Luna") || value.startsWith("WebRTC ") ? 2 : 1;
            loadingStage = Math.max(loadingStage, stage);
            if (value.startsWith("Luna queue position ")) {
                status.setText(t("Position dans la file") + "  " + value.substring("Luna queue position ".length()));
            } else {
                String[] states = {"Préparation du flux…", "Connexion au serveur Luna…", "Synchronisation de l’image…"};
                status.setText(t(states[loadingStage - 1]));
            }
            return;
        }
        status.setText(t(value));
    }

    private void startLoadingPulse() {
        for (int i = 0; i < loadingSteps.length; ++i)
            loadingSteps[i].setAlpha(i == 0 ? 1f : 0.23f);
        if (!android.animation.ValueAnimator.areAnimatorsEnabled()) return;

        loadingPulse = android.animation.ValueAnimator.ofFloat(0f, 3f);
        loadingPulse.setDuration(600);
        loadingPulse.setInterpolator(new android.view.animation.LinearInterpolator());
        loadingPulse.setRepeatCount(android.animation.ValueAnimator.INFINITE);
        loadingPulse.addUpdateListener(animation -> {
            int active = (int) (float) animation.getAnimatedValue() % loadingSteps.length;
            for (int i = 0; i < loadingSteps.length; ++i)
                loadingSteps[i].setAlpha(i == active ? 1f : 0.23f);
        });
        loadingPulse.start();
    }

    private void stopLoadingPulse() {
        if (loadingPulse == null) return;
        loadingPulse.cancel();
        loadingPulse = null;
    }

    private FrameLayout launchScreen(LibraryScreen.Content.Game game) {
        FrameLayout screen = new FrameLayout(this);
        screen.setBackgroundColor(0xff100d1d);
        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.luna_icon);
        FrameLayout.LayoutParams logoSize = new FrameLayout.LayoutParams(dp(48), dp(48), Gravity.TOP | Gravity.START);
        logoSize.setMarginStart(dp(tv ? 40 : 16));
        logoSize.topMargin = dp(tv ? 20 : 12);
        screen.addView(logo, logoSize);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setGravity(Gravity.CENTER_HORIZONTAL);
        ImageView cover = new ImageView(this);
        cover.setScaleType(ImageView.ScaleType.CENTER_CROP);
        cover.setContentDescription(game.title());
        cover.setBackground(TvUi.shape(0xff251939, dp(12), 0, 0));
        cover.setClipToOutline(true);
        content.addView(cover, new LinearLayout.LayoutParams(dp(tv ? 144 : 105), dp(tv ? 192 : 140)));
        Artwork.show(cover, game.cover());
        status = text(t("Préparation du flux…"), tv ? 22 : 16, Color.WHITE);
        status.setGravity(Gravity.CENTER);
        status.setPadding(dp(12), dp(15), dp(12), dp(12));
        content.addView(status);
        LinearLayout squares = new LinearLayout(this);
        for (int i = 0; i < loadingSteps.length; ++i) {
            View square = new View(this);
            square.setBackground(TvUi.shape(Color.WHITE, dp(2), 0, 0));
            LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(11), dp(11));
            size.setMargins(dp(5), 0, dp(5), 0);
            squares.addView(square, size);
            loadingSteps[i] = square;
        }
        content.addView(squares, new LinearLayout.LayoutParams(-2, -2));
        screen.addView(content, new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER));
        // Bottom right: back to the catalog before the game starts, after a
        // confirmation. Not selected at first: an arrow selects it.
        launchCancel = launchButton(t("Annuler le lancement du jeu"), this::showLaunchConfirm);
        launchConfirm = null;
        FrameLayout.LayoutParams position = new FrameLayout.LayoutParams(-2, dp(48), Gravity.BOTTOM | Gravity.END);
        position.setMarginEnd(dp(tv ? 44 : 16));
        position.bottomMargin = dp(tv ? 32 : 16);
        screen.addView(launchCancel, position);
        loadingStage = 1;
        return screen;
    }

    private Button launchButton(String label, Runnable action) {
        Button view = new Button(this);
        view.setText(label);
        view.setAllCaps(false);
        view.setTextColor(Color.WHITE);
        view.setTextSize(tv ? 17 : 15);
        view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setStateListAnimator(null);
        view.setPadding(dp(22), 0, dp(22), 0);
        view.setFocusable(true);
        android.graphics.drawable.StateListDrawable look = new android.graphics.drawable.StateListDrawable();
        look.addState(new int[]{android.R.attr.state_focused}, TvUi.shape(0x55aa88ff, dp(24), dp(3), 0xffaa88ff));
        look.addState(new int[]{android.R.attr.state_pressed}, TvUi.shape(0x55aa88ff, dp(24), dp(3), 0xffaa88ff));
        look.addState(new int[]{}, TvUi.shape(0xe818142b, dp(24), dp(1), 0x867d718e));
        view.setBackground(look);
        view.setOnClickListener(clicked -> action.run());
        return view;
    }

    // « Annuler le lancement du jeu — Êtes-vous sûr ? » Non (selected) / Oui
    // over the loading screen.
    private void showLaunchConfirm() {
        if (loadingScreen == null || launchConfirm != null || active == null || active.cancelled) return;
        FrameLayout shade = new FrameLayout(this);
        shade.setBackgroundColor(0xa6000000);
        shade.setClickable(true);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setGravity(Gravity.CENTER_HORIZONTAL);
        panel.setPadding(dp(36), dp(28), dp(36), dp(26));
        panel.setBackground(TvUi.shape(0xf218142b, dp(22), dp(1), 0x867d718e));
        TextView title = text(t("Annuler le lancement du jeu"), tv ? 22 : 18, Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        panel.addView(title, new LinearLayout.LayoutParams(-2, -2));
        TextView question = text(t("Êtes-vous sûr ?"), tv ? 18 : 15, 0xffc7bdd4);
        question.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams below = new LinearLayout.LayoutParams(-2, -2);
        below.topMargin = dp(8);
        panel.addView(question, below);
        LinearLayout choices = new LinearLayout(this);
        choices.setGravity(Gravity.CENTER);
        Button no = launchButton(t("Non"), this::hideLaunchConfirm);
        Button yes = launchButton(t("Oui"), this::stopPlayer);
        LinearLayout.LayoutParams left = new LinearLayout.LayoutParams(dp(150), dp(48));
        left.setMarginEnd(dp(18));
        choices.addView(no, left);
        choices.addView(yes, new LinearLayout.LayoutParams(dp(150), dp(48)));
        LinearLayout.LayoutParams row = new LinearLayout.LayoutParams(-2, -2);
        row.topMargin = dp(22);
        panel.addView(choices, row);
        shade.addView(panel, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        loadingScreen.addView(shade, new FrameLayout.LayoutParams(-1, -1));
        launchConfirm = shade;
        if (launchCancel != null) launchCancel.setFocusable(false);
        no.requestFocus();
    }

    private void hideLaunchConfirm() {
        if (launchConfirm == null) return;
        if (launchConfirm.getParent() instanceof ViewGroup parent) parent.removeView(launchConfirm);
        launchConfirm = null;
        if (launchCancel != null) {
            launchCancel.setFocusable(true);
            launchCancel.requestFocus();
        }
    }

    private void startPlayer(int index) {
        if (active != null || index < 0 || index >= catalog.games.size()) return;
        selectedGame = index;
        screen = Screen.PLAYER;
        surfaceStarted = false;
        Job job = new Job(Screen.PLAYER);
        active = job;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN |
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        if (!tv) setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        // Ask the TV for its game mode (ALLM over HDMI): its picture processing
        // otherwise adds several frames between a button press and the screen.
        if (Build.VERSION.SDK_INT >= 30) getWindow().setPreferMinimalPostProcessing(true);
        holdWifi(true);
        gamepadId = -1;
        getSystemService(android.hardware.input.InputManager.class).registerInputDeviceListener(controllers, null);
        FrameLayout player = new FrameLayout(this);
        player.setBackgroundColor(Color.BLACK);
        SurfaceView picture = new SurfaceView(this);
        player.addView(picture, new FrameLayout.LayoutParams(-1, -1));
        player.addOnLayoutChangeListener((view, l, t, r, b, oldL, oldT, oldR, oldB) -> {
            if (r <= l || b <= t) return;
            int width = Math.min(r - l, (b - t) * 16 / 9);
            int height = width * 9 / 16;
            ViewGroup.LayoutParams current = picture.getLayoutParams();
            if (current.width != width || current.height != height)
                picture.setLayoutParams(new FrameLayout.LayoutParams(width, height, Gravity.CENTER));
        });
        loadingScreen = launchScreen(catalog.games.get(index));
        player.addView(loadingScreen, new FrameLayout.LayoutParams(-1, -1));
        hud = text("", tv ? 19 : 15, Color.WHITE);
        hud.setSingleLine(true);
        hud.setPadding(dp(10), dp(6), dp(10), dp(6));
        hud.setBackground(TvUi.shape(0xc421183f, dp(8), 0, 0));
        hud.setVisibility(View.GONE);
        FrameLayout.LayoutParams hudPosition = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.END);
        hudPosition.setMarginEnd(dp(tv ? 44 : 12));
        hudPosition.topMargin = dp(tv ? 24 : 10);
        player.addView(hud, hudPosition);
        picture.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                if (active != job || job.cancelled || surfaceStarted) return;
                surfaceStarted = true;
                Surface surface = holder.getSurface();
                ENGINE.execute(() -> {
                    String error = "";
                    try {
                        if (!job.cancelled) nativePlay(index, surface, job, Build.VERSION.SDK_INT >= 29);
                    } catch (RuntimeException failure) {
                        if (!job.cancelled) error = failure.getMessage();
                    }
                    String detail = error;
                    runOnUiThread(() -> finishPlayer(job, detail));
                });
            }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {}
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                if (active == job) stopPlayer();
            }
        });
        setContentView(player);
        player.setFocusableInTouchMode(true);
        player.requestFocus();
        // Android batches stick movements until the next display refresh.
        // Deliver them as they arrive while the focused player is shown.
        if (Build.VERSION.SDK_INT >= 30)
            player.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_JOYSTICK);
        startLoadingPulse();
    }

    // Wi-Fi power saving holds packets for tens of milliseconds and loses some:
    // on the TV this showed as ~35 repaired packets a second and audio gaps.
    // Low-latency mode turns it off while the game is in the foreground.
    @SuppressWarnings("deprecation")
    private void holdWifi(boolean hold) {
        try {
            if (hold && wifiLock == null) {
                WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
                if (wifi == null) return;
                wifiLock = wifi.createWifiLock(Build.VERSION.SDK_INT >= 29 ?
                    WifiManager.WIFI_MODE_FULL_LOW_LATENCY : WifiManager.WIFI_MODE_FULL_HIGH_PERF, "LunaONE:stream");
                wifiLock.setReferenceCounted(false);
                wifiLock.acquire();
            } else if (!hold && wifiLock != null) {
                if (wifiLock.isHeld()) wifiLock.release();
                wifiLock = null;
            }
        } catch (RuntimeException unavailable) {
            wifiLock = null;
        }
    }

    private void stopPlayer() {
        Job job = active;
        if (job == null || job.screen != Screen.PLAYER || job.cancelled) return;
        job.cancelled = true;
        setStatus("Fermeture de la partie…");
        if (!surfaceStarted) finishPlayer(job, "");
    }

    private void finishPlayer(Job job, String error) {
        if (active != job || destroyed) return;
        active = null;
        stopLoadingPulse();
        loadingScreen = null;
        launchCancel = null;
        launchConfirm = null;
        gamepadId = -1;
        leftTriggerKey = rightTriggerKey = false;
        buttons = hatButtons = left = right = lx = ly = rx = ry = 0;
        nativeInput(0, 0, 0, 0, 0, 0, 0);
        getSystemService(android.hardware.input.InputManager.class).unregisterInputDeviceListener(controllers);
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= 30) getWindow().setPreferMinimalPostProcessing(false);
        holdWifi(false);
        getWindow().getDecorView().setSystemUiVisibility(0);
        if (!tv) setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
        showCatalog(error.isEmpty() ? "Partie fermée." : error);
    }

    // « Intervertir A/B » also applies to LunaONE's screens: A confirms and B
    // goes back unless the option swaps them.
    private KeyEvent swapFaceButtons(KeyEvent event) {
        int key = event.getKeyCode();
        if (key != KeyEvent.KEYCODE_BUTTON_A && key != KeyEvent.KEYCODE_BUTTON_B) return event;
        if (controllerOptions < 0) {
            try { controllerOptions = nativeControllerOptions(); }
            catch (RuntimeException unavailable) { controllerOptions = 0; }
        }
        if ((controllerOptions & 1) == 0) return event;
        int swapped = key == KeyEvent.KEYCODE_BUTTON_A ? KeyEvent.KEYCODE_BUTTON_B : KeyEvent.KEYCODE_BUTTON_A;
        return new KeyEvent(event.getDownTime(), event.getEventTime(), event.getAction(), swapped, event.getRepeatCount(),
            event.getMetaState(), event.getDeviceId(), event.getScanCode(), event.getFlags(), event.getSource());
    }

    // L1 + L2 + R1 + R2 in the catalog opens the search with the system keyboard.
    private void catalogChord() {
        boolean all = chordL1 && chordR1 && chordL2 && chordR2;
        if (all && !chordHeld && screen == Screen.CATALOG && active == null && nativeVirtualKeyboard()) showSearch();
        chordHeld = all || (chordHeld && (chordL1 || chordR1 || chordL2 || chordR2));
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        // While the game loads: an arrow selects « Annuler le lancement du
        // jeu », OK or A presses the selected button. B or Back asks
        // « Êtes-vous sûr ? », and in that question means Non.
        if (screen == Screen.PLAYER && loadingScreen != null && loadingScreen.getVisibility() == View.VISIBLE) {
            int key = event.getKeyCode();
            boolean down = event.getAction() == KeyEvent.ACTION_DOWN, up = event.getAction() == KeyEvent.ACTION_UP;
            View focused = getCurrentFocus() instanceof Button button && button.isFocusable() ? button : null;
            if (key == KeyEvent.KEYCODE_DPAD_CENTER || key == KeyEvent.KEYCODE_ENTER || key == KeyEvent.KEYCODE_BUTTON_A ||
                key == KeyEvent.KEYCODE_NUMPAD_ENTER) {
                if (focused == null) {
                    if (up && launchCancel != null) launchCancel.requestFocus();
                } else if (down) {
                    focused.setPressed(true);
                } else if (up) {
                    focused.setPressed(false);
                    focused.performClick();
                }
                return true;
            }
            if (key == KeyEvent.KEYCODE_BUTTON_B || key == KeyEvent.KEYCODE_BACK || key == KeyEvent.KEYCODE_ESCAPE) {
                if (up) {
                    if (launchConfirm != null) hideLaunchConfirm();
                    else showLaunchConfirm();
                }
                return true;
            }
            if (key == KeyEvent.KEYCODE_DPAD_UP || key == KeyEvent.KEYCODE_DPAD_DOWN ||
                key == KeyEvent.KEYCODE_DPAD_LEFT || key == KeyEvent.KEYCODE_DPAD_RIGHT) {
                if (!down) return true;
                if (focused == null) {
                    if (launchCancel != null && launchConfirm == null) launchCancel.requestFocus();
                } else if (launchConfirm != null && (key == KeyEvent.KEYCODE_DPAD_LEFT || key == KeyEvent.KEYCODE_DPAD_RIGHT)) {
                    View next = focused.focusSearch(key == KeyEvent.KEYCODE_DPAD_LEFT ? View.FOCUS_LEFT : View.FOCUS_RIGHT);
                    if (next instanceof Button) next.requestFocus();
                }
                return true;
            }
        }
        if (screen != Screen.PLAYER) event = swapFaceButtons(event);
        if (settingsScreen != null) {
            if (settingsScreen.getParent() == null) settingsScreen = null;
            else if (settingsScreen.handleKey(event)) return true;
        }
        if (screen == Screen.CATALOG && event.getDevice() != null && isGamepad(event)) {
            controllerVendor = event.getDevice().getVendorId();
            boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
            switch (event.getKeyCode()) {
                case KeyEvent.KEYCODE_BUTTON_L1 -> chordL1 = down;
                case KeyEvent.KEYCODE_BUTTON_R1 -> chordR1 = down;
                case KeyEvent.KEYCODE_BUTTON_L2 -> chordL2 = down;
                case KeyEvent.KEYCODE_BUTTON_R2 -> chordR2 = down;
                default -> { return super.dispatchKeyEvent(event); }
            }
            catalogChord();
            return true;
        }
        if (screen == Screen.PLAYER && tv && (event.getKeyCode() == KeyEvent.KEYCODE_BACK ||
            event.getKeyCode() == KeyEvent.KEYCODE_ESCAPE)) {
            if (event.getAction() == KeyEvent.ACTION_UP) stopPlayer();
            return true;
        }
        if (screen == Screen.PLAYER && (tv ? isGamepad(event) :
            (event.getSource() & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_DPAD)) != 0)) {
            int key = event.getKeyCode();
            if (key == KeyEvent.KEYCODE_BUTTON_L2 || key == KeyEvent.KEYCODE_BUTTON_R2) {
                if (!acceptController(event.getDevice(), event.getAction() == KeyEvent.ACTION_DOWN)) return true;
                boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
                if (key == KeyEvent.KEYCODE_BUTTON_L2) leftTriggerKey = down;
                else rightTriggerKey = down;
                sendInput();
                return true;
            }
            int mapped = buttonMask(key);
            if (mapped != 0) {
                if (!acceptController(event.getDevice(), event.getAction() == KeyEvent.ACTION_DOWN)) return true;
                if (event.getAction() == KeyEvent.ACTION_DOWN) buttons |= mapped;
                else if (event.getAction() == KeyEvent.ACTION_UP) buttons &= ~mapped;
                sendInput();
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    private static boolean isGamepad(KeyEvent event) {
        InputDevice device = event.getDevice();
        int sources = device == null ? event.getSource() : device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
               (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (screen == Screen.CATALOG && event.getActionMasked() == MotionEvent.ACTION_MOVE &&
            (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            if (event.getDevice() != null) controllerVendor = event.getDevice().getVendorId();
            chordL2 = trigger(event, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE) >= 128;
            chordR2 = trigger(event, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS) >= 128;
            catalogChord();
        }
        if (screen == Screen.PLAYER && event.getActionMasked() == MotionEvent.ACTION_MOVE &&
            (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            // Sticks and triggers of another device, such as a remote declaring
            // a joystick, never take over the controller in use.
            if (!acceptController(event.getDevice(), false)) return true;
            lx = axis(event, MotionEvent.AXIS_X);
            ly = -axis(event, MotionEvent.AXIS_Y);
            rx = axis(event, MotionEvent.AXIS_Z);
            ry = -axis(event, MotionEvent.AXIS_RZ);
            left = trigger(event, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE);
            right = trigger(event, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS);
            float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
            float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
            hatButtons = (hatY < -0.5f ? 1 : 0) | (hatY > 0.5f ? 2 : 0) |
                         (hatX < -0.5f ? 4 : 0) | (hatX > 0.5f ? 8 : 0);
            sendInput();
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    // A real controller: buttons and sticks. TV remotes may declare a joystick
    // or a gamepad source, but not both with stick axes.
    private static boolean isController(InputDevice device) {
        if (device == null || device.isVirtual()) return false;
        int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD &&
            (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK &&
            device.getMotionRange(MotionEvent.AXIS_X) != null && device.getMotionRange(MotionEvent.AXIS_Y) != null;
    }

    // The controller in use stays selected while it is connected. Another
    // controller takes over only when one of its buttons is pressed; other
    // devices are ignored. Each change goes to the runtime journal.
    private boolean acceptController(InputDevice device, boolean press) {
        int id = device == null ? -1 : device.getId();
        if (id == gamepadId && id != -1) return true;
        boolean current = gamepadId != -1 && InputDevice.getDevice(gamepadId) != null;
        if (!isController(device) || (current && !press)) return false;
        gamepadId = id;
        leftTriggerKey = rightTriggerKey = false;
        buttons = hatButtons = left = right = lx = ly = rx = ry = 0;
        controllerVendor = device.getVendorId();
        nativeLog("controller_selected", "name=" + device.getName() + " vendor=" +
            Integer.toHexString(device.getVendorId()) + " product=" + Integer.toHexString(device.getProductId()) +
            " id=" + id);
        return true;
    }

    private final android.hardware.input.InputManager.InputDeviceListener controllers =
        new android.hardware.input.InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int id) {
                InputDevice device = InputDevice.getDevice(id);
                if (device != null && isController(device)) nativeLog("controller_added", "name=" + device.getName() + " id=" + id);
            }
            @Override public void onInputDeviceRemoved(int id) {
                if (id != gamepadId) return;
                // Release everything the controller held when it goes away.
                nativeLog("controller_removed", "id=" + id);
                gamepadId = -1;
                leftTriggerKey = rightTriggerKey = false;
                buttons = hatButtons = left = right = lx = ly = rx = ry = 0;
                sendInput();
            }
            @Override public void onInputDeviceChanged(int id) {
                if (id == gamepadId) nativeLog("controller_changed", "id=" + id);
            }
        };

    private void sendInput() {
        nativeInput(buttons | hatButtons, leftTriggerKey ? 255 : left, rightTriggerKey ? 255 : right, lx, ly, rx, ry);
    }

    private static int axis(MotionEvent event, int axis) {
        float value = event.getAxisValue(axis);
        InputDevice device = event.getDevice();
        InputDevice.MotionRange range = device == null ? null : device.getMotionRange(axis, event.getSource());
        if (Math.abs(value) <= (range == null ? 0.05f : range.getFlat())) return 0;
        return Math.round(Math.max(-1, Math.min(1, value)) * 32767);
    }

    private static int trigger(MotionEvent event, int primary, int fallback) {
        float value = Math.max(event.getAxisValue(primary), event.getAxisValue(fallback));
        return Math.round(Math.max(0, Math.min(1, value)) * 255);
    }

    private static int buttonMask(int key) {
        switch (key) {
            case KeyEvent.KEYCODE_DPAD_UP: return 0x0001;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0x0002;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0x0004;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0x0008;
            case KeyEvent.KEYCODE_BUTTON_START: return 0x0010;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return 0x0020;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 0x0040;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 0x0080;
            case KeyEvent.KEYCODE_BUTTON_L1: return 0x0100;
            case KeyEvent.KEYCODE_BUTTON_R1: return 0x0200;
            case KeyEvent.KEYCODE_BUTTON_MODE: return 0x0400;
            case KeyEvent.KEYCODE_BUTTON_A: return 0x1000;
            case KeyEvent.KEYCODE_BUTTON_B: return 0x2000;
            case KeyEvent.KEYCODE_BUTTON_X: return 0x4000;
            case KeyEvent.KEYCODE_BUTTON_Y: return 0x8000;
            default: return 0;
        }
    }
}
