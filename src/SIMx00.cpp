/*
 * Copyright (c) 2013 Kees Bakker.  All rights reserved.
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

#include "SIMx00.h" 
#include "SIMCOM_Modem.h"

#if ENABLE_GPRSBEE_DIAG
#define diagPrint(...) { if (_diagStream) _diagStream->print(__VA_ARGS__); }
#define diagPrintLn(...) { if (_diagStream) _diagStream->println(__VA_ARGS__); }
#else
#define diagPrint(...)
#define diagPrintLn(...)
#endif

void SIMx00::init(Stream & stream, SIMCOM_Modem_OnOff &onoff, int bufferSize)
{
  initProlog(stream, bufferSize);
  _onoff = &onoff;
}

void SIMx00::initProlog(Stream &stream, size_t bufferSize)
{
  _inputBufferSize = bufferSize;
  initBuffer();

  _modemStream = &stream;
  _diagStream = 0;

  _ftpMaxLength = 0;
  _transMode = false;

  _echoOff = false;
  _skipCGATT = false;
  _changedSkipCGATT = false;

  _productId = prodid_unknown;

  _timeToOpenTCP = 0;
  _timeToCloseTCP = 0;
}

bool SIMx00::isAlive()
{
  // Send "AT" and wait for "OK"
  // Try it at least 3 times before deciding it failed
  for (int i = 0; i < 3; i++) {
    sendCommand_P(PSTR("AT"));
    if (waitForOK()) {
      return true;
    }
  }
  return false;
}

void SIMx00::switchEchoOff()
{
  if (!_echoOff) {
    // Suppress echoing
    if (!sendCommandWaitForOK_P(PSTR("ATE0"))) {
        // We didn't get an OK
        // Should we retry?
        return;
    }
    // Also disable URCs
    disableCIURC();
    _echoOff = true;
  }
}

/*!
 * \brief Utility function to do waitForSignalQuality and waitForCREG
 */
bool SIMx00::networkOn()
{
  bool status;
  status = on();
  if (status) {
    // Suppress echoing
    switchEchoOff();

    status = waitForSignalQuality();
    if (status) {
      status = waitForCREG();
    }
  }
  return status;
}

// Gets the Received Signal Strength Indication in dBm and Bit Error Rate.
// Returns true if successful.
bool SIMx00::getRSSIAndBER(int8_t* rssi, uint8_t* ber)
{
    static char berValues[] = { 49, 43, 37, 25, 19, 13, 7, 0 }; // 3GPP TS 45.008 [20] subclause 8.2.4
    int rssiRaw = 0;
    int berRaw = 0;
    // TODO get BER value
    if (getIntValue("AT+CSQ", "+CSQ:", &rssiRaw, millis() + 12000 )) {
        *rssi = ((rssiRaw == 99) ? 0 : -113 + 2 * rssiRaw);
        *ber = ((berRaw == 99 || static_cast<size_t>(berRaw) >= sizeof(berValues)) ? 0 : berValues[berRaw]);

        return true;
    }

    return false;
}

bool SIMx00::waitForSignalQuality()
{
    /*
     * The timeout is just a wild guess. If the mobile connection
     * is really bad, or even absent, then it is a waste of time
     * (and battery) to even try.
     */
    uint32_t start = millis();
    uint32_t ts_max = start + 30000;
    int8_t rssi;
    uint8_t ber;

    while (!isTimedOut(ts_max)) {
        if (getRSSIAndBER(&rssi, &ber)) {
            if (rssi != 0 && rssi >= _minSignalQuality) {
                _lastRSSI = rssi;
                _CSQtime = (int32_t) (millis() - start) / 1000;
                return true;
            }
        }
        /*sodaq_wdt_safe_*/ delay(500);
    }
    _lastRSSI = 0;
    return false;
}

bool SIMx00::waitForCREG()
{
  // TODO This timeout is maybe too long.
  uint32_t ts_max = millis() + 120000;
  int value;
  while (!isTimedOut(ts_max)) {
    sendCommand_P(PSTR("AT+CREG?"));
    // Reply is:
    // +CREG: <n>,<stat>[,<lac>,<ci>]   mostly this is +CREG: 0,1
    // we want the second number, the <stat>
    // 0 = Not registered, MT is not currently searching an operator to register to
    // 1 = Registered, home network
    // 2 = Not registered, but MT is currently trying to attach...
    // 3 = Registration denied
    // 4 = Unknown
    // 5 = Registered, roaming
    value = 0;
    if (waitForMessage_P(PSTR("+CREG:"), millis() + 12000)) {
      const char *ptr = strchr(_inputBuffer, ',');
      if (ptr) {
        ++ptr;
        value = strtoul(ptr, NULL, 0);
      }
    }
    waitForOK();
    if (value == 1 || value == 5) {
      return true;
    }
    mydelay(500);
    if (!isAlive()) {
      break;
    }
  }
  return false;
}

/*!
 * \brief Do a few common things to start a connection
 *
 * Do a few things that are common for setting up
 * a connection for TCP, FTP and HTTP.
 */
bool SIMx00::connectProlog()
{
  // TODO Use networkOn instead of switchEchoOff, waitForSignalQuality, waitForCREG

  // Suppress echoing
  switchEchoOff();

  // Wait for signal quality
  if (!waitForSignalQuality()) {
    return false;
  }

  // Wait for CREG
  if (!waitForCREG()) {
    return false;
  }

  if (!_changedSkipCGATT && _productId == prodid_unknown) {
    // Try to figure out what kind it is. SIM900? SIM800? etc.
    setProductId();
    if (_productId == prodid_SIM800) {
      _skipCGATT = true;
    }
  }

  // Attach to GPRS service
  // We need a longer timeout than the normal waitForOK
  if (!_skipCGATT && !sendCommandWaitForOK_P(PSTR("AT+CGATT=1"), 30000)) {
    return false;
  }

  return true;
}

/*
Secondly, you should use the command group AT+CSTT, AT+CIICR and AT+CIFSR to start
the task and activate the wireless connection. Lastly, you can establish TCP connection between
SIM900 and server by AT command (AT+CIPSTART=”TCP”,”IP Address of server”, “port
number of server”). If the connection is established successfully, response “CONNECT OK” will
come up from the module. Now you can send data to server with “AT+CIPSEND”.
“AT+CIPSEND” will return with promoting mark “>”, you should write data after “>” then issue
CTRL+Z (0x1a) to send. If sending is successful, it will respond “SEND OK”. And if there is data
coming from server, the module will receive the data automatically from the serial port. You can
close the TCP connection with “AT+CIPCLOSE” command. Below is an example of TCP
connection to remote server.
 */


