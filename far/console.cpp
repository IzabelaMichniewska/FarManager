﻿/*
console.cpp

Console functions
*/
/*
Copyright © 2010 Far Group
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// BUGBUG
#include "platform.headers.hpp"

// Self:
#include "console.hpp"

// Internal:
#include "imports.hpp"
#include "colormix.hpp"
#include "interf.hpp"
#include "strmix.hpp"
#include "exception.hpp"
#include "palette.hpp"
#include "encoding.hpp"
#include "char_width.hpp"
#include "log.hpp"

// Platform:
#include "platform.version.hpp"

// Common:
#include "common.hpp"
#include "common/2d/algorithm.hpp"
#include "common/algorithm.hpp"
#include "common/enum_substrings.hpp"
#include "common/function_traits.hpp"
#include "common/io.hpp"
#include "common/range.hpp"
#include "common/scope_exit.hpp"
#include "common/view/enumerate.hpp"

// External:
#include "format.hpp"

//----------------------------------------------------------------------------

#define ESC L"\x1b"
#define CSI ESC L"["

static bool sWindowMode;
static bool sEnableVirtualTerminal;

constexpr auto bad_char_replacement = L' ';

wchar_t ReplaceControlCharacter(wchar_t const Char)
{
	switch (Char)
	{
	// C0
	case 0x00: return L' '; // space
	case 0x01: return L'☺'; // white smiling face
	case 0x02: return L'☻'; // black smiling face
	case 0x03: return L'♥'; // black heart suit
	case 0x04: return L'♦'; // black diamond suit
	case 0x05: return L'♣'; // black club suit
	case 0x06: return L'♠'; // black spade suit
	case 0x07: return L'•'; // bullet
	case 0x08: return L'◘'; // inverse bullet
	case 0x09: return L'○'; // white circle
	case 0x0A: return L'◙'; // inverse white circle
	case 0x0B: return L'♂'; // male sign
	case 0x0C: return L'♀'; // female sign
	case 0x0D: return L'♪'; // eighth note
	case 0x0E: return L'♫'; // beamed eighth notes
	case 0x0F: return L'☼'; // white sun with rays
	case 0x10: return L'►'; // black right - pointing pointer
	case 0x11: return L'◄'; // black left - pointing pointer
	case 0x12: return L'↕'; // up down arrow
	case 0x13: return L'‼'; // double exclamation mark
	case 0x14: return L'¶'; // pilcrow sign
	case 0x15: return L'§'; // section sign
	case 0x16: return L'▬'; // black rectangle
	case 0x17: return L'↨'; // up down arrow with base
	case 0x18: return L'↑'; // upwards arrow
	case 0x19: return L'↓'; // downwards arrow
	case 0x1A: return L'→'; // rightwards arrow
	case 0x1B: return L'←'; // leftwards arrow
	case 0x1C: return L'∟'; // right angle
	case 0x1D: return L'↔'; // left right arrow
	case 0x1E: return L'▲'; // black up - pointing triangle
	case 0x1F: return L'▼'; // black down - pointing triangle
	case 0x7F: return L'⌂'; // house

	// C1
	// These are considered control characters too now.
	// Unlike C0, it is unclear what glyphs to use, so just replace with FFFD for now.
	case 0x80:
	case 0x81:
	case 0x82:
	case 0x83:
	case 0x84:
	case 0x85:
	case 0x86:
	case 0x87:
	case 0x88:
	case 0x89:
	case 0x8A:
	case 0x8B:
	case 0x8C:
	case 0x8D:
	case 0x8E:
	case 0x8F:
	case 0x90:
	case 0x91:
	case 0x92:
	case 0x93:
	case 0x94:
	case 0x95:
	case 0x96:
	case 0x97:
	case 0x98:
	case 0x99:
	case 0x9A:
	case 0x9B:
	case 0x9C:
	case 0x9D:
	case 0x9E:
	case 0x9F: return L'�'; // replacement character

	default:   return Char;
	}
}

static bool sanitise_dbsc_pair(FAR_CHAR_INFO& First, FAR_CHAR_INFO& Second)
{
	const auto
		IsFirst = flags::check_any(First.Attributes.Flags, COMMON_LVB_LEADING_BYTE),
		IsSecond = flags::check_any(Second.Attributes.Flags, COMMON_LVB_TRAILING_BYTE);

	if (!IsFirst && !IsSecond)
	{
		// Not DBSC, awesome
		return false;
	}

	flags::clear(First.Attributes.Flags, COMMON_LVB_LEADING_BYTE);
	flags::clear(Second.Attributes.Flags, COMMON_LVB_TRAILING_BYTE);

	if (First == Second)
	{
		// Valid DBSC, awesome
		flags::set(First.Attributes.Flags, COMMON_LVB_LEADING_BYTE);
		flags::set(Second.Attributes.Flags, COMMON_LVB_TRAILING_BYTE);

		return false;
	}

	if (IsFirst)
		First.Char = bad_char_replacement;

	if (IsSecond)
		Second.Char = bad_char_replacement;

	return true;
}

static bool sanitise_surrogate_pair(FAR_CHAR_INFO& First, FAR_CHAR_INFO& Second)
{
	const auto
		IsFirst = encoding::utf16::is_high_surrogate(First.Char),
		IsSecond = encoding::utf16::is_low_surrogate(Second.Char);

	if (!IsFirst && !IsSecond)
	{
		// Not surrogate, awesome
		return false;
	}

	if (encoding::utf16::is_valid_surrogate_pair(First.Char, Second.Char) && First.Attributes == Second.Attributes)
	{
		// Valid surrogate, awesome
		return false;
	}

	if (IsFirst)
		First.Char = bad_char_replacement;

	if (IsSecond)
		Second.Char = bad_char_replacement;

	return true;
}

void sanitise_pair(FAR_CHAR_INFO& First, FAR_CHAR_INFO& Second)
{
	sanitise_dbsc_pair(First, Second) || sanitise_surrogate_pair(First, Second);
}

bool get_console_screen_buffer_info(HANDLE ConsoleOutput, CONSOLE_SCREEN_BUFFER_INFO* ConsoleScreenBufferInfo)
{
	if (!GetConsoleScreenBufferInfo(ConsoleOutput, ConsoleScreenBufferInfo))
	{
		LOGERROR(L"GetConsoleScreenBufferInfo(): {}"sv, last_error());
		return false;
	}

	const auto& Window = ConsoleScreenBufferInfo->srWindow;

	// Mantis#3919: Windows 10 is a PITA
	if (Window.Left > Window.Right || Window.Top > Window.Bottom)
	{
		LOGERROR(L"Console window state is broken, trying to repair"sv);

		auto NewWindow = Window;

		if (Window.Left > Window.Right)
		{
			NewWindow.Left = 0;
			NewWindow.Right = std::min(ConsoleScreenBufferInfo->dwMaximumWindowSize.X - 1, ConsoleScreenBufferInfo->dwSize.X - 1);
		}

		// https://forum.farmanager.com/viewtopic.php?p=170779#p170779
		if (Window.Top > Window.Bottom)
		{
			NewWindow.Bottom = ConsoleScreenBufferInfo->dwSize.Y - 1;
			NewWindow.Top = std::clamp(NewWindow.Top, short{}, NewWindow.Bottom);
		}

		if (!SetConsoleWindowInfo(ConsoleOutput, true, &NewWindow))
		{
			LOGERROR(L"SetConsoleWindowInfo(): {}"sv, last_error());
			return false;
		}

		if (!GetConsoleScreenBufferInfo(ConsoleOutput, ConsoleScreenBufferInfo))
		{
			LOGERROR(L"GetConsoleScreenBufferInfo(): {}"sv, last_error());
			return false;
		}
	}

	return true;
}

static COORD make_coord(point const& Point)
{
	return
	{
		static_cast<short>(Point.x),
		static_cast<short>(Point.y)
	};
}

static SMALL_RECT make_rect(rectangle const& Rectangle)
{
	return
	{
		static_cast<short>(Rectangle.left),
		static_cast<short>(Rectangle.top),
		static_cast<short>(Rectangle.right),
		static_cast<short>(Rectangle.bottom)
	};
}

static short GetDelta(CONSOLE_SCREEN_BUFFER_INFO const& csbi)
{
	return csbi.dwSize.Y - (csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
}

namespace console_detail
{
	// пишем/читаем порциями по 32 K, иначе проблемы.
	const unsigned int MAXSIZE = 32768;

	class external_console
	{
	public:
		NONCOPYABLE(external_console);
		external_console() = default;

		struct ModuleImports
		{
		private:
			os::rtdl::module m_Module{ L"extendedconsole.dll"sv };

		public:
#define DECLARE_IMPORT_FUNCTION(name, ...) os::rtdl::function_pointer<__VA_ARGS__> p ## name{ m_Module, #name }

			DECLARE_IMPORT_FUNCTION(ReadOutput,           BOOL(WINAPI*)(FAR_CHAR_INFO* Buffer, COORD BufferSize, COORD BufferCoord, SMALL_RECT* ReadRegion));
			DECLARE_IMPORT_FUNCTION(WriteOutput,          BOOL(WINAPI*)(const FAR_CHAR_INFO* Buffer, COORD BufferSize, COORD BufferCoord, SMALL_RECT* WriteRegion));
			DECLARE_IMPORT_FUNCTION(Commit,               BOOL(WINAPI*)());
			DECLARE_IMPORT_FUNCTION(GetTextAttributes,    BOOL(WINAPI*)(FarColor* Attributes));
			DECLARE_IMPORT_FUNCTION(SetTextAttributes,    BOOL(WINAPI*)(const FarColor* Attributes));
			DECLARE_IMPORT_FUNCTION(ClearExtraRegions,    BOOL(WINAPI*)(const FarColor* Color, int Mode));

#undef DECLARE_IMPORT_FUNCTION
		}
		Imports;
	};

	static nifty_counter::buffer<external_console> Storage;
	static auto& ExternalConsole = reinterpret_cast<external_console&>(Storage);

	console::console():
		m_OriginalInputHandle(GetStdHandle(STD_INPUT_HANDLE)),
		m_StreamBuffersOverrider(std::make_unique<stream_buffers_overrider>())
	{
		placement::construct(ExternalConsole);
	}

	console::~console()
	{
		if (m_FileHandle != -1)
			_close(m_FileHandle);

		placement::destruct(ExternalConsole);
	}

	bool console::Allocate() const
	{
		if (!AllocConsole())
		{
			LOGERROR(L"AllocConsole(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::Free() const
	{
		if (!FreeConsole())
		{
			LOGERROR(L"FreeConsole(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	HANDLE console::GetInputHandle() const
	{
		return GetStdHandle(STD_INPUT_HANDLE);
	}

	HANDLE console::GetOutputHandle() const
	{
		return GetStdHandle(STD_OUTPUT_HANDLE);
	}

	HANDLE console::GetErrorHandle() const
	{
		return GetStdHandle(STD_ERROR_HANDLE);
	}

	HANDLE console::GetOriginalInputHandle() const
	{
		return m_OriginalInputHandle;
	}

	HWND console::GetWindow() const
	{
		return GetConsoleWindow();
	}

	bool console::GetSize(point& Size) const
	{
		CONSOLE_SCREEN_BUFFER_INFO ConsoleScreenBufferInfo;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &ConsoleScreenBufferInfo))
			return false;

		if (sWindowMode)
		{
			const auto& Window = ConsoleScreenBufferInfo.srWindow;

			Size =
			{
				Window.Right - Window.Left + 1,
				Window.Bottom - Window.Top + 1
			};
		}
		else
		{
			Size = ConsoleScreenBufferInfo.dwSize;
		}

		return true;
	}

	bool console::SetSize(point const Size) const
	{
		if (!sWindowMode)
			return SetScreenBufferSize(Size);

		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return false;

		csbi.srWindow.Left = 0;
		csbi.srWindow.Right = Size.x - 1;
		csbi.srWindow.Bottom = csbi.dwSize.Y - 1;
		csbi.srWindow.Top = csbi.srWindow.Bottom - (Size.y - 1);
		point WindowCoord{ csbi.srWindow.Right - csbi.srWindow.Left + 1, csbi.srWindow.Bottom - csbi.srWindow.Top + 1 };
		if (WindowCoord.x > csbi.dwSize.X || WindowCoord.y > csbi.dwSize.Y)
		{
			WindowCoord.x = std::max(WindowCoord.x, static_cast<int>(csbi.dwSize.X));
			WindowCoord.y = std::max(WindowCoord.y, static_cast<int>(csbi.dwSize.Y));
			if (!SetScreenBufferSize(WindowCoord))
				return false;

			if (WindowCoord.x > csbi.dwSize.X)
			{
				// windows sometimes uses existing colors to init right region of screen buffer
				FarColor Color;
				GetTextAttributes(Color);
				ClearExtraRegions(Color, CR_RIGHT);
			}
		}

		return SetWindowRect(csbi.srWindow);
	}

	bool console::SetScreenBufferSize(point const Size) const
	{
		const auto Out = GetOutputHandle();

		// This abominable workaround is for another Windows 10 bug, see https://github.com/microsoft/terminal/issues/2366
		// TODO: check the OS version once they fix it
		if (IsVtSupported())
		{
			CONSOLE_SCREEN_BUFFER_INFO Info;
			if (get_console_screen_buffer_info(Out, &Info))
			{
				// Make sure the cursor is within the new buffer
				if (!(Info.dwCursorPosition.X < Size.x && Info.dwCursorPosition.Y < Size.y))
				{
					if (!SetConsoleCursorPosition(
						Out,
						{
							std::min(Info.dwCursorPosition.X, static_cast<SHORT>(Size.x - 1)),
							std::min(Info.dwCursorPosition.Y, static_cast<SHORT>(Size.y - 1))
						}
					))
					{
						LOGERROR(L"SetConsoleCursorPosition(): {}"sv, last_error());
						return false;
					}
				}

				// Make sure the window is within the new buffer:
				rectangle Rect(Info.srWindow);
				if (Size.x < Rect.right + 1 || Size.y < Rect.bottom + 1)
				{
					const auto Width = Rect.width(), Height = Rect.height();
					Rect.bottom = std::min(Rect.bottom, Size.y - 1);
					Rect.right = std::min(Rect.right, Size.x - 1);
					Rect.top = std::max(0, Rect.bottom - Height);
					Rect.left = std::max(0, Rect.right - Width);

					if (!SetWindowRect(Rect))
						return false;
				}
			}
		}

		if (!SetConsoleScreenBufferSize(Out, make_coord(Size)))
		{
			LOGERROR(L"SetConsoleScreenBufferSize(): {}"sv, last_error());
			return false;
		}

		// After changing the buffer size the window size is not always correct
		if (IsVtSupported())
		{
			CONSOLE_SCREEN_BUFFER_INFO Info;
			if (get_console_screen_buffer_info(Out, &Info))
			{
				SetWindowRect(Info.srWindow);
			}
		}

		return true;
	}

	bool console::GetWindowRect(rectangle& ConsoleWindow) const
	{
		CONSOLE_SCREEN_BUFFER_INFO ConsoleScreenBufferInfo;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &ConsoleScreenBufferInfo))
			return false;

		ConsoleWindow = ConsoleScreenBufferInfo.srWindow;
		return true;
	}

	bool console::SetWindowRect(rectangle const& ConsoleWindow) const
	{
		const auto Rect = make_rect(ConsoleWindow);
		if (!SetConsoleWindowInfo(GetOutputHandle(), true, &Rect))
		{
			LOGERROR(L"SetConsoleWindowInfo(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::GetWorkingRect(rectangle& WorkingRect) const
	{
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return false;

		WorkingRect.bottom = csbi.dwSize.Y - 1;
		WorkingRect.left = 0;
		WorkingRect.right = WorkingRect.left + ScrX;
		WorkingRect.top = WorkingRect.bottom - ScrY;
		return true;
	}

	string console::GetPhysicalTitle() const
	{
		// Don't use GetConsoleTitle here, it's buggy.
		string Title;
		if (!os::GetWindowText(GetWindow(), Title))
		{
			LOGERROR(L"GetWindowText(): {}"sv, last_error());
		}

		return Title;
	}

	string console::GetTitle() const
	{
		return m_Title;
	}

	bool console::SetTitle(string_view const Title) const
	{
		m_Title = Title;
		if (!SetConsoleTitle(m_Title.c_str()))
		{
			LOGERROR(L"SetConsoleTitle(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::GetKeyboardLayoutName(string &strName) const
	{
		wchar_t Buffer[KL_NAMELENGTH];
		if (!imports.GetConsoleKeyboardLayoutNameW(Buffer))
		{
			LOGERROR(L"GetConsoleKeyboardLayoutNameW(): {}"sv, last_error());
			return false;
		}

		strName = Buffer;
		return true;
	}

	uintptr_t console::GetInputCodepage() const
	{
		return GetConsoleCP();
	}

	bool console::SetInputCodepage(uintptr_t Codepage) const
	{
		if (!SetConsoleCP(Codepage))
		{
			LOGERROR(L"SetConsoleCP(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	uintptr_t console::GetOutputCodepage() const
	{
		return GetConsoleOutputCP();
	}

	bool console::SetOutputCodepage(uintptr_t Codepage) const
	{
		if (!SetConsoleOutputCP(Codepage))
		{
			LOGERROR(L"SetConsoleOutputCP(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::SetControlHandler(PHANDLER_ROUTINE HandlerRoutine, bool Add) const
	{
		if (!SetConsoleCtrlHandler(HandlerRoutine, Add))
		{
			LOGERROR(L"SetConsoleCtrlHandler(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::GetMode(HANDLE ConsoleHandle, DWORD& Mode) const
	{
		if (!GetConsoleMode(ConsoleHandle, &Mode))
		{
			LOGERROR(L"GetConsoleMode(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::SetMode(HANDLE ConsoleHandle, DWORD Mode) const
	{
		if (!SetConsoleMode(ConsoleHandle, Mode))
		{
			LOGERROR(L"SetConsoleMode(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::IsVtSupported() const
	{
		static const auto Result = [&]
		{
			// https://devblogs.microsoft.com/commandline/24-bit-color-in-the-windows-console/
			if (!os::version::is_win10_build_or_later(14931))
				return false;

			const auto Handle = GetOutputHandle();

			DWORD CurrentMode;
			if (!GetMode(Handle, CurrentMode))
				return false;

			if (CurrentMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
				return true;

			if (!SetMode(Handle, CurrentMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
				return false;

			SetMode(Handle, CurrentMode);
			return true;
		}();

		return Result;
	}

	static bool get_current_console_font(HANDLE OutputHandle, CONSOLE_FONT_INFO& FontInfo)
	{
		if (!GetCurrentConsoleFont(OutputHandle, FALSE, &FontInfo))
		{
			LOGERROR(L"GetCurrentConsoleFont(): {}"sv, last_error());
			return false;
		}

		// in XP FontInfo.dwFontSize contains something else than the size in pixels.
		FontInfo.dwFontSize = GetConsoleFontSize(OutputHandle, FontInfo.nFont);

		return FontInfo.dwFontSize.X && FontInfo.dwFontSize.Y;
	}

	// Workaround for a bug in the classic console: mouse position is screen-based
	static void fix_wheel_coordinates(MOUSE_EVENT_RECORD& Record)
	{
		if (!(Record.dwEventFlags & (MOUSE_WHEELED | MOUSE_HWHEELED)))
			return;

		// 'New' console doesn't need this
		if (::console.IsVtSupported())
			return;

		// Note: using the current mouse position rather than what's in the wheel event
		POINT CursorPos;
		if (!GetCursorPos(&CursorPos))
		{
			LOGWARNING(L"GetCursorPos(): {}"sv, last_error());
			return;
		}

		const auto WindowHandle = ::console.GetWindow();

		auto RelativePos = CursorPos;
		if (!ScreenToClient(WindowHandle, &RelativePos))
		{
			LOGWARNING(L"ScreenToClient(): {}"sv, last_error());
			return;
		}

		const auto OutputHandle = ::console.GetOutputHandle();

		CONSOLE_FONT_INFO FontInfo;
		if (!get_current_console_font(OutputHandle, FontInfo))
			return;

		CONSOLE_SCREEN_BUFFER_INFO Csbi;
		if (!get_console_screen_buffer_info(OutputHandle, &Csbi))
			return;

		const auto Set = [&](auto Coord, auto Point, auto SmallRect)
		{
			Record.dwMousePosition.*Coord = std::clamp(Csbi.srWindow.*SmallRect + static_cast<int>(RelativePos.*Point) / FontInfo.dwFontSize.*Coord, 0, Csbi.dwSize.*Coord - 1);
		};

		Set(&COORD::X, &POINT::x, &SMALL_RECT::Left);
		Set(&COORD::Y, &POINT::y, &SMALL_RECT::Top);
	}

	static void AdjustMouseEvents(span<INPUT_RECORD> const Buffer, short Delta)
	{
		std::optional<point> Size;

		for (auto& i: Buffer)
		{
			if (i.EventType != MOUSE_EVENT)
				continue;

			if (!Size)
			{
				Size.emplace();
				if (!::console.GetSize(*Size))
					return;
			}

			fix_wheel_coordinates(i.Event.MouseEvent);

			i.Event.MouseEvent.dwMousePosition.Y = std::max(0, i.Event.MouseEvent.dwMousePosition.Y - Delta);
			i.Event.MouseEvent.dwMousePosition.X = std::min(i.Event.MouseEvent.dwMousePosition.X, static_cast<short>(Size->x - 1));
		}
	}

	bool console::PeekInput(span<INPUT_RECORD> const Buffer, size_t& NumberOfEventsRead) const
	{
		DWORD dwNumberOfEventsRead = 0;
		if (!PeekConsoleInput(GetInputHandle(), Buffer.data(), static_cast<DWORD>(Buffer.size()), &dwNumberOfEventsRead))
		{
			LOGERROR(L"PeekConsoleInput(): {}"sv, last_error());
			return false;
		}

		NumberOfEventsRead = dwNumberOfEventsRead;

		if (sWindowMode)
		{
			AdjustMouseEvents({Buffer.data(), NumberOfEventsRead}, GetDelta());
		}
		return true;
	}

	bool console::PeekOneInput(INPUT_RECORD& Record) const
	{
		size_t Read;
		return PeekInput({ &Record, 1 }, Read) && Read == 1;
	}

	bool console::ReadInput(span<INPUT_RECORD> const Buffer, size_t& NumberOfEventsRead) const
	{
		DWORD dwNumberOfEventsRead = 0;
		if (!ReadConsoleInput(GetInputHandle(), Buffer.data(), static_cast<DWORD>(Buffer.size()), &dwNumberOfEventsRead))
		{
			LOGERROR(L"ReadConsoleInput(): {}"sv, last_error());
			return false;
		}

		NumberOfEventsRead = dwNumberOfEventsRead;

		if (sWindowMode)
		{
			AdjustMouseEvents({Buffer.data(), NumberOfEventsRead}, GetDelta());
		}

		return true;
	}

	bool console::ReadOneInput(INPUT_RECORD& Record) const
	{
		size_t Read;
		return ReadInput({ &Record, 1 }, Read) && Read == 1;
	}

	bool console::WriteInput(span<INPUT_RECORD> const Buffer, size_t& NumberOfEventsWritten) const
	{
		if (sWindowMode)
		{
			const auto Delta = GetDelta();

			for (auto& i: Buffer)
			{
				if (i.EventType == MOUSE_EVENT)
				{
					i.Event.MouseEvent.dwMousePosition.Y += Delta;
				}
			}
		}
		DWORD dwNumberOfEventsWritten = 0;
		SCOPE_EXIT{ NumberOfEventsWritten = dwNumberOfEventsWritten; };

		if (!WriteConsoleInput(GetInputHandle(), Buffer.data(), static_cast<DWORD>(Buffer.size()), &dwNumberOfEventsWritten))
		{
			LOGERROR(L"WriteConsoleInput(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	static bool ReadOutputImpl(CHAR_INFO* const Buffer, point const BufferSize, rectangle& ReadRegion)
	{
		auto Rect = make_rect(ReadRegion);
		SCOPE_EXIT{ ReadRegion = Rect; };

		if (!ReadConsoleOutput(::console.GetOutputHandle(), Buffer, make_coord(BufferSize), {}, &Rect))
		{
			LOGERROR(L"ReadConsoleOutput(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::ReadOutput(matrix<FAR_CHAR_INFO>& Buffer, point const BufferCoord, rectangle const& ReadRegionRelative) const
	{
		if (ExternalConsole.Imports.pReadOutput)
		{
			const COORD BufferSize{ static_cast<short>(Buffer.width()), static_cast<short>(Buffer.height()) };
			auto ReadRegion = make_rect(ReadRegionRelative);
			return ExternalConsole.Imports.pReadOutput(Buffer.data(), BufferSize, make_coord(BufferCoord), &ReadRegion) != FALSE;
		}

		const int Delta = sWindowMode? GetDelta() : 0;
		auto ReadRegion = ReadRegionRelative;
		ReadRegion.top += Delta;
		ReadRegion.bottom += Delta;

		const rectangle SubRect
		{
			BufferCoord.x,
			BufferCoord.y,
			BufferCoord.x + ReadRegion.width() - 1,
			BufferCoord.y + ReadRegion.height() - 1
		};

		std::vector<CHAR_INFO> ConsoleBuffer(SubRect.width() * SubRect.height());

		point const BufferSize{ SubRect.width(), SubRect.height() };

		if (BufferSize.x * BufferSize.y * sizeof(CHAR_INFO) > MAXSIZE)
		{
			const auto HeightStep = std::max(MAXSIZE / (BufferSize.x * sizeof(CHAR_INFO)), size_t{ 1 });

			const size_t Height = ReadRegion.bottom - ReadRegion.top + 1;

			for (size_t i = 0; i < Height; i += HeightStep)
			{
				auto PartialReadRegion = ReadRegion;
				PartialReadRegion.top += static_cast<int>(i);
				PartialReadRegion.bottom = std::min(ReadRegion.bottom, static_cast<int>(PartialReadRegion.top + HeightStep - 1));
				point const PartialBufferSize{ BufferSize.x, PartialReadRegion.height() };
				if (!ReadOutputImpl(ConsoleBuffer.data() + i * PartialBufferSize.x, PartialBufferSize, PartialReadRegion))
					return false;
			}
		}
		else
		{
			auto ReadRegionCopy = ReadRegion;
			if (!ReadOutputImpl(ConsoleBuffer.data(), BufferSize, ReadRegionCopy))
				return false;
		}

		auto ConsoleBufferIterator = ConsoleBuffer.cbegin();
		for_submatrix(Buffer, SubRect, [&](FAR_CHAR_INFO& i)
		{
			const auto& Cell = *ConsoleBufferIterator++;
			i = { Cell.Char.UnicodeChar, colors::NtColorToFarColor(Cell.Attributes) };
		});

		return true;
	}

	static constexpr wchar_t vt_color_index(int const Index)
	{
		// NT is RGB, VT is BGR
		constexpr int Table[]
		{
			//BGR     RGB
			0b000, // 000
			0b100, // 001
			0b010, // 010
			0b110, // 011
			0b001, // 100
			0b101, // 101
			0b011, // 110
			0b111, // 111
		};

		return L'0' + Table[Index & 0b111];
	}

	static const struct
	{
		string_view Normal, Intense, ExtendedColour;
		COLORREF FarColor::* Color;
		FARCOLORFLAGS Flags;
	}
	ColorsMapping[]
	{
		{ L"3"sv,  L"9"sv, L"38"sv, &FarColor::ForegroundColor, FCF_FG_INDEX },
		{ L"4"sv, L"10"sv, L"48"sv, &FarColor::BackgroundColor, FCF_BG_INDEX },
	};

	static const struct
	{
		FARCOLORFLAGS Style;
		string_view On, Off;
	}
	StyleMapping[]
	{
		{ FCF_FG_BOLD,         L"1"sv,     L"22"sv },
		{ FCF_FG_ITALIC,       L"3"sv,     L"23"sv },
		{ FCF_FG_UNDERLINE,    L"4"sv,     L"24"sv },
		{ FCF_FG_UNDERLINE2,   L"21"sv,    L"24"sv },
		{ FCF_FG_OVERLINE,     L"53"sv,    L"55"sv },
		{ FCF_FG_STRIKEOUT,    L"9"sv,     L"29"sv },
		{ FCF_FG_FAINT,        L"2"sv,     L"22"sv },
		{ FCF_FG_BLINK,        L"5"sv,     L"25"sv },
		{ FCF_FG_INVERSE,      L"7"sv,     L"27"sv },
		{ FCF_FG_INVISIBLE,    L"8"sv,     L"28"sv },
	};

	static void make_vt_attributes(const FarColor& Attributes, string& Str, std::optional<FarColor> const& LastColor)
	{
		append(Str, CSI ""sv);

		for (const auto& i: ColorsMapping)
		{
			const auto ColorPart = std::invoke(i.Color, Attributes);

			if (Attributes.Flags & i.Flags)
			{
				const auto Index = colors::index_value(ColorPart);
				if (Index < 16)
					append(Str, ColorPart & FOREGROUND_INTENSITY? i.Intense : i.Normal, vt_color_index(Index));
				else
					format_to(Str, FSTR(L"{};5;{}"sv), i.ExtendedColour, Index);
			}
			else
			{
				const union { COLORREF Color; rgba RGBA; } Value { ColorPart };
				format_to(Str, FSTR(L"{};2;{};{};{}"sv), i.ExtendedColour, Value.RGBA.r, Value.RGBA.g, Value.RGBA.b);
			}

			Str += L';';
		}

		Str.pop_back();

		for (const auto& [Style, On, Off]: StyleMapping)
		{
			if (Attributes.Flags & Style)
			{
				if (!LastColor.has_value() || !(LastColor->Flags & Style))
					append(Str, L';', On);
			}
			else
			{
				if (LastColor.has_value() && LastColor->Flags & Style)
					append(Str, L';', Off);
			}
		}

		Str += L'm';
	}

	static void make_vt_sequence(span<FAR_CHAR_INFO> Input, string& Str, std::optional<FarColor>& LastColor)
	{
		const auto CharWidthEnabled = char_width::is_enabled();

		std::optional<wchar_t> LeadingChar;

		for (const auto& [Cell, n]: enumerate(Input))
		{
			if (CharWidthEnabled)
			{
				if (n != Input.size() - 1)
				{
					sanitise_pair(Cell, Input[n + 1]);
				}

				if (Cell.Attributes.Flags & COMMON_LVB_TRAILING_BYTE)
				{
					if (!LeadingChar)
					{
						Cell.Char = bad_char_replacement;
						flags::clear(Cell.Attributes.Flags, COMMON_LVB_TRAILING_BYTE);
					}
					else
WARNING_PUSH()
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80635
WARNING_DISABLE_GCC("-Wmaybe-uninitialized")

						if (Cell.Char == *LeadingChar)
WARNING_POP()
					{
						LeadingChar.reset();
						continue;
					}
				}
				else if (!n && encoding::utf16::is_low_surrogate(Cell.Char))
				{
					Cell.Char = bad_char_replacement;
				}

				LeadingChar.reset();

				if (Cell.Attributes.Flags & COMMON_LVB_LEADING_BYTE)
				{
					if (n == Input.size() - 1)
					{
						flags::clear(Cell.Attributes.Flags, COMMON_LVB_LEADING_BYTE);
						Cell.Char = bad_char_replacement;
					}
					else
					{
						LeadingChar = Cell.Char;
					}
				}
				else if (n == Input.size() - 1 && encoding::utf16::is_high_surrogate(Cell.Char))
				{
					Cell.Char = bad_char_replacement;
				}
			}

			if (!LastColor.has_value() || Cell.Attributes != *LastColor)
			{
				make_vt_attributes(Cell.Attributes, Str, LastColor);
				LastColor = Cell.Attributes;
			}

			if (CharWidthEnabled && Cell.Char == encoding::replace_char && Cell.Attributes.Reserved[0] > std::numeric_limits<wchar_t>::max())
			{
				const auto Pair = encoding::utf16::to_surrogate(Cell.Attributes.Reserved[0]);
				append(Str, Pair.first, Pair.second);

				if (char_width::is_half_width_surrogate_broken())
					append(Str, CSI L"1D"sv); // Yuck
			}
			else
			{
				Str += ReplaceControlCharacter(Cell.Char);
			}
		}
	}

	class console::implementation
	{
	public:
		static bool WriteOutputVT(matrix<FAR_CHAR_INFO>& Buffer, rectangle const SubRect, rectangle const& WriteRegion)
		{
			const auto Out = ::console.GetOutputHandle();

			CONSOLE_SCREEN_BUFFER_INFO csbi;
			if (!get_console_screen_buffer_info(Out, &csbi))
				return false;

			point SavedCursorPosition;
			if (!::console.GetCursorRealPosition(SavedCursorPosition))
				return false;

			CONSOLE_CURSOR_INFO SavedCursorInfo;
			if (!::console.GetCursorInfo(SavedCursorInfo))
				return false;

			// Ideally this should be filtered out earlier
			if (WriteRegion.left > csbi.dwSize.X - 1 || WriteRegion.top > csbi.dwSize.Y - 1)
				return false;


			// Hide cursor
			if (!::console.SetCursorInfo({1}))
				return false;

			// Move the viewport down
			if (!::console.SetCursorRealPosition({0, csbi.dwSize.Y - 1}))
				return false;

			// Set cursor position within the viewport
			if (!::console.SetCursorRealPosition({WriteRegion.left, WriteRegion.top}))
				return false;

			SCOPE_EXIT
			{
				// Move the viewport down
				::console.SetCursorRealPosition({ 0, csbi.dwSize.Y - 1 });
				// Restore cursor position within the viewport
				::console.SetCursorRealPosition(SavedCursorPosition);
				// Restore cursor
				::console.SetCursorInfo(SavedCursorInfo);
				// Restore buffer relative position
				if (csbi.srWindow.Left || csbi.srWindow.Bottom != csbi.dwSize.Y - 1)
					::console.SetWindowRect(csbi.srWindow);
			};

			point CursorPosition{ WriteRegion.left, WriteRegion.top };

			if (sWindowMode)
			{
				CursorPosition.y -= ::GetDelta(csbi);

				if (CursorPosition.y < 0)
				{
					// Drawing above the viewport
					CursorPosition.y = 0;
				}
			}

			string Str;

			// The idea is to reduce the number of reallocations,
			// but only if it's not a single / double cell update
			if (const auto Area = SubRect.width() * SubRect.height(); Area > 4)
				Str.reserve(std::max(1024, Area * 2));

			std::optional<FarColor> LastColor;

			point ViewportSize;
			if (!::console.GetSize(ViewportSize))
				return false;

			// If SubRect is too tall (e.g. when we flushing the old content of console resize), the rest will be dropped.
			// VT is a bloody joke.
			for (int SubrectOffset = 0; SubrectOffset < SubRect.height(); SubrectOffset += ViewportSize.y)
			{
				if (SubrectOffset)
				{
					// Move the viewport down
					if (!::console.SetCursorRealPosition({0, csbi.dwSize.Y - 1}))
						return false;
					// Set cursor position within the viewport
					if (!::console.SetCursorRealPosition({ WriteRegion.left, WriteRegion.top + SubrectOffset }))
						return false;
				}

				for (const auto& i: irange(SubRect.top + SubrectOffset, std::min(SubRect.top + SubrectOffset + ViewportSize.y, SubRect.bottom + 1)))
				{
					if (i != SubRect.top)
						format_to(Str, FSTR(CSI L"{};{}H"sv), CursorPosition.y + 1 + (i - SubrectOffset - SubRect.top), CursorPosition.x + 1);

					make_vt_sequence(Buffer[i].subspan(SubRect.left, SubRect.width()), Str, LastColor);
				}

				if (!::console.Write(Str))
					return false;

				Str.clear();

			}

			return ::console.Write(CSI L"0m"sv);
		}

		class cursor_suppressor
		{
		public:
			NONCOPYABLE(cursor_suppressor);

			cursor_suppressor()
			{
				CONSOLE_CURSOR_INFO Info;
				if (!::console.GetCursorInfo(Info))
					return;

				if (!::console.SetCursorInfo({ 1 }))
					return;

				m_Info = Info;

				point Position;
				if (!::console.GetCursorRealPosition(Position))
					return;

				if (!::console.SetCursorPosition({}))
					return;

				m_Position = Position;
			}

			~cursor_suppressor()
			{
				if (m_Position)
					::console.SetCursorRealPosition(*m_Position);

				if (m_Info)
					::console.SetCursorInfo(*m_Info);
			}

		private:
			std::optional<point> m_Position;
			std::optional<CONSOLE_CURSOR_INFO> m_Info;
		};

		static bool WriteOutputNTImpl(CHAR_INFO const* const Buffer, point const BufferSize, rectangle const& WriteRegion)
		{
			// https://github.com/microsoft/terminal/issues/10456
			// It looks like only a specific range of Windows 10 versions is affected.
			static const auto IsCursorPositionWorkaroundNeeded = os::version::is_win10_build_or_later(19041) && !os::version::is_win10_build_or_later(21277);

			std::optional<cursor_suppressor> CursorSuppressor;
			if (IsCursorPositionWorkaroundNeeded && char_width::is_enabled())
				CursorSuppressor.emplace();

			auto SysWriteRegion = make_rect(WriteRegion);
			if (!WriteConsoleOutput(::console.GetOutputHandle(), Buffer, make_coord(BufferSize), {}, &SysWriteRegion))
			{
				LOGERROR(L"WriteConsoleOutput(): {}"sv, last_error());
				return false;
			}

			return true;
		}

		static bool WriteOutputNTImplDebug(CHAR_INFO* const Buffer, point const BufferSize, rectangle const& WriteRegion)
		{
			if constexpr ((false))
			{
				assert(BufferSize.x == WriteRegion.width());
				assert(BufferSize.y == WriteRegion.height());

				const auto invert_colors = [&]
				{
					for (auto& i: span(Buffer, BufferSize.x* BufferSize.y))
						i.Attributes = (i.Attributes & FCF_RAWATTR_MASK) | extract_integer<BYTE, 0>(~i.Attributes);
				};

				invert_colors();

				WriteOutputNTImpl(Buffer, BufferSize, WriteRegion);
				Sleep(50);

				invert_colors();
			}

			return WriteOutputNTImpl(Buffer, BufferSize, WriteRegion) != FALSE;
		}

		static bool WriteOutputNT(matrix<FAR_CHAR_INFO>& Buffer, rectangle const SubRect, rectangle const& WriteRegion)
		{
			std::vector<CHAR_INFO> ConsoleBuffer;
			ConsoleBuffer.reserve(SubRect.width() * SubRect.height());

			if (char_width::is_enabled())
			{
				for_submatrix(Buffer, SubRect, [&](FAR_CHAR_INFO& Cell, point const Point)
				{
					if (!Point.x)
					{
						if (Cell.Attributes.Flags & COMMON_LVB_TRAILING_BYTE)
						{
							flags::clear(Cell.Attributes.Flags, COMMON_LVB_TRAILING_BYTE);
							Cell.Char = bad_char_replacement;
						}
						else if (encoding::utf16::is_low_surrogate(Cell.Char))
						{
							Cell.Char = bad_char_replacement;
						}
					}

					if (Point.x != SubRect.width() - 1)
					{
						sanitise_pair(Cell, Buffer[SubRect.top + Point.y][SubRect.left + Point.x + 1]);
					}
					else
					{
						if (Cell.Attributes.Flags & COMMON_LVB_LEADING_BYTE)
						{
							flags::clear(Cell.Attributes.Flags, COMMON_LVB_LEADING_BYTE);
							Cell.Char = bad_char_replacement;
						}
						else if (encoding::utf16::is_high_surrogate(Cell.Char))
						{
							Cell.Char = bad_char_replacement;
						}
					}

					ConsoleBuffer.emplace_back(CHAR_INFO{ { ReplaceControlCharacter(Cell.Char) }, colors::FarColorToConsoleColor(Cell.Attributes) });
				});
			}
			else
			{
				for_submatrix(Buffer, SubRect, [&](const FAR_CHAR_INFO& i)
				{
					ConsoleBuffer.emplace_back(CHAR_INFO{ { ReplaceControlCharacter(i.Char) }, colors::FarColorToConsoleColor(i.Attributes) });
				});
			}

			point const BufferSize{ SubRect.width(), SubRect.height() };

			if (BufferSize.x * BufferSize.y * sizeof(CHAR_INFO) > MAXSIZE)
			{
				const auto HeightStep = std::max(MAXSIZE / (BufferSize.x * sizeof(CHAR_INFO)), size_t{ 1 });

				for (size_t i = 0, Height = WriteRegion.height(); i < Height; i += HeightStep)
				{
					rectangle const PartialWriteRegion
					{
						WriteRegion.left,
						WriteRegion.top + static_cast<int>(i),
						WriteRegion.right,
						std::min(WriteRegion.bottom, static_cast<int>(WriteRegion.top + static_cast<int>(i) + HeightStep - 1))
					};

					point const PartialBufferSize
					{
						BufferSize.x,
						PartialWriteRegion.height()
					};

					if (!WriteOutputNTImplDebug(ConsoleBuffer.data() + i * PartialBufferSize.x, PartialBufferSize, PartialWriteRegion))
						return false;
				}
			}
			else
			{
				if (!WriteOutputNTImplDebug(ConsoleBuffer.data(), BufferSize, WriteRegion))
					return false;
			}

			return true;
		}

		static bool SetTextAttributesVT(const FarColor& Attributes)
		{
			// For fallback
			SetTextAttributesNT(Attributes);

			string Str;
			make_vt_attributes(Attributes, Str, {});
			return ::console.Write(Str);
		}

		static bool SetTextAttributesNT(const FarColor& Attributes)
		{
			if (!SetConsoleTextAttribute(::console.GetOutputHandle(), colors::FarColorToConsoleColor(Attributes)))
			{
				LOGERROR(L"SetConsoleTextAttribute(): {}"sv, last_error());
				return false;
			}

			return true;
		}
	};

	bool console::WriteOutput(matrix<FAR_CHAR_INFO>& Buffer, point BufferCoord, const rectangle& WriteRegionRelative) const
	{
		if (ExternalConsole.Imports.pWriteOutput)
		{
			const COORD BufferSize{ static_cast<short>(Buffer.width()), static_cast<short>(Buffer.height()) };
			auto WriteRegion = make_rect(WriteRegionRelative);
			return ExternalConsole.Imports.pWriteOutput(Buffer.data(), BufferSize, make_coord(BufferCoord), &WriteRegion) != FALSE;
		}

		const int Delta = sWindowMode? GetDelta() : 0;
		auto WriteRegion = WriteRegionRelative;
		WriteRegion.top += Delta;
		WriteRegion.bottom += Delta;

		const rectangle SubRect
		{
			BufferCoord.x,
			BufferCoord.y,
			BufferCoord.x + WriteRegion.width() - 1,
			BufferCoord.y + WriteRegion.height() - 1
		};

		return (IsVtEnabled()? implementation::WriteOutputVT : implementation::WriteOutputNT)(Buffer, SubRect, WriteRegion);
	}

	bool console::Read(string& Buffer, size_t& Size) const
	{
		const auto InputHandle = GetInputHandle();

		DWORD NumberOfCharsRead;

		DWORD Mode;
		if (GetMode(InputHandle, Mode))
		{
			if (!ReadConsole(InputHandle, Buffer.data(), static_cast<DWORD>(Buffer.size()), &NumberOfCharsRead, nullptr))
			{
				LOGERROR(L"ReadConsole(): {}"sv, last_error());
				return false;
			}
		}
		else
		{
			if (!ReadFile(InputHandle, Buffer.data(), static_cast<DWORD>(Buffer.size() * sizeof(wchar_t)), &NumberOfCharsRead, nullptr))
			{
				LOGERROR(L"ReadFile(): {}"sv, last_error());
				return false;
			}

			NumberOfCharsRead /= sizeof(wchar_t);
		}

		Size = NumberOfCharsRead;
		return true;
	}

	bool console::Write(const string_view Str) const
	{
		DWORD NumberOfCharsWritten;
		const auto OutputHandle = GetOutputHandle();

		DWORD Mode;
		if (GetMode(OutputHandle, Mode))
		{
			if (!WriteConsole(OutputHandle, Str.data(), static_cast<DWORD>(Str.size()), &NumberOfCharsWritten, nullptr))
			{
				LOGERROR(L"WriteConsole(): {}"sv, last_error());
				return false;
			}

			return true;
		}

		// Redirected output

		if (m_FileHandle == -1)
		{
			HANDLE OsHandle;
			if (!DuplicateHandle(GetCurrentProcess(), OutputHandle, GetCurrentProcess(), &OsHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
				return false;

			m_FileHandle = _open_osfhandle(reinterpret_cast<intptr_t>(OsHandle), _O_U8TEXT);
			if (m_FileHandle == -1)
				return false;

			_setmode(m_FileHandle, _O_U8TEXT);
		}

		return _write(m_FileHandle, Str.data(), static_cast<unsigned int>(Str.size() * sizeof(wchar_t))) != -1;
	}

	bool console::Commit() const
	{
		if (ExternalConsole.Imports.pCommit)
			return ExternalConsole.Imports.pCommit() != FALSE;

		// reserved
		return true;
	}

	bool console::GetTextAttributes(FarColor& Attributes) const
	{
		if (ExternalConsole.Imports.pGetTextAttributes)
			return ExternalConsole.Imports.pGetTextAttributes(&Attributes) != FALSE;

		CONSOLE_SCREEN_BUFFER_INFO ConsoleScreenBufferInfo;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &ConsoleScreenBufferInfo))
			return false;

		Attributes = colors::NtColorToFarColor(ConsoleScreenBufferInfo.wAttributes);
		return true;
	}

	bool console::SetTextAttributes(const FarColor& Attributes) const
	{
		if (ExternalConsole.Imports.pSetTextAttributes)
			return ExternalConsole.Imports.pSetTextAttributes(&Attributes) != FALSE;

		return (IsVtEnabled()? implementation::SetTextAttributesVT : implementation::SetTextAttributesNT)(Attributes);
	}

	bool console::GetCursorInfo(CONSOLE_CURSOR_INFO& ConsoleCursorInfo) const
	{
		if (!GetConsoleCursorInfo(GetOutputHandle(), &ConsoleCursorInfo))
		{
			LOGERROR(L"GetConsoleCursorInfo(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::SetCursorInfo(const CONSOLE_CURSOR_INFO& ConsoleCursorInfo) const
	{
		if (!SetConsoleCursorInfo(GetOutputHandle(), &ConsoleCursorInfo))
		{
			LOGERROR(L"SetConsoleCursorInfo(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	bool console::GetCursorPosition(point& Position) const
	{
		if (!GetCursorRealPosition(Position))
			return false;

		if (sWindowMode)
			Position.y -= GetDelta();

		return true;
	}

	bool console::SetCursorPosition(point Position) const
	{
		if (sWindowMode)
		{
			point Size{};
			GetSize(Size);
			Position.x = std::min(Position.x, Size.x - 1);
			Position.y = std::max(0, Position.y);
			Position.y += GetDelta();
		}
		return SetCursorRealPosition(Position);
	}

	bool console::FlushInputBuffer() const
	{
		return FlushConsoleInputBuffer(GetInputHandle()) != FALSE;
	}

	bool console::GetNumberOfInputEvents(size_t& NumberOfEvents) const
	{
		DWORD dwNumberOfEvents = 0;
		const auto Result = GetNumberOfConsoleInputEvents(GetInputHandle(), &dwNumberOfEvents) != FALSE;
		NumberOfEvents = dwNumberOfEvents;
		return Result;
	}

	bool console::GetAlias(string_view const Name, string& Value, string_view const ExeName) const
	{
		os::last_error_guard Guard;

		null_terminated const C_Name(Name), C_ExeName(ExeName);

		return os::detail::ApiDynamicErrorBasedStringReceiver(ERROR_INSUFFICIENT_BUFFER, Value, [&](span<wchar_t> Buffer)
		{
			// This API design is mental:
			// - If everything is ok, it return the string size, including the terminating \0
			// - If the buffer size is too small, it returns the input buffer size and sets last error to ERROR_INSUFFICIENT_BUFFER
			// - It can also return 0 in case of other errors
			// This means that if the string size is exactly (BufferSize - 1), the only way to understand whether it succeeded or not
			// is to look at the error code. And it doesn't reset it on success, so have to do it ourselves to be sure. *facepalm*
			SetLastError(ERROR_SUCCESS);
			const auto BufferSizeInBytes = Buffer.size() * sizeof(wchar_t);
			const size_t ReturnedSizeInBytes = GetConsoleAlias(
				const_cast<wchar_t*>(C_Name.c_str()),
				Buffer.data(),
				static_cast<DWORD>(BufferSizeInBytes),
				const_cast<wchar_t*>(C_ExeName.c_str())
			);

			if (!ReturnedSizeInBytes || (ReturnedSizeInBytes == BufferSizeInBytes && GetLastError() == ERROR_INSUFFICIENT_BUFFER))
				return size_t{};

			return ReturnedSizeInBytes / sizeof(wchar_t) - 1;
		});
	}

	console::console_aliases::console_aliases() = default;
	console::console_aliases::~console_aliases() = default;

	struct console::console_aliases::data
	{
		// We only use it to bulk copy the aliases from one console to another,
		// so no need to care about case insensitivity and fancy lookup.
		std::vector<std::pair<string, std::vector<std::pair<string, string>>>> Aliases;
	};

	console::console_aliases console::GetAllAliases() const
	{
		const auto ExeLength = GetConsoleAliasExesLength();
		if (!ExeLength)
			return {};

		std::vector<wchar_t> ExeBuffer(ExeLength / sizeof(wchar_t) + 1); // +1 for double \0
		if (!GetConsoleAliasExes(ExeBuffer.data(), ExeLength))
		{
			LOGERROR(L"GetConsoleAliasExes(): {}"sv, last_error());
			return {};
		}

		auto Aliases = std::make_unique<console_aliases::data>();

		std::vector<wchar_t> AliasesBuffer;
		for (const auto& ExeToken: enum_substrings(ExeBuffer))
		{
			// It's ok, ExeToken is guaranteed to be null-terminated
			const auto ExeNamePtr = const_cast<wchar_t*>(ExeToken.data());
			const auto AliasesLength = GetConsoleAliasesLength(ExeNamePtr);
			AliasesBuffer.resize(AliasesLength / sizeof(wchar_t) + 1); // +1 for double \0
			if (!GetConsoleAliases(AliasesBuffer.data(), AliasesLength, ExeNamePtr))
			{
				LOGERROR(L"GetConsoleAliases(): {}"sv, last_error());
				continue;
			}

			std::pair<string, std::vector<std::pair<string, string>>> ExeData;
			ExeData.first = ExeNamePtr;
			for (const auto& AliasToken: enum_substrings(AliasesBuffer))
			{
				ExeData.second.emplace_back(split(AliasToken));
			}

			Aliases->Aliases.emplace_back(std::move(ExeData));
		}

		console_aliases Result;
		Result.m_Data = std::move(Aliases);

		return Result;
	}

	void console::SetAllAliases(console_aliases&& Aliases) const
	{
		if (!Aliases.m_Data)
			return;

		for (const auto& [ExeName, ExeAliases]: Aliases.m_Data->Aliases)
		{
			for (const auto& [Alias, Value]: ExeAliases)
			{
				AddConsoleAlias(
					const_cast<wchar_t*>(Alias.c_str()),
					const_cast<wchar_t*>(Value.c_str()),
					const_cast<wchar_t*>(ExeName.c_str())
				);
			}
		}
	}

	bool console::GetDisplayMode(DWORD& Mode) const
	{
		if (!GetConsoleDisplayMode(&Mode))
		{
			LOGERROR(L"GetConsoleDisplayMode(): {}"sv, last_error());
			return false;
		}

		return true;
	}

	point console::GetLargestWindowSize() const
	{
		point Result = GetLargestConsoleWindowSize(GetOutputHandle());
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (get_console_screen_buffer_info(GetOutputHandle(), &csbi) && csbi.dwSize.Y > Result.y)
		{
			CONSOLE_FONT_INFO FontInfo;
			if (get_current_console_font(GetOutputHandle(), FontInfo))
			{
				Result.x -= Round(GetSystemMetrics(SM_CXVSCROLL), static_cast<int>(FontInfo.dwFontSize.X));
			}
		}
		return Result;
	}

	bool console::SetActiveScreenBuffer(HANDLE ConsoleOutput)
	{
		if (!SetConsoleActiveScreenBuffer(ConsoleOutput))
		{
			LOGERROR(L"SetConsoleActiveScreenBuffer(): {}"sv, last_error());
			return false;
		}

		m_ActiveConsoleScreenBuffer = ConsoleOutput;
		return true;
	}

	HANDLE console::GetActiveScreenBuffer() const
	{
		return m_ActiveConsoleScreenBuffer;
	}

	bool console::ClearExtraRegions(const FarColor& Color, int Mode) const
	{
		if (ExternalConsole.Imports.pClearExtraRegions)
			return ExternalConsole.Imports.pClearExtraRegions(&Color, Mode) != FALSE;

		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return false;

		DWORD CharsWritten;
		const auto ConColor = colors::FarColorToConsoleColor(Color);

		if (Mode&CR_TOP)
		{
			const DWORD TopSize = csbi.dwSize.X * csbi.srWindow.Top;
			const COORD TopCoord{};
			FillConsoleOutputCharacter(GetOutputHandle(), L' ', TopSize, TopCoord, &CharsWritten);
			FillConsoleOutputAttribute(GetOutputHandle(), ConColor, TopSize, TopCoord, &CharsWritten);
		}

		if (Mode&CR_RIGHT)
		{
			const DWORD RightSize = csbi.dwSize.X - csbi.srWindow.Right;
			COORD RightCoord{ csbi.srWindow.Right, ::GetDelta(csbi) };
			for (; RightCoord.Y < csbi.dwSize.Y; RightCoord.Y++)
			{
				FillConsoleOutputCharacter(GetOutputHandle(), L' ', RightSize, RightCoord, &CharsWritten);
				FillConsoleOutputAttribute(GetOutputHandle(), ConColor, RightSize, RightCoord, &CharsWritten);
			}
		}
		return true;
	}

	bool console::ScrollWindow(int Lines, int Columns) const
	{
		bool process = false;
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return false;

		if ((Lines < 0 && csbi.srWindow.Top) || (Lines > 0 && csbi.srWindow.Bottom != csbi.dwSize.Y - 1))
		{
			csbi.srWindow.Top += Lines;
			csbi.srWindow.Bottom += Lines;

			if (csbi.srWindow.Top < 0)
			{
				csbi.srWindow.Bottom -= csbi.srWindow.Top;
				csbi.srWindow.Top = 0;
			}

			if (csbi.srWindow.Bottom >= csbi.dwSize.Y)
			{
				csbi.srWindow.Top -= (csbi.srWindow.Bottom - (csbi.dwSize.Y - 1));
				csbi.srWindow.Bottom = csbi.dwSize.Y - 1;
			}
			process = true;
		}

		if ((Columns < 0 && csbi.srWindow.Left) || (Columns > 0 && csbi.srWindow.Right != csbi.dwSize.X - 1))
		{
			csbi.srWindow.Left += Columns;
			csbi.srWindow.Right += Columns;

			if (csbi.srWindow.Left < 0)
			{
				csbi.srWindow.Right -= csbi.srWindow.Left;
				csbi.srWindow.Left = 0;
			}

			if (csbi.srWindow.Right >= csbi.dwSize.X)
			{
				csbi.srWindow.Left -= (csbi.srWindow.Right - (csbi.dwSize.X - 1));
				csbi.srWindow.Right = csbi.dwSize.X - 1;
			}
			process = true;
		}

		return process && SetWindowRect(csbi.srWindow);
	}

	bool console::ScrollWindowToBegin() const
	{
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return false;

		if (!csbi.srWindow.Top)
			return false;

		csbi.srWindow.Bottom -= csbi.srWindow.Top;
		csbi.srWindow.Top = 0;
		return SetWindowRect(csbi.srWindow);
	}

	bool console::ScrollWindowToEnd() const
	{
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))

		if (csbi.srWindow.Bottom == csbi.dwSize.Y - 1)
			return false;

		csbi.srWindow.Top += csbi.dwSize.Y - 1 - csbi.srWindow.Bottom;
		csbi.srWindow.Bottom = csbi.dwSize.Y - 1;
		return SetWindowRect(csbi.srWindow);
	}

	bool console::IsFullscreenSupported() const
	{
#ifdef _WIN64
		return false;
#else
		if (!imports.GetConsoleScreenBufferInfoEx)
			return true;

		CONSOLE_SCREEN_BUFFER_INFOEX csbiex{ sizeof(csbiex) };
		if (!imports.GetConsoleScreenBufferInfoEx(GetOutputHandle(), &csbiex))
		{
			LOGWARNING(L"GetConsoleScreenBufferInfoEx(): {}"sv, last_error());
			return true;
		}

		return csbiex.bFullscreenSupported != FALSE;
#endif
	}

	void console::ResetPosition() const
	{
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return;

		if (!csbi.srWindow.Left && csbi.srWindow.Bottom == csbi.dwSize.Y - 1)
			return;

		csbi.srWindow.Right -= csbi.srWindow.Left;
		csbi.srWindow.Left = 0;
		csbi.srWindow.Top += csbi.dwSize.Y - 1 - csbi.srWindow.Bottom;
		csbi.srWindow.Bottom = csbi.dwSize.Y - 1;
		SetWindowRect(csbi.srWindow);
	}

	bool console::ResetViewportPosition() const
	{
		rectangle WindowRect;
		return
			GetWindowRect(WindowRect) &&
			SetCursorPosition({}) &&
			SetCursorPosition({ 0, WindowRect.height() - 1 });
	}

	short console::GetDelta() const
	{
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return 0;

		return ::GetDelta(csbi);
	}

	bool console::ScrollScreenBuffer(rectangle const& ScrollRectangle, point DestinationOrigin, const FAR_CHAR_INFO& Fill) const
	{
		const CHAR_INFO SysFill{ { Fill.Char }, colors::FarColorToConsoleColor(Fill.Attributes) };
		const auto SysScrollRect = make_rect(ScrollRectangle);
		return ScrollConsoleScreenBuffer(GetOutputHandle(), &SysScrollRect, {}, make_coord(DestinationOrigin), &SysFill) != FALSE;
	}

	bool console::ScrollNonClientArea(size_t NumLines, const FAR_CHAR_INFO& Fill) const
	{
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return false;

		const auto Scroll = [&](rectangle const& Rect)
		{
			return ScrollScreenBuffer(
				Rect,
				{
					Rect.left,
					static_cast<int>(Rect.top - NumLines)
				},
				Fill);
		};


		rectangle const TopRectangle
		{
			0,
			0,
			csbi.dwSize.X - 1,
			csbi.dwSize.Y - 1 - (ScrY + 1)
		};

		if (TopRectangle.bottom >= TopRectangle.top && !Scroll(TopRectangle))
			return false;

		rectangle const RightRectangle
		{
			ScrX + 1,
			TopRectangle.bottom + 1,
			csbi.dwSize.X - 1,
			csbi.dwSize.Y - 1
		};

		if (RightRectangle.right >= RightRectangle.left && !Scroll(RightRectangle))
			return false;

		return true;
	}

	bool console::IsViewportVisible() const
	{
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return false;

		const auto Height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
		const auto Width = csbi.srWindow.Right - csbi.srWindow.Left + 1;

		return csbi.srWindow.Bottom >= csbi.dwSize.Y - Height && csbi.srWindow.Left < Width;
	}

	bool console::IsViewportShifted() const
	{
		if (!sWindowMode)
			return false;

		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return false;

		return csbi.srWindow.Left || csbi.srWindow.Bottom + 1 != csbi.dwSize.Y;
	}

	bool console::IsPositionVisible(point const Position) const
	{
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &csbi))
			return false;

		if (!in_closed_range(csbi.srWindow.Left, Position.x, csbi.srWindow.Right))
			return false;

		const auto RealY = Position.y + (sWindowMode? ::GetDelta(csbi) : 0);
		return in_closed_range(csbi.srWindow.Top, RealY, csbi.srWindow.Bottom);
	}

	bool console::IsScrollbackPresent() const
	{
		return GetDelta() != 0;
	}

	bool console::IsVtEnabled() const
	{
		DWORD Mode;
		return sEnableVirtualTerminal && GetMode(GetOutputHandle(), Mode) && Mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	}

	bool console::ExternalRendererLoaded() const
	{
		return ExternalConsole.Imports.pWriteOutput.operator bool();
	}

	bool console::IsWidePreciseExpensive(unsigned int const Codepoint, bool const ClearCacheOnly)
	{
		// It ain't stupid if it works

		if (ClearCacheOnly)
		{
			m_WidthTestScreen = {};
			return false;
		}

		if (!m_WidthTestScreen)
		{
			m_WidthTestScreen.reset(CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, {}, {}, CONSOLE_TEXTMODE_BUFFER, {}));

			const auto TestScreenX = 20, TestScreenY = 1;
			const SMALL_RECT WindowInfo{ 0, 0, TestScreenX - 1, TestScreenY - 1 };
			if (!SetConsoleWindowInfo(m_WidthTestScreen.native_handle(), true, &WindowInfo))
			{
				LOGWARNING(L"SetConsoleWindowInfo(): {}"sv, last_error());
			}

			if (!SetConsoleScreenBufferSize(m_WidthTestScreen.native_handle(), { TestScreenX, TestScreenY }))
			{
				LOGWARNING(L"SetConsoleScreenBufferSize(): {}"sv, last_error());
			}
		}

		if (!SetConsoleCursorPosition(m_WidthTestScreen.native_handle(), {}))
		{
			LOGWARNING(L"SetConsoleCursorPosition(): {}"sv, last_error());
			return false;
		}

		DWORD Written;
		const auto Pair = encoding::utf16::to_surrogate(Codepoint);
		const std::array Chars{ Pair.first, Pair.second };
		if (!WriteConsole(m_WidthTestScreen.native_handle(), Chars.data(), Pair.second? 2 : 1, &Written, {}))
		{
			LOGWARNING(L"WriteConsole(): {}"sv, last_error());
			return false;
		}

		CONSOLE_SCREEN_BUFFER_INFO Info;
		if (!get_console_screen_buffer_info(m_WidthTestScreen.native_handle(), &Info))
			return false;

		return Info.dwCursorPosition.X > 1;
	}

	bool console::GetPalette(std::array<COLORREF, 16>& Palette) const
	{
		if (!imports.GetConsoleScreenBufferInfoEx)
			return false;

		CONSOLE_SCREEN_BUFFER_INFOEX csbi{ sizeof(csbi) };
		if (!imports.GetConsoleScreenBufferInfoEx(GetOutputHandle(), &csbi))
		{
			LOGERROR(L"GetConsoleScreenBufferInfoEx(): {}"sv, last_error());
			return false;
		}

		std::copy(ALL_CONST_RANGE(csbi.ColorTable), Palette.begin());

		return true;
	}

	void console::EnableWindowMode(bool const Value)
	{
		sWindowMode = Value;
	}

	void console::EnableVirtualTerminal(bool const Value)
	{
		sEnableVirtualTerminal = Value;
	}

	bool console::GetCursorRealPosition(point& Position) const
	{
		CONSOLE_SCREEN_BUFFER_INFO ConsoleScreenBufferInfo;
		if (!get_console_screen_buffer_info(GetOutputHandle(), &ConsoleScreenBufferInfo))
			return false;

		Position = ConsoleScreenBufferInfo.dwCursorPosition;
		return true;
	}

	bool console::SetCursorRealPosition(point const Position) const
	{
		if (!SetConsoleCursorPosition(GetOutputHandle(), make_coord(Position)))
		{
			LOGERROR(L"SetConsoleCursorPosition(): {}"sv, last_error());
			return false;
		}

		return true;
	}
}

NIFTY_DEFINE(console_detail::console, console);

enum
{
	BufferSize = 8192
};

class consolebuf final: public std::wstreambuf
{
public:
	NONCOPYABLE(consolebuf);

	consolebuf():
		m_InBuffer(BufferSize, {}),
		m_OutBuffer(BufferSize, {})
	{
		setg(m_InBuffer.data(), m_InBuffer.data() + m_InBuffer.size(), m_InBuffer.data() + m_InBuffer.size());
		setp(m_OutBuffer.data(), m_OutBuffer.data() + m_OutBuffer.size());
	}

	void color(const FarColor& Color)
	{
		m_Colour = Color;
	}

protected:
	int_type underflow() override
	{
		size_t Read;
		if (!console.Read(m_InBuffer, Read))
			throw MAKE_FAR_FATAL_EXCEPTION(L"Console read error"sv);

		if (!Read)
			return traits_type::eof();

		setg(m_InBuffer.data(), m_InBuffer.data(), m_InBuffer.data() + Read);
		return m_InBuffer[0];
	}

	int_type overflow(int_type Ch) override
	{
		if (!Write({ pbase(), static_cast<size_t>(pptr() - pbase()) }))
			return traits_type::eof();

		setp(m_OutBuffer.data(), m_OutBuffer.data() + m_OutBuffer.size());

		if (traits_type::eq_int_type(Ch, traits_type::eof()))
		{
			console.Commit();
		}
		else
		{
			sputc(Ch);
		}

		return 0;
	}

	int sync() override
	{
		overflow(traits_type::eof());
		return 0;
	}

private:
	bool Write(string_view Str)
	{
		if (Str.empty())
			return true;

		FarColor CurrentColor;
		const auto ChangeColour = m_Colour && console.GetTextAttributes(CurrentColor);

		if (ChangeColour)
		{
			console.SetTextAttributes(colors::merge(CurrentColor, *m_Colour));
		}

		SCOPE_EXIT{ if (ChangeColour) console.SetTextAttributes(CurrentColor); };

		return console.Write(Str);
	}

	string m_InBuffer, m_OutBuffer;
	std::optional<FarColor> m_Colour;
};

class console_detail::console::stream_buffers_overrider
{
public:
	NONCOPYABLE(stream_buffers_overrider);

	stream_buffers_overrider():
		m_In(std::wcin, m_BufIn),
		m_Out(std::wcout, m_BufOut),
		m_Err(std::wcerr, m_BufErr),
		m_Log(std::wclog, m_BufLog)
	{
		auto Color = colors::NtColorToFarColor(F_LIGHTRED);
		colors::make_transparent(Color.BackgroundColor);
		m_BufErr.color(Color);
	}

private:
	consolebuf m_BufIn, m_BufOut, m_BufErr, m_BufLog;
	io::wstreambuf_override m_In, m_Out, m_Err, m_Log;
};
