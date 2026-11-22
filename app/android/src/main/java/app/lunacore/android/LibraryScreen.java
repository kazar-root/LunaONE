// SPDX-License-Identifier: GPL-3.0-only
package app.lunacore.android;

import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.text.format.DateFormat;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Date;
import java.util.function.Function;
import java.util.function.Supplier;

final class LibraryScreen extends FrameLayout {
    static final class Content {
        record Game(String title, String cover, String backdrop, String offer, String metadata, String access) {}

        final String plan, name, avatar;
        final ArrayList<Game> games = new ArrayList<>();

        Content(String[] fields) {
            // JNI payload: plan, profile name, avatar, then six fields per game.
            if (fields.length < 3 || (fields.length - 3) % 6 != 0)
                throw new IllegalArgumentException("Invalid LunaONE catalog");
            plan = fields[0];
            name = fields[1];
            avatar = fields[2];
            for (int i = 3; i < fields.length; i += 6)
                games.add(new Game(fields[i], fields[i + 1], fields[i + 2], fields[i + 3], fields[i + 4], fields[i + 5]));
        }
    }

    interface Actions {
        void selection(int index);
        void play(int index);
        void section(int index);
        void search();
        void account();
        void settings();
        void cancel();
    }

    private static final int PAGE_SIZE = 10;
    private static final int BACKGROUND = 0xff100d1d;
    private static final int TEXT = 0xfffaf8ff;
    private static final int SECONDARY = 0xffc7bdd4;
    private static final int VIOLET = 0xffaa88ff;
    private static final int GREEN = 0xff78e3bd;
    private static final int INK = 0xff151125;
    private static final int SHELL = 0xe818142b;
    private static final String[] SECTIONS = {"Top 10", "Reprendre", "Tous les jeux", "Luna Premium"};

    private final Content content;
    private final Actions actions;
    private final Function<String, String> translate;
    private final Supplier<String> systemStatus;
    private final boolean tv, simple;
    private final int section;
    private final String query;
    private final ArrayList<View> controls = new ArrayList<>();
    private final ArrayList<FrameLayout> covers = new ArrayList<>();
    private final Runnable rebuild = this::render;
    private final Runnable delayedBackdrop = this::loadBackdrop;
    private final Runnable tick = this::updateSystemStatus;
    private int selected, first;
    private int insetLeft, insetTop, insetRight, insetBottom;
    private float scale, offsetX, offsetY, width;
    private boolean narrow, busy, cancelling;
    private String message = "";
    private ImageView backdrop;
    private TextView title, metadata, offer, status, system;
    private Button play, cancel;
    private FrameLayout activeNav;
    private HorizontalScrollView strip;
    private android.widget.ScrollView listScroll;

    LibraryScreen(Activity activity, Content content, int selected, int section, String query, boolean tv, boolean simple,
                  Function<String, String> translate, Supplier<String> systemStatus, Actions actions) {
        super(activity);
        this.content = content;
        this.selected = content.games.isEmpty() ? 0 : Math.max(0, Math.min(selected, content.games.size() - 1));
        this.section = section;
        this.query = query;
        this.tv = tv;
        this.simple = simple;
        this.translate = translate;
        this.systemStatus = systemStatus;
        this.actions = actions;
        setBackgroundColor(BACKGROUND);
        setOnApplyWindowInsetsListener((view, insets) -> {
            insetLeft = insets.getSystemWindowInsetLeft();
            insetTop = insets.getSystemWindowInsetTop();
            insetRight = insets.getSystemWindowInsetRight();
            insetBottom = insets.getSystemWindowInsetBottom();
            scheduleRender();
            return insets;
        });
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        scheduleRender();
    }

    @Override protected void onDetachedFromWindow() {
        removeCallbacks(rebuild);
        removeCallbacks(delayedBackdrop);
        removeCallbacks(tick);
        super.onDetachedFromWindow();
    }

    private String t(String key) { return translate.apply(key); }

    private void scheduleRender() {
        removeCallbacks(rebuild);
        if (getWidth() > 0 && getHeight() > 0) post(rebuild);
    }

    void setStatus(String value) {
        message = value;
        if (status != null) status.setText(message);
    }

