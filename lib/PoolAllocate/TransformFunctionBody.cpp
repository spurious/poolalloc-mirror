//===-- TransformFunctionBody.cpp - Pool Function Transformer -------------===//
//
// This file defines the PoolAllocate::TransformBody method.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "PoolAllocator"
#include "poolalloc/PoolAllocate.h"
#include "llvm/Analysis/DataStructure.h"
#include "llvm/Analysis/DSGraph.h"
#include "llvm/Module.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/InstVisitor.h"
#include "Support/Debug.h"
#include "Support/VectorExtras.h"
using namespace llvm;
using namespace PA;

namespace {
  /// FuncTransform - This class implements transformation required of pool
  /// allocated functions.
  struct FuncTransform : public InstVisitor<FuncTransform> {
    PoolAllocate &PAInfo;
    DSGraph &G;      // The Bottom-up DS Graph
    FuncInfo &FI;

    // PoolUses - For each pool (identified by the pool descriptor) keep track
    // of which blocks require the memory in the pool to not be freed.  This
    // does not include poolfree's.  Note that this is only tracked for pools
    // which this is the home of, ie, they are Alloca instructions.
    std::set<std::pair<AllocaInst*, Instruction*> > &PoolUses;

    // PoolDestroys - For each pool, keep track of the actual poolfree calls
    // inserted into the code.  This is seperated out from PoolUses.
    std::set<std::pair<AllocaInst*, CallInst*> > &PoolFrees;

    FuncTransform(PoolAllocate &P, DSGraph &g, FuncInfo &fi,
                  std::set<std::pair<AllocaInst*, Instruction*> > &poolUses,
                  std::set<std::pair<AllocaInst*, CallInst*> > &poolFrees)
      : PAInfo(P), G(g), FI(fi),
        PoolUses(poolUses), PoolFrees(poolFrees) {
    }

    template <typename InstType, typename SetType>
    void AddPoolUse(InstType &I, Value *PoolHandle, SetType &Set) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(PoolHandle))
        Set.insert(std::make_pair(AI, &I));
    }

    void visitInstruction(Instruction &I);
    void visitMallocInst(MallocInst &MI);
    void visitCallocCall(CallSite CS);
    void visitReallocCall(CallSite CS);
    void visitFreeInst(FreeInst &FI);
    void visitCallSite(CallSite CS);
    void visitCallInst(CallInst &CI) { visitCallSite(&CI); }
    void visitInvokeInst(InvokeInst &II) { visitCallSite(&II); }
    void visitLoadInst(LoadInst &I);
    void visitStoreInst (StoreInst &I);

  private:
    Instruction *TransformAllocationInstr(Instruction *I, Value *Size);
    Instruction *InsertPoolFreeInstr(Value *V, Instruction *Where);

    void UpdateNewToOldValueMap(Value *OldVal, Value *NewV1, Value *NewV2) {
      std::map<Value*, const Value*>::iterator I =
        FI.NewToOldValueMap.find(OldVal);
      assert(I != FI.NewToOldValueMap.end() && "OldVal not found in clone?");
      FI.NewToOldValueMap.insert(std::make_pair(NewV1, I->second));
      if (NewV2)
        FI.NewToOldValueMap.insert(std::make_pair(NewV2, I->second));
      FI.NewToOldValueMap.erase(I);
    }

    DSNodeHandle& getDSNodeHFor(Value *V) {
      if (!FI.NewToOldValueMap.empty()) {
        // If the NewToOldValueMap is in effect, use it.
        std::map<Value*,const Value*>::iterator I = FI.NewToOldValueMap.find(V);
        if (I != FI.NewToOldValueMap.end())
          V = (Value*)I->second;
      }

      return G.getScalarMap()[V];
    }

    Value *getPoolHandle(Value *V) {
      DSNode *Node = getDSNodeHFor(V).getNode();
      // Get the pool handle for this DSNode...
      std::map<DSNode*, Value*>::iterator I = FI.PoolDescriptors.find(Node);
      return I != FI.PoolDescriptors.end() ? I->second : 0;
    }
    
    Function* retCloneIfFunc(Value *V);
  };
}

void PoolAllocate::TransformBody(DSGraph &g, PA::FuncInfo &fi,
                       std::set<std::pair<AllocaInst*,Instruction*> > &poolUses,
                       std::set<std::pair<AllocaInst*, CallInst*> > &poolFrees,
                                 Function &F) {
  FuncTransform(*this, g, fi, poolUses, poolFrees).visit(F);
}


