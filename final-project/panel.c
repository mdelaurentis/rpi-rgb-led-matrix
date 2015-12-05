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

#define BCM2709_PERI_BASE        0x3F000000

#define GPIO_REGISTER_OFFSET         0x200000
#define COUNTER_1Mhz_REGISTER_OFFSET   0x3000

#define GPIO_PWM_BASE_OFFSET	(GPIO_REGISTER_OFFSET + 0xC000)
#define GPIO_CLK_BASE_OFFSET	0x101000

#define REGISTER_BLOCK_SIZE (4*1024)

#define PWM_RNG1     (0x10 / 4)
#define PWM_FIFO     (0x18 / 4)


#define CLK_PASSWD  (0x5A<<24)

#define CLK_CTL_KILL    (1 <<5)
#define CLK_CTL_ENAB    (1 <<4)
#define CLK_CTL_SRC(x) ((x)<<0)

#define CLK_CTL_SRC_PLLD 6  /* 500.0 MHz */

#define CLK_DIV_DIVI(x) ((x)<<12)
#define CLK_DIV_DIVF(x) ((x)<< 0)

#define CLK_PWMCTL 40
#define CLK_PWMDIV 41

enum {
  PWEN1 = 1 << 0,
  POLA1 = 1 << 4,
  USEF1 = 1 << 5,
  CLRF1 = 1 << 6
};

struct gpio_struct {
  volatile uint32_t *port;
  volatile uint32_t *set_bits;
  volatile uint32_t *clear_bits;
  volatile uint32_t *pwm_reg;
  volatile uint32_t *pwm_fifo;
  volatile uint32_t *clk_reg;
  volatile uint32_t *pwm_ctl;
  volatile uint32_t *pwm_sta;
} the_gpio_struct;

struct gpio_struct *gpio = &the_gpio_struct;

static volatile uint32_t *timer1Mhz = NULL;

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
    return NULL;
  }
  return result;
}

// Lower values create a higher framerate, but display will be a
// bit dimmer. Good values are between 100 and 200.
static const long kBaseTimeNanos = 130;


void gpio_pulse(int c) {
  
  uint32_t pwm_range = 1 << (c + 1);

  gpio->pwm_reg[PWM_RNG1] = pwm_range;
    
  *(gpio->pwm_fifo) = pwm_range;

  /*
   * We need one value at the end to have it go back to
   * default state (otherwise it just repeats the last
   * value, so will be constantly 'on').
   */
  *(gpio->pwm_fifo) = 0;   // sentinel.

  /*
   * For some reason, we need a second empty sentinel in the
   * fifo, otherwise our way to detect the end of the pulse,
   * which relies on 'is the queue empty' does not work. It is
   * not entirely clear why that is from the datasheet,
   * but probably there is some buffering register in which data
   * elements are kept after the fifo is emptied.
   */
  *(gpio->pwm_fifo) = 0;

  *gpio->pwm_ctl =  PWEN1 | POLA1 | USEF1;
}

void gpio_wait_for_pulse() {
  // Wait until the EMPT1 of the STA register is set.
  while (!(*gpio->pwm_sta & 0x2));

  *gpio->pwm_ctl = USEF1 | POLA1 | CLRF1;
}

