//===- XtensaAsmPrinter.cpp Xtensa LLVM Assembly Printer ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to GAS-format Xtensa assembly language.
//
//===----------------------------------------------------------------------===//

#include "XtensaAsmPrinter.h"
#include "MCTargetDesc/XtensaInstPrinter.h"
#include "XtensaConstantPoolValue.h"
#include "XtensaMCInstLower.h"
#include "XtensaSubtarget.h"
#include "TargetInfo/XtensaTargetInfo.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/MC/TargetRegistry.h"

using namespace llvm;

static MCSymbolRefExpr::VariantKind
getModifierVariantKind(XtensaCP::XtensaCPModifier Modifier) {
  switch (Modifier) {
  case XtensaCP::no_modifier:
    return MCSymbolRefExpr::VK_None;
  case XtensaCP::TPOFF:
    return MCSymbolRefExpr::VK_TPOFF;
  }
  llvm_unreachable("Invalid XtensaCPModifier!");
}

void XtensaAsmPrinter::emitInstruction(const MachineInstr *MI) {
  XtensaMCInstLower Lower(MF->getContext(), *this);
  MCInst LoweredMI;
  unsigned Opc = MI->getOpcode();
  const MachineConstantPool *MCP = MF->getConstantPool();
  const XtensaSubtarget *Subtarget = &MF->getSubtarget<XtensaSubtarget>();

  // If we just ended a constant pool, mark it as such.
  if (InConstantPool && Opc != Xtensa::CONSTPOOL_ENTRY) {
    OutStreamer->emitDataRegion(MCDR_DataRegionEnd);
    InConstantPool = false;
  }

  // xuansi A3 surgery: expand the pool-base pseudos once the module pool
  // layout has fixed the byte offsets (see emitConstantPool).
  if (Subtarget->useLiteralPoolBase()) {
    if (Opc == Xtensa::LPOOL_BASE) {
      emitLiteralBaseChain(MI->getOperand(0).getReg());
      return;
    }
    if (Opc == Xtensa::LPOOL_L32) {
      const MachineOperand &CP = MI->getOperand(2);
      assert(CP.isCPI() && "LPOOL_L32 without a constant pool index");
      auto It = CurPoolOffsets.find(CP.getIndex());
      if (It == CurPoolOffsets.end())
        report_fatal_error("LPOOL_L32 references a constant pool entry "
                           "outside this function's pool: " +
                           MF->getName());
      // The pool offset operand can carry a positive addend (index into a
      // wider pool entry); CP offsets are negative in LLVM convention, so
      // negate before folding in.
      int64_t Addend = CP.getOffset();
      uint64_t Off = CurPoolStart + It->second;
      if (Addend > 0)
        Off += Addend;
      else if (Addend < 0)
        report_fatal_error("negative constant pool addend in " + MF->getName());
      if (Off > 32512 + 252)
        report_fatal_error("xuansi literal pool offset " + Twine(Off) +
                           " out of addmi/l32i range in " + MF->getName());
      emitPoolLoad(MI->getOperand(0).getReg(), MI->getOperand(1).getReg(),
                   (unsigned)Off);
      return;
    }
    // Guard: with the literal base mode on, no l32r-to-pool may survive
    // to emission.  l32r pool loads created after register allocation
    // (bool-mask fixups, large frame prologues, far-branch relaxation)
    // land here: refuse loudly rather than emit an l32r behind the
    // non-executable pool.
    if (Opc == Xtensa::L32R && MI->getOperand(1).isCPI())
      report_fatal_error("l32r pool load survived the xuansi literal-base "
                         "rewrite in " +
                         MF->getName() +
                         " (post-RA creator; needs base-register expansion)");
  }

  if (Opc == Xtensa::CONSTPOOL_ENTRY) {
    // CONSTPOOL_ENTRY - This instruction represents a floating
    // constant pool in the function.  The first operand is the ID#
    // for this instruction, the second is the index into the
    // MachineConstantPool that this is, the third is the size in
    // bytes of this constant pool entry.
    // The required alignment is specified on the basic block holding this MI.
    //
    unsigned LabelId = (unsigned)MI->getOperand(0).getImm();
    unsigned CPIdx = (unsigned)MI->getOperand(1).getIndex();

    // If this is the first entry of the pool, mark it.
    if (!InConstantPool) {
      if (OutStreamer->hasRawTextSupport()) {
        OutStreamer->emitRawText(StringRef("\t.literal_position\n"));
      }
      OutStreamer->emitDataRegion(MCDR_DataRegion);
      InConstantPool = true;
    }
    const MachineConstantPoolEntry &MCPE = MCP->getConstants()[CPIdx];

    emitMachineConstantPoolEntry(MCPE, LabelId);
    return;
  }

  switch (Opc) {
  case Xtensa::BR_JT: {
    EmitToStreamer(
        *OutStreamer,
        MCInstBuilder(Xtensa::JX).addReg(MI->getOperand(0).getReg()));
    return;
  }
  case Xtensa::LOOPEND:
    return;
  }
  Lower.lower(MI, LoweredMI);
  EmitToStreamer(*OutStreamer, LoweredMI);
}

