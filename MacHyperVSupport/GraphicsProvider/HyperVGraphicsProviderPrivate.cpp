//
//  HyperVGraphicsProviderPrivate.cpp
//  Hyper-V synthetic graphics provider
//
//  Copyright © 2021-2022 Goldfish64. All rights reserved.
//

#include "HyperVGraphicsProvider.hpp"
#include "HyperVModuleDevice.hpp"
#include "HyperVPlatformProvider.hpp"

#define kBootLogoImageHeight    100
#define kBootLogoImageWidth     100

static const VMBusVersion graphicsVersions[] = {
  kHyperVGraphicsVersionV3_2,
  kHyperVGraphicsVersionV3_0
};

void HyperVGraphicsProvider::handlePacket(VMBusPacketHeader *pktHeader, UInt32 pktHeaderLength, UInt8 *pktData, UInt32 pktDataLength) {
  HyperVGraphicsMessage *gfxMsg = (HyperVGraphicsMessage*) pktData;
  void                  *responseBuffer;
  UInt32                responseLength;

  if (gfxMsg->pipeHeader.type != kHyperVGraphicsPipeMessageTypeData || gfxMsg->pipeHeader.size < __offsetof (HyperVGraphicsMessage, gfxHeader.size)) {
    HVDBGLOG("Invalid pipe packet receieved (type 0x%X, size %u)", gfxMsg->pipeHeader.type, gfxMsg->pipeHeader.size);
    return;
  }

  HVDBGLOG("Received packet type 0x%X (%u bytes)", gfxMsg->gfxHeader.type, gfxMsg->gfxHeader.size);
  switch (gfxMsg->gfxHeader.type) {
    case kHyperVGraphicsMessageTypeVersionResponse:
    case kHyperVGraphicsMessageTypeVRAMAck:
    case kHyperVGraphicsMessageTypeScreenResolutionUpdateAck:
      if (_hvDevice->getPendingTransaction(kHyperVGraphicsRequestTransactionID, &responseBuffer, &responseLength)) {
        memcpy(responseBuffer, pktData, responseLength);
        _hvDevice->wakeTransaction(kHyperVGraphicsRequestTransactionID);
      }
      break;

    default:
      break;
  }
}

void HyperVGraphicsProvider::handleRefreshTimer(IOTimerEventSource *sender) {
  HyperVGraphicsMessage gfxMsg = { };
  
  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeDIRT;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (gfxMsg.dirt);

  gfxMsg.dirt.videoOutput = 0;
  gfxMsg.dirt.dirtCount = 1;
  gfxMsg.dirt.dirtRects[0].x1 = 0;
  gfxMsg.dirt.dirtRects[0].y1 = 0;
  gfxMsg.dirt.dirtRects[0].x2 = 1024;
  gfxMsg.dirt.dirtRects[0].y2 = 768;

  sendGraphicsMessage(&gfxMsg);
  _timerEventSource->setTimeoutMS(10);
}

IOReturn HyperVGraphicsProvider::sendGraphicsMessage(HyperVGraphicsMessage *gfxMessage, HyperVGraphicsMessage *gfxMessageResponse, UInt32 gfxMessageResponseSize) {
  gfxMessage->pipeHeader.type = kHyperVGraphicsPipeMessageTypeData;
  gfxMessage->pipeHeader.size = gfxMessage->gfxHeader.size;
  
  return _hvDevice->writeInbandPacketWithTransactionId(gfxMessage, gfxMessage->gfxHeader.size + sizeof (gfxMessage->pipeHeader),
                                                       kHyperVGraphicsRequestTransactionID, gfxMessageResponse != nullptr,
                                                       gfxMessageResponse, gfxMessageResponseSize);
}

IOReturn HyperVGraphicsProvider::connectGraphics() {
  bool foundVersion = false;
  IOReturn status;

  //
  // Negotiate graphics system version.
  //
  for (UInt32 i = 0; i < arrsize(graphicsVersions); i++) {
    status = negotiateVersion(graphicsVersions[i]);
    if (status == kIOReturnSuccess) {
      foundVersion = true;
      _currentGraphicsVersion = graphicsVersions[i];
      break;
    }
  }

  if (!foundVersion) {
    HVSYSLOG("Could not negotiate graphics version");
    return kIOReturnUnsupported;
  }
  HVDBGLOG("Using graphics version %u.%u", _currentGraphicsVersion.major, _currentGraphicsVersion.minor);

  status = allocateGraphicsMemory(_fbTotalLength);
  
  //
  // Wait for platform patcher.
  //
  if (!HyperVPlatformProvider::getInstance()->waitForPatcher()) {
    HVSYSLOG("Failed to locate platform patcher");
    return kIOReturnNotFound;
  }
  
  storeBootLogo();
  
  //
  // Send location to Hyper-V.
  //
  status = updateGraphicsMemoryLocation();
  if (status != kIOReturnSuccess) {
    return status;
  }
  
  updateScreenResolution();
  
  return kIOReturnSuccess;
}

