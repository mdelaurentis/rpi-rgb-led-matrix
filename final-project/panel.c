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

#include <stdint.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#define BRIGHTNESS 100
#define COLUMNS 32
#define ROWS 16
#define BCM2709_PERI_BASE        0x3F000000
#define GPIO_REGISTER_OFFSET         0x200000
#define GPIO_PWM_BASE_OFFSET	(GPIO_REGISTER_OFFSET + 0xC000)
#define GPIO_CLK_BASE_OFFSET	0x101000
#define REGISTER_BLOCK_SIZE (4*1024)
#define CLK_PWMCTL 40
#define CLK_PWMDIV 41
#define BIT_PLANES 8
#define BASE_TIME_NANOS 130

// Clock register values
enum {
  CLK_PASSWD = 0x5a << 24,
  CLK_KILL = 1 << 5,
  CLK_ENAB = 1 << 4,
};

// PWM CTL bits
enum {
  PWEN1 = 1 << 0,
  POLA1 = 1 << 4,
  USEF1 = 1 << 5,
  CLRF1 = 1 << 6
};

struct gpio_struct {
  volatile uint32_t *set_bits;
  volatile uint32_t *clear_bits;
  volatile uint32_t *pwm_reg;
} mapped;

uint32_t color_buffer[16][11][3];
static uint16_t cie1931_lookup[256 * 100];

static uint32_t *mmap_bcm_register(off_t register_offset) {
  const off_t base = BCM2709_PERI_BASE;

  int mem_fd;
  if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
    perror("can't open /dev/mem: ");
    return NULL;
  }

  uint32_t *result =
    (uint32_t*) mmap(NULL,                  // Any adddress in our space will do
                     REGISTER_BLOCK_SIZE,   // Map length
                     PROT_READ|PROT_WRITE,  // Enable r/w on GPIO registers.
                     MAP_SHARED,
                     mem_fd,                // File to map
                     base + register_offset // Offset to bcm register
                     );
  close(mem_fd);

  if (result == MAP_FAILED) {
    fprintf(stderr, "mmap error %p\n", result);
    exit(1);
  }
  return result;
}

void gpio_init() {
  volatile uint32_t *port = mmap_bcm_register(GPIO_REGISTER_OFFSET);
  mapped.set_bits = port + (0x1C / sizeof(uint32_t));
  mapped.clear_bits = port + (0x28 / sizeof(uint32_t));
  volatile uint32_t *gpfsel1 = port + 1;
  int i = 0;
  uint32_t divider = BASE_TIME_NANOS / 4;
  int b;
  uint32_t output_bits[] = {
    4,  // strobe
    17, // clock
    18, // output enable

    22, 23, 24, // a, b, c

    7,  // b1
    8,  // r2
    9,  // g2
    10, // b2
    11, // r1
    27 // g1
  };

  for (i = 0; i < 12; i++) {
    b = output_bits[i];
    *(port+((b)/10)) &= ~(7<<(((b)%10)*3));
    *(port+((b)/10)) |=  (1<<(((b)%10)*3));       
  }

  mapped.pwm_reg  = mmap_bcm_register(GPIO_PWM_BASE_OFFSET);
  volatile uint32_t *ctl = mapped.pwm_reg;
  volatile uint32_t *clk_reg = mmap_bcm_register(GPIO_CLK_BASE_OFFSET);
  

  const int reg = 1;  
  const int mode_pos = 24;
  volatile uint32_t *clk_pwm_ctl = clk_reg + 40;
  volatile uint32_t *clk_pwm_div = clk_reg + 41;

  // Set the FSEL18 field of register GPFSEL1 to 010 (GPIO Pin 18
  // takes alternate function 5).
  *gpfsel1 = (*gpfsel1 & ~(7 << 24)) | (2 << 24);

  *ctl = USEF1 | POLA1 | CLRF1;

  // Kill the PWM clock, then set the source as 500 MHz PLLD, then set
  // the divider, then enable it again.
  *clk_pwm_ctl = CLK_PASSWD | CLK_KILL;
  *clk_pwm_ctl = CLK_PASSWD | 6;
  *clk_pwm_div = CLK_PASSWD | divider << 12;
  *clk_pwm_ctl = CLK_PASSWD | CLK_ENAB | 6;
}

// Do CIE1931 luminance correction and scale to output bitplanes
static uint16_t luminance_cie1931(uint8_t c, uint8_t brightness) {
  float out_factor = ((1 << BIT_PLANES) - 1);
  float v = (float) c * brightness / 255.0;
  return out_factor * ((v <= 8) ? v / 902.3 : pow((v + 16) / 116.0, 3));
}

