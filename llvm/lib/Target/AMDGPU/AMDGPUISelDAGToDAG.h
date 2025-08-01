//===-- AMDGPUISelDAGToDAG.h - A dag to dag inst selector for AMDGPU ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//==-----------------------------------------------------------------------===//
//
/// \file
/// Defines an instruction selector for the AMDGPU target.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_AMDGPUISELDAGTODAG_H
#define LLVM_LIB_TARGET_AMDGPU_AMDGPUISELDAGTODAG_H

#include "GCNSubtarget.h"
#include "SIMachineFunctionInfo.h"
#include "SIModeRegisterDefaults.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Support/AMDGPUAddrSpace.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {

static inline bool getConstantValue(SDValue N, uint32_t &Out) {
  // This is only used for packed vectors, where using 0 for undef should
  // always be good.
  if (N.isUndef()) {
    Out = 0;
    return true;
  }

  if (const ConstantSDNode *C = dyn_cast<ConstantSDNode>(N)) {
    Out = C->getAPIntValue().getSExtValue();
    return true;
  }

  if (const ConstantFPSDNode *C = dyn_cast<ConstantFPSDNode>(N)) {
    Out = C->getValueAPF().bitcastToAPInt().getSExtValue();
    return true;
  }

  return false;
}

// TODO: Handle undef as zero
static inline SDNode *packConstantV2I16(const SDNode *N, SelectionDAG &DAG) {
  assert(N->getOpcode() == ISD::BUILD_VECTOR && N->getNumOperands() == 2);
  uint32_t LHSVal, RHSVal;
  if (getConstantValue(N->getOperand(0), LHSVal) &&
      getConstantValue(N->getOperand(1), RHSVal)) {
    SDLoc SL(N);
    uint32_t K = (LHSVal & 0xffff) | (RHSVal << 16);
    return DAG.getMachineNode(AMDGPU::S_MOV_B32, SL, N->getValueType(0),
                              DAG.getTargetConstant(K, SL, MVT::i32));
  }

  return nullptr;
}

/// AMDGPU specific code to select AMDGPU machine instructions for
/// SelectionDAG operations.
class AMDGPUDAGToDAGISel : public SelectionDAGISel {
  // Subtarget - Keep a pointer to the AMDGPU Subtarget around so that we can
  // make the right decision when generating code for different targets.
  const GCNSubtarget *Subtarget;

  // Default FP mode for the current function.
  SIModeRegisterDefaults Mode;

  // Instructions that will be lowered with a final instruction that zeros the
  // high result bits.
  bool fp16SrcZerosHighBits(unsigned Opc) const;

public:
  AMDGPUDAGToDAGISel() = delete;

  explicit AMDGPUDAGToDAGISel(TargetMachine &TM, CodeGenOptLevel OptLevel);

  bool runOnMachineFunction(MachineFunction &MF) override;
  bool matchLoadD16FromBuildVector(SDNode *N) const;
  void PreprocessISelDAG() override;
  void Select(SDNode *N) override;
  void PostprocessISelDAG() override;

protected:
  void SelectBuildVector(SDNode *N, unsigned RegClassID);
  void SelectVectorShuffle(SDNode *N);

private:
  std::pair<SDValue, SDValue> foldFrameIndex(SDValue N) const;

  bool isInlineImmediate(const SDNode *N) const;

  bool isInlineImmediate(const APInt &Imm) const {
    return Subtarget->getInstrInfo()->isInlineConstant(Imm);
  }

  bool isInlineImmediate(const APFloat &Imm) const {
    return Subtarget->getInstrInfo()->isInlineConstant(Imm);
  }

  bool isVGPRImm(const SDNode *N) const;
  bool isUniformLoad(const SDNode *N) const;
  bool isUniformBr(const SDNode *N) const;

  // Returns true if ISD::AND SDNode `N`'s masking of the shift amount operand's
  // `ShAmtBits` bits is unneeded.
  bool isUnneededShiftMask(const SDNode *N, unsigned ShAmtBits) const;

  bool isBaseWithConstantOffset64(SDValue Addr, SDValue &LHS,
                                  SDValue &RHS) const;

  MachineSDNode *buildSMovImm64(SDLoc &DL, uint64_t Val, EVT VT) const;

