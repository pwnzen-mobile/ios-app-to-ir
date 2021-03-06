#define DEBUG_TYPE "llvm-dec"
#include "llvm/ADT/StringExtras.h"
#include <unistd.h>
#include "llvm/ADT/Triple.h"
#include "llvm/DC/DCInstrSema.h"
#include "llvm/DC/DCRegisterSema.h"
#include "llvm/DC/DCTranslator.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAnalysis/MCCachingDisassembler.h"
#include "llvm/MC/MCAnalysis/MCFunction.h"
#include "llvm/MC/MCAnalysis/MCModule.h"
#include "llvm/MC/MCAnalysis/MCObjectSymbolizer.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectDisassembler.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCOptimization.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "FunctionNamePass.h"
#include "TailCallPass.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/Timer.h"

using namespace llvm;
using namespace object;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("Input object file"), cl::Required);

static cl::opt<std::string>
TripleName("triple", cl::desc("Target triple to disassemble for, "
                              "see -version for available targets"));

static cl::opt<uint64_t>
TranslationEntrypoint("entrypoint",
                      cl::desc("Address to start translating from "
                               "(default = object entrypoint)"));

static cl::opt<bool>
AnnotateIROutput("annot", cl::desc("Enable IR output anotations"),
                 cl::init(false));

static cl::opt<bool>
        NoPrint("no-print", cl::desc("Do not print the produced source"),
                         cl::init(false));

static cl::opt<bool>
     PrintBitcode("bc", cl::desc("Bitcode output"),
                      cl::init(false));

static cl::opt<unsigned>
TransOptLevel("O",
              cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                       "(default = '-O0')"),
              cl::Prefix,
              cl::init(0u));
static cl::opt<bool>
EnableDisassemblyCache("enable-mcod-disass-cache",
    cl::desc("Enable the MC Object disassembly instruction cache"),
    cl::init(false), cl::Hidden);

static cl::opt<bool>
OptimizeOption("MC_opt",cl::desc("try to optimize MC instruction"),cl::init(false));

static cl::opt<bool>
RecordAdd("REC_add",cl::desc("start to record the address of instruction"),cl::init(false));

static cl::opt<std::string>
        OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));



static StringRef ToolName;

static const Target *getTarget(const ObjectFile *Obj) {
  // Figure out the target triple.
  Triple TheTriple("unknown-unknown-unknown");
  if (TripleName.empty()) {
  if (Obj) {
    TheTriple.setArch(Triple::ArchType(Obj->getArch()));
    // TheTriple defaults to ELF, and COFF doesn't have an environment:
    // the best we can do here is indicate that it is mach-o.
    if (Obj->isMachO())
      TheTriple.setObjectFormat(Triple::MachO);
  }
  } else {
    TheTriple.setTriple(TripleName);
  }

  // Get the Target.
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget("", TheTriple, Error);
  if (!TheTarget) {
    errs() << ToolName << ": " << Error;
    return 0;
  }

  // Update the triple name and return the found target.
  TripleName = TheTriple.getTriple();
  return TheTarget;
}

uint32_t get_all_code_size(MCModule* target_module){
  uint32_t code_size=0;
  for(MCModule::func_iterator func = target_module->func_begin();func!=target_module->func_end();func++){
      MCFunction* tmp_func = &(**func);
      for(MCFunction::iterator bb = tmp_func->begin();bb!=tmp_func->end();bb++){
        MCBasicBlock* tmp_bb  = &(**bb);
        code_size += tmp_bb->size();
      }
  }
  return code_size;
}