bool SIMx00::openTCP(const char *apn,
    const char *server, int port, bool transMode)
{
  return openTCP(apn, 0, 0, server, port, transMode);
}

bool SIMx00::openTCP(const char *apn, const char *apnuser, const char *apnpwd,
    const char *server, int port, bool transMode)
{
  uint32_t ts_max;
  boolean retval = false;
  char cmdbuf[60];              // big enough for AT+CIPSTART="TCP","server",8500
  PGM_P CIPSTART_replies[] = {
      PSTR("CONNECT OK"),
      PSTR("CONNECT"),

      PSTR("CONNECT FAIL"),
      //"STATE: TCP CLOSED",
  };
  const size_t nrReplies = sizeof(CIPSTART_replies) / sizeof(CIPSTART_replies[0]);

  if (!on()) {
    goto ending;
  }

  if (!connectProlog()) {
    goto cmd_error;
  }

  // AT+CSTT=<apn>,<username>,<password>
  strcpy_P(cmdbuf, PSTR("AT+CSTT=\""));
  strcat(cmdbuf, apn);
  strcat(cmdbuf, "\",\"");
  if (apnuser) {
    strcat(cmdbuf, apnuser);
  }
  strcat(cmdbuf, "\",\"");
  if (apnpwd) {
    strcat(cmdbuf, apnpwd);
  }
  strcat(cmdbuf, "\"");
  if (!sendCommandWaitForOK(cmdbuf)) {
    goto cmd_error;
  }

  if (!sendCommandWaitForOK_P(PSTR("AT+CIICR"))) {
    goto cmd_error;
  }

#if 0
  // Get local IP address
  if (!sendCommandWaitForOK_P(PSTR("AT+CISFR"))) {
    goto cmd_error;
  }
#endif

  // AT+CIPSHUT
  sendCommand_P(PSTR("AT+CIPSHUT"));
  ts_max = millis() + 4000;             // Is this enough?
  if (!waitForMessage_P(PSTR("SHUT OK"), ts_max)) {
    goto cmd_error;
  }

  if (transMode) {
    if (!sendCommandWaitForOK_P(PSTR("AT+CIPMODE=1"))) {
      goto cmd_error;
    }
    //AT+CIPCCFG
    // Read the current settings
    if (!sendCommandWaitForOK_P(PSTR("AT+CIPCCFG?"))) {
      goto cmd_error;
    }
  }

  // Start up the connection
  // AT+CIPSTART="TCP","server",8500
  strcpy_P(cmdbuf, PSTR("AT+CIPSTART=\"TCP\",\""));
  strcat(cmdbuf, server);
  strcat_P(cmdbuf, PSTR("\","));
  itoa(port, cmdbuf + strlen(cmdbuf), 10);
  if (!sendCommandWaitForOK(cmdbuf)) {
    goto cmd_error;
  }
  ts_max = millis() + 15000;            // Is this enough?
  int ix;
  if ((ix = waitForMessages(CIPSTART_replies, nrReplies, ts_max)) < 0) {
    // For some weird reason the SIM900 in some cases does not want
    // to give us this CONNECT OK. But then we see it later in the stream.
    // The manual (V1.03) says that we can expect "CONNECT OK", but so far
    // we have only seen just "CONNECT" (or an error of course).
    goto cmd_error;
  }
  if (ix >= 2) {
    // Only some CIPSTART_replies are acceptable, i.e. "CONNECT" and "CONNECT OK"
    goto cmd_error;
  }

  // AT+CIPQSEND=0  normal send mode (reply after each data send will be SEND OK)
  if (false && !sendCommandWaitForOK_P(PSTR("AT+CIPQSEND=0"))) {
    goto cmd_error;
  }

  _transMode = transMode;
  retval = true;
  _timeToOpenTCP = millis() - _startOn;
  goto ending;

cmd_error:
  diagPrintLn(F("openTCP failed!"));
  off();

ending:
  return retval;
}

void SIMx00::closeTCP(bool switchOff)
{
  uint32_t ts_max;
  // AT+CIPSHUT
  // Maybe we should do AT+CIPCLOSE=1
  if (_transMode) {
    mydelay(1000);
    _modemStream->print(F("+++"));
    mydelay(500);
    // TODO Will the SIM900 answer with "OK"?
  }
  sendCommand_P(PSTR("AT+CIPSHUT"));
  ts_max = millis() + 4000;             // Is this enough?
  if (!waitForMessage_P(PSTR("SHUT OK"), ts_max)) {
    diagPrintLn(F("closeTCP failed!"));
  }

  if (switchOff) {
    off();
  }
  _timeToCloseTCP = millis() - _startOn;
}

bool SIMx00::isTCPConnected()
{
  uint32_t ts_max;
  bool retval = false;
  const char *ptr;

  if (!isOn()) {
    goto end;
  }

  if (_transMode) {
    // We need to send +++
    mydelay(1000);
    _modemStream->print(F("+++"));
    mydelay(500);
    if (!waitForOK()) {
      goto end;
    }
  }

  // AT+CIPSTATUS
  // Expected answer:
  // OK
  // STATE: <state>
  // The only good answer is "CONNECT OK"
  if (!sendCommandWaitForOK_P(PSTR("AT+CIPSTATUS"))) {
    goto end;
  }
  ts_max = millis() + 4000;             // Is this enough?
  if (!waitForMessage_P(PSTR("STATE:"), ts_max)) {
    goto end;
  }
  ptr = _inputBuffer + 6;
  ptr = skipWhiteSpace(ptr);
  // Look at the state
  if (strcmp_P(ptr, PSTR("CONNECT OK")) != 0) {
    goto end;
  }

  if (_transMode) {
    // We must switch back to transparent mode
    sendCommand_P(PSTR("ATO0"));
    // TODO wait for "CONNECT" or "NO CARRIER"
    ts_max = millis() + 4000;             // Is this enough? Or too much
    if (!waitForMessage_P(PSTR("CONNECT"), ts_max)) {
      goto end;
    }
  }

  retval = true;

end:
  return retval;
}

/*!
 * \brief Send some data over the TCP connection
 */
