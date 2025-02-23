#include <codecvt>
#include <iostream>
#include <locale>
#include "SerialPort.h"
#include "utils.h"


SerialPort::SerialPort()
    : m_hSerial(INVALID_HANDLE_VALUE)
{
}

SerialPort::~SerialPort()
{
  close();
}

bool SerialPort::open(std::string portName, int baudRate)
{
  std::string portNameAnsi = "\\\\.\\" + portName;

  // Convert ANSI string to wide string.
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  std::wstring portNameW = converter.from_bytes(portNameAnsi);

  m_hSerial = CreateFile(
      portNameW.c_str(), // Port name
      GENERIC_READ,      // Open for reading
      0,                 // Do not share
      NULL,              // Default security attributes
      OPEN_EXISTING,     // Must already exist
      0,                 // Non-overlapped I/O
      NULL);

  if (m_hSerial == INVALID_HANDLE_VALUE)
  {
    std::cerr << LOG_LOCATION << "Error creating handle for port " << portName << std::endl;
    return false;
  }

  // Retrieve the current serial port settings.
  DCB dcbSerialParams = {0};
  dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
  if (!GetCommState(m_hSerial, &dcbSerialParams))
  {
    close();
    std::cerr << LOG_LOCATION << "Error getting comm state" << std::endl;
    return false;
  }

  // Set serial port parameters: 9600 baud, 8 data bits, no parity, 1 stop bit.
  dcbSerialParams.BaudRate = baudRate;
  dcbSerialParams.ByteSize = 8;
  dcbSerialParams.Parity = NOPARITY;
  dcbSerialParams.StopBits = ONESTOPBIT;
  if (!SetCommState(m_hSerial, &dcbSerialParams))
  {
    close();
    std::cerr << LOG_LOCATION << "Error setting comm state" << std::endl;
    return false;
  }

  // Configure read timeouts to avoid blocking indefinitely.
  COMMTIMEOUTS timeouts = {0};
  timeouts.ReadIntervalTimeout = 50;        // Maximum time between bytes (ms)
  timeouts.ReadTotalTimeoutConstant = 50;   // Constant timeout (ms)
  timeouts.ReadTotalTimeoutMultiplier = 1;  // Per-byte timeout (ms)
  if (!SetCommTimeouts(m_hSerial, &timeouts))
  {
    close();
    std::cerr << LOG_LOCATION << "Error setting comm timeouts" << std::endl;
    return false;
  }

  return true;
}

void SerialPort::close()
{
  if (m_hSerial != INVALID_HANDLE_VALUE)
  {
    CloseHandle(m_hSerial);
    m_hSerial = INVALID_HANDLE_VALUE;
  }
}

int SerialPort::read(uint8_t *buffer, size_t size)
{
  DWORD bytesRead = 0;
  if (!ReadFile(m_hSerial, buffer, (DWORD)size, &bytesRead, NULL))
  {
    std::cerr << LOG_LOCATION << "Error reading" << std::endl;
    return -1;
  }

  return bytesRead;
}

bool SerialPort::isOpen() const
{
  return m_hSerial != INVALID_HANDLE_VALUE;
}