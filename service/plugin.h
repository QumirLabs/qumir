#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <coroio/all.hpp>
#include <coroio/http/httpd.hpp>
#include <coroio/pipe/pipe.hpp>

namespace NQumir::NService {

// Bumped on every change to the types below or to the coroio code the plugin
// inlines (e.g. what awaitables queue in the host poller): the host refuses
// plugins built against another version.
inline constexpr int PluginApiVersion = 4;

using THandler = std::function<NNet::TFuture<void>(NNet::TRequest&, NNet::TResponse&)>;

// One finished /api/compile-* request.
struct TCompileEvent {
    std::string Target;     // "wasm" for Run, "ir", "ast", ... for the editor views
    std::string Code;
    std::string Output;     // qumirc stdout and stderr: the result for text targets, messages for "wasm"
    int ExitCode = 0;
};

using TCompileObserver = std::function<void(const TCompileEvent&)>;

// Everything a plugin gets from the host to serve its own endpoints.
struct TPluginContext {
    std::function<NNet::TPipe(const std::string&, const std::vector<std::string>&, bool)> PipeFactory;
    std::string BinaryDir;
    std::vector<std::string> Args;
    // Registers an observer called synchronously after each compilation.
    std::function<void(TCompileObserver)> AddCompileObserver;
};

// Built-in routes are matched first; a plugin can only add paths, never shadow them.
class TRouteTable {
public:
    void Get(std::string path, THandler handler) {
        Gets.emplace(std::move(path), std::move(handler));
    }

    void Post(std::string path, THandler handler) {
        Posts.emplace(std::move(path), std::move(handler));
    }

    const THandler* FindGet(const std::string& path) const {
        return Find(Gets, path);
    }

    const THandler* FindPost(const std::string& path) const {
        return Find(Posts, path);
    }

private:
    using TMap = std::unordered_map<std::string, THandler>;

    static const THandler* Find(const TMap& map, const std::string& path) {
        auto it = map.find(path);
        return it == map.end() ? nullptr : &it->second;
    }

    TMap Gets;
    TMap Posts;
};

} // namespace NQumir::NService

// Every plugin defines it as `return NQumir::NService::PluginApiVersion;`.
extern "C" int QumirPluginApiVersion();

extern "C" void QumirPluginRegister(NQumir::NService::TRouteTable& routes,
                                    const NQumir::NService::TPluginContext& context);
