#include "YouTubeAPI.h"

#include "AIMPYouTube.h"
#include "AimpHTTP.h"
#include "SDK/apiFileManager.h"
#include "SDK/apiPlaylists.h"
#include "AIMPString.h"
#include "DurationResolver.h"
#include "Tools.h"
#include "Timer.h"
#include <Strsafe.h>
#include <string>
#include <set>
#include <map>
#include <regex>
#include <array>

extern DWORD g_MainThreadId;

void YouTubeAPI::AddFromJson(IAIMPPlaylist *playlist, const rapidjson::Value &d, std::shared_ptr<LoadingState> state) {
    if (!playlist || !state || !Plugin::instance()->core())
        return;

    int insertAt = state->InsertPos;
    if (insertAt >= 0)
        insertAt += state->AdditionalPos;

    IAIMPFileInfo *file_info = nullptr;
    if (Plugin::instance()->core()->CreateObject(IID_IAIMPFileInfo, reinterpret_cast<void **>(&file_info)) == S_OK) {
        auto processItem = [&](const std::wstring &pid, const rapidjson::Value &item, const rapidjson::Value &contentDetails) {
            if (!item.HasMember("title"))
                return;

            std::wstring final_title = Tools::ToWString(item["title"]);
            if (final_title == L"Deleted video" || final_title == L"Private video")
                return;

            std::wstring trackId;
            if (item.HasMember("resourceId")) {
                trackId = Tools::ToWString(item["resourceId"]["videoId"]);
            } else {
                trackId = pid;
            }
            if (state->TrackIds.find(trackId) != state->TrackIds.end()) {
                // Already added earlier
                if (insertAt >= 0 && !(state->Flags & LoadingState::IgnoreExistingPosition)) {
                    insertAt++;
                    state->InsertPos++;
                    if (state->Flags & LoadingState::UpdateAdditionalPos)
                        state->AdditionalPos++;
                }
                return;
            }

            if (Config::TrackExclusions.find(trackId) != Config::TrackExclusions.end())
                return; // Track excluded

            state->TrackIds.insert(trackId);
            if (state->PlaylistToUpdate) {
                state->PlaylistToUpdate->Items.insert(trackId);
            }

            auto permalink = L"https://www.youtube.com/watch?v=" + trackId;

            std::wstring filename(L"youtube://");
            filename += trackId + L"/";
            filename += final_title;
            filename += L".mp4";
            file_info->SetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, AIMPString(filename));

            if (!state->ReferenceName.empty()) {
                file_info->SetValueAsObject(AIMP_FILEINFO_PROPID_ALBUM, AIMPString(state->ReferenceName));
            }
            if (item.HasMember("channelTitle") && (state->Flags & LoadingState::AddChannelTitle)) {
                file_info->SetValueAsObject(AIMP_FILEINFO_PROPID_ARTIST, AIMPString(item["channelTitle"]));
            }
            file_info->SetValueAsObject(AIMP_FILEINFO_PROPID_URL, AIMPString(permalink));

            int64_t videoDuration = 0;
            if (contentDetails.IsObject() && contentDetails.HasMember("duration")) {
                std::wstring duration = Tools::ToWString(contentDetails["duration"]);

                std::wregex re(L"PT(?:([0-9]+)H)?(?:([0-9]+)M)?(?:([0-9]+)S)?");
                std::wsmatch match;
                if (std::regex_match(duration, match, re)) {
                    auto h = match[1].matched ? std::stoll(match[1].str()) : 0;
                    auto m = match[2].matched ? std::stoll(match[2].str()) : 0;
                    auto s = match[3].matched ? std::stoll(match[3].str()) : 0;
                    videoDuration = h * 3600 + m * 60 + s;

                    file_info->SetValueAsFloat(AIMP_FILEINFO_PROPID_DURATION, videoDuration);
                }
            }

            AIMPString title(final_title);
            file_info->SetValueAsObject(AIMP_FILEINFO_PROPID_TITLE, title);

            std::wstring artwork;
            if (item.HasMember("thumbnails") && item["thumbnails"].IsObject() && item["thumbnails"].HasMember("high") && item["thumbnails"]["high"].HasMember("url")) {
                artwork = Tools::ToWString(item["thumbnails"]["high"]["url"]);
            }


            Config::TrackInfos[trackId] = Config::TrackInfo(final_title, trackId, permalink, artwork, videoDuration);

            const DWORD flags = AIMP_PLAYLIST_ADD_FLAGS_FILEINFO | AIMP_PLAYLIST_ADD_FLAGS_NOCHECKFORMAT | AIMP_PLAYLIST_ADD_FLAGS_NOEXPAND | AIMP_PLAYLIST_ADD_FLAGS_NOTHREADING;
            if (SUCCEEDED(playlist->Add(file_info, flags, insertAt))) {
                state->AddedItems++;
                if (insertAt >= 0) {
                    insertAt++;
                    state->InsertPos++;
                    if (state->Flags & LoadingState::UpdateAdditionalPos)
                        state->AdditionalPos++;
                }
            }
        };

        rapidjson::Value null;

        if (d.IsArray()) {
            for (auto x = d.Begin(), e = d.End(); x != e; x++) {
                const rapidjson::Value *px = &(*x);
                if (!px->IsObject() || !px->HasMember("snippet"))
                    continue;

                processItem((*px).HasMember("id") ? Tools::ToWString((*px)["id"]) : L"", (*px)["snippet"], px->HasMember("contentDetails")? (*px)["contentDetails"] : null);
            }
        } else if (d.IsObject() && d.HasMember("snippet")) {
            processItem(d.HasMember("id")? Tools::ToWString(d["id"]) : L"", d["snippet"], d.HasMember("contentDetails") ? d["contentDetails"] : null);
        } else if (d.IsObject()) {
            processItem(L"", d, null);
        }
        file_info->Release();
    }
}