bool SIMx00::sendDataTCP(const uint8_t *data, size_t data_len)
{
  uint32_t ts_max;
  bool retval = false;

  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+CIPSEND="));
  sendCommandAdd((int)data_len);
  sendCommandEpilog();
  ts_max = millis() + 4000;             // Is this enough?
  if (!waitForPrompt("> ", ts_max)) {
    goto error;
  }
  mydelay(50);          // TODO Why do we need this?
  // Send the data
  for (size_t i = 0; i < data_len; ++i) {
    _modemStream->print((char)*data++);
  }
  //
  ts_max = millis() + 4000;             // Is this enough?
  if (!waitForMessage_P(PSTR("SEND OK"), ts_max)) {
    goto error;
  }

  retval = true;
  goto ending;
error:
  diagPrintLn(F("sendDataTCP failed!"));
ending:
  return retval;
}

/*!
 * \brief Receive a number of bytes from the TCP connection
 *
 * If there are not enough bytes then this function will time
 * out, and it will return false.
 */
bool SIMx00::receiveDataTCP(uint8_t *data, size_t data_len, uint16_t timeout)
{
  uint32_t ts_max;
  bool retval = false;

  //diagPrintLn(F("receiveDataTCP"));
  ts_max = millis() + timeout;
  while (data_len > 0 && !isTimedOut(ts_max)) {
    if (_modemStream->available() > 0) {
      uint8_t b;
      b = _modemStream->read();
      *data++ = b;
      --data_len;
    }
  }
  if (data_len == 0) {
    retval = true;
  }

  return retval;
}

/*!
 * \brief Receive a line of ASCII via the TCP connection
 */
bool SIMx00::receiveLineTCP(const char **buffer, uint16_t timeout)
{
  uint32_t ts_max;
  bool retval = false;

  //diagPrintLn(F("receiveLineTCP"));
  *buffer = NULL;
  ts_max = millis() + timeout;
  if (readLine(ts_max) < 0) {
    goto ending;
  }
  *buffer = _inputBuffer;
  retval = true;

ending:
  return retval;
}

/*
 * \brief Open a (FTP) session
 */
bool SIMx00::openFTP(const char *apn,
    const char *server, const char *username, const char *password)
{
  return openFTP(apn, 0, 0, server, username, password);
}

bool SIMx00::openFTP(const char *apn, const char *apnuser, const char *apnpwd,
    const char *server, const char *username, const char *password)
{
  char cmd[64];

  if (!on()) {
    goto ending;
  }

  if (!connectProlog()) {
    goto cmd_error;
  }

  if (!setBearerParms(apn, apnuser, apnpwd)) {
    goto cmd_error;
  }

  if (!sendCommandWaitForOK_P(PSTR("AT+FTPCID=1"))) {
    goto cmd_error;
  }

  // connect to FTP server
  //snprintf(cmd, sizeof(cmd), "AT+FTPSERV=\"%s\"", server);
  strcpy_P(cmd, PSTR("AT+FTPSERV=\""));
  strcat(cmd, server);
  strcat(cmd, "\"");
  if (!sendCommandWaitForOK(cmd)) {
    goto cmd_error;
  }

  // optional "AT+FTPPORT=21";
  //snprintf(cmd, sizeof(cmd), "AT+FTPUN=\"%s\"", username);
  strcpy_P(cmd, PSTR("AT+FTPUN=\""));
  strcat(cmd, username);
  strcat(cmd, "\"");
  if (!sendCommandWaitForOK(cmd)) {
    goto cmd_error;
  }
  //snprintf(cmd, sizeof(cmd), "AT+FTPPW=\"%s\"", password);
  strcpy_P(cmd, PSTR("AT+FTPPW=\""));
  strcat(cmd, password);
  strcat(cmd, "\"");
  if (!sendCommandWaitForOK(cmd)) {
    goto cmd_error;
  }

  return true;

cmd_error:
  diagPrintLn(F("openFTP failed!"));
  off();

ending:
  return false;
}

bool SIMx00::closeFTP()
{
  off();            // Ignore errors
  return true;
}

/*
 * \brief Open a (FTP) session (one file)
 */
bool SIMx00::openFTPfile(const char *fname, const char *path)
{
  char cmd[64];
  const char * ptr;
  int retry;
  uint32_t ts_max;

  // Open FTP file
  //snprintf(cmd, sizeof(cmd), "AT+FTPPUTNAME=\"%s\"", fname);
  strcpy_P(cmd, PSTR("AT+FTPPUTNAME=\""));
  strcat(cmd, fname);
  strcat(cmd, "\"");
  if (!sendCommandWaitForOK(cmd)) {
    goto ending;
  }
  //snprintf(cmd, sizeof(cmd), "AT+FTPPUTPATH=\"%s\"", FTPPATH);
  strcpy_P(cmd, PSTR("AT+FTPPUTPATH=\""));
  strcat(cmd, path);
  strcat(cmd, "\"");
  if (!sendCommandWaitForOK(cmd)) {
    goto ending;
  }

  // Repeat until we get OK
  for (retry = 0; retry < 5; retry++) {
    if (sendCommandWaitForOK_P(PSTR("AT+FTPPUT=1"))) {
      // +FTPPUT:1,1,1360  <= the 1360 is <maxlength>
      // +FTPPUT:1,61      <= this is an error (Net error)
      // +FTPPUT:1,66      <= this is an error (operation not allowed)
      // This can take a while ...
      ts_max = millis() + 30000;
      if (!waitForMessage_P(PSTR("+FTPPUT:"), ts_max)) {
        // Try again.
        isAlive();
        continue;
      }
      // Skip 8 for "+FTPPUT:"
      ptr = _inputBuffer + 8;
      ptr = skipWhiteSpace(ptr);
      if (strncmp_P(ptr, PSTR("1,"), 2) != 0) {
        // We did NOT get "+FTPPUT:1,1,", it might be an error.
        goto ending;
      }
      ptr += 2;

      if (strncmp_P(ptr, PSTR("1,"), 2) != 0) {
        // We did NOT get "+FTPPUT:1,1,", it might be an error.
        goto ending;
      }
      ptr += 2;

      _ftpMaxLength = strtoul(ptr, NULL, 0);

      break;
    }
  }
  if (retry >= 5) {
    goto ending;
  }

  return true;

ending:
  return false;
}

