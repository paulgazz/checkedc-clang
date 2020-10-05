//=--AVarBoundsInfo.cpp-------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation of methods in AVarBoundsInfo.h.
//
//===----------------------------------------------------------------------===//

#include <sstream>
#include "clang/CConv/AVarBoundsInfo.h"
#include "clang/CConv/ProgramInfo.h"
#include "clang/CConv/ConstraintResolver.h"

std::vector<BoundsPriority>
    AVarBoundsInfo::PrioList {Declared, Allocator, FlowInferred, Heuristics};

extern cl::OptionCategory ArrBoundsInferCat;
static cl::opt<bool> DisableInfDecls("disable-arr-missd",
                                     cl::desc("Disable ignoring of missed "
                                              "bounds from declarations."),
                                     cl::init(false),
                                     cl::cat(ArrBoundsInferCat));

void AVarBoundsStats::print(llvm::raw_ostream &O,
                            const std::set<BoundsKey> *InSrcArrs,
                            bool JsonFormat) const {
  std::set<BoundsKey> Tmp;
  if (!JsonFormat) {
    O << "Array Bounds Inference Stats:\n";
    findIntersection(NamePrefixMatch, *InSrcArrs, Tmp);
    O << "NamePrefixMatch:" << Tmp.size() << "\n";
    findIntersection(AllocatorMatch, *InSrcArrs, Tmp);
    O << "AllocatorMatch:" << Tmp.size() << "\n";
    findIntersection(VariableNameMatch, *InSrcArrs, Tmp);
    O << "VariableNameMatch:" << Tmp.size() << "\n";
    findIntersection(NeighbourParamMatch, *InSrcArrs, Tmp);
    O << "NeighbourParamMatch:" << Tmp.size() << "\n";
    findIntersection(DataflowMatch, *InSrcArrs, Tmp);
    O << "DataflowMatch:" << Tmp.size() << "\n";
    findIntersection(DeclaredBounds, *InSrcArrs, Tmp);
    O << "Declared:" << Tmp.size() << "\n";
  } else {
    O << "\"ArrayBoundsInferenceStats\":{";
    findIntersection(NamePrefixMatch, *InSrcArrs, Tmp);
    O << "\"NamePrefixMatch\":" << Tmp.size() << ",\n";
    findIntersection(AllocatorMatch, *InSrcArrs, Tmp);
    O << "\"AllocatorMatch\":" << Tmp.size() << ",\n";
    findIntersection(VariableNameMatch, *InSrcArrs, Tmp);
    O << "\"VariableNameMatch\":" << Tmp.size() << ",\n";
    findIntersection(NeighbourParamMatch, *InSrcArrs, Tmp);
    O << "\"NeighbourParamMatch\":" << Tmp.size() << ",\n";
    findIntersection(DataflowMatch, *InSrcArrs, Tmp);
    O << "\"DataflowMatch\":" << Tmp.size() << ",\n";
    findIntersection(DeclaredBounds, *InSrcArrs, Tmp);
    O << "\"Declared\":" << Tmp.size() << "\n";
    O << "}";
  }
}

bool hasArray(ConstraintVariable *CK, Constraints &CS) {
  auto &E = CS.getVariables();
  if (PVConstraint *PV = dyn_cast<PVConstraint>(CK)) {
    if ((PV->hasArr(E, 0) || PV->hasNtArr(E, 0)) &&
        PV->isTopCvarUnsizedArr()) {
      return true;
    }
  }
  return false;
}

bool isInSrcArray(ConstraintVariable *CK, Constraints &CS) {
  auto &E = CS.getVariables();
  if (PVConstraint *PV = dyn_cast<PVConstraint>(CK)) {
    if ((PV->hasArr(E, 0) || PV->hasNtArr(E, 0)) &&
        PV->isTopCvarUnsizedArr() && PV->isForValidDecl()) {
      return true;
    }
  }
  return false;
}

// This class picks variables that are in the same scope as the provided scope.
class ScopeVisitor {
public:
  ScopeVisitor(const ProgramVarScope *S, std::set<BoundsKey> &R,
               std::map<BoundsKey, ProgramVar *> &VarM,
               std::set<BoundsKey> &P): TS(S), Res(R), VM(VarM)
    , PtrAtoms(P) { }
  void visitBoundsKey(BoundsKey V) const {
    // If the variable is non-pointer?
    if (VM.find(V) != VM.end() && PtrAtoms.find(V) == PtrAtoms.end()) {
      auto *S = VM[V];
      // If the variable is constant or in the same scope?
      if (S->IsNumConstant() ||
          (*(TS) == *(S->getScope()))) {
        Res.insert(V);
      }
    }
  }

  void filterOutBKeys(std::set<BoundsKey> &Src) {
    for (auto BK : Src) {
      // If the variable non-pointer?
      if (PtrAtoms.find(BK) == PtrAtoms.end()) {
        auto *S = VM[BK];
        // If the variable is constant or in the same scope?
        if (S->IsNumConstant() || (*(TS) == *(S->getScope()))) {
          Res.insert(BK);
        }
      }
    }
  }
  const ProgramVarScope *TS;
  std::set<BoundsKey> &Res;
  std::map<BoundsKey, ProgramVar *> &VM;
  std::set<BoundsKey> &PtrAtoms;
};

void
AvarBoundsInference::
mergeReachableProgramVars(std::set<BoundsKey> &AllVars) {
  if (AllVars.size() > 1) {
    // Convert the bounds key to corresponding program var.
    std::set<ProgramVar *> AllProgVars;
    for (auto AV : AllVars) {
      AllProgVars.insert(BI->getProgramVar(AV));
    }
    ProgramVar *BVar = nullptr;
    // We want to merge all bounds vars. We give preference to
    // non-constants if there are multiple non-constant variables,
    // we give up.
    for (auto *TmpB : AllProgVars) {
      if (BVar == nullptr) {
        BVar = TmpB;
      } else if (BVar->IsNumConstant()) {
        if (!TmpB->IsNumConstant()) {
          // We give preference to non-constant lengths.
          BVar = TmpB;
        } else if (!this->BI->areSameProgramVar(BVar->getKey(), TmpB->getKey())) {
          // If both are different constants?
          BVar = nullptr;
          break;
        }
      } else if (!TmpB->IsNumConstant() && BVar->getKey() != TmpB->getKey()) {
        // If they are different variables?
        BVar = nullptr;
        break;
      }
    }
    AllVars.clear();
    if (BVar)
      AllVars.insert(BVar->getKey());
  }
}