// Returns the clone if  V is a static function (not a pointer) and belongs 
// to an equivalence class i.e. is pool allocated
Function* FuncTransform::retCloneIfFunc(Value *V) {
  if (ConstantPointerRef *CPR = dyn_cast<ConstantPointerRef>(V))
    V = CPR->getValue();
  
  if (Function *F = dyn_cast<Function>(V))
    if (FuncInfo *FI = PAInfo.getFuncInfo(*F))
      return FI->Clone;

  return 0;
}

void FuncTransform::visitLoadInst(LoadInst &LI) {
  if (Value *PH = getPoolHandle(LI.getOperand(0)))
    AddPoolUse(LI, PH, PoolUses);
  visitInstruction(LI);
}

void FuncTransform::visitStoreInst(StoreInst &SI) {
  if (Value *PH = getPoolHandle(SI.getOperand(1)))
    AddPoolUse(SI, PH, PoolUses);
  visitInstruction(SI);
}

Instruction *FuncTransform::TransformAllocationInstr(Instruction *I,
                                                     Value *Size) {
  std::string Name = I->getName(); I->setName("");

  // Insert a call to poolalloc
  Value *PH = getPoolHandle(I);
  Instruction *V = new CallInst(PAInfo.PoolAlloc, make_vector(PH, Size, 0),
                                Name, I);

  AddPoolUse(*V, PH, PoolUses);

  // Cast to the appropriate type if necessary
  Instruction *Casted = V;
  if (V->getType() != I->getType())
    Casted = new CastInst(V, I->getType(), V->getName(), I);
    
  // Update def-use info
  I->replaceAllUsesWith(Casted);

  // If we are modifying the original function, update the DSGraph.
  if (!FI.Clone) {
    // V and Casted now point to whatever the original allocation did.
    G.getScalarMap().replaceScalar(I, V);
    if (V != Casted)
      G.getScalarMap()[Casted] = G.getScalarMap()[V];
  } else {             // Otherwise, update the NewToOldValueMap
    UpdateNewToOldValueMap(I, V, V != Casted ? Casted : 0);
  }

  // Remove old allocation instruction.
  I->getParent()->getInstList().erase(I);
  return Casted;
}


void FuncTransform::visitMallocInst(MallocInst &MI) {
  // Get the pool handle for the node that this contributes to...
  Value *PH = getPoolHandle(&MI);
  if (PH == 0) return;

  TargetData &TD = PAInfo.getAnalysis<TargetData>();
  Value *AllocSize =
    ConstantUInt::get(Type::UIntTy, TD.getTypeSize(MI.getAllocatedType()));

  if (MI.isArrayAllocation())
    AllocSize = BinaryOperator::create(Instruction::Mul, AllocSize,
                                       MI.getOperand(0), "sizetmp", &MI);

  TransformAllocationInstr(&MI, AllocSize);
}


Instruction *FuncTransform::InsertPoolFreeInstr(Value *Arg, Instruction *Where){
  Value *PH = getPoolHandle(Arg);  // Get the pool handle for this DSNode...
  if (PH == 0) return 0;

  // Insert a cast and a call to poolfree...
  Value *Casted = Arg;
  if (Arg->getType() != PointerType::get(Type::SByteTy))
    Casted = new CastInst(Arg, PointerType::get(Type::SByteTy),
				 Arg->getName()+".casted", Where);

  CallInst *FreeI = new CallInst(PAInfo.PoolFree, make_vector(PH, Casted, 0), 
				 "", Where);
  AddPoolUse(*FreeI, PH, PoolFrees);
  return FreeI;
}


void FuncTransform::visitFreeInst(FreeInst &FrI) {
  if (Instruction *I = InsertPoolFreeInstr(FrI.getOperand(0), &FrI)) {
    // Delete the now obsolete free instruction...
    FrI.getParent()->getInstList().erase(&FrI);
 
    // Update the NewToOldValueMap if this is a clone
    if (!FI.NewToOldValueMap.empty()) {
      std::map<Value*,const Value*>::iterator II =
        FI.NewToOldValueMap.find(&FrI);
      assert(II != FI.NewToOldValueMap.end() && 
             "FrI not found in clone?");
      FI.NewToOldValueMap.insert(std::make_pair(I, II->second));
      FI.NewToOldValueMap.erase(II);
    }
  }
}


