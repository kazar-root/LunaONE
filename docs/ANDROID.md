# LunaONE Android and Google TV

The app targets ARM64 and 32-bit ARM devices running Android 8.0 or later. It uses the same C++ Amazon registration, catalog, search and WebRTC session code as the desktop clients. Android provides HTTPS, H.264 decoding to a SurfaceView and audio output. A paired or wired Android gamepad uses the existing Luna input encoder.

## Build in VS Code

Install JDK 17, CMake 3.25 or later, Ninja and the Android SDK. Use SDK Platform 35, Build Tools 35.0.0, NDK 28.2.13676358 and Platform Tools for ADB. Set `JAVA_HOME` and `ANDROID_HOME` to those installations.

```sh
app/android/build-apk.sh
```

The script builds and signs `build/android/LunaONE-Android-GoogleTV.apk` with a local development key. It packages both `arm64-v8a` and `armeabi-v7a` and verifies the APK signature and alignment before producing the file.

Set `LUNACORE_ANDROID_NATIVE_DIR` to a local CMake build directory if the workspace is in a synced folder. The signed APK still goes to `build/android`.

## Test on an Android device

1. Install the APK and open **LunaONE**.
2. Choose the Amazon country, create a code and scan the QR to open the official Amazon page.
3. Return to the catalog, search for **Sonic** and tap its cover to launch.
4. Connect a gamepad and check movement, audio and video for at least 30 seconds.
5. Tap **Quitter** or press Back and check that the catalog returns and the next session can start.

## Test on Google TV

1. Install the same APK and open **LunaONE** from the TV launcher.
2. Use the remote's directional pad to select **Créer un code**. Scan the QR code displayed on the TV with a phone, then enter the Amazon code.
3. Navigate the cover carousel with the directional pad. Press OK on a title or move right to **Jouer**. The **Rechercher** field uses the TV keyboard.
4. Check that Sonic plays with video and audio and responds to the controller. Press Back or select **Quitter**. The previous catalog selection should regain focus.

The TV layout can also be opened on a connected Android test instance through its launcher alias.

```sh
adb shell am start -a android.intent.action.MAIN \
  -c android.intent.category.LEANBACK_LAUNCHER -n app.lunacore.android/.TvLauncher
```

## LunaCore validation on 2026-11-19

The ARM64 native library built with NDK r28c. The APK signature, Java-to-native entry points and 16 KB alignment were verified. A JVM smoke test confirmed that the Android HTTPS client rejects cleartext and URLs containing user information. The nine macOS regression tests passed.

The user confirmed that LunaCore opened and launched a game in the local ARM64 BlueStacks instance running Android 13. Playback on a physical Android device, sustained performance and Bluetooth or USB controller input still need dedicated tests.

Google TV 0.2 compiled for both ARM ABIs and uses the same signing certificate as Android 0.1. The APK signature, ZIP alignment, launcher alias, banner and icon were verified.

The 0.3 APK (version code 12) compiled for both ARM ABIs with the redesigned sign-in screen, and keeps the certificate of 0.1 and 0.2, so it installs over them.

The APK installed through ADB on a physical Smart TV Pro running Android 14 with an `armeabi-v7a` userspace. The TV launcher resolved `TvLauncher`, the app cold-started, and the initial focus landed on **Créer un code**. Amazon accepted device registration, and the TV loaded Luna catalog pages.

A first live session decoded 1,512 video frames and received 1,565 audio packets. Luna accepted the session termination. The second Sonic session decoded 16,586 frames, received 14,276 audio packets and recorded 932 changes in Xbox controller state. The user confirmed picture, sound, controller response and return to the catalog with the TV remote.

The user also confirmed a successful play test of Star Wars Racer on the Google TV.

## Sign-in screen on a TV

On a TV, the sign-in screen is laid out on a 960 × 540 dp page that `FitFrame` scales to fit, centred, inside the TV's safe area: 5 % of each edge stays clear, as Android TV recommends for sets that crop their edges (overscan). The page keeps its proportions whatever the TV's resolution, density or font size, and no longer leaves the picture on the sides. It uses the Mac's dark violet design: the form on the left, the three sign-in steps on the right, replaced by the QR code and the Amazon code once one is created. On a phone, the same cards are stacked and the page scrolls when it is taller than the screen. The layout still needs a look on a physical TV.