  SDNode *glueCopyToOp(SDNode *N, SDValue NewChain, SDValue Glue) const;
  SDNode *glueCopyToM0(SDNode *N, SDValue Val) const;
  SDNode *glueCopyToM0LDSInit(SDNode *N) const;

  const TargetRegisterClass *getOperandRegClass(SDNode *N, unsigned OpNo) const;
  virtual bool SelectADDRVTX_READ(SDValue Addr, SDValue &Base, SDValue &Offset);
  virtual bool SelectADDRIndirect(SDValue Addr, SDValue &Base, SDValue &Offset);
  bool isDSOffsetLegal(SDValue Base, unsigned Offset) const;
  bool isDSOffset2Legal(SDValue Base, unsigned Offset0, unsigned Offset1,
                        unsigned Size) const;

  bool isFlatScratchBaseLegal(SDValue Addr) const;
  bool isFlatScratchBaseLegalSV(SDValue Addr) const;
  bool isFlatScratchBaseLegalSVImm(SDValue Addr) const;
  bool isSOffsetLegalWithImmOffset(SDValue *SOffset, bool Imm32Only,
                                   bool IsBuffer, int64_t ImmOffset = 0) const;

  bool SelectDS1Addr1Offset(SDValue Ptr, SDValue &Base, SDValue &Offset) const;
  bool SelectDS64Bit4ByteAligned(SDValue Ptr, SDValue &Base, SDValue &Offset0,
                                 SDValue &Offset1) const;
  bool SelectDS128Bit8ByteAligned(SDValue Ptr, SDValue &Base, SDValue &Offset0,
                                  SDValue &Offset1) const;
  bool SelectDSReadWrite2(SDValue Ptr, SDValue &Base, SDValue &Offset0,
                          SDValue &Offset1, unsigned Size) const;
  bool SelectMUBUF(SDValue Addr, SDValue &SRsrc, SDValue &VAddr,
                   SDValue &SOffset, SDValue &Offset, SDValue &Offen,
                   SDValue &Idxen, SDValue &Addr64) const;
  bool SelectMUBUFAddr64(SDValue Addr, SDValue &SRsrc, SDValue &VAddr,
                         SDValue &SOffset, SDValue &Offset) const;
  bool SelectMUBUFScratchOffen(SDNode *Parent, SDValue Addr, SDValue &RSrc,
                               SDValue &VAddr, SDValue &SOffset,
                               SDValue &ImmOffset) const;
  bool SelectMUBUFScratchOffset(SDNode *Parent, SDValue Addr, SDValue &SRsrc,
                                SDValue &Soffset, SDValue &Offset) const;

  bool SelectMUBUFOffset(SDValue Addr, SDValue &SRsrc, SDValue &Soffset,
                         SDValue &Offset) const;
  bool SelectBUFSOffset(SDValue Addr, SDValue &SOffset) const;

  bool SelectFlatOffsetImpl(SDNode *N, SDValue Addr, SDValue &VAddr,
                            SDValue &Offset, uint64_t FlatVariant) const;
  bool SelectFlatOffset(SDNode *N, SDValue Addr, SDValue &VAddr,
                        SDValue &Offset) const;
  bool SelectGlobalOffset(SDNode *N, SDValue Addr, SDValue &VAddr,
                          SDValue &Offset) const;
  bool SelectScratchOffset(SDNode *N, SDValue Addr, SDValue &VAddr,
                           SDValue &Offset) const;
  bool SelectGlobalSAddr(SDNode *N, SDValue Addr, SDValue &SAddr,
                         SDValue &VOffset, SDValue &Offset, bool &ScaleOffset,
                         bool NeedIOffset = true) const;
  bool SelectGlobalSAddr(SDNode *N, SDValue Addr, SDValue &SAddr,
                         SDValue &VOffset, SDValue &Offset,
                         SDValue &CPol) const;
  bool SelectGlobalSAddrCPol(SDNode *N, SDValue Addr, SDValue &SAddr,
                             SDValue &VOffset, SDValue &Offset,
                             SDValue &CPol) const;
  bool SelectGlobalSAddrGLC(SDNode *N, SDValue Addr, SDValue &SAddr,
                            SDValue &VOffset, SDValue &Offset,
                            SDValue &CPol) const;
  bool SelectScratchSAddr(SDNode *N, SDValue Addr, SDValue &SAddr,
                          SDValue &Offset) const;
  bool checkFlatScratchSVSSwizzleBug(SDValue VAddr, SDValue SAddr,
                                     uint64_t ImmOffset) const;
  bool SelectScratchSVAddr(SDNode *N, SDValue Addr, SDValue &VAddr,
                           SDValue &SAddr, SDValue &Offset,
                           SDValue &CPol) const;

