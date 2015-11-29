// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// Copyright (C) 2013 Henner Zeller <h.zeller@acm.org>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>
#ifndef RPI_RGBMATRIX_FRAMEBUFFER_INTERNAL_H
#define RPI_RGBMATRIX_FRAMEBUFFER_INTERNAL_H

#include <stdint.h>
#include "gpio.h"

namespace rgb_matrix {
class GPIO;
class PinPulser;
namespace internal {
// Internal representation of the frame-buffer that as well can
// write itself to GPIO.
// Our internal memory layout mimicks as much as possible what needs to be
// written out.
class Framebuffer {
public:
  Framebuffer(int rows, int columns, int parallel);
  ~Framebuffer();

  // Initialize GPIO bits for output. Only call once.
  static void InitGPIO(struct gpio_struct *io, int parallel);

  // Set PWM bits used for output. Default is 11, but if you only deal with
  // simple comic-colors, 1 might be sufficient. Lower require less CPU.
  // Returns boolean to signify if value was within range.
  bool SetPWMBits(uint8_t value);
  uint8_t pwmbits() { return pwm_bits_; }

  // Map brightness of output linearly to input with CIE1931 profile.
  void set_luminance_correct(bool on) { do_luminance_correct_ = on; }
  bool luminance_correct() const { return do_luminance_correct_; }

  // Set brightness in percent; range=1..100
  // This will only affect newly set pixels.
  void SetBrightness(uint8_t b) {
    brightness_ = (b <= 100 ? (b != 0 ? b : 1) : 100);
  }
  uint8_t brightness() { return brightness_; }

  void DumpToMatrix(struct gpio_struct *io);

  // Canvas-inspired methods, but we're not implementing this interface to not
  // have an unnecessary vtable.
  inline int width() const { return columns_; }
  inline int height() const { return height_; }
  void SetPixel(int x, int y, uint8_t red, uint8_t green, uint8_t blue);
  void Clear();
  void Fill(uint8_t red, uint8_t green, uint8_t blue);

private:
  // Map color
  inline uint16_t MapColor(uint8_t c);

  const int rows_;     // Number of rows. 16 or 32.
  const int parallel_; // Parallel rows of chains. 1 or 2.
  const int height_;   // rows * parallel
  const int columns_;  // Number of columns. Number of chained boards * 32.

  uint8_t pwm_bits_;   // PWM bits to display.
  bool do_luminance_correct_;
  uint8_t brightness_;

  const int double_rows_;
  const uint8_t row_mask_;

  // Standard pinout since July 2015
  // This uses the PWM pin to create the timing.
  union IoBits {
    struct {
      // This bitset reflects the GPIO mapping. The naming of the
      // pins of type 'p0_r1' means 'first parallel chain, red-bit one'
      //                                 GPIO Header-pos
      unsigned int unused_0_1_2_3 : 4;  //  0..1  (only on RPi 1, Revision 1)
      unsigned int strobe         : 1;  //  4 P1-07
      unsigned int unused_5_6     : 2;
      // TODO: be able to disable chain 0 for higher-pin RPis to gain SPI back.
      unsigned int p0_b1          : 1;  //  7 P1-26 (masks: SPI0_CE1)
      unsigned int p0_r2          : 1;  //  8 P1-24 (masks: SPI0_CE0)
      unsigned int p0_g2          : 1;  //  9 P1-21 (masks: SPI0_MISO
      unsigned int p0_b2          : 1;  // 10 P1-19 (masks: SPI0_MOSI)
      unsigned int p0_r1          : 1;  // 11 P1-23 (masks: SPI0_SCKL)

      unsigned int unused_12_13_14_15_16 : 5;

      unsigned int clock          : 1;  // 17 P1-11

      unsigned int output_enable  : 1;  // 18 P1-12 (PWM pin: our timing)

      unsigned int unused_19_20_21 : 3;
      
      unsigned int a              : 1;  // 22 P1-15  // row bits.
      unsigned int b              : 1;  // 23 P1-16
      unsigned int c              : 1;  // 24 P1-18

      unsigned int unused_25_26   : 2;
      
      unsigned int p0_g1          : 1;  // 27 P1-13 (Not on RPi1, Rev1)
    } bits;
    uint32_t raw;
    IoBits() : raw(0) {}
  };

  // The frame-buffer is organized in bitplanes.
  // Highest level (slowest to cycle through) are double rows.
  // For each double-row, we store pwm-bits columns of a bitplane.
  // Each bitplane-column is pre-filled IoBits, of which the colors are set.
  // Of course, that means that we store unrelated bits in the frame-buffer,
  // but it allows easy access in the critical section.
  IoBits *bitplane_buffer_;
  inline IoBits *ValueAt(int double_row, int column, int bit);
};
}  // namespace internal
}  // namespace rgb_matrix
#endif // RPI_RGBMATRIX_FRAMEBUFFER_INTERNAL_H
