// SPDX-License-Identifier: GPL-3.0-only

package app.lunacore.android;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;

/**
 * Lays its one child out at a fixed design size and scales it to fit, centred,
 * inside the TV's safe area. Android TV keeps 5 % of each edge clear because
 * some TVs crop it (overscan). The sign-in screen then stays whole on every
 * TV, whatever its resolution, density, font size or overscan.
 */
final class FitFrame extends ViewGroup {
    private static final float SAFE_MARGIN = 0.05f;
    private final int designWidth;
    private final int designHeight;

    FitFrame(Context context, int designWidth, int designHeight) {
        super(context);
        this.designWidth = designWidth;
        this.designHeight = designHeight;
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        setMeasuredDimension(MeasureSpec.getSize(widthSpec), MeasureSpec.getSize(heightSpec));
        if (getChildCount() > 0)
            getChildAt(0).measure(MeasureSpec.makeMeasureSpec(designWidth, MeasureSpec.EXACTLY),
                                  MeasureSpec.makeMeasureSpec(designHeight, MeasureSpec.EXACTLY));
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        if (getChildCount() == 0)
            return;
        View child = getChildAt(0);
        int width = right - left;
        int height = bottom - top;
        float scale = Math.min(width * (1 - 2 * SAFE_MARGIN) / designWidth,
                               height * (1 - 2 * SAFE_MARGIN) / designHeight);
        child.layout(0, 0, designWidth, designHeight);
        child.setPivotX(0);
        child.setPivotY(0);
        child.setScaleX(scale);
        child.setScaleY(scale);
        child.setTranslationX((width - designWidth * scale) / 2f);
        child.setTranslationY((height - designHeight * scale) / 2f);
    }
}
