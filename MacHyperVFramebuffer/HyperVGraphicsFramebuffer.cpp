//
//  HyperVGraphicsFramebuffer.cpp
//  Hyper-V synthetic graphics framebuffer driver
//
//  Copyright © 2022 Goldfish64. All rights reserved.
//

#include "HyperVGraphicsFramebuffer.hpp"

OSDefineMetaClassAndStructors(HyperVGraphicsFramebuffer, super);

bool HyperVGraphicsFramebuffer::start(IOService *provider) {


  HVCheckDebugArgs();
  HVDBGLOG("Initializing Hyper-V Synthetic Graphics Framebuffer");

  if (HVCheckOffArg()) {
    HVSYSLOG("Disabling Hyper-V Synthetic Graphics Framebuffer due to boot arg");

    return false;
  }

  if (!super::start(provider)) {
    HVSYSLOG("super::start() returned false");

    return false;
  }

  return true;
}

void HyperVGraphicsFramebuffer::stop(IOService *provider) {
  HVDBGLOG("Hyper-V Synthetic Graphics Framebuffer is stopping");

  super::stop(provider);
}

IODeviceMemory* HyperVGraphicsFramebuffer::getApertureRange(IOPixelAperture aperture) {
  return nullptr;
}

const char* HyperVGraphicsFramebuffer::getPixelFormats() {
  return nullptr;
}

IOItemCount HyperVGraphicsFramebuffer::getDisplayModeCount() {
  return 1;
}

IOReturn HyperVGraphicsFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
  //allDisplayModes[0] = kUEFIDisplayModeID;
  return kIOReturnUnsupported;
}

IOReturn HyperVGraphicsFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode, IODisplayModeInformation *info) {

  return kIOReturnUnsupported;
}

UInt64 HyperVGraphicsFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
  //
  // Obsolete method that always returns zero.
  //
  return 0;
}

IOReturn HyperVGraphicsFramebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pixelInfo) {


  return kIOReturnUnsupported;
}

IOReturn HyperVGraphicsFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) {

  return kIOReturnUnsupported;
}
