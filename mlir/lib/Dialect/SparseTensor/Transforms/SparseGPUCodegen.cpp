//===- SparseGPUCodegen.cpp - Generates GPU code --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is a prototype GPU codegenerator for the sparsifier.
// The objective is to eventually use the right combination of
// direct code generation and libary calls into vendor-specific
// highly optimized sparse libraries (e.g. cuSparse for CUDA).
//
//===----------------------------------------------------------------------===//

#include "Utils/CodegenUtils.h"
#include "Utils/LoopEmitter.h"

#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Utils/Utils.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/SparseTensor/IR/SparseTensor.h"
#include "mlir/Dialect/SparseTensor/IR/SparseTensorType.h"
#include "mlir/Dialect/SparseTensor/Transforms/Passes.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Matchers.h"

using namespace mlir;
using namespace mlir::sparse_tensor;

namespace {

// Sparse formats supported by cuSparse.
enum class CuSparseFormat {
  kNone,
  kCOO,
  kCSR,
  kCSC,
  kBSR,
};

//===----------------------------------------------------------------------===//
// Helper methods.
//===----------------------------------------------------------------------===//

/// Marks the given top module as a GPU container module.
static void markAsGPUContainer(ModuleOp topModule) {
  topModule->setAttr(gpu::GPUDialect::getContainerModuleAttrName(),
                     UnitAttr::get(topModule->getContext()));
}

/// Constructs a new GPU module (for GPU kernels) inside the given top module,
/// or returns an existing GPU module if one was built previously.
static gpu::GPUModuleOp genGPUModule(OpBuilder &builder, ModuleOp topModule) {
  for (auto op : topModule.getBodyRegion().getOps<gpu::GPUModuleOp>())
    return op; // existing
  markAsGPUContainer(topModule);
  builder.setInsertionPointToStart(topModule.getBody());
  return gpu::GPUModuleOp::create(builder, topModule->getLoc(),
                                  "sparse_kernels");
}

/// Constructs a new GPU kernel in the given GPU module.
static gpu::GPUFuncOp genGPUFunc(OpBuilder &builder, gpu::GPUModuleOp gpuModule,
                                 SmallVectorImpl<Value> &args) {
  // Get a unique kernel name. Not very creative,
  // but we simply try kernel0, kernel1, etc.
  unsigned kernelNumber = 0;
  SmallString<16> kernelName;
  do {
    kernelName.clear();
    ("kernel" + Twine(kernelNumber++)).toStringRef(kernelName);
  } while (gpuModule.lookupSymbol(kernelName));
  // Then we insert a new kernel with given arguments into the module.
  builder.setInsertionPointToStart(gpuModule.getBody());
  SmallVector<Type> argsTp;
  for (auto arg : args)
    argsTp.push_back(arg.getType());
  FunctionType type = FunctionType::get(gpuModule->getContext(), argsTp, {});
  auto gpuFunc =
      gpu::GPUFuncOp::create(builder, gpuModule->getLoc(), kernelName, type);
  gpuFunc->setAttr(gpu::GPUDialect::getKernelFuncAttrName(),
                   builder.getUnitAttr());
  return gpuFunc;
}

/// Constructs code to launch GPU kernel.
static Value genLaunchGPUFunc(OpBuilder &builder, gpu::GPUFuncOp gpuFunc,
                              SmallVectorImpl<Value> &args,
                              SmallVectorImpl<Value> &tokens,
                              unsigned numThreads) {
  Location loc = gpuFunc->getLoc();
  Value none = TypedValue<::mlir::IntegerType>{};
  Value one = constantIndex(builder, loc, 1);
  Value numT = constantIndex(builder, loc, numThreads);
  gpu::KernelDim3 gridSize = {one, one, one};
  gpu::KernelDim3 blckSize = {numT, one, one};
  return gpu::LaunchFuncOp::create(builder, loc, gpuFunc, gridSize, blckSize,
                                   /*dynSharedMemSz*/ none, args,
                                   builder.getType<gpu::AsyncTokenType>(),
                                   tokens)
      .getAsyncToken();
}

/// Maps the provided ranked host buffer into the device address space.
/// Writes from the host are guaranteed to be visible to device kernels
/// that are launched afterwards. Writes from the device are guaranteed
/// to be visible on the host after synchronizing with the device kernel
/// completion. Needs to cast the buffer to a unranked buffer.
static Value genHostRegisterMemref(OpBuilder &builder, Location loc,
                                   Value mem) {
  MemRefType memTp = cast<MemRefType>(mem.getType());
  UnrankedMemRefType resTp =
      UnrankedMemRefType::get(memTp.getElementType(), /*memorySpace=*/0);
  Value cast = memref::CastOp::create(builder, loc, resTp, mem);
  gpu::HostRegisterOp::create(builder, loc, cast);
  return cast;
}

/// Unmaps the provided buffer, expecting the casted buffer.
static void genHostUnregisterMemref(OpBuilder &builder, Location loc,
                                    Value cast) {
  gpu::HostUnregisterOp::create(builder, loc, cast);
}

/// Generates first wait in an asynchronous chain.
static Value genFirstWait(OpBuilder &builder, Location loc) {
  Type tokenType = builder.getType<gpu::AsyncTokenType>();
  return gpu::WaitOp::create(builder, loc, tokenType, ValueRange())
      .getAsyncToken();
}

/// Generates last, blocking wait in an asynchronous chain.
static void genBlockingWait(OpBuilder &builder, Location loc,
                            ValueRange operands) {
  gpu::WaitOp::create(builder, loc, Type(), operands);
}

/// Allocates memory on the device.
/// TODO: A `host_shared` attribute could be used to indicate that
///       the buffer is visible by both host and device, but lowering
///       that feature does not seem to be fully supported yet.
static gpu::AllocOp genAllocMemRef(OpBuilder &builder, Location loc, Value mem,
                                   Value token) {
  auto tp = cast<ShapedType>(mem.getType());
  auto elemTp = tp.getElementType();
  auto shape = tp.getShape();
  auto memTp = MemRefType::get(shape, elemTp);
  SmallVector<Value> dynamicSizes;
  for (unsigned r = 0, rank = tp.getRank(); r < rank; r++) {
    if (shape[r] == ShapedType::kDynamic) {
      Value dimOp = linalg::createOrFoldDimOp(builder, loc, mem, r);
      dynamicSizes.push_back(dimOp);
    }
  }
  return gpu::AllocOp::create(builder, loc, TypeRange({memTp, token.getType()}),
                              token, dynamicSizes, ValueRange());
}

// Allocates a typed buffer on the host with given size.
static Value genHostBuffer(OpBuilder &builder, Location loc, Type type,
                           Value size) {
  const auto memTp = MemRefType::get({ShapedType::kDynamic}, type);
  return memref::AllocOp::create(builder, loc, memTp, size).getResult();
}

// Allocates a typed buffer on the device with given size.
static gpu::AllocOp genAllocBuffer(OpBuilder &builder, Location loc, Type type,
                                   Value size, Value token) {
  const auto memTp = MemRefType::get({ShapedType::kDynamic}, type);
  return gpu::AllocOp::create(builder, loc, TypeRange({memTp, token.getType()}),
                              token, size, ValueRange());
}

// Allocates a void buffer on the device with given size.
static gpu::AllocOp genAllocBuffer(OpBuilder &builder, Location loc, Value size,
                                   Value token) {
  return genAllocBuffer(builder, loc, builder.getI8Type(), size, token);
}

/// Deallocates memory from the device.
static Value genDeallocMemRef(OpBuilder &builder, Location loc, Value mem,
                              Value token) {
  return gpu::DeallocOp::create(builder, loc, token.getType(), token, mem)
      .getAsyncToken();
}

/// Copies memory between host and device (direction is implicit).
static Value genCopyMemRef(OpBuilder &builder, Location loc, Value dst,
                           Value src, Value token) {
  return gpu::MemcpyOp::create(builder, loc, token.getType(), token, dst, src)
      .getAsyncToken();
}

/// Generates an alloc/copy pair.
static Value genAllocCopy(OpBuilder &builder, Location loc, Value b,
                          SmallVectorImpl<Value> &tokens) {
  Value firstToken = genFirstWait(builder, loc);
  auto alloc = genAllocMemRef(builder, loc, b, firstToken);
  Value devMem = alloc.getResult(0);
  Value depToken = alloc.getAsyncToken(); // copy-after-alloc
  tokens.push_back(genCopyMemRef(builder, loc, devMem, b, depToken));
  return devMem;
}

/// Generates a memref from tensor operation.
static Value genTensorToMemref(PatternRewriter &rewriter, Location loc,
                               Value tensor) {
  auto tensorType = llvm::cast<ShapedType>(tensor.getType());
  auto memrefType =
      MemRefType::get(tensorType.getShape(), tensorType.getElementType());
  return bufferization::ToBufferOp::create(rewriter, loc, memrefType, tensor);
}

/// Prepares the outlined arguments, passing scalars and buffers in. Here we
/// assume that the first buffer is the one allocated for output. We create
/// a set of properly chained asynchronous allocation/copy pairs to increase
/// overlap before launching the kernel.
static Value genParametersIn(OpBuilder &builder, Location loc,
                             SmallVectorImpl<Value> &scalars,
                             SmallVectorImpl<Value> &buffers,
                             SmallVectorImpl<Value> &args,
                             SmallVectorImpl<Value> &tokens,
                             bool useHostRegistrationForOut) {
  Value out;
  // Scalars are passed by value.
  for (Value s : scalars)
    args.push_back(s);
  // Buffers are need to be made visible on device.
  for (Value b : buffers) {
    if (useHostRegistrationForOut) {
      out = genHostRegisterMemref(builder, loc, b);
      args.push_back(b);
      useHostRegistrationForOut = false;
      continue;
    }
    args.push_back(genAllocCopy(builder, loc, b, tokens));
  }
  return out;
}

/// Finalizes the outlined arguments. The output buffer is copied depending
/// on the kernel token and then deallocated. All other buffers are simply
/// deallocated. Then we wait for all operations to complete.
static void genParametersOut(OpBuilder &builder, Location loc, Value out,
                             Value kernelToken, SmallVectorImpl<Value> &scalars,
                             SmallVectorImpl<Value> &buffers,
                             SmallVectorImpl<Value> &args,
                             SmallVectorImpl<Value> &tokens) {
  unsigned base = scalars.size();
  for (unsigned i = base, e = args.size(); i < e; i++) {
    Value firstToken;
    if (i == base) {
      // Assumed output parameter: unregister or copy-out.
      if (out) {
        genHostUnregisterMemref(builder, loc, out);
        out = Value();
        continue;
      }
      firstToken =
          genCopyMemRef(builder, loc, buffers[0], args[i], kernelToken);
    } else {
      firstToken = genFirstWait(builder, loc);
    }
    tokens.push_back(genDeallocMemRef(builder, loc, args[i], firstToken));
  }
}

/// Constructs code for new GPU kernel.
static void genGPUCode(PatternRewriter &rewriter, gpu::GPUFuncOp gpuFunc,
                       scf::ParallelOp forallOp,
                       SmallVectorImpl<Value> &constants,
                       SmallVectorImpl<Value> &scalars,
                       SmallVectorImpl<Value> &buffers) {
  Location loc = gpuFunc->getLoc();
  Block &block = gpuFunc.getBody().front();
  rewriter.setInsertionPointToStart(&block);

  // Re-generate the constants, recapture all arguments.
  unsigned arg = 0;
  IRMapping irMap;
  for (Value c : constants)
    irMap.map(c, rewriter.clone(*c.getDefiningOp())->getResult(0));
  for (Value s : scalars)
    irMap.map(s, block.getArgument(arg++));
  for (Value b : buffers)
    irMap.map(b, block.getArgument(arg++));

  // Assume 1-dimensional grid/block configuration (only x dimension),
  // so that:
  //   row = blockIdx.x * blockDim.x + threadIdx.x
  //   inc = blockDim.x * gridDim.x
  Value bid = gpu::BlockIdOp::create(rewriter, loc, gpu::Dimension::x);
  Value bsz = gpu::BlockDimOp::create(rewriter, loc, gpu::Dimension::x);
  Value tid = gpu::ThreadIdOp::create(rewriter, loc, gpu::Dimension::x);
  Value gsz = gpu::GridDimOp::create(rewriter, loc, gpu::Dimension::x);
  Value mul = arith::MulIOp::create(rewriter, loc, bid, bsz);
  Value row = arith::AddIOp::create(rewriter, loc, mul, tid);
  Value inc = arith::MulIOp::create(rewriter, loc, bsz, gsz);

  // Construct the iteration over the computational space that
  // accounts for the fact that the total number of threads and
  // the amount of work to be done usually do not match precisely.
  //   for (r = row; r < N; r += inc) {
  //     <loop-body>
  //   }
  Value upper = irMap.lookup(forallOp.getUpperBound()[0]);
  scf::ForOp forOp = scf::ForOp::create(rewriter, loc, row, upper, inc);
  // The scf.for builder creates an empty block. scf.for does not allow multiple
  // blocks in its region, so delete the block before `cloneRegionBefore` adds
  // an additional block.
  rewriter.eraseBlock(forOp.getBody());
  rewriter.cloneRegionBefore(forallOp.getRegion(), forOp.getRegion(),
                             forOp.getRegion().begin(), irMap);
  // Replace the scf.reduce terminator.
  rewriter.setInsertionPoint(forOp.getBody()->getTerminator());
  rewriter.replaceOpWithNewOp<scf::YieldOp>(forOp.getBody()->getTerminator());

  // Done.
  rewriter.setInsertionPointAfter(forOp);
  gpu::ReturnOp::create(rewriter, gpuFunc->getLoc());
}

//===----------------------------------------------------------------------===//
// Library helper methods.
//===----------------------------------------------------------------------===//

/// Helper to detect a + b with arguments taken from given block.
static bool matchAddOfArgs(Block *block, Value val) {
  if (auto *def = val.getDefiningOp()) {
    if (isa<arith::AddFOp, arith::AddIOp>(def)) {
      Value a = block->getArguments()[0];
      Value b = block->getArguments()[1];
      return (def->getOperand(0) == a && def->getOperand(1) == b) ||
             (def->getOperand(0) == b && def->getOperand(1) == a);
    }
  }
  return false;
}

/// Helper to detect a * b with arguments taken from given block.
static bool matchMulOfArgs(Block *block, Value val) {
  if (auto *def = val.getDefiningOp()) {
    if (isa<arith::MulFOp, arith::MulIOp>(def)) {
      Value a = block->getArguments()[0];
      Value b = block->getArguments()[1];
      return (def->getOperand(0) == a && def->getOperand(1) == b) ||
             (def->getOperand(0) == b && def->getOperand(1) == a);
    }
  }
  return false;
}

/// Helper to detect x = x + a * b
static bool matchSumOfMultOfArgs(linalg::GenericOp op) {
  auto yieldOp = cast<linalg::YieldOp>(op.getRegion().front().getTerminator());
  if (auto *def = yieldOp.getOperand(0).getDefiningOp()) {
    if (isa<arith::AddFOp, arith::AddIOp>(def)) {
      Value x = op.getBlock()->getArguments()[2];
      return (def->getOperand(0) == x &&
              matchMulOfArgs(op.getBlock(), def->getOperand(1))) ||
             (def->getOperand(1) == x &&
              matchMulOfArgs(op.getBlock(), def->getOperand(0)));
    }
  }
  return false;
}

// Helper to detect c += spy(s) x (a * b)
static bool matchSumReductionOfMulUnary(linalg::GenericOp op) {
  auto yieldOp = cast<linalg::YieldOp>(op.getRegion().front().getTerminator());
  // The linalg yields a custom reduce result.
  Value s_out = op.getBlock()->getArguments()[2];
  if (auto redOp =
          yieldOp.getOperand(0).getDefiningOp<sparse_tensor::ReduceOp>()) {
    // The reduce consumes the output.
    Value other;
    if (s_out == redOp->getOperand(0))
      other = redOp->getOperand(1);
    else if (s_out == redOp->getOperand(1))
      other = redOp->getOperand(0);
    else
      return false;
    // The reduce op also consumes an unary which also consumes the output
    // and does not define an absent value.
    if (auto unOp = other.getDefiningOp<sparse_tensor::UnaryOp>()) {
      if (s_out != unOp->getOperand(0) || !unOp.getAbsentRegion().empty())
        return false;
      // And the bodies are as expected.
      auto yieldUn = cast<sparse_tensor::YieldOp>(
          unOp.getRegion(0).front().getTerminator());
      auto yieldRed = cast<sparse_tensor::YieldOp>(
          redOp.getRegion().front().getTerminator());
      return matchMulOfArgs(op.getBlock(), yieldUn.getOperand(0)) &&
             matchAddOfArgs(&redOp.getRegion().front(), yieldRed.getOperand(0));
    }
  }
  return false;
}

/// Test for dense tensor.
static bool isDenseTensor(Value v) {
  auto sTp = getSparseTensorType(v);
  return sTp.getDimRank() == sTp.getLvlRank() && sTp.isAllDense();
}

/// Test for suitable positions/coordinates width.
static bool isAdmissibleMetaData(SparseTensorType &aTp) {
  return (aTp.getPosWidth() == 0 || aTp.getPosWidth() >= 16) &&
         (aTp.getCrdWidth() == 0 || aTp.getCrdWidth() >= 16);
}

/// Test for sorted COO matrix with suitable metadata.
static bool isAdmissibleCOO(SparseTensorType &aTp) {
  return aTp.getDimRank() == 2 && aTp.getLvlRank() == 2 && aTp.isIdentity() &&
         aTp.isCompressedLvl(0) && aTp.isOrderedLvl(0) && !aTp.isUniqueLvl(0) &&
         aTp.isSingletonLvl(1) && aTp.isOrderedLvl(1) && aTp.isUniqueLvl(1) &&
         isAdmissibleMetaData(aTp);
}

/// Test for CSR matrix with suitable metadata.
static bool isAdmissibleCSR(SparseTensorType &aTp) {
  return aTp.getDimRank() == 2 && aTp.getLvlRank() == 2 && aTp.isIdentity() &&
         aTp.isDenseLvl(0) && aTp.isCompressedLvl(1) && aTp.isOrderedLvl(1) &&
         aTp.isUniqueLvl(1) && isAdmissibleMetaData(aTp);
}

/// Test for CSC matrix with suitable metadata.
static bool isAdmissibleCSC(SparseTensorType &aTp) {
  return aTp.getDimRank() == 2 && aTp.getLvlRank() == 2 && !aTp.isIdentity() &&
         aTp.isPermutation() && aTp.isDenseLvl(0) && aTp.isCompressedLvl(1) &&
         aTp.isOrderedLvl(1) && aTp.isUniqueLvl(1) && isAdmissibleMetaData(aTp);
}

/// Test for BSR matrix with suitable metadata.
static bool isAdmissibleBSR(SparseTensorType &aTp) {
  if (aTp.getDimRank() == 2 && aTp.getLvlRank() == 4 && aTp.isDenseLvl(0) &&
      aTp.isCompressedLvl(1) && aTp.isOrderedLvl(1) && aTp.isUniqueLvl(1) &&
      aTp.isDenseLvl(2) && aTp.isDenseLvl(3) && isAdmissibleMetaData(aTp)) {
    // CuSparse only supports "square" blocks currently.
    SmallVector<unsigned> dims = getBlockSize(aTp.getDimToLvl());
    assert(dims.size() == 2);
    return dims[0] == dims[1] && dims[0] > 1;
  }
  return false;
}

/// Test for 2:4 matrix with suitable metadata.
static bool isAdmissible24(SparseTensorType &aTp) {
  return aTp.getDimRank() == 2 && aTp.getLvlRank() == 3 && aTp.isDenseLvl(0) &&
         aTp.isDenseLvl(1) && aTp.isNOutOfMLvl(2) && isAdmissibleMetaData(aTp);
}

/// Test for conversion into 2:4 matrix.
static bool isConversionInto24(Value v) {
  if (auto cnv = v.getDefiningOp<ConvertOp>()) {
    Value a = cnv.getResult();
    Value d = cnv.getSource();
    SparseTensorType aTp = getSparseTensorType(a);
    return isDenseTensor(d) && isAdmissible24(aTp);
  }
  return false;
}

/// Returns a suitable sparse format for the operation and given operand
/// types with cuSparse, or kNone if none is available.
static CuSparseFormat getCuSparseFormat(SparseTensorType aTp,
                                        SparseTensorType bTp,
                                        SparseTensorType cTp, bool enableRT,
                                        bool isMatVec) {
  // The other operands have a dense type.
  if (bTp.hasEncoding() || cTp.hasEncoding())
    return CuSparseFormat::kNone;
  // Now check for suitable operand type for the main operand.
  if (isAdmissibleCOO(aTp))
#ifdef CUSPARSE_COO_AOS
    return isMatVec ? CuSparseFormat::kCOO : CuSparseFormat::kNone;
#else
    return enableRT ? CuSparseFormat::kCOO : CuSparseFormat::kNone;
#endif
  if (isAdmissibleCSR(aTp))
    return CuSparseFormat::kCSR;
  if (isAdmissibleCSC(aTp))
    return CuSparseFormat::kCSC;
  if (isAdmissibleBSR(aTp))
    return CuSparseFormat::kBSR;
  return CuSparseFormat::kNone;
}

/// Generates the first positions/coordinates of a sparse matrix.
static Value genFirstPosOrCrds(OpBuilder &builder, Location loc, Value a,
                               CuSparseFormat format, bool enableRT) {
  if (format == CuSparseFormat::kCOO) {
    // Library uses SoA COO, direct IR uses AoS COO.
    if (enableRT)
      return ToCoordinatesOp::create(builder, loc, a, 0);
    return ToCoordinatesBufferOp::create(builder, loc, a);
  }
  // Formats CSR/CSC and BSR use positions at 1.
  return ToPositionsOp::create(builder, loc, a, 1);
}

/// Generates the second coordinates of a sparse matrix.
static Value genSecondCrds(OpBuilder &builder, Location loc, Value a,
                           CuSparseFormat format, bool enableRT) {
  bool isCOO = format == CuSparseFormat::kCOO;
  if (isCOO && !enableRT)
    return Value(); // nothing needed
  // Formats CSR/CSC and BSR use coordinates at 1.
  return ToCoordinatesOp::create(builder, loc, a, 1);
}

/// Generates the sparse matrix handle.
static Operation *genSpMat(OpBuilder &builder, Location loc,
                           SparseTensorType &aTp, Type handleTp, Type tokenTp,
                           Value token, Value sz1, Value sz2, Value nseA,
                           Value rowA, Value colA, Value valA,
                           CuSparseFormat format, bool enableRT) {
  if (format == CuSparseFormat::kCOO) {
    // Library uses SoA COO, direct IR uses AoS COO.
    if (enableRT) {
      assert(colA);
      return gpu::CreateCooOp::create(builder, loc, handleTp, tokenTp, token,
                                      sz1, sz2, nseA, rowA, colA, valA);
    }
#ifdef CUSPARSE_COO_AOS
    assert(!colA);
    return gpu::CreateCooAoSOp::create(builder, loc, handleTp, tokenTp, token,
                                       sz1, sz2, nseA, rowA, valA);
#else
    llvm_unreachable("gpu::CreateCooAoSOp is deprecated");
#endif
  }
  assert(colA);
  if (format == CuSparseFormat::kCSR)
    return gpu::CreateCsrOp::create(builder, loc, handleTp, tokenTp, token, sz1,
                                    sz2, nseA, rowA, colA, valA);
  if (format == CuSparseFormat::kCSC)
    return gpu::CreateCscOp::create(builder, loc, handleTp, tokenTp, token, sz1,
                                    sz2, nseA, rowA, colA, valA);
  // BSR requires a bit more work since we need to pass in the block size
  // and all others sizes in terms of blocks (#block-rows, #block-cols,
  // #nonzero-blocks).
  assert(format == CuSparseFormat::kBSR);
  SmallVector<unsigned> dims = getBlockSize(aTp.getDimToLvl());
  assert(dims.size() == 2 && dims[0] == dims[1]);
  uint64_t b = dims[0];
  Value bSz = constantIndex(builder, loc, b);
  Value bRows = arith::DivUIOp::create(builder, loc, sz1, bSz);
  Value bCols = arith::DivUIOp::create(builder, loc, sz2, bSz);
  Value bNum = arith::DivUIOp::create(builder, loc, nseA,
                                      constantIndex(builder, loc, b * b));
  return gpu::CreateBsrOp::create(builder, loc, handleTp, tokenTp, token, bRows,
                                  bCols, bNum, bSz, bSz, rowA, colA, valA);
}

/// Match and rewrite SpMV kernel.
static LogicalResult rewriteSpMV(PatternRewriter &rewriter,
                                 linalg::GenericOp op, bool enableRT) {
  Location loc = op.getLoc();
  Value a = op.getOperand(0);
  Value x = op.getOperand(1);
  Value y = op.getOperand(2); // we have y = Ax
  SmallVector<Value> tokens;

  // Only admissible sparse matrix format and dense vectors (no BSR).
  SparseTensorType aTp = getSparseTensorType(a);
  SparseTensorType xTp = getSparseTensorType(x);
  SparseTensorType yTp = getSparseTensorType(y);
  auto format = getCuSparseFormat(aTp, xTp, yTp, enableRT, /*isMatVec=*/true);
  if (format == CuSparseFormat::kNone || format == CuSparseFormat::kBSR)
    return failure();

  // Start sparse kernel and copy data from host to device.
  //   a : memR/memC/memV -> rowA,colA,valA
  //   x : memX           -> vecX
  //   y : memY           -> vecY
  Value nseA = NumberOfEntriesOp::create(rewriter, loc, a);
  Value szY = linalg::createOrFoldDimOp(rewriter, loc, a, 0);
  Value szX = linalg::createOrFoldDimOp(rewriter, loc, a, 1);
  Value memR = genFirstPosOrCrds(rewriter, loc, a, format, enableRT);
  Value memC = genSecondCrds(rewriter, loc, a, format, enableRT); // or empty
  Value memV = ToValuesOp::create(rewriter, loc, a);
  Value rowA = genAllocCopy(rewriter, loc, memR, tokens);
  Value colA = memC ? genAllocCopy(rewriter, loc, memC, tokens) : Value();
  Value valA = genAllocCopy(rewriter, loc, memV, tokens);
  Value memX = genTensorToMemref(rewriter, loc, x);
  Value vecX = genAllocCopy(rewriter, loc, memX, tokens);
  Value memY = genTensorToMemref(rewriter, loc, y);
  Value vecY = genAllocCopy(rewriter, loc, memY, tokens);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Create sparse environment and sparse matrix/dense vector handles.
  Type indexTp = rewriter.getIndexType();
  Type dnTensorHandleTp = rewriter.getType<gpu::SparseDnTensorHandleType>();
  Type spmatHandleTp = rewriter.getType<gpu::SparseSpMatHandleType>();
  Type tokenTp = rewriter.getType<gpu::AsyncTokenType>();
  Value token = genFirstWait(rewriter, loc);
  Operation *spGenA =
      genSpMat(rewriter, loc, aTp, spmatHandleTp, tokenTp, token, szY, szX,
               nseA, rowA, colA, valA, format, enableRT);
  Value spMatA = spGenA->getResult(0);
  token = spGenA->getResult(1);
  auto dvecX = gpu::CreateDnTensorOp::create(rewriter, loc, dnTensorHandleTp,
                                             tokenTp, token, vecX, szX);
  Value dnX = dvecX.getResult(0);
  token = dvecX.getAsyncToken();
  auto dvecY = gpu::CreateDnTensorOp::create(rewriter, loc, dnTensorHandleTp,
                                             tokenTp, token, vecY, szY);
  Value dnY = dvecY.getResult(0);
  token = dvecY.getAsyncToken();
  auto dnYType = llvm::cast<ShapedType>(y.getType()).getElementType();

  // Precompute buffersize for SpMV.
  auto bufferComp = gpu::SpMVBufferSizeOp::create(
      rewriter, loc, indexTp, tokenTp, token, spMatA, dnX, dnY,
      /*computeType=*/dnYType);
  Value bufferSz = bufferComp.getResult(0);
  token = bufferComp.getAsyncToken();
  auto buf = genAllocBuffer(rewriter, loc, bufferSz, token);
  Value buffer = buf.getResult(0);
  token = buf.getAsyncToken();

  // Perform the SpMV.
  auto spmvComp =
      gpu::SpMVOp::create(rewriter, loc, tokenTp, token, spMatA, dnX, dnY,
                          /*computeType=*/dnYType, buffer);
  token = spmvComp.getAsyncToken();

  // Copy data back to host and free all the resoures.
  token = gpu::DestroySpMatOp::create(rewriter, loc, tokenTp, token, spMatA)
              .getAsyncToken();
  token = gpu::DestroyDnTensorOp::create(rewriter, loc, tokenTp, token, dnX)
              .getAsyncToken();
  token = gpu::DestroyDnTensorOp::create(rewriter, loc, tokenTp, token, dnY)
              .getAsyncToken();
  token = genDeallocMemRef(rewriter, loc, rowA, token);
  if (colA)
    token = genDeallocMemRef(rewriter, loc, colA, token);
  token = genDeallocMemRef(rewriter, loc, valA, token);
  token = genDeallocMemRef(rewriter, loc, buffer, token);
  token = genDeallocMemRef(rewriter, loc, vecX, token);
  token = genCopyMemRef(rewriter, loc, memY, vecY, token);
  token = genDeallocMemRef(rewriter, loc, vecY, token);
  tokens.push_back(token);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Done.
  rewriter.replaceOpWithNewOp<bufferization::ToTensorOp>(op, y.getType(), memY);
  return success();
}

/// Match and rewrite SpMM kernel.
static LogicalResult rewriteSpMM(PatternRewriter &rewriter,
                                 linalg::GenericOp op, bool enableRT) {
  Location loc = op.getLoc();
  Value a = op.getOperand(0);
  Value b = op.getOperand(1);
  Value c = op.getOperand(2); // we have C = AB
  SmallVector<Value> tokens;

  // Only admissible sparse matrix format and dense matrices (no BSR).
  SparseTensorType aTp = getSparseTensorType(a);
  SparseTensorType bTp = getSparseTensorType(b);
  SparseTensorType cTp = getSparseTensorType(c);
  auto format = getCuSparseFormat(aTp, bTp, cTp, enableRT, /*isMatVec=*/false);
  if (format == CuSparseFormat::kNone || format == CuSparseFormat::kBSR)
    return failure();

  // Start sparse kernel and copy data from host to device.
  //   a : memR/memC/memV -> rowA,colA,valA
  //   b : bufB           -> matB
  //   c : bufC           -> matC
  Value nseA = NumberOfEntriesOp::create(rewriter, loc, a);
  Value szm = linalg::createOrFoldDimOp(rewriter, loc, a, 0);
  Value szk = linalg::createOrFoldDimOp(rewriter, loc, a, 1);
  Value szn = linalg::createOrFoldDimOp(rewriter, loc, b, 1);
  Value memR = genFirstPosOrCrds(rewriter, loc, a, format, enableRT);
  Value memC = genSecondCrds(rewriter, loc, a, format, enableRT); // or empty
  Value memV = ToValuesOp::create(rewriter, loc, a);
  Value rowA = genAllocCopy(rewriter, loc, memR, tokens);
  Value colA = memC ? genAllocCopy(rewriter, loc, memC, tokens) : Value();
  Value valA = genAllocCopy(rewriter, loc, memV, tokens);
  Value bufB = genTensorToMemref(rewriter, loc, b);
  Value matB = genAllocCopy(rewriter, loc, bufB, tokens);
  Value bufC = genTensorToMemref(rewriter, loc, c);
  Value matC = genAllocCopy(rewriter, loc, bufC, tokens);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Create sparse environment and sparse matrix/dense matrix handles.
  Type indexTp = rewriter.getIndexType();
  Type dnTensorHandleTp = rewriter.getType<gpu::SparseDnTensorHandleType>();
  Type spMatHandleTp = rewriter.getType<gpu::SparseSpMatHandleType>();
  Type tokenTp = rewriter.getType<gpu::AsyncTokenType>();
  Value token = genFirstWait(rewriter, loc);
  Operation *spGenA =
      genSpMat(rewriter, loc, aTp, spMatHandleTp, tokenTp, token, szm, szk,
               nseA, rowA, colA, valA, format, enableRT);
  Value spMatA = spGenA->getResult(0);
  token = spGenA->getResult(1);
  auto dmatB =
      gpu::CreateDnTensorOp::create(rewriter, loc, dnTensorHandleTp, tokenTp,
                                    token, matB, SmallVector<Value>{szk, szn});
  Value dnB = dmatB.getResult(0);
  token = dmatB.getAsyncToken();
  auto dmatC =
      gpu::CreateDnTensorOp::create(rewriter, loc, dnTensorHandleTp, tokenTp,
                                    token, matC, SmallVector<Value>{szm, szn});
  Value dnC = dmatC.getResult(0);
  token = dmatC.getAsyncToken();
  auto dmatCType = llvm::cast<ShapedType>(c.getType()).getElementType();

  // Precompute buffersize for SpMM.
  auto bufferComp = gpu::SpMMBufferSizeOp::create(
      rewriter, loc, indexTp, tokenTp, token, spMatA, dnB, dnC,
      /*computeType=*/dmatCType);
  Value bufferSz = bufferComp.getResult(0);
  token = bufferComp.getAsyncToken();
  auto buf = genAllocBuffer(rewriter, loc, bufferSz, token);
  Value buffer = buf.getResult(0);
  token = buf.getAsyncToken();
  auto dnCType = llvm::cast<ShapedType>(c.getType()).getElementType();

  // Perform the SpMM.
  auto spmmComp =
      gpu::SpMMOp::create(rewriter, loc, tokenTp, token, spMatA, dnB, dnC,
                          /*computeType=*/dnCType, buffer);
  token = spmmComp.getAsyncToken();

  // Copy data back to host and free all the resoures.
  token = gpu::DestroySpMatOp::create(rewriter, loc, tokenTp, token, spMatA)
              .getAsyncToken();
  token = gpu::DestroyDnTensorOp::create(rewriter, loc, tokenTp, token, dnB)
              .getAsyncToken();
  token = gpu::DestroyDnTensorOp::create(rewriter, loc, tokenTp, token, dnC)
              .getAsyncToken();
  token = genDeallocMemRef(rewriter, loc, rowA, token);
  if (colA)
    token = genDeallocMemRef(rewriter, loc, colA, token);
  token = genDeallocMemRef(rewriter, loc, valA, token);
  token = genDeallocMemRef(rewriter, loc, buffer, token);
  token = genDeallocMemRef(rewriter, loc, matB, token);
  token = genCopyMemRef(rewriter, loc, bufC, matC, token);
  token = genDeallocMemRef(rewriter, loc, matC, token);
  tokens.push_back(token);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Done.
  rewriter.replaceOpWithNewOp<bufferization::ToTensorOp>(op, c.getType(), bufC);
  return success();
}

// Match and rewrite SpGEMM kernel.
static LogicalResult rewriteSpGEMM(PatternRewriter &rewriter,
                                   linalg::GenericOp op, bool enableRT) {
  Location loc = op.getLoc();
  Value a = op.getOperand(0);
  Value b = op.getOperand(1);
  Value c = op.getOperand(2); // we have C = AB
  SmallVector<Value> tokens;

  // Only CSR <- CSR x CSR supported.
  auto format = CuSparseFormat::kCSR;
  SparseTensorType aTp = getSparseTensorType(a);
  SparseTensorType bTp = getSparseTensorType(b);
  SparseTensorType cTp = getSparseTensorType(c);
  if (!isAdmissibleCSR(aTp) || !isAdmissibleCSR(bTp) || !isAdmissibleCSR(cTp))
    return failure();

  // Start sparse kernel and copy data from host to device.
  //   a : amemR/amemC/amemV -> rowA,colA,valA
  //   b : bmemR/bmemC/bmemV -> rowB,colB,valB
  //   c : materializes
  auto dnCType = cTp.getElementType();
  Value nseA = NumberOfEntriesOp::create(rewriter, loc, a);
  Value nseB = NumberOfEntriesOp::create(rewriter, loc, b);
  Value szm = linalg::createOrFoldDimOp(rewriter, loc, a, 0);
  Value szk = linalg::createOrFoldDimOp(rewriter, loc, a, 1);
  Value szn = linalg::createOrFoldDimOp(rewriter, loc, b, 1);
  Value amemR = genFirstPosOrCrds(rewriter, loc, a, format, enableRT);
  Value amemC = genSecondCrds(rewriter, loc, a, format, enableRT); // not empty
  Value amemV = ToValuesOp::create(rewriter, loc, a);
  Value bmemR = genFirstPosOrCrds(rewriter, loc, b, format, enableRT);
  Value bmemC = genSecondCrds(rewriter, loc, b, format, enableRT); // not empty
  Value bmemV = ToValuesOp::create(rewriter, loc, b);
  Value rowA = genAllocCopy(rewriter, loc, amemR, tokens);
  Value colA = genAllocCopy(rewriter, loc, amemC, tokens);
  Value valA = genAllocCopy(rewriter, loc, amemV, tokens);
  Value rowB = genAllocCopy(rewriter, loc, bmemR, tokens);
  Value colB = genAllocCopy(rewriter, loc, bmemC, tokens);
  Value valB = genAllocCopy(rewriter, loc, bmemV, tokens);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Create sparse environment and sparse matrix/dense vector handles.
  Type indexTp = rewriter.getIndexType();
  Type spmatHandleTp = rewriter.getType<gpu::SparseSpMatHandleType>();
  Type descTp = rewriter.getType<gpu::SparseSpGEMMOpHandleType>();
  Type tokenTp = rewriter.getType<gpu::AsyncTokenType>();
  Value token = genFirstWait(rewriter, loc);
  Operation *spGenA =
      genSpMat(rewriter, loc, aTp, spmatHandleTp, tokenTp, token, szm, szk,
               nseA, rowA, colA, valA, format, enableRT);
  Value spMatA = spGenA->getResult(0);
  token = spGenA->getResult(1);
  Operation *spGenB =
      genSpMat(rewriter, loc, bTp, spmatHandleTp, tokenTp, token, szk, szn,
               nseB, rowB, colB, valB, format, enableRT);
  Value spMatB = spGenB->getResult(0);
  token = spGenB->getResult(1);

  // Sparse matrix C materializes (also assumes beta == 0).
  Value zero = constantIndex(rewriter, loc, 0);
  Value one = constantIndex(rewriter, loc, 1);
  Value mplus1 = arith::AddIOp::create(rewriter, loc, szm, one);
  auto e1 = genAllocBuffer(rewriter, loc, cTp.getPosType(), mplus1, token);
  Value rowC = e1.getResult(0);
  token = e1.getAsyncToken();
  auto e2 = genAllocBuffer(rewriter, loc, cTp.getCrdType(), zero, token);
  Value colC = e2.getResult(0); // no free needed
  token = e2.getAsyncToken();
  auto e3 = genAllocBuffer(rewriter, loc, dnCType, zero, token);
  Value valC = e3.getResult(0); // no free needed
  token = e3.getAsyncToken();
  Operation *spGenC =
      genSpMat(rewriter, loc, cTp, spmatHandleTp, tokenTp, token, szm, szn,
               zero, rowC, colC, valC, format, enableRT);
  Value spMatC = spGenC->getResult(0);
  token = spGenC->getResult(1);

  // Precompute buffersizes for SpGEMM.
  Operation *descOp =
      gpu::SpGEMMCreateDescrOp::create(rewriter, loc, descTp, tokenTp, token);
  Value desc = descOp->getResult(0);
  token = descOp->getResult(1);
  Operation *work1 = gpu::SpGEMMWorkEstimationOrComputeOp::create(
      rewriter, loc, indexTp, tokenTp, token, desc,
      gpu::TransposeMode::NON_TRANSPOSE, gpu::TransposeMode::NON_TRANSPOSE,
      spMatA, spMatB, spMatC, dnCType, zero, valC,
      gpu::SpGEMMWorkEstimationOrComputeKind::WORK_ESTIMATION);
  Value bufferSz1 = work1->getResult(0);
  token = work1->getResult(1);
  auto buf1 = genAllocBuffer(rewriter, loc, bufferSz1, token);
  Value buffer1 = buf1.getResult(0);
  token = buf1.getAsyncToken();
  Operation *work2 = gpu::SpGEMMWorkEstimationOrComputeOp::create(
      rewriter, loc, indexTp, tokenTp, token, desc,
      gpu::TransposeMode::NON_TRANSPOSE, gpu::TransposeMode::NON_TRANSPOSE,
      spMatA, spMatB, spMatC, dnCType, bufferSz1, buffer1,
      gpu::SpGEMMWorkEstimationOrComputeKind::WORK_ESTIMATION);
  token = work2->getResult(1);

  // Compute step.
  Operation *compute1 = gpu::SpGEMMWorkEstimationOrComputeOp::create(
      rewriter, loc, indexTp, tokenTp, token, desc,
      gpu::TransposeMode::NON_TRANSPOSE, gpu::TransposeMode::NON_TRANSPOSE,
      spMatA, spMatB, spMatC, dnCType, zero, valC,
      gpu::SpGEMMWorkEstimationOrComputeKind::COMPUTE);
  Value bufferSz2 = compute1->getResult(0);
  token = compute1->getResult(1);
  auto buf2 = genAllocBuffer(rewriter, loc, bufferSz2, token);
  Value buffer2 = buf2.getResult(0);
  token = buf2.getAsyncToken();
  Operation *compute2 = gpu::SpGEMMWorkEstimationOrComputeOp::create(
      rewriter, loc, indexTp, tokenTp, token, desc,
      gpu::TransposeMode::NON_TRANSPOSE, gpu::TransposeMode::NON_TRANSPOSE,
      spMatA, spMatB, spMatC, dnCType, bufferSz2, buffer2,
      gpu::SpGEMMWorkEstimationOrComputeKind::COMPUTE);
  token = compute2->getResult(1);

  // Get sizes.
  Operation *sizes = gpu::SpMatGetSizeOp::create(
      rewriter, loc, indexTp, indexTp, indexTp, tokenTp, token, spMatC);
  Value nnz = sizes->getResult(2);
  token = sizes->getResult(3);
  auto a2 = genAllocBuffer(rewriter, loc, cTp.getCrdType(), nnz, token);
  colC = a2.getResult(0);
  token = a2.getAsyncToken();
  auto a3 = genAllocBuffer(rewriter, loc, dnCType, nnz, token);
  valC = a3.getResult(0);
  token = a3.getAsyncToken();

  // Update C with new pointers and copy final product back into C.
  Operation *update = gpu::SetCsrPointersOp::create(
      rewriter, loc, tokenTp, token, spMatC, rowC, colC, valC);
  token = update->getResult(0);
  Operation *copy = gpu::SpGEMMCopyOp::create(
      rewriter, loc, tokenTp, token, desc, gpu::TransposeMode::NON_TRANSPOSE,
      gpu::TransposeMode::NON_TRANSPOSE, spMatA, spMatB, spMatC, dnCType);
  token = copy->getResult(0);

  // Allocate buffers on host.
  Value rowH = genHostBuffer(rewriter, loc, cTp.getPosType(), mplus1);
  Value colH = genHostBuffer(rewriter, loc, cTp.getCrdType(), nnz);
  Value valH = genHostBuffer(rewriter, loc, dnCType, nnz);

  // Copy data back to host and free all the resoures.
  token = gpu::SpGEMMDestroyDescrOp::create(rewriter, loc, tokenTp, token, desc)
              .getAsyncToken();
  token = gpu::DestroySpMatOp::create(rewriter, loc, tokenTp, token, spMatA)
              .getAsyncToken();
  token = gpu::DestroySpMatOp::create(rewriter, loc, tokenTp, token, spMatB)
              .getAsyncToken();
  token = gpu::DestroySpMatOp::create(rewriter, loc, tokenTp, token, spMatC)
              .getAsyncToken();
  token = genCopyMemRef(rewriter, loc, rowH, rowC, token);
  token = genCopyMemRef(rewriter, loc, colH, colC, token);
  token = genCopyMemRef(rewriter, loc, valH, valC, token);
  token = genDeallocMemRef(rewriter, loc, rowA, token);
  token = genDeallocMemRef(rewriter, loc, colA, token);
  token = genDeallocMemRef(rewriter, loc, valA, token);
  token = genDeallocMemRef(rewriter, loc, rowB, token);
  token = genDeallocMemRef(rewriter, loc, colB, token);
  token = genDeallocMemRef(rewriter, loc, valB, token);
  token = genDeallocMemRef(rewriter, loc, rowC, token);
  token = genDeallocMemRef(rewriter, loc, colC, token);
  token = genDeallocMemRef(rewriter, loc, valC, token);
  token = genDeallocMemRef(rewriter, loc, buffer1, token);
  token = genDeallocMemRef(rewriter, loc, buffer2, token);
  tokens.push_back(token);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Done.
  Value vt = bufferization::ToTensorOp::create(
      rewriter, loc, memref::getTensorTypeFromMemRefType(valH.getType()), valH);
  Value rt = bufferization::ToTensorOp::create(
      rewriter, loc, memref::getTensorTypeFromMemRefType(rowH.getType()), rowH);
  Value ct = bufferization::ToTensorOp::create(
      rewriter, loc, memref::getTensorTypeFromMemRefType(colH.getType()), colH);
  rewriter.replaceOpWithNewOp<AssembleOp>(op, c.getType(), ValueRange{rt, ct},
                                          vt);
  return success();
}

// Match and rewrite 2:4 SpMM kernel.
static LogicalResult rewrite2To4SpMM(PatternRewriter &rewriter,
                                     linalg::GenericOp op) {
  Location loc = op.getLoc();
  Value A = op.getOperand(0);
  Value B = op.getOperand(1);
  Value C = op.getOperand(2); // we have C = AB
  SmallVector<Value> tokens;

  // The cuSparselt API currently only allows pruning and compression
  // to occur on the device. So we recognize the pattern
  //    A' = convert A  ; dense to 2:4
  //    C  = A'B        ; 2:4 matrix mult
  // and then perform compression and matrix multiplication on device.
  auto cnv = A.getDefiningOp<ConvertOp>();
  assert(cnv);
  A = cnv.getSource();

  // All input should be dense tensors.
  if (!isDenseTensor(A) || !isDenseTensor(B) || !isDenseTensor(C))
    return failure();

  // Start sparse kernel and copy data from host to device.
  //   a : bufA -> matA
  //   b : bufB -> matB
  //   c : bufC -> matC
  Value bufA = genTensorToMemref(rewriter, loc, A);
  Value matA = genAllocCopy(rewriter, loc, bufA, tokens);
  Value bufB = genTensorToMemref(rewriter, loc, B);
  Value matB = genAllocCopy(rewriter, loc, bufB, tokens);
  Value bufC = genTensorToMemref(rewriter, loc, C);
  Value matC = genAllocCopy(rewriter, loc, bufC, tokens);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Create sparse environment and sparse matrix/dense vector handles.
  Value szm = linalg::createOrFoldDimOp(rewriter, loc, matA, 0);
  Value szk = linalg::createOrFoldDimOp(rewriter, loc, matB, 0);
  Value szn = linalg::createOrFoldDimOp(rewriter, loc, matC, 1);
  Type indexTp = rewriter.getIndexType();
  Type dnTensorHandleTp = rewriter.getType<gpu::SparseDnTensorHandleType>();
  Type spMatHandleTp = rewriter.getType<gpu::SparseSpMatHandleType>();
  Type tokenTp = rewriter.getType<gpu::AsyncTokenType>();
  Value token = genFirstWait(rewriter, loc);
  Operation *spGenA = gpu::Create2To4SpMatOp::create(
      rewriter, loc, spMatHandleTp, tokenTp, token, szm, szk,
      gpu::Prune2To4SpMatFlag::PRUNE_AND_CHECK, matA);
  Value spMatA = spGenA->getResult(0);
  token = spGenA->getResult(1);
  auto dmatB =
      gpu::CreateDnTensorOp::create(rewriter, loc, dnTensorHandleTp, tokenTp,
                                    token, matB, SmallVector<Value>{szk, szn});
  Value dnB = dmatB.getResult(0);
  token = dmatB.getAsyncToken();
  auto dmatC =
      gpu::CreateDnTensorOp::create(rewriter, loc, dnTensorHandleTp, tokenTp,
                                    token, matC, SmallVector<Value>{szm, szn});
  Value dnC = dmatC.getResult(0);
  token = dmatC.getAsyncToken();
  auto dmatCType = llvm::cast<ShapedType>(matC.getType()).getElementType();

  // Precompute buffersize for SpMM.
  SmallVector<Type> bufferTypes_{indexTp, indexTp, indexTp};
  TypeRange bufferTypes(bufferTypes_);
  auto bufferComp = gpu::SpMMBufferSizeOp::create(
      rewriter, loc, bufferTypes, tokenTp, token,
      gpu::TransposeMode::NON_TRANSPOSE, gpu::TransposeMode::NON_TRANSPOSE,
      spMatA, dnB, dnC,
      /*computeType=*/dmatCType);
  token = bufferComp.getAsyncToken();

  // Allocate buffers on host.
  Value bufferSz1 = bufferComp.getResult(0);
  auto buf1 = genAllocBuffer(rewriter, loc, bufferSz1, token);
  Value buffer1 = buf1.getResult(0);
  token = buf1.getAsyncToken();
  Value bufferSz2 = bufferComp.getResult(1);
  auto buf2 = genAllocBuffer(rewriter, loc, bufferSz2, token);
  Value buffer2 = buf2.getResult(0);
  token = buf2.getAsyncToken();
  Value bufferSz3 = bufferComp.getResult(2);
  auto buf3 = genAllocBuffer(rewriter, loc, bufferSz3, token);
  Value buffer3 = buf3.getResult(0);
  token = buf3.getAsyncToken();

  // Perform the SpMM.
  auto dnCType = llvm::cast<ShapedType>(matC.getType()).getElementType();
  auto spmmComp = gpu::SpMMOp::create(
      rewriter, loc, tokenTp, token, spMatA, dnB, dnC, /*computeType=*/dnCType,
      SmallVector<Value>{buffer1, buffer2, buffer3});
  token = spmmComp.getAsyncToken();

  // Copy data back to host and free all the resources.
  token = gpu::DestroySpMatOp::create(rewriter, loc, tokenTp, token, spMatA)
              .getAsyncToken();
  token = gpu::DestroyDnTensorOp::create(rewriter, loc, tokenTp, token, dnB)
              .getAsyncToken();
  token = gpu::DestroyDnTensorOp::create(rewriter, loc, tokenTp, token, dnC)
              .getAsyncToken();
  token = genDeallocMemRef(rewriter, loc, buffer1, token);
  token = genDeallocMemRef(rewriter, loc, buffer2, token);
  token = genDeallocMemRef(rewriter, loc, buffer3, token);
  token = genDeallocMemRef(rewriter, loc, matA, token);
  token = genDeallocMemRef(rewriter, loc, matB, token);
  token = genCopyMemRef(rewriter, loc, bufC, matC, token);
  token = genDeallocMemRef(rewriter, loc, matC, token);
  tokens.push_back(token);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Done.
  rewriter.replaceOpWithNewOp<bufferization::ToTensorOp>(op, C.getType(), bufC);
  return success();
}

/// Match and rewrite SDDMM kernel.
static LogicalResult rewriteSDDMM(PatternRewriter &rewriter,
                                  linalg::GenericOp op, bool enableRT) {
  Location loc = op.getLoc();
  Value a = op.getOperand(0);
  Value b = op.getOperand(1);
  Value c = op.getOperand(2);
  SmallVector<Value> tokens;

  // Only admissible sparse matrix format (no COO/CSC) and dense matrices.
  SparseTensorType aTp = getSparseTensorType(a);
  SparseTensorType bTp = getSparseTensorType(b);
  SparseTensorType cTp = getSparseTensorType(c);
  auto format = getCuSparseFormat(cTp, bTp, aTp, enableRT, /*isMatVec=*/false);
  if (format == CuSparseFormat::kNone || format == CuSparseFormat::kCOO ||
      format == CuSparseFormat::kCSC)
    return failure();

  // The SDDMM does the in-place operation.
  // Start sparse kernel and copy data from host to device.
  //   a : bufA           -> matA
  //   b : bufB           -> matB
  //   c : memR/memC/memV -> rowC,colC,valC
  Value nseC = NumberOfEntriesOp::create(rewriter, loc, c);
  Value szm = linalg::createOrFoldDimOp(rewriter, loc, a, 0);
  Value szk = linalg::createOrFoldDimOp(rewriter, loc, a, 1);
  Value szn = linalg::createOrFoldDimOp(rewriter, loc, b, 1);
  Value bufA = genTensorToMemref(rewriter, loc, a);
  Value matA = genAllocCopy(rewriter, loc, bufA, tokens);
  Value bufB = genTensorToMemref(rewriter, loc, b);
  Value matB = genAllocCopy(rewriter, loc, bufB, tokens);
  Value memR = genFirstPosOrCrds(rewriter, loc, c, format, enableRT);
  Value memC = genSecondCrds(rewriter, loc, c, format, enableRT); // or empty
  Value memV = ToValuesOp::create(rewriter, loc, c);
  Value rowC = genAllocCopy(rewriter, loc, memR, tokens);
  Value colC = memC ? genAllocCopy(rewriter, loc, memC, tokens) : Value();
  Value valC = genAllocCopy(rewriter, loc, memV, tokens);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Create sparse environment and sparse matrix/dense matrix handles.
  Type indexTp = rewriter.getIndexType();
  Type dnMatHandleTp = rewriter.getType<gpu::SparseDnTensorHandleType>();
  Type spMatHandleTp = rewriter.getType<gpu::SparseSpMatHandleType>();
  Type tokenTp = rewriter.getType<gpu::AsyncTokenType>();
  Value token = genFirstWait(rewriter, loc);
  auto dmatA =
      gpu::CreateDnTensorOp::create(rewriter, loc, dnMatHandleTp, tokenTp,
                                    token, matA, SmallVector<Value>{szm, szk});
  Value dnA = dmatA.getResult(0);
  token = dmatA.getAsyncToken();
  auto dmatB =
      gpu::CreateDnTensorOp::create(rewriter, loc, dnMatHandleTp, tokenTp,
                                    token, matB, SmallVector<Value>{szk, szn});
  Value dnB = dmatB.getResult(0);
  token = dmatB.getAsyncToken();
  Operation *spGenC =
      genSpMat(rewriter, loc, cTp, spMatHandleTp, tokenTp, token, szm, szn,
               nseC, rowC, colC, valC, format, enableRT);
  Value spMatC = spGenC->getResult(0);
  token = spGenC->getResult(1);
  auto dnCType = llvm::cast<ShapedType>(c.getType()).getElementType();

  // Precompute buffersize for SDDMM.
  auto bufferComp = gpu::SDDMMBufferSizeOp::create(
      rewriter, loc, indexTp, tokenTp, token, dnA, dnB, spMatC, dnCType);
  Value bufferSz = bufferComp.getResult(0);
  token = bufferComp.getAsyncToken();
  auto buf = genAllocBuffer(rewriter, loc, bufferSz, token);
  Value buffer = buf.getResult(0);
  token = buf.getAsyncToken();

  // Perform the SDDMM.
  auto sddmmComp = gpu::SDDMMOp::create(rewriter, loc, tokenTp, token, dnA, dnB,
                                        spMatC, dnCType, buffer);
  token = sddmmComp.getAsyncToken();

  // Copy data back to host and free all the resoures.
  token = gpu::DestroyDnTensorOp::create(rewriter, loc, tokenTp, token, dnA)
              .getAsyncToken();
  token = gpu::DestroyDnTensorOp::create(rewriter, loc, tokenTp, token, dnB)
              .getAsyncToken();
  token = gpu::DestroySpMatOp::create(rewriter, loc, tokenTp, token, spMatC)
              .getAsyncToken();
  token = genDeallocMemRef(rewriter, loc, buffer, token);
  token = genDeallocMemRef(rewriter, loc, matA, token);
  token = genDeallocMemRef(rewriter, loc, matB, token);
  token = genDeallocMemRef(rewriter, loc, rowC, token);
  if (colC)
    token = genDeallocMemRef(rewriter, loc, colC, token);
  token = genCopyMemRef(rewriter, loc, memV, valC, token);
  token = genDeallocMemRef(rewriter, loc, valC, token);
  tokens.push_back(token);
  genBlockingWait(rewriter, loc, tokens);
  tokens.clear();

  // Done.
  rewriter.replaceOpWithNewOp<sparse_tensor::LoadOp>(op, c);
  return success();
}

//===----------------------------------------------------------------------===//
// Rewriting rules for direct code generation.
//===----------------------------------------------------------------------===//

/// Proof-of-concept rewriter. This rule generates a GPU implementation
/// for each outermost forall loop generated by the sparsifier.
/// TODO: right now works with parallelization-strategy=dense-outer-loop
///       but give this its own flags in the future
struct ForallRewriter : public OpRewritePattern<scf::ParallelOp> {
  using OpRewritePattern<scf::ParallelOp>::OpRewritePattern;

  ForallRewriter(MLIRContext *context, unsigned nT)
      : OpRewritePattern(context), numThreads(nT) {};

  LogicalResult matchAndRewrite(scf::ParallelOp forallOp,
                                PatternRewriter &rewriter) const override {
    // Reject inadmissible loop form.
    // Essentially only accept a loop, generated by the sparsifier,
    // of the form
    //   forall (i = 0; i < N; i++)
    // so that cyclic scheduling over the threads is easy.
    if (!forallOp->hasAttr(LoopEmitter::getLoopEmitterLoopAttrName()) ||
        forallOp.getNumReductions() != 0 || forallOp.getNumLoops() != 1 ||
        !matchPattern(forallOp.getLowerBound()[0], m_Zero()) ||
        !matchPattern(forallOp.getStep()[0], m_One()))
      return failure();
    // Collect every value that is computed outside the parallel loop.
    SetVector<Value> invariants; // stable iteration!
    forallOp->walk([&](Operation *op) {
      // Collect all values of admissible ops.
      for (OpOperand &o : op->getOpOperands()) {
        Value val = o.get();
        Block *block;
        if (auto arg = dyn_cast<BlockArgument>(val))
          block = arg.getOwner();
        else
          block = val.getDefiningOp()->getBlock();
        if (!forallOp.getRegion().findAncestorBlockInRegion(*block))
          invariants.insert(val);
      }
    });
    // Outline the outside values as proper parameters. Fail when sharing
    // value between host and device is not straightforward.
    SmallVector<Value> constants;
    SmallVector<Value> scalars;
    SmallVector<Value> buffers;
    for (Value val : invariants) {
      Type tp = val.getType();
      if (val.getDefiningOp<arith::ConstantOp>())
        constants.push_back(val);
      else if (isa<FloatType>(tp) || tp.isIntOrIndex())
        scalars.push_back(val);
      else if (isa<MemRefType>(tp))
        buffers.push_back(val);
      else
        return failure(); // don't know how to share
    }
    // Pass outlined non-constant values.
    // TODO: Experiment with `useHostRegistrationForOut` to see if we want to
    //       keep the feature at all (either through a heuristic or compiler
    //       option for gpu codegen).
    Location loc = forallOp->getLoc();
    SmallVector<Value> args;
    SmallVector<Value> tokens;
    Value out = genParametersIn(rewriter, loc, scalars, buffers, args, tokens,
                                /*useHostRegistrationForOut=*/false);
    // Set up GPU module and construct GPU function.
    auto saveIp = rewriter.saveInsertionPoint();
    ModuleOp topModule = forallOp->getParentOfType<ModuleOp>();
    auto gpuModule = genGPUModule(rewriter, topModule);
    auto gpuFunc = genGPUFunc(rewriter, gpuModule, args);
    genGPUCode(rewriter, gpuFunc, forallOp, constants, scalars, buffers);
    // Generate code that launches the kernel asynchronously, blocking on all
    // opens tokens and yielding a new token for the output.
    // TODO: Passing in tokens to launch up does not seem to be properly lowered
    //       by cubin yet, hence the current blocking wait.
    rewriter.restoreInsertionPoint(saveIp);
    genBlockingWait(rewriter, loc, tokens);
    tokens.clear();
    Value kernelToken =
        genLaunchGPUFunc(rewriter, gpuFunc, args, tokens, numThreads);
    // Finalize the outlined arguments.
    genParametersOut(rewriter, loc, out, kernelToken, scalars, buffers, args,
                     tokens);
    genBlockingWait(rewriter, loc, tokens);
    rewriter.eraseOp(forallOp);
    return success();
  }

private:
  unsigned numThreads;
};

//===----------------------------------------------------------------------===//
// Rewriting rules for library recognition and code generation.
//===----------------------------------------------------------------------===//

/// Proof-of-concept rewriter. This rule recognizes certain math kernels
/// and replaces these with corresponding calls into a sparse library.
struct LinalgOpRewriter : public OpRewritePattern<linalg::GenericOp> {
  using OpRewritePattern<linalg::GenericOp>::OpRewritePattern;

