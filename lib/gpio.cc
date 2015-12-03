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

#include "gpio.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define BCM2709_PERI_BASE        0x3F000000

#define GPIO_REGISTER_OFFSET         0x200000
#define COUNTER_1Mhz_REGISTER_OFFSET   0x3000

#define GPIO_PWM_BASE_OFFSET	(GPIO_REGISTER_OFFSET + 0xC000)
#define GPIO_CLK_BASE_OFFSET	0x101000

#define REGISTER_BLOCK_SIZE (4*1024)

#define PWM_CTL      (0x00 / 4)
#define PWM_STA      (0x04 / 4)
#define PWM_RNG1     (0x10 / 4)
#define PWM_FIFO     (0x18 / 4)

#define PWM_CTL_CLRF1 (1<<6)	// CH1 Clear Fifo (1 Clears FIFO 0 has no effect)
#define PWM_CTL_USEF1 (1<<5)	// CH1 Use Fifo (0=data reg transmit 1=Fifo used for transmission)
#define PWM_CTL_POLA1 (1<<4)	// CH1 Polarity (0=(0=low 1=high) 1=(1=low 0=high)
#define PWM_CTL_SBIT1 (1<<3)	// CH1 Silence Bit (state of output when 0 transmission takes place)
#define PWM_CTL_MODE1 (1<<1)	// CH1 Mode (0=pwm 1=serialiser mode)
#define PWM_CTL_PWEN1 (1<<0)	// CH1 Enable (0=disable 1=enable)

#define PWM_STA_EMPT1 (1<<1)
#define PWM_STA_FULL1 (1<<0)

#define CLK_PASSWD  (0x5A<<24)

#define CLK_CTL_KILL    (1 <<5)
#define CLK_CTL_ENAB    (1 <<4)
#define CLK_CTL_SRC(x) ((x)<<0)

#define CLK_CTL_SRC_PLLD 6  /* 500.0 MHz */

#define CLK_DIV_DIVI(x) ((x)<<12)
#define CLK_DIV_DIVF(x) ((x)<< 0)

#define CLK_PWMCTL 40
#define CLK_PWMDIV 41


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

void gpio_init_pulser(struct gpio_struct *gpio) {
  
  const uint32_t divider = kBaseTimeNanos / 4;
  assert(divider < (1<<12));  // we only have 12 bits.
    
  // Initialize timer
  uint32_t *timereg = mmap_bcm_register(COUNTER_1Mhz_REGISTER_OFFSET);
  timer1Mhz = timereg + 1;
    
  // Get relevant registers
  volatile uint32_t *gpioReg = mmap_bcm_register(GPIO_REGISTER_OFFSET);
  gpio->pwm_reg  = mmap_bcm_register(GPIO_PWM_BASE_OFFSET);
  gpio->clk_reg  = mmap_bcm_register(GPIO_CLK_BASE_OFFSET);
  gpio->fifo = gpio->pwm_reg + PWM_FIFO;
  assert((gpio->clk_reg != NULL) && (gpio->pwm_reg != NULL));  // init error.
  
  //    SetGPIOMode(gpioReg, 18, 2); // set GPIO 18 to PWM0 mode (Alternative 5)
  const int reg = 18 / 10;
  const int mode_pos = (18 % 10) * 3;
  gpioReg[reg] = (gpioReg[reg] & ~(7 << mode_pos)) | (2 << mode_pos);
  
  gpio->pwm_reg[PWM_CTL] = PWM_CTL_USEF1 | PWM_CTL_POLA1 | PWM_CTL_CLRF1;
  
  // reset PWM clock
  gpio->clk_reg[CLK_PWMCTL] = CLK_PASSWD | CLK_CTL_KILL;
  
  // set PWM clock source as 500 MHz PLLD
  gpio->clk_reg[CLK_PWMCTL] = CLK_PASSWD | CLK_CTL_SRC(CLK_CTL_SRC_PLLD);
  
  // set PWM clock divider
  gpio->clk_reg[CLK_PWMDIV] = CLK_PASSWD | CLK_DIV_DIVI(divider) | CLK_DIV_DIVF(0);
  
  // enable PWM clock
  gpio->clk_reg[CLK_PWMCTL] = CLK_PASSWD | CLK_CTL_ENAB | CLK_CTL_SRC(CLK_CTL_SRC_PLLD);
  
}

void gpio_pulse(struct gpio_struct *gpio, int c) {
  uint32_t pwm_range = 1 << (c + 1);
  if (pwm_range < 16) {
    gpio->pwm_reg[PWM_RNG1] = pwm_range;
    
    *(gpio->fifo) = pwm_range;
  } else {
    // Keep the actual range as short as possible, as we have to
    // wait for one full period of these in the zero phase.
    // The hardware can't deal with values < 2, so only do this when
    // have enough of these.
    gpio->pwm_reg[PWM_RNG1] = pwm_range / 8;
    
    *(gpio->fifo) = pwm_range / 8;
    *(gpio->fifo) = pwm_range / 8;
    *(gpio->fifo) = pwm_range / 8;
    *(gpio->fifo) = pwm_range / 8;
    *(gpio->fifo) = pwm_range / 8;
    *(gpio->fifo) = pwm_range / 8;
    *(gpio->fifo) = pwm_range / 8;
    *(gpio->fifo) = pwm_range / 8;
  }

  /*
   * We need one value at the end to have it go back to
   * default state (otherwise it just repeats the last
   * value, so will be constantly 'on').
   */
  *(gpio->fifo) = 0;   // sentinel.

  /*
   * For some reason, we need a second empty sentinel in the
   * fifo, otherwise our way to detect the end of the pulse,
   * which relies on 'is the queue empty' does not work. It is
   * not entirely clear why that is from the datasheet,
   * but probably there is some buffering register in which data
   * elements are kept after the fifo is emptied.
   */
  *(gpio->fifo) = 0;
  
  gpio->pwm_reg[PWM_CTL] = PWM_CTL_USEF1 | PWM_CTL_PWEN1 | PWM_CTL_POLA1;
}

void gpio_wait_for_pulse(struct gpio_struct *gpio) {
  // busy wait until done.  
  while ((gpio->pwm_reg[PWM_STA] & PWM_STA_EMPT1) == 0);

  gpio->pwm_reg[PWM_CTL] = PWM_CTL_USEF1 | PWM_CTL_POLA1 | PWM_CTL_CLRF1;
}

void gpio_init(struct gpio_struct *gpio) {
  gpio->port = mmap_bcm_register(GPIO_REGISTER_OFFSET);
  gpio->set_bits = gpio->port + (0x1C / sizeof(uint32_t));
  gpio->clear_bits = gpio->port + (0x28 / sizeof(uint32_t));
  printf("GPIO Port is %x\n", gpio->port);
}

void gpio_init_outputs(struct gpio_struct *gpio) {

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
}
