#include "llvm_codegen_impl.h"

#include <llvm/Support/raw_ostream.h>
#include <llvm/Config/llvm-config.h>
#include <stdexcept>

#include <llvm/TargetParser/Host.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/TargetParser/Host.h>

#include <llvm/ADT/SmallVector.h>

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <utility>

namespace NQumir::NCodeGen {

namespace {

constexpr auto AssemblyFile = llvm::CodeGenFileType::AssemblyFile;
constexpr auto ObjectFile = llvm::CodeGenFileType::ObjectFile;

std::pair<std::string, std::string> GetNativeCpuAndFeatures() {
    auto cpu = llvm::sys::getHostCPUName().str();
    std::string features;
    auto hostFeatures = llvm::sys::getHostCPUFeatures();
    for (const auto& feature : hostFeatures) {
        if (!features.empty()) {
            features += ",";
        }
        features += feature.getValue() ? "+" : "-";
        features += feature.getKey().str();
    }
    return {std::move(cpu), std::move(features)};
}

std::string FindRuntimeLibrary() {
    llvm::SmallVector<llvm::SmallString<256>, 3> candidates;

    if (const char* fromEnv = std::getenv("QUMIR_RUNTIME")) {
        candidates.emplace_back(fromEnv);
    }

    // /proc/self/exe resolves the /usr/bin symlink, so this is the versioned tree.
    std::string selfExePath = llvm::sys::fs::getMainExecutable(nullptr, nullptr);
    llvm::StringRef exeDir = llvm::sys::path::parent_path(selfExePath);

    // Installed layout: <prefix>/lib/qumir-<release>/{bin,lib}
    candidates.emplace_back(exeDir);
    llvm::sys::path::append(candidates.back(), "..", "lib", "libqumir_runtime.a");

    // Build tree layout
    candidates.emplace_back(exeDir);
    llvm::sys::path::append(candidates.back(), "..", "qumir", "runtime", "libqumir_runtime.a");

    for (const auto& candidate : candidates) {
        if (llvm::sys::fs::exists(candidate)) {
            return std::string(candidate);
        }
    }

    throw std::runtime_error(
        "libqumir_runtime.a not found next to " + selfExePath + "; set QUMIR_RUNTIME to its path");
}

} // namespace

void TLLVMModuleArtifacts::Generate(std::ostream& os, bool generateAsm, bool generateObj) const {
    auto triple = Module->getTargetTriple();
    std::string errStr;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, errStr);
    if (!target) {
        throw std::runtime_error(std::string("lookupTarget failed: ") + errStr);
    }

    llvm::TargetOptions opt;
    auto RM = std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);
    std::string cpu = "generic";
    std::string features;
    if (NativeCode) {
        auto [nativeCpu, nativeFeatures] = GetNativeCpuAndFeatures();
        if (!nativeCpu.empty()) {
            cpu = nativeCpu;
        }
        features = std::move(nativeFeatures);
    }
    std::unique_ptr<llvm::TargetMachine> TM {
        target->createTargetMachine(
            triple,
            cpu,
            features,
            opt,
            RM,
            std::nullopt,
            DebugPoints ? llvm::CodeGenOptLevel::None : llvm::CodeGenOptLevel::Default)
    };
    if (!TM) {
        throw std::runtime_error("createTargetMachine failed");
    }
    Module->setDataLayout(TM->createDataLayout());

    llvm::legacy::PassManager pm;
    llvm::SmallVector<char, 0> buf;
    llvm::raw_svector_ostream rso(buf);

    if (generateAsm) {
        if (TM->addPassesToEmitFile(pm, rso, nullptr, AssemblyFile))
        {
            throw std::runtime_error("TargetMachine can't emit assembly");
        }
    } else if (generateObj) {
        if (TM->addPassesToEmitFile(pm, rso, nullptr, ObjectFile))
        {
            throw std::runtime_error("TargetMachine can't emit object code");
        }
    } else {
        // run c++ ${ObjFile} -o ${exeFile}
        if (TM->addPassesToEmitFile(pm, rso, nullptr, ObjectFile))
        {
            throw std::runtime_error("TargetMachine can't emit object code");
        }

        pm.run(*Module);

        std::error_code ec;
        llvm::SmallString<128> objPath;
        if (llvm::sys::fs::createTemporaryFile("qumir", "o", objPath)) {
            throw std::runtime_error("Failed to create temporary object file");
        }

        {
            llvm::raw_fd_ostream objFile(objPath, ec);
            if (ec) {
                throw std::runtime_error("Failed to open temporary object file: " + ec.message());
            }
            objFile.write(buf.data(), buf.size());
        }

        llvm::SmallString<128> exePath;
        if (llvm::sys::fs::createTemporaryFile("qumir", "", exePath)) {
            throw std::runtime_error("Failed to create temporary executable file");
        }

        std::string runtimePath = FindRuntimeLibrary();

        std::string cmd = "c++ " + std::string(objPath.c_str()) + " -o " + std::string(exePath.c_str()) + " " + runtimePath + " 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            throw std::runtime_error("Failed to execute c++ compiler");
        }

        char buffer[128];
        std::string compilerOutput;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            compilerOutput += buffer;
        }
        int compileResult = pclose(pipe);

        if (compileResult != 0) {
            std::cerr << "Compilation failed:\n" << compilerOutput << std::endl;
        } else {
            std::ifstream exeFile(exePath.c_str(), std::ios::binary);
            if (!exeFile) {
                std::cerr << "Failed to read executable file" << std::endl;
            } else {
                os << exeFile.rdbuf();
            }
        }

        llvm::sys::fs::remove(objPath);
        llvm::sys::fs::remove(exePath);

        return;
    }
    pm.run(*Module);

    os.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    os.flush();
}

} // namespace NQumir::NCodeGen
