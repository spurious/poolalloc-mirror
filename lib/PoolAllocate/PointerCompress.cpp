//===-- PointerCompress.cpp - Pointer Compression Pass --------------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements the -pointercompress pass.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "pointercompress"
#include "EquivClassGraphs.h"
#include "PoolAllocate.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/InstVisitor.h"
#include "llvm/Transforms/Utils/Cloning.h"
using namespace llvm;

/// MEMUINTTYPE - This is the actual type we are compressing to.  This is really
/// only capable of being UIntTy, except when we are doing tests for 16-bit
/// integers, when it's UShortTy.
static const Type *MEMUINTTYPE;

/// SCALARUINTTYPE - We keep scalars the same size as the machine word on the
/// system (e.g. 64-bits), only keeping memory objects in MEMUINTTYPE.
static const Type *SCALARUINTTYPE;

namespace {
  cl::opt<bool>
  SmallIntCompress("compress-to-16-bits",
                   cl::desc("Pointer compress data structures to 16 bit "
                            "integers instead of 32-bit integers"));

  Statistic<> NumCompressed("pointercompress",
                            "Number of pools pointer compressed");
  Statistic<> NumNotCompressed("pointercompress",
                               "Number of pools not compressible");
  Statistic<> NumCloned    ("pointercompress", "Number of functions cloned");

  class CompressedPoolInfo;

  /// FunctionCloneRecord - One of these is kept for each function that is
  /// cloned.
  struct FunctionCloneRecord {
    /// PAFn - The pool allocated input function that we compressed.
    ///
    Function *PAFn;
    FunctionCloneRecord(Function *pafn) : PAFn(pafn) {}

    /// PoolDescriptors - The Value* which defines the pool descriptor for this
    /// DSNode.  Note: Does not necessarily include pool arguments that are
    /// passed in because of indirect function calls that are not used in the
    /// function.
    std::map<const DSNode*, Value*> PoolDescriptors;

    /// NewToOldValueMap - This is a mapping from the values in the cloned body
    /// to the values in PAFn.
    std::map<Value*, const Value*> NewToOldValueMap;

    const Value *getValueInOriginalFunction(Value *V) const {
      std::map<Value*, const Value*>::const_iterator I =
        NewToOldValueMap.find(V);
      if (I == NewToOldValueMap.end()) {
        for (I = NewToOldValueMap.begin(); I != NewToOldValueMap.end(); ++I)
          std::cerr << "MAP: " << *I->first << " TO: " << *I->second << "\n";
      }
      assert (I != NewToOldValueMap.end() && "Value did not come from clone!");
      return I->second;
    }
  };

  /// PointerCompress - This transformation hacks on type-safe pool allocated
  /// data structures to reduce the size of pointers in the program.
  class PointerCompress : public ModulePass {
    PoolAllocate *PoolAlloc;
    PA::EquivClassGraphs *ECG;

    /// ClonedFunctionMap - Every time we clone a function to compress its
    /// arguments, keep track of the clone and which arguments are compressed.
    typedef std::pair<Function*, std::set<const DSNode*> > CloneID;
    std::map<CloneID, Function *> ClonedFunctionMap;

    std::map<std::pair<Function*, std::vector<unsigned> >,
             Function*> ExtCloneFunctionMap;

    /// ClonedFunctionInfoMap - This identifies the pool allocated function that
    /// a clone came from.
    std::map<Function*, FunctionCloneRecord> ClonedFunctionInfoMap;
    
  public:
    Function *PoolInitPC, *PoolDestroyPC;
    typedef std::map<const DSNode*, CompressedPoolInfo> PoolInfoMap;

    bool runOnModule(Module &M);

    void getAnalysisUsage(AnalysisUsage &AU) const;

    PoolAllocate *getPoolAlloc() const { return PoolAlloc; }

    const DSGraph &getGraphForFunc(PA::FuncInfo *FI) const {
      return ECG->getDSGraph(FI->F);
    }

    /// getCloneInfo - If the specified function is a clone, return the
    /// information about the cloning process for it.  Otherwise, return a null
    /// pointer.
    FunctionCloneRecord *getCloneInfo(Function &F) {
      std::map<Function*, FunctionCloneRecord>::iterator I = 
        ClonedFunctionInfoMap.find(&F);
      return I == ClonedFunctionInfoMap.end() ? 0 : &I->second;
    }

    Function *GetFunctionClone(Function *F, 
                               std::set<const DSNode*> &PoolsToCompress,
                               PA::FuncInfo &FI, const DSGraph &CG);
    Function *GetExtFunctionClone(Function *F,
                                  const std::vector<unsigned> &Args);

  private:
    void InitializePoolLibraryFunctions(Module &M);
    bool CompressPoolsInFunction(Function &F,
                std::vector<std::pair<Value*, Value*> > *PremappedVals = 0,
                std::set<const DSNode*> *ExternalPoolsToCompress = 0);

    void FindPoolsToCompress(std::set<const DSNode*> &Pools, Function &F,
                             DSGraph &DSG, PA::FuncInfo *FI);
  };

  RegisterOpt<PointerCompress>
  X("pointercompress", "Compress type-safe data structures");
}

//===----------------------------------------------------------------------===//
//               CompressedPoolInfo Class and Implementation
//===----------------------------------------------------------------------===//

namespace {
  /// CompressedPoolInfo - An instance of this structure is created for each
  /// pool that is compressed.
  class CompressedPoolInfo {
    const DSNode *Pool;
    Value *PoolDesc;
    const Type *NewTy;
    unsigned NewSize;
  public:
    CompressedPoolInfo(const DSNode *N, Value *PD)
      : Pool(N), PoolDesc(PD), NewTy(0) {}
    
    /// Initialize - When we know all of the pools in a function that are going
    /// to be compressed, initialize our state based on that data.
    void Initialize(std::map<const DSNode*, CompressedPoolInfo> &Nodes,
                    const TargetData &TD);

    const DSNode *getNode() const { return Pool; }
    const Type *getNewType() const { return NewTy; }

    /// getNewSize - Return the size of each node after compression.
    ///
    unsigned getNewSize() const { return NewSize; }
    
