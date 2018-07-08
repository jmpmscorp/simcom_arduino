/*
 * Copyright (c) 2013-2015 Kees Bakker.  All rights reserved.
 *
 * This file is part of GPRSbee.
 *
 * GPRSbee is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or(at your option) any later version.
 *
 * GPRSbee is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GPRSbee.  If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <Arduino.h>
#include <Stream.h>
#include <avr/pgmspace.h>

#include <stdlib.h>
#include "SIMCOM_Modem.h"
#include "SIMCOM_Modem_OnOff.h"

#define DEBUG

#ifdef DEBUG
#define debugPrintLn(...) { if (this->_diagStream) this->_diagStream->println(__VA_ARGS__); }
#define debugPrint(...) { if (this->_diagStream) this->_diagStream->print(__VA_ARGS__); }
#warning "Debug mode is ON"
#else
#define debugPrintLn(...)
#define debugPrint(...)
#endif

#ifdef ARDUINO_ARCH_AVR
#include <avr/wdt.h>
#else
#define wdt_reset()
#endif

#define CR "\r"
#define LF "\n"
#define CRLF "\r\n"

// TODO this needs to be set in the compiler directives. Find something else to do
#define SIMCOM_MODEM_TERMINATOR CRLF

#ifndef SIMCOM_MODEM_TERMINATOR
#warning "SIMCOM_MODEM_TERMINATOR is not set"
#define SIMCOM_MODEM_TERMINATOR CRLF
#endif

#define SIMCOM_MODEM_TERMINATOR_LEN (sizeof(SIMCOM_MODEM_TERMINATOR) - 1) // without the NULL terminator

#define SIMCOM_MODEM_DEFAULT_INPUT_BUFFER_SIZE 128

// Constructor
SIMCOM_Modem::SIMCOM_Modem() :
    _modemStream(0),
    _diagStream(0),
    _inputBufferSize(SIMCOM_MODEM_DEFAULT_INPUT_BUFFER_SIZE),
    _inputBuffer(0),
    _onoff(0),
    _baudRateChangeCallbackPtr(0),
    _appendCommand(false),
    _lastRSSI(0),
    _CSQtime(0),
    _minSignalQuality(-93)      // -93 dBm
{
    this->_isBufferInitialized = false;
}


//SIMCOM_Modem::SIMCOM_Modem(){}

// Turns the modem on and returns true if successful.
bool SIMCOM_Modem::on()
{
    _startOn = millis();

    if (!isOn()) {
        if (_onoff) {
            _onoff->on();
        }
    }

    // wait for power up
    bool timeout = true;
    for (uint8_t i = 0; i < 10; i++) {
        if (isAlive()) {
            timeout = false;
            break;
        }
    }

    if (timeout) {
        debugPrintLn("Error: No Reply from Modem");
        return false;
    }

    return isOn(); // this essentially means isOn() && isAlive()
}

// Turns the modem off and returns true if successful.
bool SIMCOM_Modem::off()
{
    // No matter if it is on or off, turn it off.
    if (_onoff) {
        _onoff->off();
    }

    _echoOff = false;

    return !isOn();
}

// Returns true if the modem is on.
bool SIMCOM_Modem::isOn() const
{
    if (_onoff) {
        return _onoff->isOn();
    }

    // No onoff. Let's assume it is on.
    return true;
}

void SIMCOM_Modem::mydelay(uint32_t nrMillis)
{
    const uint32_t d = 10;
    while (nrMillis > d) {
        wdt_reset();
        delay(d);
        nrMillis -= d;
    }
    delay(nrMillis);
}

void SIMCOM_Modem::flushInput()
{
  int c;
  while ((c = _modemStream->read()) >= 0) {
    debugPrint((char)c);
  }
}

/*
 * \brief Read a line of input from SIM900
 */
