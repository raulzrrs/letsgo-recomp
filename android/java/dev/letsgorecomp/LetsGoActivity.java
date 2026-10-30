package dev.letsgorecomp;

import android.os.Build;
import android.os.Bundle;
import android.system.Os;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

// The app's activity: SDL's, which loads libSDL3.so and libmain.so (the runtime) and runs its SDL_main. The runtime
// finds liblasgame.so (the game code) next to libmain.so and its data in the app's external files folder.
// For testing from a computer (docs/ANDROID.md):
//   adb shell am start -n dev.letsgorecomp/.LetsGoActivity --es args "--play" --es env "LARECOMP_FONT=...;LARECOMP_X=1"
// args: the runtime's command line (--play: straight into the game); env: its environment (the LARECOMP_* switches).
public class LetsGoActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        String env = getIntent() != null ? getIntent().getStringExtra("env") : null;
        if (env != null)
            for (String kv : env.split(";")) {
                int eq = kv.indexOf('=');
                if (eq > 0) {
                    try { Os.setenv(kv.substring(0, eq).trim(), kv.substring(eq + 1), true); } catch (Exception e) { }
                }
            }
        super.onCreate(savedInstanceState);
        // the whole screen, the camera's cutout included (in landscape it's on a short edge: the image's black side bars)
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
    }

    @Override
    protected String[] getArguments() {
        String a = getIntent() != null ? getIntent().getStringExtra("args") : null;
        return a != null && !a.trim().isEmpty() ? a.trim().split("\\s+") : new String[0];
    }
}
