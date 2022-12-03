//
//  HyperVPlatformProvider.cpp
//  Hyper-V platform functions provider
//
//  Copyright © 2021-2022 Goldfish64. All rights reserved.
//

#include "HyperVPlatformProvider.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_version.hpp>
#include <Headers/plugin_start.hpp>

#include <IOKit/IOPlatformExpert.h>

OSDefineMetaClassAndStructors(PRODUCT_NAME, IOService)

HyperVPlatformProvider *HyperVPlatformProvider::_instance = nullptr;
PRODUCT_NAME *ADDPR(selfInstance)                         = nullptr;

IOService *PRODUCT_NAME::probe(IOService *provider, SInt32 *score) {
  ADDPR(selfInstance) = this;
  setProperty("VersionInfo", kextVersion);
  auto service = IOService::probe(provider, score);
  return ADDPR(startSuccess) ? service : nullptr;
}

bool PRODUCT_NAME::start(IOService *provider) {
  ADDPR(selfInstance) = this;
  if (!IOService::start(provider)) {
    SYSLOG("init", "failed to start the parent");
    return false;
  }

  if (ADDPR(startSuccess) && HyperVPlatformProvider::getInstance()->isPatcherLoaded()) {
    registerService();
  }
  return ADDPR(startSuccess);
}

void PRODUCT_NAME::stop(IOService *provider) {
  ADDPR(selfInstance) = nullptr;
  IOService::stop(provider);
}

void HyperVPlatformProvider::init() {
  HVCheckDebugArgs();
  HVDBGLOG("Initializing provider");

  //
  // Lilu is used for function hooking/patching, register patcher callback.
  //
  _patcherLoaded = false;
  lilu.onPatcherLoadForce([](void *user, KernelPatcher &patcher) {
    static_cast<HyperVPlatformProvider *>(user)->onLiluPatcherLoad(patcher);
  }, this);

  //
  // Patch setConsoleInfo to call our wrapper function instead.
  // 10.6 to 10.12 may pass garbage data to setConsoleInfo from IOPCIConfigurator::configure().
  //
 /* KernelVersion kernelVersion = getKernelVersion();
  if (kernelVersion >= KernelVersion::SnowLeopard && kernelVersion <= KernelVersion::Sierra) {
    _setConsoleInfoAddr = OSMemberFunctionCast(mach_vm_address_t, IOService::getPlatform(), &IOPlatformExpert::setConsoleInfo);

    // Save start of function.
    lilu_os_memcpy(_setConsoleInfoOrg, (void *) _setConsoleInfoAddr, sizeof (_setConsoleInfoOrg));

    // Patch to call wrapper.
#if defined(__i386__)
    uint64_t patched[2] {0x25FF | ((_setConsoleInfoAddr + 8) << 16), (UInt32) wrapSetConsoleInfo};
#elif defined(__x86_64__)
    uint64_t patched[2] {0x0225FF, (uintptr_t)wrapSetConsoleInfo};
#else
#error Unsupported arch
#endif
    if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
      lilu_os_memcpy((void *) _setConsoleInfoAddr, patched, sizeof (patched));
      MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    }

    HVDBGLOG("Patched IOPlatformExpert::setConsoleInfo");
  }*/
}

IOReturn HyperVPlatformProvider::wrapSetConsoleInfo(IOPlatformExpert *that, PE_Video *consoleInfo, unsigned int op) {
  _instance->HVDBGLOG("op %X", op);

  // Fix arg here
  if (op == kPEBaseAddressChange && consoleInfo != nullptr) {
    PE_Video consoleInfoCurrent;
    IOService::getPlatform()->getConsoleInfo(&consoleInfoCurrent);

    unsigned long baseAddr = consoleInfo->v_baseAddr;
    memcpy(consoleInfo, &consoleInfoCurrent, sizeof (*consoleInfo));
    consoleInfo->v_baseAddr = baseAddr;
  }

  // Restore original function.
  if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
    lilu_os_memcpy((void *) _instance->_setConsoleInfoAddr, _instance->_setConsoleInfoOrg, sizeof (_instance->_setConsoleInfoOrg));
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
  }

  IOReturn result = FunctionCast(wrapSetConsoleInfo, _instance->_setConsoleInfoAddr)(that, consoleInfo, op);

  // Patch again if kPEBaseAddressChange was not the operation.
  if (op != kPEBaseAddressChange) {
#if defined(__i386__)
    UInt64 patched[2] {0x25FF | ((_instance->_setConsoleInfoAddr + 8) << 16), (UInt32) wrapSetConsoleInfo};
#elif defined(__x86_64__)
    UInt64 patched[2] {0x0225FF, (uintptr_t)wrapSetConsoleInfo};
#else
#error Unsupported arch
#endif
    if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
      lilu_os_memcpy((void *) _instance->_setConsoleInfoAddr, patched, sizeof (patched));
      MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
    }
  } else {
    _instance->HVDBGLOG("kPEBaseAddressChange specified, not patching again");
  }

  return result;
}

void HyperVPlatformProvider::onLiluPatcherLoad(KernelPatcher &patcher) {
  HVDBGLOG("Patcher loaded");

  //
  // Get _vc_progress_set on 10.10 and newer.
  //
  if (getKernelVersion() >= KernelVersion::Yosemite) {
    _vcProgressSetOrg = reinterpret_cast<vcProgressSet>(patcher.solveSymbol(KernelPatcher::KernelID, "_vc_progress_set"));
  }

  _consoleInfo = reinterpret_cast<vc_info*>(patcher.solveSymbol(KernelPatcher::KernelID, "_vinfo"));
  
  //
  // Register resource class which will notify anyone waiting for the patcher.
  //
  _patcherLoaded = true;
  if (ADDPR(selfInstance) != nullptr) {
    HVDBGLOG("Registering %s service", xStringify(PRODUCT_NAME));
    ADDPR(selfInstance)->registerService();
  }
}

bool HyperVPlatformProvider::waitForPatcher() {
  //
  // Wait for resource class.
  //
  OSDictionary *hvMatching = IOService::serviceMatching(xStringify(PRODUCT_NAME));
  if (hvMatching == nullptr) {
    HVSYSLOG("Failed to create %s matching dictionary", xStringify(PRODUCT_NAME));
    return false;
  }

  HVDBGLOG("Waiting for %s resource", xStringify(PRODUCT_NAME));
#if __MAC_OS_X_VERSION_MIN_REQUIRED < __MAC_10_6
  IOService *hvService = IOService::waitForService(hvMatching);
  if (hvService != nullptr) {
    hvService->retain();
  }
#else
  IOService *hvService = IOService::waitForMatchingService(hvMatching);
  hvMatching->release();
#endif

  if (hvService == nullptr) {
    HVSYSLOG("Failed to locate %s", xStringify(PRODUCT_NAME));
    return false;
  }
  hvService->release();

  HVDBGLOG("Got instance of %s resource", xStringify(PRODUCT_NAME));
  return true;
}

void HyperVPlatformProvider::resetProgressBar() {
  if (_vcProgressSetOrg != nullptr) {
    _vcProgressSetOrg(FALSE, 0);
    _vcProgressSetOrg(TRUE, 0);
    HVDBGLOG("Reset progress bar on 10.10+");
  }
}
