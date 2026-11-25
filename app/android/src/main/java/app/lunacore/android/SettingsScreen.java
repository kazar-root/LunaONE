// SPDX-License-Identifier: GPL-3.0-only
package app.lunacore.android;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.function.Function;
import java.util.function.Supplier;

// The settings screen of every LunaONE client: categories on the left, the
// rows of the chosen one on the right. A no/yes or off/on choice is a switch;
// another opens its values in the right pane. Left or Back returns one level.
final class SettingsScreen extends FrameLayout {
    interface Actions {
        // Applies a value of a choice row (the shared PlaybackChoices order).
        void apply(int choice, int value);
        void checkUpdates();
        void signOut();
        void close();
    }

    // Data read again at each render: choices, shortcut notes, account lines, about lines.
    interface Source {
        String[] choices();
        String[] notes();
        String[] account();
        String[] about();
        boolean signedIn();
    }

    private record Choice(String section, String title, int selected, String[] values) {}
    // kind: 0 choice with a list of values, 1 switch, 2 action, 3 note.
    private record Entry(String title, String value, int kind, int choice, boolean on, Runnable action) {}
    private record Category(String title, String icon, ArrayList<Entry> rows) {}

    private static final int TEXT = 0xfffaf8ff;
    private static final int SECONDARY = 0xffc7bdd4;
    private static final int VIOLET = 0xffaa88ff;
    private static final int RING = 0x867d718e;
    private static final int SHELL = 0xe818142b;

    private final Function<String, String> translate;
    private final Source source;
    private final Actions actions;
    private final boolean tv;
    private int category, column, row, value, editing = -1, first;
    // Pane entrance: 1 deeper, 2 back, 3 another category.
    private int motion = 3;
    private boolean touched;
    private float scale, width, offsetY;
    private final Runnable rebuild = this::render;

    SettingsScreen(Activity activity, boolean tv, Function<String, String> translate, Source source, Actions actions) {
        super(activity);
        this.tv = tv;
        this.translate = translate;
        this.source = source;
        this.actions = actions;
        setFocusable(true);
        setFocusableInTouchMode(true);
        setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{0xff1a1530, 0xff0b0914}));
        setClickable(true);
    }

    private String t(String key) { return translate.apply(key); }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        removeCallbacks(rebuild);
        post(rebuild);
    }

    // Re-reads the data after a change made elsewhere (language, sign-out).
    void refresh() {
        removeCallbacks(rebuild);
        post(rebuild);
    }

    private ArrayList<Category> categories() {
        String[] data = source.choices();
        ArrayList<Choice> choices = new ArrayList<>();
        for (int i = 0; i < data.length;) {
            String section = data[i++];
            String title = data[i++];
            int selected = Integer.parseInt(data[i++]);
            int size = Integer.parseInt(data[i++]);
            String[] values = java.util.Arrays.copyOfRange(data, i, i + size);
            i += size;
            choices.add(new Choice(section, title, selected, values));
        }
        ArrayList<Category> result = new ArrayList<>();
        String[] icons = {"settings", "gamepad", "display"};
        for (int index = 0; index < choices.size(); index++) {
            Choice choice = choices.get(index);
            if (result.isEmpty() || !result.get(result.size() - 1).title().equals(choice.section())) {
                // The controller's shortcuts close its category.
                if (result.size() == 2) for (String note : source.notes()) result.get(1).rows().add(note(note));
                result.add(new Category(choice.section(), icons[Math.min(result.size(), icons.length - 1)], new ArrayList<>()));
            }
            boolean binary = choice.values().length == 2 &&
                ((choice.values()[0].equals(t("Non")) && choice.values()[1].equals(t("Oui"))) ||
                 (choice.values()[0].equals(t("Désactivé")) && choice.values()[1].equals(t("Activé"))));
            result.get(result.size() - 1).rows().add(new Entry(choice.title(), choice.values()[choice.selected()],
                binary ? 1 : 0, index, choice.selected() == 1, null));
        }
        if (source.signedIn()) {
            ArrayList<Entry> rows = new ArrayList<>();
            for (String line : source.account()) if (!line.isEmpty()) rows.add(note(line));
            rows.add(new Entry(t("Se déconnecter"), "", 2, -1, false, actions::signOut));
            result.add(new Category(t("Compte"), "account", rows));
        }
        ArrayList<Entry> about = new ArrayList<>();
        about.add(new Entry(t("Vérifier les mises à jour"), "", 2, -1, false, actions::checkUpdates));
        String[] lines = source.about();
        for (int i = 0; i < lines.length; i++) {
            // The first line, the version, checks for updates too.
            if (i == 0) about.add(new Entry(lines[i], "", 2, -1, false, actions::checkUpdates));
            else about.add(note(lines[i]));
        }
        result.add(new Category(t("À propos"), "info", about));
        return result;
    }

    private static Entry note(String text) { return new Entry(text, "", 3, -1, false, null); }

    private Choice choice(int index) {
        String[] data = source.choices();
        int current = 0;
        for (int i = 0; i < data.length;) {
            String section = data[i++];
            String title = data[i++];
            int selected = Integer.parseInt(data[i++]);
            int size = Integer.parseInt(data[i++]);
            String[] values = java.util.Arrays.copyOfRange(data, i, i + size);
            i += size;
            if (current++ == index) return new Choice(section, title, selected, values);
        }
        return null;
    }

    private ArrayList<Integer> targets(Category of) {
        ArrayList<Integer> targets = new ArrayList<>();
        for (int i = 0; i < of.rows().size(); i++) if (of.rows().get(i).kind() != 3) targets.add(i);
        return targets;
    }

    private void activate(Entry entry, int index) {
        column = 1;
        row = index;
        if (entry.kind() == 2) {
            entry.action().run();
            return;
        }
        if (entry.kind() == 1) {
            actions.apply(entry.choice(), entry.on() ? 0 : 1);
            render();
            return;
        }
        Choice chosen = choice(entry.choice());
        if (chosen == null) return;
        editing = entry.choice();
        value = chosen.selected();
        column = 2;
        motion = 1;
        render();
    }

    // Remote, controller and keyboard. Returns false for keys left to the system.
    boolean handleKey(KeyEvent event) {
        int key = event.getKeyCode();
        boolean back = key == KeyEvent.KEYCODE_BACK || key == KeyEvent.KEYCODE_ESCAPE || key == KeyEvent.KEYCODE_BUTTON_B;
        boolean enter = key == KeyEvent.KEYCODE_DPAD_CENTER || key == KeyEvent.KEYCODE_ENTER ||
            key == KeyEvent.KEYCODE_NUMPAD_ENTER || key == KeyEvent.KEYCODE_BUTTON_A;
        boolean handled = back || enter || key == KeyEvent.KEYCODE_DPAD_UP || key == KeyEvent.KEYCODE_DPAD_DOWN ||
            key == KeyEvent.KEYCODE_DPAD_LEFT || key == KeyEvent.KEYCODE_DPAD_RIGHT || key == KeyEvent.KEYCODE_BUTTON_L1 ||
            key == KeyEvent.KEYCODE_BUTTON_R1 || key == KeyEvent.KEYCODE_BUTTON_START || key == KeyEvent.KEYCODE_MENU;
        if (!handled) return false;
        if (event.getAction() != KeyEvent.ACTION_DOWN) return true;
        touched = false;
        ArrayList<Category> all = categories();
        category = Math.max(0, Math.min(category, all.size() - 1));
        Category current = all.get(category);
        ArrayList<Integer> targets = targets(current);
        if (key == KeyEvent.KEYCODE_BUTTON_START || key == KeyEvent.KEYCODE_MENU) {
            actions.close();
            return true;
        }
        if (key == KeyEvent.KEYCODE_BUTTON_L1 || key == KeyEvent.KEYCODE_BUTTON_R1) {
            changeCategory(category + (key == KeyEvent.KEYCODE_BUTTON_R1 ? 1 : -1), all.size());
            column = 0;
            render();
            return true;
        }
        // Column -1: the « Fermer » button, above the first category.
        if (column == -1) {
            if (enter || back) {
                actions.close();
                return true;
            }
            if (key == KeyEvent.KEYCODE_DPAD_DOWN || key == KeyEvent.KEYCODE_DPAD_LEFT) column = 0;
        } else if (column == 0) {
            if (key == KeyEvent.KEYCODE_DPAD_UP && category == 0) column = -1;
            else if (key == KeyEvent.KEYCODE_DPAD_UP) changeCategory(category - 1, all.size());
            else if (key == KeyEvent.KEYCODE_DPAD_DOWN) changeCategory(category + 1, all.size());
            else if ((key == KeyEvent.KEYCODE_DPAD_RIGHT || enter) && !targets.isEmpty()) {
                column = 1;
                row = targets.get(0);
            } else if (back || key == KeyEvent.KEYCODE_DPAD_LEFT) {
                actions.close();
                return true;
            }
        } else if (column == 1) {
            int at = targets.indexOf(row);
            if (at < 0 && !targets.isEmpty()) row = targets.get(0);
            else if (key == KeyEvent.KEYCODE_DPAD_UP && at > 0) row = targets.get(at - 1);
            else if (key == KeyEvent.KEYCODE_DPAD_DOWN && at + 1 < targets.size()) row = targets.get(at + 1);
            else if (key == KeyEvent.KEYCODE_DPAD_LEFT || back) column = 0;
            else if (enter && row < current.rows().size()) {
                activate(current.rows().get(row), row);
                return true;
            }
        } else {
            Choice chosen = choice(editing);
            int count = chosen == null ? 0 : chosen.values().length;
            if (key == KeyEvent.KEYCODE_DPAD_UP) value = Math.max(0, value - 1);
            else if (key == KeyEvent.KEYCODE_DPAD_DOWN) value = Math.min(count - 1, value + 1);
            else if (key == KeyEvent.KEYCODE_DPAD_LEFT || back) backToRows();
            else if (enter && editing >= 0) {
                int index = editing;
                backToRows();
                actions.apply(index, value);
            }
        }
        render();
        return true;
    }

    private void changeCategory(int index, int count) {
        index = Math.max(0, Math.min(index, count - 1));
        if (index == category) return;
        category = index;
        editing = -1;
        first = 0;
        motion = 3;
    }

    private void backToRows() {
        column = 1;
        editing = -1;
        motion = 2;
    }

    void render() {
        float usableWidth = getWidth(), usableHeight = getHeight();
        if (usableWidth < 1 || usableHeight < 1) return;
        scale = Math.min(usableWidth / 1280, usableHeight / 720);
        width = usableWidth / scale;
        offsetY = (usableHeight - 720 * scale) / 2;
        removeAllViews();
        ArrayList<Category> all = categories();
        category = Math.max(0, Math.min(category, all.size() - 1));
        Category current = all.get(category);
        boolean keys = tv || !touched;

        place(this, label(t("Réglages"), 30, TEXT, true), 64, 32, 500, 50);
        TextView close = label(t("Fermer"), 16, TEXT, true);
        close.setGravity(Gravity.CENTER);
        boolean closeFocused = keys && column == -1;
        close.setBackground(shape(closeFocused ? 0x33aa88ff : SHELL, 23, closeFocused ? 3 : 1, closeFocused ? VIOLET : RING));
        close.setOnClickListener(view -> actions.close());
        place(this, close, width - 64 - 150, 34, 150, 46);

        for (int i = 0; i < all.size(); i++) {
            boolean chosen = i == category, focused = chosen && keys && column == 0;
            FrameLayout item = new FrameLayout(getContext());
            item.setBackground(shape(chosen ? 0x33aa88ff : 0, 14, focused ? 2 : 0, VIOLET));
            if (chosen) {
                View accent = new View(getContext());
                accent.setBackground(shape(VIOLET, 2, 0, 0));
                place(item, accent, 0, 14, 4, 26);
            }
            place(item, new LineIcon(getContext(), all.get(i).icon(), chosen ? VIOLET : TEXT), 20, 15, 24, 24);
            place(item, label(all.get(i).title(), 18, chosen ? TEXT : SECONDARY, chosen), 58, 0, 230, 54);
            int index = i;
            item.setOnClickListener(view -> {
                touched = true;
                if (category != index) motion = 3;
                category = index;
                column = 0;
                editing = -1;
                first = 0;
                render();
            });
            place(this, item, 64, 112 + i * 62, 300, 54);
        }
        View divider = new View(getContext());
        divider.setBackgroundColor(0x26ffffff);
        place(this, divider, 392, 112, 1, 540);

        float paneX = 424, paneWidth = width - paneX - 64, paneHeight = 548;
        FrameLayout pane = new FrameLayout(getContext());
        pane.setClipChildren(true);
        place(this, pane, paneX, 104, paneWidth, paneHeight);
        Choice chosen = editing >= 0 ? choice(editing) : null;
        if (chosen != null) {
            TextView back = label("‹   " + chosen.title(), 22, TEXT, true);
            back.setOnClickListener(view -> {
                touched = true;
                backToRows();
                render();
            });
            place(pane, back, 0, 8, paneWidth, 44);
            int visible = 7, count = chosen.values().length;
            int top = Math.min(value, Math.max(0, Math.min(value - visible + 1, count - visible)));
            top = Math.max(0, top);
            for (int v = top; v < Math.min(count, top + visible); v++) {
                boolean selected = v == chosen.selected(), focused = v == value && keys;
                FrameLayout row = new FrameLayout(getContext());
                row.setBackground(shape(focused ? 0x33aa88ff : 0x12ffffff, 14, focused ? 2 : 0, VIOLET));
                View mark = new View(getContext());
                mark.setBackground(shape(selected ? VIOLET : 0, 10, 2, selected ? VIOLET : RING));
                place(row, mark, 20, 17, 20, 20);
                if (selected) {
                    View dot = new View(getContext());
                    dot.setBackground(shape(Color.WHITE, 4, 0, 0));
                    place(row, dot, 26, 23, 8, 8);
                }
                place(row, label(chosen.values()[v], 18, selected ? TEXT : SECONDARY, selected), 56, 0, paneWidth - 76, 54);
                int index = v, choiceIndex = editing;
                row.setOnClickListener(view -> {
                    touched = true;
                    value = index;
                    backToRows();
                    actions.apply(choiceIndex, index);
                    render();
                });
                place(pane, row, 0, 64 + (v - top) * 62, paneWidth, 54);
            }
        } else {
            place(pane, label(current.title(), 22, TEXT, true), 0, 8, paneWidth, 44);
            ArrayList<Entry> rows = current.rows();
            if (column == 1 && row < rows.size()) {
                first = Math.min(first, row);
                while (first < row && height(rows, first, row) > paneHeight - 64) first++;
            }
            first = Math.max(0, Math.min(first, Math.max(0, rows.size() - 1)));
            float y = 64;
            int shownUntil = first;
            for (int i = first; i < rows.size() && y + (rows.get(i).kind() == 3 ? 40 : 62) - 8 <= paneHeight; i++) {
                shownUntil = i + 1;
                Entry entry = rows.get(i);
                if (entry.kind() == 3) {
                    place(pane, label(entry.title(), 15, SECONDARY, false), 20, y, paneWidth - 40, 40);
                    y += 40;
                    continue;
                }
                boolean focused = keys && column == 1 && i == row;
                FrameLayout line = new FrameLayout(getContext());
                line.setBackground(shape(focused ? 0x33aa88ff : 0x12ffffff, 14, focused ? 2 : 0, VIOLET));
                place(line, label(entry.title(), 18, entry.kind() == 2 ? VIOLET : TEXT, entry.kind() == 2),
                    20, 0, paneWidth * .55f, 54);
                if (entry.kind() == 1) {
                    View track = new View(getContext());
                    track.setBackground(shape(entry.on() ? VIOLET : 0x40ffffff, 15, 0, 0));
                    place(line, track, paneWidth - 76, 12, 54, 30);
                    View knob = new View(getContext());
                    knob.setBackground(shape(Color.WHITE, 12, 0, 0));
                    place(line, knob, paneWidth - 76 + (entry.on() ? 27 : 3), 15, 24, 24);
                } else if (entry.kind() == 0) {
                    TextView current0 = label(entry.value() + "   ›", 15, SECONDARY, false);
                    current0.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
                    place(line, current0, paneWidth * .5f, 0, paneWidth * .5f - 22, 54);
                } else {
                    TextView arrow = label("›", 20, VIOLET, true);
                    arrow.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
                    place(line, arrow, paneWidth - 60, 0, 40, 54);
                }
                int index = i;
                line.setOnClickListener(view -> {
                    touched = true;
                    activate(entry, index);
                });
                place(pane, line, 0, y, paneWidth, 54);
                y += 62;
            }
            if (first > 0) place(pane, label("▲", 12, SECONDARY, false), paneWidth - 24, 20, 20, 20);
            if (shownUntil < rows.size()) place(pane, label("▼", 12, SECONDARY, false), paneWidth / 2 - 10, paneHeight - 22, 20, 20);
        }
        // The right pane slides in: from the right going deeper, from the left coming back.
        if (motion != 0 && android.animation.ValueAnimator.areAnimatorsEnabled()) {
            pane.setAlpha(0);
            pane.setTranslationX(px(motion == 1 ? 36 : motion == 2 ? -36 : 0));
            pane.setTranslationY(px(motion == 3 ? 10 : 0));
            pane.animate().alpha(1).translationX(0).translationY(0).setDuration(220)
                .setInterpolator(new android.view.animation.DecelerateInterpolator()).start();
        }
        motion = 0;
        if (tv) {
            String hint = "OK / A  " + t("Valider") + "    ← / B  " + t("Retour") + "    L1 / R1  " + t("Rubriques");
            place(this, label(hint, 13, SECONDARY, false), 64, 682, width - 128, 30);
        }
        requestFocus();
    }

    private float height(ArrayList<Entry> rows, int from, int to) {
        float total = 0;
        for (int i = from; i <= to; i++) total += rows.get(i).kind() == 3 ? 40 : 62;
        return total;
    }

    private GradientDrawable shape(int color, float radius, float stroke, int strokeColor) {
        return TvUi.shape(color, px(radius), Math.round(stroke * scale), strokeColor);
    }

    private TextView label(String value, float size, int color, boolean bold) {
        TextView view = new TextView(getContext());
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_PX, size * scale);
        view.setTextColor(color);
        view.setGravity(Gravity.CENTER_VERTICAL);
        view.setSingleLine(true);
        view.setEllipsize(TextUtils.TruncateAt.END);
        view.setIncludeFontPadding(false);
        if (bold) view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

    private int px(float value) { return Math.round(value * scale); }

    private void place(FrameLayout parent, View view, float x, float y, float w, float h) {
        LayoutParams frame = new LayoutParams(Math.max(0, px(w)), Math.max(0, px(h)), Gravity.TOP | Gravity.LEFT);
        frame.leftMargin = Math.round((parent == this ? 0 : 0) + x * scale);
        frame.topMargin = Math.round((parent == this ? offsetY : 0) + y * scale);
        parent.addView(view, frame);
    }
}
