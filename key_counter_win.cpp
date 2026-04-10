// key_counter_win_panel.cpp

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
  using clock_type = std::chrono::steady_clock;
  using time_point = clock_type::time_point;

  std::atomic<unsigned long long> g_key_count{0};
  std::atomic<bool> g_running{true};
  HHOOK g_hook = nullptr;
  DWORD g_main_thread_id = 0;

  std::mutex g_samples_mutex;
  std::deque<time_point> g_key_samples;

  constexpr SHORT kPanelCol = 4;
  constexpr SHORT kPanelRow = 2;
  constexpr SHORT kPanelWidth = 34;
  constexpr auto kUiPeriod = std::chrono::milliseconds(100);
  constexpr auto kMovingAverageWindow = std::chrono::milliseconds(2000);

  enum Color : WORD
  {
    BLACK        = 0,
    BLUE         = FOREGROUND_BLUE,
    GREEN        = FOREGROUND_GREEN,
    RED          = FOREGROUND_RED,
    CYAN         = FOREGROUND_GREEN | FOREGROUND_BLUE,
    MAGENTA      = FOREGROUND_RED   | FOREGROUND_BLUE,
    YELLOW       = FOREGROUND_RED   | FOREGROUND_GREEN,
    WHITE        = FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_BLUE,
    INTENSE      = FOREGROUND_INTENSITY,

    BG_BLUE      = BACKGROUND_BLUE,
    BG_GREEN     = BACKGROUND_GREEN,
    BG_RED       = BACKGROUND_RED,
    BG_CYAN      = BACKGROUND_GREEN | BACKGROUND_BLUE,
    BG_MAGENTA   = BACKGROUND_RED   | BACKGROUND_BLUE,
    BG_YELLOW    = BACKGROUND_RED   | BACKGROUND_GREEN,
    BG_WHITE     = BACKGROUND_RED   | BACKGROUND_GREEN | BACKGROUND_BLUE,
    BG_INTENSE   = BACKGROUND_INTENSITY
  };

  struct ConsoleState
  {
    HANDLE handle = INVALID_HANDLE_VALUE;
    WORD default_attributes = WHITE;
  };

  void move_cursor(HANDLE hconsole, SHORT col, SHORT row)
  {
    COORD pos{col, row};
    SetConsoleCursorPosition(hconsole, pos);
  }

  void set_color(HANDLE hconsole, WORD attributes)
  {
    SetConsoleTextAttribute(hconsole, attributes);
  }

  void write_text(HANDLE hconsole, SHORT col, SHORT row, const std::wstring& text, WORD attributes)
  {
    DWORD written = 0;
    move_cursor(hconsole, col, row);
    set_color(hconsole, attributes);
    WriteConsoleW(hconsole, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
  }

  std::wstring pad_or_trim(const std::wstring& s, std::size_t width)
  {
    if (s.size() >= width)
      return s.substr(0, width);
  
    return s + std::wstring(width - s.size(), L' ');
  }

  std::wstring make_inner_line(const std::wstring& content, std::size_t inner_width)
  {
    return L"│" + pad_or_trim(content, inner_width) + L"│";
  }

  void draw_panel_frame(const ConsoleState& console)
  {
    const SHORT x = kPanelCol;
    const SHORT y = kPanelRow;
    const SHORT w = kPanelWidth;
    const SHORT inner_w = w - 2;

    const WORD border_color = YELLOW | INTENSE;
    const WORD title_color  = WHITE | INTENSE | BG_BLUE;
    const WORD body_color   = WHITE | INTENSE;

    write_text(console.handle, x, y + 0, L"┌" + std::wstring(inner_w, L'─') + L"┐", border_color);
    write_text(console.handle, x, y + 1, make_inner_line(L" Keyboard counter", inner_w), title_color);
    write_text(console.handle, x, y + 2, L"├" + std::wstring(inner_w, L'─') + L"┤", border_color);

    for (SHORT row = 3; row <= 6; ++row)
      write_text(console.handle, x, y + row, make_inner_line(L"", inner_w), body_color);

    write_text(console.handle, x, y + 7, L"└" + std::wstring(inner_w, L'─') + L"┘", border_color);
  }

  double compute_keys_per_second()
  {
    const auto now = clock_type::now();
    const auto cutoff = now - kMovingAverageWindow;

    std::lock_guard<std::mutex> lock(g_samples_mutex);

    while (!g_key_samples.empty() && g_key_samples.front() < cutoff)
      g_key_samples.pop_front();

    const double seconds = std::chrono::duration<double>(kMovingAverageWindow).count();
    return static_cast<double>(g_key_samples.size()) / seconds;
  }

  void render_panel_body(const ConsoleState& console)
  {
    const SHORT x = kPanelCol;
    const SHORT y = kPanelRow;
    const SHORT inner_w = kPanelWidth - 2;
  
    const auto total = g_key_count.load();
    const double kps = compute_keys_per_second();
  
    std::wostringstream total_ss;
    total_ss << L" Total key presses: " << total;
  
    std::wostringstream kps_ss;
    kps_ss << L" Keys/sec (2s avg): " << std::fixed << std::setprecision(2) << kps;
  
    std::wstring status = g_running ? L" Running" : L" Stopped";
    std::wstring hint   = L" Ctrl+C to exit";
  
    write_text(console.handle, x, y + 3, make_inner_line(total_ss.str(), inner_w), CYAN | INTENSE);
    write_text(console.handle, x, y + 4, make_inner_line(kps_ss.str(), inner_w), GREEN | INTENSE);
    write_text(console.handle, x, y + 5, make_inner_line(status, inner_w), MAGENTA | INTENSE);
    write_text(console.handle, x, y + 6, make_inner_line(hint, inner_w), WHITE);
  }

  LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam)
  {
    if (code == HC_ACTION && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN))
    {
      ++g_key_count;

      const auto now = clock_type::now();
      std::lock_guard<std::mutex> lock(g_samples_mutex);
      g_key_samples.push_back(now);
    }

    return CallNextHookEx(g_hook, code, wparam, lparam);
  }

  BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
  {
    switch (ctrl_type)
    {
      case CTRL_C_EVENT:
      case CTRL_BREAK_EVENT:
      case CTRL_CLOSE_EVENT:
      case CTRL_LOGOFF_EVENT:
      case CTRL_SHUTDOWN_EVENT:
        g_running = false;
        //PostQuitMessage(0);
        PostThreadMessage(g_main_thread_id, WM_QUIT, 0, 0);
        return TRUE;
      default:
        return FALSE;
    }
  }
}

int main()
{
  g_main_thread_id = GetCurrentThreadId();
  ConsoleState console;
  console.handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (console.handle == INVALID_HANDLE_VALUE)
  {
    std::cerr << "Failed to get console handle.\n";
    return 1;
  }

  CONSOLE_SCREEN_BUFFER_INFO csbi{};
  if (GetConsoleScreenBufferInfo(console.handle, &csbi))
    console.default_attributes = csbi.wAttributes;

  if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE))
  {
    std::cerr << "Failed to install console control handler.\n";
    return 1;
  }

  g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, nullptr, 0);
  if (!g_hook)
  {
    std::cerr << "Failed to install keyboard hook.\n";
    return 1;
  }

  draw_panel_frame(console);
  render_panel_body(console);

  std::thread printer([&console]()
  {
    while (g_running)
    {
      render_panel_body(console);
      std::this_thread::sleep_for(kUiPeriod);
    }
    render_panel_body(console);
  });

  MSG msg{};
  //while (g_running && GetMessage(&msg, nullptr, 0, 0) > 0)
  while (true)
  {
    const BOOL result = GetMessage(&msg, nullptr, 0, 0);
    if (result <= 0)
      break;
    
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  g_running = false;

  if (printer.joinable())
    printer.join();

  if (g_hook)
  {
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
  }

  set_color(console.handle, console.default_attributes);
  move_cursor(console.handle, 0, kPanelRow + 10);
  std::wcout << L"Final count: " << g_key_count.load() << L"\n";

  return 0;
}