static void cie1931_lookup_init() {
  int i, j;
  for (i = 0; i < 256; ++i) {
    for (j = 0; j < 100; ++j) {
      cie1931_lookup[i * 100 + j] = luminance_cie1931(i, j + 1);
    }
  }
}

void init_color_buffer() {
  int i, j, k;
    
  for (i = 0; i < 16; i++)
    for (j = 0; j < 11; j++)
      for (k = 0; k < 3; k++)
        color_buffer[i][j][k] = 0;
}

void init_buffer() {
  cie1931_lookup_init();
  init_color_buffer();
}

inline uint16_t map_color(uint8_t c) {
  return cie1931_lookup[c * 100 + (BRIGHTNESS - 1)];
}

void buf_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {

  int p;
  if (x < 0 || x >= COLUMNS || y < 0 || y >= ROWS) return;
  
  uint16_t red   = map_color(r);
  uint16_t green = map_color(g);
  uint16_t blue  = map_color(b);
  
  uint32_t col_mask = 1 << x;
  for (p = 0; p < BIT_PLANES; p++) {
    color_buffer[y][p][0] &= ~col_mask;
    color_buffer[y][p][1] &= ~col_mask;
    color_buffer[y][p][2] &= ~col_mask;
    color_buffer[y][p][0] |= (red & 1)   << x;
    color_buffer[y][p][1] |= (green & 1) << x;
    color_buffer[y][p][2] |= (blue & 1)  << x;

    red   >>= 1;
    green >>= 1;
    blue  >>= 1;
  }
}

void buf_flush() {
  int b, col;
  uint8_t d_row;

  uint32_t color_mask = 1 << 7 | 1 << 8 | 1 << 9 | 1 << 10 | 1 << 11 | 1 << 27;

  volatile uint32_t *ctl  = mapped.pwm_reg;
  volatile uint32_t *sta  = mapped.pwm_reg + 0x4  / 4;
  volatile uint32_t *rng1 = mapped.pwm_reg + 0x10 / 4;    
  volatile uint32_t *fifo = mapped.pwm_reg + 0x18 / 4;
  
  for (d_row = 0; d_row < ROWS / 2; ++d_row) {
    uint32_t row_addr = d_row << 22;
    
    // Set row address (A, B, C). ABC are bits 22-24.
    *mapped.clear_bits =  ~row_addr & (7 << 22);
    *mapped.set_bits   =   row_addr;
    
    // Rows can't be switched very quickly without ghosting, so we do the
    // full PWM of one row before switching rows.
    for (b = 0; b < BIT_PLANES; ++b) {
      
      const int y = d_row;
      uint32_t r_bits1 = color_buffer[y][b][0];
      uint32_t g_bits1 = color_buffer[y][b][1];
      uint32_t b_bits1 = color_buffer[y][b][2];
      uint32_t r_bits2 = color_buffer[y+8][b][0];
      uint32_t g_bits2 = color_buffer[y+8][b][1];
      uint32_t b_bits2 = color_buffer[y+8][b][2];
      
      // While the output enable is still on, we can already clock in the next
      // data.
      for (col = 0; col < COLUMNS; ++col) {

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
        *mapped.clear_bits = 1 << 17;
        *mapped.clear_bits =  ~out_bits & color_mask;
        *mapped.set_bits   =   out_bits & color_mask;
        *mapped.set_bits   = 1 << 17;
      }
      // Clear the clock and color
      *mapped.clear_bits = (1 << 17) | color_mask;
      
      // OE of the previous row-data must be finished before strobe.
      while (!(*sta & 0x2));
      *ctl = USEF1 | POLA1 | CLRF1;      

      // Set and clear the strobe (bit 4)
      *mapped.set_bits = 1 << 4;
      *mapped.clear_bits = 1 << 4;

      // Now switch on for the sleep time necessary for that bit-plane.
      uint32_t pwm_range = 1 << (b + 1);
  
      *rng1 = pwm_range;
      *fifo = pwm_range;
      *fifo = 0;
      *fifo = 0;
      *ctl =  PWEN1 | POLA1 | USEF1;

    }
    while (!(*sta & 0x2));
    *ctl = USEF1 | POLA1 | CLRF1;      
  }
}

int main(int argc, char **argv) {
  int t, x, y, i;
  
  gpio_init();
  init_buffer();

  for (t = 0; t < 255; t += 8) {
    int b = t;
    for (x = 0; x < 32; x++) {
      for (y = 0; y < 16; y++) {
        int r = 255 * (x / 32.0);
        int g = 255 * (y / 16.0);
        buf_set_pixel(x, y, r, g, b);
        buf_flush();
      }
    }
  }
}
