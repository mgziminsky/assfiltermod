/*
 *   Copyright(C) 2016-2017 Blitzker
 *
 *   This program is free software : you can redistribute it and / or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.If not, see <http://www.gnu.org/licenses/>.
 */

#include "stdafx.h"

#include "AssFilter.h"
#include "AssDebug.h"
#include "AssPin.h"
#include "AssFilterSettingsProps.h"
#include "registry.h"
#include "resource.h"
#include "SubFrame.h"
#include "Tools.h"

AssFilter::AssFilter(LPUNKNOWN pUnk, HRESULT* pResult)
	: CBaseFilter(NAME("AssFilterMod"), pUnk, this, __uuidof(AssFilter))
{
    if (pResult)
        *pResult = S_OK;

    m_pin = std::make_unique<AssPin>(this, pResult);

    m_ass = decltype(m_ass)(ass_library_init());
    m_renderer = decltype(m_renderer)(ass_renderer_init(m_ass.get()));

    m_stringOptions["name"] = L"AssFilterMod";
    m_stringOptions["version"] = L"0.4.0.0";
    m_stringOptions["yuvMatrix"] = L"None";
    m_stringOptions["outputLevels"] = L"PC";
    m_boolOptions["combineBitmaps"] = false;
    m_boolOptions["isMovable"] = false;

    m_bSrtHeaderDone = false;
    m_bExternalFile = false;
    m_bNotFirstPause = false;
    m_bNoExtFile = false;
    m_bUnsupportedSub = false;
    m_iCurExtSubTrack = 0;
    m_ExtSubFiles = {};

    LoadSettings();

    ass_set_font_ligatures(m_renderer.get(), m_settings.DisableFontLigatures);

#ifdef DEBUG
    DbgSetModuleLevel(LOG_ERROR, DWORD_MAX);
    DbgSetModuleLevel(LOG_TRACE, DWORD_MAX);
    DbgSetLogFileDesktop(L"AssFilterDbg.Log");
#endif
}

AssFilter::~AssFilter()
{
    if (m_consumer)
        m_consumer->Disconnect();

#ifdef DEBUG
    DbgCloseLogFile();
#endif
}

CUnknown* WINAPI AssFilter::CreateInstance(LPUNKNOWN pUnk, HRESULT* pResult)
{
    try
    {
        return new AssFilter(pUnk, pResult);
    }
    catch (std::bad_alloc&)
    {
        if (pResult)
            *pResult = E_OUTOFMEMORY;
    }

    return nullptr;
}

STDMETHODIMP AssFilter::CreateTrayIcon()
{
    CAutoLock lock(this);

    if (m_pTrayIcon)
        return E_UNEXPECTED;

    m_pTrayIcon = std::make_unique<CAssFilterTrayIcon>(this, TEXT("AssFilterMod"), IDI_ICON1, m_ExtSubFiles);

    return S_OK;
}

void AssFilter::SetMediaType(const CMediaType& mt, IPin* pPin)
{
    DbgLog((LOG_TRACE, 1, L"AssFilter::SetMediaType()"));

    CAutoLock lock(this);

    m_bExternalFile = false;
    m_bUnsupportedSub = false;

    // Check if there is already a track
    bool bTrackExist = false;
    if (m_track)
    {
        bTrackExist = true;

        // Flush subtitle cache
        if (m_consumer)
        {
            m_consumer->Clear();
            ass_flush_events(m_track.get());
            mapSubLine.clear();
        }
    }

    // If a track already exist, don't allocate the fonts again.
    if (!bTrackExist)
        LoadFonts(pPin);

    struct SUBTITLEINFO
    {
        DWORD dwOffset;
        CHAR  IsoLang[4];
        WCHAR TrackName[256];
    };

    auto psi = reinterpret_cast<const SUBTITLEINFO*>(mt.Format());

    m_wsTrackName.assign(psi->TrackName);
    std::wstring tmpIsoLang = s2ws(std::string(psi->IsoLang));
    m_wsTrackLang.assign(MatchLanguage(tmpIsoLang) + L" (" + tmpIsoLang + L")");

    if (!m_pTrayIcon && m_settings.TrayIcon)
        CreateTrayIcon();

    // SRT Media Sub-Type
    if (mt.subtype == MEDIASUBTYPE_UTF8)
    {
        m_track = decltype(m_track)(ass_new_track(m_ass.get()));
        m_wsSubType.assign(L"SRT");
        m_bSrtHeaderDone = false;
        m_boolOptions["isMovable"] = true;
        m_stringOptions["yuvMatrix"] = L"None";
        //m_stringOptions["outputLevels"] = L"PC";
    }
    // ASS Media Sub-Type
    else if (mt.subtype == MEDIASUBTYPE_ASS || mt.subtype == MEDIASUBTYPE_SSA)
    {
        m_track = decltype(m_track)(ass_new_track(m_ass.get()));
        m_wsSubType.assign(L"ASS");
        m_boolOptions["isMovable"] = false;

        // Extract the yuv Matrix
        std::string strTmp((char*)mt.Format() + psi->dwOffset, mt.FormatLength() - psi->dwOffset);
        if (strTmp.find("YCbCr Matrix: TV.601") != std::string::npos)
        {
            m_stringOptions["yuvMatrix"] = L"TV.601";
            //m_stringOptions["outputLevels"] = L"TV";
        }
        else if (strTmp.find("YCbCr Matrix: TV.709") != std::string::npos)
        {
            m_stringOptions["yuvMatrix"] = L"TV.709";
            //m_stringOptions["outputLevels"] = L"TV";
        }
        else
        {
            m_stringOptions["yuvMatrix"] = L"None";
            //m_stringOptions["outputLevels"] = L"PC";
        }
        ass_process_codec_private(m_track.get(), (char*)mt.Format() + psi->dwOffset, mt.FormatLength() - psi->dwOffset);
    }
    // VobSub Media Sub-Type (NOT SUPPORTED)
    else if (mt.subtype == MEDIASUBTYPE_VOBSUB)
    {
        m_track = decltype(m_track)(ass_new_track(m_ass.get()));
        m_wsTrackName.assign(L"Not supported!");
        m_wsSubType.assign(L"VOBSUB");
        m_stringOptions["yuvMatrix"] = L"None";
        m_bUnsupportedSub = true;
    }
    // PGS Media Sub-Type (NOT SUPPORTED)
    else if (mt.subtype == MEDIASUBTYPE_HDMVSUB)
    {
        m_track = decltype(m_track)(ass_new_track(m_ass.get()));
        m_wsTrackName.assign(L"Not supported!");
        m_wsSubType.assign(L"PGS");
        m_stringOptions["yuvMatrix"] = L"None";
        m_bUnsupportedSub = true;
    }
}

