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

#include <fstream>

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

static llvm::cl::opt<bool> IgnorePCs("ignore-pcs",
    llvm::cl::desc("Ignore inputs which have souper path conditions."
                   "(default=false)"),
    llvm::cl::init(true));

static llvm::cl::opt<bool> IgnoreDF("ignore-df",
    llvm::cl::desc("Ignore inputs with dataflow constraints."
                   "(default=false)"),
    llvm::cl::init(true));

static llvm::cl::opt<std::string> ListFile("listfile",
    llvm::cl::desc("List of optimization indexes to include.\n"
                   "(default=empty-string)"),
    llvm::cl::init(""));


static const std::map<Inst::Kind, std::string> MatchOps = {
  {Inst::Add, "m_c_Add("}, {Inst::Sub, "m_Sub("},
  {Inst::Mul, "m_c_Mul("},

  {Inst::Shl, "m_Shl("}, {Inst::LShr, "m_LShr("},
  {Inst::AShr, "m_AShr("},

  {Inst::AddNSW, "m_NSWAdd("}, {Inst::SubNSW, "m_NSWSub("}, // add _c_ too?
  {Inst::MulNSW, "m_NSWMul("}, {Inst::ShlNSW, "m_NSWShl("},
  {Inst::AddNUW, "m_NUWAdd("}, {Inst::SubNUW, "m_NUWSub("},
  {Inst::MulNUW, "m_NUWMul("}, {Inst::ShlNUW, "m_NUWShl("},
  {Inst::AddNW, "m_NWAdd("}, {Inst::SubNW, "m_NWSub("},
  {Inst::MulNW, "m_NWMul("}, {Inst::ShlNW, "m_NWShl("},

  {Inst::SDiv, "m_SDiv("}, {Inst::UDiv, "m_UDiv("},
  {Inst::SRem, "m_SRem("}, {Inst::URem, "m_URem("},


  {Inst::And, "m_c_And("}, {Inst::Or, "m_c_Or("},
  {Inst::Xor, "m_c_Xor("},

  {Inst::Eq, "m_c_ICmp("},
  {Inst::Ne, "m_c_ICmp("},
  {Inst::Ule, "m_ICmp("},
  {Inst::Ult, "m_ICmp("},
  {Inst::Sle, "m_ICmp("},
  {Inst::Slt, "m_ICmp("},

  {Inst::SExt, "m_SExt("},
  {Inst::ZExt, "m_ZExt("},
  {Inst::Trunc, "m_Trunc("},
  {Inst::Select, "m_Select("},
  {Inst::Phi, "m_Phi("},
};

static const std::map<Inst::Kind, std::string> CreateOps = {
  {Inst::Shl, "CreateShl("}, {Inst::AShr, "CreateAShr("}, {Inst::LShr, "CreateLShr("},
  {Inst::Add, "CreateAdd("}, {Inst::Mul, "CreateMul("}, {Inst::Sub, "CreateSub("},
  {Inst::SDiv, "CreateSDiv("}, {Inst::UDiv, "CreateUDiv("}, {Inst::SRem, "CreateSRem("},
  {Inst::URem, "CreateURem("},
  {Inst::Or, "CreateOr("}, {Inst::And, "CreateAnd("}, {Inst::Xor, "CreateXor("},

  // FakeOps
  {Inst::LogB, "CreateLogB("},

  {Inst::Eq, "CreateCmp(ICmpInst::ICMP_EQ, "},
  {Inst::Ne, "CreateCmp(ICmpInst::ICMP_NE, "},
  {Inst::Ule, "CreateCmp(ICmpInst::ICMP_ULE, "},
  {Inst::Ult, "CreateCmp(ICmpInst::ICMP_ULT, "},
  {Inst::Sle, "CreateCmp(ICmpInst::ICMP_SLE, "},
  {Inst::Slt, "CreateCmp(ICmpInst::ICMP_SLT, "},

  {Inst::Trunc, "CreateTrunc("},
  {Inst::SExt, "CreateSExt("},
  {Inst::ZExt, "CreateZExt("},

  {Inst::Select, "CreateSelect("},

  {Inst::FShl, "CreateFShl("},
  {Inst::FShr, "CreateFShr("},
  {Inst::BSwap, "CreateBSwap("},

  {Inst::Const, "dummy"},
};