bool SIMx00::closeFTPfile()
{
  // Close file
  if (!sendCommandWaitForOK_P(PSTR("AT+FTPPUT=2,0"))) {
    return false;
  }

  /*
   * FIXME
   * Something weird happens here. If we wait too short (e.g. 4000)
   * then still no reply. But then when we switch off the SIM900 the
   * message +FTPPUT:1,nn message comes in, right before AT-OK or
   * +SAPBR 1: DEACT
   *
   * It is such a waste to wait that long (battery life and such).
   * The FTP file seems to be closed properly, so why bother?
   */
  // +FTPPUT:1,0
  uint32_t ts_max = millis() + 20000;
  if (!waitForMessage_P(PSTR("+FTPPUT:"), ts_max)) {
    // How bad is it if we ignore this
    //diagPrintLn(F("Timeout while waiting for +FTPPUT:1,"));
  }

  return true;
}

/*
 * \brief Lower layer function to insert a number of bytes in the FTP session
 *
 * The function sendBuffer() is the one to use. It takes care of splitting up
 * in chunks not bigger than maxlength
 */
bool SIMx00::sendFTPdata_low(uint8_t *buffer, size_t size)
{
  char cmd[20];         // Should be enough for "AT+FTPPUT=2,<num>"
  uint32_t ts_max;
  uint8_t *ptr = buffer;

  // Send some data
  //snprintf(cmd, sizeof(cmd), "AT+FTPPUT=2,%d", size);
  strcpy_P(cmd, PSTR("AT+FTPPUT=2,"));
  itoa(size, cmd + strlen(cmd), 10);
  sendCommand(cmd);

  ts_max = millis() + 10000;
  // +FTPPUT:2,22
  if (!waitForMessage_P(PSTR("+FTPPUT:"), ts_max)) {
    // How bad is it if we ignore this
    return false;
  }
  mydelay(100);           // TODO Find out if we can drop this

  // Send data ...
  for (size_t i = 0; i < size; ++i) {
    _modemStream->print((char)*ptr++);
  }
  //_modemStream->print('\r');          // dummy <CR>, not sure if this is needed

  // Expected reply:
  // +FTPPUT:2,22
  // OK
  // +FTPPUT:1,1,1360

  if (!waitForOK(5000)) {
    return false;
  }

  // The SIM900 informs again what the new max length is
  ts_max = millis() + 4000;
  // +FTPPUT:1,1,1360
  if (!waitForMessage_P(PSTR("+FTPPUT:"), ts_max)) {
    // How bad is it if we ignore this?
    // It informs us about the _ftpMaxLength
  }

  return true;
}

bool SIMx00::sendFTPdata_low(uint8_t (*read)(), size_t size)
{
  char cmd[20];         // Should be enough for "AT+FTPPUT=2,<num>"
  const char * ptr;
  uint32_t ts_max;

  // Send some data
  //snprintf(cmd, sizeof(cmd), "AT+FTPPUT=2,%d", size);
  strcpy_P(cmd, PSTR("AT+FTPPUT=2,"));
  itoa(size, cmd + strlen(cmd), 10);
  sendCommand(cmd);

  ts_max = millis() + 10000;
  // +FTPPUT:2,22
  if (!waitForMessage_P(PSTR("+FTPPUT:"), ts_max)) {
    ptr = _inputBuffer + 8;
    if (strncmp_P(ptr, PSTR("2,"), 2) != 0) {
      // We did NOT get "+FTPPUT:2,", it might be an error.
      return false;
    }
    ptr += 2;
    // TODO Check for the number
    // How bad is it if we ignore this
    return false;
  }
  mydelay(100);           // TODO Find out if we can drop this

  // Send data ...
  for (size_t i = 0; i < size; ++i) {
    _modemStream->print((char)(*read)());
  }

  // Expected reply:
  // +FTPPUT:2,22
  // OK
  // +FTPPUT:1,1,1360

  if (!waitForOK(5000)) {
    return false;
  }

  // The SIM900 informs again what the new max length is
  ts_max = millis() + 30000;
  // +FTPPUT:1,1,1360
  if (!waitForMessage_P(PSTR("+FTPPUT:"), ts_max)) {
    // How bad is it if we ignore this?
    // It informs us about the _ftpMaxLength
  }

  return true;
}

bool SIMx00::sendFTPdata(uint8_t *data, size_t size)
{
  // Send the bytes in chunks that are maximized by the maximum
  // FTP length
  while (size > 0) {
    size_t my_size = size;
    if (my_size > _ftpMaxLength) {
      my_size = _ftpMaxLength;
    }
    if (!sendFTPdata_low(data, my_size)) {
      return false;
    }
    data += my_size;
    size -= my_size;
  }
  return true;
}

bool SIMx00::sendFTPdata(uint8_t (*read)(), size_t size)
{
  // Send the bytes in chunks that are maximized by the maximum
  // FTP length
  while (size > 0) {
    size_t my_size = size;
    if (my_size > _ftpMaxLength) {
      my_size = _ftpMaxLength;
    }
    if (!sendFTPdata_low(read, my_size)) {
      return false;
    }
    size -= my_size;
  }
  return true;
}

bool SIMx00::sendSMS(const char *telno, const char *text)
{
  char cmd[64];
  uint32_t ts_max;
  bool retval = false;

  if (!on()) {
    goto ending;
  }

  // Suppress echoing
  switchEchoOff();

  // Wait for signal quality
  if (!waitForSignalQuality()) {
    goto cmd_error;
  }

  // Wait for CREG
  if (!waitForCREG()) {
    goto cmd_error;
  }

  if (!sendCommandWaitForOK_P(PSTR("AT+CMGF=1"))) {
    goto cmd_error;
  }

  strcpy_P(cmd, PSTR("AT+CMGS=\""));
  strcat(cmd, telno);
  strcat(cmd, "\"");
  sendCommand(cmd);
  ts_max = millis() + 4000;
  if (!waitForPrompt("> ", ts_max)) {
    goto cmd_error;
  }
  _modemStream->print(text); //the message itself
  _modemStream->print((char)26); //the ASCII code of ctrl+z is 26, this is needed to end the send modus and send the message.
  if (!waitForOK(30000)) {
    goto cmd_error;
  }

  retval = true;
  goto ending;

cmd_error:
  diagPrintLn(F("sendSMS failed!"));

ending:
  off();
  return retval;
}

/*!
 * \brief The middle part of the whole HTTP POST
 *
 * This function does:
 *  - HTTPPARA with the URL
 *  - HTTPPARA with the Content-Type if is passed
 *  - HTTPPARA with Userdata (header options) if is passed
 *  - HTTPDATA  
 *  - HTTPACTION(1)
 *  - Return HttpStatus if int is passed
 */
