// SPDX-License-Identifier: GPL-3.0-only
package app.lunacore.android;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.LruCache;
import android.widget.ImageView;

import java.net.URL;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

final class Artwork {
    private static final LruCache<String, Bitmap> CACHE = new LruCache<>(24 * 1024 * 1024) {
        @Override protected int sizeOf(String url, Bitmap image) { return image.getAllocationByteCount(); }
    };
    private static final ThreadPoolExecutor LOADER = new ThreadPoolExecutor(2, 2, 0, TimeUnit.SECONDS,
        new ArrayBlockingQueue<>(24), new ThreadPoolExecutor.DiscardOldestPolicy());
    private static final ThreadPoolExecutor BACKDROPS = new ThreadPoolExecutor(1, 1, 0, TimeUnit.SECONDS,
        new ArrayBlockingQueue<>(1), new ThreadPoolExecutor.DiscardOldestPolicy());

    static void show(ImageView image, String address) {
        show(image, address, 960, false, LOADER);
    }

    static void showBackdrop(ImageView image, String address) {
        show(image, address, 1280, true, BACKDROPS);
    }

    private static void show(ImageView image, String address, int maximum, boolean keepPrevious, ThreadPoolExecutor executor) {
        String key = maximum + ":" + address;
        if (key.equals(image.getTag())) return;
        image.setTag(key);
        if (!keepPrevious || address.isEmpty()) image.setImageDrawable(null);
        if (address.isEmpty()) return;
        Bitmap cached = CACHE.get(key);
        if (cached != null) {
            image.setImageBitmap(cached);
            return;
        }
        executor.execute(() -> {
            try {
                URL url = new URL(address);
                String host = url.getHost().toLowerCase(Locale.ROOT);
                if (!host.endsWith(".media-amazon.com") && !host.endsWith(".images-amazon.com")) return;
                byte[][] response = AndroidHttp.request("GET", address, "LunaONE", new String[0], new byte[0], 5000, 8000);
                byte[] bytes = response[1];
                BitmapFactory.Options options = new BitmapFactory.Options();
                options.inJustDecodeBounds = true;
                BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
                if (options.outWidth < 1 || options.outHeight < 1) return;
                options.inJustDecodeBounds = false;
                options.inSampleSize = 1;
                while (Math.max(options.outWidth, options.outHeight) / options.inSampleSize > maximum)
                    options.inSampleSize *= 2;
                Bitmap bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
                if (bitmap == null) return;
                CACHE.put(key, bitmap);
                image.post(() -> {
                    if (key.equals(image.getTag())) image.setImageBitmap(bitmap);
                });
            } catch (Exception ignored) {
                // Keep the title visible if artwork is temporarily unavailable.
            }
        });
    }

    private Artwork() {}
}
