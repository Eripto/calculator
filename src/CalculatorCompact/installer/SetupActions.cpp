// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "SetupActions.h"
#include "SetupUpdate.h"
#include "Unpack.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wincrypt.h>

#include <algorithm>
#include <cwctype>

namespace Setup
{
    namespace
    {
        // ----------------------------------------------------------- places

        constexpr wchar_t kStateKey[] = L"Software\\CompactCalculator";
        constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CompactCalculator";
        constexpr wchar_t kIfeoRoot[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\";
        constexpr wchar_t kIfeoBackupKey[] = L"SOFTWARE\\CompactCalculator";
        constexpr wchar_t kAppKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AppKey\\18";
        constexpr wchar_t kProtocolKey[] = L"Software\\Classes\\calculator";
        constexpr wchar_t kProtocolCommandKey[] = L"Software\\Classes\\calculator\\shell\\open\\command";
        constexpr int kPayloadId = 200;

        // The calculator has shipped under both names; win32calc.exe is the
        // classic one, still on Server SKUs and machines upgraded from 7.
        constexpr const wchar_t* kImages[] = { L"calc.exe", L"win32calc.exe" };

        // The five COM and shell GUIDs Setup uses, spelled out here rather
        // than linked from libuuid, which brings some 50KB of every other GUID
        // Windows defines along with them.
        constexpr GUID kFolderPrograms = { 0xA77F5D77, 0x2E2B, 0x44C3, { 0xA6, 0xA2, 0xAB, 0xA6, 0x01, 0x05, 0x4A, 0x51 } };
        constexpr GUID kFolderLocalAppData = { 0xF1B32785, 0x6FBA, 0x4FCF, { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 } };
        constexpr GUID kClsidShellLink = { 0x00021401, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
        constexpr GUID kIidShellLinkW = { 0x000214F9, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
        constexpr GUID kIidPersistFile = { 0x0000010B, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

        // IFEO is read by whoever calls CreateProcess, in that caller's view of
        // the registry, so a 32-bit program launching calc.exe looks in the
        // WOW64 copy. Both are written, and both restored.
        struct View
        {
            REGSAM sam;
            const wchar_t* tag;
        };
        // Named explicitly rather than left as "native", so a 32-bit build of
        // Setup still reaches the 64-bit view. Both flags are ignored on
        // 32-bit Windows, where there is only one.
        constexpr View kViews[] = { { KEY_WOW64_64KEY, L"64" }, { KEY_WOW64_32KEY, L"32" } };

        // ------------------------------------------------------- registry

        bool ReadString(HKEY root, const std::wstring& path, const wchar_t* name, std::wstring& out, REGSAM view = 0)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS)
            {
                return false;
            }
            DWORD type = 0;
            DWORD size = 0;
            LONG rc = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
            if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
            {
                RegCloseKey(key);
                return false;
            }
            std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
            rc = RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<BYTE*>(buffer.data()), &size);
            RegCloseKey(key);
            if (rc != ERROR_SUCCESS)
            {
                return false;
            }
            buffer.resize(wcsnlen(buffer.c_str(), buffer.size()));
            out = buffer;
            return true;
        }

        bool ReadDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD& out)
        {
            DWORD size = sizeof(out);
            return RegGetValueW(root, path.c_str(), name, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
        }

        bool WriteString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value, REGSAM view = 0)
        {
            HKEY key = nullptr;
            if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &key, nullptr) != ERROR_SUCCESS)
            {
                return false;
            }
            const LONG rc = RegSetValueExW(
                key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
            return rc == ERROR_SUCCESS;
        }

        bool WriteDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD value)
        {
            HKEY key = nullptr;
            if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            {
                return false;
            }
            const LONG rc = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
            RegCloseKey(key);
            return rc == ERROR_SUCCESS;
        }

        void DeleteValue(HKEY root, const std::wstring& path, const wchar_t* name, REGSAM view = 0)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, path.c_str(), 0, KEY_SET_VALUE | view, &key) == ERROR_SUCCESS)
            {
                RegDeleteValueW(key, name);
                RegCloseKey(key);
            }
        }

        // Leaves a key that anything else has put a value or subkey in.
        void DeleteKeyIfEmpty(HKEY root, const std::wstring& path, REGSAM view = 0)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS)
            {
                return;
            }
            DWORD subkeys = 0;
            DWORD values = 0;
            RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
            RegCloseKey(key);
            if (subkeys == 0 && values == 0)
            {
                RegDeleteKeyExW(root, path.c_str(), view, 0);
            }
        }

        // -------------------------------------------------------- paths

        std::wstring KnownFolder(REFKNOWNFOLDERID id)
        {
            PWSTR path = nullptr;
            std::wstring result;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path != nullptr)
            {
                result = path;
            }
            CoTaskMemFree(path);
            return result;
        }

        std::wstring ModulePath()
        {
            std::wstring buffer(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length < buffer.size())
                {
                    buffer.resize(length);
                    return buffer;
                }
                buffer.resize(buffer.size() * 2);
            }
        }

        std::wstring SystemDirectory()
        {
            wchar_t buffer[MAX_PATH]{};
            GetSystemDirectoryW(buffer, MAX_PATH);
            return buffer;
        }

        bool FileExists(const std::wstring& path)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
        }

        bool SamePath(const std::wstring& a, const std::wstring& b)
        {
            return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
        }

        // A command value is either a quoted path, possibly followed by
        // arguments, or a bare path.
        std::wstring CommandPath(std::wstring value)
        {
            size_t start = 0;
            while (start < value.size() && iswspace(value[start]))
            {
                ++start;
            }
            if (start < value.size() && value[start] == L'"')
            {
                const size_t end = value.find(L'"', start + 1);
                return value.substr(start + 1, end == std::wstring::npos ? std::wstring::npos : end - start - 1);
            }
            size_t end = value.size();
            while (end > start && iswspace(value[end - 1]))
            {
                --end;
            }
            return value.substr(start, end - start);
        }

        // Whether a setting still points at this install. Anything pointing
        // into a CompactCalculator directory counts, which covers an install
        // made by the script and one that has since moved.
        bool IsOurs(const std::wstring& value, const std::wstring& target)
        {
            const std::wstring path = CommandPath(value);
            if (SamePath(path, target))
            {
                return true;
            }
            // A literal, not a static std::wstring: this runs on both the UI and
            // worker threads, and Setup is built without thread-safe statics.
            const wchar_t tail[] = L"\\CompactCalculator\\Calculator.exe";
            const size_t tailLength = ARRAYSIZE(tail) - 1;
            return path.size() >= tailLength && SamePath(path.substr(path.size() - tailLength), tail);
        }

        // ------------------------------------------------------ processes

        bool IsElevated()
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                return false;
            }
            TOKEN_ELEVATION elevation{};
            DWORD size = 0;
            const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
            CloseHandle(token);
            return ok && elevation.TokenIsElevated;
        }

        BOOL CALLBACK CloseWindowsOf(HWND hwnd, LPARAM pid)
        {
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner == static_cast<DWORD>(pid) && IsWindowVisible(hwnd))
            {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return TRUE;
        }

        // An image that is running cannot be replaced or deleted, so any copy
        // of the installed calculator is asked to close first, and only ended
        // if it does not.
        void CloseRunning(const std::wstring& exe)
        {
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
            {
                return;
            }
            PROCESSENTRY32W entry{ sizeof(entry) };
            for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
            {
                if (entry.th32ProcessID == GetCurrentProcessId())
                {
                    continue;
                }
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (process == nullptr)
                {
                    continue;
                }
                wchar_t image[MAX_PATH * 2]{};
                DWORD length = ARRAYSIZE(image);
                if (QueryFullProcessImageNameW(process, 0, image, &length) && SamePath(image, exe))
                {
                    EnumWindows(CloseWindowsOf, static_cast<LPARAM>(entry.th32ProcessID));
                    if (WaitForSingleObject(process, 3000) != WAIT_OBJECT_0)
                    {
                        TerminateProcess(process, 0);
                        WaitForSingleObject(process, 2000);
                    }
                }
                CloseHandle(process);
            }
            CloseHandle(snapshot);
        }

        std::wstring Base64(const BYTE* data, DWORD size)
        {
            DWORD chars = 0;
            const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
            if (!CryptBinaryToStringW(data, size, flags, nullptr, &chars))
            {
                return {};
            }
            std::wstring out(chars, L'\0');
            if (!CryptBinaryToStringW(data, size, flags, out.data(), &chars))
            {
                return {};
            }
            out.resize(wcsnlen(out.c_str(), out.size()));
            return out;
        }

        std::wstring FromBase64Utf16(const std::string& text)
        {
            DWORD bytes = 0;
            if (text.empty() || !CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr))
            {
                return {};
            }
            std::wstring out(bytes / sizeof(wchar_t), L'\0');
            if (!CryptStringToBinaryA(text.c_str(), 0, CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(out.data()), &bytes, nullptr, nullptr))
            {
                return {};
            }
            return out;
        }

        // Runs Windows PowerShell with no window. Only the Store app needs it:
        // Appx registration has no flat Win32 API.
        //
        // The script travels as -EncodedCommand, so no path in it ever has to
        // survive command-line quoting, and anything it prints is read back
        // from a pipe. The wait is bounded; Appx operations can be slow but
        // should never take minutes.
        bool RunPowerShell(const std::wstring& script, DWORD& exitCode, std::string* output, DWORD timeoutMs = 120000)
        {
            const std::wstring encoded = Base64(reinterpret_cast<const BYTE*>(script.c_str()), static_cast<DWORD>(script.size() * sizeof(wchar_t)));
            if (encoded.empty())
            {
                return false;
            }
            std::wstring command = L"\"" + SystemDirectory() + L"\\WindowsPowerShell\\v1.0\\powershell.exe\""
                L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + encoded;

            SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
            HANDLE readPipe = nullptr;
            HANDLE writePipe = nullptr;
            if (!CreatePipe(&readPipe, &writePipe, &inherit, 0))
            {
                return false;
            }
            SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
            HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            startup.hStdInput = nul;
            startup.hStdOutput = writePipe;
            startup.hStdError = nul;

            PROCESS_INFORMATION process{};
            const BOOL started = CreateProcessW(
                nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
            CloseHandle(writePipe);
            if (nul != INVALID_HANDLE_VALUE)
            {
                CloseHandle(nul);
            }
            if (!started)
            {
                CloseHandle(readPipe);
                return false;
            }

            std::string collected;
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            bool finished = false;
            for (;;)
            {
                DWORD available = 0;
                while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0)
                {
                    char chunk[512];
                    DWORD read = 0;
                    if (!ReadFile(readPipe, chunk, (std::min)(available, static_cast<DWORD>(sizeof(chunk))), &read, nullptr) || read == 0)
                    {
                        break;
                    }
                    collected.append(chunk, read);
                }
                if (finished)
                {
                    break;
                }
                if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0)
                {
                    finished = true; // one more pass drains what is left in the pipe
                    continue;
                }
                if (GetTickCount64() > deadline)
                {
                    TerminateProcess(process.hProcess, 1);
                    break;
                }
            }

            exitCode = 1;
            GetExitCodeProcess(process.hProcess, &exitCode);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
            CloseHandle(readPipe);
            if (output != nullptr)
            {
                *output = collected;
            }
            return finished;
        }

        std::wstring PowerShellQuote(const std::wstring& text)
        {
            std::wstring out = L"'";
            for (wchar_t c : text)
            {
                out += c;
                if (c == L'\'')
                {
                    out += L'\'';
                }
            }
            return out + L"'";
        }

        // ------------------------------------------- the script's own record

        // Install-Calculator.ps1 keeps its record as JSON beside the exe. Setup
        // only needs a few fields from it to take an existing install over,
        // so this reads just those rather than parsing JSON in general.
        struct ScriptState
        {
            bool present = false;
            bool ifeo = false;
            bool appKey = false;
            bool protocol = false;
            bool shortcut = false;
            std::wstring storeLocation;
        };

        std::wstring ScriptStatePath(const std::wstring& dir)
        {
            return dir + L"\\install-state.json";
        }

        bool JsonValueAt(const std::wstring& json, const wchar_t* key, size_t& position)
        {
            const std::wstring needle = std::wstring(L"\"") + key + L"\"";
            size_t at = json.find(needle);
            if (at == std::wstring::npos)
            {
                return false;
            }
            at += needle.size();
            while (at < json.size() && (iswspace(json[at]) || json[at] == L':'))
            {
                ++at;
            }
            position = at;
            return at < json.size();
        }

        bool JsonTrue(const std::wstring& json, const wchar_t* key)
        {
            size_t at = 0;
            return JsonValueAt(json, key, at) && json.compare(at, 4, L"true") == 0;
        }

        bool JsonString(const std::wstring& json, const wchar_t* key, std::wstring& out)
        {
            size_t at = 0;
            if (!JsonValueAt(json, key, at) || json[at] != L'"')
            {
                return false;
            }
            out.clear();
            for (size_t i = at + 1; i < json.size(); ++i)
            {
                const wchar_t c = json[i];
                if (c == L'"')
                {
                    return true;
                }
                if (c != L'\\' || i + 1 >= json.size())
                {
                    out += c;
                    continue;
                }
                const wchar_t escaped = json[++i];
                switch (escaped)
                {
                case L'n': out += L'\n'; break;
                case L'r': out += L'\r'; break;
                case L't': out += L'\t'; break;
                case L'b': out += L'\b'; break;
                case L'f': out += L'\f'; break;
                case L'u':
                    if (i + 4 < json.size())
                    {
                        out += static_cast<wchar_t>(wcstoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
                        i += 4;
                    }
                    break;
                default: out += escaped; break; // \\ \" \/
                }
            }
            return false;
        }

        ScriptState ReadScriptState(const std::wstring& dir)
        {
            ScriptState state;
            HANDLE file = CreateFileW(ScriptStatePath(dir).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return state;
            }
            std::string bytes;
            char chunk[4096];
            DWORD read = 0;
            while (ReadFile(file, chunk, sizeof(chunk), &read, nullptr) && read > 0 && bytes.size() < (1u << 20))
            {
                bytes.append(chunk, read);
            }
            CloseHandle(file);
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF)
            {
                bytes.erase(0, 3); // Windows PowerShell writes UTF-8 with a BOM
            }
            const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            std::wstring json(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), json.data(), length);

            state.present = true;
            size_t at = 0;
            if (JsonValueAt(json, L"Ifeo", at) && json[at] == L'{')
            {
                const size_t end = json.find(L'}', at);
                state.ifeo = json.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at).find(L"\"calc.exe\"") != std::wstring::npos;
            }
            state.appKey = JsonTrue(json, L"AppKeyApplied");
            state.protocol = JsonTrue(json, L"ProtocolApplied");
            std::wstring shortcut;
            state.shortcut = JsonString(json, L"ShortcutPath", shortcut) && !shortcut.empty();
            JsonString(json, L"InstallLocation", state.storeLocation);
            return state;
        }

        DWORD OptionsFrom(const ScriptState& script)
        {
            DWORD options = 0;
            options |= script.ifeo ? DWORD{ OptionRedirectCalc } : 0;
            options |= script.appKey ? DWORD{ OptionCalculatorKey } : 0;
            options |= script.shortcut ? DWORD{ OptionStartMenu } : 0;
            // The script registers the calculator: handler on every install,
            // whether or not it removed the Store app, so only a recorded
            // removal means the Store app has anything to come back from.
            options |= !script.storeLocation.empty() ? DWORD{ OptionReplaceStore } : 0;
            return options;
        }

        void RegisterProtocol(const std::wstring& target)
        {
            WriteString(HKEY_CURRENT_USER, kProtocolKey, L"", L"URL:Calculator Protocol");
            WriteString(HKEY_CURRENT_USER, kProtocolKey, L"URL Protocol", L"");
            WriteString(HKEY_CURRENT_USER, kProtocolCommandKey, L"", L"\"" + target + L"\"");
        }

        void UnregisterProtocol(const std::wstring& target)
        {
            std::wstring command;
            if (ReadString(HKEY_CURRENT_USER, kProtocolCommandKey, L"", command) && IsOurs(command, target))
            {
                RegDeleteTreeW(HKEY_CURRENT_USER, kProtocolKey);
            }
        }

        // Moves a script install into Setup's own record. The script's saved
        // originals are not carried over: every undo here checks whether a
        // value is still ours before touching it, so the script's own values
        // are removed and anything else is left alone either way.
        void AdoptScriptInstall(const std::wstring& dir)
        {
            const ScriptState script = ReadScriptState(dir);
            if (!script.present)
            {
                return;
            }
            WriteString(HKEY_CURRENT_USER, kStateKey, L"InstallDir", dir);
            WriteString(HKEY_CURRENT_USER, kStateKey, L"Version", Version());
            WriteDword(HKEY_CURRENT_USER, kStateKey, L"Options", OptionsFrom(script));
            if (!script.storeLocation.empty())
            {
                WriteString(HKEY_CURRENT_USER, kStateKey, L"StoreLocation", script.storeLocation);
            }
            else if (script.protocol)
            {
                // Setup only takes the calculator: protocol along with the Store
                // app's place, so a handler the script left on its own goes now.
                UnregisterProtocol(dir + L"\\Calculator.exe");
            }
            DeleteFileW(ScriptStatePath(dir).c_str());
        }

        // --------------------------------------------------------- steps

        StepResult Done()
        {
            return {};
        }

        StepResult Skipped(std::wstring note)
        {
            return { Outcome::Skipped, std::move(note) };
        }

        StepResult Failed(std::wstring note)
        {
            return { Outcome::Failed, std::move(note) };
        }

        void SaveApplied(const Plan& plan)
        {
            WriteDword(HKEY_CURRENT_USER, kStateKey, L"Options", plan.applied);
        }

        // The calculator travels LZMA-compressed (installer/Unpack.h), and
        // is only written once it has unpacked and passed its checksum.
        bool LoadPayload(const uint8_t*& data, size_t& size)
        {
            HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(kPayloadId), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
            HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
            data = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
            size = resource ? SizeofResource(nullptr, resource) : 0;
            return data != nullptr && size != 0;
        }

        bool WritePayload(const std::wstring& target)
        {
            const uint8_t* packed = nullptr;
            size_t packedSize = 0;
            if (!LoadPayload(packed, packedSize))
            {
                return false;
            }
            const size_t size = PayloadSize(packed, packedSize);
            uint8_t* data = size ? static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, size)) : nullptr;
            if (data == nullptr)
            {
                return false;
            }
            bool ok = Unpack(packed, packedSize, data);

            // Written beside the target and moved over it, so a failure part
            // way never leaves a truncated Calculator.exe that calc.exe
            // already points at.
            const std::wstring staging = target + L".new";
            HANDLE file = ok ? CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
                             : INVALID_HANDLE_VALUE;
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
                CloseHandle(file);
                ok = ok && MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                if (!ok)
                {
                    DeleteFileW(staging.c_str());
                }
            }
            else
            {
                ok = false;
            }
            HeapFree(GetProcessHeap(), 0, data);
            return ok;
        }

        StepResult CopyFiles(Plan& plan)
        {
            const int created = SHCreateDirectoryExW(nullptr, plan.installDir.c_str(), nullptr);
            if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && created != ERROR_FILE_EXISTS)
            {
                return Failed(L"Setup couldn't create " + plan.installDir + L".");
            }
            CloseRunning(plan.targetExe);
            if (!WritePayload(plan.targetExe))
            {
                return Failed(L"Setup couldn't write Calculator.exe. Close any open copy and try again.");
            }

            // A copy of Setup stays with the install; it is what Settings >
            // Apps runs to remove it.
            const std::wstring self = ModulePath();
            const std::wstring kept = plan.installDir + L"\\Setup.exe";
            if (!SamePath(self, kept))
            {
                CopyFileW(self.c_str(), kept.c_str(), FALSE);
            }

            WriteString(HKEY_CURRENT_USER, kStateKey, L"InstallDir", plan.installDir);
            WriteString(HKEY_CURRENT_USER, kStateKey, L"Version", Version());
            SaveApplied(plan);
            return Done();
        }

        // Runs "/ifeo <verb>" elevated, or in-process when already elevated.
        StepResult RunIfeo(Plan& plan, const wchar_t* verb)
        {
            DWORD code = 1;
            if (IsElevated())
            {
                code = static_cast<DWORD>(RunElevatedIfeo(verb, plan.targetExe));
            }
            else
            {
                const std::wstring self = ModulePath();
                const std::wstring parameters = std::wstring(L"/ifeo ") + verb + L" \"" + plan.targetExe + L"\"";
                SHELLEXECUTEINFOW info{ sizeof(info) };
                info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
                info.hwnd = plan.owner;
                info.lpVerb = L"runas";
                info.lpFile = self.c_str();
                info.lpParameters = parameters.c_str();
                info.nShow = SW_HIDE;
                if (!ShellExecuteExW(&info))
                {
                    if (GetLastError() == ERROR_CANCELLED)
                    {
                        return Skipped(L"Administrator permission was declined.");
                    }
                    return Failed(L"Windows couldn't start the part of Setup that needs administrator permission.");
                }
                if (info.hProcess != nullptr)
                {
                    WaitForSingleObject(info.hProcess, 60000);
                    GetExitCodeProcess(info.hProcess, &code);
                    CloseHandle(info.hProcess);
                }
            }
            if (code != 0)
            {
                return Failed(L"Windows wouldn't let Setup change how calc.exe opens.");
            }
            return Done();
        }

        StepResult ApplyRedirect(Plan& plan)
        {
            StepResult result = RunIfeo(plan, L"apply");
            if (result.outcome == Outcome::Done)
            {
                plan.applied |= OptionRedirectCalc;
                SaveApplied(plan);
            }
            else if (result.outcome == Outcome::Skipped)
            {
                result.note += L" calc.exe still opens the Windows calculator. Run Setup again to change that.";
            }
            return result;
        }

        StepResult RestoreRedirect(Plan& plan)
        {
            StepResult result = RunIfeo(plan, L"restore");
            if (result.outcome == Outcome::Done)
            {
                plan.applied &= ~OptionRedirectCalc;
                SaveApplied(plan);
            }
            else
            {
                // Still redirected, so this is a failure however it came about:
                // removing the files now would leave calc.exe opening nothing.
                result.outcome = Outcome::Failed;
                result.note += L" calc.exe still opens this calculator, so its files have been kept.";
            }
            return result;
        }

        StepResult ApplyCalculatorKey(Plan& plan)
        {
            DWORD recorded = 0;
            if (!ReadDword(HKEY_CURRENT_USER, kStateKey, L"AppKeyHadPrev", recorded))
            {
                std::wstring current;
                if (ReadString(HKEY_CURRENT_USER, kAppKey, L"ShellExecute", current) && !IsOurs(current, plan.targetExe))
                {
                    WriteString(HKEY_CURRENT_USER, kStateKey, L"AppKeyPrev", current);
                    WriteDword(HKEY_CURRENT_USER, kStateKey, L"AppKeyHadPrev", 1);
                }
                else
                {
                    WriteDword(HKEY_CURRENT_USER, kStateKey, L"AppKeyHadPrev", 0);
                }
            }
            if (!WriteString(HKEY_CURRENT_USER, kAppKey, L"ShellExecute", plan.targetExe))
            {
                return Failed(L"Windows wouldn't let Setup change the Calculator key.");
            }
            plan.applied |= OptionCalculatorKey;
            SaveApplied(plan);
            return Done();
        }

        StepResult RestoreCalculatorKey(Plan& plan)
        {
            std::wstring current;
            if (ReadString(HKEY_CURRENT_USER, kAppKey, L"ShellExecute", current) && IsOurs(current, plan.targetExe))
            {
                DWORD had = 0;
                std::wstring previous;
                if (ReadDword(HKEY_CURRENT_USER, kStateKey, L"AppKeyHadPrev", had) && had == 1
                    && ReadString(HKEY_CURRENT_USER, kStateKey, L"AppKeyPrev", previous))
                {
                    WriteString(HKEY_CURRENT_USER, kAppKey, L"ShellExecute", previous);
                }
                else
                {
                    DeleteValue(HKEY_CURRENT_USER, kAppKey, L"ShellExecute");
                    DeleteKeyIfEmpty(HKEY_CURRENT_USER, kAppKey);
                }
            }
            DeleteValue(HKEY_CURRENT_USER, kStateKey, L"AppKeyPrev");
            DeleteValue(HKEY_CURRENT_USER, kStateKey, L"AppKeyHadPrev");
            plan.applied &= ~OptionCalculatorKey;
            SaveApplied(plan);
            return Done();
        }

        std::wstring ShortcutPath()
        {
            return KnownFolder(kFolderPrograms) + L"\\Calculator.lnk";
        }

        StepResult ApplyStartMenu(Plan& plan)
        {
            IShellLinkW* link = nullptr;
            HRESULT hr = CoCreateInstance(kClsidShellLink, nullptr, CLSCTX_INPROC_SERVER, kIidShellLinkW, reinterpret_cast<void**>(&link));
            if (SUCCEEDED(hr))
            {
                link->SetPath(plan.targetExe.c_str());
                link->SetWorkingDirectory(plan.installDir.c_str());
                link->SetDescription(L"Calculator");
                link->SetIconLocation(plan.targetExe.c_str(), 0);
                IPersistFile* file = nullptr;
                hr = link->QueryInterface(kIidPersistFile, reinterpret_cast<void**>(&file));
                if (SUCCEEDED(hr))
                {
                    hr = file->Save(ShortcutPath().c_str(), TRUE);
                    file->Release();
                }
                link->Release();
            }
            if (FAILED(hr))
            {
                return Failed(L"Setup couldn't add the Start menu entry.");
            }
            SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, ShortcutPath().c_str(), nullptr);
            plan.applied |= OptionStartMenu;
            SaveApplied(plan);
            return Done();
        }

        StepResult RestoreStartMenu(Plan& plan)
        {
            const std::wstring path = ShortcutPath();
            if (FileExists(path))
            {
                DeleteFileW(path.c_str());
                SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, path.c_str(), nullptr);
            }
            plan.applied &= ~OptionStartMenu;
            SaveApplied(plan);
            return Done();
        }

        StepResult ApplyReplaceStore(Plan& plan)
        {
            std::wstring recorded;
            if (!ReadString(HKEY_CURRENT_USER, kStateKey, L"StoreLocation", recorded))
            {
                // Exit 3: not installed for this user. Otherwise the location is
                // printed only once removal has succeeded, as UTF-16 in base64 so
                // no code page can mangle a path on the way back.
                const std::wstring script =
                    L"$ErrorActionPreference = 'Stop';"
                    L"$p = Get-AppxPackage -Name Microsoft.WindowsCalculator | Select-Object -First 1;"
                    L"if (-not $p) { exit 3 };"
                    L"Remove-AppxPackage -Package $p.PackageFullName;"
                    L"[Console]::Out.Write([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($p.InstallLocation)));"
                    L"exit 0";
                DWORD code = 1;
                std::string output;
                if (!RunPowerShell(script, code, &output))
                {
                    return Failed(L"Setup couldn't reach Windows PowerShell to remove the Store Calculator.");
                }
                if (code == 0)
                {
                    const std::wstring location = FromBase64Utf16(output);
                    if (!location.empty())
                    {
                        WriteString(HKEY_CURRENT_USER, kStateKey, L"StoreLocation", location);
                    }
                }
                else if (code != 3)
                {
                    return Failed(L"Windows wouldn't remove the Store Calculator, so it still owns the Start tile.");
                }
            }
            RegisterProtocol(plan.targetExe);
            plan.applied |= OptionReplaceStore;
            SaveApplied(plan);
            return Done();
        }

        StepResult RestoreStore(Plan& plan)
        {
            UnregisterProtocol(plan.targetExe);
            StepResult result = Done();

            std::wstring location;
            if (ReadString(HKEY_CURRENT_USER, kStateKey, L"StoreLocation", location) && !location.empty())
            {
                const std::wstring manifest = location + L"\\AppxManifest.xml";
                if (!FileExists(manifest))
                {
                    result = Failed(L"The Store Calculator's files are gone. Reinstall Windows Calculator from the Microsoft Store.");
                }
                else
                {
                    // Exit 4: something already brought it back, usually Windows Update.
                    const std::wstring script =
                        L"$ErrorActionPreference = 'Stop';"
                        L"if (Get-AppxPackage -Name Microsoft.WindowsCalculator) { exit 4 };"
                        L"Add-AppxPackage -Register " + PowerShellQuote(manifest) + L" -DisableDevelopmentMode;"
                        L"exit 0";
                    DWORD code = 1;
                    if (!RunPowerShell(script, code, nullptr) || (code != 0 && code != 4))
                    {
                        result = Failed(L"Windows wouldn't bring the Store Calculator back. Reinstall Windows Calculator from the Microsoft Store.");
                    }
                }
            }
            DeleteValue(HKEY_CURRENT_USER, kStateKey, L"StoreLocation");
            plan.applied &= ~OptionReplaceStore;
            SaveApplied(plan);
            return result;
        }

        unsigned long long FileBytes(const std::wstring& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            {
                return 0;
            }
            return (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }

        StepResult RegisterApp(Plan& plan)
        {
            const std::wstring setupExe = plan.installDir + L"\\Setup.exe";
            const std::wstring key = kUninstallKey;
            const DWORD sizeKb = static_cast<DWORD>((FileBytes(plan.targetExe) + FileBytes(setupExe) + 1023) / 1024);
            bool ok = true;
            ok &= WriteString(HKEY_CURRENT_USER, key, L"DisplayName", L"Calculator (compact build)");
            ok &= WriteString(HKEY_CURRENT_USER, key, L"DisplayVersion", Version());
            ok &= WriteString(HKEY_CURRENT_USER, key, L"DisplayIcon", plan.targetExe + L",0");
            ok &= WriteString(HKEY_CURRENT_USER, key, L"InstallLocation", plan.installDir);
            ok &= WriteString(HKEY_CURRENT_USER, key, L"UninstallString", L"\"" + setupExe + L"\" /uninstall");
            ok &= WriteString(HKEY_CURRENT_USER, key, L"ModifyPath", L"\"" + setupExe + L"\"");
            ok &= WriteDword(HKEY_CURRENT_USER, key, L"EstimatedSize", sizeKb);
            ok &= WriteDword(HKEY_CURRENT_USER, key, L"NoRepair", 1);
            if (!ok)
            {
                return Failed(L"Calculator works, but it won't be listed in Settings > Apps.");
            }
            return Done();
        }

        StepResult RemoveFiles(Plan& plan)
        {
            if (plan.applied & OptionRedirectCalc)
            {
                return Skipped(L"Kept, because calc.exe still points at them.");
            }

            CloseRunning(plan.targetExe);
            bool ok = true;
            if (FileExists(plan.targetExe) && !DeleteFileW(plan.targetExe.c_str()))
            {
                ok = false;
            }
            DeleteFileW((plan.targetExe + L".new").c_str());
            DeleteFileW(ScriptStatePath(plan.installDir).c_str());

            const std::wstring setupExe = plan.installDir + L"\\Setup.exe";
            if (SamePath(ModulePath(), setupExe))
            {
                plan.removeSelfOnExit = true;
            }
            else
            {
                DeleteFileW(setupExe.c_str());
                RemoveDirectoryW(plan.installDir.c_str());
            }

            RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
            RegDeleteTreeW(HKEY_CURRENT_USER, kStateKey);
            plan.complete = ok;
            if (!ok)
            {
                return Failed(L"Calculator.exe couldn't be deleted. It may still be open; you can delete it from " + plan.installDir + L".");
            }
            return Done();
        }
    }

    // --------------------------------------------------------------- public

    std::wstring DefaultInstallDir()
    {
        return KnownFolder(kFolderLocalAppData) + L"\\Programs\\CompactCalculator";
    }

    Installed QueryInstalled()
    {
        Installed info;
        std::wstring dir;
        if (ReadString(HKEY_CURRENT_USER, kStateKey, L"InstallDir", dir) && !dir.empty())
        {
            info.present = true;
            info.dir = dir;
            ReadDword(HKEY_CURRENT_USER, kStateKey, L"Options", info.options);
            ReadString(HKEY_CURRENT_USER, kStateKey, L"Version", info.version);
            return info;
        }
        const std::wstring fallback = DefaultInstallDir();
        const ScriptState script = ReadScriptState(fallback);
        if (script.present)
        {
            info.present = true;
            info.byScript = true;
            info.dir = fallback;
            info.options = OptionsFrom(script);
        }
        return info;
    }

    unsigned long long PayloadBytes()
    {
        // The size it will have once installed, not the size it travels at.
        const uint8_t* packed = nullptr;
        size_t packedSize = 0;
        return LoadPayload(packed, packedSize) ? PayloadSize(packed, packedSize) : 0;
    }

    std::unique_ptr<Plan> PlanInstall(DWORD chosen, HWND owner)
    {
        const Installed previous = QueryInstalled();
        if (previous.byScript)
        {
            AdoptScriptInstall(previous.dir);
        }

        auto plan = std::make_unique<Plan>();
        Plan* p = plan.get();
        p->owner = owner;
        p->installDir = previous.present ? previous.dir : DefaultInstallDir();
        p->targetExe = p->installDir + L"\\Calculator.exe";
        p->applied = previous.present ? previous.options : 0;

        auto& steps = p->steps;
        steps.push_back({ L"Copying Calculator", CopyFiles });

        // Each option is either applied, or -- when it was on before and has
        // now been turned off -- undone, so changing options is a re-run.
        struct Choice
        {
            DWORD bit;
            const wchar_t* applyTitle;
            const wchar_t* restoreTitle;
            StepResult (*apply)(Plan&);
            StepResult (*restore)(Plan&);
        };
        static constexpr Choice choices[] = {
            { OptionRedirectCalc, L"Opening this calculator from calc.exe", L"Giving calc.exe back to Windows", ApplyRedirect, RestoreRedirect },
            { OptionCalculatorKey, L"Setting up the Calculator key", L"Restoring the Calculator key", ApplyCalculatorKey, RestoreCalculatorKey },
            { OptionStartMenu, L"Adding Calculator to the Start menu", L"Removing the Start menu entry", ApplyStartMenu, RestoreStartMenu },
            { OptionReplaceStore, L"Replacing the Store Calculator", L"Bringing back the Store Calculator", ApplyReplaceStore, RestoreStore },
        };
        for (const Choice& choice : choices)
        {
            if (chosen & choice.bit)
            {
                steps.push_back({ choice.applyTitle, choice.apply });
            }
            else if (p->applied & choice.bit)
            {
                steps.push_back({ choice.restoreTitle, choice.restore });
            }
        }

        steps.push_back({ L"Adding Calculator to Settings > Apps", RegisterApp });
        return plan;
    }

    std::unique_ptr<Plan> PlanUninstall(HWND owner)
    {
        const Installed previous = QueryInstalled();
        if (previous.byScript)
        {
            AdoptScriptInstall(previous.dir);
        }

        auto plan = std::make_unique<Plan>();
        Plan* p = plan.get();
        p->owner = owner;
        p->removing = true;
        p->installDir = previous.present ? previous.dir : DefaultInstallDir();
        p->targetExe = p->installDir + L"\\Calculator.exe";
        p->applied = previous.options;

        auto& steps = p->steps;
        if (p->applied & OptionRedirectCalc)
        {
            steps.push_back({ L"Giving calc.exe back to Windows", RestoreRedirect });
        }
        if (p->applied & OptionCalculatorKey)
        {
            steps.push_back({ L"Restoring the Calculator key", RestoreCalculatorKey });
        }
        if (p->applied & OptionStartMenu)
        {
            steps.push_back({ L"Removing the Start menu entry", RestoreStartMenu });
        }
        if (p->applied & OptionReplaceStore)
        {
            steps.push_back({ L"Bringing back the Store Calculator", RestoreStore });
        }
        steps.push_back({ L"Removing Calculator's files", RemoveFiles });
        return plan;
    }

    void ScheduleDeletion(const std::wstring& file, const std::wstring& directory)
    {
        // A running image cannot delete itself, so a hidden cmd.exe waits for
        // this process to go and then removes the file, and the directory if
        // there is one and it is now empty.
        std::wstring command = L"\"" + SystemDirectory() + L"\\cmd.exe\" /d /s /c \""
            L"for /l %i in (1,1,30) do (if exist \"" + file + L"\" (del /f /q \"" + file + L"\" >nul 2>&1 & ping -n 2 127.0.0.1 >nul))";
        if (!directory.empty())
        {
            command += L" & rmdir \"" + directory + L"\" >nul 2>&1";
        }
        command += L"\"";
        const std::wstring workingDirectory = SystemDirectory();
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup, &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }

    void ScheduleRemoval(const Plan& plan)
    {
        if (plan.removeSelfOnExit)
        {
            ScheduleDeletion(plan.installDir + L"\\Setup.exe", plan.installDir);
        }
    }

    bool Launch(const std::wstring& exe, const wchar_t* parameters)
    {
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), parameters, nullptr, SW_SHOWNORMAL)) > 32;
    }

    std::wstring ThisExe()
    {
        return ModulePath();
    }

    int RunElevatedIfeo(const std::wstring& verb, const std::wstring& target)
    {
        const bool apply = (verb == L"apply");
        if (!apply && verb != L"restore")
        {
            return 2;
        }
        if (apply && !FileExists(target))
        {
            return 3; // never point calc.exe at something that is not there
        }

        const std::wstring debugger = L"\"" + target + L"\"";
        bool ok = true;
        for (const wchar_t* image : kImages)
        {
            const std::wstring key = std::wstring(kIfeoRoot) + image;
            for (const View& view : kViews)
            {
                const std::wstring had = std::wstring(image) + L"|" + view.tag + L".HadPrev";
                const std::wstring prev = std::wstring(image) + L"|" + view.tag + L".Prev";
                std::wstring current;
                const bool present = ReadString(HKEY_LOCAL_MACHINE, key, L"Debugger", current, view.sam);

                if (apply)
                {
                    DWORD recorded = 0;
                    if (!ReadDword(HKEY_LOCAL_MACHINE, kIfeoBackupKey, had.c_str(), recorded))
                    {
                        if (present && !IsOurs(current, target))
                        {
                            WriteString(HKEY_LOCAL_MACHINE, kIfeoBackupKey, prev.c_str(), current);
                            WriteDword(HKEY_LOCAL_MACHINE, kIfeoBackupKey, had.c_str(), 1);
                        }
                        else
                        {
                            WriteDword(HKEY_LOCAL_MACHINE, kIfeoBackupKey, had.c_str(), 0);
                        }
                    }
                    ok &= WriteString(HKEY_LOCAL_MACHINE, key, L"Debugger", debugger, view.sam);
                    continue;
                }

                if (present && IsOurs(current, target))
                {
                    DWORD wasSet = 0;
                    std::wstring original;
                    if (ReadDword(HKEY_LOCAL_MACHINE, kIfeoBackupKey, had.c_str(), wasSet) && wasSet == 1
                        && ReadString(HKEY_LOCAL_MACHINE, kIfeoBackupKey, prev.c_str(), original))
                    {
                        ok &= WriteString(HKEY_LOCAL_MACHINE, key, L"Debugger", original, view.sam);
                    }
                    else
                    {
                        DeleteValue(HKEY_LOCAL_MACHINE, key, L"Debugger", view.sam);
                        DeleteKeyIfEmpty(HKEY_LOCAL_MACHINE, key, view.sam);
                        std::wstring check;
                        ok &= !ReadString(HKEY_LOCAL_MACHINE, key, L"Debugger", check, view.sam);
                    }
                }
                DeleteValue(HKEY_LOCAL_MACHINE, kIfeoBackupKey, had.c_str());
                DeleteValue(HKEY_LOCAL_MACHINE, kIfeoBackupKey, prev.c_str());
            }
        }
        if (!apply)
        {
            DeleteKeyIfEmpty(HKEY_LOCAL_MACHINE, kIfeoBackupKey);
        }
        return ok ? 0 : 1;
    }
}