static const std::map<Inst::Kind, std::string> PredNames = {
  {Inst::Eq, "ICmpInst::ICMP_EQ"},
  {Inst::Ne, "ICmpInst::ICMP_NE"},
  {Inst::Ule, "ICmpInst::ICMP_ULE"},
  {Inst::Ult, "ICmpInst::ICMP_ULT"},
  {Inst::Sle, "ICmpInst::ICMP_SLE"},
  {Inst::Slt, "ICmpInst::ICMP_SLT"},
};

struct Constraint {
  virtual std::string print() = 0;
};

struct VarEq : public Constraint {
  VarEq(std::string LHS_, std::string RHS_) : LHS(LHS_), RHS(RHS_) {}
  std::string LHS;
  std::string RHS;
  std::string print() override {
    return LHS + " == " + RHS;
  }
};

struct PredEq : public Constraint {
  PredEq(std::string P_, std::string K_) : P(P_), K(K_) {}
  std::string P;
  std::string K;
  std::string print() override {
    return P + " == " + K;
  }
};

struct WidthEq : public Constraint {
  WidthEq(std::string Name_, size_t W_) : Name(Name_) , W(W_){}
  std::string Name;
  size_t W;
  std::string print() override {
    return "util::check_width(" + Name + ',' + std::to_string(W) + ")";
  }
};

struct DomCheck : public Constraint {
  DomCheck(std::string Name_) : Name(Name_) {}
  std::string Name;
  
  std::string print() override {
    return "util::dc(DT, I, " + Name + ")";
  }
};

struct VC : public Constraint {
  VC(std::string Cons_, std::string Name_) : Cons(Cons_), Name(Name_)  {}
  std::string print() override {
    return "util::" + Cons + "(" + Name + ")";
  }
  std::string Name;
  std::string Cons;
};

//struct VarKB : public Constraint {

//};

struct SymbolTable : public std::map<Inst *, std::vector<std::string>> {
  std::vector<Constraint *> Constraints;

  std::map<Inst *, std::string> Preds;
  std::vector<Inst *> Vars;
  std::set<Inst *> Consts, ConstRefs;

  std::set<Inst *> Used;

  void RegisterPred(Inst *I) {
    if (PredNames.find(I->K) == PredNames.end()) {
      return; // not a predicate
    }
    if (Preds.find(I) != Preds.end()) {
      return; // already registered
    }
    auto Name = "P" + std::to_string(Preds.size());
    Preds[I] = Name;
    Constraints.push_back(new PredEq(Name, PredNames.at(I->K)));
  }

  template<typename Stream>
  void PrintPreds(Stream &Out) {
    if (Preds.empty()) {
      return;
    }
    Out << "ICmpInst::Predicate ";
    bool first = true;
    for (auto &&P : Preds) {
      if (first) {
        first = false;
      } else {
        Out << ", ";
      }
      Out << P.second;
    }
    Out << ";\n";
  }
  void GenVarEqConstraints() {
    for (auto &&S : *this) {
      if (S.second.size() > 1) {
        for (size_t i = 1; i < S.second.size(); ++i) {
          Constraints.push_back(new VarEq(S.second[0], S.second[i]));
        }
      }
    }
  }
  
  void GenDomConstraints(Inst *RHS) {
    static std::set<Inst *> Visited;
    Visited.insert(RHS);
    for (auto Op : RHS->Ops) {
      if (Op->K == Inst::Const) {
        continue;
        // TODO: Find other cases
      }
      auto It = find(Op);
      if (It != end()) {
        if (Visited.find(Op) == Visited.end()) {
          Constraints.push_back(new DomCheck(It->second[0]));
          GenDomConstraints(Op);
        }
      }
    }
  }