int SIMCOM_Modem::readLine(uint32_t ts_max)
{
  if (_inputBuffer == NULL) {
    return -1;
  }

  uint32_t ts_waitLF = 0;
  bool seenCR = false;
  int c;
  size_t bufcnt;

  //debugPrintLn(F("readLine"));
  bufcnt = 0;
  while (!isTimedOut(ts_max)) {
    wdt_reset();
    if (seenCR) {
      c = _modemStream->peek();
      // ts_waitLF is guaranteed to be non-zero
      if ((c == -1 && isTimedOut(ts_waitLF)) || (c != -1 && c != '\n')) {
        //debugPrint(F("readLine:  peek '")); debugPrint(c); debugPrintLn('\'');
        // Line ended with just <CR>. That's OK too.
        goto ok;
      }
      // Only \n should fall through
    }

    c = _modemStream->read();
    if (c < 0) {
      continue;
    }
    debugPrint((char)c);                 // echo the char
    seenCR = c == '\r';
    if (c == '\r') {
      ts_waitLF = millis() + 50;        // Wait another .05 sec for an optional LF
    } else if (c == '\n') {
      goto ok;
    } else {
      // Any other character is stored in the line buffer
      if (bufcnt < (_inputBufferSize - 1)) {    // Leave room for the terminating NUL
        _inputBuffer[bufcnt++] = c;
      }
    }
  }

  debugPrintLn(F("readLine timed out"));
  return -1;            // This indicates: timed out

ok:
  _inputBuffer[bufcnt] = 0;     // Terminate with NUL byte
  //debugPrint(F(" ")); debugPrintLn(_inputBuffer);
  return bufcnt;

}

/*
 * \brief Read a number of bytes from SIM900
 *
 * Read <len> bytes from SIM900 and store at most <buflen> in the buffer.
 *
 * Return 0 if <len> bytes were read from SIM900, else return the remaining number
 * that wasn't read due to the timeout.
 * Note. The buffer is a byte buffer and not a string, and it is not terminated
 * with a NUL byte.
 */
int SIMCOM_Modem::readBytes(size_t len, uint8_t *buffer, size_t buflen, uint32_t ts_max)
{
  //debugPrintLn(F("readBytes"));
  while (!isTimedOut(ts_max) && len > 0) {
    wdt_reset();
    int c = _modemStream->read();
    if (c < 0) {
      continue;
    }
    // Each character is stored in the buffer
    --len;
    if (buflen > 0) {
      *buffer++ = c;
      --buflen;
    }
  }
  if (buflen > 0) {
    // This is just a convenience if the data is an ASCII string (which we don't know here).
    *buffer = 0;
  }
  return len;
}

bool SIMCOM_Modem::waitForOK(uint16_t timeout)
{
  int len;
  uint32_t ts_max = millis() + timeout;
  while ((len = readLine(ts_max)) >= 0) {
    if (len == 0) {
      // Skip empty lines
      continue;
    }
    if (strcmp_P(_inputBuffer, PSTR("OK")) == 0) {
      return true;
    }
    else if (strcmp_P(_inputBuffer, PSTR("ERROR")) == 0) {
      return false;
    }
    // Other input is skipped.
  }
  return false;
}

bool SIMCOM_Modem::waitForMessage(const char *msg, uint32_t ts_max)
{
  int len;
  //debugPrint(F("waitForMessage: ")); debugPrintLn(msg);
  while ((len = readLine(ts_max)) >= 0) {
    if (len == 0) {
      // Skip empty lines
      continue;
    }
    if (strncmp(_inputBuffer, msg, strlen(msg)) == 0) {
      return true;
    }
  }
  return false;         // This indicates: timed out
}
bool SIMCOM_Modem::waitForMessage_P(const char *msg, uint32_t ts_max)
{
  int len;
  //debugPrint(F("waitForMessage: ")); debugPrintLn(msg);
  while ((len = readLine(ts_max)) >= 0) {
    if (len == 0) {
      // Skip empty lines
      continue;
    }
    if (strncmp_P(_inputBuffer, msg, strlen_P(msg)) == 0) {
      return true;
    }
  }
  return false;         // This indicates: timed out
}

