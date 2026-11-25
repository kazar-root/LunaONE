// SPDX-License-Identifier: GPL-3.0-only

package app.lunacore.android;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Hands the downloaded update APK to Android's package installer. Android
 * accepts only a content:// address for it, and this provider serves that one
 * file, read-only, to the app that LunaONE grants it to.
 */
public final class UpdateProvider extends ContentProvider {
    static final String AUTHORITY = "app.lunacore.android.update";
    static final String NAME = "LunaONE-update.apk";
    static final String TYPE = "application/vnd.android.package-archive";

    static Uri uri() { return Uri.parse("content://" + AUTHORITY + "/" + NAME); }

    static File file(Context context) { return new File(new File(context.getCacheDir(), "updates"), NAME); }

    @Override public boolean onCreate() { return true; }

    @Override public String getType(Uri uri) { return NAME.equals(uri.getLastPathSegment()) ? TYPE : null; }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!NAME.equals(uri.getLastPathSegment()) || !"r".equals(mode)) throw new FileNotFoundException();
        return ParcelFileDescriptor.open(file(getContext()), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    // The installer asks for the file's name and size.
    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] arguments, String order) {
        if (!NAME.equals(uri.getLastPathSegment())) return null;
        String[] columns = projection != null ? projection : new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) row[i] = NAME;
            else if (OpenableColumns.SIZE.equals(columns[i])) row[i] = file(getContext()).length();
        }
        MatrixCursor cursor = new MatrixCursor(columns, 1);
        cursor.addRow(row);
        return cursor;
    }

    @Override public Uri insert(Uri uri, ContentValues values) { return null; }

    @Override public int delete(Uri uri, String selection, String[] arguments) { return 0; }

    @Override public int update(Uri uri, ContentValues values, String selection, String[] arguments) { return 0; }
}
