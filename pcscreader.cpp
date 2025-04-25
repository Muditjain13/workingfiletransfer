#include <winscard.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <Windows.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <wincrypt.h> // For MD5 checksum calculation

// Helper function to print bytes in hex format
std::string bytesToHex(const std::vector<BYTE>& bytes) {
    std::string result;
    char hex[3];  // Buffer size reduced to 3 (2 hex chars + null terminator)
    for (BYTE b : bytes) {
        sprintf_s(hex, "%02X", b);  // FIXED: Removed space after each byte
        result += hex;
    }
    return result;
}

// Calculate MD5 checksum of a buffer (to match Android's implementation)
std::vector<BYTE> calculateMD5Buffer(const std::vector<char>& buffer) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::vector<BYTE> hashValue;

    try {
        // Initialize cryptography provider
        if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
            throw std::runtime_error("CryptAcquireContext failed");
        }

        // Create hash object
        if (!CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash)) {
            throw std::runtime_error("CryptCreateHash failed");
        }

        // Add data to hash
        if (!CryptHashData(hHash,
            reinterpret_cast<BYTE*>(const_cast<char*>(buffer.data())),
            static_cast<DWORD>(buffer.size()),
            0)) {
            throw std::runtime_error("CryptHashData failed");
        }

        // Get hash value
        DWORD hashSize = 0;
        DWORD hashSizeSize = sizeof(DWORD);
        if (!CryptGetHashParam(hHash, HP_HASHSIZE, (BYTE*)&hashSize, &hashSizeSize, 0)) {
            throw std::runtime_error("CryptGetHashParam HP_HASHSIZE failed");
        }

        hashValue.resize(hashSize);
        if (!CryptGetHashParam(hHash, HP_HASHVAL, hashValue.data(), &hashSize, 0)) {
            throw std::runtime_error("CryptGetHashParam HP_HASHVAL failed");
        }

        // Clean up
        if (hHash) CryptDestroyHash(hHash);
        if (hProv) CryptReleaseContext(hProv, 0);

        return hashValue;
    }
    catch (const std::exception& e) {
        std::cerr << "Error calculating MD5: " << e.what() << std::endl;

        // Clean up on error
        if (hHash) CryptDestroyHash(hHash);
        if (hProv) CryptReleaseContext(hProv, 0);

        return std::vector<BYTE>();
    }
}

// Function to read entire file into memory and calculate MD5 (to match Android implementation)
std::vector<BYTE> calculateMD5(const std::string& filename) {
    try {
        // Open file
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Unable to open file for MD5 calculation");
        }

        // Get file size
        file.seekg(0, std::ios::end);
        std::streamsize fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        // Read entire file into memory at once (matching Android implementation)
        std::vector<char> buffer(fileSize);
        if (!file.read(buffer.data(), fileSize)) {
            throw std::runtime_error("Failed to read file for MD5 calculation");
        }

        // Calculate MD5 on the entire file buffer
        return calculateMD5Buffer(buffer);
    }
    catch (const std::exception& e) {
        std::cerr << "Error in calculateMD5: " << e.what() << std::endl;
        return std::vector<BYTE>();
    }
}

// New function to compare two MD5 hashes
bool compareMD5(const std::vector<BYTE>& hash1, const std::vector<BYTE>& hash2) {
    if (hash1.size() != hash2.size()) return false;

    for (size_t i = 0; i < hash1.size(); i++) {
        if (hash1[i] != hash2[i]) return false;
    }

    return true;
}

