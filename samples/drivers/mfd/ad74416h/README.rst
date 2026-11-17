.. zephyr:code-sample:: ad74416h
   :name: AD74416H software configurable I/O
   :relevant-api: mfd_interface_ad74416h dac_interface adc_interface

   Drive a voltage output and read a voltage input and the on-chip
   diagnostics of an Analog Devices AD74416H.

Overview
********

The AD74416H is a quad-channel software configurable I/O device. Each channel
can be used as a voltage or current input or output, an RTD input or a
digital I/O.

This sample configures channel A as a voltage input and channel B as a voltage
output in devicetree. It drives 5 V on channel B through the
:ref:`DAC API <dac_api>` using :c:func:`dac_write_value` with
``ad74416h_dac_uv_to_code()``. Then, once a second, it converts channel A and
the four diagnostics assigned in devicetree (die temperature, AVDD_HI, AVCC
and DVCC) with ``ad74416h_adc_convert()`` and prints them in physical units
using the conversion helpers.

Requirements
************

* An :ref:`eval_ad74416h_ardz` on the Arduino header of a board such as the
  :zephyr:board:`stm32h573i_dk`, with the jumpers in their default positions.
* A 24 V supply on the evaluation board power connector P54.

The shield describes the wiring. The channel configuration is in
:zephyr_file:`samples/drivers/mfd/ad74416h/app.overlay`.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/mfd/ad74416h
   :board: stm32h573i_dk
   :shield: eval_ad74416h_ardz
   :goals: build flash
   :compact:

Sample Output
=============

.. code-block:: console

   [00:00:00.010,000] <inf> mfd_ad74416h: AD74416H silicon revision 2
   [00:00:01.020,000] <inf> main: Channel A: 2499876 uV
   [00:00:01.021,000] <inf> main: temp: 31250 mdegC
   [00:00:01.021,000] <inf> main: AVDD_HI: 21001234 uV

Notes
*****

* The driver was written from the AD74416H data sheet Rev. 0 and has not yet
  been validated on hardware.
* SPI mode 2 (CPOL = 1, CPHA = 0) follows ADI's no-OS driver.
* The die temperature formula is from data sheet Table 30.
