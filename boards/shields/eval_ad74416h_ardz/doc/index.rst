.. _eval_ad74416h_ardz:

EVAL-AD74416H-ARDZ
##################

Overview
********

The EVAL-AD74416H-ARDZ is an evaluation board for the Analog Devices AD74416H,
a quad-channel, software configurable input and output with HART modem. Each
channel can be a voltage or current input or output, an RTD or thermocouple
input, or a digital input or output.

The board has three isolated zones. The host side (MGND) connects to the
Arduino R3 headers and a PMOD connector. The field side (GND) carries the
AD74416H and its front end. The power side (PGND) takes a 24 V supply
(19.2 V to 28.8 V) on connector P54. Besides the AD74416H the board carries:

- a MAX17691B isolated flyback that generates AVDD_HI (21 V), AVDD_LO (13 V),
  AVCC and DVCC (5 V) and AVSS (-16 V). Each rail can instead be supplied from
  its own connector.
- ADuM341E and ADuM342E digital isolators for SPI, RESET, ALERT, ADC_RDY and
  GPIO_F.
- an ADR4525 2.5 V reference connected to REFIO through header P44.
- a 33 kOhm NTC thermistor on the LVIN pin for thermocouple cold junction
  compensation, selected by header P12.
- per-channel external PMOS FETs for the digital output function and smart
  blocking diodes controlled by GPIO_A to GPIO_D.
- green LEDs DS1 to DS6 on GPIO_A to GPIO_F, and red (ALERT) and orange
  (ADC_RDY) LEDs on the host side.
- a 24AA32A I2C EEPROM holding the board identification.

Programming
***********

Set ``--shield eval_ad74416h_ardz`` when you invoke ``west build``. For
example:

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/mfd/ad74416h
   :board: stm32h573i_dk
   :shield: eval_ad74416h_ardz
   :goals: build

The shield describes the AD74416H with its ADC, DAC and GPIO child nodes,
labelled ``ad74416h``, ``ad74416h_adc``, ``ad74416h_dac`` and
``ad74416h_gpio``. It does not configure any channel. Add ``channel@N`` nodes
and diagnostics in the application overlay, for example:

.. code-block:: devicetree

   &ad74416h {
           adi,diag-assign = <AD74416H_DIAG_TEMP AD74416H_DIAG_AVDD_HI
                              AD74416H_DIAG_AVCC AD74416H_DIAG_LVIN>;

           channel@0 {
                   reg = <0>;
                   adi,ch-func = <AD74416H_CH_FUNC_VIN>;
           };
   };

Requirements
************

This shield can only be used with a board which provides a configuration for
Arduino connectors and defines node aliases for SPI, I2C and GPIO interfaces
(see :ref:`shields` for more details). The Arduino side isolators are powered
from IOREF through header P7, so the host must provide IOREF between 2.25 V
and 5.5 V.

SYNC must rise after every 40-bit frame, so the shield drives D10 as a GPIO
chip select. On boards where D10 is also the hardware NSS pin, the shield
provides a board overlay that removes NSS from the SPI pin control. One is
provided for :zephyr:board:`stm32h573i_dk`.

Jumper Settings
===============

The shield assumes the default jumper positions of the user guide (Table 1).
The settings that change what the host sees are:

+---------+-------------------------------+-----------------------------------+
| Header  | Default                       | Effect on the devicetree          |
+=========+===============================+===================================+
| P8      | Pin 1 to Pin 3, SYNC on D10   | Other positions move SYNC to A0,  |
|         |                               | A1 or A2: change ``cs-gpios``.    |
+---------+-------------------------------+-----------------------------------+
| P11     | Both jumpers on, address 0    | Change ``adi,dev-addr``.          |
+---------+-------------------------------+-----------------------------------+
| P40     | Open, RESET not connected     | Close it and add ``reset-gpios``  |
|         |                               | on D4 to use the hardware reset.  |
|         |                               | Without it the driver uses the    |
|         |                               | software reset.                   |
+---------+-------------------------------+-----------------------------------+
| P41     | Open, GPIO_F not driven       | When closed, A5 drives GPIO_F.    |
|         |                               | Configure GPIO_F as an input.     |
+---------+-------------------------------+-----------------------------------+
| P42,    | Closed, ALERT and ADC_RDY     | Remove ``alert-gpios`` or         |
| P43     | connected                     | ``adc-rdy-gpios`` when opened.    |
+---------+-------------------------------+-----------------------------------+
| P44     | Closed, ADR4525 on REFIO      | When opened, add                  |
|         |                               | ``adi,internal-ref``, see         |
|         |                               | `Voltage Reference`_.             |
+---------+-------------------------------+-----------------------------------+

Pin Assignments
===============

+--------------+----------------------------------------------------------+
| Arduino Pin  | Function                                                 |
+==============+==========================================================+
| A0, A1, A2   | Alternative SYNC lines, selected by header P8            |
+--------------+----------------------------------------------------------+
| A5           | GPIO_F input of the AD74416H, through header P41 (open)  |
+--------------+----------------------------------------------------------+
| D2           | ADC_RDY (active low, open drain, pulled up on the board) |
+--------------+----------------------------------------------------------+
| D4           | RESET (active low), through header P40 (open)            |
+--------------+----------------------------------------------------------+
| D7           | ALERT (active low, open drain, pulled up on the board)   |
+--------------+----------------------------------------------------------+
| D10          | SYNC, SPI chip select (active low)                       |
+--------------+----------------------------------------------------------+
| D11          | SPI MOSI (SDI)                                           |
+--------------+----------------------------------------------------------+
| D12          | SPI MISO (SDO), high impedance while SYNC is high        |
+--------------+----------------------------------------------------------+
| D13          | SPI SCLK                                                 |
+--------------+----------------------------------------------------------+
| SDA, SCL     | 24AA32A EEPROM at address 0x52, 10 kOhm pull-ups         |
+--------------+----------------------------------------------------------+

Voltage Reference
=================

By default header P44 connects the ADR4525 to REFIO and the AD74416H runs from this external
2.5 V reference. To use the AD74416H internal reference instead, remove the jumper from P44 and
enable it in the application overlay:

.. code-block:: devicetree

   &ad74416h {
           adi,internal-ref;
   };

The internal reference drives REFIO, so never set ``adi,internal-ref`` with P44 closed: it
would drive against the ADR4525 output. The LVIN thermistor divider is supplied from the
ADR4525 output and is not affected by P44.

LEDs
====

The ``ad74416h_leds`` node describes DS1 to DS6 on GPIO_A to GPIO_F. It is
disabled because the LED driver configures every pin as an output at boot.
With the default jumpers the GPIOs only drive the LEDs: the smart blocking
diode headers P45, P47, P49 and P51 and the GPIO_F header P41 are open. An
output would conflict with a fitted smart diode header, with GPIO controlled
digital outputs and with GPIO_F driven from A5 through a fitted P41. Enable
the node in the application overlay when the GPIOs are used only for the
LEDs:

.. code-block:: devicetree

   &ad74416h_leds {
           status = "okay";
   };

EEPROM
======

The 24AA32A is described as ``eeprom_eval_ad74416h_ardz``. It is marked
``read-only`` because it holds the board identification that the ADI ACE
software reads.

References
**********

- `AD74416H product page`_
- `EVAL-AD74416H-ARDZ user guide UG-2153`_
- `EVAL-AD74416H-ARDZ schematic`_

.. _AD74416H product page:
   https://www.analog.com/en/products/ad74416h.html

.. _EVAL-AD74416H-ARDZ user guide UG-2153:
   https://www.analog.com/media/en/technical-documentation/user-guides/eval-ad74416h-ug-2153.pdf

.. _EVAL-AD74416H-ARDZ schematic:
   https://www.analog.com/media/en/technical-documentation/eval-board-schematic/eval-ad74416h-ardz-schematic.pdf
