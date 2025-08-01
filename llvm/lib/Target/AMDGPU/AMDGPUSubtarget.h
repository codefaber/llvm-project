//=====-- AMDGPUSubtarget.h - Define Subtarget for AMDGPU -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//==-----------------------------------------------------------------------===//
//
/// \file
/// Base class for AMDGPU specific classes of TargetSubtarget.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_AMDGPUSUBTARGET_H
#define LLVM_LIB_TARGET_AMDGPU_AMDGPUSUBTARGET_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/Support/Alignment.h"
#include "llvm/TargetParser/Triple.h"

namespace llvm {

enum AMDGPUDwarfFlavour : unsigned;
class Function;
class Instruction;
class MachineFunction;
class TargetMachine;

class AMDGPUSubtarget {
public:
  enum Generation {
    INVALID = 0,
    R600 = 1,
    R700 = 2,
    EVERGREEN = 3,
    NORTHERN_ISLANDS = 4,
    SOUTHERN_ISLANDS = 5,
    SEA_ISLANDS = 6,
    VOLCANIC_ISLANDS = 7,
    GFX9 = 8,
    GFX10 = 9,
    GFX11 = 10,
    GFX12 = 11,
  };

private:
  Triple TargetTriple;

protected:
  bool GCN3Encoding = false;
  bool Has16BitInsts = false;
  bool HasTrue16BitInsts = false;
  bool HasFP8ConversionScaleInsts = false;
  bool HasBF8ConversionScaleInsts = false;
  bool HasFP4ConversionScaleInsts = false;
  bool HasFP6BF6ConversionScaleInsts = false;
  bool HasF16BF16ToFP6BF6ConversionScaleInsts = false;
  bool HasCvtPkF16F32Inst = false;
  bool HasF32ToF16BF16ConversionSRInsts = false;
  bool EnableRealTrue16Insts = false;
  bool HasBF16TransInsts = false;
  bool HasBF16ConversionInsts = false;
  bool HasBF16PackedInsts = false;
  bool HasMadMixInsts = false;
  bool HasMadMacF32Insts = false;
  bool HasDsSrc2Insts = false;
  bool HasSDWA = false;
  bool HasVOP3PInsts = false;
  bool HasMulI24 = true;
  bool HasMulU24 = true;
  bool HasSMulHi = false;
  bool HasInv2PiInlineImm = false;
  bool HasFminFmaxLegacy = true;
  bool EnablePromoteAlloca = false;
  bool HasTrigReducedRange = false;
  bool FastFMAF32 = false;
  unsigned EUsPerCU = 4;
  unsigned MaxWavesPerEU = 10;
  unsigned LocalMemorySize = 0;
  unsigned AddressableLocalMemorySize = 0;
  char WavefrontSizeLog2 = 0;

public:
  AMDGPUSubtarget(Triple TT);

  static const AMDGPUSubtarget &get(const MachineFunction &MF);
  static const AMDGPUSubtarget &get(const TargetMachine &TM,
                                    const Function &F);

  /// \returns Default range flat work group size for a calling convention.
  std::pair<unsigned, unsigned> getDefaultFlatWorkGroupSize(CallingConv::ID CC) const;

  /// \returns Subtarget's default pair of minimum/maximum flat work group sizes
  /// for function \p F, or minimum/maximum flat work group sizes explicitly
  /// requested using "amdgpu-flat-work-group-size" attribute attached to
  /// function \p F.
  ///
  /// \returns Subtarget's default values if explicitly requested values cannot
  /// be converted to integer, or violate subtarget's specifications.
  std::pair<unsigned, unsigned> getFlatWorkGroupSizes(const Function &F) const;

  /// \returns Subtarget's default pair of minimum/maximum number of waves per
  /// execution unit for function \p F, or minimum/maximum number of waves per
  /// execution unit explicitly requested using "amdgpu-waves-per-eu" attribute
  /// attached to function \p F.
  ///
  /// \returns Subtarget's default values if explicitly requested values cannot
  /// be converted to integer, violate subtarget's specifications, or are not
  /// compatible with minimum/maximum number of waves limited by flat work group
  /// size, register usage, and/or lds usage.
  std::pair<unsigned, unsigned> getWavesPerEU(const Function &F) const;

  /// Overload which uses the specified values for the flat work group sizes,
  /// rather than querying the function itself. \p FlatWorkGroupSizes Should
  /// correspond to the function's value for getFlatWorkGroupSizes.
  std::pair<unsigned, unsigned>
  getWavesPerEU(const Function &F,
                std::pair<unsigned, unsigned> FlatWorkGroupSizes) const;

  /// Overload which uses the specified values for the flat workgroup sizes and
  /// LDS space rather than querying the function itself. \p FlatWorkGroupSizes
  /// should correspond to the function's value for getFlatWorkGroupSizes and \p
  /// LDSBytes to the per-workgroup LDS allocation.
  std::pair<unsigned, unsigned>
  getWavesPerEU(std::pair<unsigned, unsigned> FlatWorkGroupSizes,
                unsigned LDSBytes, const Function &F) const;