  bool SelectSMRDOffset(SDNode *N, SDValue ByteOffsetNode, SDValue *SOffset,
                        SDValue *Offset, bool Imm32Only = false,
                        bool IsBuffer = false, bool HasSOffset = false,
                        int64_t ImmOffset = 0,
                        bool *ScaleOffset = nullptr) const;
  SDValue Expand32BitAddress(SDValue Addr) const;
  bool SelectSMRDBaseOffset(SDNode *N, SDValue Addr, SDValue &SBase,
                            SDValue *SOffset, SDValue *Offset,
                            bool Imm32Only = false, bool IsBuffer = false,
                            bool HasSOffset = false, int64_t ImmOffset = 0,
                            bool *ScaleOffset = nullptr) const;
  bool SelectSMRD(SDNode *N, SDValue Addr, SDValue &SBase, SDValue *SOffset,
                  SDValue *Offset, bool Imm32Only = false,
                  bool *ScaleOffset = nullptr) const;
  bool SelectSMRDImm(SDValue Addr, SDValue &SBase, SDValue &Offset) const;
  bool SelectSMRDImm32(SDValue Addr, SDValue &SBase, SDValue &Offset) const;
  bool SelectScaleOffset(SDNode *N, SDValue &Offset, bool IsSigned) const;
  bool SelectSMRDSgpr(SDNode *N, SDValue Addr, SDValue &SBase, SDValue &SOffset,
                      SDValue &CPol) const;
  bool SelectSMRDSgprImm(SDNode *N, SDValue Addr, SDValue &SBase,
                         SDValue &SOffset, SDValue &Offset,
                         SDValue &CPol) const;
  bool SelectSMRDBufferImm(SDValue N, SDValue &Offset) const;
  bool SelectSMRDBufferImm32(SDValue N, SDValue &Offset) const;
  bool SelectSMRDBufferSgprImm(SDValue N, SDValue &SOffset,
                               SDValue &Offset) const;
  bool SelectSMRDPrefetchImm(SDValue Addr, SDValue &SBase,
                             SDValue &Offset) const;
  bool SelectMOVRELOffset(SDValue Index, SDValue &Base, SDValue &Offset) const;

  bool SelectVOP3ModsImpl(SDValue In, SDValue &Src, unsigned &SrcMods,
                          bool IsCanonicalizing = true,
                          bool AllowAbs = true) const;
  bool SelectVOP3Mods(SDValue In, SDValue &Src, SDValue &SrcMods) const;
  bool SelectVOP3ModsNonCanonicalizing(SDValue In, SDValue &Src,
                                       SDValue &SrcMods) const;
  bool SelectVOP3BMods(SDValue In, SDValue &Src, SDValue &SrcMods) const;
  bool SelectVOP3NoMods(SDValue In, SDValue &Src) const;
  bool SelectVOP3Mods0(SDValue In, SDValue &Src, SDValue &SrcMods,
                       SDValue &Clamp, SDValue &Omod) const;
  bool SelectVOP3BMods0(SDValue In, SDValue &Src, SDValue &SrcMods,
                        SDValue &Clamp, SDValue &Omod) const;
  bool SelectVOP3NoMods0(SDValue In, SDValue &Src, SDValue &SrcMods,
                         SDValue &Clamp, SDValue &Omod) const;

  bool SelectVINTERPModsImpl(SDValue In, SDValue &Src, SDValue &SrcMods,
                             bool OpSel) const;
  bool SelectVINTERPMods(SDValue In, SDValue &Src, SDValue &SrcMods) const;
  bool SelectVINTERPModsHi(SDValue In, SDValue &Src, SDValue &SrcMods) const;

  bool SelectVOP3OMods(SDValue In, SDValue &Src, SDValue &Clamp,
                       SDValue &Omod) const;

  bool SelectVOP3PMods(SDValue In, SDValue &Src, SDValue &SrcMods,
                       bool IsDOT = false) const;
  bool SelectVOP3PModsDOT(SDValue In, SDValue &Src, SDValue &SrcMods) const;