    void setBusy(boolean value, boolean cancelling) {
        busy = value;
        this.cancelling = cancelling;
        for (View control : controls) control.setEnabled(!busy);
        if (cancel == null) return;
        cancel.setVisibility(busy ? VISIBLE : GONE);
        cancel.setEnabled(busy && !cancelling);
        if (!tv) return;
        if (cancel.isEnabled()) cancel.requestFocus();
        else if (!busy && (findFocus() == null || findFocus() == cancel)) focusSelected();
    }

    private void render() {
        final float usableWidth = getWidth() - insetLeft - insetRight;
        final float usableHeight = getHeight() - insetTop - insetBottom;
        if (usableWidth < 1 || usableHeight < 1) return;
        narrow = usableWidth < usableHeight * 1.22f;
        float designWidth = narrow ? 400 : 1280;
        float designHeight = narrow ? 660 : 720;
        scale = Math.min(usableWidth / designWidth, usableHeight / designHeight);
        width = usableWidth / scale;
        offsetX = insetLeft;
        offsetY = insetTop + (usableHeight - designHeight * scale) / 2;
        BitmapState oldImage = new BitmapState(backdrop);
        removeAllViews();
        controls.clear();
        covers.clear();
        activeNav = null;
        system = null;

        backdrop = new ImageView(getContext());
        backdrop.setScaleType(ImageView.ScaleType.CENTER_CROP);
        if (oldImage.image != null) {
            backdrop.setImageDrawable(oldImage.image);
            backdrop.setTag(oldImage.key);
        }
        addView(backdrop, new FrameLayout.LayoutParams(-1, -1));
        View shade = new View(getContext());
        shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
            new int[]{0xbd110f1d, 0x55100d1c, 0x28100c17, 0xd8090612, 0xff090611}));
        addView(shade, new FrameLayout.LayoutParams(-1, -1));
        addHeader();
        addNavigation();
        addHeading();
        addCarousel();
        addDetails();
        addFooter(designHeight);
        refreshSelection(true);
        setBusy(busy, cancelling);
        updateSystemStatus();
    }

    private void addHeader() {
        float left = narrow ? 23 : 40;
        ImageView logo = new ImageView(getContext());
        logo.setImageResource(R.drawable.luna_icon);
        logo.setBackground(TvUi.shape(SHELL, px(12), 0, 0));
        logo.setClipToOutline(true);
        logo.setScaleType(ImageView.ScaleType.CENTER_CROP);
        place(this, logo, left, 21, 46, 46, true);
        TextView name = label("LunaONE", 25, TEXT, true);
        place(this, name, left + 56, 21, 143, 46, true);

        navigation("account", t("Compte"), false, width - (narrow ? 144 : 241), 7,
            narrow ? 66 : 86, actions::account, content.avatar);
        navigation("settings", t("Réglages"), false, width - (narrow ? 74 : 133), 7,
            narrow ? 66 : 86, actions::settings, "");

        if (!narrow) {
            system = label("", 15, TEXT, false);
            system.setGravity(Gravity.CENTER);
            system.setBackground(TvUi.shape(SHELL, px(30), px(1), 0x668e80a6));
            place(this, system, width - 638, 20, 372, 48, true);
        }
    }

    private void addNavigation() {
        boolean searching = !query.isEmpty();
        float left = narrow ? 4 : 35;
        float step = narrow ? (width - 8) / 4 : 111;
        float y = narrow ? 87 : 78;
        // « Tous les jeux » holds the whole catalog, Luna Premium included.
        navigation("trophy", SECTIONS[0], section == 0 && !searching, left, y, step,
            () -> actions.section(0), "");
        navigation("clock", t(SECTIONS[1]), section == 1 && !searching, left + step, y, step,
            () -> actions.section(1), "");
        navigation("grid", t(SECTIONS[2]), section >= 2 && !searching, left + 2 * step, y, step,
            () -> actions.section(2), "");
        navigation("search", t("Rechercher"), searching, left + 3 * step, y, step, actions::search, "");
    }

    private void addHeading() {
        float left = narrow ? 24 : 64;
        float y = narrow ? 177 : 157;
        View accent = new View(getContext());
        accent.setBackground(TvUi.shape(VIOLET, px(2), 0, 0));
        place(this, accent, left, y + 8, 4, 20, true);
        String heading = query.isEmpty() ? t(SECTIONS[section]) : t("Rechercher");
        String count = t("{} jeux").replace("{}", Integer.toString(content.games.size()));
        LinearLayout info = new LinearLayout(getContext());
        info.setGravity(Gravity.CENTER_VERTICAL);
        info.addView(label(heading, 20, TEXT, true));
        TextView amount = label(count, 15, SECONDARY, false);
        amount.setPadding(px(14), 0, 0, 0);
        info.addView(amount);
        float widthForHeading = narrow ? width - left * 2 - 18 : Math.min(510, width / 2 - left);
        place(this, info, left + 16, y, widthForHeading, 36, true);

        status = label(message, narrow ? 13 : 14, SECONDARY, false);
        status.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        status.setAccessibilityLiveRegion(ACCESSIBILITY_LIVE_REGION_POLITE);
        if (narrow) place(this, status, left, 612, width - left * 2, 28, true);
        else place(this, status, width / 2, y, width / 2 - left, 36, true);
    }

    private void addCarousel() {
        float cardWidth = narrow ? 148 : 192;
        float cardHeight = narrow ? 198 : 256;
        float start = narrow ? 24 : 64;
        float y = narrow ? 224 : 211;
        float rowHeight = cardHeight + 24;
        LinearLayout track = new LinearLayout(getContext());
        if (simple) {
            cardWidth = Math.min(width - 2 * start - 8, 590);
            cardHeight = 42;
            listScroll = new android.widget.ScrollView(getContext());
            listScroll.setFocusable(false);
            track.setOrientation(LinearLayout.VERTICAL);
            track.setPadding(px(4), px(4), px(4), px(4));
            place(this, listScroll, start, y, cardWidth + 8, rowHeight, true);
            listScroll.addView(track, new android.widget.ScrollView.LayoutParams(-1, -2));
        } else {
            strip = new HorizontalScrollView(getContext());
            strip.setClipChildren(false);
            strip.setClipToPadding(false);
            strip.setHorizontalScrollBarEnabled(false);
            strip.setFocusable(false);
            place(this, strip, 0, y, width, rowHeight, true);
            track.setGravity(Gravity.CENTER_VERTICAL);
            track.setClipChildren(false);
            track.setPadding(px(start), 0, px(start), 0);
            strip.addView(track, new HorizontalScrollView.LayoutParams(-2, -1));
        }
        first = selected / PAGE_SIZE * PAGE_SIZE;
        int last = Math.min(first + PAGE_SIZE, content.games.size());
        for (int i = first; i < last; i++) {
            int index = i;
            Content.Game game = content.games.get(i);
            FrameLayout cover = new FrameLayout(getContext());
            cover.setId(View.generateViewId());
            cover.setContentDescription((section == 0 && query.isEmpty() ? "#" + (i + 1) + " " : "") + game.title());
            cover.setFocusable(true);
            cover.setClickable(true);
            cover.setClipToOutline(true);
            updateCover(cover, i == selected);
            boolean ranked = section == 0 && query.isEmpty();
            String title = simple && ranked ? "#" + (i + 1) + "  " + game.title() : game.title();
            TextView placeholder = label(title, 18, TEXT, true);
            placeholder.setGravity(simple ? Gravity.START | Gravity.CENTER_VERTICAL : Gravity.CENTER);
            placeholder.setSingleLine(false);
            placeholder.setPadding(px(8), 0, px(8), 0);
            cover.addView(placeholder, new FrameLayout.LayoutParams(-1, -1));
            if (!simple) {
                ImageView portrait = new ImageView(getContext());
                portrait.setScaleType(ImageView.ScaleType.CENTER_CROP);
                cover.addView(portrait, new FrameLayout.LayoutParams(-1, -1));
                Artwork.show(portrait, game.cover());
                if (section == 0 && query.isEmpty()) {
                    TextView rank = label("#" + (i + 1), 14, Color.WHITE, true);
                    rank.setGravity(Gravity.CENTER);
                    rank.setBackground(TvUi.shape(0xff6f42ea, px(15), 0, 0));
                    place(cover, rank, 11, 11, 37, 26, false);
                }
                if (game.access().equals("locked")) {
                    LineIcon lock = new LineIcon(getContext(), "lock", TEXT);
                    lock.setBackground(TvUi.shape(SHELL, px(12), 0, 0));
                    place(cover, lock, cardWidth - 35, 9, 26, 26, false);
                }
            }
            cover.setOnFocusChangeListener((view, focused) -> {
                if (focused && !busy) select(index, false);
            });
            cover.setOnClickListener(view -> {
                if (busy) return;
                if (tv || index == selected) actions.play(index);
                else select(index, false);
            });
            LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(px(cardWidth), px(cardHeight));
            if (simple) size.bottomMargin = px(5);
            else size.rightMargin = px(narrow ? 14 : 22);
            track.addView(cover, size);
            covers.add(cover);
            controls.add(cover);
        }
        if (covers.isEmpty()) {
            TextView empty = label(t("Aucun jeu dans cette rubrique."), 24, SECONDARY, false);
            place(this, empty, start, y + 47, width - start * 2, 56, true);
        }
    }

    private void addDetails() {
        float left = narrow ? 24 : 64;
        float y = narrow ? 457 : 495;
        float available = width - left * 2;
        title = label("", narrow ? 26 : 34, TEXT, true);
        place(this, title, left, y, available, narrow ? 42 : 54, true);
        LinearLayout line = new LinearLayout(getContext());
        line.setGravity(Gravity.CENTER_VERTICAL);
        offer = label("", narrow ? 13 : 15, GREEN, true);
        metadata = label("", narrow ? 13 : 15, SECONDARY, false);
        line.addView(offer);
        line.addView(metadata, new LinearLayout.LayoutParams(0, -1, 1));
        place(this, line, left, y + (narrow ? 40 : 51), available, 32, true);

        float controlsY = narrow ? 548 : 594;
        play = pill("▶   " + t("JOUER"), true, () -> actions.play(selected));
        play.setId(View.generateViewId());
        place(this, play, left, controlsY, narrow ? 165 : 220, 52, true);
        cancel = pill(t("Annuler"), false, actions::cancel);
        controls.remove(cancel);
        place(this, cancel, width - left - 134, controlsY, 134, 52, true);

        int count = content.games.size();
        if (count > PAGE_SIZE) {
            int pages = (count + PAGE_SIZE - 1) / PAGE_SIZE;
            int page = first / PAGE_SIZE;
            float x = left + (narrow ? 176 : 244);
            Button before = pill("‹", false, () -> turnPage(-1));
            before.setContentDescription(t("Page précédente"));
            place(this, before, x, controlsY + 4, 42, 44, true);
            before.setVisibility(page > 0 ? VISIBLE : INVISIBLE);
            TextView index = label((page + 1) + " / " + pages, 15, SECONDARY, true);
            index.setGravity(Gravity.CENTER);
            place(this, index, x + 46, controlsY + 4, 62, 44, true);
            Button after = pill("›", false, () -> turnPage(1));
            after.setContentDescription(t("Page suivante"));
            place(this, after, x + 112, controlsY + 4, 42, 44, true);
            after.setVisibility(page + 1 < pages ? VISIBLE : INVISIBLE);
        }
        for (int i = 0; i < covers.size(); i++) {
            FrameLayout cover = covers.get(i);
            if (!simple && i > 0) cover.setNextFocusLeftId(covers.get(i - 1).getId());
            if (!simple && i + 1 < covers.size()) cover.setNextFocusRightId(covers.get(i + 1).getId());
            if (simple && i > 0) cover.setNextFocusUpId(covers.get(i - 1).getId());
            else if (activeNav != null) cover.setNextFocusUpId(activeNav.getId());
            if (simple && i + 1 < covers.size()) cover.setNextFocusDownId(covers.get(i + 1).getId());
            else cover.setNextFocusDownId(play.getId());
        }
    }

    private void addFooter(float height) {
        if (narrow) return;
        try {
            String version = getContext().getPackageManager().getPackageInfo(getContext().getPackageName(), 0).versionName;
            TextView build = label("V" + version, 13, TEXT, false);
            build.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
            place(this, build, width - 165, height - 38, 130, 30, true);
        } catch (PackageManager.NameNotFoundException ignored) {}
    }

    private void updateSystemStatus() {
        removeCallbacks(tick);
        if (system == null || !isAttachedToWindow()) return;
        String clock = DateFormat.getTimeFormat(getContext()).format(new Date());
        String summary = clock + systemStatus.get();
        if (!content.plan.isEmpty()) summary += "   │   " + content.plan;
        system.setText(summary);
        postDelayed(tick, 30_000);
    }

    private void turnPage(int direction) {
        int next = direction > 0 ? first + PAGE_SIZE : first - 1;
        if (next < 0 || next >= content.games.size() || busy) return;
        selected = next;
        actions.selection(selected);
        render();
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int key = event.getKeyCode();
        boolean firstCover = !covers.isEmpty() && covers.get(0).hasFocus();
        boolean lastCover = !covers.isEmpty() && covers.get(covers.size() - 1).hasFocus();
        if (!busy && event.getAction() == KeyEvent.ACTION_DOWN) {
            if (key == (simple ? KeyEvent.KEYCODE_DPAD_UP : KeyEvent.KEYCODE_DPAD_LEFT) && firstCover && first > 0) {
                turnPage(-1);
                return true;
            }
            if (key == (simple ? KeyEvent.KEYCODE_DPAD_DOWN : KeyEvent.KEYCODE_DPAD_RIGHT) && lastCover && first + covers.size() < content.games.size()) {
                turnPage(1);
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    private void select(int index, boolean scrollImmediately) {
        if (index == selected && !scrollImmediately) return;
        selected = index;
        actions.selection(index);
        refreshSelection(scrollImmediately);
    }

    private void refreshSelection(boolean immediate) {
        if (covers.isEmpty()) {
            play.setVisibility(GONE);
            return;
        }
        Content.Game game = content.games.get(selected);
        title.setText(game.title());
        offer.setText(game.offer());
        offer.setTextColor(game.access().equals("included") ? GREEN : VIOLET);
        offer.setVisibility(game.offer().isEmpty() ? GONE : VISIBLE);
        metadata.setPadding(game.offer().isEmpty() ? 0 : px(14), 0, 0, 0);
        metadata.setText(game.metadata());
        play.setText((game.access().equals("locked") ? "" : "▶   ") + t("JOUER"));
        if (game.access().equals("locked")) play.setContentDescription(t("Nécessite Luna Premium"));
        else play.setContentDescription(t("Jouer"));
        for (int i = 0; i < covers.size(); i++) updateCover(covers.get(i), first + i == selected);
        int selection = selected;
        FrameLayout current = covers.get(selected - first);
        if (simple) listScroll.post(() -> {
            if (selection == selected && current.isAttachedToWindow())
                current.requestRectangleOnScreen(new android.graphics.Rect(0, 0, current.getWidth(), current.getHeight()), immediate);
        });
        else strip.post(() -> {
            if (selection != selected || !current.isAttachedToWindow()) return;
            int target = Math.max(0, current.getLeft() - px(narrow ? 24 : 104));
            if (immediate) strip.scrollTo(target, 0);
            else strip.smoothScrollTo(target, 0);
        });
        play.setNextFocusUpId(current.getId());
        if (activeNav != null) activeNav.setNextFocusDownId(current.getId());
        if (immediate && tv && !busy) focusSelected();
        removeCallbacks(delayedBackdrop);
        postDelayed(delayedBackdrop, immediate ? 0 : 130);
    }

    private void loadBackdrop() {
        if (!content.games.isEmpty())
            Artwork.showBackdrop(backdrop, content.games.get(selected).backdrop());
    }

    private void focusSelected() {
        if (covers.isEmpty()) {
            if (activeNav != null) activeNav.requestFocus();
        } else {
            covers.get(selected - first).requestFocus();
        }
    }

    private void updateCover(FrameLayout cover, boolean selected) {
        cover.setBackground(TvUi.shape(0xff2a2240, px(17), 0, 0));
        cover.setForeground(TvUi.shape(Color.TRANSPARENT, px(17), px(selected ? 3 : 1),
            selected ? VIOLET : 0x8dd8cce8));
        cover.setScaleX(selected && !simple ? 1.055f : 1);
        cover.setScaleY(selected && !simple ? 1.055f : 1);
        cover.setTranslationZ(px(selected ? 8 : 2));
    }

    private void navigation(String icon, String text, boolean selected, float x, float y, float w,
                            Runnable action, String avatar) {
        FrameLayout box = new FrameLayout(getContext());
        box.setFocusable(true);
        box.setClickable(true);
        box.setId(View.generateViewId());
        box.setContentDescription(text);
        box.setOnClickListener(view -> action.run());
        float diameter = narrow ? 42 : 47;
        FrameLayout disc = new FrameLayout(getContext());
        disc.setClipToOutline(true);
        place(box, disc, (w - diameter) / 2, 0, diameter, diameter, false);
        LineIcon glyph = new LineIcon(getContext(), icon, TEXT);
        place(disc, glyph, diameter / 4, diameter / 4, diameter / 2, diameter / 2, false);
        if (!avatar.isEmpty()) {
            ImageView photo = new ImageView(getContext());
            photo.setScaleType(ImageView.ScaleType.CENTER_CROP);
            disc.addView(photo, new FrameLayout.LayoutParams(-1, -1));
            Artwork.show(photo, avatar);
        }
        TextView caption = label(text, narrow ? 11 : 14, TEXT, true);
        caption.setGravity(Gravity.CENTER);
        place(box, caption, 0, diameter + 3, w, 21, false);
        Runnable state = () -> {
            boolean marked = selected || box.hasFocus();
            disc.setBackground(TvUi.shape(SHELL, px(diameter / 2), px(marked ? 2 : 1),
                marked ? VIOLET : 0x867d718e));
            disc.setForeground(TvUi.shape(Color.TRANSPARENT, px(diameter / 2), px(marked ? 2 : 1),
                marked ? VIOLET : 0x867d718e));
            caption.setTextColor(marked ? VIOLET : TEXT);
        };
        box.setOnFocusChangeListener((view, focused) -> state.run());
        state.run();
        place(this, box, x, y, w, diameter + 25, true);
        if (selected) activeNav = box;
        controls.add(box);
    }

    private Button pill(String title, boolean primary, Runnable action) {
        Button button = new Button(getContext());
        button.setText(title);
        button.setAllCaps(false);
        button.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        button.setTextSize(TypedValue.COMPLEX_UNIT_PX, px(primary ? 20 : 17));
        button.setPadding(0, 0, 0, 0);
        button.setMinWidth(0);
        button.setMinHeight(0);
        button.setMinimumWidth(0);
        button.setMinimumHeight(0);
        button.setTextColor(primary ? INK : TEXT);
        button.setFocusable(true);
        button.setStateListAnimator(null);
        Runnable decorate = () -> button.setBackground(TvUi.shape(primary ? 0xfff0eaff : SHELL, px(26),
            px(button.hasFocus() ? 3 : 0), VIOLET));
        button.setOnFocusChangeListener((view, focused) -> decorate.run());
        button.setOnClickListener(view -> action.run());
        decorate.run();
        controls.add(button);
        return button;
    }

    private TextView label(String value, float size, int color, boolean bold) {
        TextView view = new TextView(getContext());
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_PX, px(size));
        view.setTextColor(color);
        view.setGravity(Gravity.CENTER_VERTICAL);
        view.setSingleLine(true);
        view.setEllipsize(TextUtils.TruncateAt.END);
        view.setIncludeFontPadding(false);
        if (bold) view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

    private int px(float value) { return Math.round(value * scale); }

    private void place(FrameLayout parent, View view, float x, float y, float w, float h, boolean root) {
        FrameLayout.LayoutParams frame = new FrameLayout.LayoutParams(Math.max(0, px(w)), Math.max(0, px(h)), Gravity.TOP | Gravity.LEFT);
        frame.leftMargin = Math.round((root ? offsetX : 0) + x * scale);
        frame.topMargin = Math.round((root ? offsetY : 0) + y * scale);
        parent.addView(view, frame);
    }

    private static final class BitmapState {
        final Drawable image;
        final Object key;
        BitmapState(ImageView view) {
            image = view == null ? null : view.getDrawable();
            key = view == null ? null : view.getTag();
        }
    }
}
