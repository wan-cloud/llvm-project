//===- XtensaSubtarget.cpp - Xtensa Subtarget Information -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Xtensa specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#include "XtensaSubtarget.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "xtensa-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "XtensaGenSubtargetInfo.inc"

using namespace llvm;

static cl::opt<bool> TextSectionLiterals("mtext-section-literals",
                                         cl::init(false), cl::Hidden);

// xuansi A3 surgery: address the separate literal pool through a base
// register (l32i base+offset) instead of l32r, so that no executable
// window is needed to read constants.  Defaults on for this fork; pass
// -mllvm -xuansi-literal-base=0 to get the stock l32r codegen back
// (A/B comparisons in one toolchain).
static cl::opt<bool> LiteralPoolBase("xuansi-literal-base",
                                     cl::init(true), cl::Hidden);

bool XtensaSubtarget::useTextSectionLiterals() const
{
  // If code model is large then always place literals in
  // test section.
  if (TLInfo.getTargetMachine().getCodeModel() == CodeModel::Large)
    return true;

  return TextSectionLiterals;
}

bool XtensaSubtarget::useLiteralPoolBase() const {
  return LiteralPoolBase && !useTextSectionLiterals();
}


XtensaSubtarget &
XtensaSubtarget::initializeSubtargetDependencies(StringRef CPU, StringRef FS) {
  StringRef CPUName = CPU;
  if (CPUName.empty()) {
    // set default cpu name
    CPUName = "esp32";
  }

  HasDensity = false;
  HasSingleFloat = false;
  HasWindowed = false;
  HasBoolean = false;
  HasLoop = false;
  HasSEXT = false;
  HasCLAMPS = false;
  HasNSA = false;
  HasMINMAX = false;
  HasMul16 = false;
  HasMul32 = false;
  HasMul32High = false;
  HasDiv32 = false;
  HasMAC16 = false;
  HasDFPAccel = false;
  HasS32C1I = false;
  HasTHREADPTR = false;
  HasExtendedL32R = false;
  HasATOMCTL = false;
  HasMEMCTL = false;
  HasDebug = false;
  HasException = false;
  HasHighPriInterrupts = false;
  HasCoprocessor = false;
  HasInterrupt = false;
  HasRelocatableVector = false;
  HasTimerInt = false;
  HasPRID = false;
  HasRegionProtection = false;
  HasMiscSR = false;
  HasESP32S2Ops = false;
  HasESP32S3Ops = false;
  HasHIFI3 = false;
  HasForcedAtomics = false;
  HasAtomicLdSt = false;

  // Parse features string.
  ParseSubtargetFeatures(CPUName, CPUName, FS);
  return *this;
}

XtensaSubtarget::XtensaSubtarget(const Triple &TT, const std::string &CPU,
                                 const std::string &FS, const TargetMachine &TM)
    : XtensaGenSubtargetInfo(TT, CPU, /*TuneCPU*/ CPU, FS), TargetTriple(TT),
      InstrInfo(initializeSubtargetDependencies(CPU, FS)), TLInfo(TM, *this),
      TSInfo(), FrameLowering() {}