bool SIMx00::doHTTPPOSTmiddle(const char *url, const char * contentType, const char * userdata, const char *buffer, size_t len, int *responseStatus)
{
  uint32_t ts_max;
  bool retval = false;
  char num_bytes[16];

  // set http param URL value
  /*sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+HTTPPARA=\"URL\",\""));
  sendCommandAdd(url);
  sendCommandAdd('"');
  sendCommandEpilog();
  if (!waitForOK()) {
    goto ending;
  }

  if(strlen(contentType) > 0){
    sendCommandProlog();
    sendCommandAdd_P(PSTR("AT+HTTPPARA=\"CONTENT\",\""));
    sendCommandAdd(contentType);
    sendCommandAdd('"');
    sendCommandEpilog();
    if(!waitForOK()) {
      goto ending;
    }
  }

  if(strlen(userdata) > 0){
    sendCommandProlog();
    sendCommandAdd_P(PSTR("AT+HTTPPARA=\"USERDATA\",\""));
    sendCommandAdd(userdata);
    sendCommandAdd('"');
    sendCommandEpilog();
    if(!waitForOK()) {
      goto ending;
    }
  }*/

  if(!setHTTPParamsSession(url, contentType, userdata)){
    goto ending;
  }

  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+HTTPDATA="));
  itoa(len, num_bytes, 10);
  sendCommandAdd(num_bytes);
  sendCommandAdd_P(PSTR(",10000"));
  sendCommandEpilog();
  ts_max = millis() + 4000;
  if (!waitForMessage_P(PSTR("DOWNLOAD"), ts_max)) {
    goto ending;
  }

  // Send data ...
  for (size_t i = 0; i < len; ++i) {
    _modemStream->print(*buffer++);
  }

  if (!waitForOK()) {
    goto ending;
  }

  if (!doHTTPACTION(1, responseStatus)) {
    goto ending;
  }

  // All is well if we get here.
  retval = true;

ending:
  return retval;
}

/*!
 * \brief The middle part of the whole HTTP POST
 *
 * This function does:
 *  - HTTPPARA with the URL
 *  - HTTPPARA with the Content-Type if is passed
 *  - HTTPPARA with Userdata (header options) if is passed
 *  - HTTPDATA  
 *  - HTTPACTION(1)
 *  - Return HttpStatus if int is passed
 */
bool SIMx00::doHTTPPOSTmiddle(const char *url, const char * contentType, const char * userdata, Stream * streamReader, size_t len, int *responseStatus)
{
  uint32_t ts_max;
  bool retval = false;
  char num_bytes[16];

  // set http param URL value
  /*sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+HTTPPARA=\"URL\",\""));
  sendCommandAdd(url);
  sendCommandAdd('"');
  sendCommandEpilog();
  if (!waitForOK()) {
    goto ending;
  }

  if(strlen(contentType) > 0){
    sendCommandProlog();
    sendCommandAdd_P(PSTR("AT+HTTPPARA=\"CONTENT\",\""));
    sendCommandAdd(contentType);
    sendCommandAdd('"');
    sendCommandEpilog();
    if(!waitForOK()) {
      goto ending;
    }
  }

  if(strlen(userdata) > 0){
    sendCommandProlog();
    sendCommandAdd_P(PSTR("AT+HTTPPARA=\"USERDATA\",\""));
    sendCommandAdd(userdata);
    sendCommandAdd('"');
    sendCommandEpilog();
    if(!waitForOK()) {
      goto ending;
    }
  }*/

  if(!setHTTPParamsSession(url, contentType, userdata)){
    goto ending;
  }

  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+HTTPDATA="));
  itoa(len, num_bytes, 10);
  sendCommandAdd(num_bytes);
  sendCommandAdd_P(PSTR(",10000"));
  sendCommandEpilog();
  ts_max = millis() + 4000;
  if (!waitForMessage_P(PSTR("DOWNLOAD"), ts_max)) {
    goto ending;
  }

  // Send data ...
  for (size_t i = 0; i < len; ++i) {
    _modemStream->print((char)streamReader->read());
  }

  if (!waitForOK()) {
    goto ending;
  }

  if (!doHTTPACTION(1, responseStatus)) {
    goto ending;
  }

  // All is well if we get here.
  retval = true;

ending:
  return retval;
}

/*!
 * \brief The middle part of the whole HTTP POST
 *
 * This function does:
 *  - HTTPPARA with the URL
 *  - HTTPPARA with the Content-Type if is passed
 *  - HTTPPARA with Userdata (header options) if is passed
 *  - HTTPDATA  
 *  - HTTPACTION(1)
 *  - Return HttpStatus if int is passed
 */
bool SIMx00::doHTTPSPOSTmiddle(const char *url, const char * contentType, const char * userdata, const char *buffer, size_t len, int *responseStatus)
{
  uint32_t ts_max;
  bool retval = false;
  char num_bytes[16];

  if(!setHTTPParamsSession(url, contentType, userdata, true)){
    goto ending;
  }

  sendCommand_P(PSTR("AT+HTTPSSL=1"));
  if (!waitForOK()) {
    goto ending;
  }

  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+HTTPDATA="));
  itoa(len, num_bytes, 10);
  sendCommandAdd(num_bytes);
  sendCommandAdd_P(PSTR(",10000"));
  sendCommandEpilog();
  ts_max = millis() + 4000;
  if (!waitForMessage_P(PSTR("DOWNLOAD"), ts_max)) {
    goto ending;
  }

  // Send data ...
  for (size_t i = 0; i < len; ++i) {
    _modemStream->print(*buffer++);
  }

  if (!waitForOK()) {
    goto ending;
  }

  if (!doHTTPACTION(1, responseStatus)) {
    goto ending;
  }

  // All is well if we get here.
  retval = true;

ending:
  return retval;
}

/*!
 * \brief The middle part of the whole HTTP POST
 *
 * This function does:
 *  - HTTPPARA with the URL
 *  - HTTPPARA with the Content-Type if is passed
 *  - HTTPPARA with Userdata (header options) if is passed
 *  - HTTPDATA  
 *  - HTTPACTION(1)
 *  - Return HttpStatus if int is passed
 */
