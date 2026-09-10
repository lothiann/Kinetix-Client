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

bool KxPickTasFile(char *pBuf, int BufSize)
{
	wchar_t aWidePath[1024];
	aWidePath[0] = L'\0';

	wchar_t aWideCwd[1024];
	const DWORD CurDirLen = GetCurrentDirectoryW(1024, aWideCwd);

	wchar_t aWideTas[1024];
	LPCWSTR pInitialDir = NULL;
	if(CurDirLen > 0 && CurDirLen < 880)
	{
		DWORD Len = CurDirLen;
		if(aWideCwd[Len - 1] != L'\\' && aWideCwd[Len - 1] != L'/')
			aWideTas[Len++] = L'\\';
		lstrcpyW(aWideTas + Len, L"data\\tas");
		const DWORD Attr = GetFileAttributesW(aWideTas);
		if(Attr != INVALID_FILE_ATTRIBUTES && (Attr & FILE_ATTRIBUTE_DIRECTORY))
			pInitialDir = aWideTas;
	}

	OPENFILENAMEW Ofn;
	mem_zero(&Ofn, sizeof(Ofn));
	Ofn.lStructSize = sizeof(Ofn);
	Ofn.hwndOwner = GetActiveWindow();
	Ofn.lpstrFilter = L"TAS files (*.tas)\0*.tas\0All files (*.*)\0*.*\0";
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
#elif defined(CONF_PLATFORM_ANDROID)

#include <base/str.h>
#include <base/types.h>

#include <SDL.h>
#include <jni.h>

#include <mutex>

static std::mutex s_PickLock;
static int s_PickResultKind = -1;
static char s_aPickResultPath[IO_MAX_PATH_LENGTH] = {};

constexpr uint32_t COMMAND_USER = 0x8000;
constexpr uint32_t COMMAND_PICK_FILE = COMMAND_USER + 2;

extern "C" JNIEXPORT void JNICALL Java_org_ddnet_client_ClientActivity_nativeOnFilePicked(JNIEnv *pEnv, jclass, jint Kind, jstring Path)
{
	char aPath[IO_MAX_PATH_LENGTH];
	aPath[0] = '\0';
	if(Path)
	{
		const char *pUtf = pEnv->GetStringUTFChars(Path, nullptr);
		if(pUtf)
		{
			str_copy(aPath, pUtf, sizeof(aPath));
			pEnv->ReleaseStringUTFChars(Path, pUtf);
		}
	}
	std::lock_guard<std::mutex> Lock(s_PickLock);
	s_PickResultKind = Kind;
	str_copy(s_aPickResultPath, aPath, sizeof(s_aPickResultPath));
}

bool KxPickFileAsync(int Kind)
{
	if(Kind != KXFILEPICK_SCRIPT && Kind != KXFILEPICK_TAS)
		return false;

	JNIEnv *pEnv = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
	jobject Activity = (jobject)SDL_AndroidGetActivity();
	if(!pEnv || !Activity)
		return false;
	jclass ActivityClass = pEnv->GetObjectClass(Activity);

	jmethodID MethodId = pEnv->GetStaticMethodID(ActivityClass, "sendMessage", "(II)Z");
	const bool Dispatched = MethodId && pEnv->CallStaticBooleanMethod(ActivityClass, MethodId, (jint)COMMAND_PICK_FILE, (jint)Kind) != JNI_FALSE;

	pEnv->DeleteLocalRef(ActivityClass);
	pEnv->DeleteLocalRef(Activity);

	return Dispatched;
}

bool KxPickFilePump(int *pKind, char *pPath, int BufSize)
{
	std::lock_guard<std::mutex> Lock(s_PickLock);
	if(s_PickResultKind < 0)
		return false;
	*pKind = s_PickResultKind;
	str_copy(pPath, s_aPickResultPath, BufSize);
	s_PickResultKind = -1;
	s_aPickResultPath[0] = '\0';
	return true;
}

#endif

#if !defined(CONF_FAMILY_WINDOWS)
bool KxPickScriptFile(char *pBuf, int BufSize)
{
	(void)pBuf;
	(void)BufSize;
	return false;
}

bool KxPickTasFile(char *pBuf, int BufSize)
{
	(void)pBuf;
	(void)BufSize;
	return false;
}
#endif

#if !defined(CONF_PLATFORM_ANDROID)
bool KxPickFileAsync(int Kind)
{
	(void)Kind;
	return false;
}

bool KxPickFilePump(int *pKind, char *pPath, int BufSize)
{
	(void)pKind;
	(void)pPath;
	(void)BufSize;
	return false;
}
#endif