void YouTubeAPI::LoadFromUrl(std::wstring url, IAIMPPlaylist *playlist, std::shared_ptr<LoadingState> state, std::function<void()> finishCallback) {
    if (!playlist || !state)
        return;

    std::wstring reqUrl(url);
    if (reqUrl.find(L'?') == std::wstring::npos) {
        reqUrl += L'?';
    } else {
        reqUrl += L'&';
    }
    reqUrl += L"key=" TEXT(APP_KEY);
    if (Plugin::instance()->isConnected())
        reqUrl += L"\r\nAuthorization: Bearer " + Plugin::instance()->getAccessToken();

    AimpHTTP::Get(reqUrl, [playlist, state, finishCallback, url](unsigned char *data, int size) {
        rapidjson::Document d;
        d.Parse(reinterpret_cast<const char *>(data));

        playlist->BeginUpdate();
        if (d.IsObject() && d.HasMember("items") && d["items"].IsArray() && d["items"].Size() > 0 && d["items"][0].HasMember("contentDetails") && d["items"][0]["contentDetails"].HasMember("relatedPlaylists")) {
            const rapidjson::Value &i = d["items"][0]["contentDetails"]["relatedPlaylists"];
            std::wstring uploads = Tools::ToWString(i["uploads"]);
            /*std::wstring favorites = Tools::ToWString(i["favorites"]);
            std::wstring likes = Tools::ToWString(i["likes"]);
            std::wstring watchLater = Tools::ToWString(i["watchLater"]);*/
            std::wstring userName = Tools::ToWString(d["items"][0]["snippet"]["localized"]["title"]);
            IAIMPPropertyList *plProp = nullptr;
            if (SUCCEEDED(playlist->QueryInterface(IID_IAIMPPropertyList, reinterpret_cast<void **>(&plProp)))) {
                bool isRenamed = true;
                IAIMPString *str = nullptr;
                if (SUCCEEDED(plProp->GetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, IID_IAIMPString, reinterpret_cast<void **>(&str)))) {
                    isRenamed = wcscmp(L"YouTube", str->GetData());
                    str->Release();
                }
                if (!isRenamed)
                    plProp->SetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, AIMPString(userName));
                plProp->Release();
            }
            state->ReferenceName = userName;

            LoadFromUrl(L"https://content.googleapis.com/youtube/v3/playlistItems?part=contentDetails%2Csnippet&maxResults=50&playlistId=" + uploads +
                        L"&fields=items%2Fsnippet%2Ckind%2CnextPageToken%2CpageInfo%2CtokenPagination", playlist, state, finishCallback);

            playlist->EndUpdate();
            return;
        } else if (d.IsObject() && d.HasMember("items")) {
            AddFromJson(playlist, d["items"], state);
        } else {
            AddFromJson(playlist, d, state);
        }
        playlist->EndUpdate();

        bool processNextPage = d.IsObject() && d.HasMember("nextPageToken");

        if ((state->Flags & LoadingState::IgnoreNextPage) ||
            (Config::GetInt32(L"LimitUserStream", 0) && state->AddedItems >= Config::GetInt32(L"LimitUserStreamValue", 5000))) {
            processNextPage = false;
        }

        if (processNextPage) {
            std::wstring next_url(url);
            std::size_t pos = 0;
            if ((pos = next_url.find(L"&pageToken")) != std::wstring::npos)
                next_url = next_url.substr(0, pos);

            LoadFromUrl(next_url + L"&pageToken=" + Tools::ToWString(d["nextPageToken"]), playlist, state, finishCallback);
        } else if (!state->PendingUrls.empty()) {
            const LoadingState::PendingUrl &pl = state->PendingUrls.front();
            if (!pl.Title.empty()) {
                state->ReferenceName = pl.Title;
            }
            if (pl.PlaylistPosition > -3) { // -3 = don't change
                state->InsertPos = pl.PlaylistPosition;
                state->Flags = LoadingState::UpdateAdditionalPos | LoadingState::IgnoreExistingPosition;
            }

            LoadFromUrl(pl.Url, playlist, state, finishCallback);
            state->PendingUrls.pop();
        } else {
            // Finished
            Config::SaveExtendedConfig();

            DurationResolver::AddPlaylist(playlist);
            DurationResolver::Resolve();

            playlist->Release();
            if (finishCallback)
                finishCallback();
        }
    });
}

void YouTubeAPI::LoadUserPlaylist(Config::Playlist &playlist) {
    if (!Plugin::instance()->isConnected()) {
        OptionsDialog::Connect([&playlist] { LoadUserPlaylist(playlist); });
        return;
    }
    std::wstring playlistId = playlist.ID;
    std::wstring uname = Config::GetString(L"UserYTName");
    std::wstring playlistName(uname + L" - " + playlist.Title);
    std::wstring groupName(uname + L" - " + playlist.Title);

    IAIMPPlaylist *pl = Plugin::instance()->GetPlaylist(playlistName);

    auto state = std::make_shared<LoadingState>();
    state->PlaylistToUpdate = &playlist;
    state->ReferenceName = groupName;
    GetExistingTrackIds(pl, state);

    std::wstring url(L"https://content.googleapis.com/youtube/v3/playlistItems?part=contentDetails%2Csnippet&maxResults=50&playlistId=" + playlistId +
                     L"&fields=items%2Fsnippet%2Ckind%2CnextPageToken%2CpageInfo%2CtokenPagination");

    std::wstring plId;
    IAIMPPropertyList *plProp = nullptr;
    if (SUCCEEDED(pl->QueryInterface(IID_IAIMPPropertyList, reinterpret_cast<void **>(&plProp)))) {
        IAIMPString *str = nullptr;
        if (SUCCEEDED(plProp->GetValueAsObject(AIMP_PLAYLIST_PROPID_ID, IID_IAIMPString, reinterpret_cast<void **>(&str)))) {
            plId = str->GetData();
            str->Release();
        }
        plProp->Release();
    }
    playlist.AIMPPlaylistId = plId;

    if (Config::GetInt32(L"MonitorUserPlaylists", 1)) {
        auto find = [&](const Config::MonitorUrl &p) -> bool { return p.PlaylistID == plId && p.URL == url; };
        if (std::find_if(Config::MonitorUrls.begin(), Config::MonitorUrls.end(), find) == Config::MonitorUrls.end()) {
            Config::MonitorUrls.push_back({ url, plId, state->Flags, groupName });
        }
        Config::SaveExtendedConfig();
    }

    LoadFromUrl(url, pl, state);
}

