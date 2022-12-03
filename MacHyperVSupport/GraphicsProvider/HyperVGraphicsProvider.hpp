//
//  HyperVGraphicsProvider.hpp
//  Hyper-V synthetic graphics provider
//
//  Copyright © 2021-2022 Goldfish64. All rights reserved.
//

#ifndef HyperVGraphicsProvider_hpp
#define HyperVGraphicsProvider_hpp

#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOService.h>

#include "HyperVVMBusDevice.hpp"
#include "HyperVGraphicsProviderRegs.hpp"

class HyperVGraphicsProvider : public IOService {
  OSDeclareDefaultStructors(HyperVGraphicsProvider);
  HVDeclareLogFunctionsVMBusChild("gfxp");
  typedef IOService super;

private:
  HyperVVMBusDevice  *_hvDevice              = nullptr;
  VMBusVersion       _currentGraphicsVersion = { };
  IOTimerEventSource *_timerEventSource = nullptr;
  IORangeScalar     _gfxMmioBase = 0;
  IORangeScalar     _gfxMmioLength = 0;

  IORangeScalar     _fbBaseAddress   = 0;
  IORangeScalar     _fbTotalLength   = 0;
  IORangeScalar     _fbInitialLength = 0;
  
  UInt8             *_logoImageData      = nullptr;
  size_t            _logoImageSize       = 0;
  size_t            _logoRowBytes        = 0;

  void handlePacket(VMBusPacketHeader *pktHeader, UInt32 pktHeaderLength, UInt8 *pktData, UInt32 pktDataLength);
  void handleRefreshTimer(IOTimerEventSource *sender);
  IOReturn sendGraphicsMessage(HyperVGraphicsMessage *gfxMessage, HyperVGraphicsMessage *gfxMessageResponse = nullptr, UInt32 gfxMessageResponseSize = 0);
  
  IOReturn connectGraphics();
  IOReturn allocateGraphicsMemory(IORangeScalar mmioLength);
  bool storeBootLogo();
  bool drawBootLogo();
  
  IOReturn negotiateVersion(VMBusVersion version);
  IOReturn updateGraphicsMemoryLocation();
  IOReturn updateScreenResolution();

public:
  //
  // IOService overrides.
  //
  bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

  void getFramebufferArea(IORangeScalar *baseAddress, IORangeScalar *length);
};

#endif