IOReturn HyperVGraphicsProvider::allocateGraphicsMemory(IORangeScalar mmioLength) {
  HyperVModuleDevice *hvModuleDevice;
  IOReturn           status;

  //
  // Get HyperVModuleDevice instance used for allocating MMIO regions for Hyper-V.
  //
  OSDictionary *vmodMatching = IOService::serviceMatching("HyperVModuleDevice");
  if (vmodMatching == nullptr) {
    HVSYSLOG("Failed to create HyperVModuleDevice matching dictionary");
    return kIOReturnNotFound;
  }

  HVDBGLOG("Waiting for HyperVModuleDevice");
#if __MAC_OS_X_VERSION_MIN_REQUIRED < __MAC_10_6
  IOService *vmodService = IOService::waitForService(vmodMatching);
  if (vmodService != nullptr) {
    vmodService->retain();
  }
#else
  IOService *vmodService = waitForMatchingService(vmodMatching);
  vmodMatching->release();
#endif

  if (vmodService == nullptr) {
    HVSYSLOG("Failed to locate HyperVModuleDevice");
    return kIOReturnNotFound;
  }

  HVDBGLOG("Got instance of HyperVModuleDevice");
  hvModuleDevice = OSDynamicCast(HyperVModuleDevice, vmodService);

  //
  // Allocate new MMIO space for graphics memory.
  //
  _gfxMmioBase = hvModuleDevice->allocateRange(mmioLength, 0x100000, 0xFFFFFFFF);
  vmodService->release();

  if (_gfxMmioBase == 0) {
    HVSYSLOG("Failed to allocate graphics memory");
    return kIOReturnNoMemory;
  }

  _gfxMmioLength = mmioLength;
  HVDBGLOG("Allocated graphics memory at 0x%llX (0x%llX bytes)", _gfxMmioBase, _gfxMmioLength);
  return kIOReturnSuccess;
}

bool HyperVGraphicsProvider::storeBootLogo() {
  vc_info *consoleInfo;
  UInt8   *buffer;
  UInt32  logoX;
  UInt32  logoY;
  UInt8   *currentLine;

  //
  // Only store logo once.
  //
  if (_logoImageData != nullptr) {
    return true;
  }

  //
  // Get FB data address.
  //
  consoleInfo = HyperVPlatformProvider::getInstance()->getConsoleInfo();
  if (consoleInfo == nullptr) {
    return false;
  }

  buffer = (UInt8*) consoleInfo->v_baseaddr;
  logoX = (consoleInfo->v_width / 2) - (kBootLogoImageWidth / 2);
  logoY = (consoleInfo->v_height / 2) - (kBootLogoImageHeight / 2);

  HVDBGLOG("Got current framebuffer address at 0x%p, logo at %ux%u (%u bpp)", consoleInfo->v_baseaddr, logoX, logoY, consoleInfo->v_depth);

  //
  // Allocate logo buffer.
  //
  _logoImageSize = kBootLogoImageHeight * kBootLogoImageWidth * (consoleInfo->v_depth / 8);
  _logoImageData = (UInt8*) IOMalloc(_logoImageSize);
  if (_logoImageData == nullptr) {
    _logoImageSize = 0;
    return false;
  }
  _logoRowBytes = kBootLogoImageWidth * (consoleInfo->v_depth / 8);

  //
  // Capture logo from center of screen.
  //
  for (int line = logoY; line < (logoY + kBootLogoImageHeight); line++) {
    //
    // Capture logo line.
    //
    currentLine = &buffer[line * consoleInfo->v_rowbytes];
    memcpy(&_logoImageData[(line - logoY) * _logoRowBytes], &currentLine[logoX * (consoleInfo->v_depth / 8)], _logoRowBytes);
  }

  return true;
}

bool HyperVGraphicsProvider::drawBootLogo() {
  vc_info *consoleInfo;
  UInt8   *buffer;
  UInt32  logoX;
  UInt32  logoY;
  UInt8   *currentLine;

  if (_logoImageData == nullptr) {
    return true;
  }

  //
  // Get FB data buffer.
  //
  consoleInfo = HyperVPlatformProvider::getInstance()->getConsoleInfo();
  if (consoleInfo == nullptr) {
    return false;
  }

  buffer = (UInt8*) consoleInfo->v_baseaddr;
  logoX = (consoleInfo->v_width / 2) - (kBootLogoImageWidth / 2);
  logoY = (consoleInfo->v_height / 2) - (kBootLogoImageHeight / 2);

  //
  // Fill background color of screen using saved data.
  //
  for (int line = 0; line < consoleInfo->v_height; line++) {
    currentLine = &buffer[line * consoleInfo->v_rowbytes];
    for (int pixel = 0; pixel < consoleInfo->v_width; pixel++) {
      memcpy(&currentLine[pixel * (consoleInfo->v_depth / 8)], _logoImageData, (consoleInfo->v_depth / 8));
    }
  }

  //
  // Draw logo in center of screen.
  //
  for (int line = logoY; line < (logoY + kBootLogoImageHeight); line++) {
    //
    // Draw logo line.
    //
    currentLine = &buffer[line * consoleInfo->v_rowbytes];
    memcpy(&currentLine[logoX * (consoleInfo->v_depth / 8)], &_logoImageData[(line - logoY) * _logoRowBytes], _logoRowBytes);
  }
  
  return true;
}

