#include "lwweb/ipc/ipc_dispatcher.h"

#include "lwweb/common/logging.h"
#include "lwweb/runtime/local_file_grant.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace lwweb {
namespace {

OpenFileDialogOptions ParseOpenFileOptions(const nlohmann::json& params) {
  OpenFileDialogOptions options;
  if (const auto multiple = params.find("multiple"); multiple != params.end()) {
    if (!multiple->is_boolean())
      throw IpcException("INVALID_ARGUMENT", "multiple must be a boolean");
    options.multiple = multiple->get<bool>();
  }
  const auto filters = params.find("filters");
  if (filters == params.end()) return options;
  if (!filters->is_array() || filters->size() > 16)
    throw IpcException("INVALID_ARGUMENT", "filters must contain at most 16 entries");
  for (const auto& value : *filters) {
    if (!value.is_object())
      throw IpcException("INVALID_ARGUMENT", "Each file filter must be an object");
    const auto name = value.find("name");
    const auto extensions = value.find("extensions");
    if (name == value.end() || !name->is_string() || name->get_ref<const std::string&>().empty() ||
        name->get_ref<const std::string&>().size() > 128 ||
        name->get_ref<const std::string&>().find('\0') != std::string::npos ||
        extensions == value.end() ||
        !extensions->is_array() || extensions->empty() || extensions->size() > 32)
      throw IpcException("INVALID_ARGUMENT", "File filter name or extensions are invalid");
    OpenFileFilter filter;
    filter.name = name->get<std::string>();
    if (std::any_of(filter.name.begin(), filter.name.end(), [](unsigned char character) {
          return character < 0x20 || character == 0x7f;
        }))
      throw IpcException("INVALID_ARGUMENT", "File filter name is invalid");
    for (const auto& extension_value : *extensions) {
      if (!extension_value.is_string())
        throw IpcException("INVALID_ARGUMENT", "File extension must be a string");
      auto extension = extension_value.get<std::string>();
      if (!extension.empty() && extension.front() == '.') extension.erase(extension.begin());
      if (extension.empty() || extension.size() > 32 ||
          (extension != "*" &&
           !std::all_of(extension.begin(), extension.end(), [](unsigned char character) {
             return std::isalnum(character) || character == '.' || character == '_' ||
                    character == '-';
           })))
        throw IpcException("INVALID_ARGUMENT", "File extension is invalid");
      filter.extensions.push_back(std::move(extension));
    }
    options.filters.push_back(std::move(filter));
  }
  return options;
}

nlohmann::json PublicGrant(const LocalFileGrant& grant) {
  return {{"id", grant.id},
          {"name", grant.name},
          {"size", grant.size},
          {"mime", grant.mime},
          {"url", grant.url}};
}

bool IsValidGrantId(const std::string& id) {
  return id.size() == 32 &&
         std::all_of(id.begin(), id.end(), [](unsigned char character) {
           return std::isdigit(character) ||
                  (character >= 'a' && character <= 'f');
         });
}

}  // namespace

IpcDispatcher::IpcDispatcher(Manifest manifest, IpcRuntimeServices services)
    : manifest_(std::move(manifest)), services_(std::move(services)) {
  if (manifest_.ipc.enabled) {
    if (manifest_.mode != AppMode::Local)
      throw IpcException("PERMISSION_DENIED", "Native IPC is disabled in URL mode");
    auto permissions = std::make_shared<IpcFilesystemPermissions>(
        manifest_.ipc.filesystem_roots, EffectiveAppId(manifest_));
    filesystem_ = std::make_shared<IpcFilesystemAccess>(std::move(permissions));
  }
}

void IpcDispatcher::RequireCapability(const std::string& capability) const {
  if (!HasIpcCapability(manifest_.ipc, capability))
    throw IpcException("PERMISSION_DENIED", "Native capability is not granted");
}

IpcExecution IpcDispatcher::ExecutionFor(const std::string& method) const {
  if (method == "dialog.selectDirectory" || method == "dialog.openFile")
    return IpcExecution::UiThread;
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
    filesystem_->GrantDirectory(*selected);
    return {{"path", selected->u8string()}};
  }
  if (request.method == "dialog.openFile") {
    RequireCapability("dialog.file");
    if (!services_.open_files || !services_.file_grants)
      throw IpcException("UNSUPPORTED", "System file dialog is unavailable");
    const auto options = ParseOpenFileOptions(request.params);
    const auto selected = services_.open_files(options);
    if (!selected) throw IpcException("USER_CANCELLED", "File selection was canceled");
    if (selected->empty() || selected->size() > 256)
      throw IpcException("INVALID_ARGUMENT", "Selected file count is invalid");
    nlohmann::json files = nlohmann::json::array();
    std::vector<std::string> created;
    try {
      for (const auto& path : *selected) {
        auto grant = services_.file_grants->Create(path);
        created.push_back(grant.id);
        files.push_back(PublicGrant(grant));
      }
    } catch (const std::exception&) {
      for (const auto& id : created) services_.file_grants->Revoke(id);
      throw IpcException("IO_ERROR", "Selected file cannot be granted");
    }
    return {{"files", std::move(files)}};
  }
  if (request.method == "file.revoke") {
    if (!HasIpcCapability(manifest_.ipc, "dialog.file") &&
        !HasIpcCapability(manifest_.ipc, "fs.read"))
      throw IpcException("PERMISSION_DENIED", "Native capability is not granted");
    if (!services_.file_grants)
      throw IpcException("UNSUPPORTED", "Local file grants are unavailable");
    const auto id = request.params.find("id");
    if (id == request.params.end() || !id->is_string() ||
        !IsValidGrantId(id->get_ref<const std::string&>()))
      throw IpcException("INVALID_ARGUMENT", "File grant ID is invalid");
    return {{"revoked", services_.file_grants->Revoke(id->get<std::string>())}};
  }
  if (request.method == "fs.exists") {
    RequireCapability("fs.exists");
    return filesystem_->Exists(request.params);
  }
  if (request.method == "fs.list") {
    RequireCapability("fs.list");
    return filesystem_->List(request.params);
  }
  if (request.method == "fs.openRead") {
    RequireCapability("fs.read");
    if (!services_.file_grants)
      throw IpcException("UNSUPPORTED", "Local file grants are unavailable");
    try {
      return PublicGrant(
          services_.file_grants->Create(filesystem_->OpenReadPath(request.params)));
    } catch (const IpcException&) {
      throw;
    } catch (const std::exception&) {
      throw IpcException("IO_ERROR", "Authorized file cannot be opened");
    }
  }
  if (request.method == "fs.mkdir") {
    RequireCapability("fs.mkdir");
    return filesystem_->MakeDirectory(request.params);
  }
  if (request.method == "fs.copy") {
    RequireCapability("fs.copy");
    return filesystem_->Copy(request.params);
  }
  if (request.method == "fs.move") {
    RequireCapability("fs.move");
    return filesystem_->Move(request.params);
  }
  if (request.method == "fs.trash") {
    RequireCapability("fs.trash");
    if (!services_.trash_file)
      throw IpcException("UNSUPPORTED", "System trash is unavailable");
    const auto path = filesystem_->TrashPath(request.params);
    services_.trash_file(path);
    return nlohmann::json::object();
  }
  if (request.method == "fs.delete") {
    RequireCapability("fs.delete");
    return filesystem_->Delete(request.params);
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
