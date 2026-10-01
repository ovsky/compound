# Pound — R8 / ProGuard rules
#
# `isMinifyEnabled` is false for both build types today, so nothing here is
# currently applied. The rules are kept because turning R8 on is the obvious
# next step for a release APK, and getting it wrong fails at runtime rather
# than at build time.

# ---------------------------------------------------------------------------
# SDL's JNI layer
# ---------------------------------------------------------------------------
#
# SDLActivity is the framework entry point: the manifest names it, and SDL's
# native code calls it by name through JNI. R8 cannot see either reference.

-keep class org.libsdl.app.SDLActivity { *; }
-keep class org.libsdl.app.SDLSurface { *; }

# Every native method in the SDL Java layer is resolved by dlsym at runtime.
-keepclasseswithmembernames class org.libsdl.app.** {
    native <methods>;
}

# SDL's HIDAPI support is driven entirely from Java and native code that
# reflect over these types.
-keep class org.libsdl.app.HIDDevice { *; }
-keep class org.libsdl.app.HIDDeviceManager { *; }
-keep class org.libsdl.app.SDLControllerManager { *; }
-keep class org.libsdl.app.SDLAudioManager { *; }
-keep class org.libsdl.app.SDLInputConnection { *; }
-keep class org.libsdl.app.SDLGenericMotionListener_API14 { *; }
-keep class org.libsdl.app.SDLGenericMotionListener_API26 { *; }

# ---------------------------------------------------------------------------
# Pound's own activity
# ---------------------------------------------------------------------------
#
# Instantiated reflectively by the framework from the manifest.

-keep class dev.pound.emulator.MainActivity { *; }

# Line numbers make a release crash report actionable; the source file name
# adds nothing once the mapping file is uploaded.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile