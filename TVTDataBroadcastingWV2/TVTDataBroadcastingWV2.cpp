#include "pch.h"

#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "thirdparty/TVTestPlugin.h"
#include "resource.h"
#include <queue>
#include <vector>
#include <fstream>
#include <filesystem>
#include <optional>
#include <wil/stl.h>
#include <wil/win32_helpers.h>
#include "NVRAMSettingsDialog.h"
#include "proxy.h"
#include "InputDialog.h"
#include "OneSeg.h"
#include <shellapi.h>

using namespace Microsoft::WRL;

// #pragma comment(lib, "ntdll.lib")
// extern "C" ULONG DbgPrint(PCSTR Format, ...);

// メッセージウィンドウ向けメッセージ
#define WM_APP_PACKET (WM_APP + 0)
#define WM_APP_RESIZE (WM_APP + 1)
#define WM_APP_RESPONSE (WM_APP + 2)
#define WM_APP_INPUT (WM_APP + 3)
#define WM_APP_ENABLE_PLUGIN (WM_APP + 4)
// バウンダリ外への文書遷移を検出したので、ハイブリッドキャストアプリを終了する(wParam: 放送受信映像のみにするか)
#define WM_APP_HYBRIDCAST_EXIT (WM_APP + 5)

// サイドパネル向けメッセージ
#define WM_APP_ON_PANEL_COLOR_CHANGE (WM_APP + 0)
#define WM_APP_ON_PANEL_SIZE (WM_APP + 1)

struct DeferralResponse
{
    wil::com_ptr<ICoreWebView2Deferral> deferral;
    wil::com_ptr<ICoreWebView2WebResourceRequestedEventArgs> args;
    DWORD statusCode;
    std::wstring statusCodeText;
    std::wstring headers;
    std::vector<BYTE> content;
};

#define IDT_SHOW_EVR_WINDOW 1
#define IDT_RESIZE 2

struct UsedKey
{
    bool basic;
    bool dataButton;
    bool numericTuning;
    bool special1;
    bool special2;
};

struct Status
{
    std::wstring url;
    bool receiving = false;
    bool loading = false;
};

struct PacketQueue
{
private:
    std::mutex queueMutex;
    std::vector<BYTE> currentBlock;
    std::queue<std::vector<BYTE>> queue;
    std::atomic<bool> invalidate;
    std::unordered_set<WORD> pidsToExclude;
    std::unordered_map<WORD, int> pcrPIDCandidates;
    int pcrPID = -1;
    DWORD pcr = 0;
    DWORD lastBlockPCR = 0;
public:
    static constexpr size_t packetSize = 188;
    static constexpr size_t packetBlockSize = packetSize * 500;
    static constexpr size_t maxQueueLength = 100;

    PacketQueue()
    {
        currentBlock.reserve(packetBlockSize);
    }

    // TSスレッドでのみ呼び出せる
    bool enqueuePacket(BYTE* packet)
    {
        if (this->invalidate.exchange(false))
        {
            this->currentBlock.clear();
        }
        // 8-bit sync byte
        // 1-bit TEI
        // 1-bit PUSI
        // 1-bit priority
        // 13-bit PID
        bool transportErrorIndicator = !!(packet[1] & 0x80);
        WORD pid = ((packet[1] << 8) | packet[2]) & 0x1fff;
        int adaptationFieldControl = (packet[3] >> 4) & 0x03;
        bool pcrFlag = false;
        if (!transportErrorIndicator && !!(adaptationFieldControl & 2))
        {
            int adaptationLength = packet[4];
            pcrFlag = !!(packet[5] & 0x10);
            if (adaptationLength >= 6 && pcrFlag)
            {
                // 参照するPCRのPIDを適当に選ぶ
                if (pid != this->pcrPID)
                {
                    auto it = this->pcrPIDCandidates.find(pid);
                    if (it == this->pcrPIDCandidates.end())
                    {
                        it = this->pcrPIDCandidates.emplace(pid, 0).first;
                    }
                    // 最初に3回出現するか、参照済みPCRが現れずに5回出現したPCRを使う
                    it->second++;
                    if ((this->pcrPID < 0 && it->second >= 3) || it->second >= 5)
                    {
                        this->pcrPID = pid;
                    }
                }
                if (pid == this->pcrPID)
                {
                    // PCRを取得する。時計演算の便利のため下位1bitは捨てる
                    this->pcrPIDCandidates.clear();
                    this->pcr = (static_cast<DWORD>(packet[6]) << 24) | (packet[7] << 16) | (packet[8] << 8) | packet[9];
                }
            }
        }

        bool acceptPacket;
        {
            // std::mutexの内部はSRWLock等なので頻繁に呼んでも別に気にしなくてよい
            std::lock_guard<std::mutex> lock(this->queueMutex);
            acceptPacket = this->pidsToExclude.count(pid) == 0;
        }
        if (acceptPacket)
        {
            this->currentBlock.insert(this->currentBlock.end(), packet, packet + this->packetSize);
        }
        else if (pcrFlag)
        {
            // 除外されているPIDにPCRが含まれていればPCRのみをキューに加える
            BYTE pcr_packet[packetSize] = {};
            pcr_packet[0] = 0x47;
            // adaptation_field_control=0b10なのでCIは加算されない、0固定で間に合わせる
            pcr_packet[1] = pid >> 8;
            pcr_packet[2] = pid & 0xff;
            pcr_packet[3] = 2 << 4;
            pcr_packet[4] = packetSize - 5; // adaptation_field_length
            pcr_packet[5] = 0x10; // PCR
            memcpy(pcr_packet + 6, packet + 6, 6);
            memset(pcr_packet + 12, 0xff, sizeof(pcr_packet) - 12);
            this->currentBlock.insert(this->currentBlock.end(), pcr_packet, pcr_packet + sizeof(pcr_packet));
        }

        // PCRが100ミリ秒以上進めばキューに加える
        // キューには100*maxQueueLengthミリ秒分ほど貯められる
        if (this->currentBlock.size() >= this->packetBlockSize ||
            (!this->currentBlock.empty() && (this->pcr - this->lastBlockPCR) >= 45 * 100))
        {
            {
                std::lock_guard<std::mutex> lock(this->queueMutex);
                this->queue.push(std::move(this->currentBlock));
                if (this->queue.size() > this->maxQueueLength)
                {
                    // 古いものを中身再利用して捨てる
                    this->currentBlock.swap(this->queue.front());
                    this->queue.pop();
                }
            }
            this->currentBlock.clear();
            this->currentBlock.reserve(this->packetBlockSize);
            this->lastBlockPCR = this->pcr;
            return true;
        }
        return false;
    }

    // どのスレッドからも呼び出せる
    void clear()
    {
        std::lock_guard<std::mutex> lock(this->queueMutex);
        std::queue<std::vector<BYTE>>().swap(this->queue);
        this->invalidate = true;
    }

    // どのスレッドからも呼び出せる
    std::optional<std::vector<BYTE>> pop()
    {
        std::lock_guard<std::mutex> lock(this->queueMutex);
        if (this->queue.empty())
        {
            return std::nullopt;
        }
        auto r = std::move(this->queue.front());
        this->queue.pop();
        return r;
    }

    // どのスレッドからも呼び出せる
    void setPIDsToExclude(std::unordered_set<WORD> pids)
    {
        std::lock_guard<std::mutex> lock(this->queueMutex);
        this->pidsToExclude.swap(pids);
    }
};

struct Audio
{
    std::optional<BYTE> componentId;
    std::optional<int> index;
    std::optional<TVTest::DualMonoChannel> dualMonoChannel;

    Audio()
    {
    }

    Audio(BYTE componentId, TVTest::DualMonoChannel dualMonoChannel)
    {
        this->componentId = componentId;
        if (dualMonoChannel != TVTest::DUAL_MONO_CHANNEL_INVALID)
        {
            this->dualMonoChannel = dualMonoChannel;
        }
    }

    Audio(int index)
    {
        if (index != -1)
        {
            this->index = index;
        }
    }

    std::optional<int> getChannelId()
    {
        if (this->dualMonoChannel.has_value())
        {
            switch (this->dualMonoChannel.value())
            {
                // 第一チャンネル
            case TVTest::DUAL_MONO_CHANNEL_MAIN:
                return 1;
                // 第二チャンネル
            case TVTest::DUAL_MONO_CHANNEL_SUB:
                return 2;
                // 両方
            case TVTest::DUAL_MONO_CHANNEL_BOTH:
                return 3;
            }
        }
        return std::nullopt;
    }

    void setChannelId(int channelId)
    {
        switch (channelId)
        {
            // 第一チャンネル
        case 1:
            this->dualMonoChannel = TVTest::DUAL_MONO_CHANNEL_MAIN;
            break;
            // 第二チャンネル
        case 2:
            this->dualMonoChannel = TVTest::DUAL_MONO_CHANNEL_SUB;
            break;
            // 両方
        case 3:
            this->dualMonoChannel = TVTest::DUAL_MONO_CHANNEL_BOTH;
            break;
        default:
            this->dualMonoChannel = std::nullopt;
            break;
        }
    }
};

// Plugins/TVTDataBroadcastingWV2.tvtp
class CDataBroadcastingWV2 : public TVTest::CTVTestPlugin, TVTest::CTVTestEventHandler
{
    // Plugins/TVTDataBroadcastingWV2.ini
    std::wstring iniFile;
    // Plugins/TVTDataBroadcastingWV2/
    std::wstring baseDirectory;
    // Plugins/TVTDataBroadcastingWV2/resources/
    std::wstring resourceDirectory;
    // Plugins/TVTDataBroadcastingWV2/WebView2Data/
    std::wstring webView2DataDirectory;
    // ハイブリッドキャスト(HTMLアプリ)の実行状態
    struct HybridcastState
    {
        // マネージド状態(放送マネージドアプリを実行中)
        bool active = false;
        // アンマネージド状態(exitFromManagedState で一般アプリを閲覧中)
        bool unmanaged = false;
        // 起動処理中(スクリプト登録の完了待ち)
        bool launching = false;
        // アプリケーションバウンダリ(文書遷移してよいURL。前方一致、ディレクトリ境界あり)
        std::vector<std::wstring> boundary;
        // AddScriptToExecuteOnDocumentCreated の戻りID
        std::wstring scriptId;
        // 起動時のサービス。選局で別のサービスになったらアプリを終了する
        int networkId = 0;
        int serviceId = 0;
        // 試験用のAIT自動起動を済ませたサービス(networkId << 16 | serviceId)。0は未実施
        long long consumedKey = 0;
        // 「放送受信映像のみ」で戻るサービス(networkId << 16 | serviceId)。0は無し
        long long videoOnlyKey = 0;
        // アンマネージド中に映像・音声を止めるための退避
        bool savedMuteValid = false;
        bool savedMute = false;
        // 元のUser-Agent(アプリ実行中だけ仕様の書式にするため)
        std::wstring savedUserAgent;
        // resources/dist/hybridcast_api.js
        std::wstring apiScriptPath;
    } hybridcast;
    // ハイブリッドキャストのログ(Plugins\TVTDataBroadcastingWV2\hybridcast.log)
    std::mutex hybridcastLogMutex;
    bool hybridcastLogEnabled = true;
    bool hybridcastLogFullUrl = false;
    int hybridcastLogLines = 0;
    std::vector<wil::com_ptr<ICoreWebView2DevToolsProtocolEventReceiver>> hybridcastDevToolsReceivers;
    // Plugins/TVTDataBroadcastingWV2/WebView2/
    std::wstring webView2Directory;

    std::atomic<bool> webViewLoaded;
    PacketQueue packetQueue;
    std::vector<WCHAR> packetsToJsonBuf;

    HWND hRemoteWnd = nullptr;
    HWND hPanelWnd = nullptr;
    HWND hVideoWnd = nullptr;
    HWND hWebViewWnd = nullptr;
    HWND hViewWnd = nullptr;
    HWND hContainerWnd = nullptr;
    HWND hMessageWnd = nullptr;
    HWND hOneSegWnd = nullptr;
    HBRUSH hbrPanelBack = nullptr;
    HBRUSH hbrBRGYBacks[4] = {};
    HFONT hPanelFont = nullptr;
    UINT panelInitialDpi;
    std::vector<std::pair<HWND, RECT>> panelItems;
    wil::com_ptr<IBasicVideo> basicVideo;
    wil::com_ptr<IBaseFilter> vmr7Renderer;
    wil::com_ptr<IBaseFilter> vmr9Renderer;
    bool invisible = false;
    RECT videoRect = {};
    RECT containerRect = {};
    TVTest::ServiceInfo currentService = {};
    bool currentServiceIsOneSeg = false;
    TVTest::ChannelInfo currentChannel = {};
    Status status;
    bool deferWebView = false;
    const int MAX_VOLUME = 100;
    int currentVolume = MAX_VOLUME;
    bool useTVTestVolume = true;
    bool useTVTestChannelCommand = true;
    UsedKey usedKey;
    // iniのEnableNetworkが1であればProxySessionが初期化されenableNetworkもtrueになる
    // ただしenableNetworkの方は実行時にボタンで切り替えられる
    std::unique_ptr<ProxySession> proxySession;
    bool enableNetwork = true;
    std::unique_ptr<InputDialog> inputDialog;
    Audio mainAudio;
    bool isPlayingMainAudio = true;
    std::unique_ptr<OneSegWindow> oneSegWindow;
    bool oneSegWindowIsShown;
    virtual bool OnChannelChange();
    virtual bool OnServiceChange();
    virtual bool OnServiceUpdate();
    virtual bool OnCommand(int ID);
    virtual bool OnPluginEnable(bool fEnable);
    virtual void OnFilterGraphInitialized(TVTest::FilterGraphInfo* pInfo);
    virtual void OnFilterGraphFinalize(TVTest::FilterGraphInfo* pInfo);
    virtual bool OnStatusItemDraw(TVTest::StatusItemDrawInfo* pInfo);
    virtual bool OnFullscreenChange(bool fFullscreen);
    virtual bool OnPluginSettings(HWND hwndOwner);
    virtual bool OnColorChange();
    virtual bool OnPanelItemNotify(TVTest::PanelItemEventInfo* pInfo);
    virtual bool OnVolumeChange(int Volume, bool fMute);
    virtual bool OnAudioStreamChange(int Stream);
    virtual bool OnStereoModeChange(int StereoMode);
    virtual void OnAudioFormatChange();
    virtual void OnDarkModeChanged(bool fDarkMode);

    HWND GetFullscreenWindow();
    void RestoreVideoWindow();
    void ResizeVideoWindow();
    void Tune();
    bool InHybridcastMode() const { return this->hybridcast.active || this->hybridcast.unmanaged; }
    bool IsInHybridcastBoundary(const wchar_t* url) const;
    std::wstring BuildHybridcastScript(const std::string& initJson) const;
    std::wstring BuildHybridcastUserAgent(const std::wstring& baseUserAgent);
    void OnHybridcastAppMessage(const std::string& type, const nlohmann::json& message);
    void HybridcastLog(const wchar_t* level, const std::wstring& message);
    std::wstring HybridcastLogUrl(const std::wstring& url);
    void OnHybridcastNavigationStarting(ICoreWebView2NavigationStartingEventArgs* args, bool isMainFrame);
    void OnHybridcastDevToolsEvent(bool isException, const wchar_t* json);
    void LaunchHybridcast(const nlohmann::json& message);
    void ExitHybridcast(bool videoOnly);
    void EnterUnmanaged(const std::wstring& uri);
    void LeaveHybridcastState();
    void RestoreHybridcastMute();
    void InitWebView2();
    bool caption = false;
    void SetCaptionState(bool enable);
    void UpdateCaptionState(bool showIndicator);
    void UpdateVolume();
    std::wstring GetIniItem(const wchar_t* key, const wchar_t* def);
    INT GetIniItem(const wchar_t* key, INT def);
    bool SetIniItem(const wchar_t* key, const wchar_t* data);
    void Disable(bool finalize);
    void EnablePanelButtons(bool enable);
    HRESULT Proxy(ICoreWebView2WebResourceRequestedEventArgs* args, LPCWSTR proxyUrl);
    void UpdateNetworkToggleButton(HWND hWnd);
    void SetNetworkState(bool enable);
    void UpdateNetworkState();
    void UpdateAudioStream();
    void RestoreMainAudio();
    void SelectAudio(Audio audio);
    Audio GetSelectedAudio();
    void ShowRemoteControlDialog();
    void CreateMessageWindow();
    void CreateOneSegWindow();
    void DestroyOneSegWindow();
    void UpdateDigitButton();
    void SetPanelFont();

    wil::com_ptr<ICoreWebView2Controller> webViewController;
    wil::com_ptr<ICoreWebView2> webView;

    static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void* pClientData);
    static INT_PTR CALLBACK RemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    static INT_PTR CALLBACK PanelRemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData);
    static BOOL CALLBACK StreamCallback(BYTE* pData, void* pClientData);
    static LRESULT CALLBACK MessageWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static BOOL CALLBACK WindowMessageCallback(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* pResult, void* pUserData);

public:
    virtual bool GetPluginInfo(TVTest::PluginInfo* pInfo);
    virtual bool Initialize();
    virtual bool Finalize();
};

std::string wstrToUTF8String(const wchar_t* ws)
{
    auto size = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &result[0], size, nullptr, nullptr);
    result.resize(size - 1);
    return result;
}

std::wstring utf8StrToWString(const char* s)
{
    auto size = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &result[0], size);
    result.resize(size - 1);
    return result;
}

bool CDataBroadcastingWV2::GetPluginInfo(TVTest::PluginInfo* pInfo)
{
    pInfo->Type = TVTest::PLUGIN_TYPE_NORMAL;
    pInfo->Flags = TVTest::PLUGIN_FLAG_DISABLEONSTART | TVTest::PLUGIN_FLAG_HASSETTINGS;
    pInfo->pszPluginName = L"TVTDataBroadcastingWV2";
    pInfo->pszCopyright = L"2022-2023 otya";
#ifdef TVTDATABROADCASTINGWV2_VERSION
    pInfo->pszDescription = L"データ放送を表示" TVTDATABROADCASTINGWV2_VERSION;
#else
    pInfo->pszDescription = L"データ放送を表示";
#endif
    return true;
}

BOOL CALLBACK CDataBroadcastingWV2::StreamCallback(BYTE* pData, void* pClientData)
{
    auto pThis = (CDataBroadcastingWV2*)pClientData;
    if (pThis->packetQueue.enqueuePacket(pData))
    {
        PostMessageW(pThis->hMessageWnd, WM_APP_PACKET, 0, 0);
    }
    return TRUE;
}

bool CDataBroadcastingWV2::OnServiceChange()
{
    return OnServiceUpdate();
}

bool CDataBroadcastingWV2::OnChannelChange()
{
    return OnServiceUpdate();
}

bool CDataBroadcastingWV2::OnServiceUpdate()
{
    int numServices;
    auto serviceIndex = this->m_pApp->GetService(&numServices);
    if (serviceIndex == -1)
    {
        return true;
    }
    auto lastNetworkID = this->currentChannel.NetworkID;
    auto lastServiceID = this->currentService.ServiceID;
    this->m_pApp->GetCurrentChannelInfo(&this->currentChannel);
    // 過去のTVTestには音声ストリームが4より多く含まれていた場合範囲外アクセスが発生する問題があったので適当なパディングを入れる
    struct
    {
        TVTest::ServiceInfo serviceInfo;
        WORD padding[128];
    } currentService = {};
    this->m_pApp->GetServiceInfo(serviceIndex, &currentService.serviceInfo);
    this->currentService = currentService.serviceInfo;
    std::unordered_set<WORD> pesPIDList;
    for (auto i = 0; i < numServices; i++)
    {
        struct
        {
            TVTest::ServiceInfo serviceInfo;
            WORD padding[128];
        } serviceInfo = {};
        if (this->m_pApp->GetServiceInfo(i, &serviceInfo.serviceInfo))
        {
            TVTest::ElementaryStreamInfoList videoESList = {};
            TVTest::ElementaryStreamInfoList audioESList = {};
            if (this->m_pApp->GetElementaryStreamInfoList(&videoESList, TVTest::ES_MEDIA_VIDEO, serviceInfo.serviceInfo.ServiceID))
            {
                for (auto i = 0; i < videoESList.ESCount; i++)
                {
                    pesPIDList.insert(videoESList.ESList[i].PID);
                }
                this->m_pApp->MemoryFree(videoESList.ESList);
            }
            if (this->m_pApp->GetElementaryStreamInfoList(&audioESList, TVTest::ES_MEDIA_AUDIO, serviceInfo.serviceInfo.ServiceID))
            {
                for (auto i = 0; i < audioESList.ESCount; i++)
                {
                    pesPIDList.insert(audioESList.ESList[i].PID);
                }
                this->m_pApp->MemoryFree(audioESList.ESList);
            }
            pesPIDList.insert(serviceInfo.serviceInfo.VideoPID);
            for (auto j = 0; j < serviceInfo.serviceInfo.NumAudioPIDs && j < _countof(serviceInfo.serviceInfo.AudioPID); j++)
            {
                pesPIDList.insert(serviceInfo.serviceInfo.AudioPID[j]);
            }
        }
    }
    // 動画、音声のPESは不要なので削っておく
    this->packetQueue.setPIDsToExclude(std::move(pesPIDList));

    if (this->currentChannel.NetworkID != lastNetworkID ||
        this->currentService.ServiceID != lastServiceID)
    {
        this->currentServiceIsOneSeg = false;
        this->packetQueue.clear();
    }
    Tune();
    return true;
}

void CDataBroadcastingWV2::RestoreVideoWindow()
{
    RECT r;
    if (GetClientRect(hContainerWnd, &r))
    {
        PostMessageW(hContainerWnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right - r.left, r.bottom - r.top));
    }
}

BOOL CALLBACK CDataBroadcastingWV2::WindowMessageCallback(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* pResult, void* pUserData)
{
    if (uMsg == WM_SIZE)
    {
        auto pThis = (CDataBroadcastingWV2*)pUserData;
        if (pThis->hContainerWnd && pThis->hMessageWnd)
        {
#if 0
            if (!pThis->invisible)
            {
                // 無理やり動画ウィンドウを移動させている都合上リサイズ時に位置大きさが初期化されてしまうので一時的に非表示にさせる
                SetWindowPos(pThis->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
            }
#endif
            // 実際に動画ウィンドウの大きさが変わるのはメッセージ処理後なのでPostMessageでやり過ごす
            PostMessageW(pThis->hMessageWnd, WM_APP_RESIZE, 0, 0);
        }
    }
    return FALSE;
}

std::wstring CDataBroadcastingWV2::GetIniItem(const wchar_t* key, const wchar_t* def)
{
    DWORD size = 100;
    std::wstring item(size, 0);
    while (true)
    {
        auto result = GetPrivateProfileStringW(L"TVTDataBroadcastingWV2", key, def, &item[0], size, this->iniFile.c_str());
        if (result + 1 != size)
        {
            item.resize(result);
            return item;
        }
        size *= 2;
        item.resize(size);
    }
}
INT CDataBroadcastingWV2::GetIniItem(const wchar_t* key, INT def)
{
    return GetPrivateProfileIntW(L"TVTDataBroadcastingWV2", key, def, this->iniFile.c_str());
}

bool CDataBroadcastingWV2::SetIniItem(const wchar_t* key, const wchar_t* data)
{
    return WritePrivateProfileStringW(L"TVTDataBroadcastingWV2", key, data, this->iniFile.c_str());
}

bool CDataBroadcastingWV2::Initialize()
{
    auto filename = wil::GetModuleFileNameW<std::wstring>(g_hinstDLL);
    std::filesystem::path path(filename);
    path.replace_extension();
    baseDirectory = path;
    resourceDirectory = path / L"resources";
    webView2DataDirectory = path / L"WebView2Data";
    webView2Directory = path / L"WebView2";
    path.replace_extension(L".ini");
    this->iniFile = path;
    // ハイブリッドキャストのログ。HybridcastLog=0 で無効、HybridcastLogFullUrl=1 でURLのクエリも記録する
    this->hybridcastLogEnabled = this->GetIniItem(L"HybridcastLog", 1) != 0;
    this->hybridcastLogFullUrl = this->GetIniItem(L"HybridcastLogFullUrl", 0) != 0;
    m_pApp->SetEventCallback(EventCallback, this);
    m_pApp->RegisterCommand(IDC_KEY_D, L"DataButton", L"dボタン");
    m_pApp->RegisterCommand(IDC_KEY_D_OR_ENABLE_PLUGIN, L"DataButtonOrEnablePlugin", L"プラグイン有効/dボタン");
    m_pApp->RegisterCommand(IDC_KEY_UP, L"Up", L"↑");
    m_pApp->RegisterCommand(IDC_KEY_DOWN, L"Down", L"↓");
    m_pApp->RegisterCommand(IDC_KEY_LEFT, L"Left", L"←");
    m_pApp->RegisterCommand(IDC_KEY_RIGHT, L"Right", L"→");
    m_pApp->RegisterCommand(IDC_KEY_0, L"Digit0", L"0");
    m_pApp->RegisterCommand(IDC_KEY_1, L"Digit1", L"1");
    m_pApp->RegisterCommand(IDC_KEY_2, L"Digit2", L"2");
    m_pApp->RegisterCommand(IDC_KEY_3, L"Digit3", L"3");
    m_pApp->RegisterCommand(IDC_KEY_4, L"Digit4", L"4");
    m_pApp->RegisterCommand(IDC_KEY_5, L"Digit5", L"5");
    m_pApp->RegisterCommand(IDC_KEY_6, L"Digit6", L"6");
    m_pApp->RegisterCommand(IDC_KEY_7, L"Digit7", L"7");
    m_pApp->RegisterCommand(IDC_KEY_8, L"Digit8", L"8");
    m_pApp->RegisterCommand(IDC_KEY_9, L"Digit9", L"9");
    m_pApp->RegisterCommand(IDC_KEY_10, L"Digit10", L"10");
    m_pApp->RegisterCommand(IDC_KEY_11, L"Digit11", L"11");
    m_pApp->RegisterCommand(IDC_KEY_12, L"Digit12", L"12");
    m_pApp->RegisterCommand(IDC_KEY_ENTER, L"Enter", L"決定");
    m_pApp->RegisterCommand(IDC_KEY_BACK, L"Back", L"戻る");
    m_pApp->RegisterCommand(IDC_KEY_BLUE, L"BlueButton", L"青");
    m_pApp->RegisterCommand(IDC_KEY_RED, L"RedButton", L"赤");
    m_pApp->RegisterCommand(IDC_KEY_GREEN, L"GreenButton", L"緑");
    m_pApp->RegisterCommand(IDC_KEY_YELLOW, L"YellowButton", L"黄");
    m_pApp->RegisterCommand(IDC_KEY_RELOAD, L"Reload", L"再読み込み");
    m_pApp->RegisterCommand(IDC_KEY_DEVTOOL, L"OpenDevTools", L"開発者ツール");
    m_pApp->RegisterCommand(IDC_ENABLE_CAPTION, L"EnableCaption", L"字幕表示");
    m_pApp->RegisterCommand(IDC_DISABLE_CAPTION, L"DisableCaption", L"字幕非表示");
    m_pApp->RegisterCommand(IDC_TOGGLE_CAPTION, L"ToggleCaption", L"字幕表示/非表示切替");
    m_pApp->RegisterCommand(IDC_SHOW_REMOTE_CONTROL, L"ShowRemoteControl", L"リモコン表示");
    m_pApp->RegisterCommand(IDC_TASKMANAGER, L"TaskManager", L"タスクマネージャー");
    m_pApp->RegisterPluginIconFromResource(g_hinstDLL, MAKEINTRESOURCEW(IDB_PLUGIN));
    TVTest::PanelItemInfo panel = {};
    panel.Size = sizeof(panel);
    panel.Style = TVTest::PANEL_ITEM_STYLE_NEEDFOCUS;
    panel.pszIDText = L"TVTDataBroadcastingWV2Panel";
    panel.pszTitle = L"データ放送";
    panel.ID = 1;
    panel.hbmIcon = (HBITMAP)LoadImageW(g_hinstDLL, MAKEINTRESOURCEW(IDB_PLUGIN), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    m_pApp->RegisterPanelItem(&panel);
    TVTest::StatusItemInfo statusItemInfo = {};
    statusItemInfo.Size = sizeof(statusItemInfo);
    statusItemInfo.ID = 1;
    statusItemInfo.Flags = 0;
    statusItemInfo.Style = TVTest::STATUS_ITEM_STYLE_VARIABLEWIDTH;
    statusItemInfo.pszIDText = L"DataBroadcastingStatus";
    statusItemInfo.pszName = L"データ放送ステータス";
    statusItemInfo.MinWidth = 0;
    statusItemInfo.MaxWidth = -1;
    statusItemInfo.DefaultWidth = TVTest::StatusItemWidthByFontSize(10);
    statusItemInfo.MinHeight = 0;
    m_pApp->RegisterStatusItem(&statusItemInfo);
    this->useTVTestVolume = this->GetIniItem(L"UseTVTestVolume", true);
    this->useTVTestChannelCommand = this->GetIniItem(L"UseTVTestChannelCommand", true);
    if (this->GetIniItem(L"AutoEnable", 0))
    {
        // Initialize()でプラグインを有効にするかPLUGIN_FLAG_ENABLEDEFAULTだとサイドパネルのプラグインボタンが押された状態にならないので遅延する
        this->CreateMessageWindow();
        PostMessageW(this->hMessageWnd, WM_APP_ENABLE_PLUGIN, 0, 0);
    }
    return true;
}

bool CDataBroadcastingWV2::Finalize()
{
    this->Disable(true);
    return true;
}

LRESULT CALLBACK CDataBroadcastingWV2::MessageWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    auto pThis = (CDataBroadcastingWV2*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (!pThis)
    {
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
    switch (uMsg)
    {
    case WM_TIMER:
    {
        switch (wParam)
        {
        case IDT_SHOW_EVR_WINDOW:
        {
            SetWindowPos(pThis->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            KillTimer(hWnd, wParam);
            break;
        }
        case IDT_RESIZE:
        {
            if (pThis->webViewController && !pThis->oneSegWindowIsShown)
            {
                // FIXME: Fullscreen
                // Containerウィンドウが非表示になった場合Viewウィンドウを親にする
                if (!IsWindowVisible(pThis->hWebViewWnd))
                {
                    pThis->webViewController->put_ParentWindow(pThis->hViewWnd);
                }
                // Containerウィンドウが表示されていてViewウィンドウが親ならばContainerウィンドウを親にする
                else if (IsWindowVisible(pThis->hContainerWnd))
                {
                    HWND parent;
                    if (SUCCEEDED(pThis->webViewController->get_ParentWindow(&parent)))
                    {
                        if (parent != pThis->hContainerWnd)
                        {
                            pThis->webViewController->put_ParentWindow(pThis->hContainerWnd);
                        }
                    }
                }
                RECT rect;
                if (GetClientRect(pThis->hContainerWnd, &rect))
                {
                    if (memcmp(&pThis->containerRect, &rect, sizeof(RECT)))
                    {
                        pThis->containerRect = rect;
                        pThis->webViewController->put_Bounds(rect);
                    }
                }
                if (!pThis->invisible)
                {
                    pThis->ResizeVideoWindow();
                }
            }
            break;
        }
        }
        break;
    }
    case WM_APP_RESIZE:
    {
        if (pThis->webViewController && !pThis->oneSegWindowIsShown)
        {
#if 0
            if (pThis->hVideoWnd)
            {
                SetTimer(pThis->hMessageWnd, IDT_SHOW_EVR_WINDOW, 50, nullptr);
            }
#endif
            RECT rect;
            if (GetClientRect(pThis->hContainerWnd, &rect))
            {
                pThis->containerRect = rect;
                RECT prev;
                if (SUCCEEDED(pThis->webViewController->get_Bounds(&prev)))
                {
                    if (memcmp(&prev, &rect, sizeof(prev)))
                    {
                        pThis->webViewController->put_Bounds(rect);
                    }
                }
            }
        }
        break;
    }
    case WM_APP_PACKET:
    {
        if (!pThis->webView || !pThis->webViewLoaded)
        {
            // キューを消費しない
            break;
        }
        // スレッドが失速して回復したときなどに応答を維持するためメッセージごとの処理数を制限
        for (int popCount = 0; popCount < 5; popCount++)
        {
            auto packets = pThis->packetQueue.pop();
            if (!packets)
            {
                break;
            }
            WCHAR head[] = LR"({"type":"streamBase64","data":")";
            WCHAR tail[] = LR"("})";
            auto packetBlockSize = packets.value().size();
            auto packetSize = pThis->packetQueue.packetSize;
            size_t size = _countof(head) - 1 + (packetBlockSize + 2) / 3 * 4 /* Base64 */ + _countof(tail) + 1;
            if (pThis->packetsToJsonBuf.size() < size)
            {
                pThis->packetsToJsonBuf.resize(size);
            }
            {
                auto buf = pThis->packetsToJsonBuf.data();
                wcscpy_s(buf, size, head);
                size_t pos = 0;
                pos += wcslen(head);
                auto buffer = packets.value().data();
                static const WCHAR base64[66] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
                for (size_t i = 0; i < packetBlockSize; i += 3)
                {
                    buf[pos] = base64[buffer[i] >> 2];
                    pos += 1;
                    buf[pos] = base64[((buffer[i] & 3) << 4) | (i + 1 < packetBlockSize ? buffer[i + 1] >> 4 : 0)];
                    pos += 1;
                    buf[pos] = base64[i + 1 < packetBlockSize ? ((buffer[i + 1] & 15) << 2) |
                                                                (i + 2 < packetBlockSize ? buffer[i + 2] >> 6 : 0) : 64];
                    pos += 1;
                    buf[pos] = base64[i + 2 < packetBlockSize ? buffer[i + 2] & 63 : 64];
                    pos += 1;
                }
                wcscpy_s(buf + pos, size - pos, tail);
                auto hr = pThis->webView->PostWebMessageAsJson(buf);
            }
        }
        break;
    }
    case WM_APP_RESPONSE:
    {
        auto response = std::unique_ptr<DeferralResponse>((DeferralResponse*)lParam);
        wil::com_ptr<ICoreWebView2Environment> env;
        auto hr = pThis->webView.query<ICoreWebView2_2>()->get_Environment(env.put());
        if (FAILED(hr))
        {
            response->deferral->Complete();
            break;
        }
        wil::com_ptr<ICoreWebView2WebResourceResponse> webResponse;
        wil::com_ptr<IStream> stm;
        auto  hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_NODISCARD, response->content.size());
        if (!hGlobal)
        {
            response->deferral->Complete();
            break;
        }
        auto mem = GlobalLock(hGlobal);
        memcpy(mem, response->content.data(), response->content.size());
        GlobalUnlock(hGlobal);
        if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, stm.put())))
        {
            response->deferral->Complete();
            break;
        }
        std::wistringstream iss(response->headers);
        std::wostringstream replacedHeader;
        std::wstring header;
        while (std::getline(iss, header))
        {
            if (!_wcsnicmp(header.c_str(), L"Access-Control-Allow-Origin", wcslen(L"Access-Control-Allow-Origin")))
            {
            }
            else if (header != L"\r")
            {
                replacedHeader << header;
                replacedHeader << L"\n";
            }
        }
        hr = env->CreateWebResourceResponse(stm.get(), response->statusCode, response->statusCodeText.c_str(), replacedHeader.str().c_str(), webResponse.put());
        if (FAILED(hr))
        {
            response->deferral->Complete();
            break;
        }
        wil::com_ptr<ICoreWebView2HttpResponseHeaders> responseHeaders;
        hr = webResponse->get_Headers(responseHeaders.put());
        if (FAILED(hr))
        {
            response->deferral->Complete();
            break;
        }
        responseHeaders->AppendHeader(L"Access-Control-Allow-Origin", L"https://tvtdatabroadcastingwv2.invalid");
        response->args->put_Response(webResponse.get());
        response->deferral->Complete();
        break;
    }
    case WM_APP_INPUT:
    {
        TVTest::ShowDialogInfo Info;

        Info.Flags = TVTest::SHOW_DIALOG_FLAG_MODELESS;
        Info.hinst = g_hinstDLL;
        Info.pszTemplate = MAKEINTRESOURCE(IDD_INPUT);
        Info.pMessageFunc = InputDialog::DlgProc;
        pThis->inputDialog = std::unique_ptr<InputDialog>((InputDialog*)lParam);
        Info.pClientData = pThis->inputDialog.get();
        Info.hwndOwner = pThis->m_pApp->GetFullscreen() ? pThis->GetFullscreenWindow() : pThis->m_pApp->GetAppWindow();

        auto hWnd = (HWND)pThis->m_pApp->ShowDialog(&Info);
        if (hWnd)
        {
            RECT dialogRect;
            RECT rect;
            if (GetWindowRect(hWnd, &dialogRect) && GetWindowRect(Info.hwndOwner, &rect))
            {
                // 中央に配置
                auto x = (rect.right + rect.left) / 2 - (dialogRect.right - dialogRect.left) / 2;
                auto y = (rect.bottom + rect.top) / 2 - (dialogRect.bottom - dialogRect.top) / 2;
                RECT moved = { x, y, x + dialogRect.right - dialogRect.left, y + dialogRect.bottom - dialogRect.top };
                // 範囲外の場合や作業領域からはみ出る場合は移動させない
                auto monitor = MonitorFromRect(&moved, MONITOR_DEFAULTTONULL);
                MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
                if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
                {
                    if (monitorInfo.rcWork.left <= moved.left && monitorInfo.rcWork.top <= moved.top && monitorInfo.rcWork.right >= moved.right && monitorInfo.rcWork.bottom >= moved.bottom)
                    {
                        SetWindowPos(hWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
                    }
                }
            }
            ShowWindow(hWnd, SW_SHOW);
        }
        break;
    }
    case WM_APP_ENABLE_PLUGIN:
        pThis->m_pApp->EnablePlugin(true);
        return 0;
    case WM_APP_HYBRIDCAST_EXIT:
        pThis->ExitHybridcast(wParam != 0);
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

HWND CDataBroadcastingWV2::GetFullscreenWindow()
{
    TVTest::HostInfo info;
    std::wstring appName(L"TVTest");
    if (this->m_pApp->GetHostInfo(&info))
    {
        appName = info.pszAppName;
    }
    struct Args
    {
        std::wstring fullscreenClass;
        HWND containerWnd;
    } args = { appName + L" Fullscreen" , nullptr };
    // フルスクリーンのとき
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hWnd, LPARAM lParam) -> BOOL {
        auto args = (Args*)lParam;
        WCHAR className[100];
        if (GetClassNameW(hWnd, className, _countof(className)))
        {
            if (!wcscmp(className, args->fullscreenClass.c_str()))
            {
                args->containerWnd = hWnd;
                return false;
            }
        }
        return true;
    }, (LPARAM)&args);
    return args.containerWnd;
}