bool
AvarBoundsInference::convergeInferredBounds() {
  bool FoundSome = false;
  for (auto &InfABnds : CurrIterInferBounds) {
    auto *AB = BI->getBounds(InfABnds.first);
    // If there are no bounds?
    if (AB == nullptr) {
      auto &BTypeMap = InfABnds.second;
      for (auto &TySet : BTypeMap) {
        mergeReachableProgramVars(TySet.second);
      }
      // Order of preference: Count and Byte
      if (BTypeMap.find(ABounds::CountBoundKind) != BTypeMap.end() &&
          !BTypeMap[ABounds::CountBoundKind].empty()) {
        AB = new CountBound(*BTypeMap[ABounds::CountBoundKind].begin());
      } else if (BTypeMap.find(ABounds::ByteBoundKind) != BTypeMap.end() &&
                 !BTypeMap[ABounds::ByteBoundKind].empty()) {
        AB = new ByteBound(*BTypeMap[ABounds::ByteBoundKind].begin());
      }

      // If we found any bounds?
      if (AB != nullptr) {
        // Record that we inferred bounds using data-flow.
        BI->BoundsInferStats.DataflowMatch.insert(InfABnds.first);
        BI->replaceBounds(InfABnds.first, BoundsPriority::FlowInferred, AB);
        FoundSome = true;
      }
    }
  }
  return FoundSome;
}

bool
AvarBoundsInference::getReachableBoundKeys(const ProgramVarScope *DstScope,
                                           BoundsKey FromVarK,
                                           std::set<BoundsKey> &PotK,
                                           AVarGraph &BKGraph,
                                           bool CheckImmediate) {

  // First, find all the in-scope variable to which the SBKey flow to.
  auto *SBVar = BI->getProgramVar(FromVarK);

  // If both are in the same scope?
  if (*DstScope == *SBVar->getScope()) {
    PotK.insert(FromVarK);
    if (CheckImmediate) {
      return true;
    }
  }

  // All constants are reachable!
  if (SBVar->IsNumConstant()) {
    PotK.insert(FromVarK);
  }

  // Get all bounds key that are equivalent to FromVarK
  std::set<BoundsKey> AllFKeys;
  AllFKeys.clear();
  AllFKeys.insert(FromVarK);

  // If this is a constant? Then get all bounds keys that
  // correspond to the same constant
  if (SBVar->IsNumConstant()) {
    uint64_t ConsVal;
    std::istringstream IS(SBVar->getVarName());
    IS >> ConsVal;
    BI->fetchAllConstKeys(ConsVal, AllFKeys);
  }

  for (auto CurrVarK : AllFKeys) {
    // Find all the in scope variables reachable from the CurrVarK
    // bounds variable.
    ScopeVisitor TV(DstScope, PotK, BI->PVarInfo,
                    BI->PointerBoundsKey);
    BKGraph.visitBreadthFirst(CurrVarK, [&TV](BoundsKey BK) {
      TV.visitBoundsKey(BK);
    });
  }

  return !PotK.empty();
}

bool AvarBoundsInference::getRelevantBounds(BoundsKey BK,
                                            BndsKindMap &ResBounds) {
  // Try to get the bounds of all RBKeys.
  bool HasBounds = false;
  // If this pointer is used in pointer arithmetic then there
  // are no relevant bounds for this pointer.
  if (!BI->hasPointerArithmetic(BK)) {
    auto *PrevBounds = BI->getBounds(BK);
    // Does the parent arr has bounds?
    if (PrevBounds != nullptr) {
      ResBounds[PrevBounds->getKind()].insert(PrevBounds->getBKey());
      HasBounds = true;
    } else if (CurrIterInferBounds.find(BK) != CurrIterInferBounds.end()) {
      // get the bounds inferred from the current iteration
      ResBounds = CurrIterInferBounds[BK];
      HasBounds = true;
    }
  }
  return HasBounds;
}

// Variable comparison. Comparator implementation: where given two BoundsKey
// they are checked to see if they correspond to the same program variable.
struct BVarCmp {
public:
  BVarCmp(AVarBoundsInfo *ABI) {
    this->ABInfo = ABI;
  }
  bool operator()(BoundsKey a, BoundsKey b) const {
    if (this->ABInfo != nullptr &&
        this->ABInfo->areSameProgramVar(a, b)) {
      return false;
    }
    return a < b;
  };
private:
  AVarBoundsInfo *ABInfo;
};

bool
AvarBoundsInference::areDeclaredBounds(BoundsKey K,
                                       const std::pair<ABounds::BoundsKind,
                                         std::set<BoundsKey>> &Bnds) {
  bool IsDeclaredB = false;
  // Get declared bounds and check that Bnds are same as the declared
  // bounds.
  ABounds *DeclB = this->BI->getBounds(K,
                                       BoundsPriority::Declared,
                                       nullptr);
  if (DeclB && DeclB->getKind() == Bnds.first) {
    IsDeclaredB = true;
    for (auto TmpNBK : Bnds.second) {
      if (!this->BI->areSameProgramVar(TmpNBK, DeclB->getBKey())) {
        IsDeclaredB = false;
        break;
      }
    }
  }
  return IsDeclaredB;
}