  void GenVarPropConstraints(Inst *LHS) {
    std::vector<Inst *> Vars;
    findVars(LHS, Vars);

    for (auto V : Vars) {
      auto Name = this->at(V)[0];
      Constraints.push_back(new WidthEq(Name, V->Width));
      if (V->PowOfTwo) {
        Constraints.push_back(new VC("pow2", Name));
      }
      if (V->NonZero) {
        Constraints.push_back(new VC("nz", Name));
      }
      if (V->NonNegative) {
        Constraints.push_back(new VC("nn", Name));
      }
      if (V->Negative) {
        Constraints.push_back(new VC("neg", Name));
      }

    }
  }

  template <typename Stream>
  void PrintConstraintsPre(Stream &Out) {
    if (Constraints.empty()) {
      return;
    }
    Out << "if (";
    bool first = true;
    for (auto &&C : Constraints) {
      if (first) {
        first = false;
      } else {
        Out << " && ";
      }
      Out << C->print();
    }
    Out << ") {\n";
  }
  template <typename Stream>
  void PrintConstraintsPost(Stream &Out) {
    if (Constraints.empty()) {
      return;
    }
    Out << "}\n";
  }

  // Consts = consts found in LHS
  // ConstRefs = consts found in RHS
  template <typename Stream>
  void PrintConstDecls(Stream &Out) {
    size_t varnum = 0;

    auto Print = [&](SymbolTable &Syms, Inst *C){
      auto Name = "C" + std::to_string(varnum++);
      Out << "  auto " << Name << " = C("
          << C->Val.getBitWidth() <<", "
          << C->Val << ", B);\n";
      Syms[C].push_back(Name);
    };

    for (auto C : ConstRefs) {
      if (Consts.find(C) == Consts.end()) {
        Print(*this, C);
      }
    }
  }
};

template <typename Stream>
bool GenLHSMatcher(Inst *I, Stream &Out, SymbolTable &Syms) {
  if (I->K != souper::Inst::Var && Syms.Used.find(I) != Syms.Used.end()) {
    Out << "&" << Syms[I].back() << " <<= ";
  }

  auto It = MatchOps.find(I->K);
  if (It == MatchOps.end()) {
    llvm::errs() << "\nUnimplemented matcher:" << Inst::getKindName(I->K) << "\n";
    return false;
  }
  
  auto Op = It->second;

  Out << Op;

  if (I->K == Inst::SExt || I->K == Inst::ZExt || I->K == Inst::Trunc) {
    Out << I->Width << ", ";
  }
  
  if (PredNames.find(I->K) != PredNames.end()) {
    Out << Syms.Preds[I] << ", ";
  }

  bool first = true;
  for (auto Child : I->Ops) {
    if (first) {
      first = false;
    } else {
      Out << ", ";
    }

    if (Child->K == Inst::Const) {
      if (Child->K != souper::Inst::Var && Syms.Used.find(Child) != Syms.Used.end()) {
        Out << "&" << Syms[Child].back() << " <<= ";
      }
      auto Str = Child->Val.toString(10, false);
      Out << "m_SpecificInt( " << Child->Width << ", " << Str << ")";
    } else if (Child->K == Inst::Var) {
      if (Child->Name.starts_with("symconst")) {
        Out << "m_Constant(&" << Syms[Child].back() << ")";
      } else if (Child->Name.starts_with("constexpr")) {
        llvm::errs() << "FOUND A CONSTEXPR\n";
      } else {
      // FIXME What about Symbolic constants?
      // How about matching const exprs?

        Out << "m_Value(" << Syms[Child].back() << ")";
      }
      Syms[Child].pop_back();
    } else {
      if (!GenLHSMatcher(Child, Out, Syms)) {
        return false;
      }
    }

  }
  Out << ")";
  return true;
}

