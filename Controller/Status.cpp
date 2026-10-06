#include "ControllerInternal.h"

#include <wchar.h>
#include <stdio.h>

namespace ReflexProbeController {

void SetChildFont(HWND child, HFONT font)
{
    if (child)
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void SetStatusCaretToEnd()
{
    if (!g_app.status)
        return;

    const int textLength = GetWindowTextLengthW(g_app.status);
    SendMessageW(g_app.status, EM_SETSEL,
        static_cast<WPARAM>(textLength), static_cast<LPARAM>(textLength));
}

void AppendStatus(const wchar_t* text)
{
    if (!g_app.status || !text)
        return;

    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    const bool preserveSelection = GetFocus() == g_app.status;
    if (preserveSelection) {
        SendMessageW(g_app.status, EM_GETSEL,
            reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
    }

    SetStatusCaretToEnd();
    SendMessageW(g_app.status, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text));

    if (preserveSelection) {
        SendMessageW(g_app.status, EM_SETSEL,
            static_cast<WPARAM>(selectionStart), static_cast<LPARAM>(selectionEnd));
    } else {
        SetStatusCaretToEnd();
        SendMessageW(g_app.status, EM_SCROLLCARET, 0, 0);
    }
}

void AppendStatusCount(const wchar_t* text, size_t count)
{
    if (!text || !count)
        return;

    wchar_t chunk[1024]{};
    while (count) {
        const size_t copyCount = count < (_countof(chunk) - 1) ? count : (_countof(chunk) - 1);
        wmemcpy(chunk, text, copyCount);
        chunk[copyCount] = 0;
        AppendStatus(chunk);
        text += copyCount;
        count -= copyCount;
    }
}

void FormatTimestamp(LONGLONG eventQpc, wchar_t* out, size_t outCount)
{
    if (!out || !outCount)
        return;

    SYSTEMTIME localTime{};
    bool converted = false;

    if (eventQpc > 0) {
        LARGE_INTEGER nowQpc{};
        LARGE_INTEGER frequency{};
        FILETIME nowUtc{};
        if (QueryPerformanceCounter(&nowQpc) && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
            GetSystemTimeAsFileTime(&nowUtc);

            ULARGE_INTEGER eventUtc{};
            eventUtc.LowPart = nowUtc.dwLowDateTime;
            eventUtc.HighPart = nowUtc.dwHighDateTime;

            if (eventQpc < nowQpc.QuadPart) {
                const LONGLONG deltaQpc = nowQpc.QuadPart - eventQpc;
                const double delta100nsDouble =
                    (static_cast<double>(deltaQpc) * 10000000.0) /
                    static_cast<double>(frequency.QuadPart);
                const ULONGLONG delta100ns = static_cast<ULONGLONG>(delta100nsDouble);
                if (delta100ns < eventUtc.QuadPart)
                    eventUtc.QuadPart -= delta100ns;
            }

            FILETIME eventUtcFile{};
            eventUtcFile.dwLowDateTime = eventUtc.LowPart;
            eventUtcFile.dwHighDateTime = eventUtc.HighPart;
            FILETIME eventLocalFile{};
            if (FileTimeToLocalFileTime(&eventUtcFile, &eventLocalFile) &&
                FileTimeToSystemTime(&eventLocalFile, &localTime)) {
                converted = true;
            }
        }
    }

    if (!converted)
        GetLocalTime(&localTime);

    swprintf_s(out, outCount, L"[%02u:%02u:%02u] ",
        static_cast<unsigned int>(localTime.wHour),
        static_cast<unsigned int>(localTime.wMinute),
        static_cast<unsigned int>(localTime.wSecond));
}

bool EnsureTextBufferCapacity(TextBuffer& buffer, size_t requiredChars)
{
    if (requiredChars <= buffer.capacity)
        return true;

    size_t newCapacity = buffer.capacity ? buffer.capacity : kInitialTextBufferCapacity;
    while (newCapacity < requiredChars) {
        if (newCapacity > static_cast<size_t>(-1) / 2)
            return false;
        newCapacity *= 2;
    }

    if (newCapacity > static_cast<size_t>(-1) / sizeof(wchar_t))
        return false;

    const SIZE_T bytes = static_cast<SIZE_T>(newCapacity * sizeof(wchar_t));
    void* memory = buffer.data
        ? HeapReAlloc(GetProcessHeap(), 0, buffer.data, bytes)
        : HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!memory)
        return false;

    buffer.data = static_cast<wchar_t*>(memory);
    buffer.capacity = newCapacity;
    if (!buffer.length)
        buffer.data[0] = 0;
    return true;
}

void FreeTextBuffer(TextBuffer& buffer)
{
    if (buffer.data)
        HeapFree(GetProcessHeap(), 0, buffer.data);
    buffer = {};
}

bool AppendTextBufferCount(TextBuffer& buffer, const wchar_t* text, size_t count)
{
    if (!text || !count)
        return true;
    if (buffer.length > static_cast<size_t>(-1) - count - 1)
        return false;

    const size_t requiredChars = buffer.length + count + 1;
    if (!EnsureTextBufferCapacity(buffer, requiredChars))
        return false;

    wmemcpy(buffer.data + buffer.length, text, count);
    buffer.length += count;
    buffer.data[buffer.length] = 0;
    return true;
}

bool AppendTextBuffer(TextBuffer& buffer, const wchar_t* text)
{
    return !text || AppendTextBufferCount(buffer, text, wcslen(text));
}

bool AppendTextBufferLineAtQpc(TextBuffer& buffer, const wchar_t* text, LONGLONG eventQpc)
{
    if (!text)
        return true;

    const wchar_t* cursor = text;
    for (;;) {
        const wchar_t* newline = wcsstr(cursor, L"\r\n");
        const size_t lineLength = newline ? static_cast<size_t>(newline - cursor) : wcslen(cursor);

        wchar_t timestamp[32]{};
        FormatTimestamp(eventQpc, timestamp, _countof(timestamp));
        if (!AppendTextBuffer(buffer, timestamp) ||
            !AppendTextBufferCount(buffer, cursor, lineLength) ||
            !AppendTextBuffer(buffer, L"\r\n")) {
            return false;
        }

        if (!newline)
            break;
        cursor = newline + 2;
    }

    return true;
}

void AppendDisplayLineAtQpc(const wchar_t* text, LONGLONG eventQpc)
{
    if (!text)
        return;

    const wchar_t* cursor = text;
    for (;;) {
        const wchar_t* newline = wcsstr(cursor, L"\r\n");
        const size_t lineLength = newline ? static_cast<size_t>(newline - cursor) : wcslen(cursor);

        wchar_t timestamp[32]{};
        FormatTimestamp(eventQpc, timestamp, _countof(timestamp));
        AppendStatus(timestamp);
        AppendStatusCount(cursor, lineLength);
        AppendStatus(L"\r\n");

        if (!newline)
            break;
        cursor = newline + 2;
    }
}

void AppendDisplayLine(const wchar_t* text)
{
    AppendDisplayLineAtQpc(text, 0);
}

void AppendStatusLineAtQpc(const wchar_t* text, LONGLONG eventQpc)
{
    AppendDisplayLineAtQpc(text, eventQpc);
}

void AppendStatusLine(const wchar_t* text)
{
    AppendStatusLineAtQpc(text, 0);
}

LRESULT CALLBACK StatusEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_KEYDOWN && wParam == static_cast<WPARAM>('A') &&
        (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        SendMessageW(window, EM_SETSEL, 0, static_cast<LPARAM>(-1));
        return 0;
    }

