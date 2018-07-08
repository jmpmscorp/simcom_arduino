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

#ifndef SIMX00_H_
#define SIMX00_H_

#include <stdint.h>
#include <Arduino.h>
#include <Stream.h>

#include "SIMCOM_Modem.h"


// Comment this line, or make it an undef to disable
// diagnostic
#define ENABLE_GPRSBEE_DIAG     1

/*!
 * \def SIM900_DEFAULT_BUFFER_SIZE
 *
 * The GPRSbee class uses an internal buffer to read lines from
 * the SIMx00. The buffer is allocated in .init() and the default
 * size is what this define is set to.
 *
 * The function .readline() is the only function that writes to this
 * internal buffer.
 *
 * Other functions (such as receiveLineTCP) can make use of .readline
 * and sometimes it is necessary that the buffer is much bigger. Please
 * be aware that the buffer is allocated once and never freed.
 *
 * You can make it allocate a bigger buffer by calling .setBufSize()
 * before doing the .init()
 */




class SIMx00 : public SIMCOM_Modem
{
public:
  void initNdogoSIM800(Stream &stream, int pwrkeyPin, int vbatPin, int statusPin,
      int bufferSize=SIMCOM_MODEM_DEFAULT_BUFFER_SIZE);
  /*void initAutonomoSIM800(Stream &stream, int vcc33Pin, int onoffPin, int statusPin,
      int bufferSize=SIMCOM_MODEM_DEFAULT_BUFFER_SIZE);*/

  void init(Stream & stream, SIMCOM_Modem_OnOff &onoff, int bufferSize = SIMCOM_MODEM_DEFAULT_BUFFER_SIZE);

  void setSkipCGATT(bool x=true)        { _skipCGATT = x; _changedSkipCGATT = true; }

  bool networkOn();

  bool doHTTPPOST(const char *apn, const char *url, const char *contentType, const char *userdata, const char *postdata, size_t pdlen, int * responseStatus);
  bool doHTTPPOST(const char *apn, const String & url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus);
  bool doHTTPPOST(const char *apn, const char *apnuser, const char *apnpwd,
      const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus);
  
  bool doHTTPPOSTmiddle(const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus);
  bool doHTTPPOSTmiddle(const char *url, const char * contentType, const char * userdata, Stream * streamReader, size_t pdlen, int * responseStatus);
  bool doHTTPSPOSTmiddle(const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus);
  bool doHTTPSPOSTmiddle(const char *url, const char * contentType, const char * userdata, Stream * streamReader, size_t pdlen, int * responseStatus);

  bool doHTTPPOSTmiddleWithReply(const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus, char *buffer, size_t len);
  bool doHTTPPOSTmiddleWithReply(const char *url, const char * contentType, const char * userdata, Stream * streamReader, size_t pdlen, int * responseStatus, char *buffer, size_t len);

  bool doHTTPSPOSTmiddleWithReply(const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus, char *buffer, size_t len);
  bool doHTTPSPOSTmiddleWithReply(const char *url, const char * contentType, const char * userdata, Stream * streamReader, size_t pdlen, int * responseStatus, char *buffer, size_t len);

  bool doHTTPPOSTWithReply(const char *apn, const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus, char *buffer, size_t len);
  bool doHTTPPOSTWithReply(const char *apn, const String & url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus, char *buffer, size_t len);
  bool doHTTPPOSTWithReply(const char *apn, const char *apnuser, const char *apnpwd,
      const char *url, const char * contentType, const char * userdata, const char *postdata, size_t pdlen, int * responseStatus, char *buffer, size_t len);

  bool doHTTPGET(const char *apn, const char *url, char *buffer, size_t len);
  bool doHTTPGET(const char *apn, const String & url, char *buffer, size_t len);
  bool doHTTPGET(const char *apn, const char *apnuser, const char *apnpwd,
      const char *url, char *buffer, size_t len);
  bool doHTTPGETmiddle(const char *url, char *buffer, size_t len);

  bool doHTTPREAD(char *buffer, size_t len);
  bool doHTTPACTION(char num, int * responseStatus);

  bool setHTTPParamsSession(const char * url, const char * contentType, const char * userdata, bool redir = false);