template <typename Stream>
bool GenRHSCreator(Inst *I, Stream &Out, SymbolTable &Syms) {
  auto It = CreateOps.find(I->K);
  if (It == CreateOps.end()) {
    llvm::errs() << "\nUnimplemented creator:" << Inst::getKindName(I->K) << "\n";
    return false;
  }
  auto Op = It->second;

  Out << "B->" << Op;
  bool first = true;
  for (auto Child : I->Ops) {
    if (first) {
      first = false;
    } else {
      Out << ", ";
    }
    if (Syms.find(Child) != Syms.end()) {
      Out << Syms[Child][0];
    } else {
      if (!GenRHSCreator(Child, Out, Syms)) {
        return false;
      }
    }

  }
  if (I->K == Inst::Trunc || I->K == Inst::SExt || I->K == Inst::ZExt) {
    Out << ", T(" << I->Width << ", B)";
  }
  Out << ")";

  return true;
}

template <typename Stream>
bool InitSymbolTable(Inst *Root, Inst *RHS, Stream &Out, SymbolTable &Syms) {
  std::set<Inst *> LHSInsts;

//  Paths[Root] = {}; // root has an empty path
  std::vector<Inst *> Stack{Root};
//
  int varnum = 0;
  while (!Stack.empty()) {
    auto I = Stack.back();
    Stack.pop_back();
    Syms.RegisterPred(I);
    LHSInsts.insert(I);
    if (I->K == Inst::Var) {
      Syms[I].push_back("x" + std::to_string(varnum++));
    }
    if (I->K == Inst::Const) {
      Syms.Consts.insert(I);
    }
    for (int i = 0; i < I->Ops.size(); ++i) {
//      Paths[I->Ops[i]] = Paths[I]; // Child inherits parent's path
//      Paths[I->Ops[i]].push_back(i);
      Stack.push_back(I->Ops[i]); // Souper exprs are DAGs
    }
  }

  std::set<Inst *> LHSRefs;
  std::set<Inst *> Visited;
  Stack.push_back(RHS);
  while (!Stack.empty()) {
    auto I = Stack.back();
    Stack.pop_back();
    Visited.insert(I);
    if (I->K == Inst::Const) {
      Syms.ConstRefs.insert(I);
    }

    if (LHSInsts.find(I) != LHSInsts.end()) {
      if (Syms.Used.insert(I).second && Syms.find(I) == Syms.end()) {
        Syms[I].push_back("x" + std::to_string(varnum++));
      }
    }
    for (auto Child : I->Ops) {
      if (Visited.find(Child) == Visited.end()) {
        Stack.push_back(Child);
      }
    }

  }

  if (!Syms.empty()) {
    Out << "llvm::Value ";
    bool first = true;
    for (auto &&S : Syms) {
      for (auto &&Name : S.second) {
        if (first) {
          first = false;
        } else {
          Out << ", ";
        }
        Out << "*" << Name;
      }
    }
    Out << ";\n";
  }

//  varnum = 0;
//  for (auto &&P : Paths) {
//    if (P.first == Root || P.first->K == Inst::Var
//        || LHSRefs.find(P.first) == LHSRefs.end()) {
//      continue;
//    }
////    std::string Name = "I";
////    for (auto idx : P.second) {
////      auto NewName = "y" + std::to_string(varnum++);
////      Out << "auto " << NewName << " = cast<Instruction>(" << Name;
////      Out << ")->getOperand(" << idx << ");\n";
////      std::swap(Name, NewName);
////    }
////    Syms[P.first].push_back(Name);
//
//    auto Name = "y" + std::to_string(varnum++);
//    Out << "auto " << Name << " = util::node(I, ";
//    printPath(Out, P.second);
//    Out << ");\n";
//    Syms[P.first].push_back(Name);
//  }
  Syms[Root].push_back("I");
  Syms.PrintPreds(Out);
  return true;
}

