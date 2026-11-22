#pragma once

#include <qumir/modules/module.h>

namespace NQumir {
namespace NRegistry {

class RobotModule : public IModule {
public:
    explicit RobotModule(NAst::TTypeFactory& types);

    const std::string& Name() const override {
        static const std::string name = "Робот";
        return name;
    }

    const std::vector<TExternalFunction>& ExternalFunctions() const override {
        return ExternalFunctions_;
    }

    const std::vector<TExternalType>& ExternalTypes() const override {
        return ExternalTypes_;
    }

    const std::vector<TLiteralSuffix>& LiteralSuffixes() const override {
        return LiteralSuffixes_;
    }

    const std::vector<std::string>& Dependencies() const override {
        return Dependencies_;
    }

private:
    std::vector<TExternalFunction> ExternalFunctions_;
    std::vector<TExternalType> ExternalTypes_;
    std::vector<TLiteralSuffix> LiteralSuffixes_;
    std::vector<std::string> Dependencies_ = {};
};

} // namespace NRegistry
} // namespace NQumir