bool AvarBoundsInference::predictBounds(BoundsKey K,
                                        std::set<BoundsKey> &Neighbours,
                                        AVarGraph &BKGraph) {
  BndsKindMap NeighboursBnds, InferredKBnds;
  // Bounds inferred from each of the neighbours.
  std::map<BoundsKey, BndsKindMap> InferredNBnds;
  bool IsChanged = false;
  bool ErrorOccurred = false;
  bool IsFuncRet = BI->isFunctionReturn(K);
  ProgramVar *KVar = this->BI->getProgramVar(K);

  InferredNBnds.clear();
  // For reach of the Neighbour, try to infer possible bounds.
  for (auto NBK : Neighbours) {
    NeighboursBnds.clear();
    ErrorOccurred = false;
    if (getRelevantBounds(NBK, NeighboursBnds) && !NeighboursBnds.empty()) {
      std::set<BoundsKey> InfBK;
      for (auto &NKBChoice : NeighboursBnds) {
        InfBK.clear();
        for (auto TmpNBK : NKBChoice.second) {
          getReachableBoundKeys(KVar->getScope(), TmpNBK, InfBK, BKGraph);
        }
        if (!InfBK.empty()) {
          InferredNBnds[NBK][NKBChoice.first] = InfBK;
        } else {
          bool IsDeclaredB = areDeclaredBounds(NBK, NKBChoice);

          if (!IsDeclaredB || DisableInfDecls) {
            // Oh, there are bounds for neighbour NBK but no bounds
            // can be inferred for K from it.
            InferredNBnds.clear();
            ErrorOccurred = true;
            break;
          }
        }
      }
    } else if (IsFuncRet) {
      // If this is a function return we should have bounds from all
      // neighbours.
      ErrorOccurred = true;
    }
    if (ErrorOccurred) {
      // If an error occurred while processing bounds from neighbours/
      // clear the inferred bounds and break.
      InferredNBnds.clear();
      break;
    }
  }

  if (!InferredNBnds.empty()) {
    // All the possible inferred bounds for K
    InferredKBnds.clear();
    std::set<BoundsKey> TmpBKeys;
    // TODO: Figure out if there is a discrepency and try to implement
    // root-cause analysis.

    // Find intersection of all bounds from neighbours.
    for (auto &IN : InferredNBnds) {
      for (auto &INB : IN.second) {
        if (InferredKBnds.find(INB.first) == InferredKBnds.end()) {
          InferredKBnds[INB.first] = INB.second;
        } else {
          TmpBKeys.clear();
          // Here, we should use intersection by taking care of comparing
          // bounds key that correspond to the same constant.
          // Note, DO NOT use findIntersection here, as we need to take
          // care of comparing bounds key that correspond to the same
          // constant.
          auto &S1 = InferredKBnds[INB.first];
          auto &S2 = INB.second;
          std::set_intersection(S1.begin(), S1.end(),
                                S2.begin(), S2.end(),
                                std::inserter(TmpBKeys, TmpBKeys.begin()),
                                BVarCmp(this->BI));
          InferredKBnds[INB.first] = TmpBKeys;
        }
      }
    }

    // Now from the newly inferred bounds i.e., InferredKBnds, check
    // if is is different from previously known bounds of K
    for (auto &IKB : InferredKBnds) {
      bool Handled = false;
      if (CurrIterInferBounds.find(K) != CurrIterInferBounds.end()) {
        auto &BM = CurrIterInferBounds[K];
        if (BM.find(IKB.first) != BM.end()) {
          Handled = true;
          if (BM[IKB.first] != IKB.second) {
            BM[IKB.first] = IKB.second;
            if (IKB.second.empty())
              BM.erase(IKB.first);
            IsChanged = true;
          }
        }
      }
      if (!Handled) {
        CurrIterInferBounds[K][IKB.first] = IKB.second;
        if (IKB.second.empty()) {
          CurrIterInferBounds[K].erase(IKB.first);
        } else {
          IsChanged = true;
        }
      }
    }
  } else if (ErrorOccurred) {
    // If any error occurred during inferring bounds then
    // remove any previously inferred bounds for K.
    IsChanged = CurrIterInferBounds.erase(K) != 0;
  }
  return IsChanged;
}
bool AvarBoundsInference::inferBounds(BoundsKey K, AVarGraph &BKGraph,
                                      bool FromPB) {
  bool IsChanged = false;

  if (BI->InvalidBounds.find(K) == BI->InvalidBounds.end()) {
    // Infer from potential bounds?
    if (FromPB) {
      auto &PotBDs = BI->PotentialCntBounds;
      if (PotBDs.find(K) != PotBDs.end()) {
        ProgramVar *Kvar = BI->getProgramVar(K);
        std::set<BoundsKey> PotentialB;
        PotentialB.clear();
        for (auto TK : PotBDs[K]) {
          ProgramVar *TKVar = BI->getProgramVar(TK);
          getReachableBoundKeys(Kvar->getScope(), TK, PotentialB, BKGraph, true);
        }

        if (!PotentialB.empty()) {
          bool Handled = false;
          // Potential bounds are always count bounds.
          // We use potential bounds
          ABounds::BoundsKind PotKind = ABounds::CountBoundKind;
          if (CurrIterInferBounds.find(K) != CurrIterInferBounds.end()) {
            auto &BM = CurrIterInferBounds[K];
            // If we have any inferred bounds for K then ignore potential
            // bounds.
            for (auto &PosB : BM) {
              if (!PosB.second.empty()) {
                Handled = true;
                break;
              }
            }
          }
          if (!Handled) {
            CurrIterInferBounds[K][PotKind] = PotentialB;
            IsChanged = true;
          }
        }
      }
    } else {
      // Infer from the flow-graph.
      std::set<BoundsKey> TmpBkeys;
      // Try to predict bounds from predecessors.
      BKGraph.getPredecessors(K, TmpBkeys);
      IsChanged = predictBounds(K, TmpBkeys, BKGraph);
    }
  }
  return IsChanged;
}

bool AVarBoundsInfo::isValidBoundVariable(clang::Decl *D) {
  if (isa<ParmVarDecl>(D) || isa<FunctionDecl>(D)) {
    // All parameters and return values are valid bound variables.
    return true;
  } else if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
    return !VD->getNameAsString().empty();
  } else if(FieldDecl *FD = dyn_cast<FieldDecl>(D)) {
    return !FD->getNameAsString().empty();
  }
  return false;
}

void AVarBoundsInfo::insertDeclaredBounds(clang::Decl *D, ABounds *B) {
  assert(isValidBoundVariable(D) && "Declaration not a valid bounds variable");
  BoundsKey BK;
  tryGetVariable(D, BK);
  if (B != nullptr) {
    // If there is already bounds information, release it.
    removeBounds(BK);
    BInfo[BK][Declared] = B;
    BoundsInferStats.DeclaredBounds.insert(BK);
  } else {
    // Set bounds to be invalid.
    InvalidBounds.insert(BK);
  }
}