template <typename Stream>
bool GenMatcher(ParsedReplacement Input, Stream &Out, size_t OptID) {
  SymbolTable Syms;
  Out << "{\n";

  if (!InitSymbolTable(Input.Mapping.LHS, Input.Mapping.RHS, Out, Syms)) {
    return false;
  }

  Out << "if (match(I, ";

  SymbolTable SymsCopy = Syms;
  if (!GenLHSMatcher(Input.Mapping.LHS, Out, SymsCopy)) {
    return false;
  }
  Out << ")) {\n";

  Syms.GenVarEqConstraints();
  Syms.GenVarPropConstraints(Input.Mapping.LHS);
  Syms.GenDomConstraints(Input.Mapping.RHS);
  Syms.PrintConstraintsPre(Out);

  Out << "  St.hit(" << OptID << ");\n";

  Syms.PrintConstDecls(Out);

  if (Syms.find(Input.Mapping.RHS) != Syms.end()) {
    Out << "  return " << Syms[Input.Mapping.RHS][0] << ";";
  } else if (Input.Mapping.RHS->K == Inst::Const) {
    Out << "  APInt Result("
        << Input.Mapping.RHS->Width <<", "
        << Input.Mapping.RHS->Val << ");\n";
    Out << "  return ConstantInt::get(TheContext, Result);";
  } else {
    Out << "  return ";
    if (!GenRHSCreator(Input.Mapping.RHS, Out, Syms)) {
      return false;
    }
    Out << ";";
  }
  Out << "\n}\n}";

  Syms.PrintConstraintsPost(Out);

  return true;
}

std::string getLLVMInstKindName(Inst::Kind K) {
  StringRef str = MatchOps.find(K)->second;
  str.consume_front("m_");
  str.consume_back("(");
//  str.consume_front("NSW");
//  str.consume_front("NUW");
//  str.consume_front("NW");
  return str.str();
}

int profitability(const ParsedReplacement &Input) {
  return souper::cost(Input.Mapping.LHS) -
    souper::cost(Input.Mapping.RHS);
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);
  KVStore *KV = 0;

  std::unique_ptr<Solver> S = 0;
  S = GetSolver(KV);

  std::unordered_set<size_t> optnumbers;
  if (ListFile != "") {
    std::ifstream in(ListFile);
    size_t num;
    while (in >> num) {
      optnumbers.insert(num);
    }
  }

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

  std::set<Inst::Kind> Kinds;
  std::sort(Inputs.begin(), Inputs.end(),
            [&Kinds](const ParsedReplacement& A, const ParsedReplacement &B) {
    Kinds.insert(A.Mapping.LHS->K);
    Kinds.insert(B.Mapping.LHS->K);

//    if (A.Mapping.LHS->K < B.Mapping.LHS->K) {
//      return true;
//    } else if (A.Mapping.LHS->K == B.Mapping.LHS->K) {
//      return profitability(A) > profitability(B);
//    } else {
//      return false;
//    }
    return A.Mapping.LHS->K < B.Mapping.LHS->K;
  });

  if (!ErrStr.empty()) {
    llvm::errs() << ErrStr << '\n';
    return 1;
  }

  size_t optnumber = 0;