bool SIMx00::doHTTPSPOSTmiddle(const char *url, const char * contentType, const char * userdata, Stream * streamReader, size_t len, int *responseStatus)
{
  uint32_t ts_max;
  bool retval = false;
  char num_bytes[16];

  if(!setHTTPParamsSession(url, contentType, userdata, true)){
    goto ending;
  }

  sendCommand_P(PSTR("AT+HTTPSSL=1"));
  if (!waitForOK()) {
    goto ending;
  }

  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+HTTPDATA="));
  itoa(len, num_bytes, 10);
  sendCommandAdd(num_bytes);
  sendCommandAdd_P(PSTR(",10000"));
  sendCommandEpilog();
  ts_max = millis() + 4000;
  if (!waitForMessage_P(PSTR("DOWNLOAD"), ts_max)) {
    goto ending;
  }

  // Send data ...
  for (size_t i = 0; i < len; ++i) {
    _modemStream->print((char)streamReader->read());
  }

  if (!waitForOK()) {
    goto ending;
  }

  if (!doHTTPACTION(1, responseStatus)) {
    goto ending;
  }

  // All is well if we get here.
  retval = true;

ending:
  return retval;
}

/*!
 * \brief The middle part of the whole HTTP POST, with a READ
 *
 * This function does:
 *  - doHTTPPOSTmiddle() ...
 *  - HTTPREAD
 */
bool SIMx00::doHTTPPOSTmiddleWithReply(const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int *responseStatus, char *buffer, size_t len)
{
  bool retval = false;;

  if (!doHTTPPOSTmiddle(url, contentType, userdata, postdata, pdlen, responseStatus)) {
    goto ending;
  }

  // Read all data
  if (!doHTTPREAD(buffer, len)) {
    goto ending;
  }

  // All is well if we get here.
  retval = true;

ending:
    return retval;
}



/*!
 * \brief The middle part of the whole HTTP POST, with a READ
 *
 * This function does:
 *  - doHTTPPOSTmiddle() ...
 *  - HTTPREAD
 */
bool SIMx00::doHTTPPOSTmiddleWithReply(const char *url, const char * contentType, const char * userdata, Stream * streamReader, size_t pdlen, int *responseStatus, char *buffer, size_t len)
{
  bool retval = false;;

  if (!doHTTPPOSTmiddle(url, contentType, userdata, streamReader, pdlen, responseStatus)) {
    goto ending;
  }

  // Read all data
  if (!doHTTPREAD(buffer, len)) {
    goto ending;
  }

  // All is well if we get here.
  retval = true;

ending:
    return retval;
}

/*!
 * \brief The middle part of the whole HTTP POST, with a READ
 *
 * This function does:
 *  - doHTTPSPOSTmiddle() ...
 *  - HTTPREAD
 */
bool SIMx00::doHTTPSPOSTmiddleWithReply(const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int *responseStatus, char *buffer, size_t len)
{
  bool retval = false;;

  if (!doHTTPSPOSTmiddle(url, contentType, userdata, postdata, pdlen, responseStatus)) {
    goto ending;
  }

  // Read all data
  if (!doHTTPREAD(buffer, len)) {
    goto ending;
  }

  // All is well if we get here.
  retval = true;

ending:
    return retval;
}

/*!
 * \brief The middle part of the whole HTTP POST, with a READ
 *
 * This function does:
 *  - doHTTPPOSTmiddle() ...
 *  - HTTPREAD
 */
bool SIMx00::doHTTPSPOSTmiddleWithReply(const char *url, const char * contentType, const char * userdata, Stream * streamReader, size_t pdlen, int *responseStatus, char *buffer, size_t len)
{
  bool retval = false;;

  if (!doHTTPSPOSTmiddle(url, contentType, userdata, streamReader, pdlen, responseStatus)) {
    goto ending;
  }

  // Read all data
  if (!doHTTPREAD(buffer, len)) {
    goto ending;
  }

  // All is well if we get here.
  retval = true;

ending:
    return retval;
}

/*!
 * \brief The middle part of the whole HTTP GET
 *
 * This function does:
 *  - HTTPPARA with the URL
 *  - HTTPACTION(0)
 *  - HTTPREAD
 */
bool SIMx00::doHTTPGETmiddle(const char *url, char *buffer, size_t len)
{
  bool retval = false;

  // set http param URL value
  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+HTTPPARA=\"URL\",\""));
  sendCommandAdd(url);
  sendCommandAdd('"');
  sendCommandEpilog();
  if (!waitForOK()) {
    goto ending;
  }

  if (!doHTTPACTION(0, NULL)) {
    goto ending;
  }

  // Read all data
  if (!doHTTPREAD(buffer, len)) {
    goto ending;
  }

  // All is well if we get here.
  retval = true;

ending:
  return retval;
}

bool SIMx00::doHTTPprolog(const char *apn)
{
  return doHTTPprolog(apn, 0, 0);
}

bool SIMx00::doHTTPprolog(const char *apn, const char *apnuser, const char *apnpwd)
{
  bool retval = false;

  if (!connectProlog()) {
    goto ending;
  }

  if (!setBearerParms(apn, apnuser, apnpwd)) {
    goto ending;
  }

  // initialize http service
  if (!sendCommandWaitForOK_P(PSTR("AT+HTTPINIT"))) {
    goto ending;
  }

  // set http param CID value
  // FIXME Do we really need this?
  if (!sendCommandWaitForOK_P(PSTR("AT+HTTPPARA=\"CID\",1"))) {
    goto ending;
  }

  retval = true;

ending:
  return retval;
}

void SIMx00::doHTTPepilog()
{
  if (!sendCommandWaitForOK_P(PSTR("AT+HTTPTERM"))) {
    // This is an error, but we can still return success.
  }
}

/*
 * \brief Read the data from a GET or POST
 */
bool SIMx00::doHTTPREAD(char *buffer, size_t len)
{
  uint32_t ts_max;
  size_t getLength = 0;
  int i;
  bool retval = false;

  // Expect
  //   +HTTPREAD:<date_len>
  //   <data>
  //   OK
  sendCommand_P(PSTR("AT+HTTPREAD"));
  ts_max = millis() + 8000;
  if (waitForMessage_P(PSTR("+HTTPREAD:"), ts_max)) {
    const char *ptr = _inputBuffer + 10;
    char *bufend;
    getLength = strtoul(ptr, &bufend, 0);
    if (bufend == ptr) {
      // Invalid number
      goto ending;
    }
  } else {
    // Hmm. Why didn't we get this?
    goto ending;
  }
  // Read the data
  retval = true;                // assume this will succeed
  ts_max = millis() + 4000;
  i = readBytes(getLength, (uint8_t *)buffer, len, ts_max);
  if (i != 0) {
    // We didn't get the bytes that we expected
    // Still wait for OK
    retval = false;
  } else {
    // The read was successful. readBytes made sure it was terminated.
  }
  if (!waitForOK()) {
    // This is an error, but we can still return success.
  }

  // All is well if we get here.

ending:
  return retval;
}

