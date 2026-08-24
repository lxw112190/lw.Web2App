#include "lwweb/pe/pe_resources.h"

#include "lwweb/common/error.h"
#include "lwweb/common/file_utils.h"
#include "lwweb/common/pe_version.h"
#include "lwweb/pe/authenticode.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <thread>
#include <vector>

namespace lwweb {
namespace {

#pragma pack(push, 2)
// ICO 文件的目录头，布局由 Windows 图标文件格式规定。
struct IconDirHeader {
  std::uint16_t reserved;
  std::uint16_t type;
  std::uint16_t count;
};
// ICO 文件中每张图像的位置与像素格式描述。
struct IconDirEntry {
  std::uint8_t width;
  std::uint8_t height;
  std::uint8_t color_count;
  std::uint8_t reserved;
  std::uint16_t planes;
  std::uint16_t bit_count;
  std::uint32_t size;
  std::uint32_t offset;
};
// 写入 RT_GROUP_ICON 的图像目录项；id 指向独立 RT_ICON 资源。
struct GroupIconEntry {
  std::uint8_t width;
  std::uint8_t height;
  std::uint8_t color_count;
  std::uint8_t reserved;
  std::uint16_t planes;
  std::uint16_t bit_count;
  std::uint32_t size;
  std::uint16_t id;
};
#pragma pack(pop)

// 保留 Win32 错误码，供外层区分参数错误与杀毒软件/资源管理器造成的短暂文件占用。
class ResourceUpdateError final : public Error {
 public:
  ResourceUpdateError(const std::string& operation, DWORD code)
      : Error(operation + " failed: " + WindowsErrorMessage(code)), code_(code) {}

  DWORD code() const { return code_; }

