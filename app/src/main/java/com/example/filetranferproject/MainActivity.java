package com.example.filetranferproject;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.net.Uri;
import android.nfc.NfcAdapter;
import android.nfc.cardemulation.CardEmulation;
import android.os.Bundle;
import android.util.Log;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public class MainActivity extends AppCompatActivity {

    private static final String TAG = "MainActivity";
    private static final int FILE_SELECT_CODE = 456;

    private TextView statusText;
    private TextView fileInfoText;
    private Button selectFileButton;
    private NfcAdapter nfcAdapter;
    private File copiedFile;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // Initialize UI components
        statusText = findViewById(R.id.statusText);
        fileInfoText = findViewById(R.id.fileInfoText);
        selectFileButton = findViewById(R.id.selectFileButton);

        // Check if NFC is available
        nfcAdapter = NfcAdapter.getDefaultAdapter(this);
        if (nfcAdapter == null) {
            updateStatus("NFC not available");
            Toast.makeText(this, "NFC not available on this device", Toast.LENGTH_LONG).show();
            return;
        }

        // Check if NFC is enabled
        if (!nfcAdapter.isEnabled()) {
            updateStatus("NFC disabled");
            Toast.makeText(this, "Please enable NFC in settings", Toast.LENGTH_LONG).show();
        } else {
            updateStatus("NFC ready");
            setupDefaultService();
        }

        // Set up file selection
        selectFileButton.setOnClickListener(v -> selectFile());
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (nfcAdapter != null && nfcAdapter.isEnabled()) {
            updateStatus("NFC ready");
            setupDefaultService();
        }
    }

    private void setupDefaultService() {
        // Make this the preferred service while app is in foreground
        try {
            CardEmulation cardEmulation = CardEmulation.getInstance(nfcAdapter);

            // Create a proper ComponentName object for the service
            ComponentName serviceComponent = new ComponentName(getPackageName(),
                    getPackageName() + ".MyHostApduService");

            cardEmulation.setPreferredService(this, serviceComponent);
            Toast.makeText(this, "Service ready", Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Toast.makeText(this, "Failed to set service: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private void selectFile() {
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType("*/*");
        intent.addCategory(Intent.CATEGORY_OPENABLE);

        try {
            startActivityForResult(Intent.createChooser(intent, "Select a file"), FILE_SELECT_CODE);
        } catch (Exception ex) {
            Toast.makeText(this, "File manager not found", Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == FILE_SELECT_CODE && resultCode == Activity.RESULT_OK && data != null) {
            Uri selectedFileUri = data.getData();

            try {
                // Copy file to internal storage
                copiedFile = new File(getFilesDir(), "file_to_send.bin");

                try (InputStream in = getContentResolver().openInputStream(selectedFileUri);
                     FileOutputStream out = new FileOutputStream(copiedFile)) {

                    if (in == null) {
                        throw new Exception("Failed to open input stream");
                    }

                    byte[] buffer = new byte[4096];
                    int length;
                    long totalCopied = 0;

                    while ((length = in.read(buffer)) > 0) {
                        out.write(buffer, 0, length);
                        totalCopied += length;
                    }

                    fileInfoText.setText("File ready: " + copiedFile.getName() +
                            "\nSize: " + copiedFile.length() + " bytes");
                    Toast.makeText(this, "File ready for transfer", Toast.LENGTH_SHORT).show();

                    Log.i(TAG, "Copied " + totalCopied + " bytes to " + copiedFile.getAbsolutePath());
                }
            } catch (Exception e) {
                Log.e(TAG, "File copy error: " + e.getMessage());
                Toast.makeText(this, "Error preparing file: " + e.getMessage(), Toast.LENGTH_SHORT).show();
            }
        }
    }

    private void updateStatus(String status) {
        statusText.setText(status);
    }
}