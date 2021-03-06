/*
 * Copyright 2010, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstdlib>
#include <list>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "clang/Driver/Arg.h"
#include "clang/Driver/ArgList.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Option.h"
#include "clang/Driver/OptTable.h"

#include "clang/Frontend/DiagnosticOptions.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/OwningPtr.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/system_error.h"

#include "slang.h"
#include "slang_assert.h"
#include "slang_rs.h"
#include "slang_rs_reflect_utils.h"

// Class under clang::driver used are enumerated here.
using clang::driver::arg_iterator;
using clang::driver::options::DriverOption;
using clang::driver::Arg;
using clang::driver::ArgList;
using clang::driver::InputArgList;
using clang::driver::Option;
using clang::driver::OptTable;

// SaveStringInSet, ExpandArgsFromBuf and ExpandArgv are all copied from
// $(CLANG_ROOT)/tools/driver/driver.cpp for processing argc/argv passed in
// main().
static inline const char *SaveStringInSet(std::set<std::string> &SavedStrings,
                                          llvm::StringRef S) {
  return SavedStrings.insert(S).first->c_str();
}
static void ExpandArgsFromBuf(const char *Arg,
                              llvm::SmallVectorImpl<const char*> &ArgVector,
                              std::set<std::string> &SavedStrings);
static void ExpandArgv(int argc, const char **argv,
                       llvm::SmallVectorImpl<const char*> &ArgVector,
                       std::set<std::string> &SavedStrings);

enum {
  OPT_INVALID = 0,  // This is not an option ID.
#define OPTION(NAME, ID, KIND, GROUP, ALIAS, FLAGS, PARAM, \
               HELPTEXT, METAVAR) OPT_##ID,
#include "RSCCOptions.inc"
  LastOption
#undef OPTION
};

static const OptTable::Info RSCCInfoTable[] = {
#define OPTION(NAME, ID, KIND, GROUP, ALIAS, FLAGS, PARAM, \
               HELPTEXT, METAVAR)   \
  { NAME, HELPTEXT, METAVAR, Option::KIND##Class, FLAGS, PARAM, \
    OPT_##GROUP, OPT_##ALIAS },
#include "RSCCOptions.inc"
};

class RSCCOptTable : public OptTable {
 public:
  RSCCOptTable()
      : OptTable(RSCCInfoTable,
                 sizeof(RSCCInfoTable) / sizeof(RSCCInfoTable[0])) {
  }
};

OptTable *createRSCCOptTable() {
  return new RSCCOptTable();
}

///////////////////////////////////////////////////////////////////////////////

class RSCCOptions {
 public:
  // The include search paths
  std::vector<std::string> mIncludePaths;

  // The output directory, if any.
  std::string mOutputDir;

  // The output type
  slang::Slang::OutputType mOutputType;

  unsigned mAllowRSPrefix : 1;

  // The name of the target triple to compile for.
  std::string mTriple;

  // The name of the target CPU to generate code for.
  std::string mCPU;

  // The list of target specific features to enable or disable -- this should
  // be a list of strings starting with by '+' or '-'.
  std::vector<std::string> mFeatures;

  std::string mJavaReflectionPathBase;

  std::string mJavaReflectionPackageName;

  slang::BitCodeStorageType mBitcodeStorage;

  unsigned mOutputDep : 1;

  std::string mOutputDepDir;

  std::vector<std::string> mAdditionalDepTargets;

  unsigned mShowHelp : 1;  // Show the -help text.
  unsigned mShowVersion : 1;  // Show the -version text.

  unsigned int mTargetAPI;

  RSCCOptions() {
    mOutputType = slang::Slang::OT_Bitcode;
    // Triple/CPU/Features must be hard-coded to our chosen portable ABI.
    mTriple = "armv7-none-linux-gnueabi";
    mCPU = "";
    slangAssert(mFeatures.empty());
    mFeatures.push_back("+long64");
    mBitcodeStorage = slang::BCST_APK_RESOURCE;
    mOutputDep = 0;
    mShowHelp = 0;
    mShowVersion = 0;
    mTargetAPI = RS_VERSION;
  }
};

// ParseArguments -
static void ParseArguments(llvm::SmallVectorImpl<const char*> &ArgVector,
                           llvm::SmallVectorImpl<const char*> &Inputs,
                           RSCCOptions &Opts,
                           clang::DiagnosticsEngine &DiagEngine) {
  if (ArgVector.size() > 1) {
    const char **ArgBegin = ArgVector.data() + 1;
    const char **ArgEnd = ArgVector.data() + ArgVector.size();
    unsigned MissingArgIndex, MissingArgCount;
    llvm::OwningPtr<OptTable> OptParser(createRSCCOptTable());
    llvm::OwningPtr<InputArgList> Args(
      OptParser->ParseArgs(ArgBegin, ArgEnd, MissingArgIndex, MissingArgCount));

    // Check for missing argument error.
    if (MissingArgCount)
      DiagEngine.Report(clang::diag::err_drv_missing_argument)
        << Args->getArgString(MissingArgIndex) << MissingArgCount;

    // Issue errors on unknown arguments.
    for (arg_iterator it = Args->filtered_begin(OPT_UNKNOWN),
        ie = Args->filtered_end(); it != ie; ++it)
      DiagEngine.Report(clang::diag::err_drv_unknown_argument)
        << (*it)->getAsString(*Args);

    for (ArgList::const_iterator it = Args->begin(), ie = Args->end();
        it != ie; ++it) {
      const Arg *A = *it;
      if (A->getOption().getKind() == Option::InputClass)
        Inputs.push_back(A->getValue(*Args));
    }

    Opts.mIncludePaths = Args->getAllArgValues(OPT_I);

    Opts.mOutputDir = Args->getLastArgValue(OPT_o);

    if (const Arg *A = Args->getLastArg(OPT_M_Group)) {
      switch (A->getOption().getID()) {
        case OPT_M: {
          Opts.mOutputDep = 1;
          Opts.mOutputType = slang::Slang::OT_Dependency;
          break;
        }
        case OPT_MD: {
          Opts.mOutputDep = 1;
          Opts.mOutputType = slang::Slang::OT_Bitcode;
          break;
        }
        default: {
          slangAssert(false && "Invalid option in M group!");
        }
      }
    }

    if (const Arg *A = Args->getLastArg(OPT_Output_Type_Group)) {
      switch (A->getOption().getID()) {
        case OPT_emit_asm: {
          Opts.mOutputType = slang::Slang::OT_Assembly;
          break;
        }
        case OPT_emit_llvm: {
          Opts.mOutputType = slang::Slang::OT_LLVMAssembly;
          break;
        }
        case OPT_emit_bc: {
          Opts.mOutputType = slang::Slang::OT_Bitcode;
          break;
        }
        case OPT_emit_nothing: {
          Opts.mOutputType = slang::Slang::OT_Nothing;
          break;
        }
        default: {
          slangAssert(false && "Invalid option in output type group!");
        }
      }
    }

    if (Opts.mOutputDep &&
        ((Opts.mOutputType != slang::Slang::OT_Bitcode) &&
         (Opts.mOutputType != slang::Slang::OT_Dependency)))
      DiagEngine.Report(clang::diag::err_drv_argument_not_allowed_with)
          << Args->getLastArg(OPT_M_Group)->getAsString(*Args)
          << Args->getLastArg(OPT_Output_Type_Group)->getAsString(*Args);

    Opts.mAllowRSPrefix = Args->hasArg(OPT_allow_rs_prefix);

    Opts.mJavaReflectionPathBase =
        Args->getLastArgValue(OPT_java_reflection_path_base);
    Opts.mJavaReflectionPackageName =
        Args->getLastArgValue(OPT_java_reflection_package_name);

    llvm::StringRef BitcodeStorageValue =
        Args->getLastArgValue(OPT_bitcode_storage);
    if (BitcodeStorageValue == "ar")
      Opts.mBitcodeStorage = slang::BCST_APK_RESOURCE;
    else if (BitcodeStorageValue == "jc")
      Opts.mBitcodeStorage = slang::BCST_JAVA_CODE;
    else if (!BitcodeStorageValue.empty())
      DiagEngine.Report(clang::diag::err_drv_invalid_value)
          << OptParser->getOptionName(OPT_bitcode_storage)
          << BitcodeStorageValue;

    Opts.mOutputDepDir =
        Args->getLastArgValue(OPT_output_dep_dir, Opts.mOutputDir);
    Opts.mAdditionalDepTargets =
        Args->getAllArgValues(OPT_additional_dep_target);

    Opts.mShowHelp = Args->hasArg(OPT_help);
    Opts.mShowVersion = Args->hasArg(OPT_version);

    Opts.mTargetAPI = Args->getLastArgIntValue(OPT_target_api,
                                               RS_VERSION,
                                               DiagEngine);
  }

  return;
}

static const char *DetermineOutputFile(const std::string &OutputDir,
                                       const char *InputFile,
                                       slang::Slang::OutputType OutputType,
                                       std::set<std::string> &SavedStrings) {
  if (OutputType == slang::Slang::OT_Nothing)
    return "/dev/null";

  std::string OutputFile(OutputDir);

  // Append '/' to Opts.mOutputDir if not presents
  if (!OutputFile.empty() &&
      (OutputFile[OutputFile.size() - 1]) != OS_PATH_SEPARATOR)
    OutputFile.append(1, OS_PATH_SEPARATOR);

  if (OutputType == slang::Slang::OT_Dependency) {
    // The build system wants the .d file name stem to be exactly the same as
    // the source .rs file, instead of the .bc file.
    OutputFile.append(slang::RSSlangReflectUtils::GetFileNameStem(InputFile));
  } else {
    OutputFile.append(
        slang::RSSlangReflectUtils::BCFileNameFromRSFileName(InputFile));
  }

  switch (OutputType) {
    case slang::Slang::OT_Dependency: {
      OutputFile.append(".d");
      break;
    }
    case slang::Slang::OT_Assembly: {
      OutputFile.append(".S");
      break;
    }
    case slang::Slang::OT_LLVMAssembly: {
      OutputFile.append(".ll");
      break;
    }
    case slang::Slang::OT_Object: {
      OutputFile.append(".o");
      break;
    }
    case slang::Slang::OT_Bitcode: {
      OutputFile.append(".bc");
      break;
    }
    case slang::Slang::OT_Nothing:
    default: {
      slangAssert(false && "Invalid output type!");
    }
  }

  return SaveStringInSet(SavedStrings, OutputFile);
}

#define str(s) #s
#define wrap_str(s) str(s)
static void llvm_rs_cc_VersionPrinter() {
  llvm::raw_ostream &OS = llvm::outs();
  OS << "llvm-rs-cc: Renderscript compiler\n"
     << "  (http://developer.android.com/guide/topics/renderscript)\n"
     << "  based on LLVM (http://llvm.org):\n";
  OS << "  Built " << __DATE__ << " (" << __TIME__ ").\n";
  OS << "  Target APIs: " << SLANG_MINIMUM_TARGET_API << " - "
     << SLANG_MAXIMUM_TARGET_API;
  OS << "\n  Build type: " << wrap_str(TARGET_BUILD_VARIANT);
#ifndef __DISABLE_ASSERTS
  OS << " with assertions";
#endif
  OS << ".\n";
  return;
}

int main(int argc, const char **argv) {
  std::set<std::string> SavedStrings;
  llvm::SmallVector<const char*, 256> ArgVector;
  RSCCOptions Opts;
  llvm::SmallVector<const char*, 16> Inputs;
  std::string Argv0;

  atexit(llvm::llvm_shutdown);

  ExpandArgv(argc, argv, ArgVector, SavedStrings);

  // Argv0
  Argv0 = llvm::sys::path::stem(ArgVector[0]);

  // Setup diagnostic engine
  clang::TextDiagnosticPrinter *DiagClient =
    new clang::TextDiagnosticPrinter(llvm::errs(), clang::DiagnosticOptions());
  DiagClient->setPrefix(Argv0);

  llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> DiagIDs(
    new clang::DiagnosticIDs());

  clang::DiagnosticsEngine DiagEngine(DiagIDs, DiagClient, true);

  clang::Diagnostic Diags(&DiagEngine);

  slang::Slang::GlobalInitialization();

  ParseArguments(ArgVector, Inputs, Opts, DiagEngine);

  // Exits when there's any error occurred during parsing the arguments
  if (DiagEngine.hasErrorOccurred())
    return 1;

  if (Opts.mShowHelp) {
    llvm::OwningPtr<OptTable> OptTbl(createRSCCOptTable());
    OptTbl->PrintHelp(llvm::outs(), Argv0.c_str(),
                      "Renderscript source compiler");
    return 0;
  }

  if (Opts.mShowVersion) {
    llvm_rs_cc_VersionPrinter();
    return 0;
  }

  // No input file
  if (Inputs.empty()) {
    DiagEngine.Report(clang::diag::err_drv_no_input_files);
    return 1;
  }

  // Prepare input data for RS compiler.
  std::list<std::pair<const char*, const char*> > IOFiles;
  std::list<std::pair<const char*, const char*> > DepFiles;

  llvm::OwningPtr<slang::SlangRS> Compiler(new slang::SlangRS());

  Compiler->init(Opts.mTriple, Opts.mCPU, Opts.mFeatures);

  for (int i = 0, e = Inputs.size(); i != e; i++) {
    const char *InputFile = Inputs[i];
    const char *OutputFile =
        DetermineOutputFile(Opts.mOutputDir, InputFile,
                            Opts.mOutputType, SavedStrings);

    if (Opts.mOutputDep) {
      const char *BCOutputFile, *DepOutputFile;

      if (Opts.mOutputType == slang::Slang::OT_Bitcode)
        BCOutputFile = OutputFile;
      else
        BCOutputFile = DetermineOutputFile(Opts.mOutputDepDir,
                                           InputFile,
                                           slang::Slang::OT_Bitcode,
                                           SavedStrings);

      if (Opts.mOutputType == slang::Slang::OT_Dependency)
        DepOutputFile = OutputFile;
      else
        DepOutputFile = DetermineOutputFile(Opts.mOutputDepDir,
                                            InputFile,
                                            slang::Slang::OT_Dependency,
                                            SavedStrings);

      DepFiles.push_back(std::make_pair(BCOutputFile, DepOutputFile));
    }

    IOFiles.push_back(std::make_pair(InputFile, OutputFile));
  }

  // Let's rock!
  int CompileFailed = !Compiler->compile(IOFiles,
                                         DepFiles,
                                         Opts.mIncludePaths,
                                         Opts.mAdditionalDepTargets,
                                         Opts.mOutputType,
                                         Opts.mBitcodeStorage,
                                         Opts.mAllowRSPrefix,
                                         Opts.mOutputDep,
                                         Opts.mTargetAPI,
                                         Opts.mJavaReflectionPathBase,
                                         Opts.mJavaReflectionPackageName);
  Compiler->reset();

  return CompileFailed;
}

///////////////////////////////////////////////////////////////////////////////

// ExpandArgsFromBuf -
static void ExpandArgsFromBuf(const char *Arg,
                              llvm::SmallVectorImpl<const char*> &ArgVector,
                              std::set<std::string> &SavedStrings) {
  const char *FName = Arg + 1;
  llvm::OwningPtr<llvm::MemoryBuffer> MemBuf;
  if (llvm::MemoryBuffer::getFile(FName, MemBuf)) {
    // Unable to open the file
    ArgVector.push_back(SaveStringInSet(SavedStrings, Arg));
    return;
  }

  const char *Buf = MemBuf->getBufferStart();
  char InQuote = ' ';
  std::string CurArg;

  for (const char *P = Buf; ; ++P) {
    if (*P == '\0' || (isspace(*P) && InQuote == ' ')) {
      if (!CurArg.empty()) {
        if (CurArg[0] != '@') {
          ArgVector.push_back(SaveStringInSet(SavedStrings, CurArg));
        } else {
          ExpandArgsFromBuf(CurArg.c_str(), ArgVector, SavedStrings);
        }

        CurArg = "";
      }
      if (*P == '\0')
        break;
      else
        continue;
    }

    if (isspace(*P)) {
      if (InQuote != ' ')
        CurArg.push_back(*P);
      continue;
    }

    if (*P == '"' || *P == '\'') {
      if (InQuote == *P)
        InQuote = ' ';
      else if (InQuote == ' ')
        InQuote = *P;
      else
        CurArg.push_back(*P);
      continue;
    }

    if (*P == '\\') {
      ++P;
      if (*P != '\0')
        CurArg.push_back(*P);
      continue;
    }
    CurArg.push_back(*P);
  }
}

// ExpandArgsFromBuf -
static void ExpandArgv(int argc, const char **argv,
                       llvm::SmallVectorImpl<const char*> &ArgVector,
                       std::set<std::string> &SavedStrings) {
  for (int i = 0; i < argc; ++i) {
    const char *Arg = argv[i];
    if (Arg[0] != '@') {
      ArgVector.push_back(SaveStringInSet(SavedStrings, std::string(Arg)));
      continue;
    }

    ExpandArgsFromBuf(Arg, ArgVector, SavedStrings);
  }
}
