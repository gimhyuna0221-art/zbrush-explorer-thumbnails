// Registration-free console test host. No source writes, UI, or cache operations.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <propsys.h>
#include <thumbcache.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "../src/Decoder.h"
#include "../src/ProviderId.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> class ComPtr {
    T* value_ = nullptr;
public:
    ~ComPtr() { if (value_) value_->Release(); }
    T* Get() const { return value_; }
    T** Put() { Check(value_ == nullptr, "ComPtr already contains a value"); return &value_; }
    T* operator->() const { return value_; }
    void Attach(T* value) { Check(value_ == nullptr, "ComPtr attach conflict"); value_ = value; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};
struct OwnedBitmap {
    HBITMAP value = nullptr;
    ~OwnedBitmap() { if (value) DeleteObject(value); }
};
struct OwnedMemoryDC {
    HDC value = CreateCompatibleDC(nullptr);
    ~OwnedMemoryDC() { if (value) DeleteDC(value); }
};
struct OwnedModule {
    HMODULE value = nullptr;
    ~OwnedModule() { if (value) FreeLibrary(value); }
};
struct OwnedFile {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~OwnedFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

// Intentionally supplies no file name. Short reads exercise the DLL's read loop.
class TestStream final : public IStream {
    volatile LONG refs_ = 1;
    std::vector<BYTE> bytes_;
    ULONGLONG position_ = 0;
public:
    ULONGLONG bytesRead = 0;
    ULONG writeCalls = 0;
    explicit TestStream(std::vector<BYTE> bytes) : bytes_(std::move(bytes)) {}
    IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ISequentialStream) || IsEqualIID(iid, IID_IStream))
            *object = static_cast<IStream*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG remaining = InterlockedDecrement(&refs_);
        if (!remaining) delete this;
        return static_cast<ULONG>(remaining);
    }
    IFACEMETHODIMP Read(void* buffer, ULONG requested, ULONG* read) override {
        if (read) *read = 0;
        if (!buffer && requested) return STG_E_INVALIDPOINTER;
        const auto available = position_ < bytes_.size() ? bytes_.size() - static_cast<std::size_t>(position_) : 0;
        const auto count = (std::min)({static_cast<std::size_t>(requested), available, std::size_t{7}});
        if (count) std::memcpy(buffer, bytes_.data() + static_cast<std::size_t>(position_), count);
        position_ += count;
        bytesRead += count;
        if (read) *read = static_cast<ULONG>(count);
        return count == requested ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Write(const void*, ULONG, ULONG* written) override {
        ++writeCalls;
        if (written) *written = 0;
        return STG_E_ACCESSDENIED;
    }
    IFACEMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        ULONGLONG base = 0;
        if (origin == STREAM_SEEK_CUR) base = position_;
        else if (origin == STREAM_SEEK_END) base = bytes_.size();
        else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
        if (move.QuadPart < 0) {
            const auto distance = static_cast<ULONGLONG>(-(move.QuadPart + 1)) + 1;
            if (distance > base) return STG_E_INVALIDFUNCTION;
            position_ = base - distance;
        } else {
            const auto distance = static_cast<ULONGLONG>(move.QuadPart);
            if (distance > ULLONG_MAX - base) return STG_E_INVALIDFUNCTION;
            position_ = base + distance;
        }
        if (result) result->QuadPart = position_;
        return S_OK;
    }
    IFACEMETHODIMP SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    IFACEMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Commit(DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP Revert() override { return E_NOTIMPL; }
    IFACEMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP Stat(STATSTG* stat, DWORD) override {
        if (!stat) return E_POINTER;
        std::memset(stat, 0, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = bytes_.size();
        stat->grfMode = STGM_READ;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IStream** clone) override {
        if (clone) *clone = nullptr;
        return E_NOTIMPL;
    }
};

void Append16(std::vector<BYTE>& out, std::uint16_t n) {
    out.push_back(static_cast<BYTE>(n)); out.push_back(static_cast<BYTE>(n >> 8));
}
void Append32(std::vector<BYTE>& out, std::uint32_t n) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<BYTE>(n >> shift));
}
void AppendFloat(std::vector<BYTE>& out, float value) {
    std::uint32_t bits; std::memcpy(&bits, &value, 4); Append32(out, bits);
}
void AppendChunk(std::vector<BYTE>& out, std::uint16_t kind, const std::vector<BYTE>& body) {
    Append32(out, zthumb::kStart); Append16(out, kind); Append32(out, static_cast<std::uint32_t>(body.size() + 14));
    out.insert(out.end(), body.begin(), body.end()); Append32(out, zthumb::kEnd);
}
enum class RleMode { Literal, Repeat, ZeroRun, Overrun, TruncatedLiteral, TruncatedRepeat, Underrun };
std::vector<BYTE> Fixture(std::uint16_t layout, RleMode rle = RleMode::Literal) {
    std::vector<BYTE> data(92, 0);
    std::memcpy(data.data(), "ZBrush File.", 12);
    std::memcpy(data.data() + 76, layout == 8 ? "LTZ\0" : "RPZ\0", 4);
    std::memcpy(data.data() + 84, "TEST", 4);
    AppendChunk(data, 12, {0, 0, 'I', 'm', 'a', 'g', 'e', 0});
    std::vector<BYTE> dimensions;
    for (float v : {0.f, 0.f, 2.f, 3.f, 0.f, 0.f, 2.f, 3.f}) AppendFloat(dimensions, v);
    Append16(dimensions, 10); AppendChunk(data, 1, dimensions);
    // Six known RGB pixels: red, green, blue, white, black, orange.
    const BYTE rgb[18] = {255,0,0, 0,255,0, 0,0,255, 255,255,255, 0,0,0, 255,128,0};
    std::vector<BYTE> planar;
    for (unsigned component : {1u, 2u, 0u})
        for (unsigned i = 0; i < 6; ++i) planar.push_back(rgb[i * 3 + component]);
    planar.insert(planar.end(), 6, 255);
    std::vector<BYTE> encoded;
    switch (rle) {
    case RleMode::Literal:
        encoded.push_back(static_cast<BYTE>(256 - planar.size()));
        encoded.insert(encoded.end(), planar.begin(), planar.end());
        break;
    case RleMode::Repeat:
        for (std::size_t pos = 0; pos < planar.size();) {
            std::size_t count = 1;
            while (pos + count < planar.size() && count < 127 && planar[pos + count] == planar[pos]) ++count;
            encoded.push_back(static_cast<BYTE>(count));
            encoded.push_back(planar[pos]);
            pos += count;
        }
        break;
    case RleMode::ZeroRun: encoded = {0, 0}; break;
    case RleMode::Overrun: encoded = {25, 0}; break;
    case RleMode::TruncatedLiteral:
        encoded.push_back(static_cast<BYTE>(256 - planar.size()));
        encoded.insert(encoded.end(), planar.begin(), planar.end() - 1);
        break;
    case RleMode::TruncatedRepeat: encoded = {24}; break;
    case RleMode::Underrun: encoded = {23, 0}; break;
    }
    std::vector<BYTE> compression;
    for (std::uint32_t value : {24u, 0u, 0u, 0u, 0u, static_cast<std::uint32_t>(encoded.size() + 8), 0u, 1u})
        Append32(compression, value);
    compression.insert(compression.end(), encoded.begin(), encoded.end()); Append32(compression, 0);
    std::vector<BYTE> outer(layout == 8 ? 34 : 38, 0);
    AppendChunk(outer, 6, compression);
    if (layout == 10) outer.push_back(0);
    AppendChunk(data, layout, outer);
    return data;
}