void FuncTransform::visitCallocCall(CallSite CS) {
  Module *M = CS.getInstruction()->getParent()->getParent()->getParent();
  assert(CS.arg_end()-CS.arg_begin() == 2 && "calloc takes two arguments!");
  Value *V1 = *CS.arg_begin();
  Value *V2 = *(CS.arg_begin()+1);
  if (V1->getType() != V2->getType()) {
    V1 = new CastInst(V1, Type::UIntTy, V1->getName(), CS.getInstruction());
    V2 = new CastInst(V2, Type::UIntTy, V2->getName(), CS.getInstruction());
  }

  V2 = BinaryOperator::create(Instruction::Mul, V1, V2, "size",
                              CS.getInstruction());
  if (V2->getType() != Type::UByteTy)
    V2 = new CastInst(V2, Type::UIntTy, V2->getName(), CS.getInstruction());

  BasicBlock::iterator BBI =
    TransformAllocationInstr(CS.getInstruction(), V2);
  Value *Ptr = BBI++;

  // We just turned the call of 'calloc' into the equivalent of malloc.  To
  // finish calloc, we need to zero out the memory.
  Function *MemSet = M->getOrInsertFunction("llvm.memset",
                                            Type::VoidTy,
                                            PointerType::get(Type::SByteTy),
                                            Type::UByteTy, Type::UIntTy,
                                            Type::UIntTy, 0);

  if (Ptr->getType() != PointerType::get(Type::SByteTy))
    Ptr = new CastInst(Ptr, PointerType::get(Type::SByteTy), Ptr->getName(),
                       BBI);
  
  // We know that the memory returned by poolalloc is at least 4 byte aligned.
  new CallInst(MemSet, make_vector(Ptr, ConstantUInt::get(Type::UByteTy, 0),
                                   V2,  ConstantUInt::get(Type::UIntTy, 4), 0),
               "", BBI);
}


void FuncTransform::visitReallocCall(CallSite CS) {
  Module *M = CS.getInstruction()->getParent()->getParent()->getParent();
  assert(CS.arg_end()-CS.arg_begin() == 2 && "realloc takes two arguments!");
  Value *OldPtr = *CS.arg_begin();
  Value *Size = *(CS.arg_begin()+1);

  BasicBlock::iterator BBI =
    TransformAllocationInstr(CS.getInstruction(), Size);
  Value *NewPtr = BBI++;

  // We just turned the call of 'realloc' into the equivalent of malloc.  To
  // finish realloc, we need to copy the memory from the old block to the new,
  // then free the old block.
  const Type *SBPtr = PointerType::get(Type::SByteTy);
  Function *MemCpy = M->getOrInsertFunction("llvm.memcpy",
                                            Type::VoidTy, SBPtr, SBPtr,
                                            Type::UIntTy, Type::UIntTy, 0);

  if (NewPtr->getType() != SBPtr)
    NewPtr = new CastInst(NewPtr, SBPtr, NewPtr->getName(), BBI);
  if (OldPtr->getType() != SBPtr)
    OldPtr = new CastInst(OldPtr, SBPtr, OldPtr->getName(), BBI);
  if (Size->getType() != Type::UIntTy)
    Size = new CastInst(Size, Type::UIntTy, Size->getName(), BBI);
  
  // We know that the memory returned by poolalloc is at least 4 byte aligned.
  new CallInst(MemCpy, make_vector(NewPtr, OldPtr, Size, 
                                   ConstantUInt::get(Type::UIntTy, 4), 0),
               "", BBI);

  // Free the old memory now.
  InsertPoolFreeInstr(OldPtr, BBI);
}



