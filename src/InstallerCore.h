#pragma once
// Reusable per-user coordinator. No startup filesystem/registry writes.
#include "InstallerFiles.h"
#include "Package.h"
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <utility>
#include <vector>
namespace r8reg {
struct Result { bool ok; std::wstring text; };
namespace detail {
inline constexpr REGSAM kRead = KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY;
inline constexpr REGSAM kWrite = kRead | KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE;
inline constexpr DWORD kBound = 65536;
struct Failure { std::wstring text; };
inline void Need(bool good, const std::wstring& text) { if (!good) throw Failure{text}; }
inline void Win(LSTATUS code, const std::wstring& stage) {
    if (code != ERROR_SUCCESS) throw Failure{stage + L" (error " + std::to_wstring(code) + L")"};
}
inline bool Equal(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~File() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
struct Key {
    HKEY h = nullptr;
    ~Key() { if (h) RegCloseKey(h); }
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};
inline void ExistingLocal(const std::wstring& path, bool directory) {
    Need(path.size() >= 3 && path[1] == L':' && path[2] == L'\\' && path.find(L':', 2) == std::wstring::npos,
         L"Only fixed absolute local paths without alternate streams are permitted");
    Need(GetDriveTypeW(path.substr(0, 3).c_str()) == DRIVE_FIXED, L"Only a local fixed drive is permitted");
    for (std::size_t end = 3; end <= path.size(); ++end) {
        if (end != path.size() && path[end] != L'\\') continue;
        const DWORD attributes = GetFileAttributesW(path.substr(0, end).c_str());
        Need(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), L"Missing or reparse path: " + path.substr(0, end));
        if (end == path.size()) Need(((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory, L"Unexpected path type: " + path);
    }
}
inline void FinalPath(HANDLE file, const std::wstring& expected) {
    wchar_t path[32768]{};
    const DWORD count = GetFinalPathNameByHandleW(file, path, ARRAYSIZE(path), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    Need(count > 4 && count < ARRAYSIZE(path), L"Cannot verify opened file path");
    const std::wstring actual(path, count);
    Need(actual.compare(0, 4, L"\\\\?\\") == 0 && Equal(actual.substr(4), expected), L"Opened file path differs from fixed target");
    BY_HANDLE_FILE_INFORMATION info{};
    Need(GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), L"Opened file is not an ordinary file");
}
inline bool OpenMaybe(HKEY parent, const std::wstring& name, REGSAM access, Key& key) {
    const LSTATUS status = RegOpenKeyExW(parent, name.c_str(), 0, access, &key.h);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return false;
    Win(status, L"Open registry key " + name); return true;
}
inline void Append(HANDLE file, const std::string& text) {
    LARGE_INTEGER end{}, size{};
    Need(GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart + static_cast<LONGLONG>(text.size()) <= kBound,
         L"Journal exceeds 64 KiB bound");
    Need(SetFilePointerEx(file, end, nullptr, FILE_END) != FALSE, L"Cannot seek journal");
    DWORD done = 0;
    while (done < text.size()) {
        DWORD written = 0;
        const DWORD remaining = static_cast<DWORD>(text.size() - done);
        Need(WriteFile(file, text.data() + done, remaining, &written, nullptr) && written && written <= remaining,
             L"Cannot append journal; ownership may be incomplete");
        done += written;
    }
    Need(FlushFileBuffers(file) != FALSE, L"Cannot durably flush journal");
}
inline void Notify() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr); }
template<class F> inline Result Guard(F action) noexcept {
    try { return action(); }
    catch (const Failure& error) { try { return {false, L"Incomplete: " + error.text}; } catch (...) { return {false, L"Error"}; } }
    catch (const r8files::Error& error) { try { return {false, L"Incomplete: " + error.text}; } catch (...) { return {false, L"Error"}; } }
    catch (const std::bad_alloc&) { return {false, L"OOM"}; }
    catch (...) { return {false, L"Error"}; }
}
inline constexpr wchar_t kThumb[] = L"{E357FCCD-A995-4576-B01F-234630154E96}";
inline constexpr wchar_t kClasses[] = L"Software\\Classes";
inline constexpr std::size_t kNodes = 10;
using Bits = std::array<bool, kNodes>;
struct Value { std::wstring name, data; };
struct Node { std::wstring path; bool target; std::vector<Value> values; };
inline std::array<Node, kNodes> Schema(const r8files::Context& ctx) {
    const std::wstring sfa = L"SystemFileAssociations", clsid = std::wstring(L"CLSID\\") + r8package::kProviderClsid;
    return {{
        {sfa, false, {}},
        {sfa + L"\\.ztl", false, {}},
        {sfa + L"\\.ztl\\shellex", false, {}},
        {sfa + L"\\.ztl\\shellex\\" + kThumb, true, {{L"", r8package::kProviderClsid}}},
        {sfa + L"\\.zpr", false, {}},
        {sfa + L"\\.zpr\\shellex", false, {}},
        {sfa + L"\\.zpr\\shellex\\" + kThumb, true, {{L"", r8package::kProviderClsid}}},
        {L"CLSID", false, {}},
        {clsid, true, {{L"", L"ZBrush model thumbnail provider"}}},
        {clsid + L"\\InprocServer32", false, {{L"", ctx.provider}, {L"ThreadingModel", L"Apartment"}}}
    }};
}
inline std::wstring Full(const Node& n) { return std::wstring(kClasses) + L"\\" + n.path; }
inline std::wstring ParentPath(const std::wstring& path) {
    const auto slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}
inline std::wstring Leaf(const std::wstring& path) {
    const auto slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
inline bool Exists(HKEY hive, const std::wstring& path) { Key key; return OpenMaybe(hive, path, kRead, key); }
struct Com {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Com() { Need(SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE, L"Cannot initialize association queries"); }
    ~Com() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
struct Association { HRESULT status = E_UNEXPECTED; std::wstring text; };
inline std::wstring HResultText(HRESULT status) {
    wchar_t text[16]{};
    swprintf_s(text, L"0x%08lX", static_cast<unsigned long>(status));
    return text;
}
inline Association Query(const wchar_t* ext, ASSOCSTR kind, const wchar_t* extra) {
    constexpr auto flags = static_cast<ASSOCF>(ASSOCF_NOFIXUPS | ASSOCF_NOTRUNCATE);
    DWORD chars = 0;
    const HRESULT sized = AssocQueryStringW(flags, kind, ext, extra, nullptr, &chars);
    if (sized != S_FALSE) return {sized, L""};
    Need(chars > 0 && chars <= 2048, L"Association string exceeds 2048-character bound");
    std::vector<wchar_t> buffer(chars, L'\0');
    DWORD capacity = chars;
    const HRESULT got = AssocQueryStringW(flags, kind, ext, extra, buffer.data(), &capacity);
    if (got != S_OK) return {got, L""};
    Need(capacity <= chars && std::find(buffer.begin(), buffer.end(), L'\0') != buffer.end(), L"Invalid association query result");
    const std::wstring text(buffer.data());
    Need(!text.empty(), L"Empty successful association query is ambiguous");
    return {got, text};
}
inline bool Missing(const Association& value) { return value.status == HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION); }
inline void Usable(const Association& value) {
    Need((value.status == S_OK && !value.text.empty()) || (Missing(value) && value.text.empty()),
         L"Association query failed or returned an inconsistent status/string; absence was not established (HRESULT " +
         HResultText(value.status) + L")");
}
using Bindings = std::array<Association, 6>;
inline Bindings CaptureBindings() {
    Bindings result{};
    constexpr const wchar_t* extensions[] = {L".ztl", L".zpr"};
    for (std::size_t i = 0; i < 2; ++i) {
        result[i * 3] = Query(extensions[i], ASSOCSTR_PROGID, nullptr);
        result[i * 3 + 1] = Query(extensions[i], ASSOCSTR_EXECUTABLE, L"open");
        result[i * 3 + 2] = Query(extensions[i], ASSOCSTR_COMMAND, L"open");
    }
    return result;
}
inline bool SameBindings(const Bindings& a, const Bindings& b) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].status != b[i].status || a[i].text != b[i].text) return false;
    return true;
}
inline void NoDirectHandler(const std::wstring& association) {
    Need(!association.empty() && association.find_first_of(L"\\/\r\n") == std::wstring::npos,
         L"Unexpected resolved ProgID; refusing ambiguous registry lookup");
    Need(!Exists(HKEY_CLASSES_ROOT, association + L"\\shellex\\" + kThumb),
         L"An existing direct/ProgID thumbnail-handler key must be preserved: " + association);
}
inline void CheckNoHandlers(const Bindings& bindings) {
    constexpr const wchar_t* extensions[] = {L".ztl", L".zpr"};
    for (std::size_t i = 0; i < 2; ++i) {
        NoDirectHandler(extensions[i]);
        if (bindings[i * 3].status == S_OK) NoDirectHandler(bindings[i * 3].text);
        const auto handler = Query(extensions[i], ASSOCSTR_SHELLEXTENSION, kThumb);
        Usable(handler);
        Need(Missing(handler), L"An effective thumbnail handler already exists: " + std::wstring(extensions[i]) + L" " + handler.text);
    }
    // Generic fallback collisions are also refused. These are read-only lookups.
    NoDirectHandler(L"*");
    NoDirectHandler(L"AllFilesystemObjects");
}
inline Bits PriorNodes(const std::array<Node, kNodes>& nodes) {
    Bits prior{};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        prior[i] = Exists(HKEY_CURRENT_USER, Full(nodes[i]));
        if (nodes[i].target) {
            Need(!prior[i], L"Target already exists in HKCU64: " + nodes[i].path);
            Need(!Exists(HKEY_CLASSES_ROOT, nodes[i].path), L"Target already exists in merged HKCR64: " + nodes[i].path);
        }
    }
    return prior;
}
inline bool ExactValue(HKEY key, const Value& value) {
    DWORD type = 0, bytes = 0;
    const LSTATUS status = RegQueryValueExW(key, value.name.c_str(), nullptr, &type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return false;
    Win(status, L"Query exact planned value");
    const DWORD expected = static_cast<DWORD>((value.data.size() + 1) * sizeof(wchar_t));
    Need(type == REG_SZ && bytes == expected && bytes <= kBound, L"Unexpected registry value type/length");
    std::vector<BYTE> data(bytes);
    Win(RegQueryValueExW(key, value.name.c_str(), nullptr, &type, data.data(), &bytes), L"Read exact planned value");
    Need(type == REG_SZ && bytes == expected && std::memcmp(data.data(), value.data.c_str(), bytes) == 0,
         L"Registry value changed; preserving it");
    return true;
}
inline void ValidateNode(HKEY key, const std::array<Node, kNodes>& nodes, const Bits& owned, std::size_t index, bool requireAll) {
    const auto& node = nodes[index];
    for (DWORD i = 0;; ++i) {
        Need(i < 32, L"Unexpected registry value count");
        wchar_t name[256]{}; DWORD chars = ARRAYSIZE(name);
        const LSTATUS status = RegEnumValueW(key, i, name, &chars, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        Win(status, L"Enumerate owned values");
        const std::wstring actual(name, chars);
        const auto found = std::find_if(node.values.begin(), node.values.end(), [&](const Value& v) { return Equal(v.name, actual); });
        Need(found != node.values.end(), L"Unexpected value in owned node; preserving node: " + node.path);
        Need(ExactValue(key, *found), L"Owned value disappeared during validation");
    }
    for (DWORD i = 0;; ++i) {
        Need(i < 32, L"Unexpected registry child count");
        wchar_t name[256]{}; DWORD chars = ARRAYSIZE(name);
        const LSTATUS status = RegEnumKeyExW(key, i, name, &chars, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        Win(status, L"Enumerate owned child keys");
        const std::wstring child = node.path + L"\\" + std::wstring(name, chars);
        bool allowed = false;
        for (std::size_t j = 0; j < nodes.size(); ++j) if (owned[j] && Equal(nodes[j].path, child)) allowed = true;
        Need(allowed, L"Unexpected or unowned child; preserving owned parent: " + child);
    }
    if (requireAll) for (const auto& value : node.values) Need(ExactValue(key, value), L"Required registration value missing");
}
inline void ValidateOwned(const std::array<Node, kNodes>& nodes, const Bits& owned, bool requireAll) {
    for (std::size_t i = 0; i < nodes.size(); ++i) if (owned[i]) {
        Key key;
        if (OpenMaybe(HKEY_CURRENT_USER, Full(nodes[i]), kRead, key)) ValidateNode(key.h, nodes, owned, i, requireAll);
        else Need(!requireAll, L"Owned registry node disappeared: " + nodes[i].path);
    }
}
inline bool Empty(HKEY key) {
    DWORD children = 0, values = 0;
    Win(RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &children, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr), L"Check owned key emptiness");
    return children == 0 && values == 0;
}
inline void RemoveOwned(const std::array<Node, kNodes>& nodes, const Bits& owned) {
    ValidateOwned(nodes, owned, false); // preflight every owned node before any deletion
    for (std::size_t reverse = nodes.size(); reverse; --reverse) {
        const std::size_t i = reverse - 1;
        if (!owned[i]) continue;
        const auto& node = nodes[i];
        Key key;
        if (!OpenMaybe(HKEY_CURRENT_USER, Full(node), kRead | KEY_SET_VALUE, key)) continue;
        ValidateNode(key.h, nodes, owned, i, false);
        for (const auto& value : node.values) if (ExactValue(key.h, value))
            Win(RegDeleteValueW(key.h, value.name.c_str()), L"Remove exact owned value");
        Need(Empty(key.h), L"Owned key gained content; stopped before deletion: " + node.path);
        // Fixed individually owned nodes only. Never deletes a preexisting ancestor.
        Win(RegDeleteKeyExW(HKEY_CURRENT_USER, Full(node).c_str(), KEY_WOW64_64KEY, 0), L"Remove empty owned node " + node.path);
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) if (owned[i])
        Need(!Exists(HKEY_CURRENT_USER, Full(nodes[i])), L"Owned node remains/reappeared; automatic retry refused");
}
inline std::string Ascii(const std::wstring& value) {
    std::string out;
    for (wchar_t c : value) { Need(c > 0 && c < 128, L"Non-ASCII fixed journal field"); out += static_cast<char>(c); }
    return out;
}
inline std::string Hex(unsigned long value, unsigned digits) {
    constexpr char alphabet[] = "0123456789ABCDEF";
    std::string out(digits, '0');
    for (unsigned i = 0; i < digits; ++i) { out[digits - i - 1] = alphabet[value & 15]; value >>= 4; }
    return out;
}
inline unsigned long Unhex(const std::string& text) {
    Need(!text.empty() && text.size() <= 8, L"Invalid hexadecimal journal field");
    unsigned long value = 0;
    for (char c : text) {
        Need((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'), L"Noncanonical journal hexadecimal field");
        value = (value << 4) | static_cast<unsigned long>(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    return value;
}
inline std::string WideHex(const std::wstring& value) {
    Need(value.size() <= 2048 && value.find(L'\0') == std::wstring::npos, L"Journal field exceeds UTF16 bound");
    std::string text;
    for (wchar_t c : value) text += Hex(static_cast<unsigned long>(c), 4);
    return text;
}
struct ActionLock {
    File mutex;
    bool owned = false;
    explicit ActionLock(const r8files::Context& ctx) {
        const auto name = L"Global\\ZBrushModelThumbnails.Installer." + ctx.sid;
        mutex.h = CreateMutexW(nullptr, FALSE, name.c_str());
        Need(mutex.h != nullptr, L"Cannot acquire the per-user installer mutex");
        const DWORD status = WaitForSingleObject(mutex.h, 0);
        owned = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
        Need(owned, L"Another installer action is running. Try again after it completes");
    }
    ~ActionLock() { if (owned) ReleaseMutex(mutex.h); }
    ActionLock(const ActionLock&) = delete;
    ActionLock& operator=(const ActionLock&) = delete;
};
inline bool DirectoryPresent(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        Need(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, L"Cannot inspect journal directory");
        return false;
    }
    ExistingLocal(path, true); return true;
}
inline std::wstring GuidName(REFGUID id) {
    wchar_t text[40]{};
    Need(StringFromGUID2(id, text, ARRAYSIZE(text)) == 39, L"Cannot format cycle identifier");
    return L"cycle-" + std::wstring(text) + L".journal";
}
inline void ValidateJournalPath(const r8files::Context& ctx, const std::wstring& path) {
    Need(ParentPath(path) == ctx.journals, L"Journal is outside the stable journal directory");
    const auto name = Leaf(path);
    Need(name.size() == 52 && name.substr(0, 6) == L"cycle-" && name.substr(44) == L".journal", L"Unexpected journal filename");
    const auto guid = name.substr(6, 38);
    for (std::size_t i = 0; i < guid.size(); ++i) {
        const bool fixed = i == 0 || i == 37 || i == 9 || i == 14 || i == 19 || i == 24;
        if (fixed) Need(guid[i] == (i == 0 ? L'{' : i == 37 ? L'}' : L'-'), L"Invalid journal identifier punctuation");
        else Need((guid[i] >= L'0' && guid[i] <= L'9') || (guid[i] >= L'A' && guid[i] <= L'F'), L"Noncanonical journal identifier");
    }
    // Do not pass an untrusted filename to CLSIDFromString: non-GUID strings can resolve ProgIDs.
}
inline void OpenJournal(const r8files::Context& ctx, const std::wstring& path, bool write, File& file) {
    ValidateJournalPath(ctx, path); ExistingLocal(path, false);
    file.h = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0), write ? 0 : FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Need(file.h != INVALID_HANDLE_VALUE, L"Cannot open cycle journal with requested exclusive/read-only access");
    FinalPath(file.h, path);
}
inline std::string Header(const r8files::Context& ctx, const std::wstring& path) {
    ValidateJournalPath(ctx, path);
    std::string text = "ZBRUSH_MODEL_THUMBNAILS_JOURNAL_V1\r\nSID=" + Ascii(ctx.sid) +
        "\r\nVERSION_UTF16=" + WideHex(r8package::kVersion) + "\r\nPROVIDER_UTF16=" + WideHex(ctx.provider) +
        "\r\nPROVIDER_SHA256=" + Ascii(r8package::kProviderSha256) + "\r\nMANAGER_UTF16=" + WideHex(ctx.manager) +
        "\r\nMANAGER_SHA256=" + Ascii(ctx.managerHash) + "\r\nJOURNAL_UTF16=" + WideHex(path) +
        "\r\nTARGETS=ABSENT_HKCU64_AND_HKCR64\r\n";
    const auto nodes = Schema(ctx);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        text += "NODE " + std::to_string(i) + "=HKCU64\\" + Ascii(Full(nodes[i])) + "\r\n";
        for (const auto& v : nodes[i].values)
            text += "VALUE " + std::to_string(i) + "|" + (v.name.empty() ? std::string("(Default)") : Ascii(v.name)) + "|REG_SZ_UTF16|" + WideHex(v.data) + "\r\n";
    }
    return text + "BINDINGS=NOFIXUPS_NOTRUNCATE;ZTL_PROGID,OPEN_EXE,OPEN_COMMAND;ZPR_PROGID,OPEN_EXE,OPEN_COMMAND;UTF16_HEX\r\n";
}
inline std::string BaseJournal(const r8files::Context& ctx, const std::wstring& path, const Bits& prior, const Bindings& bindings) {
    std::string text = Header(ctx, path) + "PRIOR_NODES=";
    for (bool b : prior) text += b ? '1' : '0';
    text += "\r\n";
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        text += "BIND " + std::to_string(i) + "|" + Hex(static_cast<unsigned long>(bindings[i].status), 8) + "|";
        for (wchar_t c : bindings[i].text) text += Hex(static_cast<unsigned long>(c), 4);
        text += "\r\n";
    }
    return text + "END_BASE\r\n";
}
struct Journal { std::string raw; Bits prior{}, owned{}; Bindings bindings{}; bool started = false, complete = false, applied = false; };
inline Journal ReadJournal(HANDLE file, const r8files::Context& ctx, const std::wstring& path) {
    LARGE_INTEGER size{}, zero{};
    Need(GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= kBound, L"Invalid bounded journal size");
    Need(SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE, L"Cannot rewind journal");
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD done = 0;
    while (done < text.size()) {
        DWORD got = 0;
        Need(ReadFile(file, text.data() + done, static_cast<DWORD>(text.size() - done), &got, nullptr) && got, L"Cannot read journal");
        done += got;
    }
    const auto header = Header(ctx, path);
    Need(text.compare(0, header.size(), header) == 0, L"Journal fixed SID/path/hash/schema mismatch");
    std::size_t position = header.size();
    const auto line = [&]() -> std::string {
        const auto end = text.find("\r\n", position);
        Need(end != std::string::npos, L"Truncated journal record; restore refused");
        const auto value = text.substr(position, end - position); position = end + 2; return value;
    };
    Journal result; result.raw = text;
    const auto prior = line();
    // The exact prefix is 12 characters including '='.
    Need(prior.size() == 12 + kNodes && prior.substr(0, 12) == "PRIOR_NODES=", L"Invalid prior-node length/prefix");
    for (std::size_t i = 0; i < kNodes; ++i) { Need(prior[12 + i] == '0' || prior[12 + i] == '1', L"Invalid prior-node bit"); result.prior[i] = prior[12 + i] == '1'; }
    const auto nodes = Schema(ctx);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        Need(!nodes[i].target || !result.prior[i], L"Journal target was not absent");
        const auto parent = ParentPath(nodes[i].path);
        for (std::size_t j = 0; j < i; ++j) if (nodes[j].path == parent)
            Need(!result.prior[i] || result.prior[j], L"Inconsistent prior parent state");
    }
    for (std::size_t i = 0; i < result.bindings.size(); ++i) {
        const auto item = line(), prefix = "BIND " + std::to_string(i) + "|";
        Need(item.compare(0, prefix.size(), prefix) == 0 && item.size() >= prefix.size() + 9 && item[prefix.size() + 8] == '|', L"Invalid binding record");
        auto& binding = result.bindings[i];
        binding.status = static_cast<HRESULT>(Unhex(item.substr(prefix.size(), 8)));
        const auto encoded = item.substr(prefix.size() + 9);
        Need(encoded.size() % 4 == 0 && encoded.size() < 2048 * 4, L"Binding journal string exceeds bound");
        for (std::size_t p = 0; p < encoded.size(); p += 4) {
            const auto c = Unhex(encoded.substr(p, 4)); Need(c != 0, L"Embedded NUL in binding snapshot"); binding.text += static_cast<wchar_t>(c);
        }
        Usable(binding);
        Need((binding.status == S_OK) == !binding.text.empty(), L"Inconsistent binding status/string");
    }
    Need(line() == "END_BASE", L"Missing end-of-base marker");
    Need(text.substr(0, position) == BaseJournal(ctx, path, result.prior, result.bindings), L"Noncanonical base journal");
    while (position < text.size()) {
        const auto record = line();
        if (record.size() == 10 && record.compare(0, 9, "OWN_NODE ") == 0 && record[9] >= '0' && record[9] <= '9') {
            const auto i = static_cast<std::size_t>(record[9] - '0');
            Need(!result.started && !result.applied && !result.prior[i] && !result.owned[i], L"Invalid ownership record");
            const auto parent = ParentPath(nodes[i].path);
            for (std::size_t j = 0; j < i; ++j) if (nodes[j].path == parent)
                Need(result.prior[j] || result.owned[j], L"Ownership appears before parent ownership");
            result.owned[i] = true;
        } else if (record == "APPLY_COMPLETE") {
            Need(!result.started && !result.applied, L"Invalid apply marker");
            for (std::size_t i = 0; i < kNodes; ++i) Need(result.prior[i] || result.owned[i], L"Incomplete ownership before apply marker");
            result.applied = true;
        } else if (record == "RESTORE_STARTED") { Need(!result.started, L"Duplicate restore marker"); result.started = true; }
        else if (record == "RESTORE_COMPLETE") { Need(result.started && !result.complete, L"Invalid restore completion"); result.complete = true; }
        else throw Failure{L"Unknown journal record; automatic restore refused"};
    }
    return result;
}
inline std::wstring Residue(const std::array<Node, kNodes>& nodes, const Bits& owned) {
    std::wstring text;
    for (std::size_t i = 0; i < nodes.size(); ++i) if (owned[i]) text += L"\r\nHKCU64\\" + Full(nodes[i]);
    return text;
}
inline std::wstring DescribeBindings(const Bindings& bindings) {
    std::wstring text;
    constexpr const wchar_t* names[] = {L".ztl ProgID", L".ztl open executable", L".ztl open command", L".zpr ProgID", L".zpr open executable", L".zpr open command"};
    for (std::size_t i = 0; i < bindings.size(); ++i)
        text += std::wstring(names[i]) + L": HRESULT " + HResultText(bindings[i].status) + L" " + bindings[i].text + L"\r\n";
    return text;
}
struct Cycle { std::wstring path; Journal journal; };
struct State { std::size_t count = 0, completed = 0; bool active = false; Cycle current; };
inline State Scan(const r8files::Context& ctx) {
    State state;
    if (!DirectoryPresent(ctx.journals)) return state;
    WIN32_FIND_DATAW entry{};
    struct Find { HANDLE h = INVALID_HANDLE_VALUE; ~Find() { if (h != INVALID_HANDLE_VALUE) FindClose(h); } } search;
    search.h = FindFirstFileW((ctx.journals + L"\\*").c_str(), &entry);
    if (search.h == INVALID_HANDLE_VALUE) {
        Need(GetLastError() == ERROR_FILE_NOT_FOUND, L"Cannot enumerate cycle journals"); return state;
    }
    for (;;) {
        const std::wstring name(entry.cFileName);
        if (name != L"." && name != L"..") {
            Need(++state.count <= 128, L"Journal history exceeds 128 entries; no automatic pruning or mutation");
            Need(!(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)), L"Unexpected directory/reparse entry in journal history");
            const auto path = ctx.journals + L"\\" + name;
            File file; OpenJournal(ctx, path, false, file);
            auto journal = ReadJournal(file.h, ctx, path);
            if (journal.complete) ++state.completed;
            else {
                Need(journal.applied && !journal.started, L"An incomplete cycle blocks automatic actions: " + path);
                Need(!state.active, L"Multiple active cycles block automatic actions");
                state.active = true;
                state.current = {path, std::move(journal)};
            }
        }
        if (!FindNextFileW(search.h, &entry)) {
            Need(GetLastError() == ERROR_NO_MORE_FILES, L"Journal enumeration failed"); break;
        }
    }
    return state;
}
inline Bindings UsableBindings() {
    const auto bindings = CaptureBindings();
    for (const auto& binding : bindings) Usable(binding);
    return bindings;
}
inline void CheckExpectedHandlers() {
    for (const wchar_t* ext : {L".ztl", L".zpr"}) {
        const auto current = Query(ext, ASSOCSTR_SHELLEXTENSION, kThumb);
        Need(current.status == S_OK && Equal(current.text, r8package::kProviderClsid),
             L"Effective thumbnail handler differs for " + std::wstring(ext) + L" (" + HResultText(current.status) + L")");
    }
}
inline bool VerifyActive(const r8files::Context& ctx, const State& state) {
    Need(state.active && state.current.journal.applied && !state.current.journal.started, L"No complete active installation cycle");
    ValidateOwned(Schema(ctx), state.current.journal.owned, true);
    CheckExpectedHandlers();
    Need(r8files::HashFile(ctx.manager) == ctx.managerHash, L"Stable manager is missing or differs from this frozen cycle");
    return SameBindings(state.current.journal.bindings, UsableBindings());
}
inline void Refresh(r8files::Context& ctx) {
    auto fresh = r8files::Resolve();
    Need(fresh.sid == ctx.sid && fresh.base == ctx.base && fresh.provider == ctx.provider && fresh.journals == ctx.journals &&
         fresh.manager == ctx.manager && fresh.managerHash == ctx.managerHash, L"Identity or package context changed while acquiring action lock");
    ctx = std::move(fresh);
}
inline void CreateNodes(const std::array<Node, kNodes>& nodes, const Bits& prior, Bits& created, HANDLE journal) {
    Key classes;
    Win(RegOpenKeyExW(HKEY_CURRENT_USER, kClasses, 0, kRead | KEY_CREATE_SUB_KEY, &classes.h), L"Open existing HKCU Classes");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (prior[i]) { Need(Exists(HKEY_CURRENT_USER, Full(node)), L"Preexisting ancestor disappeared"); continue; }
        if (node.target) Need(!Exists(HKEY_CLASSES_ROOT, node.path), L"Merged target appeared; preserving it");
        Key parent, key;
        Win(RegOpenKeyExW(classes.h, ParentPath(node.path).c_str(), 0, kRead | KEY_CREATE_SUB_KEY, &parent.h), L"Open existing immediate parent");
        DWORD disposition = 0;
        const LSTATUS status = RegCreateKeyExW(parent.h, Leaf(node.path).c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, kWrite, nullptr, &key.h, &disposition);
        if (status == ERROR_SUCCESS && disposition == REG_CREATED_NEW_KEY) created[i] = true;
        Win(status, L"Create individual planned node");
        Need(disposition == REG_CREATED_NEW_KEY, L"Node appeared concurrently; it is unowned and preserved");
        Append(journal, "OWN_NODE " + std::to_string(i) + "\r\n");
        ValidateNode(key.h, nodes, created, i, false);
        for (const auto& value : node.values) {
            Need(!ExactValue(key.h, value), L"Value appeared concurrently; refusing replacement");
            Win(RegSetValueExW(key.h, value.name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.data.c_str()),
                static_cast<DWORD>((value.data.size() + 1) * sizeof(wchar_t))), L"Write new thumbnail registration value");
        }
    }
}
} // namespace detail

