#include "SIMCOM_Datetime.h"

SIMCOMDateTime::SIMCOMDateTime(uint8_t y, uint8_t m, uint8_t d, uint8_t hh, uint8_t mm, uint8_t ss, int8_t tz)
{
  _yOff = y;
  _m = m;
  _d = d;
  _hh = hh;
  _mm = mm;
  _ss = ss;
  _tz = tz;
}

/*
 * \brief Construct from a timestamp (seconds since Y2K Epoch)
 */
SIMCOMDateTime::SIMCOMDateTime(uint32_t ts)
{
  // whole and fractional parts of 1 day
  uint16_t days = ts / 86400UL;
  int32_t fract = ts % 86400UL;
  //Serial.print(F("days ")); Serial.println(days);

  // Extract hour, minute, and second from the fractional day
  ldiv_t lresult = ldiv(fract, 60L);
  _ss = lresult.rem;
  div_t result = div(lresult.quot, 60);
  _mm = result.rem;
  _hh = result.quot;

  //
  uint16_t n = days + SATURDAY;
  n %= 7;
  //Serial.print(F("wday ")); Serial.println(n);
  // _wday = n;

  // map into a 100 year cycle
  lresult = ldiv((long) days, 36525L);
  uint16_t years = 100 * lresult.quot;
  //Serial.print(F("years ")); Serial.println(years);

  // map into a 4 year cycle
  lresult = ldiv(lresult.rem, 1461L);
  years += 4 * lresult.quot;
  days = lresult.rem;
  if (years > 100) {
    ++days;
  }

  // 'years' is now at the first year of a 4 year leap cycle, which will always be a leap year,
  // unless it is 100. 'days' is now an index into that cycle.
  uint8_t leapyear = 1;
  if (years == 100) {
    leapyear = 0;
  }

  // compute length, in days, of first year of this cycle
  n = 364 + leapyear;

  /*
   * if the number of days remaining is greater than the length of the
   * first year, we make one more division.
   */
  if (days > n) {
      days -= leapyear;
      leapyear = 0;
      result = div(days, 365);
      years += result.quot;
      days = result.rem;
  }
  //Serial.print(F("years ")); Serial.println(years);
  _yOff = years;
  //Serial.print(F("days ")); Serial.println(days);
  // _ydays = days;

  /*
   * Given the year, day of year, and leap year indicator, we can break down the
   * month and day of month. If the day of year is less than 59 (or 60 if a leap year), then
   * we handle the Jan/Feb month pair as an exception.
   */
  n = 59 + leapyear;
  if (days < n) {
      /* special case: Jan/Feb month pair */
      result = div(days, 31);
      _m = result.quot;
      _d = result.rem;
  } else {
      /*
       * The remaining 10 months form a regular pattern of 31 day months alternating with 30 day
       * months, with a 'phase change' between July and August (153 days after March 1).
       * We proceed by mapping our position into either March-July or August-December.
       */
      days -= n;
      result = div(days, 153);
      _m = 2 + result.quot * 5;

      /* map into a 61 day pair of months */
      result = div(result.rem, 61);
      _m += result.quot * 2;

      /* map into a month */
      result = div(result.rem, 31);
      _m += result.quot;
      _d = result.rem;
  }

  _tz = 0;
}

/*
 * \brief Construct using a text string as received from AT+CCLK
 *
 * format is "yy/MM/dd,hh:mm:ss±zz"
 *
 * No serious attempt is made to validate the string. Whatever comes
 * in is used as is. Each number is assumed to have two digits.
 *
 * Also, you year number is assumed to be the offset of 2000
 *
 * Example input string: 04/01/02,00:47:32+04
 */
SIMCOMDateTime::SIMCOMDateTime(const char * cclk)
{
  _yOff = conv2d(cclk);
  cclk += 3;
  _m = conv2d(cclk) - 1;                // Month is 0 based
  cclk += 3;
  _d = conv2d(cclk) - 1;                // Day is 0 based
  cclk += 3;
  _hh = conv2d(cclk);
  cclk += 3;
  _mm = conv2d(cclk);
  cclk += 3;
  _ss = conv2d(cclk);
  cclk += 2;
  uint8_t isNeg = *cclk == '-';
  ++cclk;
  _tz = conv2d(cclk);
  if (isNeg) {
    _tz = -_tz;
  }
}

static const uint8_t daysInMonth [] PROGMEM = { 31,28,31,30,31,30,31,31,30,31,30,31 };

/*
 * \brief Compute the Y2K Epoch from the date and time
 */
uint32_t SIMCOMDateTime::getY2KEpoch() const
{
  uint32_t ts;
  uint16_t days = _d + (365 * _yOff) + ((_yOff + 3) / 4);
  // Add the days of the previous months in this year.
  for (uint8_t i = 0; i < _m; ++i) {
    days += pgm_read_byte(daysInMonth + i);
  }
  if ((_m > 2) && ((_yOff % 4) == 0)) {
    ++days;
  }

  ts = ((uint32_t)days * 24) + _hh;
  ts = (ts * 60) + _mm;
  ts = (ts * 60) + _ss;

  ts = ts - (_tz * 15 * 60);

  return ts;
}

/*
 * \brief Compute the UNIX Epoch from the date and time
 */
uint32_t SIMCOMDateTime::getUnixEpoch() const
{
  return getY2KEpoch() + 946684800;
}

/*
 * \brief Convert a single digit to a number
 */
uint8_t SIMCOMDateTime::conv1d(const char * txt)
{
  uint8_t       val = 0;
  if (*txt >= '0' && *txt <= '9') {
    val = *txt - '0';
  }
  return val;
}

/*
 * \brief Convert two digits to a number
 */
uint8_t SIMCOMDateTime::conv2d(const char * txt)
{
  uint8_t val = conv1d(txt++) * 10;
  val += conv1d(txt);
  return val;
}

/*
 * Format an integer as %0*d
 *
 * Arduino formatting sucks.
 */
static void add0Nd(String &str, uint16_t val, size_t width)
{
  if (width >= 5 && val < 10000) {
    str += '0';
  }
  if (width >= 4 && val < 1000) {
    str += '0';
  }
  if (width >= 3 && val < 100) {
    str += '0';
  }
  if (width >= 2 && val < 10) {
    str += '0';
  }
  str += val;
}

/*
 * \brief Add to the String the text for the AT+CCLK= command
 *
 * The String is expected to already have enough reserved space
 * so that an out-of-memory is not likely.
 * The format is "yy/MM/dd,hh:mm:ss±zz"
 * For the time being the timezone is set to 0 (UTC)
 */
void SIMCOMDateTime::addToString(String & str) const
{
  add0Nd(str, _yOff, 2);
  str += '/';
  add0Nd(str, _m + 1, 2);
  str += '/';
  add0Nd(str, _d + 1, 2);
  str += ',';
  add0Nd(str, _hh, 2);
  str += ':';
  add0Nd(str, _mm, 2);
  str += ':';
  add0Nd(str, _ss, 2);
  str += "+00";
}