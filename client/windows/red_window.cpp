/*
   Copyright (C) 2009 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "red_window.h"
#include "pixels_source_p.h"
#include "utils.h"
#include "debug.h"
#include "red.h"
#include "menu.h"
#include "win_platform.h"
#include "platform_utils.h"

#include <list>

#define NATIVE_CAPTION_STYLE (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

extern HINSTANCE instance;

static ATOM class_atom = 0;
static const LPCWSTR win_class_name = L"redc_wclass";
static HWND focus_window = NULL;
static HHOOK low_keyboard_hook = NULL;
static HHOOK msg_filter_hook = NULL;
typedef std::list<RedKey> KeysList;
static KeysList filtered_up_keys;

static LRESULT CALLBACK MessageFilterProc(int nCode, WPARAM wParam, LPARAM lParam);

static inline int to_red_mouse_state(WPARAM wParam)
{
    return ((wParam & MK_LBUTTON) ? REDC_LBUTTON_MASK : 0) |
           ((wParam & MK_MBUTTON) ? REDC_MBUTTON_MASK : 0) |
           ((wParam & MK_RBUTTON) ? REDC_RBUTTON_MASK : 0);
}

static inline RedKey translate_key(int virtual_key, uint32_t scan, bool escape)
{
    if (scan == 0) {
        return REDKEY_INVALID;
    }
    switch (virtual_key) {
    case VK_PAUSE:
        return REDKEY_PAUSE;
    case VK_SNAPSHOT:
        return REDKEY_CTRL_PRINT_SCREEN;
    case VK_NUMLOCK:
        return REDKEY_NUM_LOCK;
    case VK_HANGUL:
        return REDKEY_KOREAN_HANGUL;
    case VK_HANJA:
        return REDKEY_KOREAN_HANGUL_HANJA;
    case VK_PROCESSKEY:
        if (scan == 0xf1) {
            return REDKEY_INVALID; // prevent double key (VK_PROCESSKEY + VK_HANJA)
        } else if (scan == 0xf2) {
            return REDKEY_KOREAN_HANGUL;
        }
    default:
        //todo: always use vitrtual key
        if (escape) {
            scan += REDKEY_ESCAPE_BASE;
        }
        return (RedKey)scan;
    }
}

static int menu_cmd_to_app(WPARAM wparam)
{
    return 0;
}

static inline void send_filtered_keys(RedWindow* window)
{
    KeysList::iterator iter;

    for (iter = filtered_up_keys.begin(); iter != filtered_up_keys.end(); iter++) {
        window->get_listener().on_key_release(*iter);
    }
    filtered_up_keys.clear();
}

LRESULT CALLBACK RedWindow_p::WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    RedWindow* window = (RedWindow*)GetWindowLong(hWnd, GWL_USERDATA);
    ASSERT(window);

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc;

        hdc = BeginPaint(hWnd, &ps);
        Point origin = window->get_origin();
        Rect r;
        r.left = ps.rcPaint.left - origin.x;
        r.top = ps.rcPaint.top - origin.y;
        r.right = ps.rcPaint.right - origin.x;
        r.bottom = ps.rcPaint.bottom - origin.y;
        window->get_listener().on_exposed_rect(r);
        EndPaint(hWnd, &ps);
        break;
    }
    case WM_MOUSEMOVE: {
        if (!window->_pointer_in_window) {
            window->on_pointer_enter();
        }
        Point origin = window->get_origin();
        window->get_listener().on_mouse_motion(LOWORD(lParam) - origin.x, HIWORD(lParam) - origin.y,
                                               to_red_mouse_state(wParam));
        break;
    }
    case WM_MOUSELEAVE:
        window->on_pointer_leave();
        break;
    case WM_SETFOCUS:
        window->on_focus_in();
        break;
    case WM_KILLFOCUS:
        window->on_focus_out();
        break;
    case WM_LBUTTONDOWN:
        window->get_listener().on_button_press(REDC_MOUSE_LBUTTON, to_red_mouse_state(wParam));
        break;
    case WM_LBUTTONUP:
        window->get_listener().on_button_release(REDC_MOUSE_LBUTTON, to_red_mouse_state(wParam));
        break;
    case WM_RBUTTONDOWN:
        window->get_listener().on_button_press(REDC_MOUSE_RBUTTON, to_red_mouse_state(wParam));
        break;
    case WM_RBUTTONUP:
        window->get_listener().on_button_release(REDC_MOUSE_RBUTTON, to_red_mouse_state(wParam));
        break;
    case WM_MBUTTONDOWN:
        window->get_listener().on_button_press(REDC_MOUSE_MBUTTON, to_red_mouse_state(wParam));
        break;
    case WM_MBUTTONUP:
        window->get_listener().on_button_release(REDC_MOUSE_MBUTTON, to_red_mouse_state(wParam));
        break;
    case WM_MOUSEWHEEL:
        if (HIWORD(wParam) & 0x8000) {
            window->get_listener().on_button_press(REDC_MOUSE_DBUTTON,
                                                   to_red_mouse_state(wParam));
            window->get_listener().on_button_release(REDC_MOUSE_DBUTTON,
                                                     to_red_mouse_state(wParam));
        } else {
            window->get_listener().on_button_press(REDC_MOUSE_UBUTTON,
                                                   to_red_mouse_state(wParam));
            window->get_listener().on_button_release(REDC_MOUSE_UBUTTON,
                                                     to_red_mouse_state(wParam));
        }
        break;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN: {
        RedKey key = translate_key(wParam, HIWORD(lParam) & 0xff, (lParam & (1 << 24)) != 0);
        window->get_listener().on_key_press(key);
        // Allow Windows to translate Alt-F4 to WM_CLOSE message.
        if (!window->_key_interception_on) {
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    }
    case WM_SYSKEYUP:
    case WM_KEYUP: {
        RedKey key = translate_key(wParam, HIWORD(lParam) & 0xff, (lParam & (1 << 24)) != 0);
        window->get_listener().on_key_release(key);
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = (MINMAXINFO*)lParam;
        info->ptMaxSize.x = window->_window_size.x;
        info->ptMaxSize.y = window->_window_size.y;
        info->ptMinTrackSize = info->ptMaxSize;
        info->ptMaxTrackSize = info->ptMaxSize;
        info->ptMaxPosition.x = info->ptMaxPosition.y = 0;
        break;
    }
    case WM_SYSCOMMAND:
        if (window->prossec_menu_commands(wParam & ~0x0f)) {
            break;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
        ASSERT(filtered_up_keys.empty());
        DBG(0, "enter modal");
        window->get_listener().enter_modal_loop();
        WinPlatform::enter_modal_loop();
        if (msg_filter_hook) {
            LOG_WARN("entering modal loop while filter hook is active");
            UnhookWindowsHookEx(msg_filter_hook);
        }
        msg_filter_hook = SetWindowsHookEx(WH_MSGFILTER, MessageFilterProc,
                                           GetModuleHandle(NULL), GetCurrentThreadId());
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
        DBG(0, "exit modal");
        window->get_listener().exit_modal_loop();
        WinPlatform::exit_modal_loop();
        UnhookWindowsHookEx(msg_filter_hook);
        msg_filter_hook = NULL;
        send_filtered_keys(window);
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_SETCURSOR:
        if (!window->_pointer_in_window) {
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            window->on_minimized();
        } else if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
            window->on_restored();
        }
        break;
    case WM_WINDOWPOSCHANGING:
        window->on_pos_changing(*window);
        return DefWindowProc(hWnd, message, wParam, lParam);
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

static ATOM register_class(HINSTANCE instance)
{
    WNDCLASSEX wclass;

    wclass.cbSize = sizeof(WNDCLASSEX);
    wclass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wclass.lpfnWndProc = DefWindowProc;
    wclass.cbClsExtra = 0;
    wclass.cbWndExtra = 0;
    wclass.hInstance = instance;
    wclass.hIcon = NULL;
    wclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wclass.hbrBackground = NULL;
    wclass.lpszMenuName = NULL;
    wclass.lpszClassName = win_class_name;
    wclass.hIconSm = NULL;
    return RegisterClassEx(&wclass);
}

RedWindow_p::RedWindow_p()
    : _win (NULL)
    , _modal_refs (0)
    , _no_taskmgr_dll (NULL)
    , _no_taskmgr_hook (NULL)
    , _minimized (false)
    , _valid_pos (false)
    , _sys_menu (NULL)
{
}

void RedWindow_p::create(RedWindow& red_window, PixelsSource_p& pixels_source)
{
    HWND window;
    if (!(window = CreateWindow(win_class_name, L"", NATIVE_CAPTION_STYLE, CW_USEDEFAULT,
                                0, CW_USEDEFAULT, 0, NULL, NULL, NULL, NULL))) {
        THROW("create window failed");
    }
    HDC dc = GetDC(window);
    if (!dc) {
        THROW("get dc failed");
    }
    _win = window;
    pixels_source.dc = dc;
    SetWindowLong(window, GWL_USERDATA, (LONG)&red_window);
    SetWindowLong(window, GWL_WNDPROC, (LONG)WindowProc);
}

void RedWindow_p::destroy(PixelsSource_p& pixels_source)
{
    if (!_win) {
        return;
    }

    ReleaseDC(_win, pixels_source.dc);
    SetWindowLong(_win, GWL_WNDPROC, (LONG)DefWindowProc);
    SetWindowLong(_win, GWL_USERDATA, NULL);
    DestroyWindow(_win);
}

void RedWindow_p::on_pos_changing(RedWindow& red_window)
{
    if (_minimized || IsIconic(_win)) {
        return;
    }
    Point pos = red_window.get_position();
    _x = pos.x;
    _y = pos.y;
    _valid_pos = true;
}

void RedWindow_p::on_minimized()
{
    _minimized = true;
}

void RedWindow_p::on_restored()
{
    if (!_minimized) {
        return;
    }
    _minimized = false;
    if (!_valid_pos) {
        return;
    }
    _valid_pos = false;
    SetWindowPos(_win, NULL, _x, _y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
}

bool RedWindow_p::prossec_menu_commands(int cmd)
{
    CommandMap::iterator iter = _commands_map.find(cmd);
    if (iter == _commands_map.end()) {
        return false;
    }
    (*iter).second.menu->get_target().do_command((*iter).second.command);
    return true;
}

RedWindow::RedWindow(RedWindow::Listener& listener, int screen_id)
    : _listener (listener)
    , _type (TYPE_NORMAL)
    , _local_cursor (NULL)
    , _cursor_visible (true)
    , _focused (false)
    , _pointer_in_window (false)
    , _trace_key_interception (false)
    , _key_interception_on (false)
    , _menu (NULL)
{
    RECT win_rect;

    create(*this, *(PixelsSource_p*)get_opaque());
    GetWindowRect(_win, &win_rect);
    _window_size.x = win_rect.right - win_rect.left;
    _window_size.y = win_rect.bottom - win_rect.top;
}

RedWindow::~RedWindow()
{
    release_menu(_menu);
    destroy(*(PixelsSource_p*)get_opaque());
    if (_local_cursor) {
        _local_cursor->unref();
    }
}

void RedWindow::set_title(std::wstring& title)
{
    SetWindowText(_win, title.c_str());
}

void RedWindow::set_icon(Icon* icon)
{
    if (!icon) {
        return;
    }
    WinIcon* w_icon = (WinIcon *)icon;
    SendMessage(_win, WM_SETICON, (WPARAM)ICON_SMALL, (LPARAM)w_icon->get_handle());
    SendMessage(_win, WM_SETICON, (WPARAM)ICON_BIG, (LPARAM)w_icon->get_handle());
}

void RedWindow::raise()
{
    SetWindowPos(_win, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RedWindow::position_after(RedWindow *win)
{
    HWND after = NULL;

    if (win) {
        after = win->_win;
    }
    SetWindowPos(_win, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static LONG to_native_style(RedWindow::Type type)
{
    LONG win_style;

    switch (type) {
    case RedWindow::TYPE_NORMAL:
        win_style = NATIVE_CAPTION_STYLE;
        break;
    case RedWindow::TYPE_FULLSCREEN:
        win_style = 0;
        break;
    default:
        THROW("invalid type %d", type);
    }
    return win_style;
}

void RedWindow::show(int screen_id)
{
    if (IsIconic(_win)) {
        ShowWindow(_win, SW_RESTORE);
    }

    const UINT set_pos_flags = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW |
                               SWP_FRAMECHANGED;
    HWND pos;

    SetWindowLong(_win, GWL_STYLE, to_native_style(_type));
    switch (_type) {
    case TYPE_NORMAL:
        pos = HWND_NOTOPMOST;
        break;
    case TYPE_FULLSCREEN:
        pos = HWND_TOPMOST;
        break;
    default:
        THROW("invalid type %d", _type);
    }
    SetWindowPos(_win, pos, 0, 0, 0, 0, set_pos_flags);
}

void RedWindow::external_show()
{
    LONG_PTR style = ::GetWindowLongPtr(_win, GWL_STYLE);
    if ((style & WS_MINIMIZE) == WS_MINIMIZE) {
        ShowWindow(_win, SW_RESTORE);
    } else {
        // Handle the case when hide() was called and the window is not
        // visible. Since we're not the active window, the call just set the
        // windows' style and doesn't show the window.
        if ((style & WS_VISIBLE) != WS_VISIBLE) {
            show(0);
        }
        // We're not the active the window, so we must be attached to the
        // calling thread's message queue before focus is grabbed.
        HWND front = GetForegroundWindow();
        if (front != NULL) {
            DWORD thread = GetWindowThreadProcessId(front, NULL);
            AttachThreadInput(thread, GetCurrentThreadId(), TRUE);
            SetFocus(_win);
            AttachThreadInput(thread, GetCurrentThreadId(), FALSE);
        }
    }
}

void RedWindow::hide()
{
    ShowWindow(_win, SW_HIDE);
}

static void client_to_window_size(HWND win, int width, int height, Point& win_size,
                                  RedWindow::Type type)
{
    RECT area;

    SetRect(&area, 0, 0, width, height);
    AdjustWindowRectEx(&area, to_native_style(type), FALSE, GetWindowLong(win, GWL_EXSTYLE));
    win_size.x = area.right - area.left;
    win_size.y = area.bottom - area.top;
}

void RedWindow::move_and_resize(int x, int y, int width, int height)
{
    client_to_window_size(_win, width, height, _window_size, _type);
    SetWindowPos(_win, NULL, x, y, _window_size.x, _window_size.y, SWP_NOACTIVATE | SWP_NOZORDER);
    if (_minimized) {
        _valid_pos = true;
        _x = x;
        _y = y;
    }
}

void RedWindow::move(int x, int y)
{
    SetWindowPos(_win, NULL, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    if (_minimized) {
        _valid_pos = true;
        _x = x;
        _y = y;
    }
}

void RedWindow::resize(int width, int height)
{
    client_to_window_size(_win, width, height, _window_size, _type);
    SetWindowPos(_win, NULL, 0, 0, _window_size.x, _window_size.y,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
}

void RedWindow::activate()
{
    SetActiveWindow(_win);
    SetFocus(_win);
}

void RedWindow::minimize()
{
    ShowWindow(_win, SW_SHOWMINIMIZED);
}

void RedWindow::set_mouse_position(int x, int y)
{
    POINT pt;
    pt.x = x + get_origin().x;
    pt.y = y + get_origin().y;
    ClientToScreen(_win, &pt);
    SetCursorPos(pt.x, pt.y);
}

class Region_p {
public:
    Region_p(HRGN region) : _region (region) {}
    ~Region_p() {}

    void get_bbox(Rect& bbox) const
    {
        RECT box;

        if (GetRgnBox(_region, &box) == 0) {
            THROW("get region bbox failed");
        }
        bbox.left = box.left;
        bbox.right = box.right;
        bbox.top = box.top;
        bbox.bottom = box.bottom;
    }

    bool contains_point(int x, int y) const
    {
        return !!PtInRegion(_region, x, y);
    }

private:
    HRGN _region;
};

bool RedWindow::get_mouse_anchor_point(Point& pt)
{
    AutoGDIObject region(CreateRectRgn(0, 0, 0, 0));
    WindowDC win_dc(_win);

    GetRandomRgn(*win_dc, (HRGN)region.get(), SYSRGN);
    Point anchor;
    Region_p region_p((HRGN)region.get());
    if (!find_anchor_point(region_p, anchor)) {
        return false;
    }
    POINT screen_pt;
    screen_pt.x = anchor.x;
    screen_pt.y = anchor.y;
    ScreenToClient(_win, &screen_pt);
    pt.x = screen_pt.x - get_origin().x;
    pt.y = screen_pt.y - get_origin().y;
    return true;
}

void RedWindow::cupture_mouse()
{
    RECT client_rect;
    POINT origin;

    origin.x = origin.y = 0;
    ClientToScreen(_win, &origin);
    GetClientRect(_win, &client_rect);
    OffsetRect(&client_rect, origin.x, origin.y);
    ClipCursor(&client_rect);
}

void RedWindow::release_mouse()
{
    ClipCursor(NULL);
}

void RedWindow::set_cursor(LocalCursor* local_cursor)
{
    ASSERT(local_cursor);
    if (_local_cursor) {
        _local_cursor->unref();
    }
    _local_cursor = local_cursor->ref();
    if (_pointer_in_window) {
        _local_cursor->set(_win);
        while (ShowCursor(TRUE) < 0);
    }
    _cursor_visible = true;
}

void RedWindow::hide_cursor()
{
    if (_cursor_visible) {
        if (_pointer_in_window) {
            while (ShowCursor(FALSE) > -1);
        }
        _cursor_visible = false;
    }
}

void RedWindow::show_cursor()
{
    if (!_cursor_visible) {
        if (_pointer_in_window) {
            while (ShowCursor(TRUE) < 0);
        }
        _cursor_visible = true;
    }
}

Point RedWindow::get_position()
{
    Point position;
    if (_minimized || IsIconic(_win)) {
        if (_valid_pos) {
            position.x = _x;
            position.y = _y;
        } else {
            position.x = position.y = 0;
        }
    } else {
        RECT window_rect;
        GetWindowRect(_win, &window_rect);
        position.x = window_rect.left;
        position.y = window_rect.top;
    }
    return position;
}

Point RedWindow::get_size()
{
    RECT client_rect;
    GetClientRect(_win, &client_rect);
    Point pt = {client_rect.right - client_rect.left, client_rect.bottom - client_rect.top};
    return pt;
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if ((nCode == HC_ACTION)) {
        KBDLLHOOKSTRUCT *hooked = (KBDLLHOOKSTRUCT*)lParam;
        DWORD dwMsg = 1;

        //  dwMsg shall contain the information that would be stored
        //  in the usual lParam argument of a WM_KEYDOWN message.
        //  All information like hardware scan code and other flags
        //  are stored within one double word at different bit offsets.
        //  Refer to MSDN for further information:
        //
        //  (Keystroke Messages)
        dwMsg += hooked->scanCode << 16;
        dwMsg += hooked->flags << 24;

        // In some cases scan code of VK_RSHIFT is fake shift (probably a bug) so we
        // convert it to non extended code. Also, QEmu doesn't expect num-lock to be
        // an extended key.
        if ((hooked->vkCode == VK_NUMLOCK) || (hooked->vkCode == VK_RSHIFT)) {
            dwMsg &= ~(1 << 24);
        }

        SendMessage(focus_window, wParam, hooked->vkCode, dwMsg);

        // Forward all modifier key strokes to update keyboard leds & shift/ctrl/alt state
        switch (hooked->vkCode) {
        case VK_CAPITAL:
        case VK_SCROLL:
        case VK_NUMLOCK:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_LMENU:
        case VK_RMENU:
            break;
        default:
            return 1;
        }
    }

    // In all other cases, we call the next hook and return it's value.
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void RedWindow::do_start_key_interception()
{
    _key_interception_on = true;
    _listener.on_start_key_interception();
    if (low_keyboard_hook) {
        return;
    }
    low_keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                         GetModuleHandle(NULL), 0);
}

void RedWindow::do_stop_key_interception()
{
    _key_interception_on = false;
    _listener.on_stop_key_interception();
    if (!low_keyboard_hook) {
        return;
    }
    UnhookWindowsHookEx(low_keyboard_hook);
    low_keyboard_hook = NULL;
}

void RedWindow::start_key_interception()
{
    if (_trace_key_interception) {
        return;
    }
    _trace_key_interception = true;
    if (_focused && _pointer_in_window) {
        do_start_key_interception();
    }
}

void RedWindow::stop_key_interception()
{
    if (!_trace_key_interception) {
        return;
    }
    _trace_key_interception = false;
    if (_key_interception_on) {
        do_stop_key_interception();
    }
}

void RedWindow::init()
{
    if (!(class_atom = register_class(instance))) {
        THROW("register class failed");
    }
}

#ifdef USE_OGL

void RedWindow::touch_context_draw()
{
}

void RedWindow::touch_context_copy()
{
}

void RedWindow::untouch_context()
{
}

#endif

void RedWindow::set_type_gl()
{
}

void RedWindow::unset_type_gl()
{
}

void RedWindow::on_focus_in()
{
    _focused = true;
    focus_window = _win;
    get_listener().on_activate();
    if (_pointer_in_window && _trace_key_interception) {
        do_start_key_interception();
    }
}

void RedWindow::on_focus_out()
{
    if (!_focused) {
        return;
    }

    _focused = false;

    if (_key_interception_on) {
        do_stop_key_interception();
    }
    get_listener().on_deactivate();
}

void RedWindow::on_pointer_enter()
{
    if (_pointer_in_window) {
        return;
    }

    if (_cursor_visible) {
        if (_local_cursor) {
            _local_cursor->set(_win);
        }
        while (ShowCursor(TRUE) < 0);
    } else {
        while (ShowCursor(FALSE) > -1);
    }
    _pointer_in_window = true;
    _listener.on_pointer_enter();

    TRACKMOUSEEVENT tme;
    tme.cbSize = sizeof(TRACKMOUSEEVENT);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = _win;
    if (!TrackMouseEvent(&tme)) {
        THROW("track mouse event failed");
    }
    if (_focused && _trace_key_interception) {
        do_start_key_interception();
    }
}

void RedWindow::on_pointer_leave()
{
    if (!_pointer_in_window) {
        return;
    }
    if (!_cursor_visible) {
        while (ShowCursor(TRUE) < 0);
    }
    _pointer_in_window = false;
    _listener.on_pointer_leave();
    if (_key_interception_on) {
        do_stop_key_interception();
    }
}

static void insert_seperator(HMENU menu)
{
    MENUITEMINFO item_info;
    item_info.cbSize = sizeof(item_info);
    item_info.fMask = MIIM_TYPE;
    item_info.fType = MFT_SEPARATOR;
    item_info.dwTypeData = NULL;
    item_info.dwItemData = 0;
    InsertMenuItem(menu, GetMenuItemCount(menu), TRUE, &item_info);
}

static void utf8_to_wchar(const std::string& src, std::wstring& dest)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, src.c_str(), -1, NULL, 0);
    if (!len) {
        THROW("fail to conver utf8 to wchar");
    }
    dest.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, src.c_str(), -1, (wchar_t *)dest.c_str(), len);
}

static void insert_command(HMENU menu, const std::string& name, int id)
{
    MENUITEMINFO item_info;
    item_info.cbSize = sizeof(item_info);
    item_info.fMask = MIIM_TYPE | MIIM_ID;
    item_info.fType = MFT_STRING;
    std::wstring wname;
    utf8_to_wchar(name, wname);
    item_info.cch = wname.size();
    item_info.dwTypeData = (wchar_t *)wname.c_str();
    item_info.wID = id;
    InsertMenuItem(menu, GetMenuItemCount(menu), TRUE, &item_info);
}

static HMENU insert_sub_menu(HMENU menu, const std::string& name)
{
    MENUITEMINFO item_info;
    item_info.cbSize = sizeof(item_info);
    item_info.fMask = MIIM_TYPE | MIIM_SUBMENU;
    item_info.fType = MFT_STRING;
    std::wstring wname;
    utf8_to_wchar(name, wname);
    item_info.cch = wname.size();
    item_info.dwTypeData = (wchar_t *)wname.c_str();
    item_info.hSubMenu = CreateMenu();
    InsertMenuItem(menu, GetMenuItemCount(menu), TRUE, &item_info);
    return item_info.hSubMenu;
}

static int next_free_id = 1;
static const int last_id = 0x0f00;

static std::list<int> free_sys_menu_id;

static int alloc_sys_cmd_id()
{
    if (!free_sys_menu_id.empty()) {
        int ret = *free_sys_menu_id.begin();
        free_sys_menu_id.pop_front();
        return ret;
    }
    if (next_free_id == last_id) {
        THROW("failed");
    }

    return next_free_id++ << 4;
}

static void free_sys_cmd_id(int id)
{
    free_sys_menu_id.push_back(id >> 4);
}

static void insert_menu(Menu* menu, HMENU native, CommandMap& _commands_map)
{
    int pos = 0;

    for (;; pos++) {
        Menu::ItemType type = menu->item_type_at(pos);
        switch (type) {
        case Menu::MENU_ITEM_TYPE_COMMAND: {
            std::string name;
            int command_id;
            menu->command_at(pos, name, command_id);
            int sys_command = alloc_sys_cmd_id();
            _commands_map[sys_command] = CommandInfo(menu, command_id);
            insert_command(native, name, sys_command);
            break;
        }
        case Menu::MENU_ITEM_TYPE_MENU: {
            AutoRef<Menu> sub_menu(menu->sub_at(pos));
            HMENU native_sub = insert_sub_menu(native, (*sub_menu)->get_name());
            insert_menu(*sub_menu, native_sub, _commands_map);
            break;
        }
        case Menu::MENU_ITEM_TYPE_SEPARATOR:
            insert_seperator(native);
            break;
        case Menu::MENU_ITEM_TYPE_INVALID:
            return;
        }
    }
}

void RedWindow_p::release_menu(Menu* menu)
{
    if (menu) {
        while (!_commands_map.empty()) {
            free_sys_cmd_id((*_commands_map.begin()).first);
            _commands_map.erase(_commands_map.begin());
        }
        GetSystemMenu(_win, TRUE);
        _sys_menu = NULL;
        menu->unref();
        return;
    }
}

void RedWindow::set_menu(Menu* menu)
{
    release_menu(_menu);
    _menu = NULL;

    if (!menu) {
        return;
    }
    _menu = menu->ref();
    _sys_menu = GetSystemMenu(_win, FALSE);
    insert_seperator(_sys_menu);
    insert_menu(_menu, _sys_menu, _commands_map);
}

static LRESULT CALLBACK MessageFilterProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0)
    {
        MSG* msg = (MSG*)lParam;

        switch (msg->message) {
        case WM_SYSKEYUP:
        case WM_KEYUP: {
            RedKey key = translate_key(msg->wParam, HIWORD(msg->lParam) & 0xff,
                                       (msg->lParam & (1 << 24)) != 0);
            filtered_up_keys.push_back(key);
            break;
        }
        default:
            break;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}