void YouTubeAPI::GetExistingTrackIds(IAIMPPlaylist *pl, std::shared_ptr<LoadingState> state) {
    if (!pl || !state)
        return;

    // Fetch current track ids from playlist
    Plugin::instance()->ForEveryItem(pl, [&](IAIMPPlaylistItem *, IAIMPFileInfo *, const std::wstring &id) -> int {
        if (!id.empty()) {
            state->TrackIds.insert(id);
        }
        return 0;
    });
}

void YouTubeAPI::ResolveUrl(const std::wstring &url, const std::wstring &playlistTitle, bool createPlaylist) {
    if (url.find(L"youtube.com") != std::wstring::npos || url.find(L"youtu.be") != std::wstring::npos) {
        std::wstring finalUrl;
        rapidjson::Value *addDirectly = nullptr;
        std::wstring plName;
        bool monitor = true;
        auto state = std::make_shared<LoadingState>();
        std::set<std::wstring> toMonitor;
        std::wstring ytPlaylistId;

        if (url.find(L"/user/") != std::wstring::npos) {
            std::wstring id;
            std::wstring::size_type pos;
            if ((pos = url.find(L"/user/")) != std::wstring::npos) {
                id = url.c_str() + pos + 6;
            }
            if ((pos = id.find(L'/')) != std::wstring::npos)
                id.resize(pos);

            finalUrl = L"https://www.googleapis.com/youtube/v3/channels?part=contentDetails%2Csnippet&hl=" + Plugin::instance()->Lang(L"YouTube\\YouTubeLang") + L"&forUsername=" + id + L"&fields=items(contentDetails%2Csnippet)";
            plName = L"YouTube";
        } else if (url.find(L"/channel/") != std::wstring::npos) {
            std::wstring id;
            std::wstring::size_type pos;
            if ((pos = url.find(L"/channel/")) != std::wstring::npos) {
                id = url.c_str() + pos + 9;
            }
            if ((pos = id.find(L'/')) != std::wstring::npos)
                id.resize(pos);

            finalUrl = L"https://www.googleapis.com/youtube/v3/channels?part=contentDetails%2Csnippet&hl=" + Plugin::instance()->Lang(L"YouTube\\YouTubeLang") + L"&id=" + id + L"&fields=items(contentDetails%2Csnippet)";
            plName = L"YouTube";
        } else if (url.find(L"list=") != std::wstring::npos) {
            std::wstring id;
            std::wstring::size_type pos;
            if ((pos = url.find(L"?list=")) != std::wstring::npos) {
                id = url.c_str() + pos + 6;
            } else if ((pos = url.find(L"&list=")) != std::wstring::npos) {
                id = url.c_str() + pos + 6;
            }
            if ((pos = id.find(L'&')) != std::wstring::npos)
                id.resize(pos);

            finalUrl = L"https://content.googleapis.com/youtube/v3/playlistItems?part=contentDetails%2Csnippet&maxResults=50&playlistId=" + id +
                       L"&fields=items%2Fsnippet%2Ckind%2CnextPageToken%2CpageInfo%2CtokenPagination";
            plName = L"YouTube";
            ytPlaylistId = id;
        } else if (url.find(L"watch?") != std::wstring::npos) {
            std::wstring id = Tools::TrackIdFromUrl(url);
            finalUrl = L"https://www.googleapis.com/youtube/v3/videos?part=contentDetails%2Csnippet&hl=" + Plugin::instance()->Lang(L"YouTube\\YouTubeLang") + L"&id=" + id;
            plName = L"YouTube";
        } else if (url.find(L"youtu.be") != std::wstring::npos) {
            std::wstring id = Tools::TrackIdFromUrl(url);
            finalUrl = L"https://www.googleapis.com/youtube/v3/videos?part=contentDetails%2Csnippet&hl=" + Plugin::instance()->Lang(L"YouTube\\YouTubeLang") + L"&id=" + id;
            plName = L"YouTube";
        }

        IAIMPPlaylist *pl = nullptr;
        std::wstring finalPlaylistName;
        if (createPlaylist) {
            finalPlaylistName = playlistTitle.empty() ? plName : playlistTitle;
            pl = Plugin::instance()->GetPlaylist(finalPlaylistName);
            if (!pl)
                return;
        } else {
            pl = Plugin::instance()->GetCurrentPlaylist();
            if (!pl)
                return;
        }

        std::wstring playlistId;
        IAIMPPropertyList *plProp = nullptr;
        if (SUCCEEDED(pl->QueryInterface(IID_IAIMPPropertyList, reinterpret_cast<void **>(&plProp)))) {
            IAIMPString *str = nullptr;
            if (SUCCEEDED(plProp->GetValueAsObject(AIMP_PLAYLIST_PROPID_ID, IID_IAIMPString, reinterpret_cast<void **>(&str)))) {
                playlistId = str->GetData();
                str->Release();
            }
            if (finalPlaylistName.empty()) {
                if (SUCCEEDED(plProp->GetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, IID_IAIMPString, reinterpret_cast<void **>(&str)))) {
                    finalPlaylistName = str->GetData();
                    str->Release();
                }
            }
            plProp->Release();
        }
        if (!ytPlaylistId.empty()) {
            AimpHTTP::Get(L"https://www.googleapis.com/youtube/v3/playlists?part=snippet&hl=" + Plugin::instance()->Lang(L"YouTube\\YouTubeLang") + L"&id=" + ytPlaylistId + L"&key=" TEXT(APP_KEY), [pl](unsigned char *data, int size) {
                rapidjson::Document d;
                d.Parse(reinterpret_cast<const char *>(data));
                if (d.IsObject() && d.HasMember("items") && d["items"].IsArray() && d["items"].Size() > 0 && d["items"][0].HasMember("snippet")) {
                    rapidjson::Value &val = d["items"][0]["snippet"];
                    std::wstring channelName = Tools::ToWString(val["channelTitle"]);
                    std::wstring playlistTitle = Tools::ToWString(val["localized"]["title"]);
                    IAIMPPropertyList *plProp = nullptr;
                    if (SUCCEEDED(pl->QueryInterface(IID_IAIMPPropertyList, reinterpret_cast<void **>(&plProp)))) {
                        plProp->SetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, AIMPString(channelName + L" - " + playlistTitle));
                        plProp->Release();
                    }
                }
            });
        }

        state->ReferenceName = plName;

        GetExistingTrackIds(pl, state);

        if (addDirectly) {
            AddFromJson(pl, *addDirectly, state);
        } else {
            LoadFromUrl(finalUrl, pl, state);
        }

        if (monitor) {
            toMonitor.insert(finalUrl);
            for (const auto &x : toMonitor) {
                auto find = [&](const Config::MonitorUrl &p) -> bool { return p.PlaylistID == playlistId && p.URL == x; };
                if (std::find_if(Config::MonitorUrls.begin(), Config::MonitorUrls.end(), find) == Config::MonitorUrls.end()) {
                    Config::MonitorUrls.push_back({ x, playlistId, state->Flags, plName });
                }
            }
            Config::SaveExtendedConfig();
        }
        return;
    }

	if (g_MainThreadId == GetCurrentThreadId())
		MessageBox(
			Plugin::instance()->GetMainWindowHandle(), 
			Plugin::instance()->Lang(L"YouTube.Messages\\CantResolve").c_str(), 
			Plugin::instance()->Lang(L"YouTube.Messages\\Error").c_str(), 
			MB_OK | MB_ICONERROR);
}

