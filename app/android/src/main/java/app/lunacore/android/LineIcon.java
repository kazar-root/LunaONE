// SPDX-License-Identifier: GPL-3.0-only
package app.lunacore.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;

// The line icons of the library and settings screens, drawn in a 24 × 24 grid.
final class LineIcon extends View {
    private final String type;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    LineIcon(Context context, String type, int color) {
        super(context);
        this.type = type;
        paint.setColor(color);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(1.85f);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.save();
        float side = Math.min(getWidth(), getHeight());
        canvas.translate((getWidth() - side) / 2, (getHeight() - side) / 2);
        canvas.scale(side / 24, side / 24);
        switch (type) {
            case "clock" -> {
                canvas.drawCircle(12, 12, 8, paint);
                line(canvas, 12, 7, 12, 12, 15, 13);
            }
            case "search" -> {
                canvas.drawCircle(10, 10, 5.8f, paint);
                canvas.drawLine(14.5f, 14.5f, 20, 20, paint);
            }
            case "grid" -> {
                for (int x : new int[]{4, 13}) for (int y : new int[]{4, 13})
                    canvas.drawRoundRect(x, y, x + 7, y + 7, 2, 2, paint);
            }
            case "trophy" -> {
                canvas.drawRoundRect(8, 4, 16, 14, 2, 2, paint);
                line(canvas, 8, 6, 4, 6, 5, 10, 8, 11);
                line(canvas, 16, 6, 20, 6, 19, 10, 16, 11);
                line(canvas, 12, 14, 12, 20);
                canvas.drawLine(8, 20, 16, 20, paint);
            }
            case "account" -> {
                canvas.drawCircle(12, 8, 3.5f, paint);
                canvas.drawArc(new RectF(5, 13, 19, 25), 186, 168, false, paint);
            }
            case "settings" -> {
                canvas.drawCircle(12, 12, 6, paint);
                canvas.drawCircle(12, 12, 2.1f, paint);
                for (int i = 0; i < 8; i++) {
                    canvas.drawLine(12, 3, 12, 6, paint);
                    canvas.rotate(45, 12, 12);
                }
            }
            case "lock" -> {
                canvas.drawRoundRect(6, 10, 18, 20, 2, 2, paint);
                canvas.drawArc(new RectF(8, 4, 16, 14), 180, 180, false, paint);
            }
            case "gamepad" -> {
                canvas.drawRoundRect(3, 7, 21, 18, 5, 5, paint);
                canvas.drawLine(7, 12.5f, 10, 12.5f, paint);
                canvas.drawLine(8.5f, 11, 8.5f, 14, paint);
                canvas.drawCircle(15.5f, 11.5f, .7f, paint);
                canvas.drawCircle(17.5f, 13.5f, .7f, paint);
            }
            case "display" -> {
                canvas.drawRoundRect(3, 5, 21, 16, 2, 2, paint);
                canvas.drawLine(12, 16, 12, 19, paint);
                canvas.drawLine(8, 19.5f, 16, 19.5f, paint);
            }
            case "info" -> {
                canvas.drawCircle(12, 12, 8.5f, paint);
                canvas.drawLine(12, 11, 12, 16.5f, paint);
                canvas.drawCircle(12, 7.8f, .5f, paint);
            }
        }
        canvas.restore();
    }

    private void line(Canvas canvas, float... points) {
        Path path = new Path();
        path.moveTo(points[0], points[1]);
        for (int i = 2; i < points.length; i += 2) path.lineTo(points[i], points[i + 1]);
        canvas.drawPath(path, paint);
    }
}