  LinalgOpRewriter(MLIRContext *context, bool rt)
      : OpRewritePattern(context), enableRT(rt) {}

  LogicalResult matchAndRewrite(linalg::GenericOp op,
                                PatternRewriter &rewriter) const override {
    if (op.getNumDpsInits() != 1)
      return failure(); // reject multi-output

    const unsigned numLoops = op.getNumLoops();
    const unsigned numTensors = op->getNumOperands();
    const auto iteratorTypes = op.getIteratorTypesArray();
    SmallVector<AffineMap, 4> maps = op.getIndexingMapsArray();

    using MapList = ArrayRef<ArrayRef<AffineExpr>>;
    auto infer = [&](MapList m) {
      return AffineMap::inferFromExprList(m, op.getContext());
    };
    AffineExpr i, j, k;
    bindDims(getContext(), i, j, k);

    // TODO: more robust patterns, transposed versions, more kernels,
    //       identify alpha and beta and pass them to the CUDA calls.

    // Recognize a SpMV kernel.
    if (numLoops == 2 && numTensors == 3 &&
        linalg::isParallelIterator(iteratorTypes[0]) &&
        linalg::isReductionIterator(iteratorTypes[1]) &&
        maps == infer({{i, j}, {j}, {i}}) && matchSumOfMultOfArgs(op)) {
      return rewriteSpMV(rewriter, op, enableRT);
    }

    // Recognize a SpGEMM, 2:4-SpMM, or SpMM kernel.
    if (numLoops == 3 && numTensors == 3 &&
        linalg::isParallelIterator(iteratorTypes[0]) &&
        linalg::isParallelIterator(iteratorTypes[1]) &&
        linalg::isReductionIterator(iteratorTypes[2]) &&
        maps == infer({{i, k}, {k, j}, {i, j}}) && matchSumOfMultOfArgs(op)) {
      if (!isDenseTensor(op.getOperand(0)) && !isDenseTensor(op.getOperand(1)))
        return rewriteSpGEMM(rewriter, op, enableRT);
      if (isConversionInto24(op.getOperand(0)))
        return rewrite2To4SpMM(rewriter, op);
      return rewriteSpMM(rewriter, op, enableRT);
    }

    // Recognize a SDDMM kernel.
    if (numLoops == 3 && numTensors == 3 &&
        linalg::isParallelIterator(iteratorTypes[0]) &&
        linalg::isParallelIterator(iteratorTypes[1]) &&
        linalg::isReductionIterator(iteratorTypes[2]) &&
        maps == infer({{i, k}, {k, j}, {i, j}}) &&
        matchSumReductionOfMulUnary(op)) {
      return rewriteSDDMM(rewriter, op, enableRT);
    }

    return failure();
  }

private:
  bool enableRT;
};

} // namespace

//===----------------------------------------------------------------------===//
// Public method for populating GPU rewriting rules.
//
// Currently two set of rewriting rules are made available. The first set
// implements direct code generation, currently by means of convering the
// outermost paralell loop into GPU threads. The second set implements
// libary recognition of a set of sparse operations. Eventually, the right
// combination of these two approaches has to be found.
//===----------------------------------------------------------------------===//

void mlir::populateSparseGPUCodegenPatterns(RewritePatternSet &patterns,
                                            unsigned numThreads) {
  patterns.add<ForallRewriter>(patterns.getContext(), numThreads);
}

void mlir::populateSparseGPULibgenPatterns(RewritePatternSet &patterns,
                                           bool enableRT) {
  patterns.add<LinalgOpRewriter>(patterns.getContext(), enableRT);
}