void gpio_init() {
  gpio->port = mmap_bcm_register(GPIO_REGISTER_OFFSET);
  gpio->set_bits = gpio->port + (0x1C / sizeof(uint32_t));
  gpio->clear_bits = gpio->port + (0x28 / sizeof(uint32_t));
  printf("GPIO Port is %x\n", gpio->port);

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

  int i = 0;
  for (i = 0; i < 12; i++) {
    int b = output_bits[i];
    *(gpio->port+((b)/10)) &= ~(7<<(((b)%10)*3));
    *(gpio->port+((b)/10)) |=  (1<<(((b)%10)*3));       
  }
  
  const uint32_t divider = kBaseTimeNanos / 4;
  assert(divider < (1<<12));  // we only have 12 bits.
    
  // Initialize timer
  uint32_t *timereg = mmap_bcm_register(COUNTER_1Mhz_REGISTER_OFFSET);
  timer1Mhz = timereg + 1;
    
  // Get relevant registers
  volatile uint32_t *gpioReg = mmap_bcm_register(GPIO_REGISTER_OFFSET);
  gpio->pwm_reg  = mmap_bcm_register(GPIO_PWM_BASE_OFFSET);
  gpio->pwm_ctl = gpio->pwm_reg;
  gpio->pwm_sta = gpio->pwm_reg + 1;
  gpio->clk_reg  = mmap_bcm_register(GPIO_CLK_BASE_OFFSET);
  gpio->pwm_fifo = gpio->pwm_reg + 6;
  assert((gpio->clk_reg != NULL) && (gpio->pwm_reg != NULL));  // init error.
  
  //    SetGPIOMode(gpioReg, 18, 2); // set GPIO 18 to PWM0 mode (Alternative 5)
  const int reg = 18 / 10;
  const int mode_pos = (18 % 10) * 3;
  gpioReg[reg] = (gpioReg[reg] & ~(7 << mode_pos)) | (2 << mode_pos);

  *gpio->pwm_ctl = USEF1 | POLA1 | CLRF1;
  
  // reset PWM clock
  gpio->clk_reg[CLK_PWMCTL] = CLK_PASSWD | CLK_CTL_KILL;
  
  // set PWM clock source as 500 MHz PLLD
  gpio->clk_reg[CLK_PWMCTL] = CLK_PASSWD | CLK_CTL_SRC(CLK_CTL_SRC_PLLD);
  
  // set PWM clock divider
  gpio->clk_reg[CLK_PWMDIV] = CLK_PASSWD | CLK_DIV_DIVI(divider) | CLK_DIV_DIVF(0);
  
  // enable PWM clock
  gpio->clk_reg[CLK_PWMCTL] = CLK_PASSWD | CLK_CTL_ENAB | CLK_CTL_SRC(CLK_CTL_SRC_PLLD);
  
}

#define BIT_PLANES 8

uint32_t color_buffer[16][11][3];
static uint16_t cie1931_lookup[256 * 100];

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

// We need one global instance of a timing correct pulser. There are different
// implementations depending on the context.


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



#define BRIGHTNESS 100

inline uint16_t map_color(uint8_t c) {
  return cie1931_lookup[c * 100 + (BRIGHTNESS - 1)];
}

#define COLUMNS 32
#define ROWS 16

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

void buf_fill(uint8_t r, uint8_t g, uint8_t b) {
  int x, y;
  for (y = 0; y < 16; y++)
    for (x = 0; x < 32; x++)
      buf_set_pixel(x, y, r, g, b);
}

int debug_counter = 0;

void buf_flush() {
  int b, col;
  uint8_t d_row;

  //  printf("In dump to matrix\n");

  uint32_t color_mask = 1 << 7 | 1 << 8 | 1 << 9 | 1 << 10 | 1 << 11 | 1 << 27;

  debug_counter = (debug_counter + 1) % 1000;
  int debug = debug_counter == 0;

  for (d_row = 0; d_row < ROWS / 2; ++d_row) {
    uint32_t row_addr = d_row << 22;
    
    // Set row address (A, B, C). ABC are bits 22-24.
    *(gpio->clear_bits) =  ~row_addr & (7 << 22);
    *(gpio->set_bits)   =   row_addr;
    
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
        *(gpio->clear_bits) = 1 << 17;
        *(gpio->clear_bits) =  ~out_bits & color_mask;
        *(gpio->set_bits)   =   out_bits & color_mask;
        *(gpio->set_bits)   = 1 << 17;
      }
      // Clear the clock and color
      *(gpio->clear_bits) = (1 << 17) | color_mask;
      

      // OE of the previous row-data must be finished before strobe.
      gpio_wait_for_pulse();

      // Set and clear the strobe (bit 4)
      *(gpio->set_bits) = 1 << 4;
      *(gpio->clear_bits) = 1 << 4;

      // Now switch on for the sleep time necessary for that bit-plane.
      gpio_pulse(b);

    }
    gpio_wait_for_pulse();
  }
}

int main(int argc, char **argv) {
  int t, x, y, i;

  
  gpio_init();
  init_buffer();
  printf("Hi\n");
  for (t = 0; t < 255; t++) {
    int b = t;
    for (x = 0; x < 32; x++) {
      for (y = 0; y < 16; y++) {
        int r = 255 * (x / 32.0);
        int g = 255 * (y / 16.0);
        buf_set_pixel(x, y, r, g, b);
      }
    }
    usleep(50);
    buf_flush();
  }

}