void CDataBroadcastingWV2::OnFilterGraphInitialized(TVTest::FilterGraphInfo* pInfo)
{
    bool isRenderless = false;
    // VMR9
    if (SUCCEEDED(pInfo->pGraphBuilder->FindFilterByName(L"VMR9", this->vmr9Renderer.put())))
    {
        auto filterConfig = this->vmr9Renderer.query<IVMRFilterConfig9>();
        VMR9Mode mode;
        if (FAILED(filterConfig->GetRenderingMode((DWORD*)&mode)) || mode != VMR9Mode_Windowless)
        {
            // VMR9 (Renderless)
            // TVTest Video Container側の大きさを変えればいいけど字幕の位置も変わってしまう
            this->vmr9Renderer = nullptr;
            this->m_pApp->AddLog(L"VMR9 (Renderless)は非推奨", TVTest::LOG_TYPE_WARNING);
            isRenderless = true;
        }
    }
    // VMR7
    else if (SUCCEEDED(pInfo->pGraphBuilder->FindFilterByName(L"VMR7", this->vmr7Renderer.put())) || SUCCEEDED(pInfo->pGraphBuilder->FindFilterByName(L"VMR", this->vmr7Renderer.put())))
    {
        auto filterConfig = this->vmr7Renderer.try_query<IVMRFilterConfig>();
        VMRMode mode;
        if (filterConfig && (FAILED(filterConfig->GetRenderingMode((DWORD*)&mode)) || mode != VMRMode_Windowless))
        {
            // VMR7 (Renderless)
            // TVTest Video Container側の大きさを変えればいいけど字幕の位置も変わってしまう
            this->vmr7Renderer = nullptr;
            this->m_pApp->AddLog(L"VMR7 (Renderless)は非推奨", TVTest::LOG_TYPE_WARNING);
            isRenderless = true;
        }
    }
    // システムデフォルト
    else if (SUCCEEDED(pInfo->pGraphBuilder->QueryInterface(this->basicVideo.put())))
    {
        long l, t, w, h;
        if (FAILED(this->basicVideo->GetDestinationPosition(&l, &t, &w, &h)))
        {
            // EVR
            this->basicVideo = nullptr;
        }
    }
    std::vector<HWND> childWindows;
    TVTest::HostInfo info;
    std::wstring appName(L"TVTest");
    if (this->m_pApp->GetHostInfo(&info))
    {
        appName = info.pszAppName;
    }
    auto splitterClass = appName + L" Splitter";
    auto viewClass = appName + L" View";
    auto videoContainerClass = appName + L" Video Container";
    this->hViewWnd = FindWindowExW(FindWindowExW(this->m_pApp->GetAppWindow(), nullptr, splitterClass.c_str(), nullptr), nullptr, viewClass.c_str(), nullptr);
    this->hContainerWnd = FindWindowExW(this->hViewWnd, nullptr, videoContainerClass.c_str(), nullptr);
    if (!this->hContainerWnd)
    {
        auto fullscreenWnd = this->GetFullscreenWindow();
        this->hViewWnd = FindWindowExW(FindWindowExW(fullscreenWnd, nullptr, splitterClass.c_str(), nullptr), nullptr, viewClass.c_str(), nullptr);
        this->hContainerWnd = FindWindowExW(this->hViewWnd, nullptr, videoContainerClass.c_str(), nullptr);
    }
    // まず動画ウィンドウをクラス名で検索してみる
    // 0.10
    this->hVideoWnd = FindWindowExW(this->hContainerWnd, nullptr, L"LibISDB EVR Video Window", nullptr);
    if (this->hVideoWnd == nullptr)
    {
        // 0.9
        this->hVideoWnd = FindWindowExW(this->hContainerWnd, nullptr, L"Bon DTV EVR Video Window", nullptr);
    }
    if (this->hVideoWnd == nullptr)
    {
        this->hVideoWnd = FindWindowExW(this->hContainerWnd, nullptr, L"madVR", nullptr);
    }
    // 見つからなければNotification BarでもなくPseudo OSDでもないウィンドウを動画ウィンドウとする
    if (this->hVideoWnd == nullptr)
    {
        auto notifBarClass = appName + L" Notification Bar";
        auto pseudoOSDClass = appName + L" Pseudo OSD";
        EnumChildWindows(this->hContainerWnd, [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto childWindows = (std::vector<HWND>*)lParam;
            childWindows->push_back(hWnd);
            return true;
        }, (LPARAM)&childWindows);
        for (auto it = std::rbegin(childWindows); it != std::rend(childWindows); ++it)
        {
            auto hWnd = *it;
            if (GetParent(hWnd) != this->hContainerWnd)
            {
                continue;
            }
            WCHAR className[100];
            if (GetClassNameW(hWnd, className, _countof(className)))
            {
                if (className == notifBarClass || className == pseudoOSDClass)
                {
                    continue;
                }
                if (hWnd == hWebViewWnd)
                {
                    continue;
                }
                this->hVideoWnd = hWnd;
            }
        }
    }
    if (isRenderless)
    {
        this->hVideoWnd = this->hContainerWnd;
        this->hContainerWnd = GetParent(this->hContainerWnd);
    }
    if (this->deferWebView)
    {
        this->deferWebView = false;
        this->InitWebView2();
    }
    if (this->hVideoWnd)
    {
        // madVR EVR EVR (Custom Renderer)
        SetWindowPos(this->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (this->basicVideo)
    {
        this->hVideoWnd = nullptr;
    }
    this->ResizeVideoWindow();
}

void CDataBroadcastingWV2::OnFilterGraphFinalize(TVTest::FilterGraphInfo* pInfo)
{
    this->basicVideo = nullptr;
    this->vmr7Renderer = nullptr;
    this->vmr9Renderer = nullptr;
    this->hVideoWnd = nullptr;
}

void CDataBroadcastingWV2::InitWebView2()
{
    if (!this->hContainerWnd)
    {
        this->deferWebView = true;
        return;
    }
    LPCWSTR webView2Directory = nullptr;
    if (std::filesystem::is_directory(std::filesystem::path(this->webView2Directory) / L"EBWebView"))
    {
        webView2Directory = this->webView2Directory.c_str();
    }
    auto resourceDirectory = this->GetIniItem(L"ResourceDirectory", this->resourceDirectory.c_str());
    if (!std::filesystem::is_directory(resourceDirectory))
    {
        MessageBoxW(this->m_pApp->GetAppWindow(), (L"リソースディレクトリが見つかりません。\n" + resourceDirectory).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
        this->m_pApp->EnablePlugin(false);
        return;
    }
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(
        L"--autoplay-policy=no-user-gesture-required "
        L"--disable-features=msSmartScreenProtection "
        // IWebView2Controller3::put_RasterizationScaleに1.0を指定すればいいはずだけどリサイズがうまくいかなかったり挙動が謎なのでコマンドラインを使う
        // 副作用として開発者ツールやタスクマネージャーのウィンドウ枠含めたスケーリングが100%固定になる
        L"--force-device-scale-factor=1"
    );
    auto result = CreateCoreWebView2EnvironmentWithOptions(webView2Directory, this->webView2DataDirectory.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, resourceDirectory](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
        if (!env)
        {
            wchar_t buf[9]{};
            _itow_s(result, buf, 16);
            MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
            this->m_pApp->EnablePlugin(false);
            return S_OK;
        }
        wil::unique_cotaskmem_string ver;
        if (SUCCEEDED(env->get_BrowserVersionString(ver.put())))
        {
            this->m_pApp->AddLog((std::wstring(L"WebView2 version: ") + ver.get()).c_str(), TVTest::LOG_TYPE_INFORMATION);
        }
        env->CreateCoreWebView2Controller(this->hMessageWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [env, this, resourceDirectory](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (FAILED(result))
            {
                wchar_t buf[9]{};
                _itow_s(result, buf, 16);
                MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
                this->m_pApp->EnablePlugin(false);
                return S_OK;
            }
            if (controller != nullptr) {
                this->webViewController = controller;
                this->webViewController->get_CoreWebView2(this->webView.put());
            }
            auto hWebViewWnd = FindWindowExW(this->hMessageWnd, nullptr, nullptr, nullptr);
            this->hWebViewWnd = hWebViewWnd;
            // ICoreWebView2_3, ICoreWebView2Controller2: 1.0.774.44
            auto controller2 = this->webViewController.try_query<ICoreWebView2Controller2>();
            if (!controller2)
            {
                MessageBoxW(this->m_pApp->GetAppWindow(), L"WebView2のバージョンが古すぎます。", L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
            }
            else
            {
                COREWEBVIEW2_COLOR c = { };
                auto ff = controller2->put_DefaultBackgroundColor(c);
            }
            wil::com_ptr<ICoreWebView2Settings> settings;
            this->webView->get_Settings(settings.put());
            settings->put_IsScriptEnabled(TRUE);
            settings->put_AreDefaultScriptDialogsEnabled(TRUE);
            settings->put_IsWebMessageEnabled(TRUE);

            this->webViewController->put_ParentWindow(this->hContainerWnd);
            RECT bounds;
            GetClientRect(this->hContainerWnd, &bounds);
            this->webViewController->put_Bounds(bounds);
            SetWindowPos(hWebViewWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

            EventRegistrationToken token;
            auto webView3 = this->webView.query<ICoreWebView2_3>();
            webView3->SetVirtualHostNameToFolderMapping(L"TVTDataBroadcastingWV2.invalid", resourceDirectory.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            this->hybridcast.apiScriptPath = (std::filesystem::path(resourceDirectory) / L"dist" / L"hybridcast_api.js").wstring();

            // 仮想ホスト以外へのリクエストは全てブロックする
            // 通信が有効になっている場合は内部のプロキシへの通信のみ許可する
            webView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            this->webView->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                HRESULT hr = args->get_Request(request.put());
                if (FAILED(hr))
                {
                    return hr;
                }
                wil::unique_cotaskmem_string uri;
                hr = request->get_Uri(uri.put());
                if (FAILED(hr))
                {
                    return hr;
                }
                // 403を返してリクエストを遮断する
                auto block = [&]() -> HRESULT {
                    wil::com_ptr<ICoreWebView2Environment> env;
                    HRESULT hr = this->webView.query<ICoreWebView2_2>()->get_Environment(env.put());
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                    wil::com_ptr<ICoreWebView2WebResourceResponse> response;
                    hr = env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", response.put());
                    if (FAILED(hr))
                    {
                        return hr;
                    }
                    return args->put_Response(response.get());
                };
                if (this->InHybridcastMode())
                {
                    // ハイブリッドキャストのアプリ(外部ページ)の場合:
                    // プラグインの仮想ホストと内部プロキシ(どちらも https://TVTDataBroadcastingWV2*.invalid)には触れさせない。
                    // 内部プロキシは通信先を問わず取得できるため、外部ページに使わせてはいけない。
                    const wchar_t* pluginHostPrefix = L"https://TVTDataBroadcastingWV2";
                    if (!_wcsnicmp(uri.get(), pluginHostPrefix, wcslen(pluginHostPrefix)))
                    {
                        this->HybridcastLog(L"WARN", L"アプリからのプラグイン内部リソースへのアクセスを遮断しました: " + this->HybridcastLogUrl(uri.get()));
                        return block();
                    }
                    // 画像・スクリプト・通信などの部品の取得は、アプリケーションバウンダリの対象外(STD-0010 7.6.1)。
                    // バウンダリを判定するのは文書(HTML)の遷移で、NavigationStarting で行う。
                    if (!_wcsnicmp(uri.get(), L"http://", 7) || !_wcsnicmp(uri.get(), L"https://", 8))
                    {
                        return S_OK;
                    }
                    this->HybridcastLog(L"WARN", L"アプリからの http/https 以外のリクエストを遮断しました: " + this->HybridcastLogUrl(uri.get()));
                    return block();
                }
                auto get = L"https://TVTDataBroadcastingWV2-api.invalid/api/get/";
                auto post = L"https://TVTDataBroadcastingWV2-api.invalid/api/post/";
                LPCWSTR proxyUrl = nullptr;
                if (!_wcsnicmp(get, uri.get(), wcslen(get)))
                {
                    proxyUrl = uri.get() + wcslen(get);
                }
                else if (!_wcsnicmp(post, uri.get(), wcslen(post)))
                {
                    proxyUrl = uri.get() + wcslen(post);
                }
                if (this->proxySession && proxyUrl && this->enableNetwork)
                {
                    return this->Proxy(args, proxyUrl);
                }
                else if (_wcsnicmp(L"https://TVTDataBroadcastingWV2.invalid/", uri.get(), wcslen(L"https://TVTDataBroadcastingWV2.invalid/")))
                {
                    return block();
                }
                return S_OK;
            }).Get(), &token);

            // アプリケーションバウンダリ外への文書遷移(iframeを含む)を検出したら、アプリを終了して放送受信映像のみにする
            // (IPTVFJ STD-0010 7.6.1 / STD-0013 8.1.3.10.2)。終了処理はメッセージで遅らせて実行する。
            auto navigationStarting = Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                this->OnHybridcastNavigationStarting(args, true);
                return S_OK;
            });
            auto frameNavigationStarting = Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                this->OnHybridcastNavigationStarting(args, false);
                return S_OK;
            });
            this->webView->add_NavigationStarting(navigationStarting.Get(), &token);
            this->webView->add_FrameNavigationStarting(frameNavigationStarting.Get(), &token);
            // ハイブリッドキャストのアプリの読み込み結果(成功・失敗とエラーの種類)を記録する。HTTPのエラーは、下の応答の記録に出る
            this->webView->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (!this->InHybridcastMode())
                {
                    return S_OK;
                }
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                COREWEBVIEW2_WEB_ERROR_STATUS errorStatus = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                args->get_WebErrorStatus(&errorStatus);
                wil::unique_cotaskmem_string source;
                std::wstring url;
                if (SUCCEEDED(sender->get_Source(source.put())) && source)
                {
                    url = this->HybridcastLogUrl(source.get());
                }
                this->HybridcastLog(success ? L"INFO" : L"WARN",
                    std::wstring(success ? L"読み込み完了" : L"読み込み失敗") +
                    L" webErrorStatus=" + std::to_wstring((int)errorStatus) + L" " + url);
                return S_OK;
            }).Get(), &token);
            // HTTPエラー(403 など)とリダイレクト先(Location)を記録する。ユーザーエージェントなどで弾かれた場合の手がかりになる
            auto webView2 = this->webView.try_query<ICoreWebView2_2>();
            if (webView2)
            {
                webView2->add_WebResourceResponseReceived(Callback<ICoreWebView2WebResourceResponseReceivedEventHandler>(
                    [this](ICoreWebView2* sender, ICoreWebView2WebResourceResponseReceivedEventArgs* args) -> HRESULT {
                    if (!this->InHybridcastMode())
                    {
                        return S_OK;
                    }
                    wil::com_ptr<ICoreWebView2WebResourceRequest> request;
                    wil::com_ptr<ICoreWebView2WebResourceResponseView> response;
                    if (FAILED(args->get_Request(request.put())) || FAILED(args->get_Response(response.put())))
                    {
                        return S_OK;
                    }
                    int status = 0;
                    response->get_StatusCode(&status);
                    if (status < 300)
                    {
                        return S_OK;
                    }
                    wil::unique_cotaskmem_string uri;
                    request->get_Uri(uri.put());
                    std::wstring text = L"HTTP " + std::to_wstring(status) + L" " + this->HybridcastLogUrl(uri ? uri.get() : L"");
                    if (status < 400)
                    {
                        wil::com_ptr<ICoreWebView2HttpResponseHeaders> headers;
                        wil::unique_cotaskmem_string location;
                        if (SUCCEEDED(response->get_Headers(headers.put())) && headers &&
                            SUCCEEDED(headers->GetHeader(L"Location", location.put())) && location)
                        {
                            text += L" -> " + this->HybridcastLogUrl(location.get());
                        }
                    }
                    this->HybridcastLog(status >= 400 ? L"WARN" : L"INFO", text);
                    return S_OK;
                }).Get(), &token);
            }
            // アプリのJavaScriptの例外と console.error を記録する(DevToolsプロトコル)
            for (const wchar_t* eventName : { L"Runtime.exceptionThrown", L"Runtime.consoleAPICalled" })
            {
                wil::com_ptr<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
                if (SUCCEEDED(this->webView->GetDevToolsProtocolEventReceiver(eventName, receiver.put())) && receiver)
                {
                    bool isException = !wcscmp(eventName, L"Runtime.exceptionThrown");
                    receiver->add_DevToolsProtocolEventReceived(Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
                        [this, isException](ICoreWebView2* sender, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
                        if (!this->InHybridcastMode())
                        {
                            return S_OK;
                        }
                        wil::unique_cotaskmem_string json;
                        if (SUCCEEDED(args->get_ParameterObjectAsJson(json.put())) && json)
                        {
                            this->OnHybridcastDevToolsEvent(isException, json.get());
                        }
                        return S_OK;
                    }).Get(), &token);
                    this->hybridcastDevToolsReceivers.push_back(receiver);
                }
            }
            // ポップアップ(window.open など)は開かせない
            this->webView->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(), &token);

            this->webView->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this, hWebViewWnd](ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                if (SUCCEEDED(args->get_WebMessageAsJson(message.put())))
                {
                    auto messageUTF8 = wstrToUTF8String(message.get());

                    // 例外を投げない解析(不正なJSONや型違いのメッセージでプロセスを落とさない)
                    auto a = nlohmann::json::parse(messageUTF8, nullptr, false);
                    if (a.is_discarded() || !a.is_object() || !a.contains("type") || !a["type"].is_string())
                    {
                        return S_OK;
                    }
                    auto type = a["type"].get<std::string>();
                    // 送信元がプラグイン自身のページでない場合(=ハイブリッドキャストのアプリ)は、
                    // アプリ用に限定した経路でだけ処理する(tune などプラグイン用のメッセージは受け付けない)
                    {
                        wil::unique_cotaskmem_string source;
                        const wchar_t* pluginPage = L"https://TVTDataBroadcastingWV2.invalid/";
                        bool fromPlugin = SUCCEEDED(args->get_Source(source.put())) && source &&
                            !_wcsnicmp(source.get(), pluginPage, wcslen(pluginPage));
                        if (!fromPlugin)
                        {
                            this->OnHybridcastAppMessage(type, a);
                            return S_OK;
                        }
                    }
                    if (type == "videoChanged")
                    {
                        auto left = a["left"].get<double>();
                        auto right = a["right"].get<double>();
                        auto top = a["top"].get<double>();
                        auto bottom = a["bottom"].get<double>();
                        auto invisible = a["invisible"].get<bool>();
                        RECT r;
                        r.left = (int)std::floor(left);
                        r.right = (int)std::ceil(right);
                        r.top = (int)std::floor(top);
                        r.bottom = (int)std::ceil(bottom);

                        this->invisible = invisible;
                        this->videoRect = r;
                        this->ResizeVideoWindow();
                    }
                    else if (type == "hybridcastLaunch")
                    {
                        this->LaunchHybridcast(a);
                    }
                    else if (type == "hybridcastLog")
                    {
                        // データ放送側(BML)の橋渡しからのログ(AITの取得失敗、該当アプリなし など)
                        if (a.contains("text") && a["text"].is_string())
                        {
                            std::string level = a.contains("level") && a["level"].is_string() ? a["level"].get<std::string>() : "info";
                            std::wstring text = utf8StrToWString(a["text"].get<std::string>().substr(0, 600).c_str());
                            this->HybridcastLog(level == "error" ? L"ERROR" : level == "warn" ? L"WARN" : L"INFO", L"[BML] " + text);
                        }
                    }
                    else if (type == "invisible")
                    {
                        auto invisible = a["invisible"].get<bool>();
                        this->invisible = invisible;
                        this->ResizeVideoWindow();
                    }
                    else if (type == "status")
                    {
                        auto url = utf8StrToWString(a["url"].get<std::string>().c_str());
                        auto receiving = a["receiving"].get<bool>();
                        auto loading = a["loading"].get<bool>();
                        this->status.url = url;
                        this->status.receiving = receiving;
                        this->status.loading = loading;
                        this->m_pApp->StatusItemNotify(1, TVTest::STATUS_ITEM_NOTIFY_REDRAW);
                    }
                    else if (type == "tune")
                    {
                        auto originalNetworkId = a["originalNetworkId"].get<int>();
                        auto transportStreamId = a["transportStreamId"].get<int>();
                        auto serviceId = a["serviceId"].get<int>();
                        TVTest::ChannelSelectInfo info = {};
                        info.Size = sizeof(info);
                        info.Flags = TVTest::CHANNEL_SELECT_FLAG_STRICTSERVICE | TVTest::CHANNEL_SELECT_FLAG_ALLOWDISABLED;
                        // FIXME: original_network_idでない
                        info.NetworkID = originalNetworkId;
                        info.TransportStreamID = transportStreamId;
                        info.ServiceID = serviceId;
                        info.Channel = -1;
                        info.Space = -1;
                        if (!this->m_pApp->SelectChannel(&info))
                        {
                            int numSpace = 0;
                            this->m_pApp->GetTuningSpace(&numSpace);
                            bool success = false;
                            for (int space = 0; space < numSpace && !success; space++)
                            {
                                for (int channel = 0; ; channel++)
                                {
                                    TVTest::ChannelInfo channelInfo = {};
                                    if (!this->m_pApp->GetChannelInfo(space, channel, &channelInfo))
                                    {
                                        break;
                                    }
                                    // FIXME: original_network_idでない
                                    if (channelInfo.NetworkID == originalNetworkId && channelInfo.TransportStreamID == transportStreamId)
                                    {
                                        success = this->m_pApp->SetChannel(space, channel, serviceId);
                                        break;
                                    }
                                }
                            }
                            if (!success)
                            {
                                auto msg = (L"データ放送からの選局に失敗しました。映像/音声のないサービスまたはチャンネルスキャンされていない可能性があります。(original_network_id=" + std::to_wstring(originalNetworkId) + L",transport_stream_id=" + std::to_wstring(transportStreamId) + L",service_id=" + std::to_wstring(serviceId) + L")");
                                MessageBoxW(this->m_pApp->GetFullscreen() ? this->GetFullscreenWindow() : this->m_pApp->GetAppWindow(), msg.c_str(), nullptr, MB_ICONERROR | MB_OK);
                                this->m_pApp->AddLog(msg.c_str(), TVTest::LOG_TYPE_ERROR);
                                if (this->webView)
                                {
                                    this->webView->Reload();
                                }
                            }
                        }
                    }
                    else if (type == "usedKeyList")
                    {
                        auto&& usedKeyList = a["usedKeyList"];
                        if (usedKeyList.is_object())
                        {
                            UsedKey usedKey{};
                            usedKey.basic = usedKeyList["basic"].is_boolean();
                            usedKey.dataButton = usedKeyList["data-button"].is_boolean();
                            usedKey.numericTuning = usedKeyList["numeric-tuning"].is_boolean();
                            usedKey.special1 = usedKeyList["special-1"].is_boolean();
                            usedKey.special2 = usedKeyList["special-2"].is_boolean();
                            this->usedKey = usedKey;
                        }
                    }
                    else if (type == "input")
                    {
                        if (this->hMessageWnd)
                        {
                            auto&& allowedCharacters = a["allowedCharacters"];
                            auto cb = [this](std::unique_ptr<WCHAR[]> value)
                            {
                                if (!this->webView)
                                {
                                    return;
                                }
                                if (value)
                                {
                                    nlohmann::json msg{ { "type", "changeInput" }, { "value", wstrToUTF8String(value.get()) } };
                                    std::stringstream ss;
                                    ss << msg;
                                    auto wjson = utf8StrToWString(ss.str().c_str());
                                    this->webView->PostWebMessageAsJson(wjson.c_str());
                                }
                                else
                                {
                                    nlohmann::json msg{ { "type", "cancelInput" } };
                                    std::stringstream ss;
                                    ss << msg;
                                    auto wjson = utf8StrToWString(ss.str().c_str());
                                    this->webView->PostWebMessageAsJson(wjson.c_str());
                                }
                            };
                            auto inputDialog = new InputDialog(
                                utf8StrToWString(a["characterType"].get<std::string>().c_str()),
                                allowedCharacters.is_string() ? std::optional(utf8StrToWString(allowedCharacters.get<std::string>().c_str())) : std::nullopt,
                                a["maxLength"].get<int>(),
                                utf8StrToWString(a["value"].get<std::string>().c_str()),
                                std::move(cb),
                                utf8StrToWString(a["inputMode"].get<std::string>().c_str())
                            );
                            PostMessageW(this->hMessageWnd, WM_APP_INPUT, 0, (LPARAM)inputDialog);
                        }
                    }
                    else if (type == "cancelInput")
                    {
                        this->inputDialog = nullptr;
                    }
                    else if (type == "changeAudioStream")
                    {
                        auto componentId = a["componentId"].get<int>();
                        auto index = a["index"].get<int>();
                        auto&& channelId = a["channelId"];
                        if (componentId == -1)
                        {
                            this->RestoreMainAudio();
                        }
                        else
                        {
                            Audio audio;
                            audio.componentId = (BYTE)componentId;
                            audio.index = index;
                            this->isPlayingMainAudio = false;
                            if (channelId.is_number_integer())
                            {
                                audio.setChannelId(channelId.get<int>());
                            }
                            this->SelectAudio(audio);
                        }
                    }
                    else if (type == "changeMainAudioStream")
                    {
                        auto componentId = a["componentId"].get<int>();
                        auto index = a["index"].get<int>();
                        auto&& channelId = a["channelId"];
                        Audio audio;
                        audio.componentId = (BYTE)componentId;
                        audio.index = index;
                        if (channelId.is_number_integer())
                        {
                            audio.setChannelId(channelId.get<int>());
                        }
                        if (this->isPlayingMainAudio)
                        {
                            this->SelectAudio(audio);
                        }
                    }
                    else if (type == "serviceInfo")
                    {
                        auto cProfile = a["cProfile"].get<bool>();
                        auto serviceId = a["serviceId"].get<int>();
                        auto networkId = a["networkId"].get<int>();
                        if (this->currentService.ServiceID == serviceId && this->currentChannel.NetworkID == networkId)
                        {
                            this->currentServiceIsOneSeg = cProfile;
                            if (!cProfile)
                            {
                                this->DestroyOneSegWindow();
                            }
                        }
                    }
                    else if (type == "startBrowser")
                    {
                        auto uri = a["uri"].get<std::string>();
                        auto fullscreen = a["fullscreen"].get<bool>();
                        if (uri.starts_with("http://") || uri.starts_with("https://"))
                        {
#if 0
                            if (fullscreen)
                            {
                                this->DestroyOneSegWindow();
                            }
#endif
                            auto wuri = utf8StrToWString(uri.c_str());
                            ShellExecuteW(nullptr, L"open", wuri.c_str(), nullptr, nullptr, SW_SHOW);
                        }
                    }
                }
                return S_OK;
            }).Get(), &token);

            this->webView->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (!this->webViewLoaded)
                {
                    // 動画ウィンドウといい感じに合成させるために必要 (Windows 8以降じゃないと動かないはず)
                    SetWindowLongW(this->hWebViewWnd, GWL_EXSTYLE, GetWindowLongW(this->hWebViewWnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
                    this->webViewController->put_IsVisible(true);
                }
                this->UpdateCaptionState(false);
                this->UpdateVolume();
                if (this->proxySession)
                {
                    this->UpdateNetworkState();
                }
                this->UpdateAudioStream();
                this->webViewLoaded = true;
                if (this->oneSegWindowIsShown)
                {
                    this->webView->PostWebMessageAsJson(LR"({"type":"launchOneSeg"})");
                }
                return S_OK;
            }).Get(), &token);
            this->Tune();
            return S_OK;
        }).Get());
        return S_OK;
    }).Get());
    if (FAILED(result))
    {
        wchar_t buf[9]{};
        _itow_s(result, buf, 16);

        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(CreateCoreWebView2EnvironmentWithOptions)\nWebView2がインストールされていない可能性があります。\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
        }
        else
        {
            MessageBoxW(this->m_pApp->GetAppWindow(), (std::wstring(L"WebView2を初期化できませんでした。(CreateCoreWebView2EnvironmentWithOptions)\nHRESULT = 0x") + buf).c_str(), L"TVTDataBroadcastingWV2", MB_ICONERROR | MB_OK);
        }
        this->m_pApp->EnablePlugin(false);
    }
}