bool SIMx00::doHTTPACTION(char num, int * status)
{
  uint32_t ts_max;
  bool retval = false;

  // set http action type 0 = GET, 1 = POST, 2 = HEAD
  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+HTTPACTION="));
  sendCommandAdd((int)num);
  sendCommandEpilog();
  if (!waitForOK()) {
    goto ending;
  }
  // Now we're expecting something like this: +HTTPACTION: <Method>,<StatusCode>,<DataLen>
  // <Method> 0
  // <StatusCode> 200
  // <DataLen> ??
  ts_max = millis() + 20000;
  if (waitForMessage_P(PSTR("+HTTPACTION:"), ts_max)) {
    // SIM900 responds with: "+HTTPACTION:1,200,11"
    // SIM800 responds with: "+HTTPACTION: 1,200,11"
    // The 12 is the length of "+HTTPACTION:"
    // We then have to skip the digit and the comma
    const char *ptr = _inputBuffer + 12;
    ptr = skipWhiteSpace(ptr);
    ++ptr;              // The digit
    ++ptr;              // The comma
    char *bufend;
    uint16_t replycode = strtoul(ptr, &bufend, 10);
    if (bufend == ptr) {
      // Invalid number
      goto ending;
    }

    if(status != NULL){
      *status = replycode;
      retval = true;
    } else if (replycode == 200) {
      // TODO Which result codes are allowed to pass?
      retval = true;
    } else {
      // Everything else is considered an error
    }
  }

  // All is well if we get here.

ending:
  return retval;
}

bool SIMx00::setHTTPParamsSession(const char * url, const char * contentType, const char * userdata, bool redir){
  uint32_t ts_max;
  bool retval = false;
  char num_bytes[16];

  // set http param URL value
  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+HTTPPARA=\"URL\",\""));
  sendCommandAdd(url);
  sendCommandAdd('"');
  sendCommandEpilog();
  if (!waitForOK()) {
    goto ending;
  }

  if(strlen(contentType) > 0){
    sendCommandProlog();
    sendCommandAdd_P(PSTR("AT+HTTPPARA=\"CONTENT\",\""));
    sendCommandAdd(contentType);
    sendCommandAdd('"');
    sendCommandEpilog();
    if(!waitForOK()) {
      goto ending;
    }
  }

  if(strlen(userdata) > 0){
    sendCommandProlog();
    sendCommandAdd_P(PSTR("AT+HTTPPARA=\"USERDATA\",\""));
    sendCommandAdd(userdata);
    sendCommandAdd('"');
    sendCommandEpilog();
    if(!waitForOK()) {
      goto ending;
    }
  }

  if(redir){
    sendCommand_P(PSTR("AT+HTTPPARA=\"REDIR\",1"));
    if(!waitForOK()) {
      goto ending;
    }
  }

  retval = true;

ending:
  return retval;
}

bool SIMx00::doHTTPPOST(const char *apn, const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus)
{
  return doHTTPPOST(apn, 0, 0, url, contentType, userdata, postdata, pdlen, responseStatus);
}

bool SIMx00::doHTTPPOST(const char *apn, const char *apnuser, const char *apnpwd,
    const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus)
{
  bool retval = false;

  if (!on()) {
    goto ending;
  }

  if (!doHTTPprolog(apn, apnuser, apnpwd)) {
    goto cmd_error;
  }

  if (!doHTTPPOSTmiddle(url, contentType, userdata, postdata, pdlen, responseStatus)) {
    goto cmd_error;
  }

  retval = true;
  doHTTPepilog();
  goto ending;

cmd_error:
  diagPrintLn(F("doHTTPPOST failed!"));

ending:
  off();
  return retval;
}


bool SIMx00::doHTTPPOSTWithReply(const char *apn,
    const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus, char *buffer, size_t len)
{
  return doHTTPPOSTWithReply(apn, 0, 0, url, contentType, userdata, postdata, pdlen, responseStatus, buffer, len);
}

bool SIMx00::doHTTPPOSTWithReply(const char *apn, const char *apnuser, const char *apnpwd,
    const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus, char *buffer, size_t len)
{
  bool retval = false;

  if (!on()) {
    goto ending;
  }

  if (!doHTTPprolog(apn, apnuser, apnpwd)) {
    goto cmd_error;
  }

  if (!doHTTPPOSTmiddleWithReply(url, contentType, userdata, postdata, pdlen, responseStatus, buffer, len)) {
    goto cmd_error;
  }

  retval = true;
  doHTTPepilog();
  goto ending;

cmd_error:
  diagPrintLn(F("doHTTPGET failed!"));

ending:
  off();
  return retval;
}

bool SIMx00::doHTTPGET(const char *apn, const char *url, char *buffer, size_t len)
{
  return doHTTPGET(apn, 0, 0, url, buffer, len);
}

bool SIMx00::doHTTPGET(const char *apn, const String & url, char *buffer, size_t len)
{
  return doHTTPGET(apn, 0, 0, url.c_str(), buffer, len);
}

bool SIMx00::doHTTPGET(const char *apn, const char *apnuser, const char *apnpwd,
    const char *url, char *buffer, size_t len)
{
  bool retval = false;

  if (!on()) {
    goto ending;
  }

  if (!doHTTPprolog(apn, apnuser, apnpwd)) {
    goto cmd_error;
  }

  if (!doHTTPGETmiddle(url, buffer, len)) {
    goto cmd_error;
  }

  retval = true;
  doHTTPepilog();
  goto ending;

cmd_error:
  diagPrintLn(F("doHTTPGET failed!"));

ending:
  off();
  return retval;
}

bool SIMx00::setBearerParms(const char *apn, const char *user, const char *pwd)
{
  char cmd[64];
  bool retval = false;
  int retry;

  // SAPBR=3 Set bearer parameters
  if (!sendCommandWaitForOK_P(PSTR("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\""))) {
    goto ending;
  }

  // SAPBR=3 Set bearer parameters
  strcpy_P(cmd, PSTR("AT+SAPBR=3,1,\"APN\",\""));
  strcat(cmd, apn);
  strcat(cmd, "\"");
  if (!sendCommandWaitForOK(cmd)) {
    goto ending;
  }
  if (user && user[0]) {
    strcpy_P(cmd, PSTR("AT+SAPBR=3,1,\"USER\",\""));
    strcat(cmd, user);
    strcat(cmd, "\"");
    if (!sendCommandWaitForOK(cmd)) {
      goto ending;
    }
  }
  if (pwd && pwd[0]) {
    strcpy_P(cmd, PSTR("AT+SAPBR=3,1,\"PWD\",\""));
    strcat(cmd, pwd);
    strcat(cmd, "\"");
    if (!sendCommandWaitForOK(cmd)) {
      goto ending;
    }
  }

  // SAPBR=1 Open bearer
  // This command can fail if signal quality is low, or if we're too fast
  for (retry = 0; retry < 5; retry++) {
    if (sendCommandWaitForOK_P(PSTR("AT+SAPBR=1,1"),10000)) {
      break;
    }
  }
  if (retry >= 5) {
    goto ending;
  }

  // SAPBR=2 Query bearer
  // Expect +SAPBR: <cid>,<Status>,<IP_Addr>
  if (!sendCommandWaitForOK_P(PSTR("AT+SAPBR=2,1"))) {
    goto ending;
  }

  retval = true;

ending:
  return retval;
}

