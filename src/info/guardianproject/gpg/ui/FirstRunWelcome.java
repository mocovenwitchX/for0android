package info.guardianproject.gpg.ui;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.SharedPreferences.Editor;
import android.os.Bundle;
import android.preference.PreferenceManager;
import android.view.View;
import android.view.View.OnClickListener;
import android.widget.Button;

import info.guardianproject.gpg.R;

public class FirstRunWelcome extends Activity {

    public final static String PREFS_SHOW_WIZARD = "show_wizard";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.first_run_welcome_activity);

        Button next = (Button) findViewById(R.id.nextButton);
        next.setOnClickListener(new OnClickListener() {

            @Override
            public void onClick(View v) {
                // since they clicked a button, we know for sure
                // this wizard was run and at least seen
                SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(FirstRunWelcome.this);
                Editor prefsEditor = prefs.edit();
                prefsEditor.putBoolean(FirstRunWelcome.PREFS_SHOW_WIZARD, false);
                prefsEditor.commit();

                startActivity(new Intent(FirstRunWelcome.this, FirstRunSetup.class));
            }
        });
    }

}
