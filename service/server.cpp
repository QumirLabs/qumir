#include <algorithm>
#include <chrono>
#include <iostream>
#include <fstream>
#include <optional>
#include <vector>
#include <map>
#include <set>

#include <filesystem>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Error.h>

#include <coroio/all.hpp>
#include <coroio/http/httpd.hpp>
#include <coroio/pipe/pipe.hpp>
#include <coroio/ws/utils.hpp>

#include <dlfcn.h>

#include "plugin.h"

#include <qumir/parser/json/parser.h>

using namespace NNet;

namespace {

auto FindHeader(const TRequest& request, std::string_view name) {
    const auto& headers = request.Headers();
    return std::find_if(headers.begin(), headers.end(), [name](const auto& header) {
        return llvm::StringRef(header.first).equals_insensitive(name);
    });
}

std::string UrlEncode(const std::string& str) {
    std::string result;
    for (char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            result += buf;
        }
    }
    return result;
}

} // namespace

struct TOptions {
    std::function<TPipe(const std::string&, const std::vector<std::string>&, bool)> PipeFactory;
    std::string StaticDir = "static";
    std::string BinaryDir = "bin";
    std::string ExamplesDir = "examples";
    std::string SharedLinksDir = "shared";
    std::vector<std::string> Plugins;
    std::vector<std::string> PluginArgs;
};

class TRouter : public IRouter {
public:
    TRouter(TOptions options)
        : PipeFactory(std::move(options.PipeFactory))
        , StaticDir(std::move(options.StaticDir))
        , BinaryDir(std::move(options.BinaryDir))
        , ExamplesDir(std::move(options.ExamplesDir))
        , SharedLinksDir(std::move(options.SharedLinksDir))
    {
        std::error_code ec;
        StaticBaseCanonical = std::filesystem::weakly_canonical(std::filesystem::path(StaticDir), ec);
        if (ec || StaticBaseCanonical.empty()) {
            // Fallback to lexical normalization if canonicalization fails
            StaticBaseCanonical = std::filesystem::path(StaticDir).lexically_normal();
        }

        BinaryBaseCanonical = std::filesystem::weakly_canonical(std::filesystem::path(BinaryDir), ec);
        if (ec || BinaryBaseCanonical.empty()) {
            // Fallback to lexical normalization if canonicalization fails
            BinaryBaseCanonical = std::filesystem::path(BinaryDir).lexically_normal();
        }

        ExamplesBaseCanonical = std::filesystem::weakly_canonical(std::filesystem::path(ExamplesDir), ec);
        if (ec || ExamplesBaseCanonical.empty()) {
            // Fallback to lexical normalization if canonicalization fails
            ExamplesBaseCanonical = std::filesystem::path(ExamplesDir).lexically_normal();
        }

        SharedLinksBaseCanonical = std::filesystem::weakly_canonical(std::filesystem::path(SharedLinksDir), ec);
        if (ec || SharedLinksBaseCanonical.empty()) {
            // Fallback to lexical normalization if canonicalization fails
            SharedLinksBaseCanonical = std::filesystem::path(SharedLinksDir).lexically_normal();
        }

        LoadPlugins(options.Plugins, options.PluginArgs);
    }