bool AVarBoundsInfo::tryGetVariable(clang::Decl *D, BoundsKey &R) {
  if (isValidBoundVariable(D)) {
    if (ParmVarDecl *PD = dyn_cast<ParmVarDecl>(D)) {
      R = getVariable(PD);
    } else if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
      R = getVariable(VD);
    } else if (FieldDecl *FD = dyn_cast<FieldDecl>(D)) {
      R = getVariable(FD);
    } else if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
      R = getVariable(FD);
    } else {
      assert(false && "Invalid Declaration\n");
    }
    return true;
  }
  return false;
}

bool AVarBoundsInfo::tryGetVariable(clang::Expr *E,
                                    const ASTContext &C,
                                    BoundsKey &Res) {
  llvm::APSInt ConsVal;
  bool Ret = false;
  if (E != nullptr) {
    E = E->IgnoreParenCasts();
    if (E->getType()->isArithmeticType() &&
        E->isIntegerConstantExpr(ConsVal, C)) {
      Res = getVarKey(ConsVal);
      Ret = true;
    } else if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
      auto *D = DRE->getDecl();
      Ret = tryGetVariable(D, Res);
      if (!Ret) {
        assert(false && "Invalid declaration found inside bounds expression");
      }
    } else if (MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
      return tryGetVariable(ME->getMemberDecl(), Res);
    }
    else {
      // assert(false && "Variable inside bounds declaration is an expression");
    }
  }
  return Ret;
}

bool AVarBoundsInfo::mergeBounds(BoundsKey L, BoundsPriority P, ABounds *B) {
  bool RetVal = false;
  if (BInfo.find(L) != BInfo.end() && BInfo[L].find(P) != BInfo[L].end()) {
    // If previous computed bounds are not same? Then release the old bounds.
    if (!BInfo[L][P]->areSame(B, this)) {
      InvalidBounds.insert(L);
      // TODO: Should we keep bounds for other priorities?
      removeBounds(L);
    }
  } else {
    BInfo[L][P] = B;
    RetVal = true;
  }
  return RetVal;
}

bool AVarBoundsInfo::removeBounds(BoundsKey L, BoundsPriority P) {
  bool RetVal = false;
  if (BInfo.find(L) != BInfo.end()) {
    auto &PriBInfo = BInfo[L];
    if (P == Invalid) {
      // Delete bounds for all priorities.
      for (auto &T : PriBInfo) {
        delete (T.second);
      }
      BInfo.erase(L);
      RetVal = true;
    } else {
      // Delete bounds for only the given priority.
      if (PriBInfo.find(P) != PriBInfo.end()) {
        delete (PriBInfo[P]);
        PriBInfo.erase(P);
        RetVal = true;
      }
      // If there are no other bounds then remove the key.
      if (BInfo[L].empty()) {
        BInfo.erase(L);
        RetVal = true;
      }
    }
  }
  return RetVal;
}

bool AVarBoundsInfo::replaceBounds(BoundsKey L, BoundsPriority P, ABounds *B) {
  removeBounds(L);
  return mergeBounds(L, P, B);
}

ABounds *AVarBoundsInfo::getBounds(BoundsKey L, BoundsPriority ReqP,
                                   BoundsPriority *RetP) {
  if (InvalidBounds.find(L) == InvalidBounds.end() &&
      BInfo.find(L) != BInfo.end()) {
    auto &PriBInfo = BInfo[L];
    if (ReqP == Invalid) {
      // Fetch bounds by priority i.e., give the highest priority bounds.
      for (BoundsPriority P : PrioList) {
        if (PriBInfo.find(P) != PriBInfo.end()) {
          if (RetP != nullptr)
            *RetP = P;
          return PriBInfo[P];
        }
      }
      assert(false && "Bounds present but has invalid priority.");
    } else if (PriBInfo.find(ReqP) != PriBInfo.end()) {
      return PriBInfo[ReqP];
    }
  }
  return nullptr;
}

bool AVarBoundsInfo::updatePotentialCountBounds(BoundsKey BK,
                                                std::set<BoundsKey> &CntBK) {
  bool RetVal = false;
  if (!CntBK.empty()) {
    auto &TmpK = PotentialCntBounds[BK];
    TmpK.insert(CntBK.begin(), CntBK.end());
    RetVal = true;
  }
  return RetVal;
}

void AVarBoundsInfo::insertVariable(clang::Decl *D) {
  BoundsKey Tmp;
  tryGetVariable(D, Tmp);
}

BoundsKey AVarBoundsInfo::getVariable(clang::VarDecl *VD) {
  assert(isValidBoundVariable(VD) && "Not a valid bound declaration.");
  PersistentSourceLoc PSL = PersistentSourceLoc::mkPSL(VD, VD->getASTContext());
  if (!hasVarKey(PSL)) {
    BoundsKey NK = ++BCount;
    insertVarKey(PSL, NK);
    const ProgramVarScope *PVS = nullptr;
    if (VD->hasGlobalStorage()) {
      PVS = GlobalScope::getGlobalScope();
    } else {
      FunctionDecl *FD =
          dyn_cast<FunctionDecl>(VD->getParentFunctionOrMethod());
      if (FD != nullptr) {
        PVS = FunctionScope::getFunctionScope(FD->getNameAsString(),
                                              FD->isStatic());
      }
    }
    assert(PVS != nullptr && "Context not null");
    auto *PVar =
      ProgramVar::createNewProgramVar(NK, VD->getNameAsString(), PVS);
    insertProgramVar(NK, PVar);
    if (VD->getType()->isPointerType())
      PointerBoundsKey.insert(NK);
  }
  return getVarKey(PSL);
}