bool SIMx00::getIMEI(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 2000;
  return getStrValue("AT+GSN", buffer, buflen, ts_max);
}

bool SIMx00::getGCAP(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 2000;
  return getStrValue_P(PSTR("AT+GCAP"), PSTR("+GCAP:"), buffer, buflen, ts_max);
}

bool SIMx00::getCIMI(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 2000;
  return getStrValue("AT+CIMI", buffer, buflen, ts_max);
}

bool SIMx00::getCCID(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 2000;
  return getStrValue("AT+CCID", buffer, buflen, ts_max);
}

bool SIMx00::getCLIP(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 4000;
  return getStrValue_P(PSTR("AT+CLIP?"), PSTR("+CLIP:"), buffer, buflen, ts_max);
}

bool SIMx00::getCLIR(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 4000;
  return getStrValue_P(PSTR("AT+CLIR?"), PSTR("+CLIR:"), buffer, buflen, ts_max);
}

bool SIMx00::getCOLP(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 4000;
  return getStrValue_P(PSTR("AT+COLP?"), PSTR("+COLP:"), buffer, buflen, ts_max);
}

bool SIMx00::getCOPS(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 4000;
  return getStrValue_P(PSTR("AT+COPS?"), PSTR("+COPS:"), buffer, buflen, ts_max);
}

bool SIMx00::setCCLK(const SIMCOMDateTime & dt)
{
  String str;
  str.reserve(30);
  dt.addToString(str);
  switchEchoOff();
  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+CCLK=\""));
  sendCommandAdd(str);
  sendCommandAdd('"');
  sendCommandEpilog();
  return waitForOK();
}

bool SIMx00::getCCLK(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 4000;
  return getStrValue_P(PSTR("AT+CCLK?"), PSTR("+CCLK:"), buffer, buflen, ts_max);
}

bool SIMx00::getCSPN(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 4000;
  return getStrValue_P(PSTR("AT+CSPN?"), PSTR("+CSPN:"), buffer, buflen, ts_max);
}

bool SIMx00::getCGID(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 4000;
  return getStrValue_P(PSTR("AT+CGID"), PSTR("+GID:"), buffer, buflen, ts_max);
}

bool SIMx00::setCIURC(uint8_t value)
{
  switchEchoOff();
  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+CIURC="));
  sendCommandAdd((int)value);
  sendCommandEpilog();
  return waitForOK();
}

bool SIMx00::getCIURC(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 4000;
  return getStrValue_P(PSTR("AT+CIURC?"), PSTR("+CIURC:"), buffer, buflen, ts_max);
}

/*
 * \brief Set the AT+CFUN value (Set Phone Functionality)
 *
 * Allowed values are
 * - 0 Minimum functionality
 * - 1 Full functionality (Default)
 * - 4 Disable phone both transmit and receive RF circuits
 */
bool SIMx00::setCFUN(uint8_t value)
{
  switchEchoOff();
  sendCommandProlog();
  sendCommandAdd_P(PSTR("AT+CFUN="));
  sendCommandAdd((int)value);
  sendCommandEpilog();
  return waitForOK();
}

bool SIMx00::getCFUN(uint8_t * value)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 4000;
  int tmpValue;
  bool status;
  status = getIntValue_P(PSTR("AT+CFUN?"), PSTR("+CFUN:"), &tmpValue, ts_max);
  if (status) {
    *value = (uint8_t)tmpValue;
  }
  return status;
}

void SIMx00::enableLTS()
{
  if (!sendCommandWaitForOK_P(PSTR("AT+CLTS=1"), 6000)) {
  }
}

void SIMx00::disableLTS()
{
  if (!sendCommandWaitForOK_P(PSTR("AT+CLTS=0"), 6000)) {
  }
}

void SIMx00::enableCIURC()
{
  if (!sendCommandWaitForOK_P(PSTR("AT+CIURC=1"), 6000)) {
  }
}

void SIMx00::disableCIURC()
{
  if (!sendCommandWaitForOK_P(PSTR("AT+CIURC=0"), 6000)) {
  }
}

/*!
 * \brief Get Product Identification Information
 *
 * Send the ATI command and get the result.
 * SIM900 is expected to return something like:
 *    SIM900 R11.0
 * SIM800 is expected to return something like:
 *    SIM800 R11.08
 */
bool SIMx00::getPII(char *buffer, size_t buflen)
{
  switchEchoOff();
  uint32_t ts_max = millis() + 2000;
  return getStrValue("ATI", buffer, buflen, ts_max);
}

void SIMx00::setProductId()
{
  char buffer[64];
  if (getPII(buffer, sizeof(buffer))) {
    if (strncmp_P(buffer, PSTR("SIM900"), 6) == 0) {
      _productId = prodid_SIM900;
    }
    else if (strncmp_P(buffer, PSTR("SIM800"), 6) == 0) {
      _productId = prodid_SIM800;
    }
  }
}

const char * SIMx00::skipWhiteSpace(const char * txt)
{
  while (*txt != '\0' && *txt == ' ') {
    ++txt;
  }
  return txt;
}

uint32_t SIMx00::getUnixEpoch()
{
  bool status;
  char buffer[64];

  status = false;
  for (uint8_t ix = 0; !status && ix < 10; ++ix) {
    status = on();
  }

  status = false;
  for (uint8_t ix = 0; !status && ix < 10; ++ix) {
    status = getCCLK(buffer, sizeof(buffer));
  }

  const char * ptr = buffer;
  if (*ptr == '"') {
    ++ptr;
  }
  SIMCOMDateTime dt = SIMCOMDateTime(ptr);

  return dt.getUnixEpoch();
}