std::wstring messageBox(const std::wstring &msg, bool getLastError)
{
	std::wstring err = msg;

	if (getLastError)
	{
		DWORD errorMessageID = GetLastError();
		if (errorMessageID != 0)
		{
			LPWSTR messageBuffer = nullptr;
			size_t size = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				nullptr, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&messageBuffer), 0, nullptr);

			if (size > 0)
				err = msg + L" - " + messageBuffer;
			LocalFree(messageBuffer);
		}
	}

	if (g_MainThreadId == GetCurrentThreadId())
		MessageBox(Plugin::instance()->GetMainWindowHandle(), err.c_str(), Plugin::instance()->Lang(L"YouTube.Messages\\Error").c_str(), MB_OK | MB_ICONERROR);
	return nullptr;
}

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
std::wstring getYoutubeDl()
{
	static std::wstring youtube_dl;
	if (youtube_dl.empty())
	{
		WCHAR path[MAX_PATH], drive[_MAX_DRIVE], dir[_MAX_DIR], file[_MAX_FNAME], ext[_MAX_EXT];

		DWORD pathLen = GetModuleFileName(reinterpret_cast<HINSTANCE>(&__ImageBase), path, MAX_PATH);
		if (!pathLen)
			return messageBox(L"GetModuleFileName");

		_wsplitpath_s(path, drive, dir, file, ext);
		_wmakepath_s(path, drive, dir, L"youtube-dl", L"exe");

		youtube_dl = path;
	}
	return youtube_dl;
}