    if (g_app.statusOriginalProc)
        return CallWindowProcW(g_app.statusOriginalProc, window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

void RecreateStatusControl(bool wordWrap)
{
    if (!g_app.window)
        return;

    wchar_t* savedText = nullptr;
    int savedLength = 0;
    DWORD selectionStart = 0;
    DWORD selectionEnd = 0;
    int firstVisibleLine = 0;

    if (g_app.status) {
        savedLength = GetWindowTextLengthW(g_app.status);
        if (savedLength > 0) {
            const SIZE_T bytes = (static_cast<SIZE_T>(savedLength) + 1) * sizeof(wchar_t);
            savedText = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, bytes));
            if (savedText)
                GetWindowTextW(g_app.status, savedText, savedLength + 1);
        }

        SendMessageW(g_app.status, EM_GETSEL,
            reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
        firstVisibleLine = static_cast<int>(SendMessageW(g_app.status, EM_GETFIRSTVISIBLELINE, 0, 0));

        if (g_app.statusOriginalProc) {
            SetWindowLongPtrW(g_app.status, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_app.statusOriginalProc));
            g_app.statusOriginalProc = nullptr;
        }
        DestroyWindow(g_app.status);
        g_app.status = nullptr;
    }

    DWORD style = WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
        ES_READONLY | ES_NOHIDESEL | WS_VSCROLL;
    if (!wordWrap)
        style |= WS_HSCROLL | ES_AUTOHSCROLL;

    g_app.status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        style, 0, 0, 0, 0, g_app.window, reinterpret_cast<HMENU>(IDC_STATUS), g_app.instance, nullptr);
    if (!g_app.status) {
        if (savedText)
            HeapFree(GetProcessHeap(), 0, savedText);
        return;
    }

    SetChildFont(g_app.status, static_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT)));
    SendMessageW(g_app.status, EM_SETLIMITTEXT, kStatusTextLimit, 0);
    g_app.statusOriginalProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_app.status, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&StatusEditProc)));

    if (savedText) {
        SetWindowTextW(g_app.status, savedText);
        SendMessageW(g_app.status, EM_SETSEL,
            static_cast<WPARAM>(selectionStart), static_cast<LPARAM>(selectionEnd));
        const int currentFirstLine = static_cast<int>(SendMessageW(g_app.status, EM_GETFIRSTVISIBLELINE, 0, 0));
        SendMessageW(g_app.status, EM_LINESCROLL, 0,
            static_cast<LPARAM>(firstVisibleLine - currentFirstLine));
        HeapFree(GetProcessHeap(), 0, savedText);
    }

    RECT client{};
    GetClientRect(g_app.window, &client);
    LayoutControls(client.right - client.left, client.bottom - client.top);
}

} // namespace ReflexProbeController