std::wstring LocalAbsolute(const wchar_t* input) {
    DWORD required = GetFullPathNameW(input, 0, nullptr, nullptr);
    Check(required != 0 && required < 32768, "Cannot resolve local path");
    std::wstring path(required, L'\0');
    DWORD written = GetFullPathNameW(input, required, path.data(), nullptr);
    Check(written != 0 && written < required, "Cannot resolve local path"); path.resize(written);
    Check(path.size() >= 3 && path[1] == L':' && path[2] == L'\\', "Only drive-local paths are supported");
    Check(path.find(L':', 2) == std::wstring::npos, "Alternate streams are not supported");
    const auto drive = path.substr(0, 3);
    const UINT type = GetDriveTypeW(drive.c_str());
    Check(type == DRIVE_FIXED || type == DRIVE_REMOVABLE, "Remote or unknown drives are not supported");
    for (std::size_t end = 3; end <= path.size(); ++end) {
        if (end != path.size() && path[end] != L'\\') continue;
        const auto portion = path.substr(0, end);
        const DWORD attrs = GetFileAttributesW(portion.c_str());
        Check(attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT),
              "Missing or reparse-point path is not supported");
    }
    return path;
}
using GetClass = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
using CanUnload = HRESULT (STDAPICALLTYPE*)();