    TFuture<void> HandleRequest(TRequest& request, TResponse& response) override {
        if (request.Method() == "OPTIONS") {
            response.SetStatus(200);
            response.SetHeader("Access-Control-Allow-Origin", "*");
            response.SetHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.SetHeader("Access-Control-Allow-Headers", "Content-Type, X-Qumir-O, X-Qumir-Syntax, X-Qumir-Debug-Points");
            response.SetHeader("Content-Length", "0");
            co_await response.SendHeaders();
        } else if (request.Method() == "GET") {
            co_await Get(request, response);
        } else if (request.Method() == "POST") {
            co_await Post(request, response);
        } else {
            response.SetStatus(405);
            response.SetHeader("Content-Type", "text/plain");
            response.SetHeader("Connection", "close");
            co_await response.SendHeaders();
            co_await response.WriteBodyFull("Method Not Allowed");
        }
    }

private:
    // Handles are never closed: registered handlers live as long as the process.
    void LoadPlugins(const std::vector<std::string>& paths, const std::vector<std::string>& args) {
        if (paths.empty()) {
            return;
        }

        NQumir::NService::TPluginContext context{
            .PipeFactory = PipeFactory,
            .BinaryDir = BinaryBaseCanonical.generic_string(),
            .Args = args,
            .AddCompileObserver = [this](NQumir::NService::TCompileObserver observer) {
                CompileObservers.push_back(std::move(observer));
            },
        };

        for (const auto& path : paths) {
            void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                throw std::runtime_error("Failed to load plugin " + path + ": " + dlerror());
            }
            PluginHandles.push_back(handle);

            // A plugin from another release must not take the playground down with it.
            auto* versionFn = reinterpret_cast<int (*)()>(dlsym(handle, "QumirPluginApiVersion"));
            int version = versionFn ? versionFn() : 1;
            if (version != NQumir::NService::PluginApiVersion) {
                std::cerr << "Skipping plugin " << path << ": built for plugin API version " << version
                          << ", the service provides " << NQumir::NService::PluginApiVersion << "\n";
                continue;
            }

            auto* registerFn = reinterpret_cast<void (*)(NQumir::NService::TRouteTable&,
                                                         const NQumir::NService::TPluginContext&)>(
                dlsym(handle, "QumirPluginRegister"));
            if (!registerFn) {
                throw std::runtime_error("Plugin " + path + " exports no QumirPluginRegister");
            }

            registerFn(Routes, context);
            std::cerr << "Loaded plugin " << path << "\n";
        }
    }

    void Trim(std::string& s) {
        size_t first = s.find_first_not_of(" \t\r\n");
        size_t last = s.find_last_not_of(" \t\r\n");
        if (first == std::string::npos || last == std::string::npos) {
            s = "";
        } else {
            s = s.substr(first, last - first + 1);
        }
    }

    // The version text comes from a separately packaged binary: keep the first
    // line only and drop anything that would break the JSON string.
    std::string SanitizeVersion(const std::string& raw) {
        std::string out;
        for (char c : raw) {
            if (c == '\n' || c == '\r') {
                break;
            }
            if (static_cast<unsigned char>(c) < 0x20 || c == '"' || c == '\\') {
                continue;
            }
            out += c;
        }
        Trim(out);
        return out;
    }

    TFuture<void> SendJson(TResponse& response, const std::string& json, int statusCode = 200) {
        response.SetStatus(statusCode);
        response.SetHeader("Content-Type", "application/json; charset=utf-8");
        response.SetHeader("Content-Length", std::to_string(json.size()));
        co_await response.SendHeaders();
        co_await response.WriteBodyFull(json);
    }

    TFuture<void> Send404(TResponse& response, const std::string& message = "Not Found") {
        response.SetStatus(404);
        response.SetHeader("Content-Type", "text/plain");
        response.SetHeader("Content-Length", std::to_string(message.size()));
        co_await response.SendHeaders();
        co_await response.WriteBodyFull(message);
    }

    struct TExampleGroup {
        std::string Title;
        std::vector<std::pair<std::string, std::string>> Examples; // path, title
    };

    // examples/index.json lists the groups and titles in the order shown;
    // .kum files missing from it go to a trailing "Другое" group.
    std::vector<TExampleGroup> ExampleGroups() {
        using namespace NQumir::NAst::NJson;
        std::vector<TExampleGroup> groups;
        std::set<std::string> listed;
        auto stringOf = [](IValue* value) -> const std::string* {
            auto* str = value ? value->As<TString>() : nullptr;
            return str ? &str->Value : nullptr;
        };
        std::ifstream ifs(ExamplesBaseCanonical / "index.json", std::ios::binary);
        TTokenStream stream(ifs);
        auto json = TParser().Parse(stream);
        if (!json && std::filesystem::exists(ExamplesBaseCanonical / "index.json")) {
            std::cerr << "examples/index.json: " << json.error().ToString() << "\n";
        }
        auto* root = json && json->Root() ? json->Root()->As<TObject>() : nullptr;
        auto* groupsArr = root && root->Get("groups") ? root->Get("groups")->As<TArray>() : nullptr;
        for (auto* groupVal : groupsArr ? groupsArr->Elements : std::vector<IValue*>{}) {
            auto* groupObj = groupVal->As<TObject>();
            if (!groupObj) continue;
            auto* title = stringOf(groupObj->Get("title"));
            TExampleGroup group{.Title = title ? *title : ""};
            auto* examples = groupObj->Get("examples") ? groupObj->Get("examples")->As<TArray>() : nullptr;
            for (auto* exampleVal : examples ? examples->Elements : std::vector<IValue*>{}) {
                auto* exampleObj = exampleVal->As<TObject>();
                auto* examplePath = exampleObj ? stringOf(exampleObj->Get("path")) : nullptr;
                if (!examplePath || !std::filesystem::is_regular_file(ExamplesBaseCanonical / *examplePath)) {
                    continue;
                }
                auto* exampleTitle = stringOf(exampleObj->Get("title"));
                listed.insert(*examplePath);
                group.Examples.emplace_back(*examplePath, exampleTitle ? *exampleTitle : *examplePath);
            }
            if (!group.Examples.empty()) {
                groups.push_back(std::move(group));
            }
        }

        TExampleGroup other{.Title = "Другое"};
        for (auto& p : std::filesystem::recursive_directory_iterator(ExamplesBaseCanonical)) {
            if (!p.is_regular_file()) continue;
            auto ext = p.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".kum") continue;
            auto relPath = std::filesystem::relative(p.path(), ExamplesBaseCanonical).generic_string();
            if (!listed.contains(relPath)) {
                other.Examples.emplace_back(relPath, relPath);
            }
        }
        std::sort(other.Examples.begin(), other.Examples.end());
        if (!other.Examples.empty()) {
            groups.push_back(std::move(other));
        }
        return groups;
    }

    std::map<std::string, std::string> ExampleTitles() {
        std::map<std::string, std::string> titles;
        for (const auto& group : ExampleGroups()) {
            for (const auto& [examplePath, title] : group.Examples) {
                titles.emplace(examplePath, title);
            }
        }
        return titles;
    }

    std::string ExamplesListJson() {
        llvm::json::Array groups;
        for (const auto& group : ExampleGroups()) {
            llvm::json::Array examples;
            for (const auto& [examplePath, title] : group.Examples) {
                examples.push_back(llvm::json::Object{{"path", examplePath}, {"title", title}});
            }
            groups.push_back(llvm::json::Object{{"title", group.Title}, {"examples", std::move(examples)}});
        }
        std::string jsonStr;
        llvm::raw_string_ostream os(jsonStr);
        os << llvm::json::Value(llvm::json::Object{{"groups", std::move(groups)}});
        os.flush();
        return jsonStr;
    }

    TFuture<void> Get(TRequest& request, TResponse& response) {
        auto&& path = request.Uri().Path();
        if (path == "/api/version") {
            // The compiler is packaged separately and may be upgraded without
            // restarting us, so its version is asked for and cached briefly.
            auto now = std::chrono::steady_clock::now();
            if (CompilerVersion.empty() || (now - CompilerVersionTime) >= VersionCacheDuration) {
                auto qumirc = (BinaryBaseCanonical / "qumirc").generic_string();
                auto [out, code] = co_await ReadPipe(qumirc, {"--version"}, false, false, true);
                out = SanitizeVersion(out);
                CompilerVersion = (code == 0 && !out.empty()) ? out : "unknown";
                CompilerVersionTime = now;
            }
            co_await SendJson(response, "\"srv:" QUMIR_VERSION_STRING ";comp:" + CompilerVersion + "\"");
            co_return;
        } else if (path == "/api/examples") {
            co_await SendJson(response, ExamplesListJson());
        } else if (path == "/api/example") {
            auto queryParams = request.Uri().QueryParameters();
            auto it = queryParams.find("path");
            if (it == queryParams.end()) {
                co_await SendJson(response, "{\"error\":\"missing 'path' query parameter\"}", 400);
                co_return;
            }

            // Build full path to .kum file
            std::filesystem::path kumPath = ExamplesBaseCanonical / it->second;
            if (!std::filesystem::exists(kumPath) || !std::filesystem::is_regular_file(kumPath)) {
                co_await Send404(response);
                co_return;
            }

            // Read code
            std::ifstream ifs(kumPath, std::ios::binary);
            std::string code((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

            llvm::json::Object result;
            result["code"] = code;
            auto titles = ExampleTitles();
            if (auto title = titles.find(it->second); title != titles.end()) {
                result["title"] = title->second;
            }

            // Check for metadata .json file
            auto jsonPath = kumPath;
            jsonPath.replace_extension(".json");
            if (std::filesystem::exists(jsonPath) && std::filesystem::is_regular_file(jsonPath)) {
                std::ifstream mfs(jsonPath, std::ios::binary);
                std::string metaContent((std::istreambuf_iterator<char>(mfs)), std::istreambuf_iterator<char>());

                llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(metaContent);
                if (parsed) {
                    if (auto *metaObj = parsed->getAsObject()) {
                        // Copy args if present
                        if (auto *argsVal = metaObj->get("args")) {
                            result["args"] = *argsVal;
                        }
                        // Load files listed in metadata
                        if (auto *filesArr = metaObj->getArray("files")) {
                            std::filesystem::path baseDir = kumPath.parent_path();
                            llvm::json::Array loadedFiles;
                            for (const auto& fileEntry : *filesArr) {
                                if (auto *fileObj = fileEntry.getAsObject()) {
                                    auto nameOpt = fileObj->getString("name");
                                    auto pathOpt = fileObj->getString("path");
                                    if (nameOpt && pathOpt) {
                                        std::filesystem::path filePath = baseDir / std::string(*pathOpt);
                                        if (std::filesystem::exists(filePath) && std::filesystem::is_regular_file(filePath)) {
                                            std::ifstream ffs(filePath, std::ios::binary);
                                            std::string fileContent((std::istreambuf_iterator<char>(ffs)), std::istreambuf_iterator<char>());
                                            loadedFiles.push_back(llvm::json::Object{
                                                {"name", std::string(*nameOpt)},
                                                {"content", fileContent}
                                            });
                                        }
                                    }
                                }
                            }
                            if (!loadedFiles.empty()) {
                                result["files"] = std::move(loadedFiles);
                            }
                        }
                    }
                }
            }

            std::string jsonStr;
            llvm::raw_string_ostream os(jsonStr);
            os << llvm::json::Value(std::move(result));
            os.flush();
            co_await SendJson(response, jsonStr);
        } else if (path == "/api/share") {
            co_await ServeShare(request, response);
        } else if (path.find("/s/") == 0) {
            // redirect => /index.html?share={uriencode(sid)}
            auto shareId = path.substr(3); // after /s/
            // file = path / shareId . kum
            std::filesystem::path shareFilePath = SharedLinksBaseCanonical / (shareId + ".kum");
            if (!std::filesystem::exists(shareFilePath) || !std::filesystem::is_regular_file(shareFilePath)) {
                co_await Send404(response);
                co_return;
            }
            std::string redirectUrl = "/index.html?share=" + UrlEncode(shareId);
            response.SetStatus(302);
            response.SetHeader("Location", redirectUrl);
            response.SetHeader("Content-Length", "0");
            co_await response.SendHeaders();
        } else if (auto* handler = Routes.FindGet(path)) {
            co_await (*handler)(request, response);
        } else {
            co_await ServeStaticFile(response, request.Uri().Path(), StaticBaseCanonical);
        }
    }

    TFuture<void> Post(TRequest& request, TResponse& response) {
        auto&& path = request.Uri().Path();
        if (path == "/api/compile-ast") {
            co_await Compile(request, response, "ast");
        } else if (path == "/api/compile-transformed-ast") {
            co_await Compile(request, response, "transformed-ast");
        } else if (path == "/api/compile-ir") {
            co_await Compile(request, response, "ir");
        } else if (path == "/api/compile-llvm") {
            co_await Compile(request, response, "llvm");
        } else if (path == "/api/compile-asm") {
            co_await Compile(request, response, "asm");
        } else if (path == "/api/compile-llvm") {
            co_await Compile(request, response, "llvm");
        } else if (path == "/api/compile-wasm") {
            co_await Compile(request, response, "wasm");
        } else if (path == "/api/compile-wasm-text") {
            co_await Compile(request, response, "wasm-text");
        } else if (path == "/api/share") {
            co_await ServeShareCreate(request, response);
        } else if (auto* handler = Routes.FindPost(path)) {
            co_await (*handler)(request, response);
        } else {
            co_await Send404(response);
        }
    }

    void NotifyCompile(const std::string& target, std::string code, std::string output, int exitCode) {
        if (CompileObservers.empty()) {
            return;
        }
        const NQumir::NService::TCompileEvent event{
            .Target = target,
            .Code = std::move(code),
            .Output = std::move(output),
            .ExitCode = exitCode,
        };
        for (const auto& observer : CompileObservers) {
            observer(event);
        }
    }

    TFuture<void> Compile(TRequest& request, TResponse& response, const std::string& target) {
        int olevel = 0;
        auto it = FindHeader(request, "X-Qumir-O");
        if (it != request.Headers().end()) {
            std::string val(it->second);
            bool ok = true;
            try {
                olevel = std::stoi(val);
            } catch (...) {
                ok = false;
            }
            if (!ok) {
                co_await SendJson(response, "{\"error\":\"X-Qumir-O must be numeric\"}", 400);
                co_return;
            }
        }
        if (olevel < 0) olevel = 0; if (olevel > 3) olevel = 3;

        bool coreInput = false;
        {
            auto sit = FindHeader(request, "X-Qumir-Syntax");
            if (sit != request.Headers().end() && sit->second == "core") {
                coreInput = true;
            }
        }

        auto qumirc = (BinaryBaseCanonical / "qumirc").generic_string();
        std::vector<std::string> args;

        auto printCmd = [&]() {
            std::string cmdStr = qumirc;
            for (const auto& arg : args) cmdStr += " " + arg;
            std::cerr << "Running command: " << cmdStr << std::endl;
        };

        // Stream for all but wasm binary (wasm-ld does not support streaming)

        if (target != "wasm") {
            if (target == "ast") {
                args = {"--ast", "-o", "-", "-"};
            } else if (target == "transformed-ast") {
                args = {"--transformed-ast", "-o", "-", "-"};
            } else if (target == "ir") {
                args = {"--ir", "-O" + std::to_string(olevel), "-o", "-", "-"};
            } else if (target == "llvm") {
                args = {"--llvm", "-O" + std::to_string(olevel), "-o", "-", "-"};
            } else if (target == "asm") {
                args = {"-S", "-O" + std::to_string(olevel), "-o", "-", "-"};
            } else if (target == "wasm-text") {
                args = {"--wasm", "-S", "-O" + std::to_string(olevel), "-o", "-", "-"};
            }
            if (coreInput && !args.empty()) {
                args.insert(args.begin(), "--core");
            }

            printCmd();
            auto pipe = PipeFactory(qumirc, args, /* stderr to stdout */ true);

            const bool observed = !CompileObservers.empty();
            std::string code;
            std::string output;
            {
                char ibuf[1024];
                while (true) {
                    ssize_t n = co_await request.ReadBodySome(ibuf, sizeof(ibuf));
                    if (n <= 0) break;
                    if (observed) code.append(ibuf, n);
                    co_await TByteWriter(pipe).Write(ibuf, n);
                }
                pipe.CloseWrite();
            }

            response.SetStatus(200);
            response.SetHeader("Content-Type", "text/plain; charset=utf-8");
            response.SetHeader("Transfer-Encoding", "chunked");
            response.SetHeader("X-Qumir-O", std::to_string(olevel));
            co_await response.SendHeaders();

            char obuf[4096];
            auto reader = TByteReader(pipe); // buffered reader for pipe
            while (true) {
                ssize_t r = co_await reader.ReadSome(obuf, sizeof(obuf));
                if (r <= 0) break;
                if (observed) output.append(obuf, r);
                co_await response.WriteBodyChunk(obuf, r);
            }
            co_await response.WriteBodyChunk("", 0);
            int exitCode = pipe.Wait();
            NotifyCompile(target, std::move(code), std::move(output), exitCode);
            co_return;
        }

        // wasm binary: keep file-based path (read entire body)
        std::string code = co_await request.ReadBodyFull();
        if (code.empty()) {
            co_await SendJson(response, "{\"error\":\"empty body\"}", 400);
            co_return;
        }
        auto src = WriteTemp(code);
        std::string dst;
        std::string contentType = "application/wasm";
        dst = src + ".wasm";
        auto debug = FindHeader(request, "X-Qumir-Debug-Points");
        const bool debugPoints = debug != request.Headers().end() && debug->second == "1";
        if (debugPoints) {
            olevel = 0;
        }
        args = {"--wasm", "-O" + std::to_string(olevel), "-o", dst, src};
        if (debugPoints) {
            args.insert(args.begin(), "--debug-points");
        }
        if (coreInput) {
            args.insert(args.begin(), "--core");
        }

        printCmd();
        auto [output, exitCode] = co_await ReadPipe(qumirc, args, /*stderr*/ false, /*searchExe*/ false, /*stderrToStdout*/ true);
        NotifyCompile(target, code, output, exitCode);
        if (exitCode != 0) {
            std::string errBody;
            llvm::json::Object obj;
            obj["error"] = output.empty()
                ? "compilation failed"
                : output;
            obj["output"] = output;
            llvm::raw_string_ostream os(errBody);
            os << llvm::json::Value(std::move(obj));
            co_await SendJson(response, errBody, 400);
        } else {
            std::ifstream ifs(dst, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            response.SetStatus(200);
            response.SetHeader("Content-Type", contentType);
            response.SetHeader("Content-Length", std::to_string(content.size()));
            response.SetHeader("X-Qumir-O", std::to_string(olevel));
            co_await response.SendHeaders();
            co_await response.WriteBodyFull(content);
        }
        std::remove(src.c_str());
        std::remove(dst.c_str());
    }

    TFuture<void> ServeShare(const TRequest& request, TResponse& response) {
        auto queryParams = request.Uri().QueryParameters();
        auto it = queryParams.find("id");
        if (it == queryParams.end()) {
            co_await SendJson(response, "{\"error\":\"missing 'id' query parameter\"}", 400);
            co_return;
        }
        auto shareId = it->second;
        // file = path / shareId . kum
        std::filesystem::path shareFilePath = SharedLinksBaseCanonical / (shareId + ".kum");
        if (!std::filesystem::exists(shareFilePath) || !std::filesystem::is_regular_file(shareFilePath)) {
            co_await Send404(response);
            co_return;
        }
        std::ifstream ifs(shareFilePath, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        std::filesystem::path shareMetaPath = SharedLinksBaseCanonical / (shareId + ".json");
        if (std::filesystem::exists(shareMetaPath) && std::filesystem::is_regular_file(shareMetaPath)) {
            // read metadata
            std::ifstream mfs(shareMetaPath, std::ios::binary);
            std::string metaContent((std::istreambuf_iterator<char>(mfs)), std::istreambuf_iterator<char>());

            llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(metaContent);
            llvm::json::Object metaObj;
            if (parsed) {
                if (auto *obj = parsed->getAsObject()) {
                    metaObj = *obj;
                }
            }

            llvm::json::Object rootObj;
            rootObj["code"] = content;
            for (auto &entry : metaObj) {
                rootObj[entry.first] = entry.second;
            }

            std::string json;
            {
                llvm::raw_string_ostream os(json);
                os << llvm::json::Value(std::move(rootObj));
            }
            co_await SendJson(response, json);
            co_return;
        }

        response.SetStatus(200);
        response.SetHeader("Content-Type", "text/plain; charset=utf-8");
        response.SetHeader("Content-Length", std::to_string(content.size()));
        co_await response.SendHeaders();
        co_await response.WriteBodyFull(content);
    }

    TFuture<void> ServeShareCreate(TRequest& request, TResponse& response) {
        std::string payloadJson = co_await request.ReadBodyFull();
        // Parse JSON payload and extract 'code', optional 'args' and 'stdin'
        llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(payloadJson);
        if (!parsed) {
            std::string errMsg;
            llvm::handleAllErrors(parsed.takeError(), [&](const llvm::ErrorInfoBase &E){ errMsg = E.message(); });
            co_await SendJson(response, std::string("{\"error\":\"json parse failed: ") + errMsg + "\"}", 400);
            co_return;
        }

        llvm::json::Value &root = *parsed;
        auto *obj = root.getAsObject();
        if (!obj) {
            co_await SendJson(response, "{\"error\":\"object root required\"}", 400);
            co_return;
        }

        // code
        std::string code;
        if (auto *codeVal = obj->get("code")) {
            if (auto codeStr = codeVal->getAsString()) {
                code = *codeStr;
            } else {
                co_await SendJson(response, "{\"error\":\"code must be string\"}", 400);
                co_return;
            }
        } else {
            co_await SendJson(response, "{\"error\":\"code missing\"}", 400);
            co_return;
        }

        const llvm::json::Value* argsVal = obj->get("args");
        const llvm::json::Value* stdinVal = obj->get("stdin");
        const llvm::json::Value* filesVal = obj->get("files");
        std::string shareId = ComputeSharedId(code, argsVal, stdinVal, filesVal);

        // write code to file
        std::filesystem::path shareFilePath = SharedLinksBaseCanonical / (shareId + ".kum");
        {
            std::ofstream ofs(shareFilePath, std::ios::binary);
            ofs << code;
        }
        // write metadata to .json
        std::filesystem::path shareMetaPath = SharedLinksBaseCanonical / (shareId + ".json");
        {
            llvm::json::Object metaObj;
            if (auto argsVal = obj->get("args")) {
                metaObj["args"] = *argsVal;
            }
            if (auto stdinVal = obj->get("stdin")) {
                metaObj["stdin"] = *stdinVal;
            }
            if (auto filesVal = obj->get("files")) {
                metaObj["files"] = *filesVal;
            }
            std::string metaJson;
            {
                llvm::raw_string_ostream os(metaJson);
                os << llvm::json::Value(std::move(metaObj));
            }
            std::ofstream mofs(shareMetaPath, std::ios::binary);
            mofs << metaJson;
        }

        std::string host;
        auto maybeHost = request.Headers().find("Host");
        if (maybeHost == request.Headers().end()) {
            host = "localhost";
        } else {
            host = maybeHost->second;
        }
        std::string base = "http://" + host;
        co_await SendJson(response, std::string("{\"id\":\"") + shareId +
            "\",\"url\":\"" + base + "/s/" + shareId +
            "\",\"raw_url\":\"" + base + "/api/share?id=" + UrlEncode(shareId) + "\"}");
    }

    std::string ComputeSharedId(const std::string& code, const llvm::json::Value* argsVal, const llvm::json::Value* stdinVal, const llvm::json::Value* filesVal) {
        // simple hash: sha1(code + args + stdin + files)
        std::string toHash = code;
        if (argsVal) {
            std::string argsStr;
            {
                llvm::raw_string_ostream os(argsStr);
                os << *argsVal;
            }
            toHash += argsStr;
        }
        if (stdinVal) {
            std::string stdinStr;
            {
                llvm::raw_string_ostream os(stdinStr);
                os << *stdinVal;
            }
            toHash += stdinStr;
        }
        if (filesVal) {
            std::string filesStr;
            {
                llvm::raw_string_ostream os(filesStr);
                os << *filesVal;
            }
            toHash += filesStr;
        }

        // compute sha1
        unsigned char hash[41];
        NUtils::SHA1Digest(reinterpret_cast<const unsigned char*>(toHash.data()), toHash.size(), hash);
        // convert to hex
        std::string sharedId = NUtils::Base64Encode(hash, 9);
        // make URL-safe: replace '+' with '-', '/' with '_', remove '='
        std::string hashHex;
        for (char c : sharedId) {
            if (c == '+') {
                hashHex += '-';
            } else if (c == '/') {
                hashHex += '_';
            } else if (c == '=') {
                // skip
            } else {
                hashHex += c;
            }
        }
        return hashHex;
    }

    TFuture<std::pair<std::string,int>> ReadPipe(std::string exe, const std::vector<std::string>& args, bool stderr = true, bool searchExe = false, bool stderrToStdout = false) {
        if (searchExe) {
            exe = SearchInPath(exe);
        }
        auto pipe = PipeFactory(exe, args, stderrToStdout);
        pipe.CloseWrite();
        if (stderr) {
            pipe.CloseRead();
        } else {
            pipe.CloseErr();
        }

        std::string output;
        char buf[4096];
        while (true) {
            ssize_t n = stderr ? co_await pipe.ReadSomeErr(buf, sizeof(buf)) : co_await pipe.ReadSome(buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            output.append(buf, n);
        }

        auto exitCode = pipe.Wait();
        co_return std::make_pair(output, exitCode);
    }

    std::string SearchInPath(const std::string& exe) {
        if (Path.empty()) {
            Path = std::getenv("PATH");
        }
        // split Path by ':' and search for exe
        std::istringstream iss(Path);
        std::string dir;
        while (std::getline(iss, dir, ':')) {
            std::string candidate = dir + "/" + exe;
            if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
        }
        return exe;
    }

    std::string WriteTemp(const std::string& text) {
        namespace fs = std::filesystem;
        fs::path tempDir = fs::temp_directory_path();
        fs::path tempFile = tempDir / fs::path("qumir_temp_XXXXXX");
        std::string tempFileStr = tempFile.generic_string();
        std::vector<char> tmpl(tempFileStr.begin(), tempFileStr.end());
        tmpl.push_back('\0');
        int fd = mkstemp(tmpl.data());
        if (fd == -1) {
            throw std::runtime_error("failed to create temporary file");
        }
        std::string finalPath = std::string(tmpl.data());
        {
            std::ofstream ofs(finalPath, std::ios::binary);
            ofs << text;
        }
        close(fd);
        return finalPath;
    }

    TFuture<void> ServeStaticFile(TResponse& response, std::string uriPath, std::filesystem::path baseCanonical) {
        namespace fs = std::filesystem;

        // Build a path relative to the static base
        // Ensure no leading slash to keep path relative when appending
        if (!uriPath.empty() && uriPath.front() == '/') {
            uriPath.erase(0, 1);
        }

        fs::path rel = fs::path(uriPath).lexically_normal();

        // If empty or directory-like (trailing slash), serve index.html
        if (uriPath.empty() || (!uriPath.empty() && uriPath.back() == '/')) {
            rel /= "index.html";
        }

        // Security check: ensure the relative path doesn't escape the base via ..
        // Do this BEFORE resolving symlinks to prevent path traversal attacks
        auto relStr = rel.generic_string();
        if (relStr.find("..") != std::string::npos) {
            co_await Send404(response);
            co_return;
        }

        // Build candidate path (may contain symlinks)
        fs::path candidate = baseCanonical / rel;

        // Resolve symlinks to get the actual file path
        std::error_code ec;
        fs::path target = fs::canonical(candidate, ec);
        if (ec) {
            // canonical failed - file doesn't exist or broken symlink
            co_await Send404(response);
            co_return;
        }

        // If requested a directory (without trailing slash), also try index.html
        if (fs::is_directory(target)) {
            target /= "index.html";
            if (!fs::exists(target)) {
                co_await Send404(response);
                co_return;
            }
        }

        if (!fs::exists(target) || !fs::is_regular_file(target)) {
            co_await Send404(response);
            co_return;
        }

        // Read and serve the file
        std::ifstream file(target, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        response.SetStatus(200);
        std::string extension = target.extension().string();
        auto it = MimeTypes.find(extension);
        if (it != MimeTypes.end()) {
            response.SetHeader("Content-Type", it->second);
        } else {
            response.SetHeader("Content-Type", "application/octet-stream");
        }
        response.SetHeader("Content-Length", std::to_string(data.size()));
        co_await response.SendHeaders();
        co_await response.WriteBodyFull(data);
    }

    std::unordered_map<std::string, std::string> MimeTypes = {
        {".html", "text/html"},
        {".htm", "text/html"},
        {".css", "text/css"},
        {".js", "application/javascript"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},
        {".txt", "text/plain"},
        {".json", "application/json"},
        {".md", "text/markdown; charset=utf-8"},
    };

    std::function<TPipe(const std::string&, const std::vector<std::string>&, bool)> PipeFactory;
    std::string StaticDir;
    std::filesystem::path StaticBaseCanonical = std::filesystem::weakly_canonical(std::filesystem::path(StaticDir));
    std::string BinaryDir;
    std::filesystem::path BinaryBaseCanonical = std::filesystem::weakly_canonical(std::filesystem::path(BinaryDir));
    std::string ExamplesDir;
    std::filesystem::path ExamplesBaseCanonical = std::filesystem::weakly_canonical(std::filesystem::path(ExamplesDir));
    std::string SharedLinksDir;
    std::filesystem::path SharedLinksBaseCanonical = std::filesystem::weakly_canonical(std::filesystem::path(SharedLinksDir));
    std::string Path;

    static constexpr std::chrono::minutes VersionCacheDuration{5};
    std::string CompilerVersion;
    std::chrono::steady_clock::time_point CompilerVersionTime;

    NQumir::NService::TRouteTable Routes;
    std::vector<NQumir::NService::TCompileObserver> CompileObservers;
    std::vector<void*> PluginHandles;
};

int main(int argc, char** argv) {
    NNet::TInitializer init;
    int port = 8080;
    TOptions options;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i < argc-1) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--static-dir") && i < argc-1) {
            options.StaticDir = argv[++i];
        } else if (!strcmp(argv[i], "--binary-dir") && i < argc-1) {
            options.BinaryDir = argv[++i];
        } else if (!strcmp(argv[i], "--examples-dir") && i < argc-1) {
            options.ExamplesDir = argv[++i];
        } else if (!strcmp(argv[i], "--shared-links-dir") && i < argc-1) {
            options.SharedLinksDir = argv[++i];
        } else if (!strcmp(argv[i], "--plugin") && i < argc-1) {
            options.Plugins.push_back(argv[++i]);
        } else if (!strcmp(argv[i], "--plugin-arg") && i < argc-1) {
            options.PluginArgs.push_back(argv[++i]);
        } else if (!strcmp(argv[i], "--help")) {
            std::cout << "Usage: " << argv[0] << " [--port port] [--static-dir dir] [--binary-dir dir] [--examples-dir dir] [--shared-links-dir dir] [--plugin path.so] [--plugin-arg value]\n";
            return 0;
        }
    }

    TAddress address{"::", port};
    std::cerr << "Starting HTTP server on port " << port << "\n";

    auto logger = [](const std::string& msg) {
        std::cout << "[HTTPD] " << msg << std::endl;
    };

    using TPoller = TDefaultPoller;
    using TSocket = typename TPoller::TSocket;
    TLoop<TPoller> loop;
    TSocket listenSocket(loop.Poller(), address.Domain());
    listenSocket.Bind(address);
    listenSocket.Listen();
    std::cerr << "Listening on: " << listenSocket.LocalAddr()->ToString() << std::endl;

    auto pipeFactory = [&](const std::string& cmd, const std::vector<std::string>& args, bool stderrToStdout) {
        return TPipe(loop.Poller(), cmd, args, stderrToStdout);
    };

    options.PipeFactory = pipeFactory;

    std::optional<TRouter> router;
    try {
        router.emplace(options);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    TWebServer<TSocket> server(std::move(listenSocket), *router, logger);
    server.Start();

    loop.Loop();
    return 0;
}