 private:
  DWORD code_;
};

[[noreturn]] void ThrowResourceError(const char* operation) {
  const auto code = GetLastError();
  throw ResourceUpdateError(operation, code);
}

bool IsTransientResourceError(DWORD code) {
  return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ||
         code == ERROR_LOCK_VIOLATION || code == ERROR_OPEN_FAILED ||
         code == ERROR_USER_MAPPED_FILE || code == ERROR_RETRY;
}

// BeginUpdateResource/EndUpdateResource 的事务式 RAII 包装。
// 未显式 Commit 时析构会丢弃本次资源更新，避免留下损坏的 PE。
class ResourceUpdate {
 public:
  explicit ResourceUpdate(const std::filesystem::path& path) {
    handle_ = BeginUpdateResourceW(path.c_str(), FALSE);
    if (!handle_) ThrowResourceError("BeginUpdateResource");
  }
  ~ResourceUpdate() {
    if (handle_) EndUpdateResourceW(handle_, TRUE);
  }
  HANDLE get() const { return handle_; }
  void Commit() {
    if (!EndUpdateResourceW(handle_, FALSE)) {
      handle_ = nullptr;
      ThrowResourceError("EndUpdateResource");
    }
    handle_ = nullptr;
  }
 private:
  HANDLE handle_ = nullptr;
};

void AppendWord(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void AppendDword(std::vector<std::uint8_t>& out, std::uint32_t value) {
  AppendWord(out, static_cast<std::uint16_t>(value));
  AppendWord(out, static_cast<std::uint16_t>(value >> 16));
}
void AppendWide(std::vector<std::uint8_t>& out, const std::wstring& text) {
  for (wchar_t ch : text) AppendWord(out, static_cast<std::uint16_t>(ch));
  AppendWord(out, 0);
}
void Align4(std::vector<std::uint8_t>& out) {
  while (out.size() % 4) out.push_back(0);
}
void PatchWord(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value) {
  out.at(at) = static_cast<std::uint8_t>(value);
  out.at(at + 1) = static_cast<std::uint8_t>(value >> 8);
}

std::size_t BeginBlock(std::vector<std::uint8_t>& out, const std::wstring& key,
                       std::uint16_t value_length, std::uint16_t type) {
  const auto start = out.size();
  AppendWord(out, 0);
  AppendWord(out, value_length);
  AppendWord(out, type);
  AppendWide(out, key);
  Align4(out);
  return start;
}
void EndBlock(std::vector<std::uint8_t>& out, std::size_t start) {
  const auto length = out.size() - start;
  if (length > 65535) throw Error("Version resource is too large");
  PatchWord(out, start, static_cast<std::uint16_t>(length));
}

std::vector<std::uint8_t> BuildVersionResource(const PeMetadata& metadata) {
  const auto version = ParsePeVersion(metadata.version);
  const auto normalized_version =
      std::to_wstring(version[0]) + L"." + std::to_wstring(version[1]) + L"." +
      std::to_wstring(version[2]) + L"." + std::to_wstring(version[3]);
  std::vector<std::uint8_t> out;
  const auto root = BeginBlock(out, L"VS_VERSION_INFO", sizeof(VS_FIXEDFILEINFO), 0);
  VS_FIXEDFILEINFO fixed{};
  fixed.dwSignature = 0xFEEF04BD;
  fixed.dwStrucVersion = 0x00010000;
  fixed.dwFileVersionMS = MAKELONG(version[1], version[0]);
  fixed.dwFileVersionLS = MAKELONG(version[3], version[2]);
  fixed.dwProductVersionMS = fixed.dwFileVersionMS;
  fixed.dwProductVersionLS = fixed.dwFileVersionLS;
  fixed.dwFileFlagsMask = VS_FFI_FILEFLAGSMASK;
  fixed.dwFileOS = VOS_NT_WINDOWS32;
  fixed.dwFileType = VFT_APP;
  const auto* fixed_bytes = reinterpret_cast<const std::uint8_t*>(&fixed);
  out.insert(out.end(), fixed_bytes, fixed_bytes + sizeof(fixed));
  Align4(out);

  const auto strings = BeginBlock(out, L"StringFileInfo", 0, 1);
  const auto table = BeginBlock(out, L"040904B0", 0, 1);
  const std::array<std::pair<std::wstring, std::wstring>, 7> values = {{
      {L"CompanyName", metadata.company_name},
      {L"FileDescription", metadata.file_description},
      {L"FileVersion", normalized_version},
      {L"LegalCopyright", metadata.copyright},
      {L"ProductName", metadata.product_name},
      {L"ProductVersion", normalized_version},
      {L"OriginalFilename", L"application.exe"}}};
  for (const auto& [key, value] : values) {
    const auto block = BeginBlock(out, key, static_cast<std::uint16_t>(value.size() + 1), 1);
    AppendWide(out, value);
    Align4(out);
    EndBlock(out, block);
  }
  EndBlock(out, table);
  EndBlock(out, strings);

  const auto vars = BeginBlock(out, L"VarFileInfo", 0, 1);
  const auto translation = BeginBlock(out, L"Translation", 4, 0);
  AppendWord(out, 0x0409);
  AppendWord(out, 1200);
  Align4(out);
  EndBlock(out, translation);
  EndBlock(out, vars);
  EndBlock(out, root);
  return out;
}

// 将大尺寸 PNG 等比缩小后重新编码。ICO 目录的宽高字段最多只能表达 256，
// 但设计软件通常导出 512 或 1024 像素图标，因此在写入 PE 前自动适配。
std::vector<std::uint8_t> ResizePngForIcon(IWICImagingFactory* factory,
                                           IWICBitmapFrameDecode* frame,
                                           UINT source_width, UINT source_height,
                                           UINT& output_width, UINT& output_height) {
  constexpr UINT kMaximumIconDimension = 256;
  const auto scale = std::min(
      static_cast<double>(kMaximumIconDimension) / source_width,
      static_cast<double>(kMaximumIconDimension) / source_height);
  output_width = std::max<UINT>(1, static_cast<UINT>(std::lround(source_width * scale)));
  output_height = std::max<UINT>(1, static_cast<UINT>(std::lround(source_height * scale)));

  Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
  if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
      FAILED(scaler->Initialize(frame, output_width, output_height,
                                WICBitmapInterpolationModeFant)))
    throw Error("Cannot resize PNG icon");

  Microsoft::WRL::ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
    throw Error("Cannot create PNG icon stream");
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
    throw Error("Cannot initialize PNG icon encoder");

  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> encoded_frame;
  Microsoft::WRL::ComPtr<IPropertyBag2> properties;
  if (FAILED(encoder->CreateNewFrame(&encoded_frame, &properties)) ||
      FAILED(encoded_frame->Initialize(properties.Get())) ||
      FAILED(encoded_frame->SetSize(output_width, output_height)))
    throw Error("Cannot initialize resized PNG icon frame");
  WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(encoded_frame->SetPixelFormat(&pixel_format)) ||
      FAILED(encoded_frame->WriteSource(scaler.Get(), nullptr)) ||
      FAILED(encoded_frame->Commit()) || FAILED(encoder->Commit()))
    throw Error("Cannot encode resized PNG icon");