void VerifyCase(IClassFactory* factory, const std::vector<BYTE>& bytes, bool valid,
                bool fixedHeaderInvalid = false) {
    ComPtr<IThumbnailProvider> thumbnail;
    Check(SUCCEEDED(factory->CreateInstance(nullptr, __uuidof(IThumbnailProvider),
          reinterpret_cast<void**>(thumbnail.Put()))), "CreateInstance failed");
    ComPtr<IInitializeWithStream> initialize;
    Check(SUCCEEDED(thumbnail->QueryInterface(__uuidof(IInitializeWithStream),
          reinterpret_cast<void**>(initialize.Put()))), "Stream interface unavailable");
    ComPtr<IUnknown> one, two;
    Check(SUCCEEDED(thumbnail->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(one.Put()))) &&
          SUCCEEDED(initialize->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(two.Put()))) &&
          one.Get() == two.Get(), "Controlling IUnknown mismatch");
    {
        OwnedBitmap bitmap; WTS_ALPHATYPE alpha = WTSAT_ARGB;
        Check(thumbnail->GetThumbnail(64, &bitmap.value, &alpha) == E_UNEXPECTED &&
              !bitmap.value && alpha == WTSAT_UNKNOWN, "Uninitialized outputs were not rejected/reset");
        Check(thumbnail->GetThumbnail(64, nullptr, &alpha) == E_POINTER && alpha == WTSAT_UNKNOWN,
              "Null bitmap output was not rejected/reset");
        Check(thumbnail->GetThumbnail(64, &bitmap.value, nullptr) == E_POINTER && !bitmap.value,
              "Null alpha output was not rejected/reset");
    }
    ComPtr<TestStream> stream;
    stream.Attach(new TestStream(bytes));
    LARGE_INTEGER pos{}; pos.QuadPart = 11;
    Check(SUCCEEDED(stream->Seek(pos, STREAM_SEEK_SET, nullptr)), "Cannot position test stream");
    Check(initialize->Initialize(nullptr, STGM_READ) == E_POINTER, "Null input stream accepted");
    Check(initialize->Initialize(stream.Get(), STGM_WRITE) == STG_E_ACCESSDENIED, "Write-only stream accepted");
    Check(SUCCEEDED(initialize->Initialize(stream.Get(), STGM_READ)), "Initialize failed");
    Check(initialize->Initialize(stream.Get(), STGM_READ) == HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED),
          "Repeated initialization was not rejected");
    zthumb::Image expected;
    if (valid) Check(zthumb::Decode(bytes, expected), "Fixture decoder rejected valid sample");
    for (UINT cx : {1u, 2u, 16u, 64u, 96u, 256u, UINT_MAX}) {
        OwnedBitmap bitmap; WTS_ALPHATYPE alpha = WTSAT_ARGB;
        const auto before = stream->bytesRead;
        const HRESULT hr = thumbnail->GetThumbnail(cx, &bitmap.value, &alpha);
        LARGE_INTEGER zero{}; ULARGE_INTEGER current{};
        Check(SUCCEEDED(stream->Seek(zero, STREAM_SEEK_CUR, &current)) && current.QuadPart == 11,
              "Stream position was not restored");
        Check(stream->bytesRead - before <= zthumb::kMaxPrefix && stream->writeCalls == 0,
              "Read budget or no-write rule violated");
        if (fixedHeaderInvalid) Check(stream->bytesRead - before == 176, "Invalid fixed header caused extra reads");
        if (!valid) {
            Check(FAILED(hr) && !bitmap.value && alpha == WTSAT_UNKNOWN, "Invalid image produced a bitmap");
            continue;
        }
        Check(SUCCEEDED(hr) && bitmap.value && alpha == WTSAT_RGB, "GetThumbnail failed");
        DIBSECTION section{};
        Check(GetObjectW(bitmap.value, sizeof(section), &section) == sizeof(section), "Not a DIB section");
        const auto largest = (std::max)(expected.width, expected.height);
        const auto limit = (std::min)(cx, largest);
        const auto width = (std::max)(1u, expected.width * limit / largest);
        const auto height = (std::max)(1u, expected.height * limit / largest);
        std::cout << "DIB expected " << width << 'x' << height
                  << ", actual " << section.dsBm.bmWidth << 'x' << section.dsBm.bmHeight
                  << ", biHeight " << section.dsBmih.biHeight
                  << ", bpp " << section.dsBm.bmBitsPixel << '\n';
        Check(section.dsBm.bmBitsPixel == 32 && section.dsBm.bmBits &&
              section.dsBm.bmWidth == static_cast<LONG>(width) &&
              section.dsBm.bmHeight == static_cast<LONG>(height) &&
              (section.dsBmih.biHeight == static_cast<LONG>(height) ||
               section.dsBmih.biHeight == -static_cast<LONG>(height)), "Bitmap format/dimensions are incorrect");
        // GetObject on this Windows build reports a positive dsBmih.biHeight even
        // for our negative-height CreateDIBSection input. Verify actual GDI row
        // orientation by requesting a canonical top-down copy, not by its sign.
        OwnedMemoryDC dc;
        Check(dc.value != nullptr, "Cannot create memory DC for orientation check");
        BITMAPINFO topDown{};
        topDown.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        topDown.bmiHeader.biWidth = static_cast<LONG>(width);
        topDown.bmiHeader.biHeight = -static_cast<LONG>(height);
        topDown.bmiHeader.biPlanes = 1;
        topDown.bmiHeader.biBitCount = 32;
        topDown.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> canonical(static_cast<std::size_t>(width) * height * 4);
        Check(GetDIBits(dc.value, bitmap.value, 0, height, canonical.data(), &topDown, DIB_RGB_COLORS) ==
              static_cast<int>(height), "Cannot read canonical top-down bitmap rows");
        const auto* pixels = static_cast<const BYTE*>(section.dsBm.bmBits);
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const auto source = ((y * expected.height / height) * expected.width + x * expected.width / width) * 3;
            const auto target = static_cast<std::size_t>(y) * section.dsBm.bmWidthBytes + x * 4;
            Check(pixels[target] == expected.rgb[source + 2] && pixels[target + 1] == expected.rgb[source + 1] &&
                  pixels[target + 2] == expected.rgb[source] && pixels[target + 3] == 255, "Bitmap pixels differ");
            const auto canonicalTarget = (static_cast<std::size_t>(y) * width + x) * 4;
            // BI_RGB's high byte is reserved; check opaque alpha above on the
            // original provider-owned DIB, and RGB/orientation through GDI here.
            Check(canonical[canonicalTarget] == expected.rgb[source + 2] &&
                  canonical[canonicalTarget + 1] == expected.rgb[source + 1] &&
                  canonical[canonicalTarget + 2] == expected.rgb[source],
                  "GDI top-down row orientation or RGB differs");
        }
        std::cout << "PASS bitmap " << width << 'x' << height << ", read " << stream->bytesRead - before << " bytes\n";
    }
    {
        OwnedBitmap bitmap; WTS_ALPHATYPE alpha = WTSAT_RGB;
        Check(thumbnail->GetThumbnail(0, &bitmap.value, &alpha) == E_INVALIDARG && !bitmap.value &&
              alpha == WTSAT_UNKNOWN, "Zero size was not rejected");
    }
}