// xuansi A3 surgery helpers: expand the pool pseudos into real l32i/addmi
// MC instructions with baked byte offsets.

/// Materialise the literal pool base: a2 is the exec_env pointer (first
/// argument of every WAMR AOT function); load module_inst, then the pool
/// base from its dedicated slot.
void XtensaAsmPrinter::emitLiteralBaseChain(unsigned Dst) {
  EmitToStreamer(*OutStreamer,
                 MCInstBuilder(Xtensa::L32I)
                     .addReg(Dst)
                     .addReg(Xtensa::A2)
                     .addImm(XtensaSubtarget::XUANSI_EXEC_ENV_MODULE_INST_OFFSET));
  EmitToStreamer(*OutStreamer,
                 MCInstBuilder(Xtensa::L32I)
                     .addReg(Dst)
                     .addReg(Dst)
                     .addImm(XtensaSubtarget::XUANSI_LITERAL_BASE_SLOT));
}

/// Load one pool word at byte offset Off from the base register.  The
/// l32i immediate addresses bytes 0..1020; larger offsets use addmi
/// (imm8 << 8, so 256-aligned) plus a low-byte l32i.  Dst doubles as the
/// addmi scratch so no extra register is needed.
void XtensaAsmPrinter::emitPoolLoad(unsigned Dst, unsigned Base,
                                    unsigned Off) {
  if (Off <= 1020) {
    EmitToStreamer(*OutStreamer, MCInstBuilder(Xtensa::L32I)
                                     .addReg(Dst)
                                     .addReg(Base)
                                     .addImm(Off));
    return;
  }
  assert((Off & 3) == 0 && Off <= 32512 + 252 && "offset range checked by caller");
  EmitToStreamer(*OutStreamer, MCInstBuilder(Xtensa::ADDMI)
                                   .addReg(Dst)
                                   .addReg(Base)
                                   .addImm(Off & ~0xFFu));
  EmitToStreamer(*OutStreamer, MCInstBuilder(Xtensa::L32I)
                                   .addReg(Dst)
                                   .addReg(Dst)
                                   .addImm(Off & 0xFFu));
}

void XtensaAsmPrinter::emitMachineConstantPoolEntry(
    const MachineConstantPoolEntry &CPE, int i) {
  if (CPE.isMachineConstantPoolEntry()) {
    XtensaConstantPoolValue *ACPV =
        static_cast<XtensaConstantPoolValue *>(CPE.Val.MachineCPVal);
    ACPV->setLabelId(i);
    emitMachineConstantPoolValue(CPE.Val.MachineCPVal);
  } else {
    MCSymbol *LblSym = GetCPISymbol(i);
    // TODO find a better way to check whether we emit data to .s file
    if (OutStreamer->hasRawTextSupport()) {
      std::string str("\t.literal ");
      str += LblSym->getName();
      str += ", ";
      const Constant *C = CPE.Val.ConstVal;

      Type *Ty = C->getType();
      if (const auto *CFP = dyn_cast<ConstantFP>(C)) {
        str += toString(CFP->getValueAPF().bitcastToAPInt(), 10, true);
      } else if (const auto *CI = dyn_cast<ConstantInt>(C)) {
        str += toString(CI->getValue(), 10, true);
      } else if (isa<PointerType>(Ty)) {
        const MCExpr *ME = lowerConstant(C);
        const MCSymbolRefExpr &SRE = cast<MCSymbolRefExpr>(*ME);
        const MCSymbol &Sym = SRE.getSymbol();
        str += Sym.getName();
      } else {
        llvm_unreachable("unexpected constant pool entry type");
      }

      OutStreamer->emitRawText(StringRef(str));
    } else {
      OutStreamer->emitCodeAlignment(
          Align(4), OutStreamer->getContext().getSubtargetInfo());
      OutStreamer->emitLabel(LblSym);
      emitGlobalConstant(getDataLayout(), CPE.Val.ConstVal);
    }
  }
}