BoundsKey AVarBoundsInfo::getVariable(clang::ParmVarDecl *PVD) {
  assert(isValidBoundVariable(PVD) && "Not a valid bound declaration.");
  FunctionDecl *FD = dyn_cast<FunctionDecl>(PVD->getDeclContext());
  unsigned int ParamIdx = getParameterIndex(PVD, FD);
  auto Psl = PersistentSourceLoc::mkPSL(FD, FD->getASTContext());
  std::string FileName = Psl.getFileName();
  auto ParamKey = std::make_tuple(FD->getNameAsString(), FileName,
                                  FD->isStatic(), ParamIdx);
  if (ParamDeclVarMap.left().find(ParamKey) == ParamDeclVarMap.left().end()) {
    BoundsKey NK = ++BCount;
    const FunctionParamScope *FPS =
          FunctionParamScope::getFunctionParamScope(FD->getNameAsString(),
                                                  FD->isStatic());
    std::string ParamName = PVD->getNameAsString();
    // If this is a parameter without name!?
    // Just get the name from argument number.
    if (ParamName.empty())
      ParamName = "NONAMEPARAM_" + std::to_string(ParamIdx);

    auto *PVar = ProgramVar::createNewProgramVar(NK, ParamName, FPS);
    insertProgramVar(NK, PVar);
    insertParamKey(ParamKey, NK);
    if (PVD->getType()->isPointerType())
      PointerBoundsKey.insert(NK);
  }
  return ParamDeclVarMap.left().at(ParamKey);
}

BoundsKey AVarBoundsInfo::getVariable(clang::FunctionDecl *FD) {
  assert(isValidBoundVariable(FD) && "Not a valid bound declaration.");
  auto Psl = PersistentSourceLoc::mkPSL(FD, FD->getASTContext());
  std::string FileName = Psl.getFileName();
  auto FuncKey = std::make_tuple(FD->getNameAsString(), FileName,
                                 FD->isStatic());
  if (FuncDeclVarMap.left().find(FuncKey) == FuncDeclVarMap.left().end()) {
    BoundsKey NK = ++BCount;
    const FunctionParamScope *FPS =
          FunctionParamScope::getFunctionParamScope(FD->getNameAsString(),
                                                  FD->isStatic());

    auto *PVar =
      ProgramVar::createNewProgramVar(NK, FD->getNameAsString(), FPS);
    insertProgramVar(NK, PVar);
    FuncDeclVarMap.insert(FuncKey, NK);
    if (FD->getReturnType()->isPointerType())
      PointerBoundsKey.insert(NK);
  }
  return FuncDeclVarMap.left().at(FuncKey);
}

BoundsKey AVarBoundsInfo::getVariable(clang::FieldDecl *FD) {
  assert(isValidBoundVariable(FD) && "Not a valid bound declaration.");
  PersistentSourceLoc PSL =
    PersistentSourceLoc::mkPSL(FD, FD->getASTContext());
  if (!hasVarKey(PSL)) {
    BoundsKey NK = ++BCount;
    insertVarKey(PSL, NK);
    std::string StName = FD->getParent()->getNameAsString();
    const StructScope *SS = StructScope::getStructScope(StName);
    auto *PVar = ProgramVar::createNewProgramVar(NK, FD->getNameAsString(), SS);
    insertProgramVar(NK, PVar);
    if (FD->getType()->isPointerType())
      PointerBoundsKey.insert(NK);
  }
  return getVarKey(PSL);
}

BoundsKey AVarBoundsInfo::getRandomBKey() {
  BoundsKey Ret = ++BCount;
  TmpBoundsKey.insert(Ret);
  return Ret;
}

bool AVarBoundsInfo::addAssignment(clang::Decl *L, clang::Decl *R) {
  BoundsKey BL, BR;
  if (tryGetVariable(L, BL) && tryGetVariable(R, BR)) {
    return addAssignment(BL, BR);
  }
  return false;
}

bool AVarBoundsInfo::addAssignment(clang::DeclRefExpr *L,
                                   clang::DeclRefExpr *R) {
  return addAssignment(L->getDecl(), R->getDecl());
}

bool AVarBoundsInfo::handleAssignment(clang::Expr *L, const CVarSet &LCVars,
                                      const std::set<BoundsKey> &CSLKeys,
                                      clang::Expr *R, const CVarSet &RCVars,
                                      const std::set<BoundsKey> &CSRKeys,
                                      ASTContext *C, ConstraintResolver *CR) {
  bool Ret = false;
  BoundsKey TmpK;
  std::set<BoundsKey> AllLKeys = CSLKeys;
  std::set<BoundsKey> AllRKeys = CSRKeys;
  if (AllLKeys.empty() && (CR->resolveBoundsKey(LCVars, TmpK) ||
                           tryGetVariable(L, *C, TmpK))) {
    AllLKeys.insert(TmpK);
  }
  if (AllRKeys.empty() && (CR->resolveBoundsKey(RCVars, TmpK) ||
                           tryGetVariable(R, *C, TmpK))) {
    AllRKeys.insert(TmpK);
  }

  for (auto LK : AllLKeys) {
    for (auto RK : AllRKeys) {
      Ret = addAssignment(LK, RK) || Ret;
    }
  }
  return Ret;
}

bool AVarBoundsInfo::handleAssignment(clang::Decl *L, CVarOption LCVars,
                                      clang::Expr *R, const CVarSet &RCVars,
                                      const std::set<BoundsKey> &CSRKeys,
                                      ASTContext *C, ConstraintResolver *CR) {
  BoundsKey LKey, RKey;
  bool Ret = false;
  if (CR->resolveBoundsKey(LCVars, LKey) ||
      tryGetVariable(L, LKey)) {
    std::set<BoundsKey> AllRKeys = CSRKeys;
    if (AllRKeys.empty() && (CR->resolveBoundsKey(RCVars, RKey) ||
       tryGetVariable(R, *C, RKey))) {
      AllRKeys.insert(RKey);
    }
    for (auto RK : AllRKeys) {
      Ret = addAssignment(LKey, RK) || Ret;
    }
  }
  return Ret;
}

bool AVarBoundsInfo::addAssignment(BoundsKey L, BoundsKey R) {
  // If we are adding to function return, do not add bi-directional edges.
  if (isFunctionReturn(L) || isFunctionReturn(R)) {
    // Do not assign edge from return to itself.
    // This is because while inferring bounds of return value, we expect
    // all the variables used in return values to have bounds.
    // So, if we create a edge from return to itself then we create a cyclic
    // dependency and never will be able to find the bounds for the return
    // value.
    if (L != R)
      ProgVarGraph.addEdge(R, L);
  } else {
    ProgVarGraph.addEdge(L, R);
    ProgVarGraph.addEdge(R, L);
  }
  return true;
}

