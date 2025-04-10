#include <winscard.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <Windows.h>
#include <ctime>

// Helper function to print bytes in hex format
std::string bytesToHex(const std::vector<BYTE>& bytes) {
    std::string result;
    char hex[3];
    for (BYTE b : bytes) {
        sprintf_s(hex, "%02X ", b);
        result += hex;
    }
    return result;
}

int main() {
    // Establish PC/SC context
    std::cout << "app\n";
    SCARDCONTEXT hContext;
    LONG rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &hContext);
    if (rv != SCARD_S_SUCCESS) {
        std::cerr << "Failed to establish context: " << rv << std::endl;
        return 1;
    }

    // List available readers
    DWORD readersLen;
    rv = SCardListReadersW(hContext, NULL, NULL, &readersLen);
    if (rv != SCARD_S_SUCCESS) {
        std::cerr << "Failed to list readers: " << rv << std::endl;
        SCardReleaseContext(hContext);
        return 1;
    }

    std::vector<wchar_t> readers(readersLen);
    rv = SCardListReadersW(hContext, NULL, readers.data(), &readersLen);
    if (rv != SCARD_S_SUCCESS || readersLen <= 0) {
        std::cerr << "Error reading reader list or no readers found" << std::endl;
        SCardReleaseContext(hContext);
        return 1;
    }

    // Display available readers and use first one
    wchar_t* readerName = readers.data();
    int readerCount = 0;
    while (*readerName != L'\0') {
        std::wcout << L"Reader " << readerCount++ << L": " << readerName << std::endl;
        readerName += wcslen(readerName) + 1;
    }

    if (readerCount == 0) {
        std::cerr << "No readers found!" << std::endl;
        SCardReleaseContext(hContext);
        return 1;
    }

    // Use the first reader
    readerName = readers.data();
    std::wcout << L"Using reader: " << readerName << std::endl;

    // Connect to the reader
    SCARDHANDLE hCard;
    DWORD activeProtocol;
    rv = SCardConnectW(hContext, readerName, SCARD_SHARE_SHARED,
        SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
        &hCard, &activeProtocol);

    if (rv != SCARD_S_SUCCESS) {
        std::cerr << "Failed to connect to card: " << rv << std::endl;
        SCardReleaseContext(hContext);
        return 1;
    }

    std::cout << "Connected to NFC device. Protocol: "
        << (activeProtocol == SCARD_PROTOCOL_T0 ? "T=0" : "T=1") << std::endl;

    // SELECT AID APDU - using the AID from apduservice.xml
    BYTE selectAid[] = {
        0x00, // CLA
        0xA4, // INS (SELECT)
        0x04, // P1 (Select by name)
        0x00, // P2 (First occurrence)
        0x07, // Lc (length of AID)
        0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 // AID value
    };

    BYTE selectResponse[258];
    DWORD selectResponseLen = sizeof(selectResponse);

    // Send SELECT AID command
    const SCARD_IO_REQUEST* pioSendPci =
        activeProtocol == SCARD_PROTOCOL_T0 ? SCARD_PCI_T0 : SCARD_PCI_T1;

    rv = SCardTransmit(hCard, pioSendPci, selectAid, sizeof(selectAid),
        NULL, selectResponse, &selectResponseLen);

    if (rv != SCARD_S_SUCCESS || selectResponseLen < 2) {
        std::cerr << "SELECT AID failed: " << rv << std::endl;
        SCardDisconnect(hCard, SCARD_LEAVE_CARD);
        SCardReleaseContext(hContext);
        return 1;
    }

    // Check response status bytes
    BYTE sw1 = selectResponse[selectResponseLen - 2];
    BYTE sw2 = selectResponse[selectResponseLen - 1];

    if (sw1 != 0x90 || sw2 != 0x00) {
        std::cerr << "SELECT AID failed with SW1SW2: "
            << std::hex << (int)sw1 << (int)sw2 << std::dec << std::endl;
        SCardDisconnect(hCard, SCARD_LEAVE_CARD);
        SCardReleaseContext(hContext);
        return 1;
    }

    std::cout << "SELECT AID successful" << std::endl;

    // Extract file size from response
    if (selectResponseLen < 6) { // Need at least 4 bytes for size + 2 for SW
        std::cerr << "Response doesn't contain file size" << std::endl;
        SCardDisconnect(hCard, SCARD_LEAVE_CARD);
        SCardReleaseContext(hContext);
        return 1;
    }

    // Extract the file size from the response (first 4 bytes)
    int fileSize = (selectResponse[0] << 24) |
        (selectResponse[1] << 16) |
        (selectResponse[2] << 8) |
        selectResponse[3];

    std::cout << "File size: " << fileSize << " bytes" << std::endl;

    // Generate output filename with timestamp
    std::time_t currentTime = std::time(nullptr);
    char timeStr[20];
    std::strftime(timeStr, sizeof(timeStr), "%Y%m%d_%H%M%S", std::localtime(&currentTime));
    std::string outputFileName = "received_file_" + std::string(timeStr) + ".bin";

    // Open output file
    std::ofstream outputFile(outputFileName, std::ios::binary);
    if (!outputFile.is_open()) {
        std::cerr << "Failed to open output file" << std::endl;
        SCardDisconnect(hCard, SCARD_LEAVE_CARD);
        SCardReleaseContext(hContext);
        return 1;
    }

    // Receive file data
    int totalReceived = 0;
    const int maxChunkSize = 230; // Match Android chunk size

    // Command to get file data
    BYTE getDataCmd[5] = {
        0x00, // CLA
        0xB0, // INS (READ BINARY)
        0x00, // P1 (Offset MSB)
        0x00, // P2 (Offset LSB)
        maxChunkSize // Le (Expected response length)
    };

    // Receive file in chunks
    while (totalReceived < fileSize) {
        // Update offset in command
        getDataCmd[2] = (totalReceived >> 8) & 0xFF; // MSB of offset
        getDataCmd[3] = totalReceived & 0xFF;        // LSB of offset

        // Set chunk size for last piece if needed
        int remaining = fileSize - totalReceived;
        getDataCmd[4] = (remaining > maxChunkSize) ? maxChunkSize : remaining;

        // Prepare for response
        BYTE dataResponse[258]; // Max APDU response size
        DWORD dataResponseLen = sizeof(dataResponse);

        // Send READ BINARY command
        rv = SCardTransmit(hCard, pioSendPci, getDataCmd, sizeof(getDataCmd),
            NULL, dataResponse, &dataResponseLen);

        if (rv != SCARD_S_SUCCESS) {
            std::cerr << "Failed to read data: " << rv << std::endl;
            break;
        }

        // Check status bytes
        if (dataResponseLen < 2) {
            std::cerr << "Data response too short" << std::endl;
            break;
        }

        BYTE dataSw1 = dataResponse[dataResponseLen - 2];
        BYTE dataSw2 = dataResponse[dataResponseLen - 1];

        if (dataSw1 == 0x90 && dataSw2 == 0x00) {
            // Write data to file (excluding SW1SW2)
            int chunkSize = dataResponseLen - 2;
            outputFile.write(reinterpret_cast<char*>(dataResponse), chunkSize);

            totalReceived += chunkSize;

            // Display progress
            int progressPercent = (totalReceived * 100) / fileSize;
            std::cout << "Received " << totalReceived << " of " << fileSize
                << " bytes (" << progressPercent << "%)\r" << std::flush;
        }
        else {
            std::cerr << "Error reading data: SW="
                << std::hex << (int)dataSw1 << (int)dataSw2 << std::dec << std::endl;
            break;
        }

        // Delay to prevent overwhelming the NFC interface
        Sleep(50);
    }

    outputFile.close();
    std::cout << std::endl << "File reception "
        << (totalReceived == fileSize ? "completed successfully" : "incomplete")
        << ": " << totalReceived << " of " << fileSize << " bytes received" << std::endl;
    std::cout << "Saved to: " << outputFileName << std::endl;

    // Clean up
    SCardDisconnect(hCard, SCARD_LEAVE_CARD);
    SCardReleaseContext(hContext);
    return 0;
}