inline Result Inspect() noexcept {
    return detail::Guard([]() -> Result {
        auto ctx = r8files::Resolve(); detail::ActionLock lock(ctx); detail::Refresh(ctx); detail::Com com;
        const auto state = detail::Scan(ctx);
        if (state.active) {
            const bool bindingsUnchanged = detail::VerifyActive(ctx, state);
            try { r8files::VerifyInstalledProvider(ctx); }
            catch (const r8files::Error& error) {
                return {false, L"Incomplete: active registration has a missing/damaged provider. Remove remains available with the valid journal.\r\n" + error.text};
            }
            return {true, L"Installed: exact owned registration and provider hash verified.\r\nActive cycle: " + state.current.path +
                L"\r\nCompleted retained cycles: " + std::to_wstring(state.completed) +
                (bindingsUnchanged ? L"\r\nOpen bindings match the cycle snapshot.\r\n" : L"\r\nOpen bindings changed externally; current settings are preserved.\r\n") +
                detail::DescribeBindings(detail::CaptureBindings())};
        }
        detail::PriorNodes(detail::Schema(ctx)); // refuse unowned same-looking registration
        return {true, L"Removed: no active cycle and all package target keys are absent.\r\nCompleted retained cycles: " +
            std::to_wstring(state.completed) + L"\r\nInstall will recheck effective-handler conflicts. Package files and history are retained."};
    });
}
inline Result Install() noexcept {
    return detail::Guard([]() -> Result {
        auto ctx = r8files::Resolve(); detail::ActionLock lock(ctx); detail::Refresh(ctx); detail::Com com;
        const auto state = detail::Scan(ctx);
        if (state.active) {
            const bool bindingsUnchanged = detail::VerifyActive(ctx, state); r8files::VerifyInstalledProvider(ctx);
            return {true, L"Installed: this exact active cycle is already installed. No files or registry values changed.\r\n" + state.current.path +
                (bindingsUnchanged ? L"\r\nAll six open bindings match the cycle snapshot." : L"\r\nOpen bindings changed externally; current settings are preserved.")};
        }
        detail::Need(state.count < 128, L"Journal history is full (128 entries); retained history will not be pruned");
        const auto nodes = detail::Schema(ctx);
        const auto prior = detail::PriorNodes(nodes);
        const auto bindings = detail::UsableBindings();
        detail::CheckNoHandlers(bindings);
        { detail::Key key; detail::Win(RegOpenKeyExW(HKEY_CURRENT_USER, detail::kClasses, 0, detail::kRead | KEY_CREATE_SUB_KEY, &key.h), L"Preflight existing HKCU Classes access"); }
        // All registry/association conflict checks precede any package staging.
        r8files::Stage(ctx);
        r8files::File provider;
        r8files::OpenRead(ctx.provider, provider);
        detail::Need(r8files::Hash(provider.h) == r8package::kProviderSha256, L"Stable provider changed after staging");
        const auto afterStage = detail::Scan(ctx);
        detail::Need(!afterStage.active && afterStage.count == state.count && afterStage.completed == state.completed, L"Journal history changed while staging");
        detail::Need(detail::PriorNodes(nodes) == prior && detail::SameBindings(bindings, detail::UsableBindings()), L"Registry or open bindings changed while staging");
        detail::CheckNoHandlers(bindings);
        GUID id{}; detail::Need(CoCreateGuid(&id) == S_OK, L"Cannot create unique cycle identifier");
        const auto path = ctx.journals + L"\\" + detail::GuidName(id);
        const auto base = detail::BaseJournal(ctx, path, prior, bindings);
        detail::Need(base.size() + 1024 <= detail::kBound, L"Cycle snapshot exceeds bounded journal capacity");
        detail::ExistingLocal(ctx.journals, true);
        detail::File journal;
        journal.h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        detail::Need(journal.h != INVALID_HANDLE_VALUE, L"Cannot exclusively create a new cycle journal; no registry change");
        detail::FinalPath(journal.h, path);
        detail::Append(journal.h, base); // flush prior-state snapshot before registry creation
        detail::Bits created{};
        try {
            detail::Need(detail::PriorNodes(nodes) == prior && detail::SameBindings(bindings, detail::UsableBindings()), L"Registry or bindings changed before creation");
            detail::CheckNoHandlers(bindings);
            detail::CreateNodes(nodes, prior, created, journal.h);
            detail::ValidateOwned(nodes, created, true);
            detail::Notify();
            detail::Need(detail::SameBindings(bindings, detail::UsableBindings()), L"Effective open bindings changed during installation");
            detail::CheckExpectedHandlers();
            detail::Append(journal.h, "APPLY_COMPLETE\r\n");
            detail::Need(detail::ReadJournal(journal.h, ctx, path).applied, L"New active journal failed readback");
        } catch (...) {
            std::wstring reason = L"Installation failed";
            try { throw; } catch (const detail::Failure& error) { reason = error.text; }
            catch (const r8files::Error& error) { reason = error.text; } catch (...) {}
            try {
                detail::Append(journal.h, "RESTORE_STARTED\r\n");
                detail::RemoveOwned(nodes, created); detail::Notify();
                detail::Need(detail::SameBindings(bindings, detail::UsableBindings()), L"Open bindings differ after rollback");
                detail::Append(journal.h, "RESTORE_COMPLETE\r\n");
                detail::Need(detail::ReadJournal(journal.h, ctx, path).complete, L"Rollback journal failed readback");
                return {false, L"Removed: installation failed and this attempt's owned nodes were rolled back.\r\n" + reason + L"\r\nCycle retained: " + path};
            } catch (...) {
                return {false, L"Incomplete: installation/rollback could not be confirmed. Automatic retry is blocked.\r\n" + reason +
                    L"\r\nInspect cycle: " + path + L"\r\nPossible owned residue:" + detail::Residue(nodes, created)};
            }
        }
        return {true, L"Installed: exact .ztl/.zpr thumbnail registration and effective handlers verified. All six open-binding results are unchanged.\r\nCycle: " + path +
            L"\r\nExplorer rendering/save refresh requires separate observation."};
    });
}
inline Result Remove() noexcept {
    return detail::Guard([]() -> Result {
        auto ctx = r8files::Resolve(); detail::ActionLock lock(ctx); detail::Refresh(ctx); detail::Com com;
        const auto state = detail::Scan(ctx);
        const auto nodes = detail::Schema(ctx);
        if (!state.active) {
            detail::PriorNodes(nodes);
            return {true, L"Removed: no active cycle and no package target registration exists. No changes made; history retained."};
        }
        // Removal intentionally does not open, hash, load, replace or execute the provider DLL.
        detail::File journal; detail::OpenJournal(ctx, state.current.path, true, journal);
        const auto current = detail::ReadJournal(journal.h, ctx, state.current.path);
        detail::Need(current.raw == state.current.journal.raw && current.applied && !current.started && !current.complete,
                     L"Active cycle changed after scan; no removal attempted");
        detail::ValidateOwned(nodes, current.owned, false);
        const auto before = detail::UsableBindings();
        detail::Append(journal.h, "RESTORE_STARTED\r\n");
        try {
            detail::RemoveOwned(nodes, current.owned); detail::Notify();
            detail::Need(detail::SameBindings(before, detail::UsableBindings()), L"Effective open bindings changed during removal");
            detail::Append(journal.h, "RESTORE_COMPLETE\r\n");
            detail::Need(detail::ReadJournal(journal.h, ctx, state.current.path).complete, L"Completed removal journal failed readback");
        } catch (...) {
            throw detail::Failure{L"Removal stopped after its durable start marker. Automatic retry is blocked; inspect cycle " + state.current.path +
                L"\r\nSome owned nodes may already be removed:" + detail::Residue(nodes, current.owned)};
        }
        return {true, L"Removed: only exact journal-owned registration and empty owned ancestors were removed. All six current open-binding results are unchanged.\r\nPackage files and completed cycle remain: " + state.current.path +
            L"\r\nA later Install creates a new cycle; previous completed journals are never modified."};
    });
}
} // namespace r8reg
