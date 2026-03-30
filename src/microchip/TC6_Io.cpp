/*
 *  This file is part of the Arduino_10BASE_T1S library.
 *
 *  Copyright (c) 2024 Arduino SA
 *
 *  This Source Code Form is subject to the terms of the Mozilla Public
 *  License, v. 2.0. If a copy of the MPL was not distributed with this
 *  file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/**************************************************************************************
 * INCLUDE
 **************************************************************************************/

#include "TC6_Io.h"

/**************************************************************************************
 * NAMESPACE
 **************************************************************************************/

namespace TC6
{

/**************************************************************************************
 * CONSTANTS
 **************************************************************************************/

static SPISettings const LAN865x_SPI_SETTING{16 * 1000 * 1000UL, MSBFIRST, SPI_MODE0};

/**************************************************************************************
 * STATIC MEMBER DEFINITION
 **************************************************************************************/

size_t const TC6_Io::MAC_SIZE;
uint8_t const TC6_Io::FALLBACK_MAC[TC6_Io::MAC_SIZE];

/**************************************************************************************
 * CTOR/DTOR
 **************************************************************************************/

TC6_Io::TC6_Io(
  HardwareSPI &spi,
  int const cs_pin,
  int const reset_pin,
  int const irq_pin)
  : _spi{spi}, _cs_pin{cs_pin}, _reset_pin{reset_pin}, _irq_pin{irq_pin}, _int_in{0}, _int_out{0}, _int_reported{0}
{

}

/**************************************************************************************
 * PUBLIC MEMBER FUNCTIONS
 **************************************************************************************/

bool TC6_Io::begin()
{
  digitalWrite(_cs_pin, HIGH);
  pinMode(_cs_pin, OUTPUT);

  pinMode(_reset_pin, OUTPUT);
  digitalWrite(_reset_pin, LOW);
  delay(100);
  digitalWrite(_reset_pin, HIGH);
  delay(100);

  _spi.begin();

  return true;
}

void TC6_Io::onInterrupt()
{
  _int_in++;
}

bool TC6_Io::isInterruptActive()
{
  _int_reported = _int_in;
  return (_int_reported != _int_out);
}

void TC6_Io::releaseInterrupt()
{
  if (digitalRead(_irq_pin) == HIGH)
    _int_out = _int_reported;
}

bool TC6_Io::spiTransaction(uint8_t const *pTx, uint8_t *pRx, uint16_t const len)
{
  digitalWrite(_cs_pin, LOW);
  _spi.beginTransaction(LAN865x_SPI_SETTING);

  memcpy(pRx, pTx, len);
  _spi.transfer(pRx, len);

  _spi.endTransaction();
  digitalWrite(_cs_pin, HIGH);

  return true;
}

/**************************************************************************************
 * NAMESPACE
 **************************************************************************************/

} /* TC6 */