void FuncTransform::visitCallSite(CallSite CS) {
  Function *CF = CS.getCalledFunction();
  Instruction *TheCall = CS.getInstruction();

  // optimization for function pointers that are basically gotten from a cast
  // with only one use and constant expressions with casts in them
  if (!CF) {
    Value *CV = CS.getCalledValue();
    if (CastInst* CastI = dyn_cast<CastInst>(CV)) {
      if (isa<Function>(CastI->getOperand(0)) && 
	  CastI->getOperand(0)->getType() == CastI->getType())
	CF = dyn_cast<Function>(CastI->getOperand(0));
    } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(CV)) {
      if (CE->getOpcode() == Instruction::Cast)
	if (ConstantPointerRef *CPR
            = dyn_cast<ConstantPointerRef>(CE->getOperand(0)))
          CF = dyn_cast<Function>(CPR->getValue());
    }
  }

  // If this function is one of the memory manipulating functions built into
  // libc, emulate it with pool calls as appropriate.
  if (CF && CF->isExternal())
    if (CF->getName() == "calloc") {
      visitCallocCall(CS);
      return;
    } else if (CF->getName() == "realloc") {
      visitReallocCall(CS);
      return;
    } else if (CF->getName() == "strdup") {
      assert(0 && "strdup should have been linked into the program!");
    } else if (CF->getName() == "memalign" ||
               CF->getName() == "posix_memalign") {
      std::cerr << "MEMALIGN USED BUT NOT HANDLED!\n";
      abort();
    }

  // We need to figure out which local pool descriptors correspond to the pool
  // descriptor arguments passed into the function call.  Calculate a mapping
  // from callee DSNodes to caller DSNodes.  We construct a partial isomophism
  // between the graphs to figure out which pool descriptors need to be passed
  // in.  The roots of this mapping is found from arguments and return values.
  //
  DSGraph::NodeMapTy NodeMapping;
  Instruction *NewCall;
  Value *NewCallee;
  std::vector<DSNode*> ArgNodes;
  DSGraph *CalleeGraph;  // The callee graph

  if (CF) {   // Direct calls are nice and simple.
    FuncInfo *CFI = PAInfo.getFuncInfo(*CF);
    if (CFI == 0 || CFI->Clone == 0) {   // Nothing to transform...
      visitInstruction(*TheCall);
      return;
    }
    NewCallee = CFI->Clone;
    ArgNodes = CFI->ArgNodes;
    
    DEBUG(std::cerr << "  Handling direct call: " << *TheCall);
    CalleeGraph = &PAInfo.getBUDataStructures().getDSGraph(*CF);
  } else {
    DEBUG(std::cerr << "  Handling indirect call: " << *TheCall);
    
    // Figure out which set of functions this call may invoke
    Instruction *OrigInst = CS.getInstruction();

    // If this call site is in a clone function, map it back to the original
    if (!FI.NewToOldValueMap.empty())
      OrigInst = cast<Instruction>((Value*)FI.NewToOldValueMap[OrigInst]);
    const PA::EquivClassInfo &ECI =
      PAInfo.getECIForIndirectCallSite(CallSite::get(OrigInst));

    if (ECI.ArgNodes.empty())
      return;   // No arguments to add?  Transformation is a noop!

    // Here we fill in CF with one of the possible called functions.  Because we
    // merged together all of the arguments to all of the functions in the
    // equivalence set, it doesn't really matter which one we pick.
    CalleeGraph = ECI.G;
    CF = ECI.FuncsInClass.back();
    NewCallee = CS.getCalledValue();
    ArgNodes = ECI.ArgNodes;

    // Cast the function pointer to an appropriate type!
    std::vector<const Type*> ArgTys(ArgNodes.size(),
                                    PoolAllocate::PoolDescPtrTy);
    for (CallSite::arg_iterator I = CS.arg_begin(), E = CS.arg_end();
         I != E; ++I)
      ArgTys.push_back((*I)->getType());
      
    FunctionType *FTy = FunctionType::get(TheCall->getType(), ArgTys, false);
    PointerType *PFTy = PointerType::get(FTy);
    
    // If there are any pool arguments cast the function pointer to the right
    // type.
    NewCallee = new CastInst(NewCallee, PFTy, "tmp", TheCall);
  }

  Function::aiterator FAI = CF->abegin(), E = CF->aend();
  CallSite::arg_iterator AI = CS.arg_begin(), AE = CS.arg_end();
  for ( ; FAI != E && AI != AE; ++FAI, ++AI)
    if (!isa<Constant>(*AI))
      DSGraph::computeNodeMapping(CalleeGraph->getNodeForValue(FAI),
                                  getDSNodeHFor(*AI), NodeMapping, false);
  //assert(AI == AE && "Varargs calls not handled yet!");

  // Map the return value as well...
  if (TheCall->getType() != Type::VoidTy)
    DSGraph::computeNodeMapping(CalleeGraph->getReturnNodeFor(*CF),
                                getDSNodeHFor(TheCall), NodeMapping, false);

