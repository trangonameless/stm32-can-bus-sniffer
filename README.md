STM32 CAN Bus Sniffer – USB SLCAN Adapter

An STM32-based CAN bus sniffer implementing the LAWICEL/SLCAN protocol over UART, allowing Linux to recognize the device as a SocketCAN network interface.

The project demonstrates CAN communication between STM32 microcontrollers, CAN frame monitoring, and integration with Linux CAN tools.

Features

CAN 2.0 communication using STM32 bxCAN peripherals.

CAN frame reception and processing.

LAWICEL/SLCAN protocol implementation over USART2.

USB-to-UART communication with a Linux host.

Linux SocketCAN integration through slcand.

CAN frame monitoring using standard Linux CAN utilities.

Support for standard and extended CAN frames.

Support for SLCAN commands:

S – Set CAN bitrate.

O – Open CAN channel.

L – Open CAN channel in listen-only mode.

C – Close CAN channel.

t, T – Transmit standard/extended CAN frames.

r, R – Transmit standard/extended RTR frames.

F – Read status flags.

V – Read firmware version.

N – Read serial number.

Z0 – Disable timestamp output.

Hardware

STM32L476RG – CAN receiver / sniffer.

STM32F103C8T6 – CAN transmitter node.

SN65HVD230 CAN transceivers.

USB-to-UART interface connected to USART2.

CAN bus with 120 Ω termination resistors.

System architecture

       STM32F103C8T6
       CAN transmitter
              |
              |
       SN65HVD230
              |
              |
      CAN BUS (500 kbit/s)
              |
              |
       SN65HVD230
              |
              |
       STM32L476RG
       CAN sniffer
              |
           USART2
              |
       USB-to-UART
              |
              |
        Ubuntu Linux
              |
           slcand
              |
           slcan0
              |
        SocketCAN
              |
      +-------+-------+
      |       |       |
  candump  cansend  canplayer

CAN Frame Format

The current test application uses CAN identifier 0x100 to transmit temperature measurements.

CAN ID

DLC

Data bytes

Description

0x100

3

Bytes 0–1

Temperature in 0.1 °C, signed int16, little-endian

0x100

3

Byte 2

Frame counter

Linux Integration

The STM32 sniffer implements the LAWICEL/SLCAN serial protocol, allowing Linux to expose it as a SocketCAN interface.

1. Install required tools

sudo apt update
sudo apt install can-utils

2. Connect the device

Connect the STM32 sniffer to the Linux host through the USB-to-UART interface.

Check the serial device:

ls -l /dev/ttyUSB*

3. Create the SocketCAN interface

Start slcand:

sudo slcand -o -s6 -t hw -S 3000000 /dev/ttyUSB0 slcan0

Set the interface up:

sudo ip link set slcan0 up

Configuration:

Option

Description

-o

Open CAN channel

-s6

Set CAN bitrate to 500 kbit/s

-t hw

Use hardware timestamps

-S 3000000

Set serial communication speed to 3 Mbaud

/dev/ttyUSB0

Serial device

slcan0

SocketCAN interface name

4. Monitor CAN traffic

Display received frames:

candump slcan0

Display frames with absolute timestamps:

candump -ta slcan0

The interface can also be inspected using:

ip -details link show slcan0

5. Additional SocketCAN tools

Once slcan0 is active, standard Linux CAN utilities can interact with the device.

Examples:

candump slcan0

cansend slcan0 100#2D0114

cangen slcan0

Note: Transmitting CAN frames requires the STM32 firmware to support the corresponding SLCAN transmit commands.

How to Run – Bench Test

Connect both STM32 boards through SN65HVD230 CAN transceivers.

Connect CAN-H to CAN-H and CAN-L to CAN-L.

Install 120 Ω termination resistors at both ends of the CAN bus.

Connect the sniffer's USB-to-UART interface to the Linux host.

Flash the STM32L476RG sniffer firmware.

Flash the STM32F103C8T6 transmitter firmware.

Start slcand and bring slcan0 up.

Run:

candump -ta slcan0

The F103 transmitter periodically sends temperature frames with CAN ID 0x100. The L476 sniffer receives these frames and forwards them to Linux using the LAWICEL/SLCAN protocol.

Results

CAN Frame Reception

Frame reception via CAN protocol

Temperature frames transmitted by the STM32F103C8T6 and received by the STM32L476RG.

CAN Bus Capture – Logic 2

CAN normal mode: internal temperature transmission

Logic 2 capture of CAN traffic in normal mode at 500 kbit/s.

Linux SocketCAN Integration

CAN frame reception in Ubuntu using candump

Successful reception of CAN frames in Ubuntu through the slcan0 SocketCAN interface using candump.

Project Status

STM32 bxCAN initialization and loopback testing.

CAN communication between STM32F103C8T6 and STM32L476RG.

Temperature frame transmission and reception.

CAN bus monitoring using a logic analyzer.

CAN sniffer with candump-compatible output.

LAWICEL/SLCAN protocol implementation over USART2.

Linux integration using slcand.

SocketCAN interface creation (slcan0).

CAN frame reception in Ubuntu using candump.

Extended testing with different CAN devices and traffic patterns.

Vehicle CAN bus testing.

CAN logging and frame replay workflow.

Technologies

STM32 HAL

bxCAN

C

UART / USART

USB-to-UART

LAWICEL / SLCAN

Linux SocketCAN

can-utils

Ubuntu Linux

Logic 2 (Saleae)
