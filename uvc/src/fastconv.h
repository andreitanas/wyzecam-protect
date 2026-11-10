#ifndef UVC_FASTCONV_H
#define UVC_FASTCONV_H

#include "net.h"

// Convolution layers for ncnn written for the T20's FPU, in place of ncnn's
// own. A multiply-add there delivers its result after 15 cycles but a new
// one can start every 2, so a loop is fast only while many independent sums
// are in flight: these compute 16 outputs (4 pixels of 4 channels) or 8
// pixels (depthwise) at a time with every sum in a register. ncnn's loops
// for a CPU without SIMD keep 1 to 4 sums going. docs/kernel-modern.md,
// "Object detector".
// Call before the net loads its param file. Turns ncnn's channel packing off.
void fastconv_register(ncnn::Net &net);

#endif