    /// getPoolDesc - Return the Value* for the pool descriptor for this pool.
    ///
    Value *getPoolDesc() const { return PoolDesc; }

    // dump - Emit a debugging dump of this pool info.
    void dump() const;

  private:
    const Type *ComputeCompressedType(const Type *OrigTy, unsigned NodeOffset,
                           std::map<const DSNode*, CompressedPoolInfo> &Nodes);
  };
}

/// Initialize - When we know all of the pools in a function that are going
/// to be compressed, initialize our state based on that data.
void CompressedPoolInfo::Initialize(std::map<const DSNode*, 
                                             CompressedPoolInfo> &Nodes,
                                    const TargetData &TD) {
  // First step, compute the type of the compressed node.  This basically
  // replaces all pointers to compressed pools with uints.
  NewTy = ComputeCompressedType(Pool->getType(), 0, Nodes);

  // Get the compressed type size.
  NewSize = NewTy->isSized() ? TD.getTypeSize(NewTy) : 0;
}


/// ComputeCompressedType - Recursively compute the new type for this node after
/// pointer compression.  This involves compressing any pointers that point into
/// compressed pools.
const Type *CompressedPoolInfo::
ComputeCompressedType(const Type *OrigTy, unsigned NodeOffset,
                      std::map<const DSNode*, CompressedPoolInfo> &Nodes) {
  if (const PointerType *PTY = dyn_cast<PointerType>(OrigTy)) {
    // FIXME: check to see if this pointer is actually compressed!
    return MEMUINTTYPE;
  } else if (OrigTy->isFirstClassType() || OrigTy == Type::VoidTy)
    return OrigTy;

  // Okay, we have an aggregate type.
  if (const StructType *STy = dyn_cast<StructType>(OrigTy)) {
    std::vector<const Type*> Elements;
    Elements.reserve(STy->getNumElements());
    for (unsigned i = 0, e = STy->getNumElements(); i != e; ++i)
      Elements.push_back(ComputeCompressedType(STy->getElementType(i),
                                               NodeOffset, Nodes));
    return StructType::get(Elements);
  } else if (const ArrayType *ATy = dyn_cast<ArrayType>(OrigTy)) {
    return ArrayType::get(ComputeCompressedType(ATy->getElementType(),
                                                NodeOffset, Nodes),
                          ATy->getNumElements());
  } else {
    std::cerr << "TYPE: " << *OrigTy << "\n";
    assert(0 && "FIXME: Unhandled aggregate type!");
  }
}

/// dump - Emit a debugging dump for this pool info.
///
void CompressedPoolInfo::dump() const {
  std::cerr << "Node: "; getNode()->dump();
  std::cerr << "New Type: " << *NewTy << "\n";
}


//===----------------------------------------------------------------------===//
//                    PointerCompress Implementation
//===----------------------------------------------------------------------===//

void PointerCompress::getAnalysisUsage(AnalysisUsage &AU) const {
  // Need information about how pool allocation happened.
  AU.addRequired<PoolAllocatePassAllPools>();

  // Need information from DSA.
  AU.addRequired<PA::EquivClassGraphs>();
}

/// PoolIsCompressible - Return true if we can pointer compress this node.
/// If not, we should DEBUG print out why.
static bool PoolIsCompressible(const DSNode *N, Function &F) {
  assert(!N->isForwarding() && "Should not be dealing with merged nodes!");
  if (N->isNodeCompletelyFolded()) {
    DEBUG(std::cerr << "Node is not type-safe:\n");
    return false;
  }

  // FIXME: If any non-type-safe nodes point to this one, we cannot compress it.
#if 0
  bool HasFields = false;
  for (DSNode::const_edge_iterator I = N->edge_begin(), E = N->edge_end();
       I != E; ++I)
    if (!I->isNull()) {
      HasFields = true;
      if (I->getNode() != N) {
        // We currently only handle trivially self cyclic DS's right now.
        DEBUG(std::cerr << "Node points to nodes other than itself:\n");
        return false;
      }        
    }

  if (!HasFields) {
    DEBUG(std::cerr << "Node does not contain any pointers to compress:\n");
    return false;
  }
#endif

  if (N->isArray()) {
    DEBUG(std::cerr << "Node is an array (not yet handled!):\n");
    return false;
  }

  if ((N->getNodeFlags() & DSNode::Composition) != DSNode::HeapNode) {
    DEBUG(std::cerr << "Node contains non-heap values:\n");
    return false;
  }

  return true;
}

/// FindPoolsToCompress - Inspect the specified function and find pools that are
/// compressible that are homed in that function.  Return those pools in the
/// Pools set.
void PointerCompress::FindPoolsToCompress(std::set<const DSNode*> &Pools,
                                          Function &F, DSGraph &DSG,
                                          PA::FuncInfo *FI) {
  DEBUG(std::cerr << "In function '" << F.getName() << "':\n");
  for (unsigned i = 0, e = FI->NodesToPA.size(); i != e; ++i) {
    const DSNode *N = FI->NodesToPA[i];

    // Ignore potential pools that the pool allocation heuristic decided not to
    // pool allocated.
    if (!isa<ConstantPointerNull>(FI->PoolDescriptors[N]))
      if (PoolIsCompressible(N, F)) {
        Pools.insert(N);
        ++NumCompressed;
      } else {
        DEBUG(std::cerr << "PCF: "; N->dump());
        ++NumNotCompressed;
      }
  }
}


namespace {
  /// InstructionRewriter - This class implements the rewriting neccesary to
  /// transform a function body from normal pool allocation to pointer
  /// compression.  It is constructed, then the 'visit' method is called on a
  /// function.  If is responsible for rewriting all instructions that refer to
  /// pointers into compressed pools.
  class InstructionRewriter : public llvm::InstVisitor<InstructionRewriter> {
    /// OldToNewValueMap - This keeps track of what new instructions we create
    /// for instructions that used to produce pointers into our pool.
    std::map<Value*, Value*> OldToNewValueMap;
  
    const PointerCompress::PoolInfoMap &PoolInfo;

