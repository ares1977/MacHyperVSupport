//
//  HyperVGraphicsBridge.cpp
//  Hyper-V synthetic graphics bridge
//
//  Copyright © 2021-2022 Goldfish64. All rights reserved.
//

#include "HyperVGraphicsBridge.hpp"
#include "HyperVPCIRoot.hpp"
#include "HyperVGraphicsProvider.hpp"

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>

OSDefineMetaClassAndStructors(HyperVGraphicsBridge, super);

bool HyperVGraphicsBridge::start(IOService *provider) {
  HyperVGraphicsProvider *gfxProvider;

  //
  // Get parent HyperVGraphicsProvider object.
  //
  gfxProvider = OSDynamicCast(HyperVGraphicsProvider, provider);
  if (gfxProvider == nullptr) {
    HVSYSLOG("Provider is not HyperVVMBusDevice");
    return false;
  }

  //
  // Get initial framebuffer info.
  //
  gfxProvider->getFramebufferArea(&_fbBaseAddress, &_fbLength);

  HVCheckDebugArgs();
  HVDBGLOG("Initializing Hyper-V Synthetic Graphics Bridge");

  if (HVCheckOffArg()) {
    HVSYSLOG("Disabling Hyper-V Synthetic Graphics Bridge due to boot arg");
    return false;
  }

  //
  // Do not start on Gen1 VMs.
  //
  IORegistryEntry *pciEntry = IORegistryEntry::fromPath("/PCI0@0", gIODTPlane);
  if (pciEntry != nullptr) {
    HVDBGLOG("Existing PCI bus found (Gen1 VM), will not start");

    OSSafeReleaseNULL(pciEntry);
    return false;
  }

  //
  // Locate root PCI bus instance and register ourselves.
  //
  if (!HyperVPCIRoot::registerChildPCIBridge(this)) {
    HVSYSLOG("Failed to register with root PCI bus instance");
    return false;
  }

  _pciLock = IOSimpleLockAlloc();
  fillFakePCIDeviceSpace();

  if (!super::start(provider)) {
    HVSYSLOG("super::start() returned false");
    return false;
  }

  //
  // Add a friendly name to the child device produced.
  //
  OSIterator *childIterator = getChildIterator(gIOServicePlane);
  if (childIterator != NULL) {
    childIterator->reset();
    
    IOService *childService = OSDynamicCast(IOService, childIterator->getNextObject());
    if (childService != NULL) {
      HVDBGLOG("Found child %s", childService->getName());
      childService->setProperty("model", "Hyper-V Graphics");
    }

    childIterator->release();
  }

  HVDBGLOG("Initialized Hyper-V Synthetic Graphics Bridge");
  return true;
}

void HyperVGraphicsBridge::stop(IOService *provider) {
  HVDBGLOG("Hyper-V Synthetic Graphics Bridge is stopping");
  super::stop(provider);
}

bool HyperVGraphicsBridge::configure(IOService *provider) {
  //
  // Add framebuffer memory range to bridge.
  //
  bool result = addBridgeMemoryRange(_fbBaseAddress, _fbLength, true);
  HVSYSLOG("Got base at 0x%llX 0x%llX - %u", _fbBaseAddress, _fbLength, result);
  
  return super::configure(provider);
}

UInt32 HyperVGraphicsBridge::configRead32(IOPCIAddressSpace space, UInt8 offset) {
  HVDBGLOG("Bus: %u, device: %u, function: %u, offset %X", space.es.busNum, space.es.deviceNum, space.es.functionNum, offset);
  
  UInt32 data;
  IOInterruptState ints;
  
  if (space.es.deviceNum != 0 || space.es.functionNum != 0) {
    return 0xFFFFFFFF;
  }
  
  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  data = OSReadLittleInt32(_fakePCIDeviceSpace, offset);
  
  if (offset == kIOPCIConfigurationOffsetBaseAddress0) {
    HVDBGLOG("gonna read %X", data);
  }
  
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);
  return data;
}

