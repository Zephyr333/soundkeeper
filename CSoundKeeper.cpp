// Tell mmdeviceapi.h to define its GUIDs in this translation unit.
#define INITGUID

#include "CSoundKeeper.hpp"
#include "Resources.hpp"

//
// Get time to sleeping (in seconds). It is not precise and updated just once in 15-30 seconds!
// On Windows 7-10, it outputs -1 (or values like -16, -31, ...) when auto sleep mode is disabled.
// On Windows 10, it may output negative values (-30, -60, ...) when sleeping was postponed not by user input.
// On Windows 11, the TimeRemaining field is always 0, so it doesn't work for some reason.
//

EXTERN_C NTSYSCALLAPI NTSTATUS NTAPI NtPowerInformation(
    _In_ POWER_INFORMATION_LEVEL InformationLevel,
    _In_reads_bytes_opt_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(OutputBufferLength) PVOID OutputBuffer,
    _In_ ULONG OutputBufferLength);

uint32_t GetSecondsToSleeping()
{
  struct SYSTEM_POWER_INFORMATION
  {
    ULONG MaxIdlenessAllowed;
    ULONG Idleness;
    LONG TimeRemaining;
    UCHAR CoolingMode;
  } spi = {0};

  if (!NT_SUCCESS(NtPowerInformation(SystemPowerInformation, NULL, 0, &spi, sizeof(spi))))
  {
    DebugLogError("Cannot get remaining time to sleeping.");
    return 0;
  }

#ifdef _CONSOLE

  static LONG last_result = 0x80000000;

  if (last_result != spi.TimeRemaining)
  {
    last_result = spi.TimeRemaining;
    DebugLog("Remaining time to sleeping: %d seconds.", spi.TimeRemaining);
  }

#endif

  if (spi.TimeRemaining >= 0)
  {
    return spi.TimeRemaining;
  }

  return (spi.TimeRemaining % 5 == 0 ? 0 : -1);
}

//
// CSoundKeeper implementation.
//

CSoundKeeper::CSoundKeeper() {}
CSoundKeeper::~CSoundKeeper() {}

// IUnknown methods

ULONG STDMETHODCALLTYPE CSoundKeeper::AddRef()
{
  return InterlockedIncrement(&m_ref_count);
}

ULONG STDMETHODCALLTYPE CSoundKeeper::Release()
{
  ULONG result = InterlockedDecrement(&m_ref_count);
  if (result == 0)
  {
    delete this;
  }
  return result;
}

HRESULT STDMETHODCALLTYPE CSoundKeeper::QueryInterface(REFIID riid, VOID **ppvInterface)
{
  if (IID_IUnknown == riid)
  {
    AddRef();
    *ppvInterface = (IUnknown *)this;
  }
  else if (__uuidof(IMMNotificationClient) == riid)
  {
    AddRef();
    *ppvInterface = (IMMNotificationClient *)this;
  }
  else
  {
    *ppvInterface = NULL;
    return E_NOINTERFACE;
  }
  return S_OK;
}

// Callback methods for device-event notifications.
// WARNING: Don't use m_mutex, it may cause a deadlock when CSoundKeeper::Restart -> Stop is in progress.