//  genDispatchCode(Kinds);
  Inst::Kind Last = Inst::Kind::None;

  bool first = true;

  for (auto &&Input: Inputs) {
    if (IgnorePCs && !Input.PCs.empty()) {
      continue;
    }
    if (Input.Mapping.LHS == Input.Mapping.RHS) {
      continue;
    }
    if (IgnoreDF) {
      if (Input.Mapping.LHS->DemandedBits.getBitWidth()
          == Input.Mapping.LHS->Width && !Input.Mapping.LHS->DemandedBits.isAllOnesValue()) {
        continue;
      }
      std::vector<Inst *> Vars;
      findVars(Input.Mapping.LHS, Vars);
      findVars(Input.Mapping.RHS, Vars);
      bool found = false;
      for (auto V : Vars) {
        if (V->KnownOnes.getBitWidth() == V->Width && V->KnownOnes != 0) {
          found = true;
          break;
        }

        if (V->KnownZeros.getBitWidth() == V->Width && V->KnownZeros != 0) {
          found = true;
          break;
        }
//        if (!V->Range.isFullSet() || !V->Range.isEmptySet()) {
//          continue;
//        }
      }
      if (found) continue;
    }

    if (Input.Mapping.LHS->K != Last) {
      if (!first) {
        llvm::outs() << "}\n";
      }
      first = false;
      llvm::outs() << "if (";
      switch (Input.Mapping.LHS->K) {
        case Inst::AddNW:
        case Inst::AddNUW:
        case Inst::AddNSW:
        case Inst::Add: llvm::outs()
          << "I->getOpcode() == Instruction::Add"; break;

        case Inst::SubNW:
        case Inst::SubNUW:
        case Inst::SubNSW:
        case Inst::Sub: llvm::outs()
          << "I->getOpcode() == Instruction::Sub"; break;

        case Inst::MulNW:
        case Inst::MulNUW:
        case Inst::MulNSW:
        case Inst::Mul: llvm::outs()
          << "I->getOpcode() == Instruction::Mul"; break;

        case Inst::ShlNW:
        case Inst::ShlNUW:
        case Inst::ShlNSW:
        case Inst::Shl: llvm::outs()
          << "I->getOpcode() == Instruction::Shl"; break;

        case Inst::And: llvm::outs()
          << "I->getOpcode() == Instruction::And"; break;
        case Inst::Or: llvm::outs()
          << "I->getOpcode() == Instruction::Or"; break;
        case Inst::Xor: llvm::outs()
          << "I->getOpcode() == Instruction::Xor"; break;
        case Inst::SRem: llvm::outs()
          << "I->getOpcode() == Instruction::SRem"; break;
        case Inst::URem: llvm::outs()
          << "I->getOpcode() == Instruction::URem"; break;
        case Inst::SDiv: llvm::outs()
          << "I->getOpcode() == Instruction::SDiv"; break;
        case Inst::UDiv: llvm::outs()
          << "I->getOpcode() == Instruction::UDiv"; break;
        case Inst::ZExt: llvm::outs()
          << "I->getOpcode() == Instruction::ZExt"; break;
        case Inst::SExt: llvm::outs()
          << "I->getOpcode() == Instruction::SExt"; break;
        case Inst::Trunc: llvm::outs()
          << "I->getOpcode() == Instruction::Trunc"; break;
        case Inst::Select: llvm::outs()
          << "I->getOpcode() == Instruction::Select"; break;
        case Inst::Phi: llvm::outs()
          << "isa<PHINode>(I)"; break;
        case Inst::Eq:
        case Inst::Ne:
        case Inst::Ult:
        case Inst::Slt:
        case Inst::Ule:
        case Inst::Sle: llvm::outs()
          << "I->getOpcode() == Instruction::ICmp"; break;

        default: llvm::outs() << "true";
      }
      llvm::outs() << ") {\n";
    }
    Last = Input.Mapping.LHS->K;

    std::string Str;
    llvm::raw_string_ostream Out(Str);
    if (GenMatcher(Input, Out, optnumber)) {
      auto current = optnumber++;
      if (!optnumbers.empty()
          && optnumbers.find(current) == optnumbers.end()) {
        Out.flush();
        Str.clear();
        llvm::errs() << "Opt " << current <<  " skipped on demand.\n";
        continue;
      }
      llvm::outs() << "/* Opt : " << current << "\n";
      Input.print(llvm::outs(), true);
      llvm::outs() << "*/\n";
      llvm::outs() << Str << "\n";
      llvm::outs().flush();
    } else {
      Input.print(llvm::errs(), true);
      llvm::errs() << "Failed to generate matcher.\n\n\n";

      llvm::errs().flush();
    }
  }
  llvm::outs() << "}\n";
//  llvm::outs() << "end:\n";

  return 0;
}
