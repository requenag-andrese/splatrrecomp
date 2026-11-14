// sr-recomp: stand-in for the Khronos <vulkan/vulkan.h> while only the core subset of the headers is available
// (nx2android's vendored vulkan-headers ship vulkan_core.h and vk_platform.h). Enough for the files that include vk.h
// for its types; the Vulkan back end itself needs the full headers with the Win32 platform (VK_USE_PLATFORM_WIN32_KHR).
#pragma once
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>