    /// TD - The TargetData object for the current target.
    ///
    const TargetData &TD;


    const DSGraph &DSG;

    /// PAFuncInfo - Information about the transformation the pool allocator did
    /// to the original function.
    PA::FuncInfo &PAFuncInfo;

    /// FCR - If we are compressing a clone of a pool allocated function (as
    /// opposed to the pool allocated function itself), this contains
    /// information about the clone.
    FunctionCloneRecord *FCR;

    PointerCompress &PtrComp;
  public:
    InstructionRewriter(const PointerCompress::PoolInfoMap &poolInfo,
                        const DSGraph &dsg, PA::FuncInfo &pafi,
                        FunctionCloneRecord *fcr, PointerCompress &ptrcomp)
      : PoolInfo(poolInfo), TD(dsg.getTargetData()), DSG(dsg),
        PAFuncInfo(pafi), FCR(fcr), PtrComp(ptrcomp) {
    }

    ~InstructionRewriter();

    /// PremapValues - Seed the transformed value map with the specified values.
    /// This indicates that the first value (a pointer) will map to the second
    /// value (an integer).  When the InstructionRewriter is complete, all of
    /// the pointers in this vector are deleted.
    void PremapValues(std::vector<std::pair<Value*, Value*> > &Vals) {
      for (unsigned i = 0, e = Vals.size(); i != e; ++i)
        OldToNewValueMap.insert(Vals[i]);
    }

    /// getTransformedValue - Return the transformed version of the specified
    /// value, creating a new forward ref value as needed.
    Value *getTransformedValue(Value *V) {
      if (isa<ConstantPointerNull>(V))                // null -> uint 0
        return Constant::getNullValue(SCALARUINTTYPE);
      if (isa<UndefValue>(V))                // undef -> uint undef
        return UndefValue::get(SCALARUINTTYPE);

      assert(getNodeIfCompressed(V) && "Value is not compressed!");
      Value *&RV = OldToNewValueMap[V];
      if (RV) return RV;

      RV = new Argument(SCALARUINTTYPE);
      return RV;
    }

    /// setTransformedValue - When we create a new value, this method sets it as
    /// the current value.
    void setTransformedValue(Instruction &Old, Value *New) {
      Value *&EV = OldToNewValueMap[&Old];
      if (EV) {
        assert(isa<Argument>(EV) && "Not a forward reference!");
        EV->replaceAllUsesWith(New);
        delete EV;
      }
      EV = New;
    }

    /// getMappedNodeHandle - Given a pointer value that may be cloned multiple
    /// times (once for PA, once for PC) return the node handle in DSG, or a
    /// null descriptor if the value didn't exist.
    DSNodeHandle getMappedNodeHandle(Value *V) {
      assert(isa<PointerType>(V->getType()) && "Not a pointer value!");

      // If this is a function clone, map the value to the original function.
      if (FCR)
        V = const_cast<Value*>(FCR->getValueInOriginalFunction(V));

      // If this is a pool allocator clone, map the value to the REAL original
      // function.
      if (!PAFuncInfo.NewToOldValueMap.empty())
        if ((V = PAFuncInfo.MapValueToOriginal(V)) == 0)
          // Value didn't exist in the orig program (pool desc?).
          return DSNodeHandle();

      return DSG.getNodeForValue(V);
    }

    /// getNodeIfCompressed - If the specified value is a pointer that will be
    /// compressed, return the DSNode corresponding to the pool it belongs to.
    const DSNode *getNodeIfCompressed(Value *V) {
      if (!isa<PointerType>(V->getType()) || isa<ConstantPointerNull>(V) ||
          isa<Function>(V))
        return false;

      DSNode *N = getMappedNodeHandle(V).getNode();
      return PoolInfo.count(N) ? N : 0;
    }

    /// getPoolInfo - Return the pool info for the specified compressed pool.
    ///
    const CompressedPoolInfo &getPoolInfo(const DSNode *N) {
      assert(N && "Pool not compressed!");
      PointerCompress::PoolInfoMap::const_iterator I = PoolInfo.find(N);
      assert(I != PoolInfo.end() && "Pool is not compressed!");
      return I->second;
    }

    /// getPoolInfo - Return the pool info object for the specified value if the
    /// pointer points into a compressed pool, otherwise return null.
    const CompressedPoolInfo *getPoolInfo(Value *V) {
      if (const DSNode *N = getNodeIfCompressed(V))
        return &getPoolInfo(N);
      return 0;
    }

    /// getPoolInfoForPoolDesc - Given a pool descriptor as a Value*, return the
    /// pool info for the pool if it is compressed.
    const CompressedPoolInfo *getPoolInfoForPoolDesc(Value *PD) const {
      for (PointerCompress::PoolInfoMap::const_iterator I = PoolInfo.begin(),
             E = PoolInfo.end(); I != E; ++I)
        if (I->second.getPoolDesc() == PD)
          return &I->second;
      return 0;
    }

    /// ValueRemoved - Whenever we remove a value from the current function,
    /// update any maps that contain that pointer so we don't have stale
    /// pointers hanging around.
    void ValueRemoved(Value *V) {
      if (FCR) FCR->NewToOldValueMap.erase(V);
    }

    /// ValueReplaced - Whenever we replace a value from the current function,
    /// update any maps that contain that pointer so we don't have stale
    /// pointers hanging around.
    void ValueReplaced(Value &Old, Value *New) {
      if (FCR) {
        std::map<Value*, const Value*>::iterator I =
          FCR->NewToOldValueMap.find(&Old);
        assert(I != FCR->NewToOldValueMap.end() && "Didn't find element!?");
        FCR->NewToOldValueMap.insert(std::make_pair(New, I->second));
        FCR->NewToOldValueMap.erase(I);       
      }
    }

    //===------------------------------------------------------------------===//
    // Visitation methods.  These do all of the heavy lifting for the various
    // cases we have to handle.

    void visitReturnInst(ReturnInst &RI);
    void visitCastInst(CastInst &CI);
    void visitPHINode(PHINode &PN);
    void visitSelectInst(SelectInst &SI);
    void visitSetCondInst(SetCondInst &SCI);
    void visitGetElementPtrInst(GetElementPtrInst &GEPI);
    void visitLoadInst(LoadInst &LI);
    void visitStoreInst(StoreInst &SI);

