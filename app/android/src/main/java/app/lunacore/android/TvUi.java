// SPDX-License-Identifier: GPL-3.0-only

package app.lunacore.android;

import android.app.Activity;
import android.app.UiModeManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.View;
import android.widget.Button;

final class TvUi {
    static final int FOCUS = 0xff5640bb;
    static final int SELECTED = 0xffe3dcff;

    private TvUi() {}

    static boolean enabled(Activity activity) {
        Intent intent = activity.getIntent();
        UiModeManager modes = activity.getSystemService(UiModeManager.class);
        return activity.getPackageManager().hasSystemFeature(PackageManager.FEATURE_LEANBACK) ||
            (modes != null && modes.getCurrentModeType() == Configuration.UI_MODE_TYPE_TELEVISION) ||
            (intent != null && (intent.hasCategory(Intent.CATEGORY_LEANBACK_LAUNCHER) ||
                (intent.getComponent() != null && intent.getComponent().getClassName().endsWith(".TvLauncher"))));
    }

    static void focusable(View control) {
        control.setFocusable(true);
        control.setFocusableInTouchMode(true);
    }

    static void button(Button view, boolean primary, int ink, float density) {
        focusable(view);
        view.setTextSize(17);
        view.setMinHeight(Math.round(50 * density));
        view.setTextColor(primary ? Color.WHITE : ink);
        view.setBackgroundTintList(null);
        view.setStateListAnimator(null);
        int horizontal = Math.round(14 * density);
        int vertical = Math.round(4 * density);
        view.setPadding(horizontal, vertical, horizontal, vertical);
        int radius = Math.round(8 * density);
        StateListDrawable background = new StateListDrawable();
        background.addState(new int[]{-android.R.attr.state_enabled},
            shape(primary ? 0xff9b8fca : 0xffedeaf2, radius, 0, 0));
        background.addState(new int[]{android.R.attr.state_focused},
            shape(primary ? FOCUS : SELECTED, radius, Math.round(3 * density), primary ? 0xffeeebff : FOCUS));
        background.addState(new int[]{android.R.attr.state_pressed},
            shape(primary ? FOCUS : SELECTED, radius, Math.round(2 * density), FOCUS));
        background.addState(new int[]{},
            shape(primary ? 0xff32217d : Color.WHITE, radius, 0, 0));
        view.setBackground(background);
    }

    static GradientDrawable shape(int color, int radius, int strokeWidth, int strokeColor) {
        GradientDrawable background = new GradientDrawable();
        background.setColor(color);
        background.setCornerRadius(radius);
        if (strokeWidth > 0) background.setStroke(strokeWidth, strokeColor);
        return background;
    }
}
