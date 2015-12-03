// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// Small example how to use the library.
// For more examples, look at demo-main.cc
//
// This code is public domain
// (but note, that the led-matrix library this depends on is GPL v2)

#include "led-matrix.h"

#include <unistd.h>
#include "gpio.h"
using rgb_matrix::RGBMatrix;
using rgb_matrix::Canvas;


int main(int argc, char *argv[]) {
  /*
   * Set up GPIO pins. This fails when not running as root.
   */
  struct gpio_struct io;
  gpio_init(&io);
    
  /*
   * Set up the RGBMatrix. It implements a 'Canvas' interface.
   */
  int rows = 16;    // A 32x32 display. Use 16 when this is a 16x32 display.
  int chain = 1;    // Number of boards chained together.
  Canvas *canvas = new RGBMatrix(&io, rows, chain);

  for (int t = 0; t < 255; t++) {
    int b = t;
    for (int x = 0; x < 32; x++) {
      for (int y = 0; y < 16; y++) {
        int r = 255 * (x / 32.0);
        int g = 255 * (y / 16.0);
        canvas->SetPixel(x, y, r, g, b);
        usleep(1);
      }
    }
  }
  usleep(1000 * 1000 * 3);

  canvas->Clear();
  delete canvas;

  return 0;
}
