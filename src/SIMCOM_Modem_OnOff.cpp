#include "Arduino.h"
#include <stdint.h>

#include "SIMCOM_Modem_OnOff.h"
////////////////////////////////////////////////////////////////////////////////
////////////////////    GPRSbeeOnOff       /////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

GPRSBeeOnOff::GPRSBeeOnOff()
{
    _vcc33Pin = -1;
    _onoffPin = -1;
    _statusPin = -1;
}

// Initializes the instance
void GPRSBeeOnOff::init(int8_t vcc33Pin, int8_t onoffPin, int8_t statusPin)
{
    if (vcc33Pin >= 0) {
      _vcc33Pin = vcc33Pin;
      // First write the output value, and only then set the output mode.
      digitalWrite(_vcc33Pin, LOW);
      pinMode(_vcc33Pin, OUTPUT);
    }

    if (onoffPin >= 0) {
      _onoffPin = onoffPin;
      // First write the output value, and only then set the output mode.
      digitalWrite(_onoffPin, LOW);
      pinMode(_onoffPin, OUTPUT);
    }

    if (statusPin >= 0) {
      _statusPin = statusPin;
      pinMode(_statusPin, INPUT);
    }
}

void GPRSBeeOnOff::on()
{
    // First VCC 3.3 HIGH
    if (_vcc33Pin >= 0) {
        digitalWrite(_vcc33Pin, HIGH);
    }

    // Wait a little
    // TODO Figure out if this is really needed
    delay(2);
    if (_onoffPin >= 0) {
        digitalWrite(_onoffPin, HIGH);
    }
}

void GPRSBeeOnOff::off()
{
    if (_vcc33Pin >= 0) {
        digitalWrite(_vcc33Pin, LOW);
    }

    // The GPRSbee is switched off immediately
    if (_onoffPin >= 0) {
        digitalWrite(_onoffPin, LOW);
    }

    // Should be instant
    // Let's wait a little, but not too long
    delay(50);
}

bool GPRSBeeOnOff::isOn()
{
    if (_statusPin >= 0) {
        bool status = digitalRead(_statusPin);
        return status;
    }

    // No status pin. Let's assume it is on.
    return true;
}