# Pico-VDP RC2014 Board

In a nutshell, this project is a 39-pin RC2014 compatible multimedia board and associated software, providing video output, audio output, keyboard input, mouse input and [RomWBW](https://github.com/wwarthen/RomWBW) driver for Z80 retro computers. At the time of writing, it has been tested only with the [SC126 computer](https://smallcomputercentral.wordpress.com/sc126-z180-motherboard-rc2014/) but should work with others.

A goal is to emulate hardware that could, in an alternate history, have been implemented for an 8-bit home micro circa 1985. The design does not represent the state-of-the-art for 1985. For example, the Commodore Amiga 1000 computer, released in 1985, was much more capable. Rather, the design is intended to not only be possible in 1985 but cost-effective enough to put in an 8-bit home computer. For that reason, the VDP9938 graphics chip, used by MSX2 computers, is the main inspiration.

Another goal is for the design to have era-reminiscent "quirks," which might be cleverly exploited to improve performance and/or capabilities. Some of these will intentionally remain undocumented, even if I take time to write documentation.

A Raspberry Pi Pico microcontroller board, not to be confused with the Raspberry Pi computer, provides the "brains." It was chosen because it provides a cheap way to generate a VGA video signal, because it is DIY soldering friendly and because it doesn't require special hardware to program - just a regular USB cable.

The reason for not emulating actual 1985 vintage hardware, such as the VDP9938 chip, is because I'm doing this primarily for my own entertainment.

The schematic and PCB layout files are for DesignSpark PCB.

## Current Status

These are the features supported by the revision 1 hardware and associated software at the time of writing. This is a work in progress. Expect the hardware, both actual and emulated, and the software to break backwards compatibility as work progresses.

- Graphics
  - VGA connector
  - 192KB VRAM
    - 2x 32KB double-buffered Display RAM banks
    - 128KB Blitter RAM
  - Bitmap modes
    - 256x192 pixels @ 2/4/16 colors
    - 512x192 pixels @ 2/4 colors
  - 1 hardware cursor
  - Hardware assisted horizontal & vertical scrolling
  - VDP9938 inspired blitter coprocessor
  - Amstrad PCW inspired "roller RAM"
  - Commodore Amiga inspired "screens"
  - Blitter command FIFO
    - inspired by modern GPUs
    - backed by blitter RAM
    - facilitates parallelism between CPU and VDP
  -  16 cycle accurate emulation
  - RomWBW driver
    - 42x24 text, 16 colors
    - 64x24 text, 4 colors
    - 80x24 text, 4 colors
- Audio
  - PCB mount speaker
  - 2x emulated AY-891x programmable sound generators
    - 6 tone channels
    - 2 noise channels
    - 2 envelopes
  - 16 cycle accurate emulation
- USB
  - Hub supported
  - Keyboard
    - MSX International keyboard matrix
    - Programmable IO controls keyboard LED
  - Mouse
    - 8-bit X, 8-bit Y, button state
- Software
  - RP2040 firmware
  - RomWBW HBIOS drivers
  - RomWBW TUNE.COM plays AY-891x chip tunes
- Microcontroller over-clocking
  - Over-clocking not needed if audio disabled
  - 195MHz over-clock needed for video & audio

## Contemplated Future Projects

- Revision 2 board
  - Reduce board height, increase PCB area
  - Line level mono audio output instead of PCB mounted speaker
  - Mount Raspberry Pi Pico with castellated through holes rather than headers
  - Increase space between USB port and VGA port
  - Change address decoding logic to locate AY-891x IO ports at same addresses as a popular board
  - Optional enhanced RC2014 header pins
    - For mechanical support only 
    - Keep full support for 39/40 pin unenhanced RC2014 connection
  - Remove UART support
  - Remove mute header
- Single board computer utilizing the Pico-VDP
- FUZIX driver
- Video display processor SDK
  - for writing interactive bitmap graphics applications in assembly language and/or C
- BASIC interpreter with bitmap graphics modes & commands
  - CP/M and/or FUZIX
- Documentation
  - Hardware Manuals
  - SDK Reference
- Integrate HBIOS and FUZIX drivers into respective source repositories
- 256 color mode

## AY-891x Accuracy

Hello AY-3-8910 aficionado! Unfortunately the AY emulator only emulates every 16th cycle, i.e. the emulator is not cycle accurate. Another difference is, unlike the original chips, this uses pulse width modulation.
Unlike some AY emulators, this one is quite constrained by the performance of the $1 microcontroller it runs on, which also emulates a video display processor in parallel.
I tried some existing AY emulators and they simply weren't fast enough. That's why I wrote this new one, which makes compromises.
Making the emulation sound more like the original chips is desirable but making it sound exactly like the original chips is not a goal.