  STATSTG statistics{};
  if (FAILED(stream->Stat(&statistics, STATFLAG_NONAME)) ||
      statistics.cbSize.QuadPart == 0 ||
      statistics.cbSize.QuadPart > static_cast<ULONGLONG>(MAXDWORD))
    throw Error("Resized PNG icon has an invalid size");
  HGLOBAL storage = nullptr;
  if (FAILED(GetHGlobalFromStream(stream.Get(), &storage)) || !storage)
    throw Error("Cannot read resized PNG icon");
  const auto* data = static_cast<const std::uint8_t*>(GlobalLock(storage));
  if (!data) throw Error("Cannot lock resized PNG icon");
  const auto size = static_cast<std::size_t>(statistics.cbSize.QuadPart);
  std::vector<std::uint8_t> bytes(data, data + size);
  GlobalUnlock(storage);
  return bytes;
}

void UpdateIcon(HANDLE update, const std::filesystem::path& path) {
  if (path.empty()) return;
  auto extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
  if (extension == L".png") {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
      throw Error("Cannot initialize Windows Imaging Component");
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)))
      throw Error("Cannot decode PNG icon");
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0, height = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
        width == 0 || height == 0)
      throw Error("PNG icon dimensions are invalid");
    // 限制异常图片，避免解码超大 PNG 时占用过多内存；常见的 512/1024/2048 图标均支持。
    if (width > 8192 || height > 8192)
      throw Error("PNG icon dimensions must not exceed 8192 pixels");
    auto bytes = ReadFileBytes(path);
    if (width > 256 || height > 256)
      bytes = ResizePngForIcon(factory.Get(), frame.Get(), width, height, width, height);
    if (!UpdateResourceW(update, MAKEINTRESOURCEW(3), MAKEINTRESOURCEW(1),
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                         const_cast<std::uint8_t*>(bytes.data()),
                         static_cast<DWORD>(bytes.size())))
      ThrowResourceError("UpdateResource(PNG icon)");
    IconDirHeader header{0, 1, 1};
    GroupIconEntry entry{static_cast<std::uint8_t>(width == 256 ? 0 : width),
                         static_cast<std::uint8_t>(height == 256 ? 0 : height), 0, 0, 1, 32,
                         static_cast<std::uint32_t>(bytes.size()), 1};
    std::vector<std::uint8_t> group(sizeof(header) + sizeof(entry));
    std::memcpy(group.data(), &header, sizeof(header));
    std::memcpy(group.data() + sizeof(header), &entry, sizeof(entry));
    if (!UpdateResourceW(update, MAKEINTRESOURCEW(14), MAKEINTRESOURCEW(1),
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), group.data(),
                         static_cast<DWORD>(group.size())))
      ThrowResourceError("UpdateResource(PNG group icon)");
    return;
  }
  if (extension != L".ico") throw Error("Icon must be a PNG or ICO file");
  const auto bytes = ReadFileBytes(path);
  if (bytes.size() < sizeof(IconDirHeader)) throw Error("ICO file is truncated");
  IconDirHeader header{};
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (header.reserved != 0 || header.type != 1 || header.count == 0 || header.count > 64)
    throw Error("ICO header is invalid");
  const auto directory_size = sizeof(header) + sizeof(IconDirEntry) * header.count;
  if (bytes.size() < directory_size) throw Error("ICO directory is truncated");
  std::vector<std::uint8_t> group(sizeof(header) + sizeof(GroupIconEntry) * header.count);
  std::memcpy(group.data(), &header, sizeof(header));
  for (std::uint16_t i = 0; i < header.count; ++i) {
    IconDirEntry entry{};
    std::memcpy(&entry, bytes.data() + sizeof(header) + i * sizeof(entry), sizeof(entry));
    if (entry.offset > bytes.size() || entry.size > bytes.size() - entry.offset)
      throw Error("ICO image range is invalid");
    const auto id = static_cast<std::uint16_t>(i + 1);
    if (!UpdateResourceW(update, MAKEINTRESOURCEW(3), MAKEINTRESOURCEW(id), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                         const_cast<std::uint8_t*>(bytes.data() + entry.offset), entry.size))
      ThrowResourceError("UpdateResource(RT_ICON)");
    GroupIconEntry group_entry{entry.width, entry.height, entry.color_count, entry.reserved,
                               entry.planes, entry.bit_count, entry.size, id};
    std::memcpy(group.data() + sizeof(header) + i * sizeof(group_entry), &group_entry,
                sizeof(group_entry));
  }
  if (!UpdateResourceW(update, MAKEINTRESOURCEW(14), MAKEINTRESOURCEW(1),
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), group.data(),
                       static_cast<DWORD>(group.size())))
    ThrowResourceError("UpdateResource(RT_GROUP_ICON)");
}

