// SL.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <stdio.h>
#include <string>
#include <cstring>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

using namespace std;
HANDLE OpenSerialPort(const char* portName, DWORD baudRate)
{
    // Use \\.\\ prefix to support COM10+ and consistent naming
    std::string fullName = std::string("\\\\.\\") + portName; // becomes "\\.\COM3"
    HANDLE h = CreateFileA(fullName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open port %s (error %lu)\n", portName, GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) {
        fprintf(stderr, "GetCommState failed (error %lu)\n", GetLastError());
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = baudRate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;

    if (!SetCommState(h, &dcb)) {
        fprintf(stderr, "SetCommState failed (error %lu)\n", GetLastError());
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }

    // Configure timeouts (suitable for simple synchronous I/O)
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.ReadTotalTimeoutConstant = 1000;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(h, &timeouts);

    // Optional: clear buffers
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return h;
}

bool WriteSerial(HANDLE h, const char* data, DWORD len)
{
    DWORD written = 0;
    if (!WriteFile(h, data, len, &written, NULL)) {
        fprintf(stderr, "WriteFile failed (error %lu)\n", GetLastError());
        return false;
    }
    return written == len;
}

int ReadSerial(HANDLE h, char* buf, DWORD bufSize, DWORD timeoutMs)
{
    COMMTIMEOUTS oldTimeouts;
    if (!GetCommTimeouts(h, &oldTimeouts)) {
        fprintf(stderr, "GetCommTimeouts failed (error %lu)\n", GetLastError());
        return -1;
    }

    COMMTIMEOUTS tmp = oldTimeouts;
    tmp.ReadIntervalTimeout = 50;
    tmp.ReadTotalTimeoutMultiplier = 0;
    tmp.ReadTotalTimeoutConstant = timeoutMs;
    SetCommTimeouts(h, &tmp);

    DWORD read = 0;
    BOOL ok = ReadFile(h, buf, bufSize, &read, NULL);

    // restore timeouts
    SetCommTimeouts(h, &oldTimeouts);

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_OPERATION_ABORTED) return 0;
        fprintf(stderr, "ReadFile failed (error %lu)\n", err);
        return -1;
    }

    return (int)read;
}

void SendLedCommand(HANDLE h, char value)
{
    if (value != '0' && value != '1') {
        fprintf(stderr, "Invalid LED command: must be '0' or '1'\n");
        return;
    }

    if (WriteSerial(h, &value, 1)) {
        printf("Sent LED command '%c' (%s)\n", value, value == '1' ? "ON" : "OFF");
    } else {
        fprintf(stderr, "Failed to send LED command\n");
    }
}

void ToggleThread(HANDLE h, std::atomic_bool* pToggleActive, int durationSec, int loopCount)
{
    bool state = false;
    int count = 0;
    while (pToggleActive->load() && (loopCount <= 0 || count < loopCount)) {
        count++;
        if (loopCount > 0) {
            printf("[Loop %d/%d] ", count, loopCount);
        } else {
            printf("[Loop %d] ", count);
        }
        SendLedCommand(h, state ? '1' : '0');
        state = !state;
        std::this_thread::sleep_for(std::chrono::seconds(durationSec));
    }
    printf("Toggle thread stopped (%d toggles)\n", count);
}

void PrintHelp()
{
    printf("\nAvailable Commands:\n");
    printf("  help, ?              - Print this help message\n");
    printf("  led on, ledon        - Send LED ON (character '1')\n");
    printf("  led off, ledoff      - Send LED OFF (character '0')\n");
    printf("  toggle on, toggleon  - Start alternating LED on/off\n");
    printf("  toggle off, toggleoff- Stop alternating LED\n");
    printf("  exit, quit           - Exit the program\n");
    printf("\nCommand Line Arguments:\n");
    printf("  Usage: SL.exe [port] [baud] [duration] [loops]\n");
    printf("  port     - COM port name (default: COM3)\n");
    printf("  baud     - Baud rate (default: 115200)\n");
    printf("  duration - Toggle interval in seconds (default: 5)\n");
    printf("  loops    - Number of toggle loops, 0=infinite (default: 0)\n\n");
}

