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

#ifndef RPI_GPIO_H
#define RPI_GPIO_H

#include <stdint.h>

struct gpio_struct {
  volatile uint32_t *port;
  volatile uint32_t *set_bits;
  volatile uint32_t *clear_bits;
  volatile uint32_t *pwm_reg;
  volatile uint32_t *fifo;
  volatile uint32_t *clk_reg;
};


void gpio_pulse(int c);
void gpio_wait_for_pulse();
void gpio_init();

struct gpio_struct *get_gpio();

#endif  // RPI_GPIO_H