// 对一个干净的 PE 文件执行一次完整资源事务；失败后不能复用该事务句柄。
void ApplyPeResources(const std::filesystem::path& executable,
                      const PeMetadata& metadata,
                      const std::optional<PayloadBinding>& binding,
                      bool update_binding) {
  ResourceUpdate update(executable);
  if (!metadata.product_name.empty() || !metadata.company_name.empty() ||
      !metadata.file_description.empty() || !metadata.copyright.empty()) {
    auto version = BuildVersionResource(metadata);
    if (!UpdateResourceW(update.get(), MAKEINTRESOURCEW(16), MAKEINTRESOURCEW(1),
                         MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), version.data(),
                         static_cast<DWORD>(version.size())))
      ThrowResourceError("UpdateResource(RT_VERSION)");
  }
  UpdateIcon(update.get(), metadata.icon);
  if (update_binding) {
    if (binding) {
      auto bytes = EncodePayloadBinding(*binding);
      if (!UpdateResourceW(
              update.get(), kPayloadBindingResourceType,
              MAKEINTRESOURCEW(kPayloadBindingResourceId),
              MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), bytes.data(),
              static_cast<DWORD>(bytes.size())))
        ThrowResourceError("UpdateResource(LWWEB_BINDING)");
    } else if (!UpdateResourceW(
                   update.get(), kPayloadBindingResourceType,
                   MAKEINTRESOURCEW(kPayloadBindingResourceId),
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), nullptr, 0)) {
      ThrowResourceError("DeleteResource(LWWEB_BINDING)");
    }
  }
  update.Commit();
}

}  // namespace