    void visitCallInst(CallInst &CI);
    void visitPoolInit(CallInst &CI);
    void visitPoolDestroy(CallInst &CI);

    void visitInstruction(Instruction &I) {
#ifndef NDEBUG
      bool Unhandled = !!getNodeIfCompressed(&I);
      for (unsigned i = 0, e = I.getNumOperands(); i != e; ++i)
        Unhandled |= !!getNodeIfCompressed(I.getOperand(i));

      if (Unhandled) {
        std::cerr << "ERROR: UNHANDLED INSTRUCTION: " << I;
        //assert(0);
        //abort();
      }
#endif
    }
  };
} // end anonymous namespace.


InstructionRewriter::~InstructionRewriter() {
  // Nuke all of the old values from the program.
  for (std::map<Value*, Value*>::iterator I = OldToNewValueMap.begin(),
         E = OldToNewValueMap.end(); I != E; ++I) {
    assert((!isa<Argument>(I->second) || cast<Argument>(I->second)->getParent())
           && "ERROR: Unresolved value still left in the program!");
    // If there is anything still using this, provide a temporary value.
    if (!I->first->use_empty())
      I->first->replaceAllUsesWith(UndefValue::get(I->first->getType()));

    // Finally, remove it from the program.
    if (Instruction *Inst = dyn_cast<Instruction>(I->first)) {
      Inst->eraseFromParent();
      ValueRemoved(Inst);
    } else if (Argument *Arg = dyn_cast<Argument>(I->first)) {
      assert(Arg->getParent() == 0 && "Unexpected argument type here!");
      delete Arg;  // Marker node used when cloning.
    } else {
      assert(0 && "Unknown entry in this map!");
    }
  }
}

void InstructionRewriter::visitReturnInst(ReturnInst &RI) {
  if (RI.getNumOperands() && isa<PointerType>(RI.getOperand(0)->getType()))
    if (!isa<PointerType>(RI.getParent()->getParent()->getReturnType())) {
      // Compressing the return value.  
      new ReturnInst(getTransformedValue(RI.getOperand(0)), &RI);
      RI.eraseFromParent();
    }
}


void InstructionRewriter::visitCastInst(CastInst &CI) {
  if (!isa<PointerType>(CI.getType())) {
    // If this is a pointer -> integer cast, turn this into an idx -> integer
    // cast.
    if (isa<PointerType>(CI.getOperand(0)->getType()) &&
        getPoolInfo(CI.getOperand(0)))
      CI.setOperand(0, getTransformedValue(CI.getOperand(0)));
    return;
  }

  const CompressedPoolInfo *PI = getPoolInfo(&CI);
  if (!PI) return;
  assert(getPoolInfo(CI.getOperand(0)) == PI && "Not cast from ptr -> ptr?");

  // A cast from one pointer to another turns into a cast from uint -> uint,
  // which is a noop.
  setTransformedValue(CI, getTransformedValue(CI.getOperand(0)));
}

void InstructionRewriter::visitPHINode(PHINode &PN) {
  const CompressedPoolInfo *DestPI = getPoolInfo(&PN);
  if (DestPI == 0) return;

  PHINode *New = new PHINode(SCALARUINTTYPE, PN.getName(), &PN);
  New->reserveOperandSpace(PN.getNumIncomingValues());

  for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i)
    New->addIncoming(getTransformedValue(PN.getIncomingValue(i)),
                     PN.getIncomingBlock(i));
  setTransformedValue(PN, New);
}

void InstructionRewriter::visitSelectInst(SelectInst &SI) {
  const CompressedPoolInfo *DestPI = getPoolInfo(&SI);
  if (DestPI == 0) return;

  setTransformedValue(SI, new SelectInst(SI.getOperand(0),
                                         getTransformedValue(SI.getOperand(1)),
                                         getTransformedValue(SI.getOperand(2)),
                                         SI.getName(), &SI));
}

void InstructionRewriter::visitSetCondInst(SetCondInst &SCI) {
  if (!isa<PointerType>(SCI.getOperand(0)->getType())) return;
  Value *NonNullPtr = SCI.getOperand(0);
  if (isa<ConstantPointerNull>(NonNullPtr)) {
    NonNullPtr = SCI.getOperand(1);
    if (isa<ConstantPointerNull>(NonNullPtr))
      return;  // setcc null, null
  }

  const CompressedPoolInfo *SrcPI = getPoolInfo(NonNullPtr);
  if (SrcPI == 0) return;   // comparing non-compressed pointers.
 
  std::string Name = SCI.getName(); SCI.setName("");
  Value *New = new SetCondInst(SCI.getOpcode(),
                               getTransformedValue(SCI.getOperand(0)),
                               getTransformedValue(SCI.getOperand(1)),
                               Name, &SCI);
  SCI.replaceAllUsesWith(New);
  ValueReplaced(SCI, New);
  SCI.eraseFromParent();
}

void InstructionRewriter::visitGetElementPtrInst(GetElementPtrInst &GEPI) {
  const CompressedPoolInfo *PI = getPoolInfo(&GEPI);
  if (PI == 0) return;

  // For now, we only support very very simple getelementptr instructions, with
  // two indices, where the first is zero.
  assert(GEPI.getNumOperands() == 3 && isa<Constant>(GEPI.getOperand(1)) &&
         cast<Constant>(GEPI.getOperand(1))->isNullValue());
  const Type *IdxTy = 
    cast<PointerType>(GEPI.getOperand(0)->getType())->getElementType();
  assert(isa<StructType>(IdxTy) && "Can only handle structs right now!");

  Value *Val = getTransformedValue(GEPI.getOperand(0));

  unsigned Field = (unsigned)cast<ConstantUInt>(GEPI.getOperand(2))->getValue();
  if (Field) {
    const StructType *NTy = cast<StructType>(PI->getNewType());
    uint64_t FieldOffs = TD.getStructLayout(NTy)->MemberOffsets[Field];
    Constant *FieldOffsCst = ConstantUInt::get(SCALARUINTTYPE, FieldOffs);
    Val = BinaryOperator::createAdd(Val, FieldOffsCst, GEPI.getName(), &GEPI);
  }

  setTransformedValue(GEPI, Val);
}