void AssFilter::Receive(IMediaSample* pSample, REFERENCE_TIME tSegmentStart)
{
    if (m_bExternalFile || m_bUnsupportedSub)
        return;

    CAutoLock lock(this);

    DbgLog((LOG_TRACE, 1, L"AssFilter::Receive() tSegmentStart: %I64d", tSegmentStart));

    REFERENCE_TIME tStart, tStop;
    BYTE* pData;

    if (SUCCEEDED(pSample->GetTime(&tStart, &tStop)) &&
        SUCCEEDED(pSample->GetPointer(&pData)))
    {
        tStart += tSegmentStart;
        tStop += tSegmentStart;

        DbgLog((LOG_TRACE, 1, L"AssFilter::Receive() tStart: %I64d, tStop: %I64d", tStart, tStop));

        if (m_wsSubType == L"SRT")
        {
            // Send the codec private data
            if (!m_bSrtHeaderDone)
            {
                char outBuffer[1024] {};
                double resx = m_settings.SrtResX / 384.0;
                double resy = m_settings.SrtResY / 288.0;

                // Generate a standard ass header
                _snprintf_s(outBuffer, _TRUNCATE, "[Script Info]\n"
                    "; Script generated by ParseSRT\n"
                    "Title: ParseSRT generated file\n"
                    "ScriptType: v4.00+\n"
                    "WrapStyle: 0\n"
                    "ScaledBorderAndShadow: %s\n"
                    "Kerning: %s\n"
                    "YCbCr Matrix: TV.709\n"
                    "PlayResX: %u\n"
                    "PlayResY: %u\n"
                    "[V4+ Styles]\n"
                    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
                    "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
                    "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
                    "Style: Default,%s,%u,&H%X,&H%X,&H%X,&H%X,0,0,0,0,%u,%u,%u,0,1,%u,%u,%u,%u,%u,%u,1"
                    "\n\n[Events]\n"
                    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n\n",
                    m_settings.ScaledBorderAndShadow ? "yes" : "no", m_settings.Kerning ? "yes" : "no",
                    m_settings.SrtResX, m_settings.SrtResY,
                    ws2s(m_settings.FontName).c_str(), (int)std::round(m_settings.FontSize * resy), m_settings.ColorPrimary,
                    m_settings.ColorSecondary, m_settings.ColorOutline, m_settings.ColorShadow, 
                    m_settings.FontScaleX, m_settings.FontScaleY, m_settings.FontSpacing, m_settings.FontOutline, 
                    m_settings.FontShadow, m_settings.LineAlignment, (int)std::round(m_settings.MarginLeft * resx),
                    (int)std::round(m_settings.MarginRight * resx), (int)std::round(m_settings.MarginVertical * resy));
                ass_process_codec_private(m_track.get(), outBuffer, static_cast<int>(strnlen_s(outBuffer, sizeof(outBuffer))));
                m_bSrtHeaderDone = true;
            }

            // Subtitle data is in UTF-8 format.
            char subLineData[1024] {};
            strncpy_s(subLineData, _countof(subLineData), (char*)pData, pSample->GetActualDataLength());
            std::string str = subLineData;

            // This is the way i use to get a unique id for the subtitle line
            // It will only fail in the case there is 2 or more lines with the same start timecode
            // (Need to check if the matroska muxer join lines in such a case)
            m_iSubLineCount = tStart / 10000;

            // Change srt tags to ass tags
            ParseSrtLine(str, m_settings);

            // Add the custom tags
            str.insert(0, ws2s(m_settings.CustomTags));

            // Add blur
            char blur[20] {};
            _snprintf_s(blur, _TRUNCATE, "{\\blur%u}", m_settings.FontBlur);
            str.insert(0, blur);

            // ASS in MKV: ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            char outBuffer[1024] {};
            _snprintf_s(outBuffer, _TRUNCATE, "%lld,0,Default,Main,0,0,0,,%s", m_iSubLineCount, str.c_str());
            ass_process_chunk(m_track.get(), outBuffer, static_cast<int>(strnlen_s(outBuffer, sizeof(outBuffer))), tStart / 10000, (tStop - tStart) / 10000);
        }
        else
        {
            // Check for duplicate subtitle line
            // Duplicate lines will happen when there is ordered chapters in the MKV file.
            // If a duplicate ReadOrder is found, and the subtitle line is new, change the
            // ReadOrder to make the subtitle line valid.
            s_sub_line tstSubLine;
            tstSubLine.tStart = tStart;
            tstSubLine.tStop = tStop;
            tstSubLine.subLine = std::string((char*)pData, pSample->GetActualDataLength());
            size_t pos = tstSubLine.subLine.find_first_of(',');
            tstSubLine.readOrder = strtol(tstSubLine.subLine.substr(0, pos).c_str(), NULL, 10);
            DbgLog((LOG_TRACE, 1, L"AssFilter::Receive() ReadOrder: %d", tstSubLine.readOrder));
            DbgLog((LOG_TRACE, 1, L"AssFilter::Receive() pData: %S", tstSubLine.subLine.c_str()));
            if (mapSubLine.empty())
            {
                ass_process_chunk(m_track.get(), (char*)pData, pSample->GetActualDataLength(), tStart / 10000, (tStop - tStart) / 10000);
                mapSubLine.emplace(tstSubLine.readOrder, tstSubLine);
            }
            else
            {
                bool found = false;
                size_t countReadOrder = mapSubLine.count(tstSubLine.readOrder);
                DbgLog((LOG_TRACE, 1, L"AssFilter::Receive() Occurences: %d", countReadOrder));
                if (countReadOrder > 0)
                {
                    for (auto it = mapSubLine.equal_range(tstSubLine.readOrder).first; it != mapSubLine.equal_range(tstSubLine.readOrder).second; ++it)
                    {
                        if (it->second.subLine == tstSubLine.subLine)
                        {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found)
                {
                    std::string tstStr = tstSubLine.subLine.substr(pos);
                    int readOrder = tstSubLine.readOrder;
                    tstSubLine.readOrder = readOrder + ((int)countReadOrder * 30000);
                    tstStr.insert(0, std::to_string(tstSubLine.readOrder));
                    ass_process_chunk(m_track.get(), (char*)tstStr.c_str(), (int)tstStr.size(), tStart / 10000, (tStop - tStart) / 10000);
                    mapSubLine.emplace(readOrder, tstSubLine);
                    DbgLog((LOG_TRACE, 1, L"AssFilter::Receive() Converted: %S", tstStr.c_str()));
                }
            }
        }
    }
}

STDMETHODIMP AssFilter::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    CheckPointer(ppv, E_POINTER);

    if (riid == __uuidof(ISubRenderOptions))
        return GetInterface(static_cast<ISubRenderOptions*>(this), ppv);
    else if (riid == __uuidof(ISubRenderProvider))
        return GetInterface(static_cast<ISubRenderProvider*>(this), ppv);
    else if (riid == __uuidof(ISpecifyPropertyPages))
        return GetInterface(static_cast<ISpecifyPropertyPages*>(this), ppv);
    else if (riid == __uuidof(ISpecifyPropertyPages2))
        return GetInterface(static_cast<ISpecifyPropertyPages2*>(this), ppv);
    else if (riid == __uuidof(IAssFilterSettings))
        return GetInterface(static_cast<IAssFilterSettings*>(this), ppv);
    else if (riid == __uuidof(IAFMExtSubtitles))
        return GetInterface(static_cast<IAFMExtSubtitles*>(this), ppv);

    return __super::NonDelegatingQueryInterface(riid, ppv);
}

int AssFilter::GetPinCount()
{
    if (m_bExternalFile)
        return 0;

	return 1;
}

CBasePin* AssFilter::GetPin(int n)
{
    if (m_bExternalFile)
        return NULL;

    return m_pin.get();
}

STDMETHODIMP AssFilter::Pause()
{
    DbgLog((LOG_TRACE, 1, L"AssFilter::Pause()"));

    CAutoLock lock(this);

    if (!m_bNotFirstPause)
    {
        if (m_bExternalFile)
        {
            if (FAILED(LoadExternalFile()))
                m_bNoExtFile = true;

            if (m_settings.TrayIcon)
                CreateTrayIcon();
        }

        m_bNotFirstPause = true;
        if (!m_bNoExtFile)
            ConnectToConsumer(m_pGraph);
    }

    return __super::Pause();
}

STDMETHODIMP AssFilter::Stop()
{
    DbgLog((LOG_TRACE, 1, L"AssFilter::Stop()"));

    CAutoLock lock(this);

    return __super::Stop();
}

STDMETHODIMP AssFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName)
{
    CAutoLock lock(this);

    HRESULT hr = NOERROR;
    if (pGraph)
    {
        DbgLog((LOG_TRACE, 1, L"AssFilter::JoinFilterGraph() -> %s joined the graph!", pName));

        hr = __super::JoinFilterGraph(pGraph, pName);
        if (FAILED(hr))
        {
            DbgLog((LOG_TRACE, 1, L"AssFilter::JoinFilterGraph() -> Failed to join filter graph!", pName));
            return hr;
        }
        if (wcscmp(pName, L"AssFilterMod(AutoLoad)") == 0)
            m_bExternalFile = true;

        return hr;
    }
    else
    {
        DbgLog((LOG_TRACE, 1, L"AssFilter::JoinFilterGraph() -> left the graph!", pName));
        if (m_consumer)
        {
            if (FAILED(m_consumer->Disconnect()))
            {
                DbgLog((LOG_TRACE, 1, L"AssFilter::JoinFilterGraph() -> Failed to disconnect consumer!", pName));
            }
            m_consumer = nullptr;
        }
        m_bNotFirstPause = false;

        if (m_pTrayIcon)
            m_pTrayIcon.reset();

        return __super::JoinFilterGraph(pGraph, pName);
    }

    return E_FAIL;
}

