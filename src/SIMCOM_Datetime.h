
#ifndef SIMCOM_DATETIME_H_
#define SIMX00_DATETIME_H_

#include <Arduino.h>
#include <stdint.h>
/*
 * \brief A class to store clock values
 */
class SIMCOMDateTime
{
public:
  SIMCOMDateTime(uint32_t ts=0);
  SIMCOMDateTime(uint8_t y, uint8_t m, uint8_t d, uint8_t hh, uint8_t mm, uint8_t ss, int8_t tz=0);
  SIMCOMDateTime(const char * cclk);

  enum _WEEK_DAYS_ {
    SUNDAY,
    MONDAY,
    TUESDAY,
    WEDNESDAY,
    THURSDAY,
    FRIDAY,
    SATURDAY
  };

  uint16_t      year() { return _yOff + 2000; }
  uint8_t       month() { return _m + 1; }
  uint8_t       day() { return _d + 1; }
  uint8_t       hour() { return _hh; }
  uint8_t       minute() { return _mm; }
  uint8_t       second() { return _ss; }

  // 32-bit number of seconds since Unix epoch (1970-01-01)
  uint32_t getUnixEpoch() const;
  // 32-bit number of seconds since Y2K epoch (2000-01-01)
  uint32_t getY2KEpoch() const;

  void addToString(String & str) const;

private:
  uint8_t       conv1d(const char * txt);
  uint8_t       conv2d(const char * txt);

  uint8_t       _yOff;          // Year value minus 2000
  uint8_t       _m;             // month (0..11)
  uint8_t       _d;             // day (0..30)
  uint8_t       _hh;            // hour (0..23)
  uint8_t       _mm;            // minute (0..59)
  uint8_t       _ss;            // second (0..59)
  int8_t        _tz;            // timezone (multiple of 15 minutes)
};
#endif