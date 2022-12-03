//
//  HyperVPlatformProvider.hpp
//  Hyper-V platform functions provider
//
//  Copyright © 2021-2022 Goldfish64. All rights reserved.
//

#ifndef HyperVPlatformProvider_hpp
#define HyperVPlatformProvider_hpp

#include <IOKit/IOService.h>
#include <Headers/kern_patcher.hpp>

#include "HyperV.hpp"

//
// Console info structure, taken from osfmk/console/video_console.h
// Last updated from XNU 4570.1.46.
//
struct vc_info {
  unsigned int   v_height;        /* pixels */
  unsigned int   v_width;         /* pixels */
  unsigned int   v_depth;
  unsigned int   v_rowbytes;
  unsigned long  v_baseaddr;
  unsigned int   v_type;
  char           v_name[32];
  uint64_t       v_physaddr;
  unsigned int   v_rows;          /* characters */
  unsigned int   v_columns;       /* characters */
  unsigned int   v_rowscanbytes;  /* Actualy number of bytes used for display per row*/
  unsigned int   v_scale;
  unsigned int   v_rotate;
  unsigned int   v_reserved[3];
};

class EXPORT PRODUCT_NAME : public IOService {
  OSDeclareDefaultStructors(PRODUCT_NAME)
public:
  IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
  bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};

extern PRODUCT_NAME *ADDPR(selfInstance);

class HyperVPlatformProvider {
  HVDeclareLogFunctionsNonIOKit("prov", "HyperVPlatformProvider");

private:
  //
  // Global instance.
  //
  static HyperVPlatformProvider *_instance;
  bool                          _patcherLoaded = false;

  //
  // IOPlatformExpert::setConsoleInfo wrapping
  //
  mach_vm_address_t _setConsoleInfoAddr = 0;
  UInt64            _setConsoleInfoOrg[2] {};
  static IOReturn wrapSetConsoleInfo(IOPlatformExpert *that, PE_Video *consoleInfo, unsigned int op);

  //
  // vc_progress_set on 10.10 and newer.
  //
  using vcProgressSet = void (*)(boolean_t enable, uint32_t vc_delay);
  vcProgressSet _vcProgressSetOrg = nullptr;
  vc_info *_consoleInfo = nullptr;

  //
  // Initialization function.
  //
  void init();
  void onLiluPatcherLoad(KernelPatcher &patcher);

public:
  //
  // Instance creator.
  //
  static HyperVPlatformProvider *getInstance() {
    if (_instance == nullptr) {
      _instance = new HyperVPlatformProvider;
      if (_instance != nullptr) {
        _instance->init();
      }
    }

    return _instance;
  }

  bool isPatcherLoaded() { return _patcherLoaded; }
  bool waitForPatcher();
  vc_info *getConsoleInfo() { return _consoleInfo; }
  void resetProgressBar();
};

#endif