  bool SelectVOP3PModsNeg(SDValue In, SDValue &Src) const;
  bool SelectVOP3PModsNegs(SDValue In, SDValue &Src) const;
  bool SelectVOP3PModsNegAbs(SDValue In, SDValue &Src) const;
  bool SelectWMMAOpSelVOP3PMods(SDValue In, SDValue &Src) const;

  bool SelectWMMAModsF32NegAbs(SDValue In, SDValue &Src,
                               SDValue &SrcMods) const;
  bool SelectWMMAModsF16Neg(SDValue In, SDValue &Src, SDValue &SrcMods) const;
  bool SelectWMMAModsF16NegAbs(SDValue In, SDValue &Src,
                               SDValue &SrcMods) const;
  bool SelectWMMAVISrc(SDValue In, SDValue &Src) const;

  bool SelectSWMMACIndex8(SDValue In, SDValue &Src, SDValue &IndexKey) const;
  bool SelectSWMMACIndex16(SDValue In, SDValue &Src, SDValue &IndexKey) const;
  bool SelectSWMMACIndex32(SDValue In, SDValue &Src, SDValue &IndexKey) const;

  bool SelectVOP3OpSel(SDValue In, SDValue &Src, SDValue &SrcMods) const;

  bool SelectVOP3OpSelMods(SDValue In, SDValue &Src, SDValue &SrcMods) const;
  bool SelectVOP3PMadMixModsImpl(SDValue In, SDValue &Src, unsigned &Mods,
                                 MVT VT) const;
  bool SelectVOP3PMadMixModsExt(SDValue In, SDValue &Src,
                                SDValue &SrcMods) const;
  bool SelectVOP3PMadMixMods(SDValue In, SDValue &Src, SDValue &SrcMods) const;
  bool SelectVOP3PMadMixBF16ModsExt(SDValue In, SDValue &Src,
                                    SDValue &SrcMods) const;
  bool SelectVOP3PMadMixBF16Mods(SDValue In, SDValue &Src,
                                 SDValue &SrcMods) const;

  bool SelectBITOP3(SDValue In, SDValue &Src0, SDValue &Src1, SDValue &Src2,
                   SDValue &Tbl) const;

  SDValue getHi16Elt(SDValue In) const;

  SDValue getMaterializedScalarImm32(int64_t Val, const SDLoc &DL) const;

  void SelectADD_SUB_I64(SDNode *N);
  void SelectAddcSubb(SDNode *N);
  void SelectUADDO_USUBO(SDNode *N);
  void SelectDIV_SCALE(SDNode *N);
  void SelectMAD_64_32(SDNode *N);
  void SelectMUL_LOHI(SDNode *N);
  void SelectFMA_W_CHAIN(SDNode *N);
  void SelectFMUL_W_CHAIN(SDNode *N);
  SDNode *getBFE32(bool IsSigned, const SDLoc &DL, SDValue Val, uint32_t Offset,
                   uint32_t Width);
  void SelectS_BFEFromShifts(SDNode *N);
  void SelectS_BFE(SDNode *N);
  bool isCBranchSCC(const SDNode *N) const;
  void SelectBRCOND(SDNode *N);
  void SelectFMAD_FMA(SDNode *N);
  void SelectFP_EXTEND(SDNode *N);
  void SelectDSAppendConsume(SDNode *N, unsigned IntrID);
  void SelectDSBvhStackIntrinsic(SDNode *N, unsigned IntrID);
  void SelectDS_GWS(SDNode *N, unsigned IntrID);
  void SelectInterpP1F16(SDNode *N);
  void SelectINTRINSIC_W_CHAIN(SDNode *N);
  void SelectINTRINSIC_WO_CHAIN(SDNode *N);
  void SelectINTRINSIC_VOID(SDNode *N);
  void SelectWAVE_ADDRESS(SDNode *N);
  void SelectSTACKRESTORE(SDNode *N);

protected:
  // Include the pieces autogenerated from the target description.
#include "AMDGPUGenDAGISel.inc"
};

class AMDGPUISelDAGToDAGPass : public SelectionDAGISelPass {
public:
  AMDGPUISelDAGToDAGPass(TargetMachine &TM);

  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);
};

class AMDGPUDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;

  AMDGPUDAGToDAGISelLegacy(TargetMachine &TM, CodeGenOptLevel OptLevel);

  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  StringRef getPassName() const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_AMDGPU_AMDGPUISELDAGTODAG_H