int SIMCOM_Modem::waitForMessages(PGM_P msgs[], size_t nrMsgs, uint32_t ts_max)
{
  int len;
  //debugPrint(F("waitForMessages: ")); debugPrintLn(msgs[0]);
  while ((len = readLine(ts_max)) >= 0) {
    if (len == 0) {
      // Skip empty lines
      continue;
    }
    //debugPrint(F(" checking \"")); debugPrint(_inputBuffer); debugPrintLn("\"");
    for (size_t i = 0; i < nrMsgs; ++i) {
      //debugPrint(F("  checking \"")); debugPrint(msgs[i]); debugPrintLn("\"");
      if (strcmp_P(_inputBuffer, msgs[i]) == 0) {
        //debugPrint(F("  found i=")); debugPrint((int)i); debugPrintLn("");
        return i;
      }
    }
  }
  return -1;         // This indicates: timed out
}

/*
 * \brief Wait for a prompt, or timeout
 *
 * \return true if succeeded (the reply received), false if otherwise (timed out)
 */
bool SIMCOM_Modem::waitForPrompt(const char *prompt, uint32_t ts_max)
{
  const char * ptr = prompt;

  while (*ptr != '\0') {
    wdt_reset();
    if (isTimedOut(ts_max)) {
      break;
    }

    int c = _modemStream->read();
    if (c < 0) {
      continue;
    }

    debugPrint((char)c);
    switch (c) {
    case '\r':
      // Ignore
      break;
    case '\n':
      // Start all over
      ptr = prompt;
      break;
    default:
      if (*ptr == c) {
        ptr++;
      } else {
        // Start all over
        ptr = prompt;
      }
      break;
    }
  }

  return true;
}

/*
 * \brief Prepare for a new command
 */
void SIMCOM_Modem::sendCommandProlog()
{
  flushInput();
  mydelay(50);                  // Without this we get lots of "readLine timed out". Unclear why
  debugPrint(F(">> "));
}

/*
 * \brief Add a part of the command (don't yet send the final CR)
 */
void SIMCOM_Modem::sendCommandAdd(char c)
{
  debugPrint(c);
  _modemStream->print(c);
}
void SIMCOM_Modem::sendCommandAdd(int i)
{
  debugPrint(i);
  _modemStream->print(i);
}
void SIMCOM_Modem::sendCommandAdd(const char *cmd)
{
  debugPrint(cmd);
  _modemStream->print(cmd);
}
void SIMCOM_Modem::sendCommandAdd(const String & cmd)
{
  debugPrint(cmd);
  _modemStream->print(cmd);
}
void SIMCOM_Modem::sendCommandAdd_P(const char *cmd)
{
  debugPrint(reinterpret_cast<const __FlashStringHelper *>(cmd));
  _modemStream->print(reinterpret_cast<const __FlashStringHelper *>(cmd));
}

/*
 * \brief Send the final CR of the command
 */
void SIMCOM_Modem::sendCommandEpilog()
{
  debugPrintLn();
  _modemStream->print('\r');
}

void SIMCOM_Modem::sendCommand(const char *cmd)
{
  sendCommandProlog();
  sendCommandAdd(cmd);
  sendCommandEpilog();
}
void SIMCOM_Modem::sendCommand_P(const char *cmd)
{
  sendCommandProlog();
  sendCommandAdd_P(cmd);
  sendCommandEpilog();
}

/*
 * \brief Send a command to the SIM900 and wait for "OK"
 *
 * The command string should not include the <CR>
 * Return true, only if "OK" is seen. "ERROR" and timeout
 * result in false.
 */
bool SIMCOM_Modem::sendCommandWaitForOK(const char *cmd, uint16_t timeout)
{
  sendCommand(cmd);
  return waitForOK(timeout);
}
bool SIMCOM_Modem::sendCommandWaitForOK(const String & cmd, uint16_t timeout)
{
  sendCommand(cmd.c_str());
  return waitForOK(timeout);
}
bool SIMCOM_Modem::sendCommandWaitForOK_P(const char *cmd, uint16_t timeout)
{
  sendCommand_P(cmd);
  return waitForOK(timeout);
}