HRESULT STDMETHODCALLTYPE CSoundKeeper::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR device_id)
{
  DebugLog("Device '%S' is default for flow %d and role %d.", device_id ? device_id : L"", flow, role);
  if (m_cfg_device_type == KeepDeviceType::Primary && flow == eRender && role == eConsole)
  {
    this->FireRestart();
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundKeeper::OnDeviceAdded(LPCWSTR device_id)
{
  DebugLog("Device '%S' was added.", device_id);
  if (m_cfg_device_type != KeepDeviceType::Primary)
  {
    this->FireRestart();
  }
  return S_OK;
};

HRESULT STDMETHODCALLTYPE CSoundKeeper::OnDeviceRemoved(LPCWSTR device_id)
{
  DebugLog("Device '%S' was removed.", device_id);
  if (m_cfg_device_type != KeepDeviceType::Primary)
  {
    this->FireRestart();
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundKeeper::OnDeviceStateChanged(LPCWSTR device_id, DWORD new_state)
{
  DebugLog("Device '%S' new state: %d.", device_id, new_state);
  if (new_state == DEVICE_STATE_ACTIVE)
  {
    this->FireRestart();
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundKeeper::OnPropertyValueChanged(LPCWSTR device_id, const PROPERTYKEY key)
{
  return S_OK;
}

// Main thread methods.

uint32_t GetDeviceFormFactor(IMMDevice *device)
{
  uint32_t formfactor = -1;

  IPropertyStore *properties = nullptr;
  HRESULT hr = device->OpenPropertyStore(STGM_READ, &properties);
  if (FAILED(hr))
  {
    DebugLogWarning("Unable to retrieve property store of an audio device: 0x%08X.", hr);
    return formfactor;
  }

  PROPVARIANT prop_formfactor;
  PropVariantInit(&prop_formfactor);
  hr = properties->GetValue(PKEY_AudioEndpoint_FormFactor, &prop_formfactor);
  if (SUCCEEDED(hr) && prop_formfactor.vt == VT_UI4)
  {
    formfactor = prop_formfactor.uintVal;
#ifdef _CONSOLE
    LPWSTR device_id = nullptr;
    hr = device->GetId(&device_id);
    if (FAILED(hr))
    {
      DebugLogWarning("Unable to get device ID: 0x%08X.", hr);
    }
    else
    {
      DebugLog("Device ID: '%S'. Form Factor: %d.", device_id, formfactor);
      CoTaskMemFree(device_id);
    }
#endif
  }
  else
  {
    DebugLogWarning("Unable to retrieve formfactor of an audio device: 0x%08X.", hr);
  }

  PropVariantClear(&prop_formfactor);
  SafeRelease(properties);

  return formfactor;
}

HRESULT CSoundKeeper::Start()
{
  ScopedLock lock(m_mutex);

  HRESULT hr = S_OK;
  if (m_is_started)
  {
    return hr;
  }
  m_is_retry_required = false;

  if (m_cfg_device_type == KeepDeviceType::Primary)
  {
    DebugLog("Getting primary audio device...");

    IMMDevice *device = nullptr;
    hr = m_dev_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr))
    {
      if (hr == E_NOTFOUND)
      {
        DebugLogWarning("No primary device found. Working as a dummy...");
        m_is_started = true;
        return hr;
      }
      else
      {
        DebugLogError("Unable to retrieve default render device: 0x%08X.", hr);
        return hr;
      }
    }
    defer[&] { device->Release(); };

    m_is_started = true;

    if (uint32_t formfactor = GetDeviceFormFactor(device); formfactor == -1)
    {
      return hr;
    }
    else if (!m_cfg_allow_remote && formfactor == RemoteNetworkDevice)
    {
      DebugLog("Ignoring remote desktop audio device.");
      return hr;
    }

    m_sessions_count = 1;
    m_sessions = new CSoundSession *[m_sessions_count]();

    m_sessions[0] = new CSoundSession(this, device);
    m_sessions[0]->SetStreamType(m_cfg_stream_type);
    m_sessions[0]->SetFrequency(m_cfg_frequency);
    m_sessions[0]->SetAmplitude(m_cfg_amplitude);
    m_sessions[0]->SetPeriodicPlaying(m_cfg_play_seconds);
    m_sessions[0]->SetPeriodicWaiting(m_cfg_wait_seconds);
    m_sessions[0]->SetFading(m_cfg_fade_seconds);

    if (!m_sessions[0]->Start())
    {
      m_is_retry_required = true;
    }
  }
  else
  {
    DebugLog("Enumerating active audio devices...");

    IMMDeviceCollection *dev_collection = nullptr;
    hr = m_dev_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &dev_collection);
    if (FAILED(hr))
    {
      DebugLogError("Unable to retrieve device collection: 0x%08X.", hr);
      return hr;
    }
    defer[&] { dev_collection->Release(); };

    hr = dev_collection->GetCount(&m_sessions_count);
    if (FAILED(hr))
    {
      DebugLogError("Unable to get device collection length: 0x%08X.", hr);
      return hr;
    }

    m_is_started = true;

    m_sessions = new CSoundSession *[m_sessions_count]();

    for (UINT i = 0; i < m_sessions_count; i++)
    {
      m_sessions[i] = nullptr;

      IMMDevice *device = nullptr;
      if (dev_collection->Item(i, &device) != S_OK)
      {
        continue;
      }
      defer[&] { device->Release(); };

      if (uint32_t formfactor = GetDeviceFormFactor(device); formfactor == -1)
      {
        continue;
      }
      else if (!m_cfg_allow_remote && formfactor == RemoteNetworkDevice)
      {
        DebugLog("Ignoring remote desktop audio device.");
        continue;
      }
      else if ((m_cfg_device_type == KeepDeviceType::Digital || m_cfg_device_type == KeepDeviceType::Analog) && (m_cfg_device_type == KeepDeviceType::Digital) != (formfactor == SPDIF || formfactor == HDMI))
      {
        DebugLog("Skipping this device because of the Digital / Analog filter.");
        continue;
      }

      m_sessions[i] = new CSoundSession(this, device);
      m_sessions[i]->SetStreamType(m_cfg_stream_type);
      m_sessions[i]->SetFrequency(m_cfg_frequency);
      m_sessions[i]->SetAmplitude(m_cfg_amplitude);
      m_sessions[i]->SetPeriodicPlaying(m_cfg_play_seconds);
      m_sessions[i]->SetPeriodicWaiting(m_cfg_wait_seconds);
      m_sessions[i]->SetFading(m_cfg_fade_seconds);

      if (!m_sessions[i]->Start())
      {
        m_is_retry_required = true;
      }
    }

    if (m_sessions_count == 0)
    {
      DebugLogWarning("No suitable devices found. Working as a dummy...");
    }
  }

  return hr;
}

bool CSoundKeeper::Retry()
{
  ScopedLock lock(m_mutex);

  if (!m_is_retry_required)
    return true;
  m_is_retry_required = false;

  if (m_sessions != nullptr)
  {
    for (UINT i = 0; i < m_sessions_count; i++)
    {
      if (m_sessions[i] != nullptr)
      {
        if (!m_sessions[i]->Start())
        {
          m_is_retry_required = true;
        }
      }
    }
  }

  return !m_is_retry_required;
}

HRESULT CSoundKeeper::Stop()
{
  ScopedLock lock(m_mutex);

  if (!m_is_started)
    return S_OK;

  if (m_sessions != nullptr)
  {
    for (UINT i = 0; i < m_sessions_count; i++)
    {
      if (m_sessions[i] != nullptr)
      {
        m_sessions[i]->Stop();
        m_sessions[i]->Release();
      }
    }
    delete m_sessions;
  }

  m_sessions = nullptr;
  m_sessions_count = 0;
  m_is_started = false;
  m_is_retry_required = false;
  return S_OK;
}

HRESULT CSoundKeeper::Restart()
{
  ScopedLock lock(m_mutex);

  HRESULT hr = S_OK;

  hr = this->Stop();
  if (FAILED(hr))
  {
    return hr;
  }

  hr = this->Start();
  if (FAILED(hr))
  {
    return hr;
  }

  return S_OK;
}

CSoundSession *CSoundKeeper::FindSession(LPCWSTR device_id)
{
  ScopedLock lock(m_mutex);

  for (UINT i = 0; i < m_sessions_count; i++)
  {
    if (m_sessions[i] == nullptr)
    {
      continue;
    }

    if (auto curr = m_sessions[i]->GetDeviceId(); curr && StringEquals(curr, device_id))
    {
      // Call AddRef()? Use ComPtr?
      return m_sessions[i];
    }
  }

  return nullptr;
}

// Fire main thread control events.

void CSoundKeeper::FireRetry()
{
  TraceLog("Fire Retry!");
  m_do_retry = true;
}

void CSoundKeeper::FireRestart()
{
  TraceLog("Fire Restart!");
  m_do_restart = true;
}

void CSoundKeeper::FireShutdown()
{
  TraceLog("Fire Shutdown!");
  m_do_shutdown = true;
}

// Mute toggle and state persistence via HKCU\Software\SoundKeeper.

static const wchar_t *kConfigKey = L"Software\\SoundKeeper";
static const wchar_t *kMutedValue = L"IsMuted";

bool CSoundKeeper::LoadMuteState() const
{
  HKEY hKey = NULL;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
    return false;

  DWORD val = 0;
  DWORD cbData = sizeof(val);
  DWORD type = 0;
  bool muted = false;
  if (RegQueryValueExW(hKey, kMutedValue, NULL, &type, (LPBYTE)&val, &cbData) == ERROR_SUCCESS && type == REG_DWORD)
  {
    muted = (val != 0);
  }

  RegCloseKey(hKey);
  return muted;
}

void CSoundKeeper::SaveMuteState() const
{
  HKEY hKey = NULL;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS)
  {
    DebugLogError("SaveMuteState: failed to open or create config registry key.");
    return;
  }

  DWORD val = m_is_muted ? 1 : 0;
  RegSetValueExW(hKey, kMutedValue, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
  RegCloseKey(hKey);
}

void CSoundKeeper::ToggleMute()
{
  m_is_muted = !m_is_muted;
  this->SaveMuteState();
  DebugLog("Mute toggled: %s.", m_is_muted ? "Muted" : "Unmuted");

  if (m_is_muted)
  {
    this->Stop();
  }
  else
  {
    this->FireRestart();
  }

  this->UpdateTrayIcon();
}

// Tray icon methods.

HICON CSoundKeeper::CreateSpeakerIcon(bool muted)
{
  // Use the standard small icon size for tray (e.g. 16x16 at 100% DPI).
  const int S = GetSystemMetrics(SM_CXSMICON);
  if (S <= 0)
    return NULL;

  // Create a 32-bit ARGB DIB section for per-pixel alpha.
  BITMAPINFOHEADER bih = {};
  bih.biSize = sizeof(bih);
  bih.biWidth = S;
  bih.biHeight = -S; // top-down
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;

  uint32_t *bits = nullptr;
  HDC hScreenDC = GetDC(NULL);
  HDC hDC = CreateCompatibleDC(hScreenDC);
  HBITMAP hBmp = CreateDIBSection(hDC, (BITMAPINFO *)&bih, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
  if (!hBmp || !bits)
  {
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreenDC);
    return NULL;
  }
  HBITMAP hOldBmp = (HBITMAP)SelectObject(hDC, hBmp);

  // Fill with transparent black.
  memset(bits, 0, S * S * sizeof(uint32_t));

  // Use Segoe UI Emoji font to render the emoji character onto the bitmap.
  // Pick font height slightly smaller than icon size for padding.
  int fontHeight = S > 4 ? S - 2 : S;
  HFONT hFont = CreateFontW(
      fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI Emoji");
  HFONT hOldFont = (HFONT)SelectObject(hDC, hFont);

  // Transparent background for DrawText.
  SetBkMode(hDC, TRANSPARENT);
  SetTextColor(hDC, RGB(255, 255, 255));

  // Choose the emoji: \U0001F50A = \xD83D\xDD0A (speaker high) or \U0001F507 = \xD83D\xDD07 (muted speaker)
  const wchar_t *emoji = muted ? L"\xD83D\xDD07" : L"\xD83D\xDD0A";

  RECT rc = {0, 0, S, S};
  DrawTextW(hDC, emoji, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

  SelectObject(hDC, hOldFont);
  DeleteObject(hFont);

  // After DrawTextW, Windows renders color emoji with premultiplied alpha into the DIB.
  // However, the alpha channel might be 0 even for colored pixels (GDI ignores alpha).
  // Fix: for any pixel that has non-zero RGB but zero alpha, set alpha to 0xFF.
  for (int i = 0; i < S * S; i++)
  {
    uint32_t px = bits[i];
    if ((px & 0x00FFFFFF) != 0 && (px & 0xFF000000) == 0)
    {
      bits[i] = px | 0xFF000000;
    }
  }

  SelectObject(hDC, hOldBmp);

  // Mask bitmap: all black = opaque (alpha channel handles transparency).
  HBITMAP hMask = CreateBitmap(S, S, 1, 1, NULL);
  {
    HDC hMaskDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hOldMask = (HBITMAP)SelectObject(hMaskDC, hMask);
    PatBlt(hMaskDC, 0, 0, S, S, BLACKNESS);
    SelectObject(hMaskDC, hOldMask);
    DeleteDC(hMaskDC);
  }

  ICONINFO ii = {};
  ii.fIcon = TRUE;
  ii.hbmMask = hMask;
  ii.hbmColor = hBmp;
  HICON hIcon = CreateIconIndirect(&ii);

  DeleteObject(hMask);
  DeleteObject(hBmp);
  DeleteDC(hDC);
  ReleaseDC(NULL, hScreenDC);

  return hIcon;
}

UINT CSoundKeeper::s_msg_taskbar_created = 0;

LRESULT CALLBACK CSoundKeeper::TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  CSoundKeeper *self = (CSoundKeeper *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

  if (s_msg_taskbar_created != 0 && msg == s_msg_taskbar_created)
  {
    if (self)
    {
      DebugLog("TaskbarCreated received. Re-adding tray icon.");
      self->EnsureTrayIcon(true);
    }
    return 0;
  }

  switch (msg)
  {
  case WM_TIMER:
    if (wParam == 1 && self)
    {
      self->EnsureTrayIcon(false);
    }
    return 0;

  case WM_TRAYICON:
    switch (LOWORD(lParam))
    {
    case WM_LBUTTONUP:
      if (self)
      {
        self->ToggleMute();
      }
      break;

    case WM_RBUTTONUP:
      if (self)
      {
        POINT pt;
        GetCursorPos(&pt);
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING | (self->m_is_muted ? MF_CHECKED : MF_UNCHECKED), IDM_TOGGLE_MUTE, L"\u9759\u97F3");
        AppendMenuW(hMenu, MF_STRING | (self->IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED), IDM_AUTOSTART, L"\u5F00\u673A\u81EA\u542F");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"\u9000\u51FA");

        BOOL menuRightAligned = FALSE;
        SystemParametersInfoW(SPI_GETMENUDROPALIGNMENT, 0, &menuRightAligned, 0);
        if (menuRightAligned)
        {
          SystemParametersInfoW(SPI_SETMENUDROPALIGNMENT, FALSE, NULL, 0);
        }

        SetForegroundWindow(hwnd);
        TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);

        if (menuRightAligned)
        {
          SystemParametersInfoW(SPI_SETMENUDROPALIGNMENT, TRUE, NULL, 0);
        }

        DestroyMenu(hMenu);
        PostMessage(hwnd, WM_NULL, 0, 0);
      }
      break;
    }
    return 0;

  case WM_COMMAND:
    switch (LOWORD(wParam))
    {
    case IDM_TOGGLE_MUTE:
      if (self)
      {
        self->ToggleMute();
      }
      break;
    case IDM_AUTOSTART:
      if (self)
      {
        self->SetAutoStart(!self->IsAutoStartEnabled());
      }
      break;
    case IDM_EXIT:
      if (self)
      {
        self->FireShutdown();
      }
      break;
    }
    return 0;

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Auto-start on boot via HKCU Run registry key.

static const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *kRunValue = L"SoundKeeper";

bool CSoundKeeper::IsAutoStartEnabled() const
{
  HKEY hKey = NULL;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
    return false;

  // Get current exe path.
  wchar_t exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);

  wchar_t regVal[MAX_PATH * 2] = {};
  DWORD cbData = sizeof(regVal);
  DWORD type = 0;
  bool enabled = false;
  if (RegQueryValueExW(hKey, kRunValue, NULL, &type, (LPBYTE)regVal, &cbData) == ERROR_SUCCESS && type == REG_SZ)
  {
    // Check if the stored path matches the current exe (quoted or unquoted).
    wchar_t quoted[MAX_PATH + 4];
    wsprintfW(quoted, L"\"%s\"", exePath);
    enabled = (_wcsicmp(regVal, exePath) == 0 || _wcsicmp(regVal, quoted) == 0);
  }

  RegCloseKey(hKey);
  return enabled;
}

void CSoundKeeper::SetAutoStart(bool enable)
{
  HKEY hKey = NULL;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
  {
    DebugLogError("SetAutoStart: failed to open Run registry key.");
    return;
  }

  if (enable)
  {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t quoted[MAX_PATH + 4];
    wsprintfW(quoted, L"\"%s\"", exePath);
    RegSetValueExW(hKey, kRunValue, 0, REG_SZ,
                   (const BYTE *)quoted, (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    DebugLog("Auto-start enabled.");
  }
  else
  {
    RegDeleteValueW(hKey, kRunValue);
    DebugLog("Auto-start disabled.");
  }

  RegCloseKey(hKey);
}

void CSoundKeeper::EnsureTrayIcon(bool forceReadd)
{
  if (!m_tray_hwnd)
    return;

  m_nid.hIcon = m_is_muted ? m_icon_muted : m_icon_normal;
  wcscpy_s(m_nid.szTip, m_is_muted ? L"Sound Keeper - \u5DF2\u9759\u97F3" : L"Sound Keeper - \u64AD\u653E\u4E2D");
  m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  m_nid.szInfo[0] = L'\0';
  m_nid.szInfoTitle[0] = L'\0';

  HWND hShellTray = FindWindowW(L"Shell_TrayWnd", NULL);
  if (!hShellTray)
  {
    m_tray_icon_added = false;
    m_last_shell_tray_hwnd = NULL;
    m_tray_check_count = 0;
    SetTimer(m_tray_hwnd, 1, 1000, NULL);
    return;
  }

  if (!forceReadd && m_tray_icon_added && hShellTray == m_last_shell_tray_hwnd)
  {
    if (Shell_NotifyIconW(NIM_MODIFY, &m_nid))
    {
      if (m_tray_check_count < 10)
      {
        m_tray_check_count++;
        if (m_tray_check_count >= 10)
        {
          SetTimer(m_tray_hwnd, 1, 30000, NULL);
        }
        else
        {
          SetTimer(m_tray_hwnd, 1, 3000, NULL);
        }
      }
      return;
    }
    m_tray_icon_added = false;
    m_tray_check_count = 0;
  }

  m_tray_check_count = 0;
  Shell_NotifyIconW(NIM_DELETE, &m_nid);
  if (Shell_NotifyIconW(NIM_ADD, &m_nid))
  {
    m_tray_icon_added = true;
    m_last_shell_tray_hwnd = hShellTray;
    SetTimer(m_tray_hwnd, 1, 3000, NULL);
    DebugLog("Tray icon added successfully.");
  }
  else
  {
    m_tray_icon_added = false;
    SetTimer(m_tray_hwnd, 1, 1000, NULL);
    DebugLogWarning("Shell_NotifyIconW(NIM_ADD) failed, scheduled retry in 1s.");
  }
}

bool CSoundKeeper::InitTrayIcon(HINSTANCE hInstance)
{
  s_msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

  // Enable dark mode and modern rounded menu styling via uxtheme.dll ordinals 135 & 136.
  if (HMODULE hUxTheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32))
  {
    using fnSetPreferredAppMode = int(WINAPI *)(int);
    using fnFlushMenuThemes = void(WINAPI *)();
    if (auto pSetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135)))
    {
      pSetPreferredAppMode(2); // AllowDark
    }
    if (auto pFlushMenuThemes = (fnFlushMenuThemes)GetProcAddress(hUxTheme, MAKEINTRESOURCEA(136)))
    {
      pFlushMenuThemes();
    }
  }

  // Create icons.
  m_icon_normal = CreateSpeakerIcon(false);
  m_icon_muted = CreateSpeakerIcon(true);

  if (!m_icon_normal || !m_icon_muted)
  {
    DebugLogError("Failed to create tray icons. GetLastError=%u", GetLastError());
    return false;
  }

  // Register window class.
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = TrayWndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = L"SoundKeeperTrayClass";
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
  {
    DebugLogError("Failed to register tray window class. GetLastError=%u", GetLastError());
    return false;
  }

  // Create a hidden top-level window (NOT HWND_MESSAGE, which cannot receive TaskbarCreated broadcasts).
  m_tray_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"SoundKeeperTrayClass", L"Sound Keeper", WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, hInstance, NULL);
  if (!m_tray_hwnd)
  {
    DebugLogError("Failed to create tray window. GetLastError=%u", GetLastError());
    return false;
  }

  if (s_msg_taskbar_created != 0)
  {
    ChangeWindowMessageFilterEx(m_tray_hwnd, s_msg_taskbar_created, MSGFLT_ALLOW, NULL);
  }

  // Store the this pointer for use in WndProc.
  SetWindowLongPtr(m_tray_hwnd, GWLP_USERDATA, (LONG_PTR)this);

  // Set up tray icon structure and add it (with automatic retry if Explorer tray is not ready yet).
  m_nid = {};
  m_nid.cbSize = sizeof(NOTIFYICONDATAW);
  m_nid.hWnd = m_tray_hwnd;
  m_nid.uID = 1;
  m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  m_nid.uCallbackMessage = WM_TRAYICON;

  this->EnsureTrayIcon(true);

  DebugLog("Tray icon initialized.");
  return true;
}

void CSoundKeeper::RemoveTrayIcon()
{
  if (m_tray_hwnd)
  {
    KillTimer(m_tray_hwnd, 1);
  }

  Shell_NotifyIconW(NIM_DELETE, &m_nid);
  m_tray_icon_added = false;

  if (m_tray_hwnd)
  {
    DestroyWindow(m_tray_hwnd);
    m_tray_hwnd = NULL;
  }

  if (m_icon_normal)
  {
    DestroyIcon(m_icon_normal);
    m_icon_normal = NULL;
  }
  if (m_icon_muted)
  {
    DestroyIcon(m_icon_muted);
    m_icon_muted = NULL;
  }

  DebugLog("Tray icon removed.");
}

void CSoundKeeper::UpdateTrayIcon()
{
  m_nid.hIcon = m_is_muted ? m_icon_muted : m_icon_normal;
  wcscpy_s(m_nid.szTip, m_is_muted ? L"Sound Keeper - \u5DF2\u9759\u97F3" : L"Sound Keeper - \u64AD\u653E\u4E2D");

  // Show a balloon notification so the user knows the state changed.
  m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
  wcscpy_s(m_nid.szInfoTitle, L"Sound Keeper");
  wcscpy_s(m_nid.szInfo, m_is_muted ? L"\u5DF2\u9759\u97F3 \xD83D\xDD07" : L"\u64AD\u653E\u4E2D \xD83D\xDD0A");
  m_nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
  m_nid.uTimeout = 1500;

  if (!Shell_NotifyIconW(NIM_MODIFY, &m_nid))
  {
    m_tray_icon_added = false;
    this->EnsureTrayIcon(true);
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
  }

  // Clear the balloon flag so it doesn't re-show on future NIM_MODIFY calls.
  m_nid.uFlags &= ~NIF_INFO;
  m_nid.szInfo[0] = L'\0';
  m_nid.szInfoTitle[0] = L'\0';
}

// Entry point methods.

void CSoundKeeper::SetStreamTypeDefaults(KeepStreamType stream_type)
{
  m_cfg_stream_type = stream_type;

  m_cfg_frequency = 0.0;
  m_cfg_amplitude = 0.0;
  m_cfg_play_seconds = 0.0;
  m_cfg_wait_seconds = 0.0;
  m_cfg_fade_seconds = 0.0;

  switch (stream_type)
  {
  case KeepStreamType::Fluctuate:
    m_cfg_frequency = 50.0;
    break;
  case KeepStreamType::Sine:
    m_cfg_frequency = 1.0;
    [[fallthrough]];
  case KeepStreamType::WhiteNoise:
  case KeepStreamType::BrownNoise:
  case KeepStreamType::PinkNoise:
    m_cfg_amplitude = 0.01;
    m_cfg_fade_seconds = 0.1;
    [[fallthrough]];
  default:
    break;
  }
}

void CSoundKeeper::ParseStreamArgs(KeepStreamType stream_type, const char *args)
{
  this->SetStreamTypeDefaults(stream_type);

  char *p = (char *)args;
  while (*p)
  {
    if (*p == ' ' || *p == '\t' || *p == '-')
    {
      p++;
    }
    else if (*p == 'f' || *p == 'a' || *p == 'l' || *p == 'w' || *p == 't')
    {
      char type = *p;
      p++;
      while (*p == ' ' || *p == '\t' || *p == '=')
      {
        p++;
      }
      if (*p < '0' || '9' < *p)
      {
        break;
      }

      double value = strtod(p, &p);
      if (type == 'f')
      {
        this->SetFrequency(std::min(value, 96000.0));
      }
      else if (type == 'a')
      {
        this->SetAmplitude(std::min(value / 100.0, 1.0));
      }
      else if (type == 'l')
      {
        this->SetPeriodicPlaying(value);
      }
      else if (type == 'w')
      {
        this->SetPeriodicWaiting(value);
      }
      else if (type == 't')
      {
        this->SetFading(value);
      }
    }
    else
    {
      break;
    }
  }
}

void CSoundKeeper::ParseModeString(const char *args)
{
  char buf[MAX_PATH];
  strcpy_s(buf, args);
  _strlwr(buf);

  if (strstr(buf, "all"))
  {
    this->SetDeviceType(KeepDeviceType::All);
  }
  if (strstr(buf, "analog"))
  {
    this->SetDeviceType(KeepDeviceType::Analog);
  }
  if (strstr(buf, "digital"))
  {
    this->SetDeviceType(KeepDeviceType::Digital);
  }
  if (strstr(buf, "kill"))
  {
    this->SetDeviceType(KeepDeviceType::None);
  }
  if (strstr(buf, "remote"))
  {
    this->SetAllowRemote(true);
  }
  if (strstr(buf, "nosleep"))
  {
    this->SetNoSleep(true);
  }

  if (strstr(buf, "openonly"))
  {
    this->SetStreamType(KeepStreamType::None);
  }
  else if (strstr(buf, "zero") || strstr(buf, "null"))
  {
    this->SetStreamType(KeepStreamType::Zero);
  }
  else if (char *p = strstr(buf, "fluctuate"))
  {
    this->ParseStreamArgs(KeepStreamType::Fluctuate, p + 9);
  }
  else if (char *p = strstr(buf, "sine"))
  {
    this->ParseStreamArgs(KeepStreamType::Sine, p + 4);
  }
  else if (char *p = strstr(buf, "white"))
  {
    this->ParseStreamArgs(KeepStreamType::WhiteNoise, p + 5);
  }
  else if (char *p = strstr(buf, "brown"))
  {
    this->ParseStreamArgs(KeepStreamType::BrownNoise, p + 5);
  }
  else if (char *p = strstr(buf, "pink"))
  {
    this->ParseStreamArgs(KeepStreamType::PinkNoise, p + 4);
  }
}

HRESULT CSoundKeeper::Run()
{
  // Windows 8-10 audio service leaks handles and shared memory when exclusive mode is used, enable a workaround.
  uint32_t nt_build_number = GetNtBuildNumber();
  bool is_leaky_wasapi = 7601 < nt_build_number && nt_build_number < 22000;
  DebugLog("Windows Build Number: %u%s.", nt_build_number, is_leaky_wasapi ? " (leaky WASAPI)" : "");
  CSoundSession::EnableWaitExclusiveWorkaround(is_leaky_wasapi);

  // Set defaults.
  this->SetDeviceType(KeepDeviceType::Primary);
  this->SetStreamTypeDefaults(KeepStreamType::Fluctuate);

  // Parse file name for defaults.
  char fn_buffer[MAX_PATH];
  DWORD fn_size = GetModuleFileNameA(NULL, fn_buffer, MAX_PATH);
  if (fn_size != 0 && fn_size != MAX_PATH)
  {
    char *filename = strrchr(fn_buffer, '\\');
    if (filename)
    {
      filename++;
      DebugLog("Exe File Name: %s.", filename);
      this->ParseModeString(filename);
    }
  }

  // Parse command line for arguments.
  if (const char *cmdln = GetCommandLineA())
  {
    // Skip program file name.
    while (*cmdln == ' ')
    {
      cmdln++;
    }
    if (cmdln[0] == '"')
    {
      cmdln++;
      while (*cmdln != '"' && *cmdln != 0)
      {
        cmdln++;
      }
      if (*cmdln == '"')
      {
        cmdln++;
      }
    }
    else
    {
      while (*cmdln != ' ' && *cmdln != 0)
      {
        cmdln++;
      }
    }
    while (*cmdln == ' ')
    {
      cmdln++;
    }

    if (*cmdln != 0)
    {
      DebugLog("Command Line: %s.", cmdln);
      this->ParseModeString(cmdln);
    }
  }

  if (GetSecondsToSleeping() == 0)
  {
    DebugLogWarning("Sleep timer informarion is not available. Sleep detection is disabled.");
    m_cfg_no_sleep = true;
  }

#ifdef _CONSOLE

  switch (this->GetDeviceType())
  {
  case KeepDeviceType::None:
    DebugLog("Device Type: None.");
    break;
  case KeepDeviceType::Primary:
    DebugLog("Device Type: Primary.");
    break;
  case KeepDeviceType::All:
    DebugLog("Device Type: All.");
    break;
  case KeepDeviceType::Analog:
    DebugLog("Device Type: Analog.");
    break;
  case KeepDeviceType::Digital:
    DebugLog("Device Type: Digital.");
    break;
  default:
    DebugLogError("Unknown Device Type.");
    break;
  }

  switch (this->GetStreamType())
  {
  case KeepStreamType::None:
    DebugLog("Stream Type: None (Open Only).");
    break;
  case KeepStreamType::Zero:
    DebugLog("Stream Type: Zero.");
    break;
  case KeepStreamType::Fluctuate:
    DebugLog("Stream Type: Fluctuate (Frequency: %.3fHz).", this->GetFrequency());
    break;
  case KeepStreamType::Sine:
    DebugLog("Stream Type: Sine (Frequency: %.3fHz; Amplitude: %.3f%%; Fading: %.3fs).", this->GetFrequency(), this->GetAmplitude() * 100.0, this->GetFading());
    break;
  case KeepStreamType::WhiteNoise:
    DebugLog("Stream Type: White Noise (Amplitude: %.3f%%; Fading: %.3fs).", this->GetAmplitude() * 100.0, this->GetFading());
    break;
  case KeepStreamType::BrownNoise:
    DebugLog("Stream Type: Brown Noise (Amplitude: %.3f%%; Fading: %.3fs).", this->GetAmplitude() * 100.0, this->GetFading());
    break;
  case KeepStreamType::PinkNoise:
    DebugLog("Stream Type: Pink Noise (Amplitude: %.3f%%; Fading: %.3fs).", this->GetAmplitude() * 100.0, this->GetFading());
    break;
  default:
    DebugLogError("Unknown Stream Type.");
    break;
  }

  if (this->GetPeriodicPlaying() || this->GetPeriodicWaiting())
  {
    DebugLog("Periodicity: Enabled (Length: %.3fs; Waiting: %.3fs).", this->GetPeriodicPlaying(), this->GetPeriodicWaiting());
  }
  else
  {
    DebugLog("Periodicity: Disabled.");
  }

  if (m_cfg_no_sleep)
  {
    DebugLog("Sleep Detection: Disabled.");
  }

#endif

  // Stop another instance.

  Handle global_stop_event = CreateEventA(NULL, TRUE, FALSE, "SoundKeeperStopEvent");
  if (!global_stop_event)
  {
    DWORD le = GetLastError();
    DebugLogError("Unable to open global stop event. Error code: %d.", le);
    return HRESULT_FROM_WIN32(le);
  }

  Handle global_mutex = CreateMutexA(NULL, TRUE, "SoundKeeperMutex");
  if (!global_mutex)
  {
    DWORD le = GetLastError();
    DebugLogError("Unable to open global mutex. Error code: %d.", le);
    return HRESULT_FROM_WIN32(le);
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS)
  {
    DebugLog("Stopping another instance...");
    SetEvent(global_stop_event);
    DWORD wait_res = WaitForOne(global_mutex, 5000);
    ResetEvent(global_stop_event);
    if (wait_res != WAIT_OBJECT_0 && wait_res != WAIT_ABANDONED)
    {
      DebugLogError("Time out.");
      return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
  }
  defer[&] { ReleaseMutex(global_mutex); };

  HRESULT hr = S_OK;

  if (m_cfg_device_type == KeepDeviceType::None)
  {
    DebugLog("Self kill mode is enabled. Exit.");
    return hr;
  }

  // Register for automatic restart if the process crashes unexpectedly.
  RegisterApplicationRestart(NULL, 8 /* RESTART_NO_PATCH | RESTART_NO_REBOOT */);

  // Initialization.

  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_dev_enumerator));
  if (FAILED(hr))
  {
    DebugLogError("Unable to instantiate device enumerator: 0x%08X.", hr);
    return hr;
  }
  defer[&] { m_dev_enumerator->Release(); };

  hr = m_dev_enumerator->RegisterEndpointNotificationCallback(this);
  if (FAILED(hr))
  {
    DebugLogError("Unable to register for stream switch notifications: 0x%08X.", hr);
    return hr;
  }
  defer[&] { m_dev_enumerator->UnregisterEndpointNotificationCallback(this); };

  // Restore saved mute state.
  m_is_muted = this->LoadMuteState();
  DebugLog("Initial mute state: %s.", m_is_muted ? "Muted" : "Unmuted");

  // Initialize tray icon.
  {
    HINSTANCE hInst = GetModuleHandle(NULL);
    if (!this->InitTrayIcon(hInst))
    {
      DebugLogError("Failed to initialize tray icon.");
    }
  }
  defer[&] { this->RemoveTrayIcon(); };

  // Working loop.

  DebugLog("Enter main loop.");

  for (bool working = true; working;)
  {
    {
      MSG msg;
      while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
      {
        if (msg.message == WM_QUIT)
        {
          working = false;
          break;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
      if (!working)
      {
        break;
      }
    }

    uint32_t seconds_to_sleeping = (m_cfg_no_sleep ? -1 : GetSecondsToSleeping());

    if (m_is_muted)
    {
      // When muted, keep sessions stopped.
      if (m_is_started)
      {
        this->Stop();
      }
    }
    else if (seconds_to_sleeping == 0)
    {
      if (m_is_started)
      {
        DebugLog("Going to sleep...");
        this->Stop();
      }
    }
    else
    {
      if (!m_is_started)
      {
        DebugLog("Starting...");
        this->Start();
      }
      else if (m_is_retry_required)
      {
        DebugLog("Retrying...");
        this->Retry();
      }
    }

    DWORD timeout = (m_is_retry_required || seconds_to_sleeping <= 30) ? 500 : (m_cfg_no_sleep ? INFINITE : 5000);

    HANDLE wait_handles[] = {m_do_retry, m_do_restart, m_do_shutdown, global_stop_event};
    constexpr DWORD handle_count = 4;

    // Use MsgWaitForMultipleObjectsEx with MWMO_INPUTAVAILABLE so unread messages never stall the wait.
    DWORD wait_result = MsgWaitForMultipleObjectsEx(handle_count, wait_handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    switch (wait_result)
    {
    case WAIT_TIMEOUT:

      break;

    case WAIT_OBJECT_0 + 0:

      // Prevent multiple retries.
      while (WaitForOne(m_do_retry, 500) != WAIT_TIMEOUT)
        ;
      if (!m_is_muted)
      {
        m_is_retry_required = true;
      }
      break;

    case WAIT_OBJECT_0 + 1:

      // Prevent multiple restarts.
      while (WaitForOne(m_do_restart, 500) != WAIT_TIMEOUT)
        ;
      if (!m_is_muted)
      {
        DebugLog("Restarting...");
        this->Restart();
      }
      break;

    case WAIT_OBJECT_0 + 2:
    case WAIT_OBJECT_0 + 3:

      // We're done, exit the loop.
      DebugLog("Shutdown.");
      working = false;
      break;

    case WAIT_OBJECT_0 + handle_count:
      // Process Windows messages for tray icon.
      {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
          if (msg.message == WM_QUIT)
          {
            working = false;
            break;
          }
          TranslateMessage(&msg);
          DispatchMessage(&msg);
        }
      }
      break;

    default:

      // Unexpected result, exit.
      DebugLog("Shutdown (unexpected wait result).");
      working = false;
      break;
    }
  }

  DebugLog("Leave main loop.");
  Stop();
  return hr;
}

FORCEINLINE HRESULT CSoundKeeper::Main()
{
  DebugThreadName("Main");

  DebugLog("Enter main thread.");

  if (HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE); FAILED(hr))
  {
#ifndef _CONSOLE
    MessageBoxA(0, "Cannot initialize COM.", "Sound Keeper", MB_ICONERROR | MB_OK | MB_SYSTEMMODAL);
#else
    DebugLogError("Cannot initialize COM: 0x%08X.", hr);
#endif
    return hr;
  }

  CSoundKeeper *keeper = new CSoundKeeper();
  HRESULT hr = keeper->Run();
  SafeRelease(keeper);

  CoUninitialize();

#ifndef _CONSOLE
  if (FAILED(hr))
  {
    MessageBoxA(0, "Cannot initialize WASAPI.", "Sound Keeper", MB_ICONERROR | MB_OK | MB_SYSTEMMODAL);
  }
#else
  if (hr == S_OK)
  {
    DebugLog("Leave main thread. Exit code: 0.");
  }
  else
  {
    DebugLog("Leave main thread. Exit code: 0x%08X.", hr);
  }
#endif

  return hr;
}

#ifdef _CONSOLE

int main()
{
  if (const VS_FIXEDFILEINFO *ffi = GetFixedVersion())
  {
    DebugLog("Sound Keeper v%hu.%hu.%hu.%hu [%04hu/%02hu/%02hu] (" APP_ARCH ")",
             HIWORD(ffi->dwProductVersionMS),
             LOWORD(ffi->dwProductVersionMS),
             HIWORD(ffi->dwProductVersionLS),
             LOWORD(ffi->dwProductVersionLS),
             HIWORD(ffi->dwFileVersionMS),
             LOWORD(ffi->dwFileVersionMS),
             HIWORD(ffi->dwFileVersionLS),
             LOWORD(ffi->dwFileVersionLS));
  }

  return CSoundKeeper::Main();
}

#else

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ PWSTR pCmdLine, _In_ int nCmdShow)
{
  return CSoundKeeper::Main();
}

#endif