// Visitor to collect all the variables that are used during the life-time
// of the visitor.
// This class also has a flag that gets set when a variable is observed
// more than once.
class CollectDeclsVisitor : public RecursiveASTVisitor<CollectDeclsVisitor> {
public:

  std::set<VarDecl*> ObservedDecls;
  std::set<std::string> StructAccess;

  explicit CollectDeclsVisitor(ASTContext *Ctx) : C(Ctx) {
    ObservedDecls.clear();
    StructAccess.clear();
  }
  virtual ~CollectDeclsVisitor() {
    ObservedDecls.clear();
  }

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    VarDecl *VD = dyn_cast_or_null<VarDecl>(DRE->getDecl());
    if (VD != nullptr) {
      ObservedDecls.insert(VD);
    }
    return true;
  }

  // For a->b; We need to get `a->b`
  bool VisitMemberExpr(MemberExpr *ME) {
    std::string MAccess = getSourceText(ME->getSourceRange(), *C);
    if (!MAccess.empty()) {
      StructAccess.insert(MAccess);
    }
    return false;
  }

private:
  ASTContext *C;
};

bool
AVarBoundsInfo::handlePointerAssignment(clang::Stmt *St, clang::Expr *L,
                                        clang::Expr *R,
                                        ASTContext *C,
                                        ConstraintResolver *CR) {
  CollectDeclsVisitor LVarVis(C);
  LVarVis.TraverseStmt(L->getExprStmt());

  CollectDeclsVisitor RVarVis(C);
  RVarVis.TraverseStmt(R->getExprStmt());

  std::set<VarDecl *> CommonVars;
  std::set<std::string> CommonStVars;
  findIntersection(LVarVis.ObservedDecls, RVarVis.ObservedDecls, CommonVars);
  findIntersection(LVarVis.StructAccess, RVarVis.StructAccess, CommonStVars);

  if (!CommonVars.empty() || CommonStVars.empty()) {
    for (auto *LHSCVar : CR->getExprConstraintVarsSet(L)) {
      if (LHSCVar->hasBoundsKey())
        ArrPointerBoundsKey.insert(LHSCVar->getBoundsKey());
    }
  }
  return true;
}

void
AVarBoundsInfo::recordArithmeticOperation(clang::Expr *E,
                                          ConstraintResolver *CR) {
  CVarSet CSet = CR->getExprConstraintVarsSet(E);
  for (auto *CV : CSet) {
    if (CV->hasBoundsKey())
      ArrPointersWithArithmetic.insert(CV->getBoundsKey());
  }
}

bool AVarBoundsInfo::hasPointerArithmetic(BoundsKey BK) {
  return ArrPointersWithArithmetic.find(BK) != ArrPointersWithArithmetic.end();
}

ProgramVar *AVarBoundsInfo::getProgramVar(BoundsKey VK) {
  ProgramVar *Ret = nullptr;
  if (PVarInfo.find(VK) != PVarInfo.end()) {
    Ret = PVarInfo[VK];
  }
  return Ret;
}

void AVarBoundsInfo::brainTransplant(BoundsKey NewBK, BoundsKey OldBK) {
  // Here, we use the ProgramVar of NewBK and use it for OldBK.
  if (NewBK != OldBK) {
    ProgramVar *NewPVar = getProgramVar(NewBK);
    insertProgramVar(OldBK, NewPVar);
  }
}

bool AVarBoundsInfo::hasVarKey(PersistentSourceLoc &PSL) {
  return DeclVarMap.left().find(PSL) != DeclVarMap.left().end();
}

BoundsKey AVarBoundsInfo::getVarKey(PersistentSourceLoc &PSL) {
  assert (hasVarKey(PSL) && "VarKey doesn't exist");
  return DeclVarMap.left().at(PSL);
}

BoundsKey AVarBoundsInfo::getConstKey(uint64_t value) {
  BoundsKey NK = ++BCount;
  std::string ConsString = std::to_string(value);
  ProgramVar *NPV =
    ProgramVar::createNewProgramVar(NK,
                                    ConsString,
                                    GlobalScope::getGlobalScope(),
                                    true);
  insertProgramVar(NK, NPV);
  ConstVarKeys[value].insert(NK);
  return NK;
}

bool AVarBoundsInfo::fetchAllConstKeys(uint64_t value,
                                       std::set<BoundsKey> &AllKeys) {
  if (ConstVarKeys.find(value) != ConstVarKeys.end()) {
    AllKeys.insert(ConstVarKeys[value].begin(), ConstVarKeys[value].end());
    return true;
  }
  return false;
}

BoundsKey AVarBoundsInfo::getVarKey(llvm::APSInt &API) {
  return getConstKey(API.abs().getZExtValue());
}

void AVarBoundsInfo::insertVarKey(PersistentSourceLoc &PSL, BoundsKey NK) {
  DeclVarMap.insert(PSL, NK);
}

void AVarBoundsInfo::insertParamKey(AVarBoundsInfo::ParamDeclType ParamDecl,
                                    BoundsKey NK) {
  ParamDeclVarMap.insert(ParamDecl, NK);
}

void AVarBoundsInfo::insertProgramVar(BoundsKey NK, ProgramVar *PV) {
  PVarInfo[NK] = PV;
}

bool
AVarBoundsInfo::performWorkListInference(const std::set<BoundsKey> &ArrNeededBounds,
                                         AVarGraph &BKGraph,
                                         AvarBoundsInference &BI,
                                         bool FromPB) {
  bool RetVal = false;
  std::set<BoundsKey> WorkList;
  WorkList.insert(ArrNeededBounds.begin(), ArrNeededBounds.end());
  std::set<BoundsKey> NextIterArrs;
  bool Changed = true;
  while (Changed) {
    Changed = false;
    NextIterArrs.clear();
    // Are there any ARR atoms that need bounds?
    while (!WorkList.empty()) {
      BoundsKey CurrArrKey = *WorkList.begin();
      // Remove the bounds key from the worklist.
      WorkList.erase(CurrArrKey);
      // Can we find bounds for this Arr?
      if (BI.inferBounds(CurrArrKey, BKGraph, FromPB)) {
        RetVal = true;
        Changed = true;
        // Get all the successors of the ARR whose bounds we just found.
        BKGraph.getSuccessors(CurrArrKey, NextIterArrs);
      }
    }
    if (Changed) {
      findIntersection(ArrNeededBounds, NextIterArrs, WorkList);
    }
  }
  return RetVal;
}