/*
 * \brief Get SIM900 integer value
 *
 * Send the SIM900 command and wait for the reply. The reply
 * also contains the value that we want. We use the first value
 * upto the comma or the end
 * Finally the SIM900 should give "OK"
 *
 * An example is:
 *   >> AT+CSQ
 *   << +CSQ: 18,0
 *   <<
 *   << OK
 */
bool SIMCOM_Modem::getIntValue(const char *cmd, const char *reply, int * value, uint32_t ts_max)
{
  sendCommand(cmd);

  // First we expect the reply
  if (waitForMessage(reply, ts_max)) {
    const char *ptr = _inputBuffer + strlen(reply);
    char *bufend;
    *value = strtoul(ptr, &bufend, 0);
    if (bufend == ptr) {
      // Invalid number
      return false;
    }
    // Wait for "OK"
    return waitForOK();
  }
  return false;
}

bool SIMCOM_Modem::getIntValue_P(const char *cmd, const char *reply, int * value, uint32_t ts_max)
{
  sendCommand_P(cmd);

  // First we expect the reply
  if (waitForMessage_P(reply, ts_max)) {
    const char *ptr = _inputBuffer + strlen_P(reply);
    char *bufend;
    *value = strtoul(ptr, &bufend, 0);
    if (bufend == ptr) {
      // Invalid number
      return false;
    }
    // Wait for "OK"
    return waitForOK();
  }
  return false;
}

/*
 * \brief Get SIM900 string value with the result of an AT command
 *
 *\param cmd    the AT command
 *\param reply  the prefix of the expected reply (this is stripped from the result
 *\param str    a pointer to where the result must be copied
 *\param size   the length of the result buffer
 *\param ts_max the maximum ts to wait for the reply
 *
 * Send the SIM900 command and wait for the reply (prefixed with <reply>.
 * Finally the SIM900 should give "OK"
 *
 * An example is:
 *   >> AT+GCAP
 *   << +GCAP:+FCLASS,+CGSM
 *   <<
 *   << OK
 */
bool SIMCOM_Modem::getStrValue(const char *cmd, const char *reply, char * str, size_t size, uint32_t ts_max)
{
  sendCommand(cmd);

  if (waitForMessage(reply, ts_max)) {
    const char *ptr = _inputBuffer + strlen(reply);
    // Strip leading white space
    while (*ptr != '\0' && *ptr == ' ') {
      ++ptr;
    }
    strncpy(str, ptr, size - 1);
    str[size - 1] = '\0';               // Terminate, just to be sure
    // Wait for "OK"
    return waitForOK();
  }
  return false;
}

bool SIMCOM_Modem::getStrValue_P(const char *cmd, const char *reply, char * str, size_t size, uint32_t ts_max)
{
  sendCommand_P(cmd);

  if (waitForMessage_P(reply, ts_max)) {
    const char *ptr = _inputBuffer + strlen_P(reply);
    // Strip leading white space
    while (*ptr != '\0' && *ptr == ' ') {
      ++ptr;
    }
    strncpy(str, ptr, size - 1);
    str[size - 1] = '\0';               // Terminate, just to be sure
    // Wait for "OK"
    return waitForOK();
  }
  return false;
}

/*
 * \brief Get SIM900 string value with the result of an AT command
 *
 *\param cmd    the AT command
 *\param str    a pointer to where the result must be copied
 *\param size   the length of the result buffer
 *\param ts_max the maximum ts to wait for the reply
 *
 * Send the SIM900 command and wait for the reply.
 * Finally the SIM900 should give "OK"
 *
 * An example is:
 *   >> AT+GSN
 *   << 861785005921311
 *   <<
 *   << OK
 */
