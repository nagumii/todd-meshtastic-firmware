// This file allows hardware vendors to set a custom screensaver frame for E-Ink displays
// To enable this option: add build flag "-D EINK_CUSTOM_SCREENSAVER=myCustomScreensaver" to variant's platformio.ini file,
// Then set display.eink_screensaver preference to CUSTOM

#pragma once

#include "configuration.h"

#if defined(USE_EINK) && defined(EINK_CUSTOM_SCREENSAVER)

// Used to log value of EINK_CUSTOM_SCREENSAVER
#ifndef __STRINGIFY
#define __STRINGIFY(a) #a // Defined for ESP32-S3, not sure about other platforms
#endif
#ifndef EINK_SCREENSAVER_TOSTRING
#define EINK_SCREENSAVER_TOSTRING(a) __STRINGIFY(a)
#endif

#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

namespace einkCustomScreensavers
{

/*
 * 1. Write your custom method here
 *
 * 2. Define EINK_CUSTOM_SCREENSAVER is variant's platformio.ini build flags:
 *    -D EINK_CUSTOM_SCREENSAVER helloWorld
 *    See variants/heltec_wireless_paper/platformio.ini
 *
 * 3. Regen protobufs for both firmware, and CLI, using modified config.proto file
 *    Archived in this branches root.
 *
 *  4. Set the display.eink_screensaver setting to CUSTOM
 */

void helloWorld(OLEDDisplay *display)
{
    // Macros EINK_WHITE and EINK_BLACK provide the correct colors for drawing

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawString(30, 30, "Hello world!");
}

} // namespace einkCustomScreensavers

#endif