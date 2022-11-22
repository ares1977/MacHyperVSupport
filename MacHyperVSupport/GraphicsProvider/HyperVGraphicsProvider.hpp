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
  HyperVVMBusDevice *_hvDevice              = nullptr;
  VMBusVersion      _currentGraphicsVersion = { };

  IORangeScalar     _fbBaseAddress   = 0;
  IORangeScalar     _fbTotalLength   = 0;
  IORangeScalar     _fbInitialLength = 0;

  void handlePacket(VMBusPacketHeader *pktHeader, UInt32 pktHeaderLength, UInt8 *pktData, UInt32 pktDataLength);
  IOReturn sendGraphicsMessage(HyperVGraphicsMessage *gfxMessage, HyperVGraphicsMessage *gfxMessageResponse = nullptr, UInt32 gfxMessageResponseSize = 0);
  IOReturn negotiateVersion(VMBusVersion version);
  IOReturn connectGraphics();

public:
  //
  // IOService overrides.
  //
  bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

  void getFramebufferArea(IORangeScalar *baseAddress, IORangeScalar *totalLength, IORangeScalar *initialLength);
};

#endif
