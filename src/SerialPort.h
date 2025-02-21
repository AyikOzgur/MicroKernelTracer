#pragma once
#include <string>
#include <windows.h>

class SerialPort
{
public:
  SerialPort();
  ~SerialPort();

  bool open(std::string portName);
  void close();
  int read(uint8_t *buffer, size_t size);

private:
  HANDLE m_hSerial;
};