void HyperVGraphicsBridge::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data) {
  HVDBGLOG("Bus: %u, device: %u, function: %u, offset %X", space.es.busNum, space.es.deviceNum, space.es.functionNum, offset);
  
  IOInterruptState ints;
  
  if (space.es.deviceNum != 0 || space.es.functionNum != 0 || (offset > kIOPCIConfigurationOffsetBaseAddress0 && offset <= kIOPCIConfigurationOffsetBaseAddress5) || offset == kIOPCIConfigurationOffsetExpansionROMBase) {
    HVDBGLOG("ignoring offset %X", offset);
    return;
  }
  
  if (offset == kIOPCIConfigurationOffsetBaseAddress0) {
    HVDBGLOG("gonna write %X", data);
  }
  
  if (offset == kIOPCIConfigurationOffsetBaseAddress0 && data == 0xFFFFFFFF) {
    HVDBGLOG("Got bar size request");
    OSWriteLittleInt32(_fakePCIDeviceSpace, offset, (0xFFFFFFFF - (UInt32) _fbLength) + 1);
    return;
  }
  
  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  OSWriteLittleInt32(_fakePCIDeviceSpace, offset, data);
  
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);
}

UInt16 HyperVGraphicsBridge::configRead16(IOPCIAddressSpace space, UInt8 offset) {
  HVDBGLOG("Bus: %u, device: %u, function: %u, offset %X", space.es.busNum, space.es.deviceNum, space.es.functionNum, offset);
  
  UInt16 data;
  IOInterruptState ints;
  
  if (space.es.deviceNum != 0 || space.es.functionNum != 0) {
    return 0xFFFF;
  }
  
  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  data = OSReadLittleInt16(_fakePCIDeviceSpace, offset);
  
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);
  return data;
}

void HyperVGraphicsBridge::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data) {
  HVDBGLOG("Bus: %u, device: %u, function: %u, offset %X", space.es.busNum, space.es.deviceNum, space.es.functionNum, offset);
  
  IOInterruptState ints;
  
  if (space.es.deviceNum != 0 || space.es.functionNum != 0 || (offset >= kIOPCIConfigurationOffsetBaseAddress0 && offset <= kIOPCIConfigurationOffsetBaseAddress5) || offset == kIOPCIConfigurationOffsetExpansionROMBase) {
    return;
  }
  
  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  OSWriteLittleInt16(_fakePCIDeviceSpace, offset, data);
  
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);
}

UInt8 HyperVGraphicsBridge::configRead8(IOPCIAddressSpace space, UInt8 offset) {
  HVDBGLOG("Bus: %u, device: %u, function: %u, offset %X", space.es.busNum, space.es.deviceNum, space.es.functionNum, offset);
  
  UInt8 data;
  IOInterruptState ints;
  
  if (space.es.deviceNum != 0 || space.es.functionNum != 0) {
    return 0xFF;
  }
  
  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  data = _fakePCIDeviceSpace[offset];
  
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);
  return data;
}

void HyperVGraphicsBridge::configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data) {
  HVDBGLOG("Bus: %u, device: %u, function: %u, offset %X", space.es.busNum, space.es.deviceNum, space.es.functionNum, offset);
  
  IOInterruptState ints;
  
  if (space.es.deviceNum != 0 || space.es.functionNum != 0 || (offset >= kIOPCIConfigurationOffsetBaseAddress0 && offset <= kIOPCIConfigurationOffsetBaseAddress5) || offset == kIOPCIConfigurationOffsetExpansionROMBase) {
    return;
  }
  
  ints = IOSimpleLockLockDisableInterrupt(_pciLock);
  _fakePCIDeviceSpace[offset] = data;
  
  IOSimpleLockUnlockEnableInterrupt(_pciLock, ints);
}

void HyperVGraphicsBridge::fillFakePCIDeviceSpace() {
  //
  // Fill PCI device config space.
  //
  // PCI bridge will contain a single PCI graphics device
  // with the framebuffer memory at BAR0. The vendor/device ID is
  // the same as what a generation 1 Hyper-V VM uses for the
  // emulated graphics.
  //
  bzero(_fakePCIDeviceSpace, sizeof (_fakePCIDeviceSpace));

  OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigVendorID, kHyperVPCIVendorMicrosoft);
  OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigDeviceID, kHyperVPCIDeviceHyperVVideo);
  OSWriteLittleInt32(_fakePCIDeviceSpace, kIOPCIConfigRevisionID, 0x3000000);
  OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigSubSystemVendorID, kHyperVPCIVendorMicrosoft);
  OSWriteLittleInt16(_fakePCIDeviceSpace, kIOPCIConfigSubSystemID, kHyperVPCIDeviceHyperVVideo);

  OSWriteLittleInt32(_fakePCIDeviceSpace, kIOPCIConfigBaseAddress0, (UInt32)_fbBaseAddress);
 // OSWriteLittleInt32(_fakePCIDeviceSpace, kIOPCIConfigBaseAddress0, (UInt32)(_fbBaseAddress >> 32));
}