BoundsKey
AVarBoundsInfo::getCtxSensCEBoundsKey(const PersistentSourceLoc &PSL,
                                      BoundsKey BK) {
  return CSBKeyHandler.getCtxSensCEBoundsKey(PSL, BK);
}

void AVarBoundsInfo::computerArrPointers(ProgramInfo *PI,
                                         std::set<BoundsKey> &ArrPointers) {
  auto &CS = PI->getConstraints();
  for (auto Bkey : PointerBoundsKey) {
    // Regular variables.
    auto &BkeyToPSL = DeclVarMap.right();
    if (BkeyToPSL.find(Bkey) != BkeyToPSL.end()) {
      auto &PSL = BkeyToPSL.at(Bkey);
      if (hasArray(PI->getVarMap().at(PSL), CS)) {
        ArrPointers.insert(Bkey);
      }
      // Does this array belongs to a valid program variable?
      if (isInSrcArray(PI->getVarMap().at(PSL), CS)) {
        InProgramArrPtrBoundsKeys.insert(Bkey);
      }
      continue;
    }

    // Function parameters
    auto &ParmBkeyToPSL = ParamDeclVarMap.right();
    if (ParmBkeyToPSL.find(Bkey) != ParmBkeyToPSL.end()) {
      auto &ParmTup = ParmBkeyToPSL.at(Bkey);
      std::string FuncName = std::get<0>(ParmTup);
      std::string FileName = std::get<1>(ParmTup);
      bool IsStatic = std::get<2>(ParmTup);
      unsigned ParmNum = std::get<3>(ParmTup);
      FVConstraint *FV = nullptr;
      if (IsStatic || !PI->getExtFuncDefnConstraint(FuncName)) {
        FV = PI->getStaticFuncConstraint(FuncName, FileName);
      } else {
        FV = PI->getExtFuncDefnConstraint(FuncName);
      }

      if (hasArray(FV->getParamVar(ParmNum), CS)) {
        ArrPointers.insert(Bkey);
      }
      // Does this array belongs to a valid program variable?
      if (isInSrcArray(FV->getParamVar(ParmNum), CS)) {
        InProgramArrPtrBoundsKeys.insert(Bkey);
      }

      continue;
    }
    // Function returns.
    auto &FuncKeyToPSL = FuncDeclVarMap.right();
    if (FuncKeyToPSL.find(Bkey) != FuncKeyToPSL.end()) {
      auto &FuncRet = FuncKeyToPSL.at(Bkey);
      std::string FuncName = std::get<0>(FuncRet);
      std::string FileName = std::get<1>(FuncRet);
      bool IsStatic = std::get<2>(FuncRet);
      const FVConstraint *FV = nullptr;
      std::set<FVConstraint *> Tmp;
      Tmp.clear();
      if (IsStatic || !PI->getExtFuncDefnConstraint(FuncName)) {
        Tmp.insert(PI->getStaticFuncConstraint(FuncName, FileName));
        FV = getOnly(Tmp);
      } else {
        Tmp.insert(PI->getExtFuncDefnConstraint(FuncName));
        FV = getOnly(Tmp);
      }

      if (hasArray(FV->getReturnVar(), CS)) {
        ArrPointers.insert(Bkey);
      }
      // Does this array belongs to a valid program variable?
      if (isInSrcArray(FV->getReturnVar(), CS)) {
        InProgramArrPtrBoundsKeys.insert(Bkey);
      }
      continue;
    }
  }

  // Get all context-sensitive BoundsKey for each of the actual BKs
  // and consider them to be array pointers as well.
  // Since context-sensitive BoundsKey will be immediate children
  // of the regular bounds key, we just get the neighbours (predecessors
  // and successors) of the regular bounds key to get the context-sensitive
  // counterparts.
  std::set<BoundsKey> CtxSensBKeys;
  CtxSensBKeys.clear();
  std::set<BoundsKey> TmpBKeys, TmpBKeysF;
  for (auto BK : ArrPointers) {
    TmpBKeys.clear();
    ProgVarGraph.getPredecessors(BK, TmpBKeys);
    TmpBKeysF.insert(TmpBKeys.begin(), TmpBKeys.end());
    TmpBKeys.clear();
    ProgVarGraph.getSuccessors(BK, TmpBKeys);
    TmpBKeysF.insert(TmpBKeys.begin(), TmpBKeys.end());
    for (auto TBK : TmpBKeysF) {
      ProgramVar *TmpPVar = getProgramVar(TBK);
      if (TmpPVar != nullptr) {
        if (isa<CtxFunctionArgScope>(TmpPVar->getScope())) {
          CtxSensBKeys.insert(TBK);
        }
        if (isa<CtxStructScope>(TmpPVar->getScope())) {
          CtxSensBKeys.insert(TBK);
        }
      }
    }
  }

  ArrPointers.insert(CtxSensBKeys.begin(), CtxSensBKeys.end());
}

void
AVarBoundsInfo::getBoundsNeededArrPointers(const std::set<BoundsKey> &ArrPtrs,
                                           std::set<BoundsKey> &AB) {
  // Next, get the ARR pointers that has bounds.
  // These are pointers with bounds.
  std::set<BoundsKey> ArrWithBounds;
  for (auto &T : BInfo) {
    ArrWithBounds.insert(T.first);
  }
  // Also add arrays with invalid bounds.
  ArrWithBounds.insert(InvalidBounds.begin(), InvalidBounds.end());

  // This are the array atoms that need bounds.
  // i.e., AB = ArrPtrs - ArrPtrsWithBounds.
  std::set_difference(ArrPtrs.begin(), ArrPtrs.end(),
                      ArrWithBounds.begin(), ArrWithBounds.end(),
                      std::inserter(AB, AB.end()));
}