// cors無効コマンドライン引数を追加するなどでWebView2側に任せることも可能ではあるけど融通が利かないためWinHTTPを使う あとバージョン依存の問題があるらしい?
HRESULT CDataBroadcastingWV2::Proxy(ICoreWebView2WebResourceRequestedEventArgs* args, LPCWSTR proxyUrl)
{
    wil::com_ptr<ICoreWebView2WebResourceRequest> request;
    HRESULT hr = args->get_Request(request.put());
    if (FAILED(hr))
    {
        return hr;
    }
    wil::unique_cotaskmem_string method;
    hr = request->get_Method(method.put());
    if (FAILED(hr))
    {
        return hr;
    }
    wil::com_ptr<IStream> content;
    hr = request->get_Content(content.put());
    std::vector<BYTE> contentBuffer;
    ULONG contentSize;
    if (SUCCEEDED(hr) && content)
    {
        // fetchから呼んだ場合CMemStreamなので同期的に取得する
        STATSTG statstg = {};
        hr = content->Stat(&statstg, STATFLAG_NONAME);
        auto size = statstg.cbSize.QuadPart;
        if (SUCCEEDED(hr) && size <= std::numeric_limits<ULONG>::max())
        {
            contentBuffer.resize((ULONG)size);
            content->Read(contentBuffer.data(), (ULONG)size, &contentSize);
            contentBuffer.resize(contentSize);
        }
    }
    wil::com_ptr<ICoreWebView2HttpRequestHeaders> headers;
    hr = request->get_Headers(headers.put());
    if (FAILED(hr))
    {
        return hr;
    }
    wil::com_ptr<ICoreWebView2HttpHeadersCollectionIterator> iterator;
    hr = headers->GetIterator(iterator.put());
    if (FAILED(hr))
    {
        return hr;
    }
    BOOL hasCurrent = false;
    std::vector<std::pair<wil::unique_cotaskmem_string, wil::unique_cotaskmem_string>> headersCo;
    std::vector<std::pair<LPCWSTR, LPCWSTR>> headersPtr
    {
        { L"Pragma", L"no-cache" },
        { L"Accept-Language", L"ja" },
    };
    while (SUCCEEDED(iterator->get_HasCurrentHeader(&hasCurrent)) && hasCurrent)
    {
        wil::unique_cotaskmem_string name;
        wil::unique_cotaskmem_string value;
        if (SUCCEEDED(iterator->GetCurrentHeader(name.put(), value.put())))
        {
            if (!_wcsicmp(name.get(), L"if-modified-since") || !_wcsicmp(name.get(), L"cache-control") || !_wcsicmp(name.get(), L"content-type"))
            {
                headersPtr.push_back({ name.get(), value.get() });
                headersCo.push_back({ std::move(name), std::move(value) });
            }
            BOOL hasNext;
            if (FAILED(iterator->MoveNext(&hasNext)))
            {
                break;
            }
        }
    }
    wil::com_ptr<ICoreWebView2Deferral> deferral;
    args->GetDeferral(deferral.put());
    if (!ProxyRequest::RequestAsync(*this->proxySession.get(), proxyUrl, method.get(), std::move(contentBuffer), headersPtr, [deferral]() -> void
    {
        // error
        deferral->Complete();
    }, [this, args = wil::com_ptr<ICoreWebView2WebResourceRequestedEventArgs>(args), deferral](DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content) -> void
    {
        if (this->hMessageWnd)
        {
            auto response = new DeferralResponse
            {
                deferral,
                args,
                statusCode,
                std::wstring(statusCodeText),
                std::wstring(headers),
                std::vector<BYTE>(content, content + contentLength),
            };
            PostMessageW(this->hMessageWnd, WM_APP_RESPONSE, 0, (LPARAM)response);
        }
        else
        {
            deferral->Complete();
        }
        return;
    }))
    {
        deferral->Complete();
    }
    return S_OK;
}

void CDataBroadcastingWV2::ResizeVideoWindow()
{
    if (this->invisible || this->oneSegWindowIsShown)
    {
        this->RestoreVideoWindow();
    }
    else
    {
        if (this->videoRect.right - this->videoRect.left && this->videoRect.bottom - this->videoRect.top)
        {
            if (this->vmr9Renderer)
            {
                auto vmr9WindowlessControl = this->vmr9Renderer.query<IVMRWindowlessControl9>();
                vmr9WindowlessControl->SetVideoPosition(nullptr, &this->videoRect);
            }
            else if (this->vmr7Renderer)
            {
                auto vmr7WindowlessControl = this->vmr7Renderer.query<IVMRWindowlessControl>();
                vmr7WindowlessControl->SetVideoPosition(nullptr, &this->videoRect);
            }
            else if (this->basicVideo)
            {
                this->basicVideo->SetDestinationPosition(this->videoRect.left, this->videoRect.top, this->videoRect.right - this->videoRect.left, this->videoRect.bottom - this->videoRect.top);
            }
            else
            {
                RECT rect;
                if (GetClientRect(this->hVideoWnd, &rect))
                {
                    if (memcmp(&rect, &this->videoRect, sizeof(rect)))
                    {
                        SetWindowPos(this->hVideoWnd, HWND_BOTTOM, this->videoRect.left, this->videoRect.top, this->videoRect.right - this->videoRect.left, this->videoRect.bottom - this->videoRect.top, SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                    }
                }
            }
        }
    }
}

void CDataBroadcastingWV2::Disable(bool finalize)
{
    this->RestoreMainAudio();
    this->RestoreVideoWindow();
    m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback, this);
    if (!finalize)
    {
        m_pApp->SetWindowMessageCallback(nullptr, this);
    }
    this->oneSegWindow = nullptr;
    if (this->hRemoteWnd)
    {
        auto hWnd = this->hRemoteWnd;
        this->hRemoteWnd = nullptr;
        DestroyWindow(hWnd);
    }
    if (this->webViewController)
    {
        this->webView->Stop();
        this->webView.reset();
        this->webViewController->Close();
        this->webViewController.reset();
    }
    this->webViewLoaded = false;

    this->packetQueue.clear();

    if (this->hMessageWnd)
    {
        auto hWnd = this->hMessageWnd;
        DestroyWindow(hWnd);
        this->hMessageWnd = nullptr;
    }

    this->proxySession = nullptr;

    this->inputDialog = nullptr;

    if (finalize)
    {
        auto hWnd = this->hPanelWnd;
        this->hPanelWnd = nullptr;
        DestroyWindow(hWnd);
        if (this->hbrPanelBack)
        {
            DeleteObject(this->hbrPanelBack);
            this->hbrPanelBack = nullptr;
        }
        for (size_t i = 0; i < _countof(this->hbrBRGYBacks); i++)
        {
            if (this->hbrBRGYBacks[i])
            {
                DeleteObject(this->hbrBRGYBacks[i]);
                this->hbrBRGYBacks[i] = nullptr;
            }
        }
        if (this->hPanelFont)
        {
            DeleteObject(this->hPanelFont);
            this->hPanelFont = nullptr;
        }
    }
}

void CDataBroadcastingWV2::CreateMessageWindow()
{
    if (this->hMessageWnd != nullptr)
    {
        return;
    }
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MessageWndProc;
    wc.hInstance = g_hinstDLL;
    wc.lpszClassName = L"TVTDataBroadcastingWV2 Message Window";
    RegisterClassExW(&wc);
    this->hMessageWnd = CreateWindowExW(0, L"TVTDataBroadcastingWV2 Message Window", L"TVTDataBroadcastingWV2 Message Window", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    SetWindowLongPtrW(this->hMessageWnd, GWLP_USERDATA, (LONG_PTR)this);
}

bool CDataBroadcastingWV2::OnPluginEnable(bool fEnable)
{
    this->status = {};
    if (fEnable)
    {
        this->CreateMessageWindow();
        if (!this->GetIniItem(L"DisableRemoteControl", 0))
        {
            this->ShowRemoteControlDialog();
        }
        m_pApp->SetStreamCallback(0, StreamCallback, this);
        m_pApp->SetWindowMessageCallback(WindowMessageCallback, this);
        if (this->useTVTestVolume)
        {
            this->OnVolumeChange(this->m_pApp->GetVolume(), this->m_pApp->GetMute());
        }
        InitWebView2();
        SetTimer(this->hMessageWnd, IDT_RESIZE, 1000, nullptr);
        this->EnablePanelButtons(true);
        if (this->GetIniItem(L"EnableNetwork", 0))
        {
            this->proxySession = std::unique_ptr<ProxySession>(new ProxySession());
        }
    }
    else
    {
        this->EnablePanelButtons(false);
        this->Disable(false);
    }
    return true;
}

namespace
{
std::wstring PercentEncode(const std::wstring& s)
{
    auto utf8 = wstrToUTF8String(s.c_str());
    std::wstring out;
    for (unsigned char c : utf8)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            out += (wchar_t)c;
        }
        else
        {
            wchar_t buf[4];
            swprintf_s(buf, L"%%%02X", c);
            out += buf;
        }
    }
    return out;
}
}

void CDataBroadcastingWV2::Tune()
{
    if (this->webView && this->currentService.ServiceID)
    {
        // ハイブリッドキャストのアプリ実行中は、同じサービスのままなら画面を切り替えない。
        // 選局で別のサービスになったら、アプリを終了して新しいサービスのデータ放送へ遷移する(STD-0013 8.1.3.10.1)。
        if (this->InHybridcastMode())
        {
            if (this->hybridcast.serviceId == this->currentService.ServiceID &&
                this->hybridcast.networkId == this->currentChannel.NetworkID)
            {
                return;
            }
            this->HybridcastLog(L"INFO", L"選局されたため、アプリを終了します");
            this->LeaveHybridcastState();
        }
        this->status = {};
        wil::unique_cotaskmem_string source;
        if (SUCCEEDED(this->webView->get_Source(source.put())))
        {
            std::wstring baseUrl(L"https://TVTDataBroadcastingWV2.invalid/TVTDataBroadcastingWV2.html?serviceId=");
            baseUrl += std::to_wstring(this->currentService.ServiceID);
            baseUrl += L"&networkId=";
            baseUrl += std::to_wstring(this->currentChannel.NetworkID);
            long long key = ((long long)this->currentChannel.NetworkID << 16) | this->currentService.ServiceID;
            // アプリ終了後は「放送受信映像のみ」の状態で戻る(別のサービスになったら解除)
            if (this->hybridcast.videoOnlyKey == key)
            {
                baseUrl += L"&videoOnly=1";
            }
            else
            {
                this->hybridcast.videoOnlyKey = 0;
            }
            // 試験用: iniの HybridcastAitUrl があれば、サービスごとに1回だけAITからの起動を試みる
            if (this->hybridcast.consumedKey != key)
            {
                this->hybridcast.consumedKey = 0;
                auto aitUrl = this->GetIniItem(L"HybridcastAitUrl", L"");
                if (!aitUrl.empty())
                {
                    baseUrl += L"&hybridcastAit=";
                    baseUrl += PercentEncode(aitUrl);
                }
            }
            if (_wcsicmp(source.get(), baseUrl.c_str()))
            {
                this->oneSegWindow = nullptr;
                this->RestoreVideoWindow();
                this->webView->Navigate(baseUrl.c_str());
                this->usedKey.basic = true;
                this->usedKey.dataButton = true;
                this->usedKey.numericTuning = false;
            }
        }
    }
}

namespace
{
// バウンダリの判定に使うURLの要素(IPTVFJ STD-0013 6.1.3.1 表 6-1)。判定の規則は browser/src/hybridcast/boundary.ts と同じ。
struct ParsedHttpUrl
{
    std::wstring scheme; // L"http" または L"https"
    std::wstring host;   // 小文字
    std::wstring port;   // 既定のポートは空
    std::wstring path;   // "/" 始まり。パス指定のない managedURL では空
    bool hasPath = false;
    bool valid = false;
};

// isManaged: managedURL の記述(クエリ・フラグメントを含まない。ドメイン直後のスラッシュの有無に意味がある)
ParsedHttpUrl ParseHttpUrl(const std::wstring& text, bool isManaged)
{
    ParsedHttpUrl r;
    size_t schemeEnd = text.find(L"://");
    if (schemeEnd == std::wstring::npos)
    {
        return r;
    }
    std::wstring scheme;
    for (size_t i = 0; i < schemeEnd; i++)
    {
        scheme += (wchar_t)towlower(text[i]);
    }
    if (scheme != L"http" && scheme != L"https")
    {
        return r;
    }
    size_t authStart = schemeEnd + 3;
    size_t authEnd = text.find_first_of(L"/?#", authStart);
    if (authEnd == std::wstring::npos)
    {
        authEnd = text.size();
    }
    std::wstring authority = text.substr(authStart, authEnd - authStart);
    // ユーザー情報("nhk.or.jp@evil.example" のような偽装)、IPv6、空白・バックスラッシュは扱わない
    if (authority.empty() || authority.find_first_of(L"@[]\\ ") != std::wstring::npos)
    {
        return r;
    }
    std::wstring hostText = authority;
    std::wstring port;
    size_t colon = authority.rfind(L':');
    if (colon != std::wstring::npos)
    {
        hostText = authority.substr(0, colon);
        port = authority.substr(colon + 1);
        if (port.empty() || port.find_first_not_of(L"0123456789") != std::wstring::npos)
        {
            return r;
        }
    }
    std::wstring host;
    for (wchar_t c : hostText)
    {
        wchar_t lower = (wchar_t)towlower(c);
        bool ok = (lower >= L'a' && lower <= L'z') || (lower >= L'0' && lower <= L'9') || lower == L'-' || lower == L'.';
        if (!ok)
        {
            return r;
        }
        host += lower;
    }
    if (host.empty())
    {
        return r;
    }
    if ((scheme == L"http" && port == L"80") || (scheme == L"https" && port == L"443"))
    {
        port.clear();
    }
    std::wstring rest = text.substr(authEnd);
    bool hasPath = !rest.empty() && rest[0] == L'/';
    std::wstring path;
    if (isManaged)
    {
        if (rest.find_first_of(L"?#") != std::wstring::npos)
        {
            return r;
        }
        path = hasPath ? rest : L"";
    }
    else
    {
        size_t q = rest.find_first_of(L"?#");
        path = rest.substr(0, q);
        if (path.empty())
        {
            path = L"/";
        }
        hasPath = true;
    }
    r.scheme = scheme;
    r.host = host;
    r.port = port;
    r.path = path;
    r.hasPath = hasPath;
    r.valid = true;
    return r;
}

// ディレクトリ境界を守った前方一致("/sub" は "/sub" と "/sub/..." に一致し、"/subversion" には一致しない)
bool PathContains(const std::wstring& managedPath, const std::wstring& docPath)
{
    std::wstring base = managedPath;
    if (base.empty() || base.back() != L'/')
    {
        base += L'/';
    }
    return docPath == managedPath || docPath.compare(0, base.size(), base) == 0;
}

bool ManagedUrlMatches(const ParsedHttpUrl& m, const ParsedHttpUrl& doc)
{
    if (m.scheme != doc.scheme || m.port != doc.port)
    {
        return false;
    }
    if (m.hasPath)
    {
        // パスを書いた場合は、ドメインを厳格に解釈する(下位ドメインを含まない)
        return m.host == doc.host && PathContains(m.path, doc.path);
    }
    // ドメインだけの場合は、下位のドメインを含む("." を挟んだ後方一致。"xnhk.or.jp" は含まない)
    if (doc.host == m.host)
    {
        return true;
    }
    std::wstring suffix = L"." + m.host;
    return doc.host.size() > suffix.size() &&
        doc.host.compare(doc.host.size() - suffix.size(), suffix.size(), suffix) == 0;
}
}

