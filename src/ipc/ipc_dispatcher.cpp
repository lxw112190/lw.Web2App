#include "lwweb/ipc/ipc_dispatcher.h"

#include "lwweb/common/file_utils.h"
#include "lwweb/common/logging.h"

#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace lwweb {
namespace {

std::string RequiredString(const nlohmann::json& params, const char* name) {
  const auto found = params.find(name);
  if (found == params.end() || !found->is_string() || found->get_ref<const std::string&>().empty())
    throw IpcException("INVALID_ARGUMENT", std::string(name) + " must be a non-empty string");
  return found->get<std::string>();
}

std::string PathText(const std::filesystem::path& path) { return path.u8string(); }

bool OptionalBool(const nlohmann::json& params, const char* name,
                  bool fallback = false) {
  const auto found = params.find(name);
  if (found == params.end()) return fallback;
  if (!found->is_boolean())
    throw IpcException("INVALID_ARGUMENT", std::string(name) + " must be a boolean");
  return found->get<bool>();
}

IpcException FilesystemError(const std::error_code& error, const char* operation) {
  if (error == std::errc::no_such_file_or_directory)
    return IpcException("NOT_FOUND", std::string(operation) + " failed: path not found");
  if (error == std::errc::file_exists)
    return IpcException("ALREADY_EXISTS", std::string(operation) + " failed: target exists");
  if (error == std::errc::permission_denied)
    return IpcException("PERMISSION_DENIED", std::string(operation) + " was denied");
  return IpcException("IO_ERROR", std::string(operation) + " failed");
}

}  // namespace

IpcDispatcher::IpcDispatcher(Manifest manifest, IpcRuntimeServices services)
    : manifest_(std::move(manifest)), services_(std::move(services)) {
  if (manifest_.ipc.enabled) {
    if (manifest_.mode != AppMode::Local)
      throw IpcException("PERMISSION_DENIED", "Native IPC is disabled in URL mode");
    filesystem_ = std::make_shared<IpcFilesystemPermissions>(
        manifest_.ipc.filesystem_roots, EffectiveAppId(manifest_));
  }
}

void IpcDispatcher::RequireCapability(const std::string& capability) const {
  if (!HasIpcCapability(manifest_.ipc, capability))
    throw IpcException("PERMISSION_DENIED", "Native capability is not granted");
}

IpcExecution IpcDispatcher::ExecutionFor(const std::string& method) const {
  if (method == "dialog.selectDirectory") return IpcExecution::UiThread;
  if (method.rfind("fs.", 0) == 0) return IpcExecution::Worker;
  return IpcExecution::Immediate;
}

bool IpcDispatcher::TryBegin(const std::string& id) {
  std::lock_guard lock(pending_mutex_);
  if (pending_ids_.size() >= 64 || pending_ids_.count(id)) return false;
  pending_ids_.insert(id);
  return true;
}

void IpcDispatcher::End(const std::string& id) {
  std::lock_guard lock(pending_mutex_);
  pending_ids_.erase(id);
}

IpcResponse IpcDispatcher::Dispatch(const IpcRequest& request) {
  try {
    if (!manifest_.ipc.enabled)
      throw IpcException("PERMISSION_DENIED", "Native IPC is disabled");
    IpcResponse response;
    response.id = request.id;
    response.ok = true;
    response.result = DispatchImpl(request);
    return response;
  } catch (const IpcException& error) {
    return MakeIpcError(request.id, error.Code(), error.what());
  } catch (const std::filesystem::filesystem_error&) {
    return MakeIpcError(request.id, "IO_ERROR", "Filesystem operation failed");
  } catch (const std::exception&) {
    return MakeIpcError(request.id, "INTERNAL_ERROR", "Native operation failed");
  }
}