  /// Returns the target minimum/maximum number of waves per EU. This is based
  /// on the minimum/maximum number of \p RequestedWavesPerEU and further
  /// limited by the maximum achievable occupancy derived from the range of \p
  /// FlatWorkGroupSizes and number of \p LDSBytes per workgroup.
  std::pair<unsigned, unsigned>
  getEffectiveWavesPerEU(std::pair<unsigned, unsigned> RequestedWavesPerEU,
                         std::pair<unsigned, unsigned> FlatWorkGroupSizes,
                         unsigned LDSBytes) const;

  /// Return the amount of LDS that can be used that will not restrict the
  /// occupancy lower than WaveCount.
  unsigned getMaxLocalMemSizeWithWaveCount(unsigned WaveCount,
                                           const Function &) const;

  /// Subtarget's minimum/maximum occupancy, in number of waves per EU, that can
  /// be achieved when the only function running on a CU is \p F and each
  /// workgroup running the function requires \p LDSBytes bytes of LDS space.
  /// This notably depends on the range of allowed flat group sizes for the
  /// function and hardware characteristics.
  std::pair<unsigned, unsigned>
  getOccupancyWithWorkGroupSizes(uint32_t LDSBytes, const Function &F) const {
    return getOccupancyWithWorkGroupSizes(LDSBytes, getFlatWorkGroupSizes(F));
  }

  /// Overload which uses the specified values for the flat work group sizes,
  /// rather than querying the function itself. \p FlatWorkGroupSizes should
  /// correspond to the function's value for getFlatWorkGroupSizes.
  std::pair<unsigned, unsigned> getOccupancyWithWorkGroupSizes(
      uint32_t LDSBytes,
      std::pair<unsigned, unsigned> FlatWorkGroupSizes) const;

  /// Subtarget's minimum/maximum occupancy, in number of waves per EU, that can
  /// be achieved when the only function running on a CU is \p MF. This notably
  /// depends on the range of allowed flat group sizes for the function, the
  /// amount of per-workgroup LDS space required by the function, and hardware
  /// characteristics.
  std::pair<unsigned, unsigned>
  getOccupancyWithWorkGroupSizes(const MachineFunction &MF) const;

  bool isAmdHsaOS() const {
    return TargetTriple.getOS() == Triple::AMDHSA;
  }

  bool isAmdPalOS() const {
    return TargetTriple.getOS() == Triple::AMDPAL;
  }

  bool isMesa3DOS() const {
    return TargetTriple.getOS() == Triple::Mesa3D;
  }

  bool isMesaKernel(const Function &F) const;

  bool isAmdHsaOrMesa(const Function &F) const {
    return isAmdHsaOS() || isMesaKernel(F);
  }

  bool isGCN() const { return TargetTriple.isAMDGCN(); }

  bool isGCN3Encoding() const {
    return GCN3Encoding;
  }

  bool has16BitInsts() const {
    return Has16BitInsts;
  }

  /// Return true if the subtarget supports True16 instructions.
  bool hasTrue16BitInsts() const { return HasTrue16BitInsts; }

  /// Return true if real (non-fake) variants of True16 instructions using
  /// 16-bit registers should be code-generated. Fake True16 instructions are
  /// identical to non-fake ones except that they take 32-bit registers as
  /// operands and always use their low halves.
  // TODO: Remove and use hasTrue16BitInsts() instead once True16 is fully
  // supported and the support for fake True16 instructions is removed.
  bool useRealTrue16Insts() const;

  bool hasBF16TransInsts() const { return HasBF16TransInsts; }

  bool hasBF16ConversionInsts() const {
    return HasBF16ConversionInsts;
  }

  bool hasBF16PackedInsts() const { return HasBF16PackedInsts; }

  bool hasMadMixInsts() const {
    return HasMadMixInsts;
  }

  bool hasFP8ConversionScaleInsts() const { return HasFP8ConversionScaleInsts; }

  bool hasBF8ConversionScaleInsts() const { return HasBF8ConversionScaleInsts; }

  bool hasFP4ConversionScaleInsts() const { return HasFP4ConversionScaleInsts; }

  bool hasFP6BF6ConversionScaleInsts() const {
    return HasFP6BF6ConversionScaleInsts;
  }

  bool hasF16BF16ToFP6BF6ConversionScaleInsts() const {
    return HasF16BF16ToFP6BF6ConversionScaleInsts;
  }

  bool hasCvtPkF16F32Inst() const { return HasCvtPkF16F32Inst; }

  bool hasF32ToF16BF16ConversionSRInsts() const {
    return HasF32ToF16BF16ConversionSRInsts;
  }

  bool hasMadMacF32Insts() const {
    return HasMadMacF32Insts || !isGCN();
  }

