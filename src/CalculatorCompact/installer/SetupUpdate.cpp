// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "SetupActions.h"
#include "SetupUpdate.h"
#include "version.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <cwctype>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace Setup
{
    namespace
    {
        // Where releases come from. The download has to be one of this
        // repository's own release assets; GitHub then redirects it to its
        // storage host, which WinHTTP follows over HTTPS only.
        constexpr wchar_t kLatestRelease[] = L"https://api.github.com/repos/Eripto/calculator/releases/latest";
        constexpr wchar_t kAssetPrefix[] = L"https://github.com/Eripto/calculator/releases/download/";
        constexpr char kAssetName[] = "CalculatorSetup.exe";
        constexpr size_t kMaxReleaseJson = 1u << 20;
        constexpr size_t kMaxInstaller = 16u << 20;

        constexpr wchar_t kStateKey[] = L"Software\\CompactCalculator";

        std::wstring Widen(const std::string& text)
        {
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
            return out;
        }

        // --------------------------------------------------------- HTTPS

        struct Handle
        {
            HINTERNET h = nullptr;
            ~Handle()
            {
                if (h != nullptr)
                {
                    WinHttpCloseHandle(h);
                }
            }
        };

        // One GET, HTTPS only, with the body capped at `limit`. Returns the
        // HTTP status, or 0 if no answer came back at all.
        constexpr wchar_t kApiHeaders[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        constexpr wchar_t kDownloadHeaders[] = L"Accept: application/octet-stream\r\n";

        DWORD HttpsGet(const std::wstring& url, const wchar_t* headers, size_t limit, std::string& body)
        {
            wchar_t host[256];
            wchar_t path[2048];
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            parts.lpszHostName = host;
            parts.dwHostNameLength = ARRAYSIZE(host);
            parts.lpszUrlPath = path;
            parts.dwUrlPathLength = ARRAYSIZE(path);
            if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
            {
                return 0;
            }

            // The system proxy settings, as a browser would use them.
            // AUTOMATIC_PROXY is Windows 8.1 and later; DEFAULT_PROXY is the
            // fallback for anything older.
            Handle session{ WinHttpOpen(L"CompactCalculator-Setup", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0) };
            if (session.h == nullptr)
            {
                session.h = WinHttpOpen(L"CompactCalculator-Setup", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
            }
            if (session.h == nullptr)
            {
                return 0;
            }
            WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);

            Handle connection{ WinHttpConnect(session.h, host, parts.nPort, 0) };
            Handle request{ connection.h ? WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                         : nullptr };
            if (request.h == nullptr)
            {
                return 0;
            }
            // Redirects are followed, but never from HTTPS down to HTTP.
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
            WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

            if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                || !WinHttpReceiveResponse(request.h, nullptr))
            {
                return 0;
            }
            DWORD status = 0;
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);

            body.clear();
            for (;;)
            {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request.h, &available))
                {
                    return 0;
                }
                if (available == 0)
                {
                    break;
                }
                if (body.size() + available > limit)
                {
                    return 0; // far bigger than anything this should be
                }
                const size_t at = body.size();
                body.resize(at + available);
                DWORD read = 0;
                if (!WinHttpReadData(request.h, &body[at], available, &read))
                {
                    return 0;
                }
                body.resize(at + read);
            }
            return status;
        }

        // ---------------------------------------------------------- JSON
        //
        // Just enough JSON to read a release: strings, and skipping whatever
        // else a value turns out to be. Anything malformed stops the parse,
        // and a release that did not parse is not offered.

        struct JsonReader
        {
            const char* p;
            const char* end;
            bool ok = true;

            void Space()
            {
                while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                {
                    ++p;
                }
            }

            bool Take(char c)
            {
                Space();
                if (p < end && *p == c)
                {
                    ++p;
                    return true;
                }
                return false;
            }

            bool Peek(char c)
            {
                Space();
                return p < end && *p == c;
            }

            std::string String()
            {
                std::string out;
                if (!Take('"'))
                {
                    ok = false;
                    return out;
                }
                while (p < end && *p != '"')
                {
                    char c = *p++;
                    if (c == '\\' && p < end)
                    {
                        c = *p++;
                        switch (c)
                        {
                        case 'n': c = '\n'; break;
                        case 't': c = '\t'; break;
                        case 'r': c = '\r'; break;
                        case 'b': c = '\b'; break;
                        case 'f': c = '\f'; break;
                        case 'u':
                            // Only ASCII matters for the fields read here;
                            // anything else becomes '?', which no URL,
                            // version or digest this accepts contains.
                            if (end - p >= 4)
                            {
                                const unsigned code = strtoul(std::string(p, p + 4).c_str(), nullptr, 16);
                                c = code < 0x80 ? static_cast<char>(code) : '?';
                                p += 4;
                            }
                            break;
                        default: break; // \" \\ \/
                        }
                    }
                    out += c;
                }
                ok = ok && Take('"');
                return out;
            }

            void Skip(int depth = 0)
            {
                Space();
                if (!ok || p >= end || depth > 64)
                {
                    ok = false;
                    return;
                }
                if (*p == '"')
                {
                    String();
                }
                else if (*p == '{' || *p == '[')
                {
                    const char close = *p == '{' ? '}' : ']';
                    const bool object = *p == '{';
                    ++p;
                    if (Take(close))
                    {
                        return;
                    }
                    do
                    {
                        if (object)
                        {
                            String();
                            ok = ok && Take(':');
                        }
                        Skip(depth + 1);
                    } while (ok && Take(','));
                    ok = ok && Take(close);
                }
                else
                {
                    // A number, true, false or null.
                    const char* start = p;
                    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
                    {
                        ++p;
                    }
                    ok = p > start;
                }
            }

            // Reads the value in place as a number or literal token.
            std::string Token()
            {
                Space();
                const char* start = p;
                Skip();
                return std::string(start, p);
            }
        };

        bool ParseRelease(const std::string& body, Release& release)
        {
            JsonReader json{ body.data(), body.data() + body.size() };
            std::string tag;
            bool usable = true;
            if (!json.Take('{'))
            {
                return false;
            }
            if (!json.Take('}'))
            {
                do
                {
                    const std::string key = json.String();
                    json.ok = json.ok && json.Take(':');
                    if (key == "tag_name" && json.Peek('"'))
                    {
                        tag = json.String();
                    }
                    else if (key == "draft" || key == "prerelease")
                    {
                        usable = usable && json.Token() == "false";
                    }
                    else if (key == "assets" && json.Take('['))
                    {
                        if (json.Take(']'))
                        {
                            continue;
                        }
                        do
                        {
                            if (!json.Take('{'))
                            {
                                json.ok = false;
                                break;
                            }
                            std::string name, url, digest, size;
                            if (!json.Take('}'))
                            {
                                do
                                {
                                    const std::string field = json.String();
                                    json.ok = json.ok && json.Take(':');
                                    if (field == "name" && json.Peek('"'))
                                    {
                                        name = json.String();
                                    }
                                    else if (field == "browser_download_url" && json.Peek('"'))
                                    {
                                        url = json.String();
                                    }
                                    else if (field == "digest" && json.Peek('"'))
                                    {
                                        digest = json.String();
                                    }
                                    else if (field == "size")
                                    {
                                        size = json.Token();
                                    }
                                    else
                                    {
                                        json.Skip();
                                    }
                                } while (json.ok && json.Take(','));
                                json.ok = json.ok && json.Take('}');
                            }
                            if (lstrcmpiA(name.c_str(), kAssetName) == 0)
                            {
                                release.downloadUrl = Widen(url);
                                release.size = strtoull(size.c_str(), nullptr, 10);
                                release.sha256.clear();
                                if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64)
                                {
                                    for (size_t i = 7; i < digest.size(); ++i)
                                    {
                                        release.sha256 += static_cast<wchar_t>(towlower(static_cast<wchar_t>(digest[i])));
                                    }
                                }
                            }
                        } while (json.ok && json.Take(','));
                        json.ok = json.ok && json.Take(']');
                    }
                    else
                    {
                        json.Skip();
                    }
                } while (json.ok && json.Take(','));
            }
            if (!json.ok || !usable || tag.empty() || release.downloadUrl.empty())
            {
                return false;
            }

            release.version = Widen(tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag);
            // The asset has to come from this repository's own releases.
            return release.downloadUrl.compare(0, ARRAYSIZE(kAssetPrefix) - 1, kAssetPrefix) == 0
                && release.size > 0 && release.size <= kMaxInstaller;
        }

        std::wstring Sha256Hex(const std::string& data)
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            UCHAR digest[32]{};
            bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
                && BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0
                && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) == 0
                && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
            if (hash != nullptr)
            {
                BCryptDestroyHash(hash);
            }
            if (algorithm != nullptr)
            {
                BCryptCloseAlgorithmProvider(algorithm, 0);
            }
            std::wstring hex;
            if (ok)
            {
                for (UCHAR b : digest)
                {
                    hex += L"0123456789abcdef"[b >> 4];
                    hex += L"0123456789abcdef"[b & 15];
                }
            }
            return hex;
        }

        DWORD ReadDword(const wchar_t* name, DWORD fallback)
        {
            DWORD value = fallback;
            DWORD size = sizeof(value);
            RegGetValueW(HKEY_CURRENT_USER, kStateKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
            return value;
        }
    }

    const wchar_t* Version()
    {
        return L"" CALC_VERSION;
    }

    int CompareVersions(const std::wstring& a, const std::wstring& b)
    {
        size_t i = 0;
        size_t j = 0;
        for (int part = 0; part < 4; ++part)
        {
            unsigned long x = 0;
            unsigned long y = 0;
            while (i < a.size() && iswdigit(a[i]))
            {
                x = x * 10 + (a[i++] - L'0');
            }
            while (j < b.size() && iswdigit(b[j]))
            {
                y = y * 10 + (b[j++] - L'0');
            }
            if (x != y)
            {
                return x < y ? -1 : 1;
            }
            // Only a dot carries on to the next part; anything else, such as a
            // "-beta" suffix, ends the number there.
            i = (i < a.size() && a[i] == L'.') ? i + 1 : a.size();
            j = (j < b.size() && b[j] == L'.') ? j + 1 : b.size();
        }
        return 0;
    }

    CheckResult CheckForUpdate(Release& release)
    {
        std::string body;
        const DWORD status = HttpsGet(kLatestRelease, kApiHeaders, kMaxReleaseJson, body);
        if (status == 404)
        {
            return CheckResult::UpToDate; // no releases published yet
        }
        if (status != 200 || !ParseRelease(body, release))
        {
            return CheckResult::Failed;
        }
        return CompareVersions(release.version, Version()) > 0 ? CheckResult::UpdateAvailable : CheckResult::UpToDate;
    }

    bool DownloadUpdate(const Release& release, std::wstring& path, std::wstring& error)
    {
        std::string data;
        if (HttpsGet(release.downloadUrl, kDownloadHeaders, kMaxInstaller, data) != 200)
        {
            error = L"Setup couldn't download the update. Check your connection and try again.";
            return false;
        }
        // Checked in this order so the message says what actually went wrong.
        if (data.size() != release.size || data.size() < 2 || data[0] != 'M' || data[1] != 'Z')
        {
            error = L"The download was incomplete or wasn't an installer, so it wasn't run.";
            return false;
        }
        if (!release.sha256.empty() && Sha256Hex(data) != release.sha256)
        {
            error = L"The download didn't match the checksum GitHub published for it, so it wasn't run.";
            return false;
        }

        // A version made only of digits and dots goes into the file name;
        // anything else in a tag is left out of the path.
        std::wstring version;
        for (wchar_t c : release.version)
        {
            if (iswdigit(c) || c == L'.')
            {
                version += c;
            }
        }
        wchar_t temp[MAX_PATH + 1]{};
        GetTempPathW(ARRAYSIZE(temp), temp);
        path = std::wstring(temp) + L"CalculatorSetup-" + version + L".exe";

        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
            && written == data.size();
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
        }
        if (!ok)
        {
            DeleteFileW(path.c_str());
            error = L"Setup couldn't save the update to your temporary folder.";
        }
        return ok;
    }

    bool UpdateChecksEnabled()
    {
        return ReadDword(L"UpdateChecks", 1) != 0;
    }

    void RecordAvailableUpdate(const std::wstring& version)
    {
        RegSetKeyValueW(HKEY_CURRENT_USER, kStateKey, L"UpdateVersion", REG_SZ, version.c_str(),
                        static_cast<DWORD>((version.size() + 1) * sizeof(wchar_t)));
    }

    void ClearAvailableUpdate()
    {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kStateKey, L"UpdateVersion");
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kStateKey, L"UpdateDismissed");
    }

    namespace
    {
        // Asked afresh rather than trusting what the daily check recorded:
        // the download needs the release's URL, size and digest as they are
        // now, and the answer may have changed since.
        StepResult FindStep(Plan& plan)
        {
            switch (CheckForUpdate(plan.release))
            {
            case CheckResult::UpdateAvailable:
                RecordAvailableUpdate(plan.release.version);
                return {};
            case CheckResult::UpToDate:
                ClearAvailableUpdate();
                plan.upToDate = true;
                return { Outcome::Skipped, std::wstring() };
            default:
                return { Outcome::Failed, L"Setup couldn't reach GitHub. Check your connection and try again." };
            }
        }

        StepResult DownloadStep(Plan& plan)
        {
            if (plan.upToDate)
            {
                return { Outcome::Skipped, std::wstring() };
            }
            std::wstring error;
            if (!DownloadUpdate(plan.release, plan.downloadedPath, error))
            {
                plan.downloadedPath.clear();
                return { Outcome::Failed, error };
            }
            return {};
        }
    }

    std::unique_ptr<Plan> PlanDownload(HWND owner)
    {
        auto plan = std::make_unique<Plan>();
        plan->owner = owner;
        plan->downloading = true;
        plan->steps.push_back({ L"Finding the latest release", FindStep });
        plan->steps.push_back({ L"Downloading the update", DownloadStep });
        return plan;
    }
}
