#include "psdthumb.h"
#include "PsdThumbnailProvider.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <new>
#include <string>

HMODULE g_hModule = nullptr;
volatile LONG g_cDllRef = 0;

namespace {

class ClassFactory : public IClassFactory {
public:
    ClassFactory() : m_ref(1) { InterlockedIncrement(&g_cDllRef); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        static const QITAB qit[] = { QITABENT(ClassFactory, IClassFactory), { nullptr, 0 } };
        return QISearch(this, qit, riid, ppv);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG ref = InterlockedDecrement(&m_ref);
        if (ref == 0) delete this;
        return ref;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        PsdThumbnailProvider* p = new (std::nothrow) PsdThumbnailProvider();
        if (!p) return E_OUTOFMEMORY;
        const HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_cDllRef);
        else InterlockedDecrement(&g_cDllRef);
        return S_OK;
    }

private:
    ~ClassFactory() { InterlockedDecrement(&g_cDllRef); }
    LONG m_ref;
};

const wchar_t* const kExtensions[] = { L".psd", L".psb" };
const wchar_t kThumbnailHandlerKey[] = L"{E357FCCD-A995-4576-B01F-234630154E96}";
const wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";

LSTATUS SetKeyValue(HKEY root, const std::wstring& subkey, const wchar_t* valueName, const std::wstring& data) {
    HKEY hk;
    LSTATUS st = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hk, nullptr);
    if (st != ERROR_SUCCESS) return st;
    st = RegSetValueExW(hk, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                        (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return st;
}

bool ReadDefaultValue(HKEY root, const std::wstring& subkey, std::wstring& out) {
    wchar_t buf[128] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    const LSTATUS st = RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &cb);
    if (st != ERROR_SUCCESS) return false;
    out = buf;
    return true;
}

// root is HKEY_LOCAL_MACHINE (all users, needs admin) or HKEY_CURRENT_USER (this user only).
HRESULT RegisterServer(HKEY root) {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(g_hModule, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring clsidKey = classes + L"CLSID\\" + PSDTHUMB_CLSID_STRING;
    LSTATUS st;
    if ((st = SetKeyValue(root, clsidKey, nullptr, PSDTHUMB_FRIENDLY_NAME)) != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    if ((st = SetKeyValue(root, clsidKey + L"\\InprocServer32", nullptr, path)) != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    if ((st = SetKeyValue(root, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment")) != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    for (const wchar_t* ext : kExtensions) {
        const std::wstring key = classes + ext + L"\\ShellEx\\" + kThumbnailHandlerKey;
        if ((st = SetKeyValue(root, key, nullptr, PSDTHUMB_CLSID_STRING)) != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    }
    if (root == HKEY_LOCAL_MACHINE) SetKeyValue(root, kApprovedKey, PSDTHUMB_CLSID_STRING, PSDTHUMB_FRIENDLY_NAME);  // best effort
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

HRESULT UnregisterServer(HKEY root) {
    const std::wstring classes = L"Software\\Classes\\";
    for (const wchar_t* ext : kExtensions) {
        const std::wstring key = classes + ext + L"\\ShellEx\\" + kThumbnailHandlerKey;
        std::wstring current;
        if (ReadDefaultValue(root, key, current) && _wcsicmp(current.c_str(), PSDTHUMB_CLSID_STRING) == 0)
            RegDeleteTreeW(root, key.c_str());  // only remove the association if it still points at us
    }
    RegDeleteTreeW(root, (classes + L"CLSID\\" + PSDTHUMB_CLSID_STRING).c_str());
    if (root == HKEY_LOCAL_MACHINE) {
        HKEY hk;
        if (RegOpenKeyExW(root, kApprovedKey, 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
            RegDeleteValueW(hk, PSDTHUMB_CLSID_STRING);
            RegCloseKey(hk);
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    return g_cDllRef > 0 ? S_FALSE : S_OK;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_PsdThumbnailProvider)) return CLASS_E_CLASSNOTAVAILABLE;
    ClassFactory* f = new (std::nothrow) ClassFactory();
    if (!f) return E_OUTOFMEMORY;
    const HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

// regsvr32 psdthumb.dll            -> all users (HKLM, needs an elevated prompt)
STDAPI DllRegisterServer() {
    return RegisterServer(HKEY_LOCAL_MACHINE);
}

STDAPI DllUnregisterServer() {
    return UnregisterServer(HKEY_LOCAL_MACHINE);
}

// regsvr32 /n /i:user psdthumb.dll -> current user only (HKCU, no admin needed)
STDAPI DllInstall(BOOL bInstall, PCWSTR pszCmdLine) {
    const bool perUser = pszCmdLine && StrStrIW(pszCmdLine, L"user") != nullptr;
    const HKEY root = perUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    return bInstall ? RegisterServer(root) : UnregisterServer(root);
}
