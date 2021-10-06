#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/KnownBits.h"

#include "souper/Infer/Preconditions.h"
#include "souper/Infer/EnumerativeSynthesis.h"
#include "souper/Infer/ConstantSynthesis.h"
#include "souper/Inst/InstGraph.h"
#include "souper/Parser/Parser.h"
#include "souper/Tool/GetSolver.h"
#include "souper/Util/DfaUtils.h"

using namespace llvm;
using namespace souper;

unsigned DebugLevel;

static cl::opt<unsigned, /*ExternalStorage=*/true>
DebugFlagParser("souper-debug-level",
     cl::desc("Control the verbose level of debug output (default=1). "
     "The larger the number is, the more fine-grained debug "
     "information will be printed."),
     cl::location(DebugLevel), cl::init(1));

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input souper optimization>"),
              cl::init("-"));

static llvm::cl::opt<bool> Reduce("reduce",
    llvm::cl::desc("Try to reduce the number of instructions by replacing instructions with variables."
                   "(default=false)"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> ReducePrintAll("reduce-all-results",
    llvm::cl::desc("Print all reduced results."
                   "(default=false)"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> SymbolizeConstant("symbolize",
    llvm::cl::desc("Try to replace a concrete constant with a symbolic constant."
                   "(default=false)"),
    llvm::cl::init(false));

static llvm::cl::opt<size_t> SymbolizeNumInsts("symbolize-num-insts",
    llvm::cl::desc("Number of instructions to synthesize"
                   "(default=1)"),
    llvm::cl::init(1));

static llvm::cl::opt<bool> SymbolizeNoDFP("symbolize-no-dataflow",
    llvm::cl::desc("Do not generate optimizations with dataflow preconditions."),
    llvm::cl::init(false));

static llvm::cl::opt<bool> FixIt("fixit",
    llvm::cl::desc("Given an invalid optimization, generate a valid one."
                   "(default=false)"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> GeneralizeWidth("generalize-width",
    llvm::cl::desc("Given a valid optimization, generalize bitwidth."
                   "(default=false)"),
    llvm::cl::init(false));

static cl::opt<size_t> NumResults("generalization-num-results",
    cl::desc("Number of Generalization Results"),
    cl::init(5));

void Generalize(InstContext &IC, Solver *S, ParsedReplacement Input) {
  bool FoundWP = false;
  std::vector<std::map<Inst *, llvm::KnownBits>> KBResults;
  std::vector<std::map<Inst *, llvm::ConstantRange>> CRResults;
  S->abstractPrecondition(Input.BPCs, Input.PCs, Input.Mapping, IC, FoundWP, KBResults, CRResults);

  if (FoundWP && KBResults.empty() && CRResults.empty()) {
    Input.print(llvm::outs(), true);
  } else if (!KBResults.empty()) {
    for (auto &&Result : KBResults) { // Each result is a disjunction
      for (auto &Pair: Result) {
        Pair.first->KnownOnes = Pair.second.One;
        Pair.first->KnownZeros = Pair.second.Zero;
      }
      Input.print(llvm::outs(), true);
    }
  } else if (!CRResults.empty()) {
    for (auto &&Result : CRResults) { // Each result is a disjunction
      for (auto &Pair: Result) {
        Pair.first->Range = Pair.second;
      }
      Input.print(llvm::outs(), true);
    }
  }
}


void SymbolizeAndGeneralize(InstContext &IC, Solver *S, ParsedReplacement Input,
                            std::vector<Inst *> LHSConsts,
                            std::vector<Inst *> RHSConsts,
                            CandidateMap &Results) {
  std::map<Inst *, Inst *> InstCache;
  std::vector<Inst *> FakeConsts;
  for (size_t i = 0; i < LHSConsts.size(); ++i) {
    FakeConsts.push_back(
          IC.createVar(LHSConsts[i]->Width, "fakeconst_" + std::to_string(i)));
    InstCache[LHSConsts[i]] = FakeConsts[i];
  }

  // Does it makes sense for the expression to depend on other variables?
  // If yes, expand the third argument to include inputs
  EnumerativeSynthesis ES;
  auto Guesses = ES.generateExprs(IC, SymbolizeNumInsts, FakeConsts,
                                  RHSConsts[0]->Width);

  std::vector<std::vector<std::map<Inst *, llvm::KnownBits>>>
      Preconditions;

  std::map<Block *, Block *> BlockCache;
  std::map<Inst *, APInt> ConstMap;
  auto LHS = getInstCopy(Input.Mapping.LHS, IC, InstCache,
                         BlockCache, &ConstMap, false);

  std::vector<Inst *> WithoutConsts;

  for (auto &Guess : Guesses) {
    std::set<Inst *> ConstSet;
    std::map <Inst *, llvm::APInt> ResultConstMap;
    souper::getConstants(Guess, ConstSet);
    if (!ConstSet.empty()) {
      std::map<Inst *, Inst *> InstCacheCopy = InstCache;
      InstCacheCopy[RHSConsts[0]] = Guess;
      auto RHS = getInstCopy(Input.Mapping.RHS, IC, InstCacheCopy,
                             BlockCache, &ConstMap, false);
      ConstantSynthesis CS;
      auto SMTSolver = GetUnderlyingSolver();
      auto EC = CS.synthesize(SMTSolver.get(), Input.BPCs, Input.PCs,
                           InstMapping (LHS, RHS), ConstSet,
                           ResultConstMap, IC, /*MaxTries=*/30, 10,
                           /*AvoidNops=*/true);
      if (!ResultConstMap.empty()) {
        std::map<Inst *, Inst *> InstCache;
        std::map<Block *, Block *> BlockCache;
        RHS = getInstCopy(RHS, IC, InstCache, BlockCache, &ResultConstMap, false);

        Results.push_back(CandidateReplacement(/*Origin=*/nullptr, InstMapping(LHS, RHS)));

      } else {
        if (DebugLevel > 2) {
          llvm::errs() << "Costant Synthesis ((no Dataflow Preconditions)) failed. \n";
        }
      }
    } else {
      WithoutConsts.push_back(Guess);
    }
  }
  std::swap(WithoutConsts, Guesses);

  for (auto &Guess : Guesses) {
    std::map<Inst *, Inst *> InstCacheCopy = InstCache;
    InstCacheCopy[RHSConsts[0]] = Guess;

    auto RHS = getInstCopy(Input.Mapping.RHS, IC, InstCacheCopy,
                           BlockCache, &ConstMap, false);

    std::vector<std::map<Inst *, llvm::KnownBits>> KBResults;
    std::vector<std::map<Inst *, llvm::ConstantRange>> CRResults;
    bool FoundWP = false;
    if (!SymbolizeNoDFP) {
      InstMapping Mapping(LHS, RHS);
      S->abstractPrecondition(Input.BPCs, Input.PCs, Mapping, IC, FoundWP, KBResults, CRResults);
    }
    Preconditions.push_back(KBResults);
    if (!FoundWP) {
      Guess = nullptr; // TODO: Better failure indicator
    } else {
      Guess = RHS;
    }
  }

  std::vector<size_t> Idx;
  std::vector<int> Utility;
  for (size_t i = 0; i < Guesses.size(); ++i) {
    Idx.push_back(i);
  }
  for (size_t i = 0; i < Preconditions.size(); ++i) {
    Utility.push_back(0);
    if (!Guesses[i]) continue;
    if (Preconditions[i].empty()) {
      Utility[i] = 1000; // High magic number
    }

    for (auto V : Preconditions[i]) {
      for (auto P : V) {
        auto W = P.second.getBitWidth();
        Utility[i] += (W - P.second.Zero.countPopulation());
        Utility[i] += (W - P.second.One.countPopulation());
      }
    }
  }

  std::sort(Idx.begin(), Idx.end(), [&Utility](size_t a, size_t b) {
    return Utility[a] > Utility[b];
  });

  for (size_t i = 0; i < Idx.size(); ++i) {
    if (Preconditions[Idx[i]].empty() && Guesses[Idx[i]]) {
      Results.push_back(CandidateReplacement(/*Origin=*/nullptr, InstMapping(LHS, Guesses[Idx[i]])));
    }
  }

  // if (!SymbolizeNoDFP) {
  //   for (size_t i = 0; i < std::min(Idx.size(), NumResults.getValue()); ++i) {
  //     for (auto Computed : Preconditions[Idx[i]]) {
  //       for (auto Pair : Computed) {
  //         Pair.first->KnownOnes = Pair.second.One;
  //         Pair.first->KnownZeros = Pair.second.Zero;
  //       }
  //       Results.push_back(CandidateReplacement(/*Origin=*/nullptr, InstMapping(LHS, Guesses[Idx[i]])));
  //     }
  //   }
  // }
}

void SymbolizeAndGeneralize(InstContext &IC,
                            Solver *S, ParsedReplacement Input) {
  std::vector<Inst *> LHSConsts, RHSConsts;
  auto Pred = [](Inst *I) {return I->K == Inst::Const;};
  findInsts(Input.Mapping.LHS, LHSConsts, Pred);
  findInsts(Input.Mapping.RHS, RHSConsts, Pred);

  CandidateMap Results;

  // One at a time
  for (auto LHSConst : LHSConsts) {
    SymbolizeAndGeneralize(IC, S, Input, {LHSConst}, RHSConsts, Results);
  }
  // TODO: Two at a time, etc. Is this replaceable by DFP?

  // All at once
  SymbolizeAndGeneralize(IC, S, Input, LHSConsts, RHSConsts, Results);

  // TODO: Move sorting here
  for (auto &&Result : Results) {
    Result.print(llvm::outs(), true);
    llvm::outs() << "\n";
  }
}

size_t InferWidth(Inst::Kind K, const std::vector<Inst *> &Ops) {
  switch (K) {
    case Inst::And:
    case Inst::Or:
    case Inst::Xor:
    case Inst::Sub:
    case Inst::Mul:
    case Inst::Add: return Ops[0]->Width;
    case Inst::Slt:
    case Inst::Sle:
    case Inst::Ult:
    case Inst::Ule: return 1;
    default: llvm_unreachable((std::string("Unimplemented ") + Inst::getKindName(K)).c_str());
  }
}

Inst *CloneInst(InstContext &IC, Inst *I, std::map<Inst *, size_t> &WidthMap) {
  if (I->K == Inst::Var) {
    return IC.createVar(WidthMap[I], I->Name); // TODO other attributes
  } else if (I->K == Inst::Const) {
    llvm_unreachable("Const");
  } else {
    std::vector<Inst *> Ops;
    for (auto Op : I->Ops) {
      Ops.push_back(CloneInst(IC, Op, WidthMap));
    }
    return IC.getInst(I->K, InferWidth(I->K, Ops), Ops);
  }
}

void GeneralizeBitWidth(InstContext &IC, Solver *S,
                     ParsedReplacement Input) {
  auto Vars = IC.getVariablesFor(Input.Mapping.LHS);

  assert(Vars.size() == 1 && "Multiple variables unimplemented.");

  std::map<Inst *, size_t> WidthMap;

  for (int i = 1; i < 64; ++i) {
    WidthMap[Vars[0]] = i;
    auto LHS = CloneInst(IC, Input.Mapping.LHS, WidthMap);
    auto RHS = CloneInst(IC, Input.Mapping.RHS, WidthMap);

    ReplacementContext RC;
    auto str = RC.printInst(LHS, llvm::outs(), true);
    llvm::outs() << "infer " << str << "\n";
    str = RC.printInst(RHS, llvm::outs(), true);
    llvm::outs() << "result " << str << "\n\n";
  }

}

void collectInsts(Inst *I, std::unordered_set<Inst *> &Results) {
  std::vector<Inst *> Stack{I};
  while (!Stack.empty()) {
    auto Current = Stack.back();
    Stack.pop_back();

    Results.insert(Current);

    for (auto Child : Current->Ops) {
      if (Results.find(Child) == Results.end()) {
        Stack.push_back(Child);
      }
    }
  }
}
void ReduceRec(InstContext &IC,
     Solver *S, ParsedReplacement Input_, std::vector<ParsedReplacement> &Results, std::unordered_set<std::string> &DNR) {
  auto Str = Input_.getString(false);
  if (DNR.find(Str) != DNR.end()) {
    return;
  } else {
    DNR.insert(Str);
  }

  static int varnum = 0; // persistent state, maybe 'oop' away at some point.

  std::unordered_set<Inst *> Insts;
  collectInsts(Input_.Mapping.LHS, Insts);
  collectInsts(Input_.Mapping.RHS, Insts);

  for (auto &&PC : Input_.PCs) {
    collectInsts(PC.LHS, Insts);
    collectInsts(PC.RHS, Insts);
  }

  for (auto &&BPC : Input_.BPCs) {
    collectInsts(BPC.PC.LHS, Insts);
    collectInsts(BPC.PC.RHS, Insts);
  }

  if (Insts.size() <= 1) {
    return; // Base case
  }

  // Remove at least one instruction and call recursively for valid opts
  for (auto I : Insts) {
    ParsedReplacement Input = Input_;

    if (I == Input.Mapping.LHS || I == Input.Mapping.RHS || I->K == Inst::Var || I->K == Inst::Const) {
      continue;
    }

    // Try to replace I with a new Var.
    Inst *NewVar = IC.createVar(I->Width, "newvar" + std::to_string(varnum++));

    std::map<Inst *, Inst *> ICache;
    ICache[I] = NewVar;

    std::map<Block *, Block *> BCache;
    std::map<Inst *, llvm::APInt> CMap;

    ParsedReplacement NewInst = Input;

    Input.Mapping.LHS = getInstCopy(Input.Mapping.LHS, IC, ICache,
                                    BCache, &CMap, false);

    Input.Mapping.RHS = getInstCopy(Input.Mapping.RHS, IC, ICache,
                                    BCache, &CMap, false);

    for (auto &M : Input.PCs) {
      M.LHS = getInstCopy(M.LHS, IC, ICache, BCache, &CMap, false);
      M.RHS = getInstCopy(M.RHS, IC, ICache, BCache, &CMap, false);
    }
    for (auto &BPC : Input.BPCs) {
      BPC.PC.LHS = getInstCopy(BPC.PC.LHS, IC, ICache, BCache, &CMap, false);
      BPC.PC.RHS = getInstCopy(BPC.PC.RHS, IC, ICache, BCache, &CMap, false);
    }

    std::vector<std::pair<Inst *, APInt>> Models;
    bool Valid;
    if (std::error_code EC = S->isValid(IC, Input.BPCs, Input.PCs, Input.Mapping, Valid, &Models)) {
      llvm::errs() << EC.message() << '\n';
    }

    if (Valid) {
      Results.push_back(Input);
      ReduceRec(IC, S, Input, Results, DNR);
    } else {
      if (DebugLevel >= 2) {
        llvm::outs() << "Invalid attempt.\n";
        Input.print(llvm::outs(), true);
      }
    }
  }
}

void ReduceAndGeneralize(InstContext &IC,
                               Solver *S, ParsedReplacement Input) {
  std::vector<std::pair<Inst *, APInt>> Models;
  bool Valid;
  if (std::error_code EC = S->isValid(IC, Input.BPCs, Input.PCs, Input.Mapping, Valid, &Models)) {
    llvm::errs() << EC.message() << '\n';
  }
  if (!Valid) {
    llvm::errs() << "Invalid Input.\n";
    return;
  }

  std::vector<ParsedReplacement> Results;
  std::unordered_set<std::string> DoNotRepeat;
  ReduceRec(IC, S, Input, Results, DoNotRepeat);

  if (!Results.empty()) {
    std::set<std::string> DedupedResults;
    for (auto &&Result : Results) {
      DedupedResults.insert(Result.getString(false));
    }

    std::vector<std::string> SortedResults(DedupedResults.begin(), DedupedResults.end());
    std::sort(SortedResults.begin(), SortedResults.end(), [](auto a, auto b){return a.length() < b.length();});

    for (auto &&S : SortedResults) {
      if (DebugLevel > 2) {
        llvm::outs() << "\n\nResult:\n";
      }
      llvm::outs() << S << '\n';
      if (!ReducePrintAll) {
        break;
      }
    }
  } else {
    if (DebugLevel > 2) {
      llvm::errs() << "Failed to Generalize.\n";
    }
  }
  if (DebugLevel > 2) {
    llvm::outs() << "Number of Results: " << Results.size() << ".\n";
  }
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);
  KVStore *KV = 0;

  std::unique_ptr<Solver> S = 0;
  S = GetSolver(KV);

  auto MB = MemoryBuffer::getFileOrSTDIN(InputFilename);
  if (!MB) {
    llvm::errs() << MB.getError().message() << '\n';
    return 1;
  }

  InstContext IC;
  std::string ErrStr;

  auto &&Data = (*MB)->getMemBufferRef();
  auto Inputs = ParseReplacements(IC, Data.getBufferIdentifier(),
                                  Data.getBuffer(), ErrStr);

  if (!ErrStr.empty()) {
    llvm::errs() << ErrStr << '\n';
    return 1;
  }

  // TODO: Write default action which chooses what to do based on input structure

  for (auto &&Input: Inputs) {
    if (FixIt) {
      // TODO: Verify that inputs are valid optimizations
      Generalize(IC, S.get(), Input);
    }
    if (Reduce) {
      ReduceAndGeneralize(IC, S.get(), Input);
    }
    if (SymbolizeConstant) {
      SymbolizeAndGeneralize(IC, S.get(), Input);
    }

    if (GeneralizeWidth) {
      GeneralizeBitWidth(IC, S.get(), Input);
    }
  }

  return 0;
}
