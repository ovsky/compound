package dev.pound.emulator

import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.WindowManager
import org.libsdl.app.SDLActivity

/**
 * Android host activity for the Pound emulator.
 *
 * SDL's Java runtime drives the native side: it dlopen()s `libmain.so` and
 * calls the exported `SDL_main`, which `src/main.c` produces by renaming its
 * `main` through `<SDL3/SDL_main.h>`. Everything below is therefore either an
 * SDL contract override or mobile-window housekeeping; the emulator itself does
 * not know it is running on Android.
 */
class MainActivity : SDLActivity() {

    override fun getMainSharedObject(): String = MAIN_SHARED_OBJECT

    override fun getMainFunction(): String = MAIN_FUNCTION

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            // Without this the surface is letterboxed around the camera cutout
            // and SDL is told a size the window never actually has.
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }

        // The emulator runs for long stretches without touch input; letting the
        // display time out would suspend the process mid-session.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        applyImmersiveMode()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)

        // Bars shown while unfocused (a notification shade pull, a system
        // dialog) are not retracted on their own when focus returns.
        if (hasFocus) {
            applyImmersiveMode()
        }
    }

    /**
     * Records the intent that started or re-targeted the activity.
     *
     * Deliberately does not hand the data to the native side: Pound has no
     * "open this" native entry point yet, and guessing one would produce an
     * emulator that silently ignores the request. SDLActivity still needs
     * `setIntent` called so its own `getIntent()` stays current.
     */
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
    }

    /**
     * Hides the status and navigation bars.
     *
     * `IMMERSIVE_STICKY` rather than plain `IMMERSIVE_FULLSCREEN` so a swipe
     * reveals the bars transiently and they retract again on their own, which
     * is the behaviour a handheld user expects from a fullscreen emulator.
     */
    @Suppress("DEPRECATION")
    private fun applyImmersiveMode() {
        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
            View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
    }

    private companion object {
        /**
         * Must match the `OUTPUT_NAME` of the Pound target in the top-level
         * CMakeLists.txt for Android builds; SDL dlopen()s this name.
         */
        const val MAIN_SHARED_OBJECT = "main"

        /**
         * Must match the symbol `src/main.c` produces by including
         * `<SDL3/SDL_main.h>`.
         */
        const val MAIN_FUNCTION = "SDL_main"
    }
}