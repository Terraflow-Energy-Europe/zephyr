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

* An EV-AD74416H-ARDZ evaluation board on the Arduino header of the
  :zephyr:board:`stm32h573i_dk`.
* SYNC on D10, driven as a GPIO chip select so that it rises after every
  40-bit frame.
* The RESET, ALERT and ADC_RDY pins are not wired in the provided overlay. Add
  ``reset-gpios``, ``alert-gpios`` and ``adc-rdy-gpios`` as described in the
  evaluation board user guide.
* The analog supplies of the evaluation board must be powered as described in
  the evaluation board documentation.

The overlay is in :zephyr_file:`samples/drivers/mfd/ad74416h/boards`.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/mfd/ad74416h
   :board: stm32h573i_dk
   :goals: build flash
   :compact:

Sample Output
=============

.. code-block:: console

   [00:00:00.010,000] <inf> mfd_ad74416h: AD74416H silicon revision 2
   [00:00:01.020,000] <inf> main: Channel A: 2499876 uV
   [00:00:01.021,000] <inf> main: temp: 31250 mdegC
   [00:00:01.021,000] <inf> main: AVDD_HI: 24001234 uV

Notes
*****

* The driver was written from the AD74416H data sheet Rev. 0 and has not yet
  been validated on hardware.
* SPI mode 2 (CPOL = 1, CPHA = 0) follows ADI's no-OS driver.
* The die temperature formula is from data sheet Table 30.