// ハイブリッドキャストのログを Plugins\TVTDataBroadcastingWV2\hybridcast.log に追記する(UTF-8, 1行1件)。
// 2MBを超えたら hybridcast.old.log へ退避する(1世代)。WARN/ERROR はTVTestのログにも出す。
void CDataBroadcastingWV2::HybridcastLog(const wchar_t* level, const std::wstring& message)
{
    if (!this->hybridcastLogEnabled)
    {
        return;
    }
    try
    {
        std::lock_guard<std::mutex> lock(this->hybridcastLogMutex);
        // 暴走するアプリがログを埋め尽くさないよう、1回の起動での行数に上限を設ける
        const int maxLines = 3000;
        if (this->hybridcastLogLines > maxLines)
        {
            return;
        }
        this->hybridcastLogLines++;
        std::wstring text = this->hybridcastLogLines > maxLines ? L"ログが多いため、以降の記録を省略します" : message;
        // 1行にそろえる
        for (auto& c : text)
        {
            if (c == L'\r' || c == L'\n')
            {
                c = L' ';
            }
        }
        std::filesystem::path path = std::filesystem::path(this->baseDirectory) / L"hybridcast.log";
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        if (!ec && size > 2u * 1024u * 1024u)
        {
            std::filesystem::rename(path, path.parent_path() / L"hybridcast.old.log", ec);
        }
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t head[96];
        swprintf_s(head, L"%04d-%02d-%02d %02d:%02d:%02d.%03d [%ls] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level);
        std::string line = wstrToUTF8String((std::wstring(head) + text).c_str());
        line += "\r\n";
        std::ofstream ofs(path, std::ios::binary | std::ios::app);
        if (ofs)
        {
            ofs.write(line.data(), (std::streamsize)line.size());
        }
        if (wcscmp(level, L"INFO") != 0)
        {
            this->m_pApp->AddLog((L"TVTDataBroadcastingWV2: " + text).c_str());
        }
    }
    catch (...)
    {
        // ログの失敗で本来の処理を止めない
    }
}

// ログに書くURL。個人情報が含まれ得るクエリ・フラグメントと、ユーザー情報は、既定では記録しない(HybridcastLogFullUrl=1 で記録)
std::wstring CDataBroadcastingWV2::HybridcastLogUrl(const std::wstring& url)
{
    if (this->hybridcastLogFullUrl)
    {
        return url;
    }
    std::wstring result = url;
    size_t authStart = result.find(L"://");
    if (authStart != std::wstring::npos)
    {
        authStart += 3;
        size_t authEnd = result.find_first_of(L"/?#", authStart);
        size_t at = result.find(L'@', authStart);
        if (at != std::wstring::npos && (authEnd == std::wstring::npos || at < authEnd))
        {
            result.replace(authStart, at - authStart + 1, L"***@");
        }
    }
    size_t cut = result.find_first_of(L"?#");
    if (cut != std::wstring::npos)
    {
        result = result.substr(0, cut) + L"?(省略)";
    }
    if (result.size() > 300)
    {
        result = result.substr(0, 300) + L"...";
    }
    return result;
}

// アプリ(マネージド状態)の文書遷移を、バウンダリで判定する。バウンダリ外なら、アプリを終了して放送受信映像のみにする
// (IPTVFJ STD-0010 7.6.1 / STD-0013 8.1.3.10.2)。遷移先はすべてログに残す(どのサイトに誘導されたかを調べるため)。
void CDataBroadcastingWV2::OnHybridcastNavigationStarting(ICoreWebView2NavigationStartingEventArgs* args, bool isMainFrame)
{
    if (!this->InHybridcastMode())
    {
        return;
    }
    wil::unique_cotaskmem_string uri;
    if (FAILED(args->get_Uri(uri.put())) || !uri)
    {
        return;
    }
    BOOL redirected = FALSE;
    args->get_IsRedirected(&redirected);
    BOOL userInitiated = FALSE;
    args->get_IsUserInitiated(&userInitiated);
    std::wstring detail = std::wstring(isMainFrame ? L"遷移" : L"遷移(フレーム)") + (redirected ? L"[リダイレクト] " : L" ") +
        (userInitiated ? L"[操作] " : L"") + this->HybridcastLogUrl(uri.get());
    if (this->hybridcast.unmanaged)
    {
        this->HybridcastLog(L"INFO", L"アンマネージド状態の" + detail);
        return;
    }
    if (!_wcsicmp(uri.get(), L"about:blank") || !_wcsicmp(uri.get(), L"about:srcdoc") || this->IsInHybridcastBoundary(uri.get()))
    {
        this->HybridcastLog(L"INFO", detail);
        return;
    }
    this->HybridcastLog(L"WARN", L"バウンダリ外への" + detail + L" -> アプリを終了します");
    args->put_Cancel(TRUE);
    PostMessageW(this->hMessageWnd, WM_APP_HYBRIDCAST_EXIT, 1, 0);
}

// アプリのJavaScriptの例外(Runtime.exceptionThrown)と console.error(Runtime.consoleAPICalled)をログに残す
void CDataBroadcastingWV2::OnHybridcastDevToolsEvent(bool isException, const wchar_t* json)
{
    try
    {
        auto j = nlohmann::json::parse(wstrToUTF8String(json), nullptr, false);
        if (j.is_discarded() || !j.is_object())
        {
            return;
        }
        std::wstring text;
        if (isException)
        {
            auto details = j.value("exceptionDetails", nlohmann::json::object());
            std::string message = details.value("text", std::string());
            if (details.contains("exception") && details["exception"].is_object())
            {
                message = details["exception"].value("description", message);
            }
            std::string url = details.value("url", std::string());
            int line = details.value("lineNumber", -1);
            text = L"JavaScriptの例外: " + utf8StrToWString(message.c_str());
            if (!url.empty())
            {
                text += L" (" + this->HybridcastLogUrl(utf8StrToWString(url.c_str())) + L":" + std::to_wstring(line) + L")";
            }
        }
        else
        {
            std::string type = j.value("type", std::string());
            if (type != "error" && type != "assert")
            {
                return;
            }
            std::string joined;
            if (j.contains("args") && j["args"].is_array())
            {
                for (auto& a : j["args"])
                {
                    if (!joined.empty())
                    {
                        joined += " ";
                    }
                    if (a.is_object() && a.contains("value"))
                    {
                        joined += a["value"].is_string() ? a["value"].get<std::string>() : a["value"].dump();
                    }
                    else if (a.is_object() && a.contains("description") && a["description"].is_string())
                    {
                        joined += a["description"].get<std::string>();
                    }
                }
            }
            text = L"console." + utf8StrToWString(type.c_str()) + L": " + utf8StrToWString(joined.c_str());
        }
        if (text.size() > 500)
        {
            text = text.substr(0, 500) + L"...";
        }
        this->HybridcastLog(L"ERROR", text);
    }
    catch (...)
    {
    }
}

// アプリケーションバウンダリ内の文書か(IPTVFJ STD-0013 6.1.3.1 表 6-1 の規則)
bool CDataBroadcastingWV2::IsInHybridcastBoundary(const wchar_t* url) const
{
    if (!url)
    {
        return false;
    }
    ParsedHttpUrl doc = ParseHttpUrl(url, false);
    if (!doc.valid)
    {
        return false;
    }
    for (const auto& managed : this->hybridcast.boundary)
    {
        ParsedHttpUrl m = ParseHttpUrl(managed, true);
        if (m.valid && ManagedUrlMatches(m, doc))
        {
            return true;
        }
    }
    return false;
}

// resources/dist/hybridcast_api.js を、アプリごとの初期情報つきで注入するスクリプトにする
std::wstring CDataBroadcastingWV2::BuildHybridcastScript(const std::string& initJson) const
{
    std::ifstream ifs(std::filesystem::path(this->hybridcast.apiScriptPath), std::ios::binary);
    if (!ifs)
    {
        return L"";
    }
    std::string bundle((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    // 最上位のページでのみ有効にする(iframeでは chrome.webview が使えない可能性があるため何もしない)
    std::string js =
        "(function(){\n"
        "if (window.top !== window || !(window.chrome && window.chrome.webview)) return;\n" +
        bundle +
        "\n;HybridcastApi.installHybridcastApi(window," + initJson +
        ",{post:function(m){window.chrome.webview.postMessage(m);},"
        "listen:function(h){window.chrome.webview.addEventListener('message',function(e){h(e.data);});}});\n"
        "})();";
    return utf8StrToWString(js.c_str());
}

// IPTVFJ STD-0013 8.1.4.1.1.8 の書式:
//   User-Agent: <others> Hybridcast/1.0 (<Capability>;<MakerId>;<ModelId>;<MajorVer>;<MinorVer>;<reserved>) <others>
// <others> には、ブラウザエンジンを判別できるよう元のUser-Agentを含める。
// MakerId は製造メーカーのIEEE OUI(6桁の16進)。他社のIDを名乗らないよう、既定は 000000。必要ならiniで指定する。
std::wstring CDataBroadcastingWV2::BuildHybridcastUserAgent(const std::wstring& baseUserAgent)
{
    auto sanitize = [](const std::wstring& in, size_t maxLen) {
        std::wstring out;
        for (wchar_t c : in)
        {
            bool ok = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'-' || c == L'_' || c == L'.';
            out += ok ? c : L'_';
            if (out.size() >= maxLen)
            {
                break;
            }
        }
        return out;
    };
    std::wstring makerId = this->GetIniItem(L"HybridcastMakerId", L"000000");
    bool validMakerId = makerId.size() == 6;
    for (wchar_t c : makerId)
    {
        validMakerId = validMakerId && ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f'));
    }
    if (!validMakerId)
    {
        makerId = L"000000";
    }
    std::wstring modelId = sanitize(this->GetIniItem(L"HybridcastModelId", L"TVTDataBroadcastingWV2"), 32);
    std::wstring ua = baseUserAgent;
    ua += L" Hybridcast/1.0 (HD,HD;";
    ua += makerId;
    ua += L";";
    ua += modelId;
    ua += L";1;0;)";
    return ua;
}

void CDataBroadcastingWV2::RestoreHybridcastMute()
{
    if (this->hybridcast.savedMuteValid)
    {
        this->hybridcast.savedMuteValid = false;
        this->m_pApp->SetMute(this->hybridcast.savedMute);
    }
}

// ハイブリッドキャストの状態(マネージド/アンマネージド)を終了し、注入スクリプト・User-Agent・音声を元に戻す
void CDataBroadcastingWV2::LeaveHybridcastState()
{
    this->hybridcast.active = false;
    this->hybridcast.unmanaged = false;
    this->hybridcast.launching = false;
    // addPermissionManagedArea で追加した領域も、アプリの終了時に初期化される(STD-0011 3.1.3.3)
    this->hybridcast.boundary.clear();
    if (this->webView)
    {
        if (!this->hybridcast.scriptId.empty())
        {
            this->webView->RemoveScriptToExecuteOnDocumentCreated(this->hybridcast.scriptId.c_str());
            this->webView->CallDevToolsProtocolMethod(L"Runtime.disable", L"{}", nullptr);
        }
        if (!this->hybridcast.savedUserAgent.empty())
        {
            wil::com_ptr<ICoreWebView2Settings> settings;
            if (SUCCEEDED(this->webView->get_Settings(settings.put())))
            {
                auto settings2 = settings.try_query<ICoreWebView2Settings2>();
                if (settings2)
                {
                    settings2->put_UserAgent(this->hybridcast.savedUserAgent.c_str());
                }
            }
        }
    }
    this->hybridcast.scriptId.clear();
    this->hybridcast.savedUserAgent.clear();
    this->RestoreHybridcastMute();
}

void CDataBroadcastingWV2::LaunchHybridcast(const nlohmann::json& message)
{
    if (!this->webView)
    {
        return;
    }
    // 通信が無効なら起動しない
    if (!this->enableNetwork)
    {
        this->HybridcastLog(L"WARN", L"起動要求を無視しました: 通信が無効です(Plugins\\TVTDataBroadcastingWV2.ini の EnableNetwork=1 が必要)");
        return;
    }
    // 実行中・起動処理中の二重起動もしない
    if (this->InHybridcastMode() || this->hybridcast.launching)
    {
        this->HybridcastLog(L"WARN", L"起動要求を無視しました: すでにアプリを実行中(または起動処理中)です");
        return;
    }
    try
    {
        {
            std::lock_guard<std::mutex> lock(this->hybridcastLogMutex);
            this->hybridcastLogLines = 0; // ログの行数の上限は、起動ごとに数え直す
        }
        auto entryUrl = utf8StrToWString(message.at("entryUrl").get<std::string>().c_str());
        auto init = message.at("init");
        this->HybridcastLog(L"INFO", L"起動要求: " + this->HybridcastLogUrl(entryUrl) +
            L" (組織識別=" + std::to_wstring(init.value("organizationId", -1)) +
            L" アプリ識別=" + std::to_wstring(init.value("applicationId", -1)) + L")");
        if (_wcsnicmp(entryUrl.c_str(), L"http://", 7) && _wcsnicmp(entryUrl.c_str(), L"https://", 8))
        {
            this->HybridcastLog(L"WARN", L"起動要求を無視しました: 起動URLが http/https ではありません");
            return;
        }
        std::vector<std::wstring> boundary;
        for (auto& u : message.at("boundaryUrls"))
        {
            boundary.push_back(utf8StrToWString(u.get<std::string>().c_str()));
        }
        auto script = this->BuildHybridcastScript(init.dump());
        if (script.empty())
        {
            this->HybridcastLog(L"ERROR", L"起動できません: 拡張APIのスクリプトを読み込めません: " + this->hybridcast.apiScriptPath);
            return;
        }
        this->hybridcast.launching = true;
        this->hybridcast.networkId = this->currentChannel.NetworkID;
        this->hybridcast.serviceId = this->currentService.ServiceID;
        this->hybridcast.consumedKey = ((long long)this->hybridcast.networkId << 16) | this->hybridcast.serviceId;
        this->hybridcast.boundary = std::move(boundary);
        this->hybridcast.savedMute = this->m_pApp->GetMute();
        this->hybridcast.savedMuteValid = true;
        // アプリへの通信のUser-Agentを、仕様の書式にする(アプリ終了時に元へ戻す)
        wil::com_ptr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(this->webView->get_Settings(settings.put())))
        {
            auto settings2 = settings.try_query<ICoreWebView2Settings2>();
            if (settings2)
            {
                wil::unique_cotaskmem_string baseUserAgent;
                if (SUCCEEDED(settings2->get_UserAgent(baseUserAgent.put())) && baseUserAgent)
                {
                    this->hybridcast.savedUserAgent = baseUserAgent.get();
                    auto userAgent = this->BuildHybridcastUserAgent(baseUserAgent.get());
                    settings2->put_UserAgent(userAgent.c_str());
                    this->HybridcastLog(L"INFO", L"User-Agent: " + userAgent);
                }
            }
        }
        HRESULT hr = this->webView->AddScriptToExecuteOnDocumentCreated(script.c_str(),
            Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                [this, entryUrl](HRESULT result, PCWSTR id) -> HRESULT {
            if (FAILED(result) || !this->hybridcast.launching)
            {
                this->HybridcastLog(L"ERROR", L"起動できません: 拡張APIの登録に失敗しました hr=" + std::to_wstring((long)result));
                if (SUCCEEDED(result) && id)
                {
                    this->webView->RemoveScriptToExecuteOnDocumentCreated(id);
                }
                this->LeaveHybridcastState();
                return result;
            }
            // 起動処理中に選局されていたら、起動を中止する
            if (this->hybridcast.serviceId != this->currentService.ServiceID ||
                this->hybridcast.networkId != this->currentChannel.NetworkID)
            {
                this->HybridcastLog(L"WARN", L"起動を中止しました: 起動処理中に選局されました");
                this->webView->RemoveScriptToExecuteOnDocumentCreated(id);
                this->LeaveHybridcastState();
                return S_OK;
            }
            this->hybridcast.scriptId = id;
            this->hybridcast.launching = false;
            this->hybridcast.active = true;
            // アプリのJavaScriptの例外とconsole.errorを受け取れるようにする
            this->webView->CallDevToolsProtocolMethod(L"Runtime.enable", L"{}", nullptr);
            this->HybridcastLog(L"INFO", L"アプリの読み込みを開始します: " + this->HybridcastLogUrl(entryUrl));
            this->webView->Navigate(entryUrl.c_str());
            return S_OK;
        }).Get());
        if (FAILED(hr))
        {
            this->HybridcastLog(L"ERROR", L"起動できません: 拡張APIの登録を開始できません hr=" + std::to_wstring((long)hr));
            this->LeaveHybridcastState();
        }
    }
    catch (const std::exception&)
    {
        this->HybridcastLog(L"ERROR", L"起動要求の内容が不正なため、無視しました");
        this->LeaveHybridcastState();
    }
}

// マネージド状態を終了する。videoOnly なら「放送受信映像のみ」の状態へ(STD-0013 8.1.3.10.1, 8.1.3.10.2)
void CDataBroadcastingWV2::ExitHybridcast(bool videoOnly)
{
    if (!this->InHybridcastMode())
    {
        return;
    }
    this->HybridcastLog(L"INFO", videoOnly ? L"アプリを終了しました: 放送受信映像のみの状態に戻ります" : L"アプリを終了しました: データ放送に戻ります");
    long long key = ((long long)this->hybridcast.networkId << 16) | this->hybridcast.serviceId;
    this->LeaveHybridcastState();
    this->hybridcast.videoOnlyKey = videoOnly ? key : 0;
    // 起動したサービスのデータ放送ページへ戻る(同じサービスでのAIT自動起動は consumedKey で抑止される)
    this->Tune();
}

// exitFromManagedState: 一般アプリケーションへ、アンマネージド状態で遷移する(STD-0011 3.1.3.3, STD-0013 8.1.3.10.1)。
// 映像・音声は直ちに提示を中止する。拡張APIは注入しない。選局でサービスが変わったら終了する。
void CDataBroadcastingWV2::EnterUnmanaged(const std::wstring& uri)
{
    if (!this->hybridcast.active || !this->webView)
    {
        return;
    }
    this->HybridcastLog(L"INFO", L"アプリが一般アプリケーションへの遷移を要求しました(映像と音声を止めます): " + this->HybridcastLogUrl(uri));
    // 音声の元の状態(savedMute)は保ったまま、スクリプトとUser-Agentだけを元に戻す
    bool savedMuteValid = this->hybridcast.savedMuteValid;
    bool savedMute = this->hybridcast.savedMute;
    this->hybridcast.savedMuteValid = false;
    this->LeaveHybridcastState();
    this->hybridcast.savedMuteValid = savedMuteValid;
    this->hybridcast.savedMute = savedMute;
    this->hybridcast.unmanaged = true;
    this->m_pApp->SetMute(true);
    this->invisible = false;
    this->videoRect = RECT{ 0, 0, 1, 1 };
    this->ResizeVideoWindow();
    this->webView->Navigate(uri.c_str());
}

// ハイブリッドキャストのアプリ(外部ページ)から届いたメッセージ。プラグイン用のメッセージは一切受け付けない。
void CDataBroadcastingWV2::OnHybridcastAppMessage(const std::string& type, const nlohmann::json& a)
{
    // マネージド状態のアプリからだけ受け付ける(通常時やアンマネージド中の外部ページは無視)
    if (!this->hybridcast.active)
    {
        std::wstring shortType = utf8StrToWString(type.substr(0, 64).c_str());
        this->HybridcastLog(L"WARN", L"プラグイン以外のページからのメッセージを無視しました: " + shortType);
        return;
    }
    try
    {
        if (type != "videoChanged")
        {
            this->HybridcastLog(L"INFO", L"アプリからの要求: " + utf8StrToWString(type.substr(0, 64).c_str()));
        }
        if (type == "videoChanged")
        {
            auto left = a.at("left").get<double>();
            auto right = a.at("right").get<double>();
            auto top = a.at("top").get<double>();
            auto bottom = a.at("bottom").get<double>();
            auto inRange = [](double v) { return v > -100000.0 && v < 100000.0; };
            if (!inRange(left) || !inRange(right) || !inRange(top) || !inRange(bottom))
            {
                return;
            }
            RECT r;
            r.left = (int)std::floor(left);
            r.right = (int)std::ceil(right);
            r.top = (int)std::floor(top);
            r.bottom = (int)std::ceil(bottom);
            this->invisible = false;
            this->videoRect = r;
            this->ResizeVideoWindow();
        }
        else if (type == "hc.audioMute")
        {
            this->m_pApp->SetMute(a.at("enable").get<bool>());
        }
        else if (type == "hc.destroyApplication")
        {
            this->ExitHybridcast(true);
        }
        else if (type == "hc.launchDataBroadcastingBrowser")
        {
            this->ExitHybridcast(false);
        }
        else if (type == "hc.exitFromManagedState")
        {
            auto uri = utf8StrToWString(a.at("uri").get<std::string>().c_str());
            // 最大250文字のhttp/https(STD-0013 表 8-11)。API側でも検査済みだが、ホスト側でも再検査する
            if (uri.empty() || uri.size() > 250 ||
                (_wcsnicmp(uri.c_str(), L"http://", 7) && _wcsnicmp(uri.c_str(), L"https://", 8)))
            {
                this->HybridcastLog(L"WARN", L"exitFromManagedState の遷移先が不正なため、無視しました");
                return;
            }
            this->EnterUnmanaged(uri);
        }
        else if (type == "hc.addPermissionManagedArea")
        {
            // バウンダリの拡張(STD-0010 7.6.1)。urls が null(その他すべてのロケーション)は、安全のためバウンダリに含めない
            if (a.contains("urls") && a["urls"].is_array())
            {
                for (auto& u : a["urls"])
                {
                    auto url = utf8StrToWString(u.get<std::string>().c_str());
                    if (ParseHttpUrl(url, true).valid)
                    {
                        this->hybridcast.boundary.push_back(url);
                        this->HybridcastLog(L"INFO", L"バウンダリを追加しました: " + this->HybridcastLogUrl(url));
                    }
                }
            }
        }
    }
    catch (const std::exception&)
    {
        // 型違いなど不正なメッセージは無視する
    }
}

LRESULT CALLBACK CDataBroadcastingWV2::EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    return pThis->HandleEvent(Event, lParam1, lParam2, pClientData);
}

