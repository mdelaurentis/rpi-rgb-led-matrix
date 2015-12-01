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

// The framebuffer is the workhorse: it represents the frame in some internal
// format that is friendly to be dumped to the matrix quickly. Provides methods
// to manipulate the content.

#include "framebuffer-internal.h"

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "gpio.h"

  uint32_t color_buffer[16][11][3];



namespace rgb_matrix {
namespace internal {
enum {
  kBitPlanes = 11  // maximum usable bitplanes.
};

// Lower values create a higher framerate, but display will be a
// bit dimmer. Good values are between 100 and 200.
static const long kBaseTimeNanos = 130;

// We need one global instance of a timing correct pulser. There are different
// implementations depending on the context.
static PinPulser *sOutputEnablePulser = NULL;

Framebuffer::Framebuffer(int rows, int columns, int parallel)
  : rows_(rows),
    parallel_(parallel),
    height_(rows * parallel),
    columns_(columns),
    pwm_bits_(kBitPlanes), do_luminance_correct_(true), brightness_(100),
    double_rows_(rows / 2), row_mask_(double_rows_ - 1) {
  bitplane_buffer_ = new IoBits [double_rows_ * columns_ * kBitPlanes];
  int i, j, k;
  for (i = 0; i < 16; i++)
    for (j = 0; j < 11; j++)
      for (k = 0; k < 3; k++)
        color_buffer[i][j][k] = 0;

  Clear();
  assert(rows_ <= 32);
  assert(parallel >= 1 && parallel <= 3);
}

Framebuffer::~Framebuffer() {
  delete [] bitplane_buffer_;
}

/* static */ void Framebuffer::InitGPIO(struct gpio_struct *io, int parallel) {
  if (sOutputEnablePulser != NULL)
    return;  // already initialized.

  // Tell GPIO about all bits we intend to use.
  IoBits b;
  b.raw = 0;
  b.bits.output_enable = 1;
  b.bits.clock = 1;
  b.bits.strobe = 1;

  b.bits.p0_r1 = b.bits.p0_g1 = b.bits.p0_b1 = 1;
  b.bits.p0_r2 = b.bits.p0_g2 = b.bits.p0_b2 = 1;

  b.bits.a = b.bits.b = b.bits.c = 1;

  // Initialize outputs, make sure that all of these are supported bits.
  gpio_init_outputs(io, b.raw);

  // Now, set up the PinPulser for output enable.
  IoBits output_enable_bits;
  output_enable_bits.bits.output_enable = 1;

  sOutputEnablePulser = new PinPulser(kBaseTimeNanos);
}

bool Framebuffer::SetPWMBits(uint8_t value) {
  if (value < 1 || value > kBitPlanes)
    return false;
  pwm_bits_ = value;
  return true;
}

inline Framebuffer::IoBits *Framebuffer::ValueAt(int double_row,
                                                 int column, int bit) {
  return &bitplane_buffer_[ double_row * (columns_ * kBitPlanes)
                            + bit * columns_
                            + column ];
}

// Do CIE1931 luminance correction and scale to output bitplanes
static uint16_t luminance_cie1931(uint8_t c, uint8_t brightness) {
  float out_factor = ((1 << kBitPlanes) - 1);
  float v = (float) c * brightness / 255.0;
  return out_factor * ((v <= 8) ? v / 902.3 : pow((v + 16) / 116.0, 3));
}

static uint16_t *CreateLuminanceCIE1931LookupTable() {
  uint16_t *result = new uint16_t[256 * 100];
  for (int i = 0; i < 256; ++i)
    for (int j = 0; j < 100; ++j)
      result[i * 100 + j] = luminance_cie1931(i, j + 1);

  return result;
}

inline uint16_t Framebuffer::MapColor(uint8_t c) {

  if (do_luminance_correct_) {
    static uint16_t *luminance_lookup = CreateLuminanceCIE1931LookupTable();
    return luminance_lookup[c * 100 + (brightness_ - 1)];
  } else {
    // simple scale down the color value
    c = c * brightness_ / 100;

    enum {shift = kBitPlanes - 8};  //constexpr; shift to be left aligned.
    return (shift > 0) ? (c << shift) : (c >> -shift);
  }

}

void Framebuffer::Clear() {
  memset(bitplane_buffer_, 0,
         sizeof(*bitplane_buffer_) * double_rows_ * columns_ * kBitPlanes);
}

void Framebuffer::Fill(uint8_t r, uint8_t g, uint8_t b) {

  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 32; x++)
      SetPixel(x, y, r, g, b);

}

void Framebuffer::SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || x >= columns_ || y < 0 || y >= height_) return;

  uint16_t red   = MapColor(r);
  uint16_t green = MapColor(g);
  uint16_t blue  = MapColor(b);

  const int min_bit_plane = kBitPlanes - pwm_bits_;

  if (y >= rows_) {
    return;
  }

  uint32_t col_mask = 1 << x;
  for (int b = min_bit_plane; b < kBitPlanes; b++) {
    color_buffer[y][b][0] &= ~col_mask;
    color_buffer[y][b][1] &= ~col_mask;
    color_buffer[y][b][2] &= ~col_mask;
    color_buffer[y][b][0] |= (red & 1)   << x;
    color_buffer[y][b][1] |= (green & 1) << x;
    color_buffer[y][b][2] |= (blue & 1)  << x;

    red   >>= 1;
    green >>= 1;
    blue  >>= 1;
  }
}


  int debug_counter = 0;



