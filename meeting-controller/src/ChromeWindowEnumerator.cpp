#include "ChromeWindowEnumerator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
struct ProcessUnicodeString
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

using NtQueryInformationProcessFn = LONG(WINAPI*)(
    HANDLE process,
    ULONG informationClass,
    void* information,
    ULONG informationLength,
    ULONG* returnLength);

struct EnumContext
{
    QJsonArray windows;
    QHash<DWORD, QString> commandLines;
    QHash<QString, QJsonObject> profileInfoCache;
};

QString readWindowTitle(HWND hwnd)
{
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0)
        return {};

    std::vector<wchar_t> buffer(static_cast<std::size_t>(length) + 1);

    const int copied = GetWindowTextW(
        hwnd,
        buffer.data(),
        static_cast<int>(buffer.size()));

    if (copied <= 0)
        return {};

    return QString::fromWCharArray(buffer.data(), copied);
}

QString readWindowClass(HWND hwnd)
{
    wchar_t className[256]{};
    const int copied = GetClassNameW(hwnd, className, 255);

    if (copied <= 0)
        return {};

    return QString::fromWCharArray(className, copied);
}

QString readProcessImagePath(DWORD processId)
{
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        processId);

    if (!process)
        return {};

    wchar_t path[32768]{};
    DWORD size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));

    const BOOL ok = QueryFullProcessImageNameW(
        process,
        0,
        path,
        &size);

    CloseHandle(process);

    if (!ok)
        return {};

    return QString::fromWCharArray(path, static_cast<qsizetype>(size));
}

// Chromium, Edge, Brave and other forks also ship chrome.exe or a window class
// named Chrome_WidgetWin_1. Only Google Chrome's own install is accepted.
bool isGoogleChrome(const QString& imagePath)
{
    const QString normalized = QDir::fromNativeSeparators(imagePath).toLower();
    return normalized.endsWith("/chrome.exe")
        && normalized.contains("/google/chrome")
        && !normalized.contains("/chromium/");
}

QString readProcessCommandLine(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return {};

    const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));

    QString commandLine;
    if (query)
    {
        // ProcessCommandLineInformation. The kernel copies the text into the
        // buffer immediately after the UNICODE_STRING header.
        constexpr ULONG kProcessCommandLineInformation = 60;
        ULONG needed = 0;
        query(process, kProcessCommandLineInformation, nullptr, 0, &needed);

        if (needed > 0)
        {
            std::vector<unsigned char> buffer(needed);
            if (query(process,
                      kProcessCommandLineInformation,
                      buffer.data(),
                      static_cast<ULONG>(buffer.size()),
                      &needed) >= 0
                && buffer.size() >= sizeof(ProcessUnicodeString))
            {
                const auto* text = reinterpret_cast<const ProcessUnicodeString*>(buffer.data());
                const auto* start = buffer.data();
                const auto* end = buffer.data() + buffer.size();
                const wchar_t* chars = nullptr;

                const auto* pointed = reinterpret_cast<const unsigned char*>(text->Buffer);
                if (pointed >= start && pointed + text->Length <= end)
                    chars = reinterpret_cast<const wchar_t*>(pointed);
                else if (buffer.size() >= sizeof(ProcessUnicodeString) + text->Length)
                    chars = reinterpret_cast<const wchar_t*>(buffer.data() + sizeof(ProcessUnicodeString));

                if (chars && text->Length > 0)
                    commandLine = QString::fromWCharArray(chars, text->Length / sizeof(wchar_t));
            }
        }
    }

    CloseHandle(process);
    return commandLine;
}

QString commandLineOption(const QString& commandLine, const QString& name)
{
    const QRegularExpression pattern(
        QString("--%1=(?:\"([^\"]*)\"|(\\S+))").arg(QRegularExpression::escape(name)));
    const QRegularExpressionMatch match = pattern.match(commandLine);
    if (!match.hasMatch())
        return {};

    return match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
}

QString defaultUserDataDir(const QString& imagePath)
{
    const QString normalized = QDir::fromNativeSeparators(imagePath);
    const int google = normalized.indexOf("/Google/", 0, Qt::CaseInsensitive);
    const int application = normalized.indexOf("/Application/", google, Qt::CaseInsensitive);
    if (google < 0 || application < 0)
        return {};

    const QString channel = normalized.mid(google + 8, application - (google + 8));
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.isEmpty())
        return {};

    return QDir(localAppData).filePath("Google/" + channel + "/User Data");
}

QString profileDisplayName(
    EnumContext* context,
    const QString& userDataDir,
    const QString& profileDirectory)
{
    if (!context->profileInfoCache.contains(userDataDir))
    {
        QJsonObject infoCache;
        QFile file(QDir(userDataDir).filePath("Local State"));
        if (file.open(QIODevice::ReadOnly))
        {
            const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            infoCache = root.value("profile").toObject().value("info_cache").toObject();
        }
        context->profileInfoCache.insert(userDataDir, infoCache);
    }

    const QJsonObject entry =
        context->profileInfoCache.value(userDataDir).value(profileDirectory).toObject();

    QString name = entry.value("name").toString().trimmed();
    if (name.isEmpty())
        name = profileDirectory;

    const QString account = entry.value("user_name").toString().trimmed();
    if (!account.isEmpty())
        name += "  ·  " + account;

    return name;
}

BOOL CALLBACK enumWindowsCallback(HWND hwnd, LPARAM parameter)
{
    auto* context = reinterpret_cast<EnumContext*>(parameter);

    if (!IsWindowVisible(hwnd))
        return TRUE;

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    if (processId == 0)
        return TRUE;

    const QString imagePath = readProcessImagePath(processId);
    if (!isGoogleChrome(imagePath))
        return TRUE;

    if (!context->commandLines.contains(processId))
        context->commandLines.insert(processId, readProcessCommandLine(processId));

    const QString commandLine = context->commandLines.value(processId);
    // Renderer, GPU and utility processes are not browser windows.
    if (commandLine.contains("--type="))
        return TRUE;

    QString profileDirectory = commandLineOption(commandLine, "profile-directory");
    if (profileDirectory.isEmpty())
        profileDirectory = "Default";

    QString userDataDir = commandLineOption(commandLine, "user-data-dir");
    if (userDataDir.isEmpty())
        userDataDir = defaultUserDataDir(imagePath);
    userDataDir = QDir::fromNativeSeparators(QDir::cleanPath(userDataDir));

    const QString profileName = profileDisplayName(context, userDataDir, profileDirectory);

    QJsonObject item;
    item.insert("title", readWindowTitle(hwnd));
    item.insert("processId", static_cast<qint64>(processId));
    item.insert("hwnd", ChromeWindowEnumerator::windowHandleToString(
        reinterpret_cast<quintptr>(hwnd)));
    item.insert("className", readWindowClass(hwnd));
    item.insert("imagePath", imagePath);
    item.insert("profileName", profileName);
    item.insert("profileKey", userDataDir + "/" + profileDirectory);

    context->windows.append(item);
    return TRUE;
}
}

QJsonArray ChromeWindowEnumerator::enumerateVisibleChromeWindows()
{
    EnumContext context;
    EnumWindows(enumWindowsCallback, reinterpret_cast<LPARAM>(&context));
    return context.windows;
}

QString ChromeWindowEnumerator::windowHandleToString(quintptr hwnd)
{
    return QString::number(static_cast<qulonglong>(hwnd), 16).toUpper();
}
