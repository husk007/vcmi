package eu.vcmi.vcmi;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;

/**
 * Runs in its own process, ends the game process and starts the game again.
 * Used after a LAN mod preset change: mods are loaded only when the game process starts.
 */
public class ActivityLanRelaunch extends Activity
{
    public static final String EXTRA_GAME_PID = "eu.vcmi.vcmi.gamePid";
    public static final String EXTRA_TOKEN = "eu.vcmi.vcmi.lanRelaunchToken";

    // gives the game process time to finish writing its files before it is ended
    private static final long GAME_EXIT_DELAY_MS = 1500;

    @Override
    protected void onCreate(final Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);
        final int gamePid = getIntent().getIntExtra(EXTRA_GAME_PID, -1);
        final String token = getIntent().getStringExtra(EXTRA_TOKEN);

        new Handler(Looper.getMainLooper()).postDelayed(() ->
        {
            if (gamePid > 0 && gamePid != Process.myPid())
                Process.killProcess(gamePid);

            final Intent game = new Intent(this, VcmiSDLActivity.class);
            game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            if (token != null && !token.isEmpty())
                game.putExtra(VcmiSDLActivity.EXTRA_LAN_RELAUNCH_TOKEN, token);
            startActivity(game);
            finish();
            Runtime.getRuntime().exit(0);
        }, GAME_EXIT_DELAY_MS);
    }
}
