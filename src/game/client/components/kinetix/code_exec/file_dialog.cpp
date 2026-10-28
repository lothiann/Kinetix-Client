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

// UTF-8 -> UTF-16 helper for the dialog paths.
static void KxUtf8ToWide(const char *pUtf8, wchar_t *pWide, int WideSize)
{
	pWide[0] = L'\0';
	if(!pUtf8 || !pUtf8[0] || WideSize <= 0)
		return;
	MultiByteToWideChar(CP_UTF8, 0, pUtf8, -1, pWide, WideSize);
}

bool KxPickScriptFile(char *pBuf, int BufSize, const char *pInitialDir)
{
	wchar_t aWidePath[1024];
	aWidePath[0] = L'\0';

	wchar_t aWideCwd[1024];
	const DWORD CurDirLen = GetCurrentDirectoryW(1024, aWideCwd);

	// v6.5: open in the caller-provided folder (Kinetix/Scripts in the save
	// dir) instead of the binary's data/scripts.
	wchar_t aWideDir[1024];
	KxUtf8ToWide(pInitialDir, aWideDir, 1024);
	LPCWSTR pDir = aWideDir[0] ? aWideDir : NULL;

	OPENFILENAMEW Ofn;
	mem_zero(&Ofn, sizeof(Ofn));
	Ofn.lStructSize = sizeof(Ofn);
	Ofn.hwndOwner = GetActiveWindow();
	Ofn.lpstrFilter = L"Lua scripts (*.lua, *.txt)\0*.lua;*.txt\0All files (*.*)\0*.*\0";
	Ofn.lpstrFile = aWidePath;
	Ofn.nMaxFile = 1024;
	Ofn.lpstrInitialDir = pDir;
	Ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

	const BOOL Picked = GetOpenFileNameW(&Ofn);

	if(CurDirLen > 0 && CurDirLen < 1024)
		SetCurrentDirectoryW(aWideCwd);

	if(!Picked)
		return false;

	return WideCharToMultiByte(CP_UTF8, 0, aWidePath, -1, pBuf, BufSize, NULL, NULL) > 0;
}

bool KxPickTasFile(char *pBuf, int BufSize, const char *pInitialDir)
{
	wchar_t aWidePath[1024];
	aWidePath[0] = L'\0';

	wchar_t aWideCwd[1024];
	const DWORD CurDirLen = GetCurrentDirectoryW(1024, aWideCwd);

	// v6.5: open in the caller-provided folder (Kinetix/TAS in the save dir)
	// instead of the binary's data/tas.
	wchar_t aWideDir[1024];
	KxUtf8ToWide(pInitialDir, aWideDir, 1024);
	LPCWSTR pDir = aWideDir[0] ? aWideDir : NULL;

	OPENFILENAMEW Ofn;
	mem_zero(&Ofn, sizeof(Ofn));
	Ofn.lStructSize = sizeof(Ofn);
	Ofn.hwndOwner = GetActiveWindow();
	Ofn.lpstrFilter = L"TAS files (*.tas)\0*.tas\0All files (*.*)\0*.*\0";
	Ofn.lpstrFile = aWidePath;
	Ofn.nMaxFile = 1024;
	Ofn.lpstrInitialDir = pDir;
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

	// v6.5: SDL_AndroidGetActivity() returns an SDL-owned reference — deleting
	// it with DeleteLocalRef crashed the client. Wrap it in our own local ref
	// and only ever release that one.
	jobject ActivityLocal = pEnv->NewLocalRef(Activity);
	if(!ActivityLocal)
		return false;

	bool Dispatched = false;
	jclass ActivityClass = pEnv->GetObjectClass(ActivityLocal);
	if(ActivityClass)
	{
		jmethodID MethodId = pEnv->GetStaticMethodID(ActivityClass, "sendMessage", "(II)Z");
		if(MethodId)
			Dispatched = pEnv->CallStaticBooleanMethod(ActivityClass, MethodId, (jint)COMMAND_PICK_FILE, (jint)Kind) != JNI_FALSE;
		if(pEnv->ExceptionCheck())
		{
			// A Java exception must never leak into the game thread.
			pEnv->ExceptionDescribe();
			pEnv->ExceptionClear();
			Dispatched = false;
		}
		pEnv->DeleteLocalRef(ActivityClass);
	}
	pEnv->DeleteLocalRef(ActivityLocal);
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
bool KxPickScriptFile(char *pBuf, int BufSize, const char *pInitialDir)
{
	(void)pBuf;
	(void)BufSize;
	(void)pInitialDir;
	return false;
}

bool KxPickTasFile(char *pBuf, int BufSize, const char *pInitialDir)
{
	(void)pBuf;
	(void)BufSize;
	(void)pInitialDir;
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