bool AVarBoundsInfo::performFlowAnalysis(ProgramInfo *PI) {
  bool RetVal = false;
  AvarBoundsInference ABI(this);
  // First get all the pointer vars which are ARRs
  std::set<BoundsKey> ArrPointers;
  computerArrPointers(PI, ArrPointers);

  // Repopulate array bounds key.
  ArrPointerBoundsKey.clear();
  ArrPointerBoundsKey.insert(ArrPointers.begin(), ArrPointers.end());

  // Keep only highest priority bounds.
  // Any thing changed? which means bounds of a variable changed
  // Which means we need to recompute the flow based bounds for
  // all arrays that have flow based bounds.
  if (keepHighestPriorityBounds(ArrPointerBoundsKey)) {
    // Remove flow inferred bounds, if exist for all the array pointers.
    for (auto TBK : ArrPointerBoundsKey)
      removeBounds(TBK, FlowInferred);
  }

  std::set<BoundsKey> ArrNeededBounds, ArrNeededBoundsNew;
  ArrNeededBounds.clear();


  getBoundsNeededArrPointers(ArrPointers, ArrNeededBounds);

  bool Changed = !ArrNeededBounds.empty();

  // Now compute the bounds information of all the ARR pointers that need it.
  // We iterate until there are no new array variables whose bounds are found.
  // The expectation is every iteration we will find bounds for at least
  // one array variable.
  while (Changed) {
    // Clear all inferred bounds.
    ABI.clearInferredBounds();
    // Regular flow inference.
    performWorkListInference(ArrNeededBounds, this->ProgVarGraph, ABI);
    // Flow inference using potential bounds.
    performWorkListInference(ArrNeededBounds, this->ProgVarGraph, ABI, true);
    // Flow inference from context sensitive keys to original keys.
    performWorkListInference(ArrNeededBounds, this->CtxSensProgVarGraph, ABI);
    // Now, by using potential bounds.
    performWorkListInference(ArrNeededBounds, this->CtxSensProgVarGraph, ABI, true);

    // From all the sets of bounds computed for various array variables.
    // Intersect them and find the common bound variable.
    ABI.convergeInferredBounds();

    ArrNeededBoundsNew.clear();
    // Get array variables that still need bounds.
    getBoundsNeededArrPointers(ArrPointers, ArrNeededBoundsNew);
    // Did we find bounds for new array variables?
    Changed = ArrNeededBounds != ArrNeededBoundsNew;
    if (ArrNeededBounds.size() == ArrNeededBoundsNew.size()) {
      assert (!Changed && "New arrays needed bounds after inference");
    }
    assert (ArrNeededBoundsNew.size() <= ArrNeededBounds.size() &&
            "We should always have less number of arrays whose bounds needs "
            "to be inferred after each round.");
    ArrNeededBounds = ArrNeededBoundsNew;
  }


  return RetVal;
}

bool AVarBoundsInfo::keepHighestPriorityBounds(std::set<BoundsKey> &ArrPtrs) {
  bool FoundBounds = false;
  bool HasChanged = false;
  for (auto BK : ArrPtrs) {
    FoundBounds = false;
    for (BoundsPriority P : PrioList) {
      if (FoundBounds) {
        // We already found bounds. So delete these bounds.
        HasChanged = removeBounds(BK, P) || HasChanged;
      } else if (getBounds(BK, P) != nullptr) {
        FoundBounds = true;
      }
    }
  }
  return HasChanged;
}

void AVarBoundsInfo::dumpAVarGraph(const std::string &DFPath) {
  std::error_code Err;
  llvm::raw_fd_ostream DotFile(DFPath, Err);
  llvm::WriteGraph(DotFile, ProgVarGraph);
  DotFile.close();
}

bool AVarBoundsInfo::isFunctionReturn(BoundsKey BK) {
  return (FuncDeclVarMap.right().find(BK) != FuncDeclVarMap.right().end());
}

void AVarBoundsInfo::print_stats(llvm::raw_ostream &O,
                                 const CVarSet &SrcCVarSet,
                                 bool JsonFormat) const {
  std::set<BoundsKey> InSrcBKeys, InSrcArrBKeys, Tmp;
  for (auto *C : SrcCVarSet) {
    if (C->isForValidDecl() && C->hasBoundsKey())
      InSrcBKeys.insert(C->getBoundsKey());
  }
  findIntersection(InProgramArrPtrBoundsKeys, InSrcBKeys, InSrcArrBKeys);
  if (!JsonFormat) {
    findIntersection(ArrPointerBoundsKey, InSrcArrBKeys, Tmp);
    O << "NumPointersNeedBounds:" << Tmp.size() << ",\n";
    O << "Details:\n";
    findIntersection(InvalidBounds, InSrcArrBKeys, Tmp);
    O << "Invalid:" << Tmp.size() << "\n,BoundsFound:\n";
    BoundsInferStats.print(O, &InSrcArrBKeys, JsonFormat);
  } else {
    findIntersection(ArrPointerBoundsKey, InSrcArrBKeys, Tmp);
    O << "{\"NumPointersNeedBounds\":" << Tmp.size() << ",";
    O << "\"Details\":{";
    findIntersection(InvalidBounds, InSrcArrBKeys, Tmp);
    O << "\"Invalid\":" << Tmp.size() << ",\"BoundsFound\":{";
    BoundsInferStats.print(O, &InSrcArrBKeys, JsonFormat);
    O << "}";
    O << "}";
    O << "}";
  }
}

bool AVarBoundsInfo::areSameProgramVar(BoundsKey B1, BoundsKey B2) {
  if (B1 != B2) {
    ProgramVar *P1 = getProgramVar(B1);
    ProgramVar *P2 = getProgramVar(B2);
    return P1->IsNumConstant() && P2->IsNumConstant() &&
           P1->getVarName() == P2->getVarName();
  }
  return B1 == B2;
}

std::set<BoundsKey>
AVarBoundsInfo::getCtxSensFieldBoundsKey(Expr *E, ASTContext *C,
                                         ProgramInfo &I) {
  std::set<BoundsKey> Ret;
  if (MemberExpr *ME = dyn_cast_or_null<MemberExpr>(E->IgnoreParenCasts())) {
    BoundsKey NewBK;
    if (CSBKeyHandler.tryGetFieldCSKey(ME, C, I, NewBK))
      Ret.insert(NewBK);
  }
  return Ret;
}