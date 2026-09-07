#ifndef GAME_CLIENT_COMPONENTS_KINETIX_CODE_EXEC_FILE_DIALOG_H
#define GAME_CLIENT_COMPONENTS_KINETIX_CODE_EXEC_FILE_DIALOG_H

enum
{
	KXFILEPICK_SCRIPT = 0,
	KXFILEPICK_TAS = 1,
};

bool KxPickFileAsync(int Kind);
bool KxPickFilePump(int *pKind, char *pPath, int BufSize);

bool KxPickScriptFile(char *pBuf, int BufSize);
bool KxPickTasFile(char *pBuf, int BufSize);

#endif // GAME_CLIENT_COMPONENTS_KINETIX_CODE_EXEC_FILE_DIALOG_H