/// EmitConstantPool - Print to the current output stream assembly
/// representations of the constants in the constant pool MCP. This is
/// used to print out constants which have been "spilled to memory" by
/// the code generator.
void XtensaAsmPrinter::emitConstantPool() {
  const Function &F = MF->getFunction();
  const MachineConstantPool *MCP = MF->getConstantPool();
  const std::vector<MachineConstantPoolEntry> &CP = MCP->getConstants();
  const XtensaSubtarget *Subtarget = &MF->getSubtarget<XtensaSubtarget>();

  if (Subtarget->useTextSectionLiterals())
    return;

  if (CP.empty())
    return;

  if (Subtarget->useLiteralPoolBase()) {
    // xuansi A3 surgery: record this function's slice of the module pool
    // before emitting it.  Entries are emitted in index order, each one
    // 4-byte aligned, exactly as the loop below does, so the running
    // offset of entry i is the sum of align4(size_j) for j < i.
    // LPOOL_L32 expansion during the function body reads this back.
    CurPoolOffsets.clear();
    CurPoolStart = ModulePoolBytes;
    unsigned Running = 0;
    for (unsigned i = 0, e = CP.size(); i != e; ++i) {
      Running = (Running + 3u) & ~3u;
      CurPoolOffsets[i] = Running;
      Running += CP[i].getSizeInBytes(getDataLayout());
    }
    ModulePoolBytes += Running;
  }

  for (unsigned i = 0, e = CP.size(); i != e; ++i) {
    const MachineConstantPoolEntry &CPE = CP[i];

    if (i == 0) {
      if (OutStreamer->hasRawTextSupport()) {
        OutStreamer->switchSection(
            getObjFileLowering().SectionForGlobal(&F, TM));
        OutStreamer->emitRawText(StringRef("\t.literal_position\n"));
      } else {
        MCSectionELF *CS =
            (MCSectionELF *)getObjFileLowering().SectionForGlobal(&F, TM);
        std::string CSectionName = CS->getName().str();
        std::string SectionName;
        std::size_t Pos = CSectionName.find(".text");
        if (Pos != std::string::npos) {
          if (Pos > 0)
            SectionName = CSectionName.substr(0, Pos + 5);
          else
            SectionName = "";
          SectionName += ".literal";
          SectionName += CSectionName.substr(Pos + 5);
        } else {
          SectionName = CSectionName;
          SectionName += ".literal";
        }

        MCSectionELF *S = OutContext.getELFSection(SectionName,
                                                   ELF::SHT_PROGBITS,
                                                   ELF::SHF_ALLOC);
        S->setAlignment(Align(4));
        OutStreamer->switchSection(S);
        if (Subtarget->useLiteralPoolBase() && SectionName != ".literal") {
          // The literal-base mode bakes offsets against one module-wide
          // pool; function-specific pool sections (text hot/cold
          // splitting) would each restart at offset zero.
          report_fatal_error("xuansi-literal-base requires a single pool "
                             "section, got: " +
                             Twine(SectionName));
        }
      }
    }

    emitMachineConstantPoolEntry(CPE, i);
  }
}