bool SIMCOM_Modem::getStrValue(const char *cmd, char * str, size_t size, uint32_t ts_max)
{
  sendCommand(cmd);

  int len;
  while ((len = readLine(ts_max)) >= 0) {
    if (len == 0) {
      // Skip empty lines
      continue;
    }
    strncpy(str, _inputBuffer, size - 1);
    str[size - 1] = '\0';               // Terminate, just to be sure
    break;
  }
  if (len < 0) {
      // There was a timeout
      return false;
  }
  // Wait for "OK"
  return waitForOK();
}

void SIMCOM_Modem::writeProlog()
{
    if (!_appendCommand) {
        debugPrint(">> ");
        _appendCommand = true;
    }
}

// Write a byte, as binary data
size_t SIMCOM_Modem::writeByte(uint8_t value)
{
    return _modemStream->write(value);
}

size_t SIMCOM_Modem::print(const String& buffer)
{
    writeProlog();
    debugPrint(buffer);

    return _modemStream->print(buffer);
}

size_t SIMCOM_Modem::print(const char buffer[])
{
    writeProlog();
    debugPrint(buffer);

    return _modemStream->print(buffer);
}

size_t SIMCOM_Modem::print(char value)
{
    writeProlog();
    debugPrint(value);

    return _modemStream->print(value);
};

size_t SIMCOM_Modem::print(unsigned char value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t SIMCOM_Modem::print(int value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t SIMCOM_Modem::print(unsigned int value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t SIMCOM_Modem::print(long value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t SIMCOM_Modem::print(unsigned long value, int base)
{
    writeProlog();
    debugPrint(value, base);

    return _modemStream->print(value, base);
};

size_t SIMCOM_Modem::println(const __FlashStringHelper *ifsh)
{
    size_t n = print(ifsh);
    n += println();
    return n;
}

size_t SIMCOM_Modem::println(const String &s)
{
    size_t n = print(s);
    n += println();
    return n;
}

size_t SIMCOM_Modem::println(const char c[])
{
    size_t n = print(c);
    n += println();
    return n;
}

size_t SIMCOM_Modem::println(char c)
{
    size_t n = print(c);
    n += println();
    return n;
}

size_t SIMCOM_Modem::println(unsigned char b, int base)
{
    size_t i = print(b, base);
    return i + println();
}

size_t SIMCOM_Modem::println(int num, int base)
{
    size_t i = print(num, base);
    return i + println();
}

size_t SIMCOM_Modem::println(unsigned int num, int base)
{
    size_t i = print(num, base);
    return i + println();
}

size_t SIMCOM_Modem::println(long num, int base)
{
    size_t i = print(num, base);
    return i + println();
}

size_t SIMCOM_Modem::println(unsigned long num, int base)
{
    size_t i = print(num, base);
    return i + println();
}

size_t SIMCOM_Modem::println(double num, int digits)
{
    writeProlog();
    debugPrint(num, digits);

    return _modemStream->println(num, digits);
}

size_t SIMCOM_Modem::println(const Printable& x)
{
    size_t i = print(x);
    return i + println();
}

size_t SIMCOM_Modem::println(void)
{
    debugPrintLn();
    size_t i = print('\r');
    _appendCommand = false;
    return i;
}

// Initializes the input buffer and makes sure it is only initialized once. 
// Safe to call multiple times.
void SIMCOM_Modem::initBuffer()
{
    debugPrintLn("[initBuffer]");

    // make sure the buffers are only initialized once
    if (!_isBufferInitialized) {
        this->_inputBuffer = static_cast<char*>(malloc(this->_inputBufferSize));

        _isBufferInitialized = true;
    }
}

// Sets the modem stream.
void SIMCOM_Modem::setModemStream(Stream& stream)
{
    this->_modemStream = &stream;
}

void SIMCOM_Modem::setPin(const char * pin)
{
    size_t len = strlen(pin);
    _pin = static_cast<char*>(realloc(_pin, len + 1));
    strcpy(_pin, pin);
}

void SIMCOM_Modem::setMinSignalQuality(int q)
{
    if (q < 0) {
        _minSignalQuality = q;
    } else {
        // This is correct for UBlox
        // For SIM is is close enough
        _minSignalQuality = -113 + 2 * q;
    }
}