  bool doHTTPprolog(const char *apn);
  bool doHTTPprolog(const char *apn, const char *apnuser, const char *apnpwd);
  void doHTTPepilog();

  bool openTCP(const char *apn, const char *server, int port, bool transMode=false);
  bool openTCP(const char *apn, const char *apnuser, const char *apnpwd,
      const char *server, int port, bool transMode=false);
  void closeTCP(bool switchOff=true);
  bool isTCPConnected();
  bool sendDataTCP(const uint8_t *data, size_t data_len);
  bool receiveDataTCP(uint8_t *data, size_t data_len, uint16_t timeout=4000);
  bool receiveLineTCP(const char **buffer, uint16_t timeout=4000);

  bool openFTP(const char *apn, const char *server,
      const char *username, const char *password);
  bool openFTP(const char *apn, const char *apnuser, const char *apnpwd,
      const char *server, const char *username, const char *password);
  bool closeFTP();
  bool openFTPfile(const char *fname, const char *path);
  bool sendFTPdata(uint8_t *data, size_t size);
  bool sendFTPdata(uint8_t (*read)(), size_t size);
  bool closeFTPfile();

  bool sendSMS(const char *telno, const char *text);

  // Get the Received Signal Strength Indication and Bit Error Rate
  bool getRSSIAndBER(int8_t* rssi, uint8_t* ber);

  bool getIMEI(char *buffer, size_t buflen);
  bool getGCAP(char *buffer, size_t buflen);
  bool getCIMI(char *buffer, size_t buflen);
  bool getCCID(char *buffer, size_t buflen);
  bool getCLIP(char *buffer, size_t buflen);
  bool getCLIR(char *buffer, size_t buflen);
  bool getCOLP(char *buffer, size_t buflen);
  bool getCOPS(char *buffer, size_t buflen);
  bool setCCLK(const SIMCOMDateTime & dt);
  bool getCCLK(char *buffer, size_t buflen);
  bool getCSPN(char *buffer, size_t buflen);
  bool getCGID(char *buffer, size_t buflen);
  bool setCIURC(uint8_t value);
  bool getCIURC(char *buffer, size_t buflen);
  bool setCFUN(uint8_t value);
  bool getCFUN(uint8_t * value);

  void enableCIURC();
  void disableCIURC();
  void enableLTS();
  void disableLTS();  

  // Using CCLK, get 32-bit number of seconds since Unix epoch (1970-01-01)
  uint32_t getUnixEpoch();
  // Using CCLK, get 32-bit number of seconds since Y2K epoch (2000-01-01)
  uint32_t getY2KEpoch();

  // Getters of diagnostic values
  uint32_t getTimeToOpenTCP() { return _timeToOpenTCP; }
  uint32_t getTimeToCloseTCP() { return _timeToCloseTCP; }

protected:
  void initProlog(Stream &stream, size_t bufferSize);

  void onToggle();
  void offToggle();
  void onSwitchMbiliJP2();
  void offSwitchMbiliJP2();
  void onSwitchNdogoSIM800();
  void offSwitchNdogoSIM800();
  void onSwitchAutonomoSIM800();
  void offSwitchAutonomoSIM800();

  bool isAlive();
  bool isOn();
  void toggle();

  void switchEchoOff();  

  bool connectProlog();
  bool waitForSignalQuality();
  bool waitForCREG();
  bool setBearerParms(const char *apn, const char *user, const char *pwd);

  bool getPII(char *buffer, size_t buflen);
  void setProductId();

  

  const char * skipWhiteSpace(const char * txt);

  bool sendFTPdata_low(uint8_t *buffer, size_t size);
  bool sendFTPdata_low(uint8_t (*read)(), size_t size);
  
  size_t _ftpMaxLength;
  bool _transMode;
  bool _skipCGATT;
  bool _changedSkipCGATT;		// This is set when the user has changed it.
  enum productIdKind {
    prodid_unknown,
    prodid_SIM900,
    prodid_SIM800,
  };
  enum productIdKind _productId;

  uint32_t _timeToOpenTCP;
  uint32_t _timeToCloseTCP;

};

//extern SIMx00 gprsbee;

#endif /* SIMX00_H_ */
