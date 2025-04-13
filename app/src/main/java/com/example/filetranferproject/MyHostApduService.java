package com.example.filetranferproject;

import android.nfc.cardemulation.HostApduService;
import android.os.Bundle;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.security.MessageDigest;

public class MyHostApduService extends HostApduService {

    private static final String TAG = "HostApduService";
    private static final int CHUNK_SIZE = 230;

    private byte[] fileData;
    private byte[] fileChecksum;

    // Add variables for file metadata
    private String originalFileName = "";
    private String originalFileExtension = "";

    // Command instructions
    private static final byte INS_SELECT = (byte) 0xA4;
    private static final byte INS_READ_BINARY = (byte) 0xB0;
    private static final byte INS_GET_CHECKSUM = (byte) 0xB1;
    private static final byte INS_GET_FILE_METADATA = (byte) 0xB2; // New instruction for file metadata

    @Override
    public void onCreate() {
        super.onCreate();
        loadFile();
        loadFileMetadata();
    }

    private void loadFile() {
        try {
            File file = new File(getFilesDir(), "file_to_send.bin");
            if (!file.exists()) {
                Log.e(TAG, "File not found: " + file.getAbsolutePath());
                return;
            }

            // Load file into memory
            fileData = new byte[(int) file.length()];
            try (FileInputStream fis = new FileInputStream(file)) {
                fis.read(fileData);
            }

            // Calculate MD5 checksum
            MessageDigest md = MessageDigest.getInstance("MD5");
            fileChecksum = md.digest(fileData);

            Log.i(TAG, "Loaded file: " + file.length() + " bytes");
        } catch (Exception e) {
            Log.e(TAG, "Error loading file: " + e.getMessage());
            fileData = null;
        }
    }

    private void loadFileMetadata() {
        try {
            File metadataFile = new File(getFilesDir(), "file_metadata.txt");
            if (!metadataFile.exists()) {
                Log.e(TAG, "Metadata file not found");
                return;
            }

            try (BufferedReader reader = new BufferedReader(new FileReader(metadataFile))) {
                originalFileName = reader.readLine();
                originalFileExtension = reader.readLine();

                Log.i(TAG, "Loaded metadata: filename=" + originalFileName +
                        ", extension=" + originalFileExtension);
            }
        } catch (Exception e) {
            Log.e(TAG, "Error loading metadata: " + e.getMessage());
        }
    }

    @Override
    public byte[] processCommandApdu(byte[] apdu, Bundle extras) {
        if (apdu.length < 4) {
            return new byte[]{(byte) 0x6F, (byte) 0x00}; // Technical error
        }

        byte ins = apdu[1];

        // Handle different instructions
        if (ins == INS_SELECT) {
            return handleSelect();
        } else if (ins == INS_READ_BINARY) {
            return handleReadBinary(apdu);
        } else if (ins == INS_GET_CHECKSUM) {
            return handleGetChecksum();
        } else if (ins == INS_GET_FILE_METADATA) {
            return handleGetFileMetadata();
        }

        // Unsupported instruction
        return new byte[]{(byte) 0x6D, (byte) 0x00}; // Instruction not supported
    }

    private byte[] handleSelect() {
        Log.i(TAG, "SELECT command received");

        if (fileData == null || fileData.length == 0) {
            Log.e(TAG, "No file available");
            return new byte[]{(byte) 0x6A, (byte) 0x82}; // File not found
        }

        // Send file size in response
        byte[] fileSizeBytes = new byte[]{
                (byte) (fileData.length >> 24),
                (byte) (fileData.length >> 16),
                (byte) (fileData.length >> 8),
                (byte) fileData.length
        };

        byte[] response = new byte[fileSizeBytes.length + 2];
        System.arraycopy(fileSizeBytes, 0, response, 0, fileSizeBytes.length);
        response[fileSizeBytes.length] = (byte) 0x90;
        response[fileSizeBytes.length + 1] = (byte) 0x00;

        Log.i(TAG, "Sending file size: " + fileData.length + " bytes");
        return response;
    }

    private byte[] handleReadBinary(byte[] apdu) {
        if (fileData == null) {
            return new byte[]{(byte) 0x69, (byte) 0x86}; // Command not allowed
        }

        // Extract offset from P1-P2
        int offset = ((apdu[2] & 0xFF) << 8) | (apdu[3] & 0xFF);
        Log.d(TAG, "Reading from offset: " + offset);

        // Check if offset is valid
        if (offset >= fileData.length) {
            return new byte[]{(byte) 0x6B, (byte) 0x00}; // Wrong parameters
        }

        // Calculate actual bytes to send
        int remaining = fileData.length - offset;
        int toSend = Math.min(CHUNK_SIZE, remaining);

        // Prepare response
        byte[] response = new byte[toSend + 2];
        System.arraycopy(fileData, offset, response, 0, toSend);
        response[toSend] = (byte) 0x90;
        response[toSend + 1] = (byte) 0x00;

        Log.i(TAG, "Sending " + toSend + " bytes from offset " + offset);
        return response;
    }

    private byte[] handleGetChecksum() {
        if (fileChecksum == null) {
            return new byte[]{(byte) 0x6F, (byte) 0x00}; // Technical problem
        }

        // Return checksum + status
        byte[] response = new byte[fileChecksum.length + 2];
        System.arraycopy(fileChecksum, 0, response, 0, fileChecksum.length);
        response[fileChecksum.length] = (byte) 0x90;
        response[fileChecksum.length + 1] = (byte) 0x00;

        Log.i(TAG, "Sending file checksum");
        return response;
    }

    private byte[] handleGetFileMetadata() {
        Log.i(TAG, "Metadata request received");

        // Prepare metadata string (format: filename + '\n' + extension)
        String metadata = originalFileName + "\n" + originalFileExtension;
        byte[] metadataBytes = metadata.getBytes();

        // Prepare response: metadata + status bytes
        byte[] response = new byte[metadataBytes.length + 2];
        System.arraycopy(metadataBytes, 0, response, 0, metadataBytes.length);
        response[metadataBytes.length] = (byte) 0x90;
        response[metadataBytes.length + 1] = (byte) 0x00;

        Log.i(TAG, "Sending file metadata: " + metadata);
        return response;
    }

    @Override
    public void onDeactivated(int reason) {
        Log.i(TAG, "Connection deactivated, reason: " + reason);
    }
}