// Original generated test chart, not a ZBrush/vendored/user model asset.
std::vector<BYTE> GradientFixture(bool project) {
    std::vector<BYTE> data(92, 0);
    std::memcpy(data.data(), "ZBrush File.", 12);
    std::memcpy(data.data() + 76, project ? "RPZ\0" : "LTZ\0", 4);
    AppendChunk(data, 12, {0, 0, 'I', 'm', 'a', 'g', 'e', 0});
    std::vector<BYTE> dimensions;
    for (float v : {0.f, 0.f, 96.f, 96.f, 0.f, 0.f, 96.f, 96.f}) AppendFloat(dimensions, v);
    Append16(dimensions, 10); AppendChunk(data, 1, dimensions);
    std::vector<BYTE> rgb;
    for (unsigned y = 0; y < 96; ++y) for (unsigned x = 0; x < 96; ++x) {
        rgb.push_back(static_cast<BYTE>(x * 255 / 95));
        rgb.push_back(static_cast<BYTE>(y * 255 / 95));
        rgb.push_back(static_cast<BYTE>(((x / 12 + y / 12) % 2) * 150 + 40));
    }
    std::vector<BYTE> planar;
    for (unsigned component : {1u, 2u, 0u})
        for (std::size_t pixel = 0; pixel < 96 * 96; ++pixel) planar.push_back(rgb[pixel * 3 + component]);
    planar.insert(planar.end(), 96 * 96, 255);
    std::vector<BYTE> encoded;
    for (std::size_t position = 0; position < planar.size();) {
        const auto length = (std::min)(std::size_t{128}, planar.size() - position);
        encoded.push_back(static_cast<BYTE>(256 - length));
        encoded.insert(encoded.end(), planar.begin() + position, planar.begin() + position + length);
        position += length;
    }
    std::vector<BYTE> compression;
    for (std::uint32_t v : {96u * 96 * 4, 0u, 0u, 0u, 0u, static_cast<std::uint32_t>(encoded.size() + 8), 0u, 1u})
        Append32(compression, v);
    compression.insert(compression.end(), encoded.begin(), encoded.end()); Append32(compression, 0);
    std::vector<BYTE> outer(38, 0); AppendChunk(outer, 6, compression); outer.push_back(0);
    AppendChunk(data, 10, outer);
    zthumb::Image decoded;
    Check(zthumb::Decode(data, decoded) && decoded.width == 96 && decoded.height == 96 && decoded.rgb == rgb,
          "Generated 96x96 chart differs from independently specified RGB");
    return data;
}

