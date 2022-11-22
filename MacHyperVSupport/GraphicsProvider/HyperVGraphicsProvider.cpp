//
//  HyperVGraphicsProvider.cpp
//  Hyper-V synthetic graphics provider
//
//  Copyright © 2021-2022 Goldfish64. All rights reserved.
//

#include "HyperVGraphicsProvider.hpp"

OSDefineMetaClassAndStructors(HyperVGraphicsProvider, super);

bool HyperVGraphicsProvider::start(IOService *provider) {
  bool     result = false;
  IOReturn status;
  PE_Video consoleInfo;
  OSNumber *mmioBytesNumber;

  //
  // Get parent VMBus device object.
  //
  _hvDevice = OSDynamicCast(HyperVVMBusDevice, provider);
  if (_hvDevice == nullptr) {
    HVSYSLOG("Provider is not HyperVVMBusDevice");
    return false;
  }
  _hvDevice->retain();

  HVCheckDebugArgs();
  HVDBGLOG("Initializing Hyper-V Synthetic Graphics Provider");

  if (HVCheckOffArg()) {
    HVSYSLOG("Disabling Hyper-V Synthetic Graphics Provider due to boot arg");
    OSSafeReleaseNULL(_hvDevice);
    return false;
  }

  if (!super::start(provider)) {
    HVSYSLOG("super::start() returned false");
    OSSafeReleaseNULL(_hvDevice);
    return false;
  }

  do {
    //
    // Pull console info. We'll use the base address but the length will be gathered from Hyper-V.
    //
    if (getPlatform()->getConsoleInfo(&consoleInfo) != kIOReturnSuccess) {
      HVSYSLOG("Failed to get console info");
      break;
    }
    HVDBGLOG("Console is at 0x%X (%ux%u, bpp: %u, bytes/row: %u)",
             consoleInfo.v_baseAddr, consoleInfo.v_height, consoleInfo.v_width, consoleInfo.v_depth, consoleInfo.v_rowBytes);
    _fbBaseAddress = consoleInfo.v_baseAddr;

    //
    // Get MMIO bytes.
    //
    mmioBytesNumber = OSDynamicCast(OSNumber, provider->getProperty(kHyperVVMBusDeviceChannelMMIOByteCount));
    if (mmioBytesNumber == nullptr) {
      HVSYSLOG("Failed to get MMIO byte count");
      break;
    }
    _fbTotalLength = mmioBytesNumber->unsigned64BitValue();
    HVDBGLOG("Framebuffer MMIO size: %llu bytes", _fbTotalLength);
    _fbInitialLength = consoleInfo.v_height * consoleInfo.v_rowBytes;

    //
    // Install packet handler.
    //
    status = _hvDevice->installPacketActions(this, OSMemberFunctionCast(HyperVVMBusDevice::PacketReadyAction, this, &HyperVGraphicsProvider::handlePacket),
                                             nullptr, kHyperVGraphicsMaxPacketSize);
    if (status != kIOReturnSuccess) {
      HVSYSLOG("Failed to install packet handler with status 0x%X", status);
      break;
    }

    //
    // Open VMBus channel and connect to graphics system.
    //
    status = _hvDevice->openVMBusChannel(kHyperVGraphicsRingBufferSize, kHyperVGraphicsRingBufferSize);
    if (status != kIOReturnSuccess) {
      HVSYSLOG("Failed to open VMBus channel with status 0x%X", status);
      break;
    }

    status = connectGraphics();
    if (status != kIOReturnSuccess) {
      HVSYSLOG("Failed to connect to graphics device with status 0x%X", status);
      break;
    }

    registerService();
    HVDBGLOG("Initialized Hyper-V Synthetic Graphics Provider");
    result = true;
  } while (false);

  if (!result) {
    stop(provider);
  }
  return result;
}

void HyperVGraphicsProvider::stop(IOService *provider) {
  HVDBGLOG("Hyper-V Synthetic Graphics Provider is stopping");

  if (_hvDevice != nullptr) {
    _hvDevice->closeVMBusChannel();
    _hvDevice->uninstallPacketActions();
    OSSafeReleaseNULL(_hvDevice);
  }

  super::stop(provider);
}

void HyperVGraphicsProvider::getFramebufferArea(IORangeScalar *baseAddress, IORangeScalar *totalLength, IORangeScalar *initialLength) {
  *baseAddress   = _fbBaseAddress;
  *totalLength   = _fbTotalLength;
  *initialLength = _fbInitialLength;
}
