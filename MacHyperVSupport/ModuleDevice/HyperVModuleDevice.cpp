//
//  HyperVModuleDevice.cpp
//  Hyper-V module device driver (provides MMIO space for synthetic graphics and PCI passthrough)
//
//  Copyright © 2022 Goldfish64. All rights reserved.
//

#include "HyperVModuleDevice.hpp"
#include "HyperVGraphicsProvider.hpp"
#include "AppleACPIRange.hpp"

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

  getFramebufferArea();

  HVDBGLOG("Hyper-V Module Device initialized with free size: %u bytes (low) %u bytes (high)",
           _rangeAllocatorLow->getFreeCount(), _rangeAllocatorHigh->getFreeCount());
  registerService();
  return true;
}

void HyperVModuleDevice::stop(IOService *provider) {
  OSSafeReleaseNULL(_rangeAllocatorLow);
  OSSafeReleaseNULL(_rangeAllocatorHigh);
}

bool HyperVModuleDevice::getFramebufferArea() {
  OSDictionary           *gfxProvMatching;
  IOService              *gfxProvService;
  HyperVGraphicsProvider *gfxProvider;
  IORangeScalar          fbBase;
  IORangeScalar          fbTotalLength;
  IORangeScalar          fbInitialLength;

  //
  // Get HyperVGraphicsProvider instance.
  // We'll query it for the current framebuffer location and size.
  //
  gfxProvMatching = IOService::serviceMatching("HyperVGraphicsProvider");
  if (gfxProvMatching == nullptr) {
    HVSYSLOG("Failed to create HyperVGraphicsProvider matching dictionary");
    return false;
  }

  HVDBGLOG("Waiting for HyperVGraphicsProvider");
#if __MAC_OS_X_VERSION_MIN_REQUIRED < __MAC_10_6
  gfxProvService = IOService::waitForService(gfxProvMatching);
  if (gfxProvService != nullptr) {
    gfxProvService->retain();
  }
#else
  gfxProvService = waitForMatchingService(gfxProvMatching);
  gfxProvMatching->release();
#endif

  if (gfxProvService == nullptr) {
    HVSYSLOG("Failed to locate HyperVGraphicsProvider");
    return false;
  }
  gfxProvider = OSDynamicCast(HyperVGraphicsProvider, gfxProvService);
  if (gfxProvider == nullptr) {
    gfxProvService->release();
    HVSYSLOG("Failed to locate HyperVGraphicsProvider");
    return false;
  }
  HVDBGLOG("Got instance of HyperVGraphicsProvider");

  gfxProvider->getFramebufferArea(&fbBase, &fbTotalLength, &fbInitialLength);
  gfxProvService->release();

  //
  // Reserve FB memory.
  //
  HVDBGLOG("Framebuffer is at 0x%llX (%llu bytes)", fbBase, fbTotalLength);
  _rangeAllocatorLow->allocateRange(fbBase, fbTotalLength);
  return true;
}

IORangeScalar HyperVModuleDevice::allocateRange(IORangeScalar size, IORangeScalar alignment, bool highMemory) {
  IORangeScalar range;
  bool result = false;

  if (highMemory) {
    result = _rangeAllocatorHigh->allocate(size, &range, alignment);
  } else {
    result = _rangeAllocatorLow->allocate(size, &range, alignment);
  }
  HVDBGLOG("Allocation result for size 0x%llX (high: %u) - %u", size, highMemory, result);
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