#if 1
    // Map the nodes that are pointed to by globals.
    // For all globals map getDSNodeForGlobal(g)->CG.getDSNodeForGlobal(g)
    for (DSGraph::ScalarMapTy::iterator SMI = G.getScalarMap().begin(), 
	   SME = G.getScalarMap().end(); SMI != SME; ++SMI)
      if (GlobalValue *GV = dyn_cast<GlobalValue>(SMI->first)) 
	DSGraph::computeNodeMapping(CalleeGraph->getNodeForValue(GV),
                                    SMI->second, NodeMapping, false);
#endif


  // Okay, now that we have established our mapping, we can figure out which
  // pool descriptors to pass in...
  std::vector<Value*> Args;
  for (unsigned i = 0, e = ArgNodes.size(); i != e; ++i) {
    Value *ArgVal = 0;
    if (NodeMapping.count(ArgNodes[i]))
      if (DSNode *LocalNode = NodeMapping[ArgNodes[i]].getNode())
        if (FI.PoolDescriptors.count(LocalNode))
          ArgVal = FI.PoolDescriptors.find(LocalNode)->second;
    Args.push_back(ArgVal ? ArgVal : 
                   Constant::getNullValue(PoolAllocate::PoolDescPtrTy));
  }

  // Add the rest of the arguments...
  Args.insert(Args.end(), CS.arg_begin(), CS.arg_end());
    
  std::string Name = TheCall->getName(); TheCall->setName("");

  if (InvokeInst *II = dyn_cast<InvokeInst>(TheCall)) {
    NewCall = new InvokeInst(NewCallee, II->getNormalDest(),
                             II->getUnwindDest(), Args, Name, TheCall);
  } else {
    NewCall = new CallInst(NewCallee, Args, Name, TheCall);
  }

  // Add all of the uses of the pool descriptor
  for (unsigned i = 0, e = ArgNodes.size(); i != e; ++i)
    AddPoolUse(*NewCall, Args[i], PoolUses);

  TheCall->replaceAllUsesWith(NewCall);
  DEBUG(std::cerr << "  Result Call: " << *NewCall);

  if (TheCall->getType() != Type::VoidTy) {
    // If we are modifying the original function, update the DSGraph... 
    DSGraph::ScalarMapTy &SM = G.getScalarMap();
    DSGraph::ScalarMapTy::iterator CII = SM.find(TheCall);
    if (CII != SM.end()) {
      SM[NewCall] = CII->second;
      SM.erase(CII);                     // Destroy the CallInst
    } else { 
      // Otherwise update the NewToOldValueMap with the new CI return value
      std::map<Value*,const Value*>::iterator CII = 
        FI.NewToOldValueMap.find(TheCall);
      assert(CII != FI.NewToOldValueMap.end() && "CI not found in clone?");
      FI.NewToOldValueMap.insert(std::make_pair(NewCall, CII->second));
      FI.NewToOldValueMap.erase(CII);
    }
  } else if (!FI.NewToOldValueMap.empty()) {
    std::map<Value*,const Value*>::iterator II =
      FI.NewToOldValueMap.find(TheCall);
    assert(II != FI.NewToOldValueMap.end() && 
           "CI not found in clone?");
    FI.NewToOldValueMap.insert(std::make_pair(NewCall, II->second));
    FI.NewToOldValueMap.erase(II);
  }

  TheCall->getParent()->getInstList().erase(TheCall);
  visitInstruction(*NewCall);
}


// visitInstruction - For all instructions in the transformed function bodies,
// replace any references to the original calls with references to the
// transformed calls.  Many instructions can "take the address of" a function,
// and we must make sure to catch each of these uses, and transform it into a
// reference to the new, transformed, function.
void FuncTransform::visitInstruction(Instruction &I) {
  for (unsigned i = 0, e = I.getNumOperands(); i != e; ++i)
    if (Function *clonedFunc = retCloneIfFunc(I.getOperand(i))) {
      Constant *CF = ConstantPointerRef::get(clonedFunc);
      I.setOperand(i, ConstantExpr::getCast(CF, I.getOperand(i)->getType()));
    }
}