void XtensaAsmPrinter::emitMachineConstantPoolValue(
    MachineConstantPoolValue *MCPV) {
  XtensaConstantPoolValue *ACPV = static_cast<XtensaConstantPoolValue *>(MCPV);

  MCSymbol *MCSym;
  if (ACPV->isBlockAddress()) {
    const BlockAddress *BA =
        cast<XtensaConstantPoolConstant>(ACPV)->getBlockAddress();
    MCSym = GetBlockAddressSymbol(BA);
  } else if (ACPV->isGlobalValue()) {
    const GlobalValue *GV = cast<XtensaConstantPoolConstant>(ACPV)->getGV();
    // TODO some modifiers
    MCSym = getSymbol(GV);
  } else if (ACPV->isMachineBasicBlock()) {
    const MachineBasicBlock *MBB = cast<XtensaConstantPoolMBB>(ACPV)->getMBB();
    MCSym = MBB->getSymbol();
  } else if (ACPV->isJumpTable()) {
    unsigned idx = cast<XtensaConstantPoolJumpTable>(ACPV)->getIndex();
    MCSym = this->GetJTISymbol(idx, false);
  } else {
    assert(ACPV->isExtSymbol() && "unrecognized constant pool value");
    XtensaConstantPoolSymbol *XtensaSym = cast<XtensaConstantPoolSymbol>(ACPV);
    const char *Sym = XtensaSym->getSymbol();
    // TODO it's a trick to distinguish static references and generated rodata
    // references Some clear method required
    {
      std::string SymName(Sym);
      if (XtensaSym->isPrivateLinkage())
        SymName = ".L" + SymName;
      MCSym = GetExternalSymbolSymbol(StringRef(SymName));
    }
  }

  MCSymbol *LblSym = GetCPISymbol(ACPV->getLabelId());
  // TODO find a better way to check whether we emit data to .s file
  if (OutStreamer->hasRawTextSupport()) {
    std::string SymName("\t.literal ");
    SymName += LblSym->getName();
    SymName += ", ";
    SymName += MCSym->getName();

    StringRef Modifier = ACPV->getModifierText();
    SymName += Modifier;

    OutStreamer->emitRawText(StringRef(SymName));
  } else {
    MCSymbolRefExpr::VariantKind VK =
        getModifierVariantKind(ACPV->getModifier());

    if (ACPV->getModifier() != XtensaCP::no_modifier) {
      std::string SymName(MCSym->getName());
      MCSym = GetExternalSymbolSymbol(StringRef(SymName));
    }

    const MCExpr *Expr = MCSymbolRefExpr::create(MCSym, VK, OutContext);
    uint64_t Size = getDataLayout().getTypeAllocSize(ACPV->getType());
    OutStreamer->emitCodeAlignment(
        Align(4), OutStreamer->getContext().getSubtargetInfo());
    OutStreamer->emitLabel(LblSym);
    OutStreamer->emitValue(Expr, Size);
  }
}

void XtensaAsmPrinter::printOperand(const MachineInstr *MI, int OpNo,
                                    raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNo);

  switch (MO.getType()) {
  case MachineOperand::MO_Register:
  case MachineOperand::MO_Immediate: {
    XtensaMCInstLower Lower(MF->getContext(), *this);
    MCOperand MC(Lower.lowerOperand(MI->getOperand(OpNo)));
    XtensaInstPrinter::printOperand(MC, O);
    break;
  }
  case MachineOperand::MO_GlobalAddress:
    O << *getSymbol(MO.getGlobal());
    break;
  default:
    llvm_unreachable("<unknown operand type>");
  }

  if (MO.getTargetFlags()) {
    O << ")";
  }
}

bool XtensaAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                       const char *ExtraCode, raw_ostream &O) {
  if (ExtraCode && *ExtraCode == 'n') {
    if (!MI->getOperand(OpNo).isImm())
      return true;
    O << -int64_t(MI->getOperand(OpNo).getImm());
  } else {
    printOperand(MI, OpNo, O);
  }
  return false;
}

bool XtensaAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                             unsigned OpNo,
                                             const char *ExtraCode,
                                             raw_ostream &OS) {
  if (ExtraCode && ExtraCode[0])
    return true; // Unknown modifier.

  assert(OpNo + 1 < MI->getNumOperands() && "Insufficient operands");

  const MachineOperand &Base = MI->getOperand(OpNo);
  const MachineOperand &Offset = MI->getOperand(OpNo + 1);

  assert(Base.isReg() &&
         "Unexpected base pointer for inline asm memory operand.");
  assert(Offset.isImm() && "Unexpected offset for inline asm memory operand.");

  OS << XtensaInstPrinter::getRegisterName(Base.getReg());
  OS << ", ";
  OS << Offset.getImm();

  return false;
}

// Force static initialization.
extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeXtensaAsmPrinter() {
  RegisterAsmPrinter<XtensaAsmPrinter> A(getTheXtensaTarget());
}
