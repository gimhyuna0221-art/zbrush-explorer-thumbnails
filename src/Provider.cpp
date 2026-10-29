#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <propsys.h>
#include <thumbcache.h>
#include <new>
#include <limits>
#include "Decoder.h"
#include "ProviderId.h"

namespace {
volatile LONG g_objects = 0;
volatile LONG g_locks = 0;
const HRESULT kInvalidImage = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

class ExclusiveLock {
    SRWLOCK* lock_;
public:
    explicit ExclusiveLock(SRWLOCK* lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(lock_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(lock_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
};

HRESULT ReadExactly(IStream* stream, BYTE* target, ULONG length) noexcept {
    ULONG done = 0;
    while (done < length) {
        ULONG count = 0;
        const HRESULT hr = stream->Read(target + done, length - done, &count);
        if (FAILED(hr)) return hr;
        if (count == 0 || count > length - done) return STG_E_READFAULT;
        done += count; // S_FALSE with progress is permitted; request remaining bytes.
    }
    return S_OK;
}

// Restores the original stream position even when allocation/decoding throws.
class StreamPosition {
    IStream* stream_;
    ULARGE_INTEGER previous_{};
    bool valid_ = false;
public:
    explicit StreamPosition(IStream* stream) noexcept : stream_(stream) {}
    HRESULT Save() noexcept {
        LARGE_INTEGER zero{};
        const HRESULT hr = stream_->Seek(zero, STREAM_SEEK_CUR, &previous_);
        if (FAILED(hr)) return hr;
        if (previous_.QuadPart > static_cast<ULONGLONG>((std::numeric_limits<LONGLONG>::max)()))
            return STG_E_INVALIDFUNCTION;
        valid_ = true;
        return S_OK;
    }
    HRESULT Restore() noexcept {
        if (!valid_) return S_OK;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(previous_.QuadPart);
        const HRESULT hr = stream_->Seek(position, STREAM_SEEK_SET, nullptr);
        if (SUCCEEDED(hr)) valid_ = false;
        return hr;
    }
    ~StreamPosition() { if (valid_) Restore(); }
};

HRESULT DecodeStream(IStream* stream, zthumb::Image& image) {
    StreamPosition original(stream);
    HRESULT hr = original.Save();
    if (FAILED(hr)) return hr;
    LARGE_INTEGER zero{};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (SUCCEEDED(hr)) {
        std::vector<BYTE> prefix(176);
        hr = ReadExactly(stream, prefix.data(), static_cast<ULONG>(prefix.size()));
        if (SUCCEEDED(hr)) {
            std::size_t required = 0;
            if (!zthumb::RequiredPrefix(prefix, required)) hr = kInvalidImage;
            else {
                prefix.resize(required);
                hr = ReadExactly(stream, prefix.data() + 176, static_cast<ULONG>(required - 176));
                if (SUCCEEDED(hr) && !zthumb::Decode(prefix, image)) hr = kInvalidImage;
            }
        }
    }
    const HRESULT restored = original.Restore();
    return FAILED(restored) ? restored : hr;
}

HRESULT CreateThumbnail(const zthumb::Image& image, UINT cx, HBITMAP* result) noexcept {
    if (cx == 0 || image.width == 0 || image.height == 0) return E_INVALIDARG;
    const auto largest = (std::max)(image.width, image.height);
    const auto limit = (std::min)(static_cast<std::uint32_t>(cx), largest); // never upscale
    const auto width = (std::max)(1u, static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(image.width) * limit / largest));
    const auto height = (std::max)(1u, static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(image.height) * limit / largest));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        return E_OUTOFMEMORY;
    }
    auto* bgra = static_cast<BYTE*>(pixels);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto sourceY = static_cast<std::uint64_t>(y) * image.height / height;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto sourceX = static_cast<std::uint64_t>(x) * image.width / width;
            const auto in = static_cast<std::size_t>((sourceY * image.width + sourceX) * 3);
            const auto out = static_cast<std::size_t>((static_cast<std::uint64_t>(y) * width + x) * 4);
            bgra[out] = image.rgb[in + 2];
            bgra[out + 1] = image.rgb[in + 1];
            bgra[out + 2] = image.rgb[in];
            bgra[out + 3] = 255;
        }
    }
    *result = bitmap; // ownership transfers to the Shell/caller, which uses DeleteObject.
    return S_OK;
}

class Provider final : public IThumbnailProvider, public IInitializeWithStream {
    volatile LONG refs_ = 1;
    SRWLOCK mutex_ = SRWLOCK_INIT;
    IStream* stream_ = nullptr;
public:
    Provider() noexcept { InterlockedIncrement(&g_objects); }
    ~Provider() {
        if (stream_) stream_->Release();
        InterlockedDecrement(&g_objects);
    }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, __uuidof(IThumbnailProvider)))
            *object = static_cast<IThumbnailProvider*>(this);
        else if (IsEqualIID(iid, __uuidof(IInitializeWithStream)))
            *object = static_cast<IInitializeWithStream*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG remaining = InterlockedDecrement(&refs_);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override {
        if (!stream) return E_POINTER;
        ExclusiveLock lock(&mutex_);
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        const DWORD access = mode & 3;
        if (access != STGM_READ && access != STGM_READWRITE) return STG_E_ACCESSDENIED;
        // Even for a read/write-capable supplied stream, this implementation only reads/seeks.
        stream->AddRef();
        stream_ = stream;
        return S_OK;
    }
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override {
        if (bitmap) *bitmap = nullptr;
        if (alpha) *alpha = WTSAT_UNKNOWN;
        if (!bitmap || !alpha) return E_POINTER;
        if (cx == 0) return E_INVALIDARG;
        ExclusiveLock lock(&mutex_);
        if (!stream_) return E_UNEXPECTED;
        try {
            zthumb::Image image;
            HRESULT hr = DecodeStream(stream_, image);
            if (FAILED(hr)) return hr;
            hr = CreateThumbnail(image, cx, bitmap);
            if (SUCCEEDED(hr)) *alpha = WTSAT_RGB;
            return hr;
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
          catch (...) { return E_FAIL; }
    }
};

class Factory final : public IClassFactory {
    volatile LONG refs_ = 1;
public:
    Factory() noexcept { InterlockedIncrement(&g_objects); }
    ~Factory() { InterlockedDecrement(&g_objects); }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IClassFactory)) return E_NOINTERFACE;
        *object = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG remaining = InterlockedDecrement(&refs_);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* provider = new (std::nothrow) Provider();
        if (!provider) return E_OUTOFMEMORY;
        const HRESULT hr = provider->QueryInterface(iid, object);
        provider->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) { InterlockedIncrement(&g_locks); return S_OK; }
        LONG count = InterlockedCompareExchange(&g_locks, 0, 0);
        while (count > 0) {
            const LONG previous = InterlockedCompareExchange(&g_locks, count - 1, count);
            if (previous == count) return S_OK;
            count = previous;
        }
        return E_UNEXPECTED; // reject an unmatched unlock rather than corrupt unload state
    }
};
} // namespace

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (!IsEqualCLSID(clsid, CLSID_ZBrushThumbnailProvider)) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) Factory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(iid, object);
    factory->Release();
    return hr;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return InterlockedCompareExchange(&g_objects, 0, 0) == 0 &&
           InterlockedCompareExchange(&g_locks, 0, 0) == 0 ? S_OK : S_FALSE;
}
// Registration is managed by the separate per-user installer, never by this DLL.