int main() {
    // Establish PC/SC context
    std::cout << "NFC File Receiver Application\n";
    std::ofstream checksumLogFile("packet_checksums.txt", std::ios::trunc);
    checksumLogFile << "Starting log ";

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

    // Get file metadata (using INS_GET_FILE_METADATA)
    BYTE getMetadataCmd[] = {
        0x00, // CLA
        0xB2, // INS (GET_FILE_METADATA)
        0x00, // P1
        0x00, // P2
        0x00  // Le (get all available data)
    };

    BYTE metadataResponse[258];
    DWORD metadataResponseLen = sizeof(metadataResponse);

    rv = SCardTransmit(hCard, pioSendPci, getMetadataCmd, sizeof(getMetadataCmd),
        NULL, metadataResponse, &metadataResponseLen);

    std::string fileName = "received_file";
    std::string fileExtension = ".bin";

    if (rv == SCARD_S_SUCCESS && metadataResponseLen > 2) {
        // Last two bytes are SW1SW2, metadata is everything before
        int metadataLength = metadataResponseLen - 2;

        if (metadataLength > 0) {
            // Convert metadata bytes to string
            std::string metadataStr(reinterpret_cast<char*>(metadataResponse), metadataLength);

            // Split string by newline
            size_t newlinePos = metadataStr.find('\n');
            if (newlinePos != std::string::npos) {
                fileName = metadataStr.substr(0, newlinePos);
                fileExtension = metadataStr.substr(newlinePos + 1);

                std::cout << "File metadata received" << std::endl;
                std::cout << "  Original filename: " << fileName << std::endl;
                std::cout << "  File extension: " << fileExtension << std::endl;
            }
        }
    }
    else {
        std::cout << "Couldn't get file metadata, using default filename" << std::endl;
    }

    // Clean up filename by removing invalid characters
    for (char& c : fileName) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }

    // Create final filename - use the original name with extension
    std::string outputFileName = fileName + fileExtension;
    std::string tempFileName = outputFileName + ".temp";

    // Open temporary output file
    std::ofstream outputFile(tempFileName, std::ios::binary);
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

    // Create a buffer to store the entire file data (to match Android implementation)
    std::vector<char> fileBuffer(fileSize);

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
            // Calculate actual data size (excluding SW1SW2)
            int dataSize = dataResponseLen - 2;  // Exclude status bytes
            int bytesToWrite = (dataSize < remaining) ? dataSize : remaining;

            // Write data to file and buffer WITHOUT including status bytes
            outputFile.write(reinterpret_cast<char*>(dataResponse), bytesToWrite);

            // Copy to in-memory buffer ONLY the actual data (not status bytes)

            std::memcpy(&fileBuffer[totalReceived], dataResponse, bytesToWrite);

            // Update the received count with the actual bytes written
            totalReceived += bytesToWrite;
            std::vector<char> packetBuffer(dataResponse, dataResponse + bytesToWrite);
            std::vector<BYTE> packetChecksum = calculateMD5Buffer(packetBuffer);
            // Inside the packet receiving loop


