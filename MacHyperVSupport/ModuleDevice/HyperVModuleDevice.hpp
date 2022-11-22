//
//  HyperVModuleDevice.hpp
//  Hyper-V module device driver (provides MMIO space for synthetic graphics and PCI passthrough)
//
//  Copyright © 2022 Goldfish64. All rights reserved.
//

#ifndef HyperVModuleDevice_hpp
#define HyperVModuleDevice_hpp

#include <IOKit/IOService.h>
#include <IOKit/IORangeAllocator.h>

#include "HyperV.hpp"

class HyperVModuleDevice : public IOService {
  OSDeclareDefaultStructors(HyperVModuleDevice);
  HVDeclareLogFunctions("hmod");
  typedef IOService super;

private:
  //
  // Range allocators for low and high allocations.
  //
  IORangeAllocator *_rangeAllocatorLow  = nullptr;
  IORangeAllocator *_rangeAllocatorHigh = nullptr;

  bool getFramebufferArea();

public:
  //
  // IOService overrides.
  //
  bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

  IORangeScalar allocateRange(IORangeScalar size, IORangeScalar alignment, bool highMemory);
  void freeRange(IORangeScalar start, IORangeScalar size);
};

#endif