STDMETHODIMP AssFilter::RequestFrame(REFERENCE_TIME start, REFERENCE_TIME stop, LPVOID context)
{
    DbgLog((LOG_TRACE, 1, L"AssFilter::RequestFrame() start: %I64d, stop: %I64d", start, stop));

    CAutoLock lock(this);

    CheckPointer(m_consumer, E_UNEXPECTED);

    RECT videoOutputRect;
    m_consumer->GetRect("videoOutputRect", &videoOutputRect);
    DbgLog((LOG_TRACE, 1, L"AssFilter::RequestFrame() videoOutputRect: %u, %u, %u, %u", videoOutputRect.left, videoOutputRect.top, videoOutputRect.right, videoOutputRect.bottom));

    // The video rect we render the subtitles on
    RECT videoRect{};

    // Check to draw subtitles at custom resolution
    if (m_settings.NativeSize)
    {
        SIZE originalVideoSize;
        m_consumer->GetSize("originalVideoSize", &originalVideoSize);

        switch (m_settings.CustomRes)
        {
        case 0:
            videoRect.right = originalVideoSize.cx;
            videoRect.bottom = originalVideoSize.cy;
            break;
        case 1:
            videoRect.right = 3840;
            videoRect.bottom = 2160;
            break;
        case 2:
            videoRect.right = 2560;
            videoRect.bottom = 1440;
            break;
        case 3:
            videoRect.right = 1920;
            videoRect.bottom = 1080;
            break;
        case 4:
            videoRect.right = 1440;
            videoRect.bottom = 900;
            break;
        case 5:
            videoRect.right = 1280;
            videoRect.bottom = 720;
            break;
        case 6:
            videoRect.right = 1024;
            videoRect.bottom = 768;
            break;
        case 7:
            videoRect.right = 800;
            videoRect.bottom = 600;
            break;
        case 8:
            videoRect.right = 640;
            videoRect.bottom = 480;
            break;
        default:
            videoRect.right = originalVideoSize.cx;
            videoRect.bottom = originalVideoSize.cy;
            break;
        }
    }
    else
        videoRect = videoOutputRect;

    ass_set_frame_size(m_renderer.get(), videoRect.right , videoRect.bottom);

    DbgLog((LOG_TRACE, 1, L"AssFilter::RequestFrame() videoRect: %u, %u, %u, %u", videoRect.left, videoRect.top, videoRect.right, videoRect.bottom));

    int frameChange = 0;
    ISubRenderFramePtr frame = new SubFrame(videoRect, m_consumerLastId++,
                                            ass_render_frame(m_renderer.get(),
                                            m_bExternalFile ? m_extSubTrack[m_ExtSubFiles[m_iCurExtSubTrack].vecPos].get() : m_track.get(),
                                            start / 10000, &frameChange));
    return m_consumer->DeliverFrame(start, stop, context, frame);
}