void InstructionRewriter::visitLoadInst(LoadInst &LI) {
  if (isa<ConstantPointerNull>(LI.getOperand(0))) { // load null ??
    // Load null doesn't make any sense, but if the result is a pointer into a
    // compressed pool, we have to transform it.
    if (isa<PointerType>(LI.getType()) && getPoolInfo(&LI))
      setTransformedValue(LI, UndefValue::get(SCALARUINTTYPE));
    return;
  }

  const CompressedPoolInfo *SrcPI = getPoolInfo(LI.getOperand(0));
  if (SrcPI == 0) {
    assert(getPoolInfo(&LI) == 0 &&
           "Cannot load a compressed pointer from non-compressed memory!");
    return;
  }

  // We care about two cases, here:
  //  1. Loading a normal value from a ptr compressed data structure.
  //  2. Loading a compressed ptr from a ptr compressed data structure.
  bool LoadingCompressedPtr = getNodeIfCompressed(&LI) != 0;
  
  // Get the pool base pointer.
  Constant *Zero = Constant::getNullValue(Type::UIntTy);
  Value *BasePtrPtr = new GetElementPtrInst(SrcPI->getPoolDesc(), Zero, Zero,
                                            "poolbaseptrptr", &LI);
  Value *BasePtr = new LoadInst(BasePtrPtr, "poolbaseptr", &LI);

  // Get the pointer to load from.
  std::vector<Value*> Ops;
  Ops.push_back(getTransformedValue(LI.getOperand(0)));
  if (Ops[0]->getType() == Type::UShortTy)
    Ops[0] = new CastInst(Ops[0], Type::UIntTy, "extend_idx", &LI);
  Value *SrcPtr = new GetElementPtrInst(BasePtr, Ops,
                                        LI.getOperand(0)->getName()+".pp", &LI);
  const Type *DestTy = LoadingCompressedPtr ? MEMUINTTYPE : LI.getType();
  SrcPtr = new CastInst(SrcPtr, PointerType::get(DestTy),
                        SrcPtr->getName(), &LI);
  std::string OldName = LI.getName(); LI.setName("");
  Value *NewLoad = new LoadInst(SrcPtr, OldName, &LI);

  if (LoadingCompressedPtr) {
    // Convert from MEMUINTTYPE to SCALARUINTTYPE if different.
    if (MEMUINTTYPE != SCALARUINTTYPE)
      NewLoad = new CastInst(NewLoad, SCALARUINTTYPE, NewLoad->getName(), &LI);

    setTransformedValue(LI, NewLoad);
  } else {
    LI.replaceAllUsesWith(NewLoad);
    ValueReplaced(LI, NewLoad);
    LI.eraseFromParent();
  }
}



void InstructionRewriter::visitStoreInst(StoreInst &SI) {
  const CompressedPoolInfo *DestPI = getPoolInfo(SI.getOperand(1));
  if (DestPI == 0) {
    if (isa<ConstantPointerNull>(SI.getOperand(1)))
      SI.eraseFromParent();
    else
      assert(getPoolInfo(SI.getOperand(0)) == 0 &&
             "Cannot store a compressed pointer into non-compressed memory!");
    return;
  }

  // We care about two cases, here:
  //  1. Storing a normal value into a ptr compressed data structure.
  //  2. Storing a compressed ptr into a ptr compressed data structure.  Note
  //     that we cannot use the src value to decide if this is a compressed
  //     pointer if it's a null pointer.  We have to try harder.
  //
  Value *SrcVal = SI.getOperand(0);
  if (!isa<ConstantPointerNull>(SrcVal)) {
    if (const CompressedPoolInfo *SrcPI = getPoolInfo(SrcVal)) {
      // If the stored value is compressed, get the xformed version
      SrcVal = getTransformedValue(SrcVal);

      // If SCALAR type is not the MEM type, reduce it now.
      if (SrcVal->getType() != MEMUINTTYPE)
        SrcVal = new CastInst(SrcVal, MEMUINTTYPE, SrcVal->getName(), &SI);
    }
  } else {
    // FIXME: This assumes that all null pointers are compressed!
    SrcVal = Constant::getNullValue(MEMUINTTYPE);
  }
  
  // Get the pool base pointer.
  Constant *Zero = Constant::getNullValue(Type::UIntTy);
  Value *BasePtrPtr = new GetElementPtrInst(DestPI->getPoolDesc(), Zero, Zero,
                                            "poolbaseptrptr", &SI);
  Value *BasePtr = new LoadInst(BasePtrPtr, "poolbaseptr", &SI);

  // Get the pointer to store to.
  std::vector<Value*> Ops;
  Ops.push_back(getTransformedValue(SI.getOperand(1)));
  if (Ops[0]->getType() == Type::UShortTy)
    Ops[0] = new CastInst(Ops[0], Type::UIntTy, "extend_idx", &SI);

  Value *DestPtr = new GetElementPtrInst(BasePtr, Ops,
                                         SI.getOperand(1)->getName()+".pp",
                                         &SI);
  DestPtr = new CastInst(DestPtr, PointerType::get(SrcVal->getType()),
                         DestPtr->getName(), &SI);
  new StoreInst(SrcVal, DestPtr, &SI);

  // Finally, explicitly remove the store from the program, as it does not
  // produce a pointer result.
  SI.eraseFromParent();
}


void InstructionRewriter::visitPoolInit(CallInst &CI) {
  // Transform to poolinit_pc if this is initializing a pool that we are
  // compressing.
  const CompressedPoolInfo *PI = getPoolInfoForPoolDesc(CI.getOperand(1));
  if (PI == 0) return;  // Pool isn't compressed.

  std::vector<Value*> Ops;
  Ops.push_back(CI.getOperand(1));
  // Transform to pass in the orig and compressed sizes.
  Ops.push_back(CI.getOperand(2));
  Ops.push_back(ConstantUInt::get(Type::UIntTy, PI->getNewSize()));
  Ops.push_back(CI.getOperand(3));
  // TODO: Compression could reduce the alignment restriction for the pool!
  new CallInst(PtrComp.PoolInitPC, Ops, "", &CI);
  CI.eraseFromParent();
}

