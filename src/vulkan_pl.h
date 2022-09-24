/*
   vulkan_pl.h
*/

/*
Index of this file:
// [SECTION] includes
// [SECTION] globals
*/

#ifndef WIN32_PL_H
#define WIN32_PL_H

//-----------------------------------------------------------------------------
// [SECTION] includes
//-----------------------------------------------------------------------------

#include "vulkan_pl_graphics.h"

//-----------------------------------------------------------------------------
// [SECTION] globals
//-----------------------------------------------------------------------------

plVulkanDevice    gDevice       = {0};
plVulkanGraphics  gGraphics     = {0};
plVulkanSwapchain gSwapchain    = { .vsync = true };
bool              gRunning      = true;
int               gActualWidth  = 0;
int               gActualHeight = 0;
int               gClientWidth  = 500;
int               gClientHeight = 500;

#endif // WIN32_PL_H