std::wstring YouTubeAPI::GetStreamUrl(const std::wstring &id) {
	std::wstring youtube_dl = L"\"" + getYoutubeDl() + L"\" -g " + Plugin::instance()->YoutubeDLCmd() + L" -- " + id;

	HANDLE pipeReadOut = nullptr;
	HANDLE pipeReadErr = nullptr;
	HANDLE pipeWriteOut = nullptr;
	HANDLE pipeWriteErr = nullptr;

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = nullptr;

	if (!CreatePipe(&pipeReadOut, &pipeWriteOut, &sa, 0))
		return messageBox(L"CreatePipe");
	if (!CreatePipe(&pipeReadErr, &pipeWriteErr, &sa, 0))
		return messageBox(L"CreatePipe");
	if (!SetHandleInformation(pipeReadOut, HANDLE_FLAG_INHERIT, 0))
		return messageBox(L"SetHandleInformation");
	if (!SetHandleInformation(pipeReadErr, HANDLE_FLAG_INHERIT, 0))
		return messageBox(L"SetHandleInformation");

	STARTUPINFO si;
	ZeroMemory(&si, sizeof si);
	si.cb = sizeof STARTUPINFO;
	si.hStdOutput = pipeWriteOut;
	si.hStdError = pipeWriteErr;
	si.dwFlags |= STARTF_USESTDHANDLES;

	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof pi);

	if (!CreateProcess(nullptr, const_cast<LPWSTR>(youtube_dl.c_str()), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		return messageBox(L"CreateProcess");

	DWORD waitResult = WaitForSingleObject(pi.hProcess, Plugin::instance()->YoutubeDLTimeout() * 1000);
	if (waitResult == WAIT_TIMEOUT)
		return std::wstring();
	if (waitResult != WAIT_OBJECT_0)
		return messageBox(L"WaitForSingleObject");

	DWORD lpExitCode;
	if (!GetExitCodeProcess(pi.hProcess, &lpExitCode))
		return messageBox(L"GetExitCodeProcess");

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(pipeWriteOut);
	CloseHandle(pipeWriteErr);

	DWORD dwRead;
	std::string result;
	std::array<char, 512> buffer;
	do
	{
		ReadFile(pipeReadOut, buffer.data(), buffer.size(), &dwRead, nullptr);
		result.append(buffer.data(), dwRead);
	} while (dwRead > 0);
	CloseHandle(pipeReadOut);

	std::string error;
	do
	{
		ReadFile(pipeReadErr, buffer.data(), buffer.size(), &dwRead, nullptr);
		error.append(buffer.data(), dwRead);
	} while (dwRead > 0);
	CloseHandle(pipeReadErr);

	std::wstring wError = Tools::ToWString(error);
	if (lpExitCode)
		return messageBox(wError, false);

	std::wstring wResult = Tools::ToWString(result);
	return wResult;
}

void YouTubeAPI::AddToPlaylist(Config::Playlist &pl, const std::wstring &trackId) {
    if (!Plugin::instance()->isConnected()) {
        OptionsDialog::Connect([&pl, trackId] { AddToPlaylist(pl, trackId); });
        return;
    }
    std::string postData("{"
        "\"snippet\": {"
            "\"playlistId\": \"" + Tools::ToString(pl.ID) + "\","
            "\"resourceId\": {\"videoId\": \""+Tools::ToString(trackId)+"\", \"kind\": \"youtube#video\" }"
        "}"
    "}");
    std::wstring headers(L"\r\nContent-Type: application/json"
                         L"\r\nAuthorization: Bearer " + Plugin::instance()->getAccessToken());

    AimpHTTP::Post(L"https://www.googleapis.com/youtube/v3/playlistItems?part=snippet" + headers, postData, [&pl, trackId](unsigned char *data, int size) {
        if (strstr(reinterpret_cast<char *>(data), "youtube#playlistItem")) {
            pl.Items.insert(trackId);
            Config::SaveExtendedConfig();

            Timer::SingleShot(0, Plugin::MonitorCallback);
        }
    });
}

void YouTubeAPI::RemoveFromPlaylist(Config::Playlist &pl, const std::wstring &trackId) {
    if (!Plugin::instance()->isConnected()) {
        OptionsDialog::Connect([&pl, trackId] { RemoveFromPlaylist(pl, trackId); });
        return;
    }
    std::wstring headers(L"\r\nAuthorization: Bearer " + Plugin::instance()->getAccessToken());

    AimpHTTP::Get(L"https://content.googleapis.com/youtube/v3/playlistItems?part=id&videoId=" + trackId + L"&playlistId=" + pl.ID +
                  L"&fields=items%2Fid" + headers, [&pl, trackId, headers](unsigned char *data, int size) {
        rapidjson::Document d;
        d.Parse(reinterpret_cast<const char *>(data));

        if (d.IsObject() && d.HasMember("items") && d["items"].IsArray() && d["items"].Size() > 0) {
            std::wstring url(L"https://www.googleapis.com/youtube/v3/playlistItems?id=" + Tools::ToWString(d["items"][0]["id"]));

            url += L"\r\nX-HTTP-Method-Override: DELETE";
            url += headers;

            AimpHTTP::Post(url, std::string(), [&pl, trackId](unsigned char *data, int size) {
                if (strlen(reinterpret_cast<char *>(data)) == 0) {
                    // removed correctly
                    pl.Items.erase(trackId);
                    Config::SaveExtendedConfig();

                    if (IAIMPPlaylist *playlist = Plugin::instance()->GetPlaylistById(pl.AIMPPlaylistId)) {
                        Plugin::instance()->ForEveryItem(playlist, [&trackId](IAIMPPlaylistItem *, IAIMPFileInfo *, const std::wstring &id) {
                            if (!id.empty() && id == trackId) {
                                return Plugin::FLAG_DELETE_ITEM | Plugin::FLAG_STOP_LOOP;
                            }
                            return 0;
                        });
                        playlist->Release();
                    }
                }
            });
        }
    });
}

