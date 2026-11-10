//===-- XtensaLiteralPool.cpp - rewrite pool loads via a base register ---===//
//
//                     The LLVM Compiler Infrastructure
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// xuansi A3 surgery: every l32r that reads the literal pool is rewritten
// into the pseudo LPOOL_L32 (base register + constant pool index).  A
// single LPOOL_BASE pseudo is inserted at the top of the entry block of
// each function that references the pool; it materialises the pool base
// from the exec_env -> module instance chain (see the constants in
// XtensaSubtarget.h).  XtensaAsmPrinter expands both pseudos once the
// module-wide pool layout fixes the byte offsets.
//
// The base lives in a virtual register, so the register allocator picks
// a call-preserved register when the value spans calls, spills it under
// pressure and rematerialises nothing -- the same treatment any other
// long-lived value gets, without shrinking the allocatable set and
// without adding any assumption beyond the backend's own windowed-ABI
// callee-saved model.
//
//===----------------------------------------------------------------------===//

#include "Xtensa.h"
#include "XtensaInstrInfo.h"
#include "XtensaSubtarget.h"

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Register.h"

using namespace llvm;

#define DEBUG_TYPE "xtensa-literal-pool"

namespace {

class XtensaLiteralPoolPass : public MachineFunctionPass {
public:
  static char ID;
  XtensaLiteralPoolPass() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "Xtensa literal pool base rewrite pass";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // end anonymous namespace

char XtensaLiteralPoolPass::ID = 0;

bool XtensaLiteralPoolPass::runOnMachineFunction(MachineFunction &MF) {
  const XtensaSubtarget &STI = MF.getSubtarget<XtensaSubtarget>();
  if (!STI.useLiteralPoolBase())
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  const XtensaInstrInfo &TII = *STI.getInstrInfo();

  Register BaseReg;
  bool Changed = false;

  for (MachineBasicBlock &MBB : MF) {
    for (auto MI = MBB.begin(), ME = MBB.end(); MI != ME; /* below */) {
      MachineInstr &Ins = *MI;
      ++MI;
      if (Ins.getOpcode() != Xtensa::L32R || !Ins.getOperand(1).isCPI())
        continue;

      if (!BaseReg.isValid()) {
        // First pool reference in this function: materialise the pool
        // base at the very top of the entry block.  The expansion reads
        // a2 (the exec_env argument register), which still holds its
        // incoming value there: nothing in the entry block before the
        // first real instruction can clobber an incoming argument.
        BaseReg = MRI.createVirtualRegister(&Xtensa::ARRegClass);
        MachineBasicBlock &Entry = MF.front();
        // Mark a2 (exec_env, first argument register) as used so the
        // pseudo keeps the incoming argument live until its expansion.
        BuildMI(Entry, Entry.getFirstNonPHI(), DebugLoc(),
                TII.get(Xtensa::LPOOL_BASE), BaseReg)
            .addReg(Xtensa::A2, RegState::Implicit);
      }

      Register Dst = Ins.getOperand(0).getReg();
      BuildMI(MBB, Ins, Ins.getDebugLoc(), TII.get(Xtensa::LPOOL_L32), Dst)
          .addReg(BaseReg)
          .addConstantPoolIndex(Ins.getOperand(1).getIndex(),
                                Ins.getOperand(1).getOffset());
      Ins.eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}

FunctionPass *llvm::createXtensaLiteralPoolPass() {
  return new XtensaLiteralPoolPass();
}
