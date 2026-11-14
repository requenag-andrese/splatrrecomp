// sr-recomp: the opaque Vulkan Memory Allocator types runtime/core/vk.h names, for building without the Vulkan back
// end (gfx_null). Only on the include path of the null build: the Vulkan build uses AMD's real vk_mem_alloc.h.
#pragma once
typedef struct VmaAllocator_T *VmaAllocator;
typedef struct VmaAllocation_T *VmaAllocation;
typedef struct VmaDeviceMemoryCallbacks VmaDeviceMemoryCallbacks;
