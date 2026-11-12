// SPDX-License-Identifier: GPL-3.0-or-later
/* The core's screenshot.c needs libpng; a headless harness never takes screenshots. */
#include "main/screenshot.h"

void ScreenshotRomOpen(void) {}
void TakeScreenshot(int iFrameNumber) { (void)iFrameNumber; }