IOReturn HyperVGraphicsProvider::negotiateVersion(VMBusVersion version) {
  IOReturn status;
  HyperVGraphicsMessage gfxMsg = { };

  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeVersionRequest;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (gfxMsg.versionRequest);
  gfxMsg.versionRequest.version = version;

  HVDBGLOG("Trying version %u.%u", version.major, version.minor);
  status = sendGraphicsMessage(&gfxMsg, &gfxMsg, sizeof (gfxMsg));
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send negotiate version with status 0x%X", status);
    return status;
  }

  HVDBGLOG("Version %u.%u accepted: 0x%X (actual version %u.%u) max video outputs: %u", version.major, version.minor,
           gfxMsg.versionResponse.accepted, gfxMsg.versionResponse.version.major,
           gfxMsg.versionResponse.version.minor, gfxMsg.versionResponse.maxVideoOutputs);
  return gfxMsg.versionResponse.accepted != 0 ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn HyperVGraphicsProvider::updateGraphicsMemoryLocation() {
  IOReturn status;
  HyperVGraphicsMessage gfxMsg = { };

  //
  // Send location of graphics memory (VRAM).
  //
  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeVRAMLocation;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (gfxMsg.vramLocation);
  gfxMsg.vramLocation.context = gfxMsg.vramLocation.vramGPA = _gfxMmioBase;
  gfxMsg.vramLocation.isVRAMGPASpecified = 1;

  status = sendGraphicsMessage(&gfxMsg, &gfxMsg, sizeof (gfxMsg));
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send graphics memory location with status 0x%X", status);
    return status;
  }
  if (gfxMsg.vramAck.context != _gfxMmioBase) {
    HVSYSLOG("Returned context 0x%llX is incorrect, should be 0x%llX", gfxMsg.vramAck.context, _gfxMmioBase);
    return kIOReturnIOError;
  }

  HVDBGLOG("Sent graphics memory location 0x%llX to Hyper-V", _gfxMmioBase);
  return kIOReturnSuccess;
}

IOReturn HyperVGraphicsProvider::updateScreenResolution() {
  IOReturn status;
  HyperVGraphicsMessage gfxMsg = { };

  //
  // Send screen resolution and pixel depth information.
  //
  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeScreenResolutionUpdate;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (gfxMsg.screenResolutionUpdate);

  gfxMsg.screenResolutionUpdate.context = 0;
  gfxMsg.screenResolutionUpdate.videoOutputCount = 1;
  gfxMsg.screenResolutionUpdate.videoOutputs[0].active = 1;
  gfxMsg.screenResolutionUpdate.videoOutputs[0].vramOffset = 0;
  gfxMsg.screenResolutionUpdate.videoOutputs[0].depth = 32;
  gfxMsg.screenResolutionUpdate.videoOutputs[0].width = 640;
  gfxMsg.screenResolutionUpdate.videoOutputs[0].height = 480;
  gfxMsg.screenResolutionUpdate.videoOutputs[0].pitch = 2560;

  status = sendGraphicsMessage(&gfxMsg, &gfxMsg, sizeof (gfxMsg));
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send graphics memory location with status 0x%X", status);
    return status;
  }
  
  PE_Video consoleInfo;
  getPlatform()->getConsoleInfo(&consoleInfo);
  
  consoleInfo.v_offset = 0;
  consoleInfo.v_baseAddr = _gfxMmioBase | 1;
  
  getPlatform()->setConsoleInfo(0, kPEDisableScreen);
  getPlatform()->setConsoleInfo(&consoleInfo, kPEBaseAddressChange);

  getPlatform()->getConsoleInfo(&consoleInfo);
  consoleInfo.v_height = 480;
  consoleInfo.v_width = 640;
  consoleInfo.v_rowBytes = 2560;
  getPlatform()->setConsoleInfo(&consoleInfo, kPEEnableScreen);

  
  drawBootLogo();
  
  HyperVPlatformProvider::getInstance()->resetProgressBar();
  
  bzero(&gfxMsg, sizeof (gfxMsg));

  gfxMsg.gfxHeader.type = kHyperVGraphicsMessageTypeDIRT;
  gfxMsg.gfxHeader.size = sizeof (gfxMsg.gfxHeader) + sizeof (gfxMsg.dirt);

  gfxMsg.dirt.videoOutput = 0;
  gfxMsg.dirt.dirtCount = 1;
  gfxMsg.dirt.dirtRects[0].x1 = 0;
  gfxMsg.dirt.dirtRects[0].y1 = 0;
  gfxMsg.dirt.dirtRects[0].x2 = 1024;
  gfxMsg.dirt.dirtRects[0].y2 = 768;

  status = sendGraphicsMessage(&gfxMsg);
  if (status != kIOReturnSuccess) {
    HVSYSLOG("Failed to send graphics memory location with status 0x%X", status);
    return status;
  }
  HVDBGLOG("Sent screen resolution to Hyper-V");
  return kIOReturnSuccess;
}
