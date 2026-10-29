#pragma once
// R8 fixed-purpose file staging. Resolve/Inspect helpers only read.
// Stage is called only by the explicit Install operation. Files are retained
// on removal; this module never deletes, overwrites or executes a payload.
#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <bcrypt.h>
#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>
#include "Package.h"

namespace r8files {
struct Error { std::wstring text; };
struct Context {
    std::wstring base, provider, manager, journals, sourceManager, sourceDll, sid, managerHash;
};
inline void Need(bool good, const std::wstring& message) { if (!good) throw Error{message}; }
inline void Win(bool good, const std::wstring& stage) {
    if (!good) throw Error{stage + L" (Win32 " + std::to_wstring(GetLastError()) + L")"};
}
inline bool Equal(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    File() = default; File(const File&) = delete; File& operator=(const File&) = delete;
    ~File() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
inline void CheckLocalForm(const std::wstring& path) {
    Need(path.size() >= 3 && path[1] == L':' && path[2] == L'\\' && path.find(L':', 2) == std::wstring::npos &&
         path.find(L"\\..") == std::wstring::npos && path.find(L"\\.\\") == std::wstring::npos,
         L"Only absolute local paths without alternate streams/traversal are permitted");
    Need(GetDriveTypeW(path.substr(0, 3).c_str()) == DRIVE_FIXED, L"Only a fixed local drive is permitted");
}
inline bool ValidatePath(const std::wstring& path, bool directory, bool allowMissing = false) {
    CheckLocalForm(path);
    for (std::size_t end = 3; end <= path.size(); ++end) {
        if (end != path.size() && path[end] != L'\\') continue;
        const std::wstring part = path.substr(0, end);
        const DWORD attributes = GetFileAttributesW(part.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            Need(allowMissing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND),
                 L"Missing/unreadable path: " + part + L" (Win32 " + std::to_wstring(error) + L")");
            return false;
        }
        Need(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), L"Reparse path refused: " + part);
        if (end != path.size()) Need((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, L"Parent is not a directory: " + part);
        else Need(((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory, L"Unexpected path type: " + part);
    }
    return true;
}
inline void FinalPath(HANDLE file, const std::wstring& expected) {
    wchar_t path[32768]{};
    const DWORD count = GetFinalPathNameByHandleW(file, path, ARRAYSIZE(path), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    Need(count > 4 && count < ARRAYSIZE(path), L"Cannot verify final opened path");
    const std::wstring actual(path, count);
    Need(actual.compare(0, 4, L"\\\\?\\") == 0 && Equal(actual.substr(4), expected), L"Opened path differs from expected local path");
    BY_HANDLE_FILE_INFORMATION info{};
    Win(GetFileInformationByHandle(file, &info) != FALSE, L"Read opened file attributes");
    Need(!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), L"Opened target is not an ordinary file");
}
inline void OpenRead(const std::wstring& path, File& file) {
    ValidatePath(path, false);
    file.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    Win(file.h != INVALID_HANDLE_VALUE, L"Open read-only file " + path);
    FinalPath(file.h, path);
}
inline std::wstring Hash(HANDLE file) {
    LARGE_INTEGER size{}, zero{};
    Win(GetFileSizeEx(file, &size) != FALSE, L"Read payload size");
    Need(size.QuadPart > 0 && size.QuadPart <= 8 * 1024 * 1024, L"Payload exceeds bounded 8 MiB hashing limit");
    Win(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE, L"Rewind payload hash input");
    struct Algorithm { BCRYPT_ALG_HANDLE h = nullptr; ~Algorithm() { if (h) BCryptCloseAlgorithmProvider(h, 0); } } algorithm;
    struct Digest { BCRYPT_HASH_HANDLE h = nullptr; ~Digest() { if (h) BCryptDestroyHash(h); } } digest;
    Need(BCryptOpenAlgorithmProvider(&algorithm.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
         BCryptCreateHash(algorithm.h, &digest.h, nullptr, 0, nullptr, 0, 0) >= 0, L"Cannot initialize payload SHA256");
    BYTE block[16384]{};
    LONGLONG total = 0;
    for (;;) {
        DWORD received = 0; Win(ReadFile(file, block, sizeof(block), &received, nullptr) != FALSE, L"Read payload hash input");
        if (!received) break;
        total += received;
        Need(total <= size.QuadPart, L"Payload changed during hashing");
        Need(BCryptHashData(digest.h, block, received, 0) >= 0, L"Payload SHA256 update failed");
    }
    Need(total == size.QuadPart, L"Payload size changed during hashing");
    BYTE bytes[32]{}; Need(BCryptFinishHash(digest.h, bytes, sizeof(bytes), 0) >= 0, L"Payload SHA256 finish failed");
    constexpr wchar_t alphabet[] = L"0123456789ABCDEF";
    std::wstring result;
    for (BYTE byte : bytes) { result += alphabet[byte >> 4]; result += alphabet[byte & 15]; }
    return result;
}
inline std::wstring HashFile(const std::wstring& path) { File file; OpenRead(path, file); return Hash(file.h); }
inline std::wstring Identity() {
    Need(sizeof(void*) == 8, L"Use the x64 manager");
    File token;
    Win(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.h) != FALSE, L"Query own user token");
    DWORD size = 0;
    GetTokenInformation(token.h, TokenUser, nullptr, 0, &size);
    Need(size > 0 && size <= 4096, L"Invalid own token user size");
    std::vector<BYTE> data(size);
    Win(GetTokenInformation(token.h, TokenUser, data.data(), size, &size) != FALSE, L"Read own user SID");
    LPWSTR raw = nullptr;
    Win(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &raw) != FALSE, L"Format own user SID");
    std::wstring sid;
    try { sid = raw; } catch (...) { LocalFree(raw); throw; }
    LocalFree(raw);

    TOKEN_ELEVATION elevation{};
    Win(GetTokenInformation(token.h, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE, L"Read own elevation");
    Need(!elevation.TokenIsElevated, L"Run normally, not as administrator");
    BYTE admin[SECURITY_MAX_SID_SIZE]{}; DWORD adminSize = sizeof(admin);
    Win(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin, &adminSize) != FALSE, L"Create administrator comparison SID");
    GetTokenInformation(token.h, TokenGroups, nullptr, 0, &size);
    Need(size > 0 && size <= 65536, L"Invalid own token groups size"); data.resize(size);
    Win(GetTokenInformation(token.h, TokenGroups, data.data(), size, &size) != FALSE, L"Read own token groups");
    const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(data.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i)
        Need(!(EqualSid(groups->Groups[i].Sid, admin) && (groups->Groups[i].Attributes & SE_GROUP_ENABLED)), L"Enabled administrator token refused");
    return sid;
}
inline Context Resolve() {
    Context context;
    context.sid = Identity();
    PWSTR local = nullptr;
    const HRESULT folder = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local);
    Need(SUCCEEDED(folder) && local, L"Cannot resolve the current user's LocalAppData");
    std::wstring localPath;
    try { localPath = local; } catch (...) { CoTaskMemFree(local); throw; }
    CoTaskMemFree(local);
    ValidatePath(localPath, true);
    context.base = localPath + L"\\ZBrushModelThumbnails\\" + r8package::kVersion;
    context.provider = context.base + L"\\" + r8package::kProviderFilename;
    context.manager = context.base + L"\\" + r8package::kManagerFilename;
    context.journals = context.base + L"\\journals";
    ValidatePath(context.base, true, true); ValidatePath(context.journals, true, true);
    wchar_t self[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, self, ARRAYSIZE(self));
    Need(length > 0 && length < ARRAYSIZE(self), L"Cannot resolve running manager path");
    context.sourceManager.assign(self, length);
    ValidatePath(context.sourceManager, false);
    const auto slash = context.sourceManager.find_last_of(L'\\');
    Need(slash != std::wstring::npos, L"Manager parent directory missing");
    context.sourceDll = context.sourceManager.substr(0, slash + 1) + r8package::kProviderFilename;
    context.managerHash = HashFile(context.sourceManager);
    return context;
}
inline void EnsureDirectory(const std::wstring& path) {
    if (ValidatePath(path, true, true)) return;
    const auto slash = path.find_last_of(L'\\');
    Need(slash != std::wstring::npos, L"Missing target directory parent");
    ValidatePath(path.substr(0, slash), true);
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        const DWORD error = GetLastError();
        Need(error == ERROR_ALREADY_EXISTS, L"Cannot create package directory " + path + L" (Win32 " + std::to_wstring(error) + L")");
    }
    ValidatePath(path, true);
}
inline void ImmutableCopy(const std::wstring& source, const std::wstring& target, const std::wstring& expectedHash) {
    File input; OpenRead(source, input);
    Need(Hash(input.h) == expectedHash, L"Source payload hash differs: " + source);
    if (ValidatePath(target, false, true)) {
        Need(HashFile(target) == expectedHash, L"Existing stable payload differs; it will not be overwritten: " + target);
        return;
    }
    const auto parent = target.substr(0, target.find_last_of(L'\\'));
    ValidatePath(parent, true);
    // Source handle denies writes/deletes until copy and destination verification
    // complete. CopyFile failIfExists prevents overwriting a concurrent creator.
    if (!CopyFileW(source.c_str(), target.c_str(), TRUE))
        throw Error{L"Copy without overwrite failed (Win32 " + std::to_wstring(GetLastError()) + L"). Any created target is preserved for inspection: " + target};
    Need(HashFile(target) == expectedHash, L"Copied stable payload hash differs; preserved for inspection: " + target);
}
inline void VerifyInstalledProvider(Context& context) {
    Need(HashFile(context.provider) == r8package::kProviderSha256, L"Stable provider is missing or differs from the frozen package");
}
inline void Stage(Context& context) {
    // Validate both sources before creating any directory. No provider is loaded.
    Need(HashFile(context.sourceDll) == r8package::kProviderSha256, L"Bundled provider does not match the frozen package");
    Need(HashFile(context.sourceManager) == context.managerHash, L"Manager changed since operation preflight");
    const auto product = context.base.substr(0, context.base.find_last_of(L'\\'));
    EnsureDirectory(product); EnsureDirectory(context.base); EnsureDirectory(context.journals);
    ImmutableCopy(context.sourceDll, context.provider, r8package::kProviderSha256);
    ImmutableCopy(context.sourceManager, context.manager, context.managerHash);
    VerifyInstalledProvider(context);
    Need(HashFile(context.manager) == context.managerHash, L"Stable manager verification failed");
}
} // namespace r8files
