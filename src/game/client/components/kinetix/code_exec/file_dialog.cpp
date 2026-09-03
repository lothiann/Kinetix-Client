#include <base/detect.h>
#include <base/mem.h>

#include <game/client/components/kinetix/code_exec/file_dialog.h>

#if defined(CONF_FAMILY_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma push_macro("NOGDI")
#undef NOGDI
#include <windows.h>
#include <commdlg.h>
#pragma pop_macro("NOGDI")

bool KxPickScriptFile(char *pBuf, int BufSize)
{
	wchar_t aWidePath[1024];
	aWidePath[0] = L'\0';

	wchar_t aWideCwd[1024];
	const DWORD CurDirLen = GetCurrentDirectoryW(1024, aWideCwd);

	wchar_t aWideScripts[1024];
	LPCWSTR pInitialDir = NULL;
	if(CurDirLen > 0 && CurDirLen < 880)
	{
		DWORD Len = CurDirLen;
		if(aWideCwd[Len - 1] != L'\\' && aWideCwd[Len - 1] != L'/')
			aWideScripts[Len++] = L'\\';
		lstrcpyW(aWideScripts + Len, L"data\\scripts");
		const DWORD Attr = GetFileAttributesW(aWideScripts);
		if(Attr != INVALID_FILE_ATTRIBUTES && (Attr & FILE_ATTRIBUTE_DIRECTORY))
			pInitialDir = aWideScripts;
	}

	OPENFILENAMEW Ofn;
	mem_zero(&Ofn, sizeof(Ofn));
	Ofn.lStructSize = sizeof(Ofn);
	Ofn.hwndOwner = GetActiveWindow();
	Ofn.lpstrFilter = L"Lua scripts (*.lua, *.txt)\0*.lua;*.txt\0All files (*.*)\0*.*\0";
	Ofn.lpstrFile = aWidePath;
	Ofn.nMaxFile = 1024;
	Ofn.lpstrInitialDir = pInitialDir;
	Ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

	const BOOL Picked = GetOpenFileNameW(&Ofn);

	if(CurDirLen > 0 && CurDirLen < 1024)
		SetCurrentDirectoryW(aWideCwd);

	if(!Picked)
		return false;

	return WideCharToMultiByte(CP_UTF8, 0, aWidePath, -1, pBuf, BufSize, NULL, NULL) > 0;
}
#else
bool KxPickScriptFile(char *pBuf, int BufSize)
{
	(void)pBuf;
	(void)BufSize;
	return false;
}
#endif