nlohmann::json IpcDispatcher::DispatchImpl(const IpcRequest& request) {
  if (request.method == "app.getInfo") {
    RequireCapability("app.info");
    return {{"appId", EffectiveAppId(manifest_)},
            {"title", manifest_.title},
            {"platform", services_.platform},
            {"arch", services_.arch},
            {"version", services_.runtime_version}};
  }
  if (request.method == "dialog.selectDirectory") {
    RequireCapability("dialog.directory");
    if (!services_.select_directory)
      throw IpcException("UNSUPPORTED", "Directory dialog is unavailable");
    const auto selected = services_.select_directory();
    if (!selected) throw IpcException("USER_CANCELLED", "Directory selection was canceled");
    filesystem_->AddSessionGrant(*selected);
    return {{"path", PathText(*selected)}};
  }
  if (request.method == "fs.list") {
    RequireCapability("fs.list");
    const auto path = filesystem_->RequireExisting(RequiredString(request.params, "path"));
    std::error_code error;
    if (!std::filesystem::is_directory(path, error))
      throw IpcException("INVALID_ARGUMENT", "fs.list path must be a directory");
    nlohmann::json entries = nlohmann::json::array();
    std::size_t count = 0;
    for (std::filesystem::directory_iterator iterator(path, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (++count > 10000)
        throw IpcException("IO_ERROR", "Directory entry count exceeds the safety limit");
      const auto& item = *iterator;
      const auto status = item.symlink_status(error);
      if (error) break;
      nlohmann::json entry = {{"name", item.path().filename().u8string()},
                              {"path", item.path().u8string()},
                              {"type", std::filesystem::is_symlink(status)
                                           ? "symlink"
                                           : std::filesystem::is_directory(status) ? "directory"
                                                                                   : "file"}};
      if (std::filesystem::is_regular_file(status)) {
        const auto size = item.file_size(error);
        if (!error) entry["size"] = size;
      }
      entries.push_back(std::move(entry));
    }
    if (error) throw FilesystemError(error, "Directory listing");
    return {{"entries", std::move(entries)}};
  }
  if (request.method == "fs.move") {
    RequireCapability("fs.move");
    const auto source = filesystem_->RequireExisting(RequiredString(request.params, "from"));
    const auto destination =
        filesystem_->RequireDestination(RequiredString(request.params, "to"));
    const bool overwrite = OptionalBool(request.params, "overwrite");
    std::error_code comparison_error;
    if (std::filesystem::equivalent(source, destination, comparison_error) &&
        !comparison_error)
      throw IpcException("INVALID_ARGUMENT", "Move source and destination must differ");
    std::error_code error;
    if (std::filesystem::exists(destination, error)) {
      if (!overwrite) throw IpcException("ALREADY_EXISTS", "Move destination already exists");
    }
#ifdef _WIN32
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
                        (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(source.c_str(), destination.c_str(), flags)) {
      error = std::error_code(static_cast<int>(GetLastError()),
                              std::system_category());
    }
#else
    std::filesystem::rename(source, destination, error);
#endif
    if (error) throw FilesystemError(error, "Move");
    return {{"path", PathText(destination)}};
  }
  if (request.method == "fs.delete") {
    RequireCapability("fs.delete");
    const auto path = filesystem_->RequireExisting(RequiredString(request.params, "path"));
    const bool recursive = OptionalBool(request.params, "recursive");
    std::error_code error;
    if (recursive)
      (void)std::filesystem::remove_all(path, error);
    else if (!std::filesystem::remove(path, error) && !error)
      throw IpcException("IO_ERROR", "Directory is not empty");
    if (error) throw FilesystemError(error, "Delete");
    return nlohmann::json::object();
  }
  throw IpcException("METHOD_NOT_FOUND", "Native method is not available");
}

std::string BuildIpcBridgeScript(const std::string& transport,
                                 const std::string& platform,
                                 const std::string& transport_token) {
  const auto platform_json = nlohmann::json(platform).dump();
  const auto token_json = nlohmann::json(transport_token + ":").dump();
  const std::string send = transport == "windows"
      ? "function send(m){chrome.webview.postMessage(m)};"
        "chrome.webview.addEventListener('message',function(e){receive(e.data)});"
      : "function send(m){window.webkit.messageHandlers.lwIpc.postMessage(" + token_json +
            "+JSON.stringify(m))};";
  return "(()=>{if(window.lw)return;let seq=0;const pending=new Map(),listeners=new Map();"
         "function receive(m){if(!m||m.v!==1)return;if(m.kind==='response'){const p=pending.get(m.id);"
         "if(!p)return;pending.delete(m.id);m.ok?p.resolve(m.result):p.reject(Object.assign(new Error("
         "m.error&&m.error.message||'Native operation failed'),{code:m.error&&m.error.code||'INTERNAL_ERROR'}));}"
         "else if(m.kind==='event'){(listeners.get(m.event)||[]).slice().forEach(f=>{try{f(m.data)}catch(_){}})}};" +
         send +
         "function invoke(method,params){return new Promise((resolve,reject)=>{if(typeof method!=='string'||!method)"
         "return reject(Object.assign(new Error('Invalid method'),{code:'INVALID_ARGUMENT'}));"
         "const id=String(++seq);pending.set(id,{resolve,reject});try{send({v:1,kind:'request',id,method,"
         "params:params==null?{}:params})}catch(e){pending.delete(id);reject(e)}})};"
         "function on(name,fn){if(typeof fn!=='function')return;const a=listeners.get(name)||[];a.push(fn);listeners.set(name,a)};"
         "function off(name,fn){const a=listeners.get(name)||[];listeners.set(name,a.filter(x=>x!==fn))};"
         "Object.defineProperty(window,'__lwIpcReceive',{value:receive});Object.defineProperty(window,'lw',{"
         "value:Object.freeze({platform:" + platform_json + ",invoke,on,off}),writable:false,configurable:false})})()";
}

}  // namespace lwweb