void InstructionRewriter::visitPoolDestroy(CallInst &CI) {
  // Transform to pooldestroy_pc if this is destroying a pool that we are
  // compressing.
  const CompressedPoolInfo *PI = getPoolInfoForPoolDesc(CI.getOperand(1));
  if (PI == 0) return;  // Pool isn't compressed.

  std::vector<Value*> Ops;
  Ops.push_back(CI.getOperand(1));
  new CallInst(PtrComp.PoolDestroyPC, Ops, "", &CI);
  CI.eraseFromParent();
}

void InstructionRewriter::visitCallInst(CallInst &CI) {
  if (Function *F = CI.getCalledFunction())
    // These functions are handled specially.
    if (F->getName() == "poolinit") {
      visitPoolInit(CI);
      return;
    } else if (F->getName() == "pooldestroy") {
      visitPoolDestroy(CI);
      return;
    }
  
  // Normal function call: check to see if this call produces or uses a pointer
  // into a compressed pool.  If so, we will need to transform the callee or use
  // a previously transformed version.

  // PoolsToCompress - Keep track of which pools we are supposed to compress,
  // with the nodes from the callee's graph.
  std::set<const DSNode*> PoolsToCompress;

  // If this is a direct call, get the information about the callee.
  PA::FuncInfo *FI = 0;
  const DSGraph *CG = 0;
  Function *Callee = CI.getCalledFunction();
  if (Callee)
    if (FI = PtrComp.getPoolAlloc()->getFuncInfoOrClone(*Callee))
      CG = &PtrComp.getGraphForFunc(FI);

  if (!Callee) {
    // Indirect call: you CAN'T passed compress pointers in.  Don't even think
    // about it.
    return;
  } else if (Callee->isExternal()) {
    // We don't have a DSG for the callee in this case.  Assume that things will
    // work out if we pass compressed pointers.
    std::vector<Value*> Operands;
    Operands.reserve(CI.getNumOperands()-1);

    std::vector<unsigned> CompressedArgs;
    if (isa<PointerType>(CI.getType()) && getPoolInfo(&CI))
      CompressedArgs.push_back(0);  // Compress retval.
  
    for (unsigned i = 1, e = CI.getNumOperands(); i != e; ++i)
      if (isa<PointerType>(CI.getOperand(i)->getType()) &&
          getPoolInfo(CI.getOperand(i))) {
        CompressedArgs.push_back(i);
        Operands.push_back(getTransformedValue(CI.getOperand(i)));
      } else {
        Operands.push_back(CI.getOperand(i));
      }

    if (CompressedArgs.empty())
      return;  // Nothing to compress!

    Function *Clone = PtrComp.GetExtFunctionClone(Callee, CompressedArgs);
    Value *NC = new CallInst(Clone, Operands, CI.getName(), &CI);
    if (NC->getType() != CI.getType())      // Compressing return value?
      setTransformedValue(CI, NC);
    else {
      if (CI.getType() != Type::VoidTy) {
        CI.replaceAllUsesWith(NC);
        ValueReplaced(CI, NC);
      }
      CI.eraseFromParent();
    }
    return;
  }

  // CalleeCallerMap: Mapping from nodes in the callee to nodes in the caller.
  DSGraph::NodeMapTy CalleeCallerMap;
  
  // Do we need to compress the return value?
  if (isa<PointerType>(CI.getType()) && getNodeIfCompressed(&CI)) {
    DSGraph::computeNodeMapping(CG->getReturnNodeFor(FI->F),
                                getMappedNodeHandle(&CI), CalleeCallerMap);
    PoolsToCompress.insert(CG->getReturnNodeFor(FI->F).getNode());
  }
    
  // Find the arguments we need to compress.
  unsigned NumPoolArgs = FI ? FI->ArgNodes.size() : 0;
  for (unsigned i = 1, e = CI.getNumOperands(); i != e; ++i)
    if (isa<PointerType>(CI.getOperand(i)->getType()) &&
        getNodeIfCompressed(CI.getOperand(i))) {
      Argument *FormalArg = next(FI->F.abegin(), i-1-NumPoolArgs);
        
      DSGraph::computeNodeMapping(CG->getNodeForValue(FormalArg),
                                  getMappedNodeHandle(CI.getOperand(i)),
                                  CalleeCallerMap);
        
      PoolsToCompress.insert(CG->getNodeForValue(FormalArg).getNode());
    }

  // If this function doesn't require compression, there is nothing to do!
  if (PoolsToCompress.empty()) return;

  // Now that we know the basic pools passed/returned through the
  // argument/retval of the call, add the compressed pools that are reachable
  // from them.  The CalleeCallerMap contains a mapping from callee nodes to the
  // caller nodes they correspond to (a many-to-one mapping).
  for (DSGraph::NodeMapTy::iterator I = CalleeCallerMap.begin(),
         E = CalleeCallerMap.end(); I != E; ++I) {
    // If the destination is compressed, so should the source be.
    if (PoolInfo.count(I->second.getNode()))
      PoolsToCompress.insert(I->first);
  }
    
  // Get the clone of this function that uses compressed pointers instead of
  // normal pointers.
  Function *Clone = PtrComp.GetFunctionClone(Callee, PoolsToCompress,
                                             *FI, *CG);


  // Okay, we now have our clone: rewrite the call instruction.
  std::vector<Value*> Operands;
  Operands.reserve(CI.getNumOperands()-1);

  Function::aiterator AI = FI->F.abegin();
  
  // Pass pool descriptors.
  for (unsigned i = 1; i != NumPoolArgs+1; ++i)
    Operands.push_back(CI.getOperand(i));

  for (unsigned i = NumPoolArgs+1, e = CI.getNumOperands(); i != e; ++i, ++AI)
    if (isa<PointerType>(CI.getOperand(i)->getType()) &&
        PoolsToCompress.count(CG->getNodeForValue(AI).getNode())) {
      Operands.push_back(getTransformedValue(CI.getOperand(i)));
    } else {
      Operands.push_back(CI.getOperand(i));
    }

  Value *NC = new CallInst(Clone, Operands, CI.getName(), &CI);
  if (NC->getType() != CI.getType())      // Compressing return value?
    setTransformedValue(CI, NC);
  else {
    if (CI.getType() != Type::VoidTy) {
      CI.replaceAllUsesWith(NC);
      ValueReplaced(CI, NC);
    }
    CI.eraseFromParent();
  }
}


