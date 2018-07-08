/*
 * Copyright (c) 2015-2016 Kees Bakker.  All rights reserved.
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

#ifndef _SIMCOM_MODEM_h
#define _SIMCOM_MODEM_h

#include <Arduino.h>
#include <stdint.h>
#include <Stream.h>
#include "SIMCOM_Datetime.h"
#include "SIMCOM_Modem_OnOff.h"

// callback for changing the baudrate of the modem stream.
typedef void (*BaudRateChangeCallbackPtr)(uint32_t newBaudrate);

#define SIMCOM_MODEM_DEFAULT_BUFFER_SIZE      64
#define DEFAULT_READ_MS 5000 // Used in readResponse()

class SIMCOM_Modem {
public:
    // Constructor
    SIMCOM_Modem();
    virtual ~SIMCOM_Modem() {}

    

    // Sets the onoff instance
    void setOnOff(SIMCOM_Modem_OnOff & onoff) { _onoff = &onoff; }

    // Turns the modem on and returns true if successful.
    bool on();

    // Turns the modem off and returns true if successful.
    bool off();

    // Sets the optional "Diagnostics and Debug" stream.
    void setDiag(Stream &stream) { _diagStream = &stream; }
    void setDiag(Stream *stream) { _diagStream = stream; }

    // Sets the size of the input buffer.
    // Needs to be called before init().
    void setInputBufferSize(size_t value) { this->_inputBufferSize = value; };

    // Store APN and user and password
    void setApn(const char *apn, const char *user = NULL, const char *pass = NULL);
    void setApnUser(const char *user);
    void setApnPass(const char *pass);

    // Store PIN
    void setPin(const char *pin);

    // Returns the default baud rate of the modem. 
    // To be used when initializing the modem stream for the first time.
    //virtual uint32_t getDefaultBaudrate() = 0;

    // Enables the change of the baud rate to a higher speed when the modem is ready to do so.
    // Needs a callback in the main application to re-initialize the stream.
    void enableBaudrateChange(BaudRateChangeCallbackPtr callback) { _baudRateChangeCallbackPtr = callback; };

    void setMinSignalQuality(int q);
    uint8_t getCSQtime() const { return _CSQtime; }

    uint8_t getLastRSSI() const { return _lastRSSI; }    

    // Gets the Received Signal Strength Indication in dBm and Bit Error Rate.
    // Returns true if successful.
    virtual bool getRSSIAndBER(int8_t* rssi, uint8_t* ber) = 0;

    // Gets the Operator Name.
    // Returns true if successful.
    //virtual bool getOperatorName(char* buffer, size_t size) = 0;

    // Gets Mobile Directory Number.
    // Returns true if successful.
    //virtual bool getMobileDirectoryNumber(char* buffer, size_t size) = 0;

    // Gets International Mobile Equipment Identity.
    // Should be provided with a buffer of at least 16 bytes.
    // Returns true if successful.
    virtual bool getIMEI(char* buffer, size_t size) = 0;

    // Gets Integrated Circuit Card ID.
    // Should be provided with a buffer of at least 21 bytes.
    // Returns true if successful.
    virtual bool getCCID(char* buffer, size_t size) = 0;

    // Gets the International Mobile Station Identity.
    // Should be provided with a buffer of at least 16 bytes.
    // Returns true if successful.
    //virtual bool getIMSI(char* buffer, size_t size) = 0;

    bool sendCommandWaitForOK(const char *cmd, uint16_t timeout=4000);
    bool sendCommandWaitForOK(const String & cmd, uint16_t timeout=4000);
    bool sendCommandWaitForOK_P(const char *cmd, uint16_t timeout=4000);

protected:
    // The stream that communicates with the device.
    Stream* _modemStream;

    // The (optional) stream to show debug information.
    Stream* _diagStream;

    // The size of the input buffer. Equals SODAQ_GSM_MODEM_DEFAULT_INPUT_BUFFER_SIZE
    // by default or (optionally) a user-defined value when using USE_DYNAMIC_BUFFER.
    size_t _inputBufferSize;

    // Flag to make sure the buffers are not allocated more than once.
    bool _isBufferInitialized;

    // The buffer used when reading from the modem. The space is allocated during init() via initBuffer().
    char* _inputBuffer;

    char * _pin;

    // The on-off pin power controller object.
    SIMCOM_Modem_OnOff * _onoff;

    // The callback for requesting baudrate change of the modem stream.
    BaudRateChangeCallbackPtr _baudRateChangeCallbackPtr;

    // This flag keeps track if the next write is the continuation of the current command
    // A Carriage Return will reset this flag.
    bool _appendCommand;

    // This is the value of the most recent CSQ
    // Notice that CSQ is somewhat standard. SIM800/SIM900 and Ublox
    // compute to comparable numbers. With minor deviations.
    // For example SIM800
    //   1              -111 dBm
    //   2...30         -110... -54 dBm
    // For example UBlox
    //   1              -111 dBm
    //   2..30          -109 to -53 dBm
    int8_t _lastRSSI;   // 0 not known or not detectable

    // This is the number of second it took when CSQ was record last
    uint8_t _CSQtime;

    // This is the minimum required CSQ to continue making the connection
    int _minSignalQuality;

    // Keep track if ATE0 was sent
    bool _echoOff;

    // Keep track when connect started. Use this to record various status changes.
    uint32_t _startOn;

    // Initializes the input buffer and makes sure it is only initialized once.
    // Safe to call multiple times.
    void initBuffer();

    void mydelay(uint32_t nrMillis);
    // Returns true if the modem is ON (and replies to "AT" commands without timing out)
    virtual bool isAlive() = 0;

    // Returns true if the modem is on.
    bool isOn() const;

    virtual void switchEchoOff() = 0;

    // Sets the modem stream.
    void setModemStream(Stream& stream);

    // Small utility to see if we timed out
    bool isTimedOut(uint32_t ts) { return (long)(millis() - ts) >= 0; }

    void flushInput();
    int readLine(uint32_t ts_max);
    int readBytes(size_t len, uint8_t *buffer, size_t buflen, uint32_t ts_max);
    bool waitForOK(uint16_t timeout=4000);
    bool waitForMessage(const char *msg, uint32_t ts_max);
    bool waitForMessage_P(const char *msg, uint32_t ts_max);
    int waitForMessages(const char *msgs[], size_t nrMsgs, uint32_t ts_max);
    bool waitForPrompt(const char *prompt, uint32_t ts_max);

    void sendCommandProlog();
    void sendCommandAdd(char c);
    void sendCommandAdd(int i);
    void sendCommandAdd(const char *cmd);
    void sendCommandAdd(const String & cmd);
    void sendCommandAdd_P(const char *cmd);
    void sendCommandEpilog();

    void sendCommand(const char *cmd);
    void sendCommand_P(const char *cmd);

    bool getIntValue(const char *cmd, const char *reply, int * value, uint32_t ts_max);
    bool getIntValue_P(const char *cmd, const char *reply, int * value, uint32_t ts_max);
    bool getStrValue(const char *cmd, const char *reply, char * str, size_t size, uint32_t ts_max);
    bool getStrValue_P(const char *cmd, const char *reply, char * str, size_t size, uint32_t ts_max);
    bool getStrValue(const char *cmd, char * str, size_t size, uint32_t ts_max);

    // Write a byte
    size_t writeByte(uint8_t value);

    // Write the command prolog (just for debugging
    void writeProlog();

    size_t print(const __FlashStringHelper *);
    size_t print(const String &);
    size_t print(const char[]);
    size_t print(char);
    size_t print(unsigned char, int = DEC);
    size_t print(int, int = DEC);
    size_t print(unsigned int, int = DEC);
    size_t print(long, int = DEC);
    size_t print(unsigned long, int = DEC);
    size_t print(double, int = 2);
    size_t print(const Printable&);

    size_t println(const __FlashStringHelper *);
    size_t println(const String &s);
    size_t println(const char[]);
    size_t println(char);
    size_t println(unsigned char, int = DEC);
    size_t println(int, int = DEC);
    size_t println(unsigned int, int = DEC);
    size_t println(long, int = DEC);
    size_t println(unsigned long, int = DEC);
    size_t println(double, int = 2);
    size_t println(const Printable&);
    size_t println(void);
};

#endif