void Framebuffer::DumpToMatrix(struct gpio_struct *io) {
  //  printf("In dump to matrix\n");

  uint32_t color_mask = 1 << 7 | 1 << 8 | 1 << 9 | 1 << 10 | 1 << 11 | 1 << 27;

  debug_counter = (debug_counter + 1) % 1000;
  int debug = debug_counter == 0;

  const int pwm_to_show = pwm_bits_;  // Local copy, might change in process.
  for (uint8_t d_row = 0; d_row < double_rows_; ++d_row) {
    uint32_t row_addr = d_row << 22;
    
    // Set row address (A, B, C). ABC are bits 22-24.
    *(io->clear_bits) =  ~row_addr & (7 << 22);
    *(io->set_bits)   =   row_addr;
    
    // Rows can't be switched very quickly without ghosting, so we do the
    // full PWM of one row before switching rows.
    for (int b = kBitPlanes - pwm_to_show; b < kBitPlanes; ++b) {
      
      const int y = d_row;
      uint32_t r_bits1 = color_buffer[y][b][0];
      uint32_t g_bits1 = color_buffer[y][b][1];
      uint32_t b_bits1 = color_buffer[y][b][2];
      uint32_t r_bits2 = color_buffer[y+8][b][0];
      uint32_t g_bits2 = color_buffer[y+8][b][1];
      uint32_t b_bits2 = color_buffer[y+8][b][2];
      
      // While the output enable is still on, we can already clock in the next
      // data.
      for (int col = 0; col < columns_; ++col) {

        uint32_t out_bits = (((r_bits1 & 1) << 11) |
                             ((g_bits1 & 1) << 27) |
                             ((b_bits1 & 1) <<  7) |
                             ((r_bits2 & 1) <<  8) |
                             ((g_bits2 & 1) <<  9) |
                             ((b_bits2 & 1) << 10));

        r_bits1 >>= 1;
        g_bits1 >>= 1;
        b_bits1 >>= 1;
        r_bits2 >>= 1;
        g_bits2 >>= 1;
        b_bits2 >>= 1;

        // Clear the clock and color, then set color and clock. Clock
        // is bit 17.
        *(io->clear_bits) = 1 << 17;
        *(io->clear_bits) =  ~out_bits & color_mask;
        *(io->set_bits)   =   out_bits & color_mask;
        *(io->set_bits)   = 1 << 17;
      }
      // Clear the clock and color
      *(io->clear_bits) = (1 << 17) | color_mask;
      

      // OE of the previous row-data must be finished before strobe.
      sOutputEnablePulser->WaitPulseFinished();

      // Set and clear the strobe (bit 4)
      *(io->set_bits) = 1 << 4;
      *(io->clear_bits) = 1 << 4;

      // Now switch on for the sleep time necessary for that bit-plane.
      sOutputEnablePulser->SendPulse(b);
    }
    sOutputEnablePulser->WaitPulseFinished();
  }
}
}  // namespace internal
}  // namespace rgb_matrix
