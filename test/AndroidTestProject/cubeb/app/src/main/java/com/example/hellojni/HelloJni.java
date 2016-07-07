/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.example.hellojni;

import android.app.Activity;
import android.content.res.AssetManager;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.os.Bundle;


public class HelloJni extends Activity
{
    static AssetManager assetManager;

    /** Called when the activity is first created. */
    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);

        // creating LinearLayout
        LinearLayout linLayout = new LinearLayout(this);
        // specifying vertical orientation
        linLayout.setOrientation(LinearLayout.VERTICAL);
        // creating LayoutParams
        ViewGroup.LayoutParams linLayoutParam = new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        // set LinearLayout as a root element of the screen
        setContentView(linLayout, linLayoutParam);

        /* Create a TextView and set its content.
         * the text is retrieved by calling a native
         * function.
         */
        TextView  tv = new TextView(this);
        tv.setText( stringFromJNI() );
        linLayout.addView(tv);

        // Create init cubeb button
        final Button init_client_bt = new Button(this);
        init_client_bt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // Perform action on click
                initClient();
            }
        });
        init_client_bt.setText("Init cubeb");
        linLayout.addView(init_client_bt);

        // Create start capture button
        final Button start_read_bt = new Button(this);
        start_read_bt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // Perform action on click
                startCapture();
            }
        });
        start_read_bt.setText("Start Record");
        linLayout.addView(start_read_bt);


        // Create start playback button
        final Button start_write_bt = new Button(this);
        start_write_bt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // Perform action on click
                startPlayback();
            }
        });
        start_write_bt.setText("Start Playback");
        linLayout.addView(start_write_bt);

        // Create start full duplex button
        final Button start_fd_bt = new Button(this);
        start_fd_bt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // Perform action on click
                startFullDuplex();
            }
        });
        start_fd_bt.setText("Start Full Duplex");
        linLayout.addView(start_fd_bt);

        // Create stop stream button
        final Button stop_stream_bt = new Button(this);
        stop_stream_bt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // Perform action on click
                stopCubebStream();
            }
        });
        stop_stream_bt.setText("Stop Stream");
        linLayout.addView(stop_stream_bt);

        // Create destroy stream button
        final Button destroy_stream_bt = new Button(this);
        destroy_stream_bt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // Perform action on click
                destroyCubebStream();
            }
        });
        destroy_stream_bt.setText("Destroy Stream");
        linLayout.addView(destroy_stream_bt);

        // Create stop and destrot stream Button
        final Button stop_cubeb_bt = new Button(this);
        stop_cubeb_bt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // Perform action on click
                stopCubeb();
            }
        });
        stop_cubeb_bt.setText("Stop & Destroy Stream");
        linLayout.addView(stop_cubeb_bt);

        final Button destory_cubeb_bt = new Button(this);
        destory_cubeb_bt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // Perform action on click
                destroyClient();
            }
        });
        destory_cubeb_bt.setText("Destroy cubeb");
        linLayout.addView(destory_cubeb_bt);

        final Button test_audio_bt = new Button(this);
        test_audio_bt.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                // Perform action on click
                cubebTestAll();
            }
        });
        test_audio_bt.setText("Cubeb Tests");
        linLayout.addView(test_audio_bt);

    }

    @Override
    protected void onDestroy() {
        //destroyClient();
        super.onDestroy();
    }

    /* A native method that is implemented by the
     * 'hello-jni' native library, which is packaged
     * with this application.
     */
    public native String  stringFromJNI();

    public static native void initClient();
    public static native void startCapture();
    public static native void startPlayback();
    public static native void startFullDuplex();
    public static native void stopCubebStream();
    public static native void destroyCubebStream();
    public static native void stopCubeb();
    public static native void destroyClient();
    public static native void cubebTestAll();

    static {
        System.loadLibrary("hello-jni");
    }
}
