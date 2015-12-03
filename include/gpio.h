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
};


// A PinPulser is a utility class that pulses a GPIO pin.
class PinPulser {
public:

  PinPulser();
  
  // Send a pulse with a given length (index into nano_wait_spec array).
  void SendPulse(int time_spec_number);

  // If SendPulse() is asynchronously implemented, wait for pulse to finish.
  void WaitPulseFinished();

private:
  volatile uint32_t *pwm_reg_;
  volatile uint32_t *fifo_;
  volatile uint32_t *clk_reg_;
};


void gpio_init(struct gpio_struct * gpio);
void gpio_init_outputs(struct gpio_struct * gpio);
// void gpio_set(struct gpio_struct *gpio, uint32_t);

#endif  // RPI_GPIO_H