void Run(HMODULE module) {
    auto getClass = reinterpret_cast<GetClass>(GetProcAddress(module, "DllGetClassObject"));
    auto canUnload = reinterpret_cast<CanUnload>(GetProcAddress(module, "DllCanUnloadNow"));
    Check(getClass && canUnload && canUnload() == S_OK, "DLL export or initial unload state invalid");
    Check(!GetProcAddress(module, "DllRegisterServer"), "Unexpected self-registration export");
    {
        const GUID unknownClass{};
        void* result = reinterpret_cast<void*>(1);
        Check(getClass(unknownClass, IID_IClassFactory, &result) == CLASS_E_CLASSNOTAVAILABLE && !result,
              "Unknown class was not rejected/reset");
        Check(getClass(CLSID_ZBrushThumbnailProvider, IID_IStream, &result) == E_NOINTERFACE && !result &&
              canUnload() == S_OK, "Unknown factory interface was not rejected cleanly");
    }
    {
        ComPtr<IClassFactory> factory;
        Check(SUCCEEDED(getClass(CLSID_ZBrushThumbnailProvider, IID_IClassFactory,
              reinterpret_cast<void**>(factory.Put()))), "Class factory unavailable");
        Check(canUnload() == S_FALSE, "Factory reference missing from unload count");
        Check(SUCCEEDED(factory->LockServer(TRUE)) && SUCCEEDED(factory->LockServer(FALSE)), "Server lock failed");
        Check(factory->LockServer(FALSE) == E_UNEXPECTED, "Unmatched server unlock accepted");
        void* aggregate = reinterpret_cast<void*>(1);
        Check(factory->CreateInstance(factory.Get(), IID_IUnknown, &aggregate) == CLASS_E_NOAGGREGATION && !aggregate,
              "Aggregation was not rejected");
        for (auto layout : {8u, 10u}) {
            auto bytes = Fixture(static_cast<std::uint16_t>(layout));
            zthumb::Image known;
            const std::vector<BYTE> expectedRgb = {255,0,0, 0,255,0, 0,0,255, 255,255,255, 0,0,0, 255,128,0};
            Check(zthumb::Decode(bytes, known) && known.width == 2 && known.height == 3 && known.rgb == expectedRgb,
                  "Fixture RGB differs from independently specified pixels");
            VerifyCase(factory.Get(), bytes, true);
            const auto repeated = Fixture(static_cast<std::uint16_t>(layout), RleMode::Repeat);
            Check(zthumb::Decode(repeated, known) && known.rgb == expectedRgb, "Repeat RLE pixel mismatch");
            VerifyCase(factory.Get(), repeated, true);
            for (auto mode : {RleMode::ZeroRun, RleMode::Overrun, RleMode::TruncatedLiteral,
                              RleMode::TruncatedRepeat, RleMode::Underrun})
                VerifyCase(factory.Get(), Fixture(static_cast<std::uint16_t>(layout), mode), false);
            bytes[168] = bytes[169] = bytes[170] = bytes[171] = 255; // excessive outer length
            VerifyCase(factory.Get(), bytes, false, true);
        }
        auto bytes = Fixture(10);
        bytes[132] = 0; bytes[133] = 0; bytes[134] = 192; bytes[135] = 127; // NaN width
        VerifyCase(factory.Get(), bytes, false, true);
        bytes = Fixture(10); bytes.pop_back();
        VerifyCase(factory.Get(), bytes, false);
        bytes = Fixture(10); bytes.back() = 1; // invalid outer close marker
        VerifyCase(factory.Get(), bytes, false);
        bytes = Fixture(10); bytes[76] = 'X'; // unknown file kind
        VerifyCase(factory.Get(), bytes, false, true);
        bytes = Fixture(10); bytes[166] = 9; // unsupported image layout
        VerifyCase(factory.Get(), bytes, false, true);
        VerifyCase(factory.Get(), GradientFixture(false), true);
        VerifyCase(factory.Get(), GradientFixture(true), true);
    }
    Check(canUnload() == S_OK, "COM objects or server locks leaked");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "Usage: ProviderTests.exe <local DLL path>\n"
                     "Runs only generated synthetic tests; no model reads, registry, UI or output files.\n";
        return 2;
    }
    try {
        ComApartment apartment;
        Check(SUCCEEDED(apartment.result), "COM initialization failed");
        OwnedModule module;
        const auto dllPath = LocalAbsolute(argv[1]);
        module.value = LoadLibraryExW(dllPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        Check(module.value != nullptr, "Cannot load local test DLL");
        Run(module.value);
        std::cout << "All registration-free host checks passed. Explorer integration remains untested.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
