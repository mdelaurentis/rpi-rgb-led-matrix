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

#define GPIO_REGISTER_OFFSET     0x3F200000
#define GPIO_PWM_BASE_OFFSET	 0x3F20c000
#define GPIO_CLK_BASE_OFFSET	 0x3F101000
#define REGISTER_BLOCK_SIZE (4*1024)

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

uint32_t color_buffer[16][8][3];

static uint32_t *mmap_bcm_register(uint32_t *addr, off_t register_offset) {

  int mem_fd;
  if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
    perror("can't open /dev/mem: ");
    return NULL;
  }

  uint32_t *result =
    (uint32_t*) mmap(addr,                  // Any adddress in our space will do
                     REGISTER_BLOCK_SIZE,   // Map length
                     PROT_READ|PROT_WRITE,  // Enable r/w on GPIO registers.
                     MAP_SHARED,
                     mem_fd,                // File to map
                     register_offset // Offset to bcm register
                     );
  close(mem_fd);

  if (result == MAP_FAILED) {
    fprintf(stderr, "mmap error %p\n", result);
    exit(1);
  }
  return result;
}

enum {
  GPFSEL0 = 0x7e200000,
  GPFSEL1 = 0x7e200004,
  GPSET0  = 0x7e20001c,
  GPCLR0  = 0x7e200028,

  // PWM registers. The actual addresses aren't listed in the
  // datasheet, but the offsets are.
  PWMCTL    = 0x7e20c000,
  PWMSTA    = 0x7e20c004,
  PWMRNG1   = 0x7e20c010,
  PWMFIF1   = 0x7e20c018,

  // PWM registers. The actual addresses aren't listed in the
  // datasheet, but the offsets are.  
  CM_PWMCTL = 0x7e1010a0,
  CM_PWMDIV = 0x7e1010a4
};

char cie1931_lookup[] =
  {
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 15, 16, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 28, 28, 29, 29, 30, 31, 31, 32, 33, 33, 34, 35, 35, 36, 37, 37, 38, 39, 40, 40, 41, 42, 43, 44, 44, 45, 46, 47, 48, 49, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 75, 76, 77, 78, 79, 80, 82, 83, 84, 85, 87, 88, 89, 90, 92, 93, 94, 96, 97, 99, 100, 101, 103, 104, 106, 107, 108, 110, 111, 113, 114, 116, 118, 119, 121, 122, 124, 125, 127, 129, 130, 132, 134, 135, 137, 139, 141, 142, 144, 146, 148, 149, 151, 153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 174, 176, 178, 180, 182, 185, 187, 189, 191, 193, 195, 197, 200, 202, 204, 206, 208, 211, 213, 215, 218, 220, 222, 225, 227, 230, 232, 234, 237, 239, 242, 244, 247, 249, 252, 255};


void ledpanel_clear() {
  int row, bp, color;
  for (row = 0; row < 16; row++)
    for (bp = 0; bp < 8; bp++)
      for (color = 0; color < 3; color++)
        color_buffer[row][bp][color] = 0;
}


void ledpanel_init() {

  mmap_bcm_register((uint32_t*)0x7e200000, GPIO_REGISTER_OFFSET);
  mmap_bcm_register((uint32_t*)0x7e20c000, GPIO_PWM_BASE_OFFSET);
  mmap_bcm_register((uint32_t*)0x7e1010a0, GPIO_CLK_BASE_OFFSET);
  
  volatile uint32_t* gpfsel1 = (uint32_t*) GPFSEL1;
  volatile uint32_t* gpfsel = (uint32_t*) GPFSEL0;
  volatile uint32_t* gpset0 = (uint32_t*) GPSET0;
  volatile uint32_t* gpclr0 = (uint32_t*) GPCLR0;
  volatile uint32_t *pwmctl = (uint32_t*) PWMCTL;
  volatile uint32_t *cm_pwmctl = (uint32_t*) CM_PWMCTL;
  volatile uint32_t *cm_pwmdiv = (uint32_t*) CM_PWMDIV;
  volatile uint32_t* reg;
  
  uint32_t fld, i, b;
  uint32_t divider = BASE_TIME_NANOS / 4;
  
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
    reg = gpfsel + b / 10;
    fld = (b % 10) * 3;
    *reg &= ~(7<< fld);
    *reg |=  (1<< fld);       
  }

  // Set the FSEL18 field of register GPFSEL1 to 010 (GPIO Pin 18
  // takes alternate function 5).
  reg = gpfsel + 18 / 10;
  fld = (18 % 10) * 3;
  *reg &= ~(7<< fld);
  *reg |=  (2<< fld);       

  *pwmctl = USEF1 | POLA1 | CLRF1;

  // Kill the PWM clock, then set the source as 500 MHz PLLD, then set
  // the divider, then enable it again.
  *cm_pwmctl = CLK_PASSWD | CLK_KILL;
  *cm_pwmctl = CLK_PASSWD | 6;
  *cm_pwmdiv = CLK_PASSWD | divider << 12;
  *cm_pwmctl = CLK_PASSWD | CLK_ENAB | 6;

  ledpanel_clear();  
}


void ledpanel_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {

  int p;
  if (x < 0 || x >= COLUMNS || y < 0 || y >= ROWS) return;
  
  uint16_t red   = cie1931_lookup[r];
  uint16_t green = cie1931_lookup[g];
  uint16_t blue  = cie1931_lookup[b];
  
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

void ledpanel_refresh() {
  int b, col;
  uint8_t d_row;

  uint32_t color_mask = 1 << 7 | 1 << 8 | 1 << 9 | 1 << 10 | 1 << 11 | 1 << 27;

  volatile uint32_t *ctl  = (uint32_t*) PWMCTL;
  volatile uint32_t *sta  = (uint32_t*) PWMSTA;
  volatile uint32_t *rng1 = (uint32_t*) PWMRNG1;
  volatile uint32_t *fifo = (uint32_t*) PWMFIF1;
  volatile uint32_t *gpset0 = (uint32_t*) GPSET0;
  volatile uint32_t *gpclr0 = (uint32_t*) GPCLR0;
  
  for (d_row = 0; d_row < ROWS / 2; ++d_row) {
    uint32_t row_addr = d_row << 22;
    
    // Set row address (A, B, C). ABC are bits 22-24.
    *gpclr0 =  ~row_addr & (7 << 22);
    *gpset0   =   row_addr;
    
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
        *gpclr0 = 1 << 17;
        *gpclr0 =  ~out_bits & color_mask;
        *gpset0   =   out_bits & color_mask;
        *gpset0   = 1 << 17;
      }
      // Clear the clock and color
      *gpclr0 = (1 << 17) | color_mask;
      
      // OE of the previous row-data must be finished before strobe.
      while (!(*sta & 0x2));
      *ctl = USEF1 | POLA1 | CLRF1;      

      // Set and clear the strobe (bit 4)
      *gpset0 = 1 << 4;
      *gpclr0 = 1 << 4;

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

//
// Main program
//

int main(int argc, char **argv) {
  int t, x, y, i;
  
  ledpanel_init();

  for (t = 0; t < 255; t += 8) {
    int b = t;
    for (x = 0; x < 32; x++) {
      for (y = 0; y < 16; y++) {
        int r = 255 * (x / 32.0);
        int g = 255 * (y / 16.0);
        ledpanel_set_pixel(x, y, r, g, b);
        ledpanel_refresh();
      }
    }
  }
}