// Log checksum to file
checksumLogFile << "Packet " << ceil(totalReceived / maxChunkSize) << ": " << bytesToHex(packetChecksum) << std::endl;


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

        // Small delay to prevent overwhelming the NFC interface
        Sleep(50);
    }

    // Close the output file to ensure all data is written
    outputFile.close();

    // Final report - use exact match for completion check
    std::cout << std::endl << "Final fileSize: " << fileSize
        << ", totalReceived: " << totalReceived
        << ", discrepancy: " << (fileSize - totalReceived) << std::endl;

    bool transferComplete = (totalReceived == fileSize);
    std::cout << "File reception "
        << (transferComplete ? "completed successfully" : "incomplete")
        << ": " << totalReceived << " of " << fileSize << " bytes received" << std::endl;

    // Get file checksum from sender
    BYTE getChecksumCmd[] = {
    0x00, // CLA
    0xB1, // INS (GET_CHECKSUM)
    0x00, // P1
    0x00, // P2
    0x00  // Le (get all available bytes)
    };

    BYTE checksumResponse[258];
    DWORD checksumResponseLen = sizeof(checksumResponse);

    std::vector<BYTE> receivedChecksum;
    bool checksumVerified = false;

    rv = SCardTransmit(hCard, pioSendPci, getChecksumCmd, sizeof(getChecksumCmd),
        NULL, checksumResponse, &checksumResponseLen);

    if (rv == SCARD_S_SUCCESS && checksumResponseLen > 2) {
        // Last two bytes are SW1SW2, checksum is everything before
        int checksumLength = checksumResponseLen - 2;
        receivedChecksum.assign(checksumResponse, checksumResponse + checksumLength);

        // Get hex string of received checksum (without spaces)
        std::string receivedHexStr = bytesToHex(receivedChecksum);
        std::cout << "Received MD5 checksum: " << receivedHexStr << std::endl;

        // Calculate MD5 from our buffer
        std::vector<char> actualBuffer(fileBuffer.begin(), fileBuffer.begin() + totalReceived);
        std::vector<BYTE> calculatedChecksumFromBuffer = calculateMD5Buffer(actualBuffer);
        std::string calculatedHexStr = bytesToHex(calculatedChecksumFromBuffer);

        std::cout << "Calculated MD5 checksum: " << calculatedHexStr << std::endl;

        // Also calculate from file as a backup check
        std::vector<BYTE> calculatedChecksumFromFile = calculateMD5(tempFileName);
        std::string fileHexStr = bytesToHex(calculatedChecksumFromFile);

        // IMPORTANT: Compare string representations (case-insensitive)
        auto caseInsensitiveCompare = [](const std::string& a, const std::string& b) {
            if (a.length() != b.length()) return false;
            for (size_t i = 0; i < a.length(); i++) {
                if (toupper(a[i]) != toupper(b[i])) return false;
            }
            return true;
            };

        // Try matching with and without spaces
        checksumVerified = caseInsensitiveCompare(receivedHexStr, calculatedHexStr);

        // If still not matching, try normalizing both strings (remove all spaces)
        if (!checksumVerified) {
            std::string normalizedReceived = receivedHexStr;
            std::string normalizedCalculated = calculatedHexStr;

            // Remove any spaces
            normalizedReceived.erase(std::remove(normalizedReceived.begin(), normalizedReceived.end(), ' '), normalizedReceived.end());
            normalizedCalculated.erase(std::remove(normalizedCalculated.begin(), normalizedCalculated.end(), ' '), normalizedCalculated.end());

            std::cout << "Normalized checksums for comparison:" << std::endl;
            std::cout << "  Received : " << normalizedReceived << std::endl;
            std::cout << "  Calculated: " << normalizedCalculated << std::endl;

            checksumVerified = caseInsensitiveCompare(normalizedReceived, normalizedCalculated);
        }

        std::cout << "Checksum verification: " << (checksumVerified ? "PASSED" : "FAILED") << std::endl;

        // If checksum verification still fails, add an error message with possible causes
        if (!checksumVerified) {
            std::cout << "\nPossible causes of checksum mismatch:" << std::endl;
            std::cout << "1. Data corruption during transfer" << std::endl;
            std::cout << "2. Different MD5 implementation between Android and Windows" << std::endl;
            std::cout << "3. Incorrect byte handling in the transfer process" << std::endl;
        }
    }
    else {
        std::cout << "Could not retrieve checksum from sender" << std::endl;
    }

    // Despite checksum failure, if file size matches, offer option to use file anyway
    if (transferComplete && !checksumVerified) {
        std::cout << "\nFile size is correct but checksum failed." << std::endl;
        std::cout << "Do you want to use the file anyway? (y/n): ";
        char response;
        std::cin >> response;

        if (response == 'y' || response == 'Y') {
            // Rename temp file to final filename
            if (std::rename(tempFileName.c_str(), outputFileName.c_str()) != 0) {
                std::cerr << "Error moving temporary file to final location" << std::endl;
            }
            else {
                std::cout << "File saved despite checksum mismatch: " << outputFileName << std::endl;
            }
        }
    }

    // Clean up
    SCardDisconnect(hCard, SCARD_LEAVE_CARD);
    SCardReleaseContext(hContext);
    return 0;
}