bool CDataBroadcastingWV2::OnStatusItemDraw(TVTest::StatusItemDrawInfo* pInfo)
{
    std::wstring statusItem;
    if ((pInfo->Flags & TVTest::STATUS_ITEM_DRAW_FLAG_PREVIEW) == 0)
    {
        if (this->status.receiving)
        {
            statusItem += L"データ取得中...";
        }
        statusItem += this->status.url;
        if (this->status.loading)
        {
            statusItem += L"を読み込み中...";
        }
    }
    else
    {
        statusItem = L"データ取得中...";
    }
    this->m_pApp->ThemeDrawText(pInfo->pszStyle, pInfo->hdc, statusItem.c_str(),
        pInfo->DrawRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
        pInfo->Color);

    return true;
}

bool CDataBroadcastingWV2::OnFullscreenChange(bool fFullscreen)
{
    if (this->hContainerWnd && this->hMessageWnd)
    {
        this->inputDialog = nullptr;
#if 0
        if (!this->invisible)
        {
            // 無理やり動画ウィンドウを移動させている都合上リサイズ時に位置大きさが初期化されてしまうので一時的に非表示にさせる
            SetWindowPos(this->hVideoWnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW);
        }
#endif
        // 実際に動画ウィンドウの大きさが変わるのはメッセージ処理後なのでPostMessageでやり過ごす
        PostMessageW(this->hMessageWnd, WM_APP_RESIZE, 0, 0);
    }
    return true;
}

void CDataBroadcastingWV2::UpdateCaptionState(bool showIndicator)
{
    if (!this->webView)
    {
        return;
    }
    nlohmann::json msg{ { "type", "caption" }, { "enable", this->caption }, { "showIndicator", showIndicator } };
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

void CDataBroadcastingWV2::UpdateVolume()
{
    if (!this->webView)
    {
        return;
    }
    nlohmann::json msg{ { "type", "volume" }, { "value", this->useTVTestVolume ? this->currentVolume / (double)MAX_VOLUME : 1.0 } };
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

void CDataBroadcastingWV2::SetCaptionState(bool enable)
{
    this->caption = enable;
    if (this->hRemoteWnd)
    {
        SendDlgItemMessageW(this->hRemoteWnd, IDC_TOGGLE_CAPTION, BM_SETCHECK, this->caption ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (this->hPanelWnd)
    {
        SendDlgItemMessageW(this->hPanelWnd, IDC_TOGGLE_CAPTION, BM_SETCHECK, this->caption ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    this->UpdateCaptionState(true);
}

void CDataBroadcastingWV2::UpdateNetworkToggleButton(HWND hWnd)
{
    auto label = this->enableNetwork ? L"通信機能を無効化" : L"通信機能を有効化";
    SetDlgItemTextW(hWnd, IDC_TOGGLE_NETWORK, label);
}

void CDataBroadcastingWV2::SetNetworkState(bool enable)
{
    this->enableNetwork = enable;
    if (this->hRemoteWnd)
    {
        this->UpdateNetworkToggleButton(this->hRemoteWnd);
    }
    if (this->hPanelWnd)
    {
        this->UpdateNetworkToggleButton(this->hPanelWnd);
    }
    this->UpdateNetworkState();
}

void CDataBroadcastingWV2::UpdateNetworkState()
{
    if (!this->webView)
    {
        return;
    }
    nlohmann::json msg{ { "type", "enableNetwork" }, { "enable", this->enableNetwork } };
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

enum class UsedKeyType
{
    Unused,
    Basic,
    DataButton,
    NumericTuning,
    Special1,
};

struct CommandInfo
{
    int keyCode;
    UsedKeyType usedKeyType;
    LPCWSTR commandName;
};

static std::unordered_map<int, CommandInfo> commandList
{
    { IDC_KEY_UP, { 1, UsedKeyType::Basic } },
    { IDC_KEY_DOWN, { 2, UsedKeyType::Basic } },
    { IDC_KEY_LEFT, { 3, UsedKeyType::Basic } },
    { IDC_KEY_RIGHT, { 4, UsedKeyType::Basic } },
    { IDC_KEY_ENTER, { 18, UsedKeyType::Basic } },
    { IDC_KEY_BACK, { 19, UsedKeyType::Basic } },
    { IDC_KEY_BLUE, { 21, UsedKeyType::DataButton } },
    { IDC_KEY_RED, { 22, UsedKeyType::DataButton } },
    { IDC_KEY_GREEN, { 23, UsedKeyType::DataButton } },
    { IDC_KEY_YELLOW, { 24, UsedKeyType::DataButton } },
    { IDC_KEY_0, { 5, UsedKeyType::NumericTuning } },
    { IDC_KEY_1, { 6, UsedKeyType::NumericTuning, L"Channel1" } },
    { IDC_KEY_2, { 7, UsedKeyType::NumericTuning, L"Channel2" } },
    { IDC_KEY_3, { 8, UsedKeyType::NumericTuning, L"Channel3" } },
    { IDC_KEY_4, { 9, UsedKeyType::NumericTuning, L"Channel4" } },
    { IDC_KEY_5, { 10, UsedKeyType::NumericTuning, L"Channel5" } },
    { IDC_KEY_6, { 11, UsedKeyType::NumericTuning, L"Channel6" } },
    { IDC_KEY_7, { 12, UsedKeyType::NumericTuning, L"Channel7" } },
    { IDC_KEY_8, { 13, UsedKeyType::NumericTuning, L"Channel8" } },
    { IDC_KEY_9, { 14, UsedKeyType::NumericTuning, L"Channel9" } },
    { IDC_KEY_10, { 15, UsedKeyType::NumericTuning, L"Channel10" } },
    { IDC_KEY_11, { 16, UsedKeyType::NumericTuning, L"Channel11" } },
    { IDC_KEY_12, { 17, UsedKeyType::NumericTuning, L"Channel12" } },
};

static std::unordered_map<int, CommandInfo> oneSegCommandList
{
    { IDC_KEY_UP, { 1, UsedKeyType::Basic } },
    { IDC_KEY_DOWN, { 2, UsedKeyType::Basic } },
    { IDC_KEY_LEFT, { 3, UsedKeyType::Basic } },
    { IDC_KEY_RIGHT, { 4, UsedKeyType::Basic } },
    { IDC_KEY_ENTER, { 18, UsedKeyType::Basic } },
    { IDC_KEY_BACK, { 19, UsedKeyType::Basic } },
    { IDC_KEY_0, { 5, UsedKeyType::NumericTuning } },
    { IDC_KEY_1, { 6, UsedKeyType::NumericTuning, L"Channel1" } },
    { IDC_KEY_2, { 7, UsedKeyType::NumericTuning, L"Channel2" } },
    { IDC_KEY_3, { 8, UsedKeyType::NumericTuning, L"Channel3" } },
    { IDC_KEY_4, { 9, UsedKeyType::NumericTuning, L"Channel4" } },
    { IDC_KEY_5, { 10, UsedKeyType::NumericTuning, L"Channel5" } },
    { IDC_KEY_6, { 11, UsedKeyType::NumericTuning, L"Channel6" } },
    { IDC_KEY_7, { 12, UsedKeyType::NumericTuning, L"Channel7" } },
    { IDC_KEY_8, { 13, UsedKeyType::NumericTuning, L"Channel8" } },
    { IDC_KEY_9, { 14, UsedKeyType::NumericTuning, L"Channel9" } },
    { IDC_KEY_10, { 101, UsedKeyType::Special1, L"Channel10" } },
    { IDC_KEY_11, { 16, UsedKeyType::Unused, L"Channel11" } },
    { IDC_KEY_12, { 102, UsedKeyType::Special1, L"Channel12" } },
};

void CDataBroadcastingWV2::ShowRemoteControlDialog()
{
    if (!this->hRemoteWnd)
    {
        TVTest::ShowDialogInfo Info;

        Info.Flags = TVTest::SHOW_DIALOG_FLAG_MODELESS;
        Info.hinst = g_hinstDLL;
        Info.pszTemplate = MAKEINTRESOURCE(IDD_REMOTE_CONTROL);
        Info.pMessageFunc = RemoteControlDlgProc;
        Info.pClientData = this;
        Info.hwndOwner = this->m_pApp->GetAppWindow();

        if ((HWND)this->m_pApp->ShowDialog(&Info) != nullptr && this->hRemoteWnd != nullptr)
        {
            ShowWindow(this->hRemoteWnd, SW_SHOW);
        }
    }
}

bool CDataBroadcastingWV2::OnCommand(int ID)
{
    if (this->m_pApp->IsPluginEnabled())
    {
        switch (ID)
        {
        case IDC_SHOW_REMOTE_CONTROL:
        {
            this->ShowRemoteControlDialog();
            return true;
        }
        case IDC_KEY_SETTINGS:
        {
            if (this->m_pApp->GetFullscreen())
            {
                return this->OnPluginSettings(this->GetFullscreenWindow());
            }
            return this->OnPluginSettings(this->m_pApp->GetAppWindow());
        }
        case IDC_TOGGLE_NETWORK:
        {
            this->SetNetworkState(!this->enableNetwork);
            return true;
        }
        case IDC_TOGGLE_CAPTION:
            this->SetCaptionState(!this->caption);
            return true;
        case IDC_ENABLE_CAPTION:
            this->SetCaptionState(true);
            return true;
        case IDC_DISABLE_CAPTION:
            this->SetCaptionState(false);
            return true;
        }
    }
    if (!this->webView)
    {
        if (ID == IDC_KEY_D_OR_ENABLE_PLUGIN)
        {
            if (!this->m_pApp->IsPluginEnabled())
            {
                this->m_pApp->EnablePlugin(true);
                return true;
            }
        }
        auto command = commandList.find(ID);
        if (command != commandList.end())
        {
            if (command->second.commandName)
            {
                if (this->useTVTestChannelCommand)
                {
                    this->m_pApp->DoCommand(command->second.commandName);
                }
            }
        }
        return false;
    }
    switch (ID)
    {
    case IDC_KEY_D_OR_ENABLE_PLUGIN:
    case IDC_KEY_D:
        if (this->currentServiceIsOneSeg)
        {
            this->CreateOneSegWindow();
            this->webView->PostWebMessageAsJson(LR"({"type":"launchOneSeg"})");
        }
        else
        {
            this->webView->PostWebMessageAsJson(LR"({"type":"key","keyCode":20})");
        }
        break;
    case IDC_KEY_DEVTOOL:
        this->webView->OpenDevToolsWindow();
        break;
    case IDC_KEY_RELOAD:
        this->webView->Reload();
        break;
    case IDC_TASKMANAGER:
    {
        auto webView6 = this->webView.try_query<ICoreWebView2_6>();
        if (webView6)
        {
            webView6->OpenTaskManagerWindow();
        }
        break;
    }
    default:
    {
        auto&& list = this->oneSegWindowIsShown ? oneSegCommandList : commandList;
        auto command = list.find(ID);
        if (command != list.end())
        {
            bool post = false;
            switch (command->second.usedKeyType)
            {
            case UsedKeyType::Basic:
                post = this->usedKey.basic;
                break;
            case UsedKeyType::DataButton:
                post = this->usedKey.dataButton;
                break;
            case UsedKeyType::NumericTuning:
                post = this->usedKey.numericTuning;
                break;
            case UsedKeyType::Special1:
                post = this->usedKey.special1;
                break;
            }
            if (post)
            {
                nlohmann::json msg{ { "type", "key" }, { "keyCode", command->second.keyCode } };
                std::stringstream ss;
                ss << msg;
                auto wjson = utf8StrToWString(ss.str().c_str());
                this->webView->PostWebMessageAsJson(wjson.c_str());
            }
            else if (command->second.commandName)
            {
                if (this->useTVTestChannelCommand)
                {
                    this->m_pApp->DoCommand(command->second.commandName);
                }
            }
        }
    }
    }
    return true;
}

INT_PTR CALLBACK CDataBroadcastingWV2::RemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg) {
    case WM_INITDIALOG:
    {
        pThis->hRemoteWnd = hDlg;
        if (pThis->caption)
        {
            SendDlgItemMessageW(hDlg, IDC_TOGGLE_CAPTION, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"EnableNetwork", 0))
        {
            SetWindowLongW(GetDlgItem(hDlg, IDC_TOGGLE_NETWORK), GWL_STYLE, GetWindowLongW(GetDlgItem(hDlg, IDC_TOGGLE_NETWORK), GWL_STYLE) | WS_VISIBLE);
            pThis->UpdateNetworkToggleButton(hDlg);
        }
        pThis->UpdateDigitButton();
        return 1;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDC_TOGGLE_CAPTION)
        {
            if (SendMessageW((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED)
            {
                pThis->OnCommand(IDC_ENABLE_CAPTION);
            }
            else
            {
                pThis->OnCommand(IDC_DISABLE_CAPTION);
            }
        }
        else
        {
            pThis->OnCommand(LOWORD(wParam));
        }
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    {
        static const COLORREF crBRGY[4] = { RGB(0, 114, 214), RGB(201, 0, 0), RGB(27, 135, 0), RGB(227, 178, 0) };
        int id = GetDlgCtrlID((HWND)lParam);
        int i = id == IDC_STATIC_BLUE ? 0 : id == IDC_STATIC_RED ? 1 :
                id == IDC_STATIC_GREEN ? 2 : id == IDC_STATIC_YELLOW ? 3 : -1;
        if (i >= 0)
        {
            // スタティックコントロールの背景色を変える
            SetBkColor((HDC)wParam, crBRGY[i]);
            if (!pThis->hbrBRGYBacks[i])
            {
                pThis->hbrBRGYBacks[i] = CreateSolidBrush(crBRGY[i]);
            }
            return (INT_PTR)pThis->hbrBRGYBacks[i];
        }
        break;
    }
    case WM_CLOSE:
    {
        DestroyWindow(hDlg);
        return 1;
    }
    case WM_DESTROY:
    {
        pThis->hRemoteWnd = nullptr;
        return 1;
    }
    }
    return 0;
}

void CDataBroadcastingWV2::EnablePanelButtons(bool enable)
{
    if (!this->hPanelWnd)
    {
        return;
    }
    struct Args
    {
        bool enable;
        bool useTVTestChannelCommand;
    };
    Args args = { enable, this->useTVTestChannelCommand };
    EnumChildWindows(this->hPanelWnd, [](HWND hWnd, LPARAM lParam) -> BOOL {
        auto args = (Args*)lParam;
        auto id = GetDlgCtrlID(hWnd);
        if (id != IDC_KEY_D_OR_ENABLE_PLUGIN && id != IDC_KEY_SETTINGS)
        {
            if (args->useTVTestChannelCommand)
            {
                auto command = commandList.find(id);
                if (command == commandList.end() || !command->second.commandName)
                {
                    EnableWindow(hWnd, args->enable);
                }
                else
                {
                    EnableWindow(hWnd, true);
                }
            }
            else
            {
                EnableWindow(hWnd, args->enable);
            }
        }
        return true;
    }, (LPARAM)&args);
}

INT_PTR CALLBACK CDataBroadcastingWV2::PanelRemoteControlDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        pThis->hPanelWnd = hDlg;
        if (pThis->caption)
        {
            SendDlgItemMessageW(hDlg, IDC_TOGGLE_CAPTION, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"EnableNetwork", 0))
        {
            SetWindowLongW(GetDlgItem(hDlg, IDC_TOGGLE_NETWORK), GWL_STYLE, GetWindowLongW(GetDlgItem(hDlg, IDC_TOGGLE_NETWORK), GWL_STYLE) | WS_VISIBLE);
            pThis->UpdateNetworkToggleButton(hDlg);
        }
        pThis->panelInitialDpi = GetDpi(hDlg);
        // アイテムの初期位置を記録
        pThis->panelItems.clear();
        EnumChildWindows(hDlg, [](HWND hWnd, LPARAM lParam) -> BOOL {
            RECT rect;
            if (GetWindowRect(hWnd, &rect))
            {
                ((std::vector<std::pair<HWND, RECT>>*)lParam)->emplace_back(hWnd, rect);
            }
            return true;
        }, (LPARAM)&pThis->panelItems);
        SendMessageW(hDlg, WM_APP_ON_PANEL_COLOR_CHANGE, 0, 0);
        SendMessageW(hDlg, WM_APP_ON_PANEL_SIZE, 0, 0);
        return 1;
    }
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
        return (INT_PTR)pThis->hbrPanelBack;
    case WM_DESTROY:
    {
        pThis->hPanelWnd = nullptr;
        return 1;
    }
    case WM_SIZE:
    {
        SendMessageW(hDlg, WM_APP_ON_PANEL_SIZE, 0, 0);
        break;
    }
    case WM_APP_ON_PANEL_COLOR_CHANGE:
    {
        COLORREF crBack = pThis->m_pApp->GetColor(L"PanelBack");
        if (pThis->hbrPanelBack)
        {
            DeleteObject(pThis->hbrPanelBack);
        }
        pThis->hbrPanelBack = CreateSolidBrush(crBack);
        if (pThis->m_pApp->GetDarkModeStatus() & TVTest::DARK_MODE_STATUS_PANEL_SUPPORTED)
        {
            // 必要に応じてボタンをダークモードにする
            bool dark = pThis->m_pApp->IsDarkModeColor(crBack);
            pThis->m_pApp->SetWindowDarkMode(hDlg, dark);
            for (const auto& item : pThis->panelItems)
            {
                WCHAR className[100];
                if (GetClassNameW(item.first, className, _countof(className)) && !_wcsicmp(className, L"BUTTON"))
                {
                    pThis->m_pApp->SetWindowDarkMode(item.first, dark);
                }
            }
        }
        InvalidateRect(hDlg, nullptr, TRUE);
        return 0;
    }
    case WM_APP_ON_PANEL_SIZE:
    {
        RECT clientRect;
        if (!pThis->panelItems.empty() && GetClientRect(hDlg, &clientRect))
        {
            // アイテムの縦方向が領域に収まらないときは縮める
            RECT unionRect = pThis->panelItems.front().second;
            for (const auto& item : pThis->panelItems)
            {
                if (GetWindowLongW(item.first, GWL_STYLE) & WS_VISIBLE)
                {
                    RECT rect = unionRect;
                    UnionRect(&unionRect, &rect, &item.second);
                }
            }
            UINT dpi = GetDpi(hDlg);
            double scaleY = (double)clientRect.bottom / MulDiv(unionRect.bottom - unionRect.top, dpi, pThis->panelInitialDpi);
            HDWP hDwp = BeginDeferWindowPos(pThis->panelItems.size());
            scaleY = scaleY < 0.6 ? 0.6 : scaleY < 1 ? scaleY : 1;
            for (const auto& item : pThis->panelItems)
            {
                int x = item.second.left - unionRect.left;
                int y = (int)((item.second.top - unionRect.top) * scaleY);
                int width = item.second.right - item.second.left;
                int height = (int)((item.second.bottom - unionRect.top) * scaleY) - y;
                hDwp = DeferWindowPos(hDwp, item.first, nullptr, MulDiv(x, dpi, pThis->panelInitialDpi), MulDiv(y, dpi, pThis->panelInitialDpi), MulDiv(width, dpi, pThis->panelInitialDpi), MulDiv(height, dpi, pThis->panelInitialDpi), SWP_NOZORDER | SWP_NOACTIVATE);
            }
            EndDeferWindowPos(hDwp);
        }
        return 0;
    }
    case WM_DPICHANGED:
    {
        pThis->SetPanelFont();
        SendMessageW(hDlg, WM_APP_ON_PANEL_SIZE, 0, 0);
        return 0;
    }
    }
    return RemoteControlDlgProc(hDlg, uMsg, wParam, lParam, pClientData);
}

INT_PTR CALLBACK CDataBroadcastingWV2::SettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    CDataBroadcastingWV2* pThis = static_cast<CDataBroadcastingWV2*>(pClientData);
    switch (uMsg) {
    case WM_INITDIALOG:
    {
        if (pThis->GetIniItem(L"DisableRemoteControl", 0))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_DISABLE_REMOTECON, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"AutoEnable", 0))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_AUTOENABLE, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"UseTVTestVolume", 1))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_USE_TVTEST_VOLUME, BM_SETCHECK, BST_CHECKED, 0);
        }
        if (pThis->GetIniItem(L"UseTVTestChannelCommand", 1))
        {
            SendDlgItemMessageW(hDlg, IDC_CHECK_USE_TVTEST_CHANNEL_COMMAND, BM_SETCHECK, BST_CHECKED, 0);
        }
        return 1;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            if (LOWORD(wParam) == IDOK)
            {
                auto disableRemoteControl = SendDlgItemMessageW(hDlg, IDC_CHECK_DISABLE_REMOTECON, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"DisableRemoteControl", disableRemoteControl ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                auto autoEnable = SendDlgItemMessageW(hDlg, IDC_CHECK_AUTOENABLE, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"AutoEnable", autoEnable ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                pThis->useTVTestVolume = SendDlgItemMessageW(hDlg, IDC_CHECK_USE_TVTEST_VOLUME, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"UseTVTestVolume", pThis->useTVTestVolume ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                pThis->UpdateVolume();
                pThis->useTVTestChannelCommand = SendDlgItemMessageW(hDlg, IDC_CHECK_USE_TVTEST_CHANNEL_COMMAND, BM_GETCHECK, 0, 0);
                if (!pThis->SetIniItem(L"UseTVTestChannelCommand", pThis->useTVTestChannelCommand ? L"1" : L"0"))
                {
                    MessageBoxW(hDlg, L"設定を保存できませんでした", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
                    EndDialog(hDlg, IDCANCEL);
                    return 1;
                }
                pThis->EnablePanelButtons(pThis->m_pApp->IsPluginEnabled());
            }
            EndDialog(hDlg, LOWORD(wParam));
        }
        else if (LOWORD(wParam) == IDC_BUTTON_NVRAM_SETTING)
        {
            if (!pThis->webView)
            {
                MessageBoxW(hDlg, L"プラグインが有効の間のみ郵便番号・保存領域の設定を行えます。", L"TVTDataBroadcastingWV2の設定", MB_ICONERROR | MB_OK);
            }
            else
            {
                TVTest::ShowDialogInfo Info;

                Info.Flags = 0;
                Info.hinst = g_hinstDLL;
                Info.pszTemplate = MAKEINTRESOURCE(IDD_SETTING_NVRAM);
                Info.pMessageFunc = NVRAMSettingsDialog::DlgProc;
                std::unique_ptr< NVRAMSettingsDialog> dialog(new NVRAMSettingsDialog(pThis->webView));
                Info.pClientData = dialog.get();
                Info.hwndOwner = hDlg;

                pThis->m_pApp->ShowDialog(&Info);
            }
        }
        return 1;
    }
    case WM_CLOSE:
    {
        EndDialog(hDlg, IDCANCEL);
        return 1;
    }
    case WM_DESTROY:
    {
        return 1;
    }
    }
    return 0;
}