/// CompressPoolsInFunction - Find all pools that are compressible in this
/// function and compress them.
bool PointerCompress::
CompressPoolsInFunction(Function &F,
                        std::vector<std::pair<Value*, Value*> > *PremappedVals,
                        std::set<const DSNode*> *ExternalPoolsToCompress){
  if (F.isExternal()) return false;

  // If this is a pointer compressed clone of a pool allocated function, get the
  // the pool allocated function.  Rewriting a clone means that there are
  // incoming arguments that point into compressed pools.
  FunctionCloneRecord *FCR = getCloneInfo(F);
  Function *CloneSource = FCR ? FCR->PAFn : 0;

  PA::FuncInfo *FI;
  if (CloneSource)
    FI = PoolAlloc->getFuncInfoOrClone(*CloneSource);
  else
    FI = PoolAlloc->getFuncInfoOrClone(F);

  if (FI == 0) {
    std::cerr << "DIDN'T FIND POOL INFO FOR: "
              << *F.getType() << F.getName() << "!\n";
    return false;
  }

  // If this function was cloned, and this is the original function, ignore it
  // (it's dead).  We'll deal with the cloned version later when we run into it
  // again.
  if (FI->Clone && &FI->F == &F)
    return false;

  // If there are no pools in this function, exit early.
  if (FI->NodesToPA.empty() && CloneSource == 0)
    return false;

  // Get the DSGraph for this function.
  DSGraph &DSG = ECG->getDSGraph(FI->F);

  std::set<const DSNode*> PoolsToCompressSet;

  // Compute the set of compressible pools in this function that are hosted
  // here.
  FindPoolsToCompress(PoolsToCompressSet, F, DSG, FI);

  // Handle pools that are passed into the function through arguments or
  // returned by the function.  If this occurs, we must be dealing with a ptr
  // compressed clone of the pool allocated clone of the original function.
  if (ExternalPoolsToCompress)
    PoolsToCompressSet.insert(ExternalPoolsToCompress->begin(),
                              ExternalPoolsToCompress->end());

  // If there is nothing that we can compress, exit now.
  if (PoolsToCompressSet.empty()) return false;

  // Compute the initial collection of compressed pointer infos.
  std::map<const DSNode*, CompressedPoolInfo> PoolsToCompress;

  for (std::set<const DSNode*>::iterator I = PoolsToCompressSet.begin(),
         E = PoolsToCompressSet.end(); I != E; ++I) {
    Value *PD;
    if (FCR)
      PD = FCR->PoolDescriptors.find(*I)->second;
    else
      PD = FI->PoolDescriptors[*I];
    assert(PD && "No pool descriptor available for this pool???");

    PoolsToCompress.insert(std::make_pair(*I, CompressedPoolInfo(*I, PD)));
  }

  // Use these to compute the closure of compression information.  In
  // particular, if one pool points to another, we need to know if the outgoing
  // pointer is compressed.
  const TargetData &TD = DSG.getTargetData();
  std::cerr << "In function '" << F.getName() << "':\n";
  for (std::map<const DSNode*, CompressedPoolInfo>::iterator
         I = PoolsToCompress.begin(), E = PoolsToCompress.end(); I != E; ++I) {
    I->second.Initialize(PoolsToCompress, TD);
    std::cerr << "  COMPRESSING POOL:\nPCS:";
    I->second.dump();
  }
  
  // Finally, rewrite the function body to use compressed pointers!
  InstructionRewriter IR(PoolsToCompress, DSG, *FI, FCR, *this);
  if (PremappedVals)
    IR.PremapValues(*PremappedVals);
  IR.visit(F);
  return true;
}


/// GetExtFunctionClone - Return a clone of the specified external function with
/// the specified arguments compressed.
Function *PointerCompress::
GetExtFunctionClone(Function *F, const std::vector<unsigned> &ArgsToComp) {
  assert(!ArgsToComp.empty() && "No reason to make a clone!");
  Function *&Clone = ExtCloneFunctionMap[std::make_pair(F, ArgsToComp)];
  if (Clone) return Clone;

  const FunctionType *FTy = F->getFunctionType();
  const Type *RetTy = FTy->getReturnType();
  unsigned ArgIdx = 0;
  if (isa<PointerType>(RetTy) && ArgsToComp[0] == 0) {
    RetTy = SCALARUINTTYPE;
    ++ArgIdx;
  }

  std::vector<const Type*> ParamTypes;

  for (unsigned i = 0, e = FTy->getNumParams(); i != e; ++i)
    if (ArgIdx < ArgsToComp.size() && ArgsToComp[ArgIdx]-1 == i) {
      // Compressed pool, pass an index.
      ParamTypes.push_back(SCALARUINTTYPE);
      ++ArgIdx;
    } else {
      ParamTypes.push_back(FTy->getParamType(i));
    }
  FunctionType *CFTy = FunctionType::get(RetTy, ParamTypes, FTy->isVarArg());

  // Next, create the clone prototype and insert it into the module.
  Clone = new Function(CFTy, GlobalValue::ExternalLinkage,
                       F->getName()+"_pc");
  F->getParent()->getFunctionList().insert(F, Clone);
  return Clone;
}