  bool hasDsSrc2Insts() const {
    return HasDsSrc2Insts;
  }

  bool hasSDWA() const {
    return HasSDWA;
  }

  bool hasVOP3PInsts() const {
    return HasVOP3PInsts;
  }

  bool hasMulI24() const {
    return HasMulI24;
  }

  bool hasMulU24() const {
    return HasMulU24;
  }

  bool hasSMulHi() const {
    return HasSMulHi;
  }

  bool hasInv2PiInlineImm() const {
    return HasInv2PiInlineImm;
  }

  bool hasFminFmaxLegacy() const {
    return HasFminFmaxLegacy;
  }

  bool hasTrigReducedRange() const {
    return HasTrigReducedRange;
  }

  bool hasFastFMAF32() const {
    return FastFMAF32;
  }

  bool isPromoteAllocaEnabled() const {
    return EnablePromoteAlloca;
  }

  unsigned getWavefrontSize() const {
    return 1 << WavefrontSizeLog2;
  }

  unsigned getWavefrontSizeLog2() const {
    return WavefrontSizeLog2;
  }

  /// Return the maximum number of bytes of LDS available for all workgroups
  /// running on the same WGP or CU.
  /// For GFX10-GFX12 in WGP mode this is 128k even though each workgroup is
  /// limited to 64k.
  unsigned getLocalMemorySize() const {
    return LocalMemorySize;
  }

  /// Return the maximum number of bytes of LDS that can be allocated to a
  /// single workgroup.
  /// For GFX10-GFX12 in WGP mode this is limited to 64k even though the WGP has
  /// 128k in total.
  unsigned getAddressableLocalMemorySize() const {
    return AddressableLocalMemorySize;
  }

  /// Number of SIMDs/EUs (execution units) per "CU" ("compute unit"), where the
  /// "CU" is the unit onto which workgroups are mapped. This takes WGP mode vs.
  /// CU mode into account.
  unsigned getEUsPerCU() const { return EUsPerCU; }

  Align getAlignmentForImplicitArgPtr() const {
    return isAmdHsaOS() ? Align(8) : Align(4);
  }

  /// Returns the offset in bytes from the start of the input buffer
  ///        of the first explicit kernel argument.
  unsigned getExplicitKernelArgOffset() const {
    switch (TargetTriple.getOS()) {
    case Triple::AMDHSA:
    case Triple::AMDPAL:
    case Triple::Mesa3D:
      return 0;
    case Triple::UnknownOS:
    default:
      // For legacy reasons unknown/other is treated as a different version of
      // mesa.
      return 36;
    }

    llvm_unreachable("invalid triple OS");
  }

  /// \returns Maximum number of work groups per compute unit supported by the
  /// subtarget and limited by given \p FlatWorkGroupSize.
  virtual unsigned getMaxWorkGroupsPerCU(unsigned FlatWorkGroupSize) const = 0;

  /// \returns Minimum flat work group size supported by the subtarget.
  virtual unsigned getMinFlatWorkGroupSize() const = 0;

  /// \returns Maximum flat work group size supported by the subtarget.
  virtual unsigned getMaxFlatWorkGroupSize() const = 0;

  /// \returns Number of waves per execution unit required to support the given
  /// \p FlatWorkGroupSize.
  virtual unsigned
  getWavesPerEUForWorkGroup(unsigned FlatWorkGroupSize) const = 0;

  /// \returns Minimum number of waves per execution unit supported by the
  /// subtarget.
  virtual unsigned getMinWavesPerEU() const = 0;

  /// \returns Maximum number of waves per execution unit supported by the
  /// subtarget without any kind of limitation.
  unsigned getMaxWavesPerEU() const { return MaxWavesPerEU; }

  /// Return the maximum workitem ID value in the function, for the given (0, 1,
  /// 2) dimension.
  unsigned getMaxWorkitemID(const Function &Kernel, unsigned Dimension) const;

  /// Return the number of work groups for the function.
  SmallVector<unsigned> getMaxNumWorkGroups(const Function &F) const;

  /// Return true if only a single workitem can be active in a wave.
  bool isSingleLaneExecution(const Function &Kernel) const;

  /// Creates value range metadata on an workitemid.* intrinsic call or load.
  bool makeLIDRangeMetadata(Instruction *I) const;

  /// \returns Number of bytes of arguments that are passed to a shader or
  /// kernel in addition to the explicit ones declared for the function.
  unsigned getImplicitArgNumBytes(const Function &F) const;
  uint64_t getExplicitKernArgSize(const Function &F, Align &MaxAlign) const;
  unsigned getKernArgSegmentSize(const Function &F, Align &MaxAlign) const;

  /// \returns Corresponding DWARF register number mapping flavour for the
  /// \p WavefrontSize.
  AMDGPUDwarfFlavour getAMDGPUDwarfFlavour() const;

  virtual ~AMDGPUSubtarget() = default;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_AMDGPU_AMDGPUSUBTARGET_H