void UpdatePeResources(const std::filesystem::path& executable,
                       const PeMetadata& metadata,
                       const std::optional<PayloadBinding>& binding) {
  const bool has_metadata =
      !metadata.product_name.empty() || !metadata.company_name.empty() ||
      !metadata.file_description.empty() || !metadata.copyright.empty() ||
      !metadata.icon.empty();
  const bool update_binding =
      binding.has_value() || ReadPePayloadBinding(executable).has_value();
  if (!has_metadata && !update_binding) return;
  auto backup = executable;
  backup += L".pe-resource-backup.tmp";
  std::error_code file_error;
  std::filesystem::remove(backup, file_error);
  file_error.clear();
  std::filesystem::copy_file(executable, backup,
                             std::filesystem::copy_options::overwrite_existing,
                             file_error);
  if (file_error)
    throw Error("Cannot create PE resource retry snapshot: " + file_error.message());

  // EndUpdateResource 偶尔会与 Defender、资源管理器缩略图或刚关闭的同名程序竞争。
  // 每次重试前恢复干净 Runner，避免在可能已被部分修改的 PE 上继续提交。
  constexpr std::array<int, 3> retry_delays_ms = {75, 200, 500};
  try {
    for (std::size_t attempt = 0; attempt <= retry_delays_ms.size(); ++attempt) {
      if (attempt != 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(retry_delays_ms[attempt - 1]));
        file_error.clear();
        std::filesystem::copy_file(backup, executable,
                                   std::filesystem::copy_options::overwrite_existing,
                                   file_error);
        if (file_error) {
          if (attempt < retry_delays_ms.size()) continue;
          throw Error("Cannot restore PE before retry: " + file_error.message());
        }
      }
      try {
        ApplyPeResources(executable, metadata, binding, update_binding);
        file_error.clear();
        std::filesystem::remove(backup, file_error);
        return;
      } catch (const ResourceUpdateError& error) {
        if (!IsTransientResourceError(error.code()) ||
            attempt == retry_delays_ms.size()) {
          throw Error(std::string(error.what()) +
                      ". Automatic retries were exhausted; close any running output EXE, "
                      "Explorer preview, or security scan and try again.");
        }
      }
    }
  } catch (...) {
    std::filesystem::remove(backup, file_error);
    throw;
  }
}

std::optional<PayloadBinding> ReadPePayloadBinding(
    const std::filesystem::path& executable) {
  const auto module = LoadLibraryExW(
      executable.c_str(), nullptr,
      LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!module)
    throw Error("Cannot open PE resources: " +
                WindowsErrorMessage(GetLastError()));
  struct ModuleGuard {
    HMODULE module;
    ~ModuleGuard() { FreeLibrary(module); }
  } guard{module};

  const auto resource = FindResourceW(
      module, MAKEINTRESOURCEW(kPayloadBindingResourceId),
      kPayloadBindingResourceType);
  if (!resource) {
    const auto code = GetLastError();
    if (code == ERROR_RESOURCE_DATA_NOT_FOUND ||
        code == ERROR_RESOURCE_NAME_NOT_FOUND ||
        code == ERROR_RESOURCE_TYPE_NOT_FOUND)
      return std::nullopt;
    throw Error("Cannot find LWWEB_BINDING resource: " +
                WindowsErrorMessage(code));
  }
  const auto size = SizeofResource(module, resource);
  if (size != kPayloadBindingSize)
    throw Error("LWWEB_BINDING resource has an invalid size");
  const auto loaded = LoadResource(module, resource);
  if (!loaded)
    throw Error("Cannot load LWWEB_BINDING resource: " +
                WindowsErrorMessage(GetLastError()));
  const auto* data = static_cast<const std::uint8_t*>(LockResource(loaded));
  if (!data) throw Error("Cannot lock LWWEB_BINDING resource");
  std::array<std::uint8_t, kPayloadBindingSize> bytes{};
  std::copy_n(data, bytes.size(), bytes.begin());
  return DecodePayloadBinding(bytes);
}

void VerifyPePayloadBinding(const std::filesystem::path& executable,
                            const Sha256Digest& footer_digest) {
  const auto binding = ReadPePayloadBinding(executable);
  if (!binding) return;
  if (binding->payload_sha256 != footer_digest)
    throw Error("Signed payload binding does not match the application payload");
  if ((binding->flags & kBindingAuthenticodeRequired) != 0)
    VerifyAuthenticodeSignature(executable);
}

}  // namespace lwweb