/// GetFunctionClone - Lazily create clones of pool allocated functions that we
/// need in compressed form.  This memoizes the functions that have been cloned
/// to allow only one clone of each function in a desired permutation.
Function *PointerCompress::
GetFunctionClone(Function *F, std::set<const DSNode*> &PoolsToCompress,
                 PA::FuncInfo &FI, const DSGraph &CG) {
  assert(!PoolsToCompress.empty() && "No clone needed!");

  // Check to see if we have already compressed this function, if so, there is
  // no need to make another clone.  This is also important to avoid infinite
  // recursion.
  Function *&Clone = ClonedFunctionMap[std::make_pair(F, PoolsToCompress)];
  if (Clone) return Clone;

  // First step, construct the new function prototype.
  const FunctionType *FTy = F->getFunctionType();
  const Type *RetTy = FTy->getReturnType();
  if (isa<PointerType>(RetTy) &&
      PoolsToCompress.count(CG.getReturnNodeFor(FI.F).getNode())) {
    RetTy = SCALARUINTTYPE;
  }
  std::vector<const Type*> ParamTypes;
  unsigned NumPoolArgs = FI.ArgNodes.size();

  // Pass all pool args unmodified.
  for (unsigned i = 0; i != NumPoolArgs; ++i)
    ParamTypes.push_back(FTy->getParamType(i));

  Function::aiterator AI = FI.F.abegin();
  for (unsigned i = NumPoolArgs, e = FTy->getNumParams(); i != e; ++i, ++AI)
    if (isa<PointerType>(FTy->getParamType(i)) &&
        PoolsToCompress.count(CG.getNodeForValue(AI).getNode())) {
      // Compressed pool, pass an index.
      ParamTypes.push_back(SCALARUINTTYPE);
    } else {
      ParamTypes.push_back(FTy->getParamType(i));
    }
  FunctionType *CFTy = FunctionType::get(RetTy, ParamTypes, FTy->isVarArg());

  // Next, create the clone prototype and insert it into the module.
  Clone = new Function(CFTy, GlobalValue::InternalLinkage,
                       F->getName()+".pc");
  F->getParent()->getFunctionList().insert(F, Clone);

  // Remember where this clone came from.
  FunctionCloneRecord &CFI = 
    ClonedFunctionInfoMap.insert(std::make_pair(Clone, F)).first->second;

  ++NumCloned;
  std::cerr << " CLONING FUNCTION: " << F->getName() << " -> "
            << Clone->getName() << "\n";

  if (F->isExternal()) {
    Clone->setLinkage(GlobalValue::ExternalLinkage);
    return Clone;
  }

  std::map<const Value*, Value*> ValueMap;

  // Create dummy Value*'s of pointer type for any arguments that are
  // compressed.  These are needed to satisfy typing constraints before the
  // function body has been rewritten.
  std::vector<std::pair<Value*,Value*> > RemappedArgs;

  // Process arguments, setting up the ValueMap for them.
  Function::aiterator CI = Clone->abegin();   // Iterator over cloned fn args.
  for (Function::aiterator I = F->abegin(), E = F->aend(); I != E; ++I, ++CI) {
    // Transfer the argument names over.
    CI->setName(I->getName());

    // If we are compressing this argument, set up RemappedArgs.
    if (CI->getType() != I->getType()) {
      // Create a useless value* that is only needed to hold the uselist for the
      // argument.
      Value *V = new Argument(I->getType());   // dummy argument
      RemappedArgs.push_back(std::make_pair(V, CI));
      ValueMap[I] = V;
    } else {
      // Otherwise, just remember the mapping.
      ValueMap[I] = CI;
    }
  }

  // Clone the actual function body over.
  std::vector<ReturnInst*> Returns;
  CloneFunctionInto(Clone, F, ValueMap, Returns);
  Returns.clear();  // Don't need this.
  
  // Invert the ValueMap into the NewToOldValueMap
  std::map<Value*, const Value*> &NewToOldValueMap = CFI.NewToOldValueMap;
  for (std::map<const Value*, Value*>::iterator I = ValueMap.begin(),
         E = ValueMap.end(); I != E; ++I)
    NewToOldValueMap.insert(std::make_pair(I->second, I->first));

  // Compute the PoolDescriptors map for the cloned function.
  for (std::map<const DSNode*, Value*>::iterator I =
         FI.PoolDescriptors.begin(), E = FI.PoolDescriptors.end();
       I != E; ++I)
    CFI.PoolDescriptors[I->first] = ValueMap[I->second];
  
  ValueMap.clear();
  
  // Recursively transform the function.
  CompressPoolsInFunction(*Clone, &RemappedArgs, &PoolsToCompress);
  return Clone;
}


/// InitializePoolLibraryFunctions - Create the function prototypes for pointer
/// compress runtime library functions.
void PointerCompress::InitializePoolLibraryFunctions(Module &M) {
  const Type *VoidPtrTy = PointerType::get(Type::SByteTy);
  const Type *PoolDescPtrTy = PointerType::get(ArrayType::get(VoidPtrTy, 16));

  PoolInitPC = M.getOrInsertFunction("poolinit_pc", Type::VoidTy,
                                     PoolDescPtrTy, Type::UIntTy,
                                     Type::UIntTy, Type::UIntTy, 0);
  PoolDestroyPC = M.getOrInsertFunction("pooldestroy_pc", Type::VoidTy,
                                        PoolDescPtrTy, 0);
  // FIXME: Need bumppointer versions as well as realloc??/memalign??
}

bool PointerCompress::runOnModule(Module &M) {
  PoolAlloc = &getAnalysis<PoolAllocatePassAllPools>();
  ECG = &getAnalysis<PA::EquivClassGraphs>();
  
  if (SmallIntCompress)
    MEMUINTTYPE = Type::UShortTy;
  else 
    MEMUINTTYPE = Type::UIntTy;

  // FIXME: make this IntPtrTy.
  SCALARUINTTYPE = Type::ULongTy;

  // Create the function prototypes for pointer compress runtime library
  // functions.
  InitializePoolLibraryFunctions(M);

  // Iterate over all functions in the module, looking for compressible data
  // structures.
  bool Changed = false;
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    // If this function is not a pointer-compressed clone, compress any pools in
    // it now.
    if (!ClonedFunctionInfoMap.count(I))
      Changed |= CompressPoolsInFunction(*I);
  }

  ClonedFunctionMap.clear();
  return Changed;
}