STDMETHODIMP AssFilter::Disconnect(void)
{
    DbgLog((LOG_TRACE, 1, L"AssFilter::Disconnect()"));

    CAutoLock lock(this);
    m_consumer = nullptr;

    return S_OK;
}

STDMETHODIMP AssFilter::GetBool(LPCSTR field, bool* value)
{
    CheckPointer(value, E_POINTER);
    *value = m_boolOptions[field];

    return S_OK;
}

STDMETHODIMP AssFilter::GetInt(LPCSTR field, int* value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::GetSize(LPCSTR field, SIZE* value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::GetRect(LPCSTR field, RECT* value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::GetUlonglong(LPCSTR field, ULONGLONG* value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::GetDouble(LPCSTR field, double* value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::GetString(LPCSTR field, LPWSTR* value, int* chars)
{
    CheckPointer(value, E_POINTER);
    auto str = m_stringOptions[field];
    size_t len = str.length();
    *value = (LPWSTR)LocalAlloc(0, (len + 1) * sizeof(WCHAR));
    memcpy(*value, str.data(), len * sizeof(WCHAR));
    (*value)[len] = '\0';
    if (chars)
        *chars = static_cast<int>(len);

    DbgLog((LOG_TRACE, 1, L"AssFilter::GetString() field: %S, value: %s, chars: %d", field, *value, *chars));

    return S_OK;
}

STDMETHODIMP AssFilter::GetBin(LPCSTR field, LPVOID* value, int* size)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::SetBool(LPCSTR field, bool value)
{
    if (strcmp(field, "combineBitmaps") == 0)
        m_boolOptions[field] = value;

    return S_OK;
}

STDMETHODIMP AssFilter::SetInt(LPCSTR field, int value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::SetSize(LPCSTR field, SIZE value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::SetRect(LPCSTR field, RECT value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::SetUlonglong(LPCSTR field, ULONGLONG value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::SetDouble(LPCSTR field, double value)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::SetString(LPCSTR field, LPWSTR value, int chars)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::SetBin(LPCSTR field, LPVOID value, int size)
{
    return S_FALSE;
}

STDMETHODIMP AssFilter::GetPages(CAUUID *pPages)
{
    DbgLog((LOG_TRACE, 1, L"AssFilter::GetPages()"));

    CheckPointer(pPages, E_POINTER);
    pPages->cElems = 4;
    pPages->pElems = (GUID*)CoTaskMemAlloc(sizeof(GUID) * pPages->cElems);
    if (pPages->pElems == nullptr)
    {
        return E_OUTOFMEMORY;
    }
    pPages->pElems[0] = __uuidof(CAssFilterGeneralProp);
    pPages->pElems[1] = __uuidof(CAssFilterSettingsProp);
    pPages->pElems[2] = __uuidof(CAssFilterStatusProp);
    pPages->pElems[3] = __uuidof(CAssFilterAboutProp);

    return S_OK;
}

STDMETHODIMP AssFilter::CreatePage(const GUID& guid, IPropertyPage** ppPage)
{
    DbgLog((LOG_TRACE, 1, L"AssFilter::CreatePage()"));

    CheckPointer(ppPage, E_POINTER);
    HRESULT hr = S_OK;

    if (*ppPage != nullptr)
        return E_INVALIDARG;

    if (guid == __uuidof(CAssFilterGeneralProp))
        *ppPage = new CAssFilterGeneralProp(nullptr, &hr);
    else if (guid == __uuidof(CAssFilterSettingsProp))
        *ppPage = new CAssFilterSettingsProp(nullptr, &hr);
    else if (guid == __uuidof(CAssFilterStatusProp))
        *ppPage = new CAssFilterStatusProp(nullptr, &hr);
    else if (guid == __uuidof(CAssFilterAboutProp))
        *ppPage = new CAssFilterAboutProp(nullptr, &hr);

    if (SUCCEEDED(hr) && *ppPage)
    {
        (*ppPage)->AddRef();
        return S_OK;
    }
    else
    {
        SAFE_DELETE(*ppPage);
        return E_FAIL;
    }
}

// IAssFilterSettings
STDMETHODIMP AssFilter::GetTrackInfo(const WCHAR **pTrackName, const WCHAR **pTrackLang, const WCHAR **pSubType)
{
    if (pTrackName)
        *pTrackName = m_wsTrackName.c_str();

    if (pTrackLang)
        *pTrackLang = m_wsTrackLang.c_str();

    if (pSubType)
        *pSubType = m_wsSubType.c_str();

    return S_OK;
}

STDMETHODIMP AssFilter::GetConsumerInfo(const WCHAR **pName, const WCHAR **pVersion)
{
    if (pName)
        *pName = m_wsConsumerName.c_str();

    if (pVersion)
        *pVersion = m_wsConsumerVer.c_str();

    return S_OK;
}

// IAFMExtSubtitles
STDMETHODIMP_(int) AssFilter::GetTotalExternalSubs()
{
    return static_cast<int>(m_ExtSubFiles.size());
}

STDMETHODIMP_(int) AssFilter::GetCurExternalSub()
{
    return m_iCurExtSubTrack;
}

STDMETHODIMP AssFilter::SetCurExternalSub(int iCurExtSub)
{
    if ((iCurExtSub < 0) || (iCurExtSub >= m_ExtSubFiles.size()))
        return E_INVALIDARG;

    CAutoLock lock(this);

    m_iCurExtSubTrack = iCurExtSub;
    if (m_ExtSubFiles[m_iCurExtSubTrack].vecPos == SIZE_MAX)
    {
        if (m_ExtSubFiles[m_iCurExtSubTrack].subType == L"ASS")
        {
            m_track = decltype(m_track)(ass_read_file(m_ass.get(), const_cast<char*>(ws2s(m_ExtSubFiles[m_iCurExtSubTrack].subFile).c_str()), "UTF-8"));
            m_ExtSubFiles[m_iCurExtSubTrack].yuvMatrix = ExtractYuvMatrix(m_ExtSubFiles[m_iCurExtSubTrack].subFile);
        }
        else
        {
            // Check if the file needs conversion to UTF-8
            if ((m_ExtSubFiles[m_iCurExtSubTrack].codePage != 0) && isFileUTF8(m_ExtSubFiles[m_iCurExtSubTrack].subFile))
                m_ExtSubFiles[m_iCurExtSubTrack].codePage = 0;
            m_track = decltype(m_track)(srt_read_file(m_ass.get(), m_ExtSubFiles[m_iCurExtSubTrack].subFile, m_settings, m_ExtSubFiles[m_iCurExtSubTrack].codePage));
        }
        m_extSubTrack.push_back(std::move(m_track));
        m_track.reset();
        m_ExtSubFiles[m_iCurExtSubTrack].vecPos = m_extSubTrack.size() - 1;
    }
    m_stringOptions["yuvMatrix"] = m_ExtSubFiles[m_iCurExtSubTrack].yuvMatrix;
    m_boolOptions["isMovable"] = m_ExtSubFiles[m_iCurExtSubTrack].subType == L"SRT" ? true : false;
    m_wsTrackName = m_ExtSubFiles[m_iCurExtSubTrack].subFile;
    m_wsTrackLang = m_ExtSubFiles[m_iCurExtSubTrack].subLang;
    m_wsSubType = m_ExtSubFiles[m_iCurExtSubTrack].subType;

    if (m_consumer)
        m_consumer->Clear();

    return S_OK;
}

HRESULT AssFilter::LoadDefaults()
{
    m_settings.TrayIcon = FALSE;
    m_settings.NativeSize = FALSE;
    m_settings.ScaledBorderAndShadow = TRUE;
    m_settings.DisableFontLigatures = FALSE;
    m_settings.DisableAutoLoad = FALSE;
    m_settings.Kerning = FALSE;

    m_settings.FontName = L"Arial";
    m_settings.FontSize = 18;
    m_settings.FontScaleX = 100;
    m_settings.FontScaleY = 100;
    m_settings.FontSpacing = 0;
    m_settings.FontBlur = 0;

    m_settings.FontOutline = 2;
    m_settings.FontShadow = 3;
    m_settings.LineAlignment = 2;
    m_settings.MarginLeft = 20;
    m_settings.MarginRight = 20;
    m_settings.MarginVertical = 10;
    m_settings.ColorPrimary = 0x00FFFFFF;
    m_settings.ColorSecondary = 0x00FFFF;
    m_settings.ColorOutline = 0;
    m_settings.ColorShadow = 0x7F000000;
    m_settings.CustomRes = 0;
    m_settings.SrtResX = 1920;
    m_settings.SrtResY = 1080;

    m_settings.CustomTags = L"";
    m_settings.ExtraFontsDir = L"{FILE_DIR}";
    m_settings.ExtraSubsDir = L"Subs";

    return S_OK;
}

HRESULT AssFilter::ReadSettings(HKEY rootKey)
{
    HRESULT hr;
    DWORD dwVal;
    BOOL bFlag;
    std::wstring strVal;

    CRegistry reg = CRegistry(rootKey, ASSFILTER_REGISTRY_KEY, hr, TRUE);
    if (SUCCEEDED(hr))
    {
        bFlag = reg.ReadBOOL(L"TrayIcon", hr);
        if (SUCCEEDED(hr)) m_settings.TrayIcon = bFlag;

        bFlag = reg.ReadBOOL(L"NativeSize", hr);
        if (SUCCEEDED(hr)) m_settings.NativeSize = bFlag;

        bFlag = reg.ReadBOOL(L"ScaledBorderAndShadow", hr);
        if (SUCCEEDED(hr)) m_settings.ScaledBorderAndShadow = bFlag;

        bFlag = reg.ReadBOOL(L"DisableFontLigatures", hr);
        if (SUCCEEDED(hr)) m_settings.DisableFontLigatures = bFlag;

        bFlag = reg.ReadBOOL(L"DisableAutoLoad", hr);
        if (SUCCEEDED(hr)) m_settings.DisableAutoLoad = bFlag;

        bFlag = reg.ReadBOOL(L"Kerning", hr);
        if (SUCCEEDED(hr)) m_settings.Kerning = bFlag;

        strVal = reg.ReadString(L"FontName", hr);
        if (SUCCEEDED(hr)) m_settings.FontName = strVal;

        dwVal = reg.ReadDWORD(L"FontSize", hr);
        if (SUCCEEDED(hr)) m_settings.FontSize = dwVal;

        dwVal = reg.ReadDWORD(L"FontScaleX", hr);
        if (SUCCEEDED(hr)) m_settings.FontScaleX = dwVal;

        dwVal = reg.ReadDWORD(L"FontScaleY", hr);
        if (SUCCEEDED(hr)) m_settings.FontScaleY = dwVal;

        dwVal = reg.ReadDWORD(L"FontSpacing", hr);
        if (SUCCEEDED(hr)) m_settings.FontSpacing = dwVal;

        dwVal = reg.ReadDWORD(L"FontBlur", hr);
        if (SUCCEEDED(hr)) m_settings.FontBlur = dwVal;

        dwVal = reg.ReadDWORD(L"FontOutline", hr);
        if (SUCCEEDED(hr)) m_settings.FontOutline = dwVal;

        dwVal = reg.ReadDWORD(L"FontShadow", hr);
        if (SUCCEEDED(hr)) m_settings.FontShadow = dwVal;

        dwVal = reg.ReadDWORD(L"LineAlignment", hr);
        if (SUCCEEDED(hr)) m_settings.LineAlignment = dwVal;

        dwVal = reg.ReadDWORD(L"MarginLeft", hr);
        if (SUCCEEDED(hr)) m_settings.MarginLeft = dwVal;

        dwVal = reg.ReadDWORD(L"MarginRight", hr);
        if (SUCCEEDED(hr)) m_settings.MarginRight = dwVal;

        dwVal = reg.ReadDWORD(L"MarginVertical", hr);
        if (SUCCEEDED(hr)) m_settings.MarginVertical = dwVal;

        dwVal = reg.ReadDWORD(L"ColorPrimary", hr);
        if (SUCCEEDED(hr)) m_settings.ColorPrimary = dwVal;

        dwVal = reg.ReadDWORD(L"ColorSecondary", hr);
        if (SUCCEEDED(hr)) m_settings.ColorSecondary = dwVal;

        dwVal = reg.ReadDWORD(L"ColorOutline", hr);
        if (SUCCEEDED(hr)) m_settings.ColorOutline = dwVal;

        dwVal = reg.ReadDWORD(L"ColorShadow", hr);
        if (SUCCEEDED(hr)) m_settings.ColorShadow = dwVal;

        dwVal = reg.ReadDWORD(L"CustomRes", hr);
        if (SUCCEEDED(hr)) m_settings.CustomRes = dwVal;

        dwVal = reg.ReadDWORD(L"SrtResX", hr);
        if (SUCCEEDED(hr)) m_settings.SrtResX = dwVal;

        dwVal = reg.ReadDWORD(L"SrtResY", hr);
        if (SUCCEEDED(hr)) m_settings.SrtResY = dwVal;

        strVal = reg.ReadString(L"CustomTags", hr);
        if (SUCCEEDED(hr)) m_settings.CustomTags = strVal;

        strVal = reg.ReadString(L"ExtraFontsDir", hr);
        if (SUCCEEDED(hr)) m_settings.ExtraFontsDir = strVal;

        strVal = reg.ReadString(L"ExtraSubsDir", hr);
        if (SUCCEEDED(hr)) m_settings.ExtraSubsDir = strVal;
    }
    else
        SaveSettings();

    return S_OK;
}

HRESULT AssFilter::LoadSettings()
{
    LoadDefaults();

    ReadSettings(HKEY_CURRENT_USER);
    return S_OK;
}

HRESULT AssFilter::SaveSettings()
{
    HRESULT hr;
    CreateRegistryKey(HKEY_CURRENT_USER, ASSFILTER_REGISTRY_KEY);
    CRegistry reg = CRegistry(HKEY_CURRENT_USER, ASSFILTER_REGISTRY_KEY, hr);
    if (SUCCEEDED(hr))
    {
        reg.WriteBOOL(L"TrayIcon", m_settings.TrayIcon);
        reg.WriteBOOL(L"NativeSize", m_settings.NativeSize);
        reg.WriteBOOL(L"ScaledBorderAndShadow", m_settings.ScaledBorderAndShadow);
        reg.WriteBOOL(L"DisableFontLigatures", m_settings.DisableFontLigatures);
        reg.WriteBOOL(L"DisableAutoLoad", m_settings.DisableAutoLoad);
        reg.WriteBOOL(L"Kerning", m_settings.Kerning);
        reg.WriteString(L"FontName", m_settings.FontName.c_str());
        reg.WriteDWORD(L"FontSize", m_settings.FontSize);
        reg.WriteDWORD(L"FontScaleX", m_settings.FontScaleX);
        reg.WriteDWORD(L"FontScaleY", m_settings.FontScaleY);
        reg.WriteDWORD(L"FontSpacing", m_settings.FontSpacing);
        reg.WriteDWORD(L"FontBlur", m_settings.FontBlur);
        reg.WriteDWORD(L"FontOutline", m_settings.FontOutline);
        reg.WriteDWORD(L"FontShadow", m_settings.FontShadow);
        reg.WriteDWORD(L"LineAlignment", m_settings.LineAlignment);
        reg.WriteDWORD(L"MarginLeft", m_settings.MarginLeft);
        reg.WriteDWORD(L"MarginRight", m_settings.MarginRight);
        reg.WriteDWORD(L"MarginVertical", m_settings.MarginVertical);
        reg.WriteDWORD(L"ColorPrimary", m_settings.ColorPrimary);
        reg.WriteDWORD(L"ColorSecondary", m_settings.ColorSecondary);
        reg.WriteDWORD(L"ColorOutline", m_settings.ColorOutline);
        reg.WriteDWORD(L"ColorShadow", m_settings.ColorShadow);
        reg.WriteDWORD(L"CustomRes", m_settings.CustomRes);
        reg.WriteDWORD(L"SrtResX", m_settings.SrtResX);
        reg.WriteDWORD(L"SrtResY", m_settings.SrtResY);
        reg.WriteString(L"CustomTags", m_settings.CustomTags.c_str());
        reg.WriteString(L"ExtraFontsDir", m_settings.ExtraFontsDir.c_str());
        reg.WriteString(L"ExtraSubsDir", m_settings.ExtraSubsDir.c_str());
    }

    return S_OK;
}

HRESULT AssFilter::ConnectToConsumer(IFilterGraph* pGraph)
{
    CheckPointer(pGraph, E_POINTER);

    IEnumFiltersPtr filters;
    if (SUCCEEDED(pGraph->EnumFilters(&filters)))
    {
        ISubRenderConsumer2Ptr consumer;
        ISubRenderProviderPtr provider;
        for (IBaseFilterPtr filter; filters->Next(1, &filter, 0) == S_OK; filter = NULL)
        {
            if (SUCCEEDED(filter->QueryInterface(IID_PPV_ARGS(&consumer))) &&
                SUCCEEDED(QueryInterface(IID_PPV_ARGS(&provider))))
            {
                if (FAILED(consumer->Connect(provider)))
                {
                    DbgLog((LOG_TRACE, 1, L"AssFilter::ConnectToConsumer() -> Already connected"));

                    return S_OK;
                }

                m_consumer = consumer;
                m_consumerLastId = 0;

                LPWSTR cName;
                int cChars;

                m_consumer->GetString("name", &cName, &cChars);
                m_wsConsumerName.assign(cName);
                LocalFree(cName);

                m_consumer->GetString("version", &cName, &cChars);
                m_wsConsumerVer.assign(cName);
                LocalFree(cName);

                DbgLog((LOG_TRACE, 1, L"AssFilter::ConnectToConsumer() -> Connected to consumer %s v%s", m_wsConsumerName.c_str(), m_wsConsumerVer.c_str()));

                return S_OK;
            }
        }
    }

    return E_FAIL;
}

HRESULT AssFilter::LoadFonts(IPin* pPin)
{
    // Try to load fonts in the container
    IAMGraphStreamsPtr graphStreams;
    IDSMResourceBagPtr bag;
    if (SUCCEEDED(GetFilterGraph()->QueryInterface(IID_PPV_ARGS(&graphStreams))) &&
        SUCCEEDED(graphStreams->FindUpstreamInterface(pPin, IID_PPV_ARGS(&bag), AM_INTF_SEARCH_FILTER)))
    {
        for (DWORD i = 0; i < bag->ResGetCount(); ++i)
        {
            _bstr_t name, desc, mime;
            BYTE* pData = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(bag->ResGet(i, &name.GetBSTR(), &desc.GetBSTR(), &mime.GetBSTR(), &pData, &len, nullptr)))
            {
                if (wcscmp(mime.GetBSTR(), L"application/x-truetype-font") == 0 ||
                    wcscmp(mime.GetBSTR(), L"application/vnd.ms-opentype") == 0) // TODO: more mimes?
                {
                    ass_add_font(m_ass.get(), "", (char*)pData, len);
                    // TODO: clear these fonts somewhere?
                }
                CoTaskMemFree(pData);
            }
        }
    }

    ass_set_fonts(m_renderer.get(), NULL, NULL, ASS_FONTPROVIDER_DIRECTWRITE, NULL, NULL);

    return S_OK;
}

HRESULT AssFilter::LoadExternalFile()
{
    // Check for external subs
    std::wstring extFileName;
    IEnumFiltersPtr pEnumFilters;
    if (SUCCEEDED(m_pGraph->EnumFilters(&pEnumFilters)))
    {
        for (IBaseFilterPtr pBaseFilter; pEnumFilters->Next(1, &pBaseFilter, 0) == S_OK; pBaseFilter = NULL)
        {
            IFileSourceFilterPtr pFSF;
            if (SUCCEEDED(pBaseFilter->QueryInterface(IID_PPV_ARGS(&pFSF))))
            {
                LPOLESTR fnw = NULL;
                if (!pFSF || FAILED(pFSF->GetCurFile(&fnw, NULL)) || !fnw)
                    continue;
                extFileName.assign(fnw);
                CoTaskMemFree(fnw);
                break;
            }
        }
    }

    std::wstring mediaNameWithoutExt = extFileName.substr(0, extFileName.find_last_of(L'.') + 1);

    // Get all subtitle files matching the media file name in the same folder
    std::vector<std::wstring> mediaDirSubFiles = FindMatchingSubs(mediaNameWithoutExt);

    // Get all subtitle files located in the extra folders
    std::vector<std::wstring> extraDirSubFiles;
    std::vector<std::wstring> extraFolders;
    std::wstring lcExtraSubsDir(m_settings.ExtraSubsDir);
    std::transform(lcExtraSubsDir.begin(), lcExtraSubsDir.end(), lcExtraSubsDir.begin(), ::towlower);
    tokenize(m_settings.ExtraSubsDir, extraFolders, std::wstring(L";"));

    // Remove duplicates
    std::sort(extraFolders.begin(), extraFolders.end());
    extraFolders.erase(std::unique(extraFolders.begin(), extraFolders.end()), extraFolders.end());
    for (auto i = 0; i < extraFolders.size(); ++i)
    {
        std::vector<std::wstring> possibleSubFiles;
        trim(extraFolders[i]);
        if (!extraFolders[i].empty() && extraFolders[i] != L".")
        {
            std::wstring extraSubsFolder(mediaNameWithoutExt.substr(0, mediaNameWithoutExt.find_last_of(L'\\') + 1) + extraFolders[i] + L"\\");
            possibleSubFiles = FindMatchingSubs(extraSubsFolder);
        }
        if (!possibleSubFiles.empty())
            extraDirSubFiles.insert(std::end(extraDirSubFiles), std::begin(possibleSubFiles), std::end(possibleSubFiles));
    }

    // Try to find an external subtitle file
    DbgLog((LOG_TRACE, 1, L"AssFilter::LoadExternalFile() -> System CodePage is %u", GetACP()));
    if (!mediaDirSubFiles.empty() || !extraDirSubFiles.empty())
    {
        // Work with lowercase for the compares
        std::wstring lcName(mediaNameWithoutExt);
        std::transform(lcName.begin(), lcName.end(), lcName.begin(), ::towlower);
        for (auto j = 0; j < mediaDirSubFiles.size(); ++j)
        {
            bool bSubFound = false;
            std::wstring lcSubFile(mediaDirSubFiles[j]);
            std::transform(lcSubFile.begin(), lcSubFile.end(), lcSubFile.begin(), ::towlower);
            for (auto i = 0; i < _countof(iso639_lang); ++i)
            {
                if (std::wstring(lcName).append(iso639_lang[i].lang2).append(L".ass") == lcSubFile)
                {
                    s_ext_sub extSub;
                    extSub.subFile = mediaDirSubFiles[j];
                    extSub.subLang.assign(iso639_lang[i].language);
                    extSub.subType.assign(L"ASS");
                    extSub.yuvMatrix.assign(L"None");
                    extSub.vecPos = SIZE_MAX;       // uninitialized
                    m_ExtSubFiles.push_back(extSub);
                    bSubFound = true;
                    break;
                }
                else if (std::wstring(lcName).append(iso639_lang[i].lang2).append(L".srt") == lcSubFile)
                {
                    s_ext_sub extSub;
                    extSub.subFile = mediaDirSubFiles[j];
                    extSub.subLang.assign(iso639_lang[i].language);
                    extSub.codePage = iso639_lang[i].codepage;
                    extSub.subType.assign(L"SRT");
                    extSub.yuvMatrix.assign(L"None");
                    extSub.vecPos = SIZE_MAX;       // uninitialized
                    m_ExtSubFiles.push_back(extSub);
                    bSubFound = true;
                    break;
                }
                // Try a filename without a 2 letters language code
                if (i == _countof(iso639_lang) - 1)
                {
                    if (std::wstring(lcName).append(L"ass") == lcSubFile)
                    {
                        s_ext_sub extSub;
                        extSub.subFile = mediaDirSubFiles[j];
                        extSub.subLang.assign(MatchLanguage(std::wstring(L"und")));
                        extSub.subType.assign(L"ASS");
                        extSub.yuvMatrix.assign(L"None");
                        extSub.vecPos = SIZE_MAX;   // uninitialized
                        m_ExtSubFiles.push_back(extSub);
                        bSubFound = true;
                    }
                    else if (std::wstring(lcName).append(L"srt") == lcSubFile)
                    {
                        s_ext_sub extSub;
                        extSub.subFile = mediaDirSubFiles[j];
                        extSub.subLang.assign(MatchLanguage(std::wstring(L"und")));
                        extSub.subType.assign(L"SRT");
                        extSub.yuvMatrix.assign(L"None");
                        extSub.codePage = GetACP();
                        extSub.vecPos = SIZE_MAX;   // uninitialized
                        m_ExtSubFiles.push_back(extSub);
                        bSubFound = true;
                    }
                }
            }
            // Subtitle not matching any language
            if (!bSubFound)
            {
                if (EndsWith(lcSubFile, std::wstring(L".ass")))
                {
                    s_ext_sub extSub;
                    extSub.subAltName = mediaDirSubFiles[j].substr(mediaNameWithoutExt.size(), mediaDirSubFiles[j].size() - mediaNameWithoutExt.size() - 4);
                    extSub.subFile = mediaDirSubFiles[j];
                    extSub.subLang.assign(MatchLanguage(std::wstring(L"und")));
                    extSub.subType.assign(L"ASS");
                    extSub.yuvMatrix.assign(L"None");
                    extSub.vecPos = SIZE_MAX;   // uninitialized
                    m_ExtSubFiles.push_back(extSub);
                }
                else
                {
                    s_ext_sub extSub;
                    extSub.subAltName = mediaDirSubFiles[j].substr(mediaNameWithoutExt.size(), mediaDirSubFiles[j].size() - mediaNameWithoutExt.size() - 4);
                    extSub.subFile = mediaDirSubFiles[j];
                    extSub.subLang.assign(MatchLanguage(std::wstring(L"und")));
                    extSub.subType.assign(L"SRT");
                    extSub.yuvMatrix.assign(L"None");
                    extSub.codePage = GetACP();
                    extSub.vecPos = SIZE_MAX;   // uninitialized
                    m_ExtSubFiles.push_back(extSub);
                }
            }
        }

        // Check external subtitles folders
        for (auto j = 0; j < extraDirSubFiles.size(); ++j)
        {
            // Work with lowercase for the compares
            std::wstring lcName(extraDirSubFiles[j]);
            std::transform(lcName.begin(), lcName.end(), lcName.begin(), ::towlower);
            if (EndsWith(lcName, std::wstring(L".ass")))
            {
                s_ext_sub extSub;
                extSub.subAltName = extraDirSubFiles[j].substr(extraDirSubFiles[j].find_last_of(L'\\') + 1, extraDirSubFiles[j].size() - extraDirSubFiles[j].find_last_of(L'\\') - 5);
                extSub.subFile = extraDirSubFiles[j];
                extSub.subLang.assign(MatchLanguage(std::wstring(L"und")));
                extSub.subType.assign(L"ASS");
                extSub.yuvMatrix.assign(L"None");
                extSub.vecPos = SIZE_MAX;   // uninitialized
                m_ExtSubFiles.push_back(extSub);
            }
            else
            {
                s_ext_sub extSub;
                extSub.subAltName = extraDirSubFiles[j].substr(extraDirSubFiles[j].find_last_of(L'\\') + 1, extraDirSubFiles[j].size() - extraDirSubFiles[j].find_last_of(L'\\') - 5);
                extSub.subFile = extraDirSubFiles[j];
                extSub.subLang.assign(MatchLanguage(std::wstring(L"und")));
                extSub.subType.assign(L"SRT");
                extSub.yuvMatrix.assign(L"None");
                extSub.codePage = GetACP();
                extSub.vecPos = SIZE_MAX;   // uninitialized
                m_ExtSubFiles.push_back(extSub);
            }
        }
        // Install the fonts
        m_pFontInstaller = std::make_unique<CFontInstaller>();
        std::vector<std::wstring> fonts = ListFontsInFolder(ParseFontsPath(m_settings.ExtraFontsDir, mediaNameWithoutExt));
        for (const auto& font : fonts)
            m_pFontInstaller->InstallFont(font);

        ass_set_fonts_dir(m_ass.get(), ws2s(ParseFontsPath(m_settings.ExtraFontsDir, mediaNameWithoutExt)).c_str());
        ass_set_extract_fonts(m_ass.get(), TRUE);
        SetCurExternalSub(m_iCurExtSubTrack);
        ass_set_fonts(m_renderer.get(), NULL, NULL, ASS_FONTPROVIDER_DIRECTWRITE, NULL, NULL);
    }
    else
        return E_FAIL;

    return S_OK;
}

void AssFilter::ASS_LibraryDeleter::operator()(ASS_Library* p)
{
    if (p) ass_library_done(p);
}

void AssFilter::ASS_RendererDeleter::operator()(ASS_Renderer* p)
{
    if (p) ass_renderer_done(p);
}

void AssFilter::ASS_TrackDeleter::operator()(ASS_Track* p)
{
    if (p) ass_free_track(p);
}