int main(int argc, char **argv) {
    //git
  sys::PrintStackTraceOnErrorSignal();
  PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj Y;

  InitializeAllTargetInfos();
  InitializeAllTargetDCs();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  InitializeAllDisassemblers();

  cl::ParseCommandLineOptions(argc, argv, "Function disassembler\n");

  ToolName = argv[0];
  
  TimerGroup TG("... llvm-dec module time report ...");

  Timer *BinLoadTimer = new Timer("Bin load overhead", TG);
  BinLoadTimer->startTimer();
  auto Binary = createBinary(InputFilename);
  if (std::error_code ec = Binary.getError()) {
    errs() << ToolName << ": '" << InputFilename << "': "
           << ec.message() << ".\n";
    return 1;
  }
  BinLoadTimer->stopTimer();

  Timer *MachOParseTimer = new Timer("Mach-O parse overhead", TG);
  MachOParseTimer->startTimer();
  ObjectFile *Obj;
  if (!(Obj = dyn_cast<ObjectFile>((*Binary).getBinary())))
    errs() << ToolName << ": '" << InputFilename << "': "
           << "Unrecognized file type.\n";
  MachOParseTimer->stopTimer();
    
  const Target *TheTarget = getTarget(Obj);

  std::unique_ptr<const MCRegisterInfo> MRI(
    TheTarget->createMCRegInfo(TripleName));
  if (!MRI) {
    errs() << "error: no register info for target " << TripleName << "\n";
    return 1;
  }

  // Set up disassembler.
  std::unique_ptr<const MCAsmInfo> MAI(
    TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!MAI) {
    errs() << "error: no assembly info for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<const MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TripleName, "", ""));
  if (!STI) {
    errs() << "error: no subtarget info for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<const MCInstrInfo> MII(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "error: no instruction info for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<const MCObjectFileInfo> MOFI(new MCObjectFileInfo);
  MCContext Ctx(MAI.get(), MRI.get(), MOFI.get());

  std::unique_ptr<MCDisassembler> DisAsm(
      TheTarget->createMCDisassembler(*STI, Ctx));
  if (!DisAsm) {
    errs() << "error: no disassembler for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<MCDisassembler> DisAsmImpl;
  if (EnableDisassemblyCache) {
    DisAsmImpl = std::move(DisAsm);
    DisAsm.reset(new MCCachingDisassembler(*DisAsmImpl, *STI));
  }

  std::unique_ptr<MCInstPrinter> MIP(
      TheTarget->createMCInstPrinter(Triple(TripleName), 0, *MAI, *MII, *MRI));
  if (!MIP) {
    errs() << "error: no instprinter for target " << TripleName << "\n";
    return 1;
  }

  std::unique_ptr<MCRelocationInfo> RelInfo(
      TheTarget->createMCRelocationInfo(TripleName, Ctx));
  if (!RelInfo) {
    errs() << "error: no relocation info for target " << TripleName << "\n";
    return 1;
  }
  std::unique_ptr<MCObjectSymbolizer> MOS(
      TheTarget->createMCObjectSymbolizer(Ctx, *Obj, std::move(RelInfo)));
  if (!MOS) {
    errs() << "error: no object symbolizer for target " << TripleName << "\n";
    return 1;
  }
  // FIXME: should we set the symbolizer on OD? maybe under a CLI option.

  std::unique_ptr<const MCInstrAnalysis> MIA(
      TheTarget->createMCInstrAnalysis(MII.get()));

  Timer *MCTimer = new Timer("MC overhead", TG);
  MCTimer->startTimer();
  std::unique_ptr<MCObjectDisassembler> OD(
      new MCObjectDisassembler(*Obj, *DisAsm, *MIA));
  std::unique_ptr<MCModule> MCM(OD->buildModule());
    
  errs() << "Linear code size: " << utostr(OD->TextSegList.size()) << "\n";
  errs() << "Recursive disassembled code size: " << utostr(OD->InstParsedList.size()) << "\n";
  errs() << "None general operand code size: " << utostr(OD->NoneGeneralOperandList.size()) << "\n";

// to find the operands len distribution
//    for (int i = 0; i < sizeof(OD->DisInstSize) / sizeof(unsigned int); i++)
//        errs() << utostr(i) << " :" << utostr(OD->DisInstSize[i]) << "\n";

//ugly coding, but I only find PackedVector support such kind of operation...
//    for (size_t i = 0; i < OD->TextSegList.size(); ++i) {
//      const uint64_t Addr = OD->TextSegList[i];
//      if (OD->InstParsedList.count(Addr) == 0)
//        errs() << "Cross check IDA for addr: 0x"<< utohexstr(Addr) << "\n";
//    }
        
  MCTimer->stopTimer();
//  delete MCTimer;
    
  /*
    add by -death
   */
  if(OptimizeOption){
    uint32_t code_size;
    code_size = get_all_code_size(&(*MCM));
    if(MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(Obj)){
      std::unique_ptr<MCOptimization> MCOpt(new MCOptimization(&(*MCM),MachO));
      MCOpt->try_to_optimize();
      errs() << "None Semantic ARC code erased: " << utostr(MCOpt->getNoneSemanticARCCount()) << "\n";
      errs() << "Semantic ARC code replaced: " << utostr(MCOpt->getSemanticARCCount()) << "\n";
    }

//    errs()<<"all code size : "<<code_size<<"\n";
  }
//    return 0;
  /*
    add by -death end 
   */
  if (!MCM)
    return 1;

  TransOpt::Level TOLvl;
  switch (TransOptLevel) {
  default:
    errs() << ToolName << ": invalid optimization level.\n";
    return 1;
  case 0: TOLvl = TransOpt::None; break;
  case 1: TOLvl = TransOpt::Less; break;
  case 2: TOLvl = TransOpt::Default; break;
  case 3: TOLvl = TransOpt::Aggressive; break;
  }

  // FIXME: should we have a non-default datalayout?
  DataLayout DL("");
  

  std::unique_ptr<DCRegisterSema> DRS(
      TheTarget->createDCRegisterSema(TripleName, *MRI, *MII, DL));
  if (!DRS) {
    errs() << "error: no dc register sema for target " << TripleName << "\n";
    return 1;
  }
  std::unique_ptr<DCInstrSema> DIS(
      TheTarget->createDCInstrSema(TripleName, *DRS, *MRI, *MII));
  if (!DIS) {
    errs() << "error: no dc instruction sema for target " << TripleName << "\n";
    return 1;
  }
   /*
  add by -death 
   */
  getGlobalContext().setRecordOrNot(RecordAdd);
  /*
  add by -death end 
   */

  std::unique_ptr<DCTranslator> DT(
    new DCTranslator(
                     getGlobalContext(),    /* LLVMContext */
                     DL,        /* DataLayout */
                     TOLvl,     /* TransOpt::Level */
                     *DIS,      /* DCInstrSema */
                     *DRS,      /* DCRegisterSema */
                     *MIP,      /* MCInstPrinter */
                     *STI,      /* MCSubtargetInfo */
                     *MCM,      /* MCModule */
                     OD.get(),  /* MCObjectDisassembler */
                     AnnotateIROutput   /* EnableIRAnnotation */
                     ));

  if (!TranslationEntrypoint)
    TranslationEntrypoint = MOS->getEntrypoint();   /* MCObjectSymbolizer */

    Timer *DCTimer = new Timer("DC overhead", TG);
    DCTimer->startTimer();
//  DT->createMainFunctionWrapper(
//      DT->translateRecursivelyAt(TranslationEntrypoint));
    DT->translateAllKnownFunctions();
    Function *main_fn = DT->getCurrentTranslationModule()->getFunction("fn_" + utohexstr(TranslationEntrypoint));
    DCTimer->stopTimer();
 
    Timer *FuncTimer = new Timer("FunctionNamePass overhead", TG);
    FuncTimer->startTimer();
//    assert(main_fn);
    if (main_fn)
        DT->createMainFunctionWrapper(main_fn);

    if (MachOObjectFile *MachO = dyn_cast<MachOObjectFile>(Obj)) {
        legacy::PassManager *pm = new legacy::PassManager();
        MCObjectDisassembler::AddressSetTy functionStarts = OD->findFunctionStarts();
//        pm->add(new TailCallPass(functionStarts));
        pm->add(new FunctionNamePass(MachO, DisAsm));
        pm->run(*DT->getCurrentTranslationModule());
    }
    FuncTimer->stopTimer();
    
    if (!NoPrint) {
        std::error_code EC;
        sys::fs::OpenFlags OpenFlags = sys::fs::F_None;
        if (!Binary)
            OpenFlags |= sys::fs::F_Text;
        std::unique_ptr<tool_output_file> FDOut = llvm::make_unique<tool_output_file>(OutputFilename, EC,
                                                         OpenFlags);
        if (EC) {
            errs() << EC.message() << '\n';
            return -1;
        }
        

        if (PrintBitcode) {
            Timer *SaveBinTimer = new Timer("Bin save overhead", TG);
            SaveBinTimer->startTimer();
            WriteBitcodeToFile(DT->getCurrentTranslationModule(), FDOut->os(), true);
            SaveBinTimer->stopTimer();
        } else {
            FDOut->os() << *DT->getCurrentTranslationModule();
        }

        FDOut->keep();
        //DT->printCurrentModule(FDOut->os());


    }
  return 0;
}
