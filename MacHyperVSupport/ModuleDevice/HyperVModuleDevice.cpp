//
//  HyperVModuleDevice.cpp
//  Hyper-V module device driver (provides MMIO space for synthetic graphics and PCI passthrough)
//
//  Copyright © 2022 Goldfish64. All rights reserved.
//

#include "HyperVModuleDevice.hpp"

#include "AppleACPIRange.hpp"
#include <IOKit/IOPlatformExpert.h>

OSDefineMetaClassAndStructors(HyperVModuleDevice, super);

bool HyperVModuleDevice::start(IOService *provider) {
  AppleACPIRange *acpiRanges;
  UInt32         acpiRangeCount;

  OSArray        *deviceMemoryArray;
  IODeviceMemory *deviceMemory;

  HVCheckDebugArgs();

  if (!super::start(provider)) {
    HVSYSLOG("super::start() returned false");
    return false;
  }

  //
  // Add memory ranges from ACPI.
  //
  OSData *acpiAddressSpaces = OSDynamicCast(OSData, provider->getProperty("acpi-address-spaces"));
  if (acpiAddressSpaces == nullptr) {
    HVSYSLOG("Unable to locate acpi-address-spaces property on VMOD device");
    stop(provider);
    return false;
  }

  acpiRanges     = (AppleACPIRange*) acpiAddressSpaces->getBytesNoCopy();
  acpiRangeCount = acpiAddressSpaces->getLength() / sizeof (AppleACPIRange);

  deviceMemoryArray   = OSArray::withCapacity(acpiRangeCount);
  _rangeAllocatorLow  = IORangeAllocator::withRange(0);
  _rangeAllocatorHigh = IORangeAllocator::withRange(0);
  if (deviceMemoryArray == nullptr || _rangeAllocatorLow == nullptr || _rangeAllocatorHigh == nullptr) {
    HVSYSLOG("Unable to allocate range allocators");

    OSSafeReleaseNULL(deviceMemoryArray);
    stop(provider);
    return false;
  }

  for (int i = 0; i < acpiRangeCount; i++) {
    HVDBGLOG("Range type %u, min 0x%llX, max 0x%llX, len 0x%llX, high %u", acpiRanges[i].type, acpiRanges[i].min,
             acpiRanges[i].max, acpiRanges[i].length, acpiRanges[i].min > UINT32_MAX);

    deviceMemory = IODeviceMemory::withRange(static_cast<IOPhysicalAddress>(acpiRanges[i].min), static_cast<IOPhysicalLength>(acpiRanges[i].length));
    if (deviceMemory == nullptr) {
      HVSYSLOG("Unable to allocate device memory for range 0x%llX", acpiRanges[i].min);
      OSSafeReleaseNULL(deviceMemoryArray);
      stop(provider);
      return false;
    }

    //
    // Add to device memory array and appropriate range allocator.
    //
    deviceMemoryArray->setObject(deviceMemory);
    deviceMemory->release();
    if (acpiRanges[i].min > UINT32_MAX) {
      _rangeAllocatorHigh->deallocate(static_cast<IOPhysicalAddress>(acpiRanges[i].min), static_cast<IOPhysicalLength>(acpiRanges[i].length));
    } else {
      _rangeAllocatorLow->deallocate(static_cast<IOPhysicalAddress>(acpiRanges[i].min), static_cast<IOPhysicalLength>(acpiRanges[i].length));
    }
  }

  //
  // Set device memory with found ranges.
  //
  setDeviceMemory(deviceMemoryArray);
  deviceMemoryArray->release();

  reserveFramebufferArea();

  HVDBGLOG("Hyper-V Module Device initialized with free size: %u bytes (low) %u bytes (high)",
           _rangeAllocatorLow->getFreeCount(), _rangeAllocatorHigh->getFreeCount());
  registerService();
  return true;
}

void HyperVModuleDevice::stop(IOService *provider) {
  OSSafeReleaseNULL(_rangeAllocatorLow);
  OSSafeReleaseNULL(_rangeAllocatorHigh);
}

bool HyperVModuleDevice::reserveFramebufferArea() {
  PE_Video consoleInfo;
  IORangeScalar fbStart;
  IORangeScalar fbLength;

  //
  // Pull console info. We'll use the base address but the length will be gathered from Hyper-V.
  //
  if (getPlatform()->getConsoleInfo(&consoleInfo) != kIOReturnSuccess) {
    HVSYSLOG("Failed to get console info");
    return false;
  }
  fbStart  = consoleInfo.v_baseAddr;
  fbLength = consoleInfo.v_height * consoleInfo.v_rowBytes;
  HVDBGLOG("Console is at 0x%X size 0x%X (%ux%u, bpp: %u, bytes/row: %u)",
           fbStart, fbLength, consoleInfo.v_width, consoleInfo.v_height,
           consoleInfo.v_depth, consoleInfo.v_rowBytes);

  //
  // Allocate intial framebuffer area to prevent use.
  // On some versions of Hyper-V, the initial framebuffer may not actually be in the MMIO ranges.
  // This can be silently ignored.
  //
  if (fbStart > UINT32_MAX) {
    _rangeAllocatorHigh->allocateRange(fbStart, fbLength);
  } else {
    _rangeAllocatorLow->allocateRange(fbStart, fbLength);
  }
  return true;
}

IORangeScalar HyperVModuleDevice::allocateRange(IORangeScalar size, IORangeScalar alignment, IORangeScalar maxAddress) {
  IORangeScalar range = 0;
  bool result         = false;

  if (maxAddress > UINT32_MAX) {
    result = _rangeAllocatorHigh->allocate(size, &range, alignment);
  }
  if (!result) {
    result = _rangeAllocatorLow->allocate(size, &range, alignment);
  }

  HVDBGLOG("Allocation result for size 0x%llX (max: 0x%llX) - %u", size, maxAddress, result);
  HVDBGLOG("Range result: 0x%llX", result ? range : 0);

  return result ? range : 0;
}

void HyperVModuleDevice::freeRange(IORangeScalar start, IORangeScalar size) {
  if (start > UINT32_MAX) {
    _rangeAllocatorHigh->deallocate(start, size);
  } else {
    _rangeAllocatorLow->deallocate(start, size);
  }
}
