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

    // Dark buttons for the sign-in page: violet for the main action, frosted
    // glass for the others, and a bright ring when the remote focuses one.
    static void button(Button view, boolean primary, float density) {
        view.setMinHeight(Math.round(50 * density));
        view.setTextColor(Color.WHITE);
        view.setBackgroundTintList(null);
        view.setStateListAnimator(null);
        int horizontal = Math.round(14 * density);
        int vertical = Math.round(4 * density);
        view.setPadding(horizontal, vertical, horizontal, vertical);
        int radius = Math.round(12 * density);
        int thin = Math.max(1, Math.round(density));
        int ring = Math.round(3 * density);
        StateListDrawable background = new StateListDrawable();
        background.addState(new int[]{-android.R.attr.state_enabled},
            shape(primary ? 0x806a4cff : 0x0dffffff, radius, primary ? 0 : thin, 0x26ffffff));
        background.addState(new int[]{android.R.attr.state_focused},
            shape(primary ? 0xff8466ff : 0x55aa88ff, radius, ring, primary ? Color.WHITE : 0xffaa88ff));
        background.addState(new int[]{android.R.attr.state_pressed},
            shape(primary ? 0xff8466ff : 0x55aa88ff, radius, primary ? 0 : thin, 0x4dffffff));
        background.addState(new int[]{},
            shape(primary ? 0xff6a4cff : 0x1affffff, radius, primary ? 0 : thin, 0x4dffffff));
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