bool CDataBroadcastingWV2::OnPluginSettings(HWND hwndOwner)
{
    TVTest::ShowDialogInfo Info;

    Info.Flags = 0;
    Info.hinst = g_hinstDLL;
    Info.pszTemplate = MAKEINTRESOURCE(IDD_SETTING);
    Info.pMessageFunc = SettingsDlgProc;
    Info.pClientData = this;
    Info.hwndOwner = hwndOwner;

    auto result = this->m_pApp->ShowDialog(&Info);
    return result == IDOK;
}

bool CDataBroadcastingWV2::OnColorChange()
{
    if (this->hPanelWnd)
    {
        SendMessageW(this->hPanelWnd, WM_APP_ON_PANEL_COLOR_CHANGE, 0, 0);
    }
    return true;
}

bool CDataBroadcastingWV2::OnPanelItemNotify(TVTest::PanelItemEventInfo* pInfo)
{
    switch (pInfo->Event)
    {
    case TVTest::PANEL_ITEM_EVENT_CREATE:
    {
        auto createEventInfo = CONTAINING_RECORD(pInfo, TVTest::PanelItemCreateEventInfo, EventInfo);
        TVTest::ShowDialogInfo Info;

        Info.Flags = TVTest::SHOW_DIALOG_FLAG_MODELESS;
        Info.hinst = g_hinstDLL;
        Info.pszTemplate = MAKEINTRESOURCE(IDD_REMOTE_CONTROL_PANEL);
        Info.pMessageFunc = PanelRemoteControlDlgProc;
        Info.pClientData = this;
        Info.hwndOwner = createEventInfo->hwndParent;
        // できればこのようにしたいが、親がダイアログでない限りWS_CHILDなダイアログはうまくダークモードにならない
        // auto hWnd = (HWND)this->m_pApp->ShowDialog(&Info);
        // 本体側のダークモード関連の処理と競合するため素のダイアログを使う
        HWND hWnd = CreateDialogParamW(Info.hinst, Info.pszTemplate, Info.hwndOwner,
                                       [](HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) -> INT_PTR {
            if (uMsg == WM_INITDIALOG)
            {
                SetWindowLongPtrW(hDlg, GWLP_USERDATA, lParam);
            }
            auto pClientData = (void*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
            return pClientData ? PanelRemoteControlDlgProc(hDlg, uMsg, wParam, lParam, pClientData) : 0;
        }, (LPARAM)Info.pClientData);
        ShowWindow(hWnd, SW_SHOW);
        createEventInfo->hwndItem = hWnd;
        this->EnablePanelButtons(this->m_pApp->IsPluginEnabled());
    }
    [[fallthrough]];
    case TVTest::PANEL_ITEM_EVENT_FONTCHANGED:
    {
        this->SetPanelFont();
        break;
    }
    }
    return true;
}

void CDataBroadcastingWV2::SetPanelFont()
{
    // フォントが大きすぎるとはみ出してしまうのでフォントの大きさに合わせてボタンの大きさを変更すべきではある
    // TVTestがやっているように全部自前で描画して処理するのは大変なのでダイアログで妥協
    if (this->hPanelFont)
    {
        DeleteObject(this->hPanelFont);
    }
    LOGFONTW lf;
    this->m_pApp->GetFont(L"PanelFont", &lf, GetDpi(this->hPanelWnd));
    this->hPanelFont = CreateFontIndirectW(&lf);
    SendMessageW(this->hPanelWnd, WM_SETFONT, (WPARAM)this->hPanelFont, true);
    EnumChildWindows(this->hPanelWnd, [](HWND hWnd, LPARAM lParam) -> BOOL {
        auto hFont = (HFONT)lParam;
        SendMessageW(hWnd, WM_SETFONT, (WPARAM)hFont, 0);
        return true;
    }, (LPARAM)this->hPanelFont);
}

bool CDataBroadcastingWV2::OnVolumeChange(int Volume, bool fMute)
{
    this->currentVolume = fMute ? 0 : Volume;
    this->UpdateVolume();
    return true;
}

void CDataBroadcastingWV2::UpdateAudioStream()
{
    if (this->isPlayingMainAudio)
    {
        this->mainAudio = this->GetSelectedAudio();
    }
    if (!this->webView)
    {
        return;
    }
    nlohmann::json msg{ { "type", "mainAudioStreamChanged" } };
    if (this->mainAudio.componentId.has_value())
    {
        msg["componentId"] = this->mainAudio.componentId.value();
    }
    else if (this->mainAudio.index.has_value())
    {
        auto index = this->mainAudio.index.value();
        msg["index"] = index;
        if (index >= 0 && _countof(this->currentService.AudioPID) > index && this->currentService.NumAudioPIDs > index)
        {
            msg.push_back({ "pid", this->currentService.AudioPID[index] });
        }
    }
    else
    {
        return;
    }
    if (this->mainAudio.getChannelId().has_value())
    {
        msg["channelId"] = this->mainAudio.getChannelId().value();
    }
    std::stringstream ss;
    ss << msg;
    auto wjson = utf8StrToWString(ss.str().c_str());
    this->webView->PostWebMessageAsJson(wjson.c_str());
}

// Index=1からサービスを変更したとしてもOnAudioStreamChangeは呼ばれない
// ただしOnStereoModeChangeは複数回呼ばれるので問題なさそう

bool CDataBroadcastingWV2::OnAudioStreamChange(int Stream)
{
    UNREFERENCED_PARAMETER(Stream);
    this->UpdateAudioStream();
    return true;
}


bool CDataBroadcastingWV2::OnStereoModeChange(int StereoMode)
{
    UNREFERENCED_PARAMETER(StereoMode);
    this->UpdateAudioStream();
    return true;
}

void CDataBroadcastingWV2::OnAudioFormatChange()
{
    this->UpdateAudioStream();
}

void CDataBroadcastingWV2::RestoreMainAudio()
{
    if (this->isPlayingMainAudio)
    {
        return;
    }
    this->isPlayingMainAudio = true;
    this->SelectAudio(this->mainAudio);
}

void CDataBroadcastingWV2::SelectAudio(Audio audio)
{
    if (audio.componentId.has_value())
    {
        TVTest::AudioSelectInfo asi = { sizeof(asi) };
        asi.ComponentTag = audio.componentId.value();
        asi.Flags = TVTest::AUDIO_SELECT_FLAG_COMPONENT_TAG;
        if (audio.dualMonoChannel.has_value())
        {
            asi.Flags |= TVTest::AUDIO_SELECT_FLAG_DUAL_MONO;
            asi.DualMono = audio.dualMonoChannel.value();
        }
        this->m_pApp->SelectAudio(&asi);
    }
    else if (audio.index.has_value())
    {
        this->m_pApp->SetAudioStream(audio.index.value());
    }
}

Audio CDataBroadcastingWV2::GetSelectedAudio()
{
    TVTest::AudioSelectInfo asi = { sizeof(asi) };
    if (this->m_pApp->GetSelectedAudio(&asi))
    {
        if (asi.Index == -1)
        {
            return Audio();
        }
        return Audio(asi.ComponentTag, asi.DualMono);
    }
    auto index = this->m_pApp->GetAudioStream();
    return Audio(index);
}

void CDataBroadcastingWV2::CreateOneSegWindow()
{
    if (this->hOneSegWnd != nullptr)
    {
        return;
    }
    this->oneSegWindowIsShown = true;
    this->oneSegWindow = std::make_unique<OneSegWindow>(
        this->m_pApp->GetAppWindow(),
        g_hinstDLL,
        this->hContainerWnd,
        this->webViewController,
        [this]()
        {
            this->oneSegWindowIsShown = false;
            this->webViewController->put_ParentWindow(this->hContainerWnd);
            PostMessageW(this->hMessageWnd, WM_APP_RESIZE, 0, 0);
            this->hOneSegWnd = nullptr;
            if (this->currentServiceIsOneSeg)
            {
                this->webView->Reload();
            }
            this->UpdateDigitButton();
        }
    );
    this->hOneSegWnd = this->oneSegWindow->GetWindowHandle();
    this->OnDarkModeChanged(this->m_pApp->GetDarkModeStatus() & TVTest::DARK_MODE_STATUS_DIALOG_DARK);
    this->UpdateDigitButton();
}

void CDataBroadcastingWV2::DestroyOneSegWindow()
{
    this->oneSegWindow = nullptr;
}

void CDataBroadcastingWV2::OnDarkModeChanged(bool fDarkMode)
{
    if (this->hOneSegWnd != nullptr)
    {
        this->m_pApp->SetWindowDarkMode(this->hOneSegWnd, this->m_pApp->GetDarkModeStatus() & TVTest::DARK_MODE_STATUS_DIALOG_DARK);
    }
}

void CDataBroadcastingWV2::UpdateDigitButton()
{
    if (this->oneSegWindowIsShown)
    {
        SetDlgItemTextW(this->hRemoteWnd, IDC_KEY_10, L"＊/10");
        SetDlgItemTextW(this->hRemoteWnd, IDC_KEY_12, L"＃/12");
        SetDlgItemTextW(this->hPanelWnd, IDC_KEY_10, L"＊/10");
        SetDlgItemTextW(this->hPanelWnd, IDC_KEY_12, L"＃/12");
    }
    else
    {
        SetDlgItemTextW(this->hRemoteWnd, IDC_KEY_10, L"10");
        SetDlgItemTextW(this->hRemoteWnd, IDC_KEY_12, L"12");
        SetDlgItemTextW(this->hPanelWnd, IDC_KEY_10, L"10");
        SetDlgItemTextW(this->hPanelWnd, IDC_KEY_12, L"12");
    }
}

TVTest::CTVTestPlugin* CreatePluginClass()
{
    return new CDataBroadcastingWV2;
}
