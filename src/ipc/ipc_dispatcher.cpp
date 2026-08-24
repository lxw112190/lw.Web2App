#include "lwweb/ipc/ipc_dispatcher.h"

#include "lwweb/common/logging.h"

#include <filesystem>

namespace lwweb {

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
    filesystem_->GrantDirectory(*selected);
    return {{"path", selected->u8string()}};
  }
  if (request.method == "fs.exists") {
    RequireCapability("fs.exists");
    return filesystem_->Exists(request.params);
  }
  if (request.method == "fs.list") {
    RequireCapability("fs.list");
    return filesystem_->List(request.params);
  }
  if (request.method == "fs.copy") {
    RequireCapability("fs.copy");
    return filesystem_->Copy(request.params);
  }
  if (request.method == "fs.move") {
    RequireCapability("fs.move");
    return filesystem_->Move(request.params);
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