int main(int argc, char** argv)
{
    const char* port = (argc > 1) ? argv[1] : "COM3";
    DWORD baud = (argc > 2) ? (DWORD)atoi(argv[2]) : 115200;
    int toggleDuration = (argc > 3) ? atoi(argv[3]) : 5;
    int toggleLoops = (argc > 4) ? atoi(argv[4]) : 0;

    printf("Opening %s at %lu baud...\n", port, (unsigned long)baud);
    printf("Toggle duration: %d seconds\n", toggleDuration);
    printf("Toggle loops: %s\n", toggleLoops > 0 ? std::to_string(toggleLoops).c_str() : "infinite");
    HANDLE h = OpenSerialPort(port, baud);
    if (h == INVALID_HANDLE_VALUE) return 1;

    printf("Starting background RX thread. Enter commands on console. Type 'help' for commands, 'exit' to quit.\n");

    std::atomic_bool running(true);
    std::atomic_bool toggleActive(false);
    std::thread toggleThread;

    std::thread rxThread([&]() {
        char rbuf[256];
        while (running.load()) {
            int rn = ReadSerial(h, rbuf, (DWORD)sizeof(rbuf) - 1, 500); // 500ms timeout
            if (rn > 0) {
                int len = rn;
                if (len > (int)sizeof(rbuf) - 1) len = (int)sizeof(rbuf) - 1;
                rbuf[len] = '\0';
                printf("<RX %d bytes>: %s\n", rn, rbuf);
            } else if (rn == 0) {
                continue;
            } else {
                fprintf(stderr, "RX thread read error\n");
                break;
            }
        }
    });

    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            printf("End of input, exiting\n");
            break;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();

        printf("Command: %s\n", line.c_str());
        std::string cmd = line;
        for (char &c : cmd) c = (char)tolower((unsigned char)c);

        if (cmd == "exit" || cmd == "quit") {
            const char* ack = "OK: exiting\r\n";
            WriteSerial(h, ack, (DWORD)strlen(ack));
            break;
        } else if (cmd == "help" || cmd == "?") {
            PrintHelp();
        } else if (cmd == "led on" || cmd == "ledon") {
            SendLedCommand(h, '1');
        } else if (cmd == "led off" || cmd == "ledoff") {
            SendLedCommand(h, '0');
        } else if (cmd == "toggle on" || cmd == "toggleon") {
            if (!toggleActive.load()) {
                toggleActive.store(true);
                toggleThread = std::thread(ToggleThread, h, &toggleActive, toggleDuration, toggleLoops);
                printf("Toggle started (alternating 1/0 every %d seconds for %s)\n", 
                    toggleDuration, toggleLoops > 0 ? std::to_string(toggleLoops).c_str() : "infinite loops");
            } else {
                printf("Toggle already running\n");
            }
        } else if (cmd == "toggle off" || cmd == "toggleoff") {
            if (toggleActive.load()) {
                toggleActive.store(false);
                if (toggleThread.joinable()) toggleThread.join();
                printf("Toggle stopped\n");
            } else {
                printf("Toggle not running\n");
            }
        } else {
            std::string resp = std::string("Echo: ") + line + "\r\n";
            WriteSerial(h, resp.c_str(), (DWORD)resp.size());
            printf("%s", resp.c_str());
        }
    }

    // stop receiver
    running.store(false);
    CancelIoEx(h, NULL);
    if (rxThread.joinable()) rxThread.join();

    // stop toggle if running
    if (toggleActive.load()) {
        toggleActive.store(false);
        if (toggleThread.joinable()) toggleThread.join();
    }

    CloseHandle(h);
    return 0;
}
