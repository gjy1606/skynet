// simple lua socket library for client
// It's only for demo, limited feature. Don't use it in your project.
// Rewrite socket library by yourself .

#include <lua.h>
#include <lauxlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>

#ifdef _WIN32
#include <direct.h>
#include <stdio.h>
#include <conio.h>
#include <Windows.h>
#elif defined ANDROID
#include "android/log.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>

typedef int                 BOOL;
#ifndef FALSE
#define FALSE               0
#endif
#ifndef TRUE
#define TRUE                1
#endif
#ifndef MAX_PATH
#define MAX_PATH            PATH_MAX
#endif
#define _vsnprintf vsnprintf
#endif // _WIN32
#include <signal.h>

enum LogLevel {
	kLogUnknown = 0,        // 未知级别
	kLogVerbose = 1,        // 调试级别
	kLogInfo = 2,        // 提示级别
	kLogWarning = 3,        // 警告级别
	kLogError = 4,        // 错误级别
	kLogMust = 5,        // 必须显示的信息
};

struct LogStat {
	const char *prefix;
	const char *directory;
	FILE *file;
	struct tm time;
};

static struct LogStat log_stat;

#ifdef _WIN32
// 记录默认的屏幕设置（颜色+背景颜色）
static CONSOLE_SCREEN_BUFFER_INFO ScreenInfo;
static BOOL initialized = FALSE;

#define LOG_BACKGROUND 0x0000 //BACKGROUND_INTENSITY; //BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;

static const unsigned short level_color[] = {
	LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_GREEN | FOREGROUND_BLUE, // LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_GREEN | FOREGROUND_BLUE,  // kLogUnknown
	LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_GREEN, // LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_GREEN,                    // kLogVerbose
	LOG_BACKGROUND | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE, // LOG_BACKGROUND | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,  // kLogInfo
	LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN, // LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN, // kLogWarning
	LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_RED, // LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_RED,                      // kLogError
	LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_BLUE, // LOG_BACKGROUND | FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_BLUE,  // kLogMust
};

unsigned short * wszGBK[4096];
const char *szGBK[4096];

const char * UTF8ToGBK(const char * strUTF8, int * len)
{
	(*len) = MultiByteToWideChar(CP_UTF8, 0, strUTF8, -1, NULL, 0);
	//unsigned short * wszGBK = new unsigned short[len + 1];
	memset(wszGBK, 0, (*len));
	MultiByteToWideChar(CP_UTF8, 0, (LPCTSTR)strUTF8, -1, wszGBK, (*len));

	(*len) = WideCharToMultiByte(CP_ACP, 0, wszGBK, -1, NULL, 0, NULL, NULL);
	//char *szGBK = new char[len + 1];
	memset(szGBK, 0, (*len) + 1);
	WideCharToMultiByte(CP_ACP, 0, wszGBK, -1, szGBK, (*len), NULL, NULL);
	szGBK[(*len) + 1] = "\0";
	//strUTF8 = szGBK;  
	//std::string strTemp(szGBK);
	//delete[]szGBK;
	//delete[]wszGBK;
	//return strTemp;
	return szGBK;
}

#endif

int logInit(lua_State *L)
{
	size_t dir_len = 512;
	size_t pre_len = 20;
	static char dir[512];
	memset(dir, 0x00, 512);
	static char pre[20];
	memset(pre, 0x00, 20);
	const char * directory = luaL_checkstring(L, 1);
	size_t len = strlen(directory);
	if (len > 512) {
		len = 511;
	}
	memcpy(dir, directory, len);
	dir[len + 1] = '\0';
	log_stat.directory = dir;

	const char * prefix = luaL_checkstring(L, 2);
	if (strlen(prefix) != 0) {
		len = strlen(prefix);
		if (len > 20) {
			len = 19;
		}
		memcpy(pre, prefix, len);
		pre[len + 1] = '\0';
		log_stat.prefix = pre;
	}

	return 1;
}

void logUninit()
{
	if (log_stat.file != NULL)
	{
		fclose(log_stat.file);
		log_stat.file = NULL;
	}
}

static const char* getLevelStr(int level)
{
	switch (level)
	{
	case kLogVerbose:
		return "VERB";
	case kLogInfo:
		return "INFO";
	case kLogWarning:
		return "WARN";
	case kLogError:
		return "ERRS";
	case kLogMust:
		return "MUST";
	case kLogUnknown:
	default:
		return "UNKN";
	}
}

static void flushLogMessage(int level, const char *message, struct tm *time)
{
	// 此函数同一时间只能由一个线程调用~~~
	//  这里进行加锁
	//static CLock lock;
	//GUARD(&lock);

#ifdef WIN32
	// 记录默认的屏幕设置（颜色+背景颜色）
	if (!initialized)
	{
		initialized = TRUE;
		// 保存原来的Console信息
		GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ScreenInfo);
	}

	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), level_color[level]);
#endif // WIN32

	// win32平台下输出unicode字符串
	// linux下直接输出utf8字符串
#ifdef _WIN32
	//mem::wstring ucs2 = string::Utf8ToUcs2(message);
	//::WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), ucs2.c_str(), ucs2.length(), NULL, NULL);

	int len = 0;
	const char * msg = UTF8ToGBK(message, &len);
	//WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), msg, len, NULL, NULL);

	WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), message, strlen(message), NULL, NULL);
#elif defined ANDROID
#define LOG_TAG "JNITag"
	switch (level)
	{
	case kLogVerbose:
		__android_log_print(ANDROID_LOG_INFO, LOG_TAG, message, "");
		break;
	case kLogInfo:
		__android_log_print(ANDROID_LOG_INFO, LOG_TAG, message, "");
		break;
	case kLogWarning:
		__android_log_print(ANDROID_LOG_WARN, LOG_TAG, message, ""); // LOG类型:warning
		break;
	case kLogError:
		__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, message, ""); // LOG类型:error
		break;
	case kLogMust:
		__android_log_print(ANDROID_LOG_INFO, LOG_TAG, message, ""); // LOG类型:Verbose???
		break;
	default:
		__android_log_print(ANDROID_LOG_INFO, LOG_TAG, message, "");
	}
#else
	fwrite(message, 1, strlen(message), stdout);
	fflush(stdout);
#endif // _WIN32

	// 创建目录
	mkdir(log_stat.directory == NULL ? "log" : log_stat.directory
#ifndef _WIN32
		, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IXOTH
#endif // _WIN32
		);

	if (log_stat.file == NULL || log_stat.time.tm_mday != time->tm_mday)
	{
		log_stat.time = *time;

		char fileName[MAX_PATH];
		sprintf(fileName, "%s/%s%s%4d_%02d_%02d_%02d_%02d_%02d.log",
			log_stat.directory == NULL ? "log" : log_stat.directory,
			log_stat.prefix == NULL ? "" : log_stat.prefix,
			log_stat.prefix == NULL ? "" : "_",
			time->tm_year + 1900,
			time->tm_mon + 1,
			time->tm_mday,
			time->tm_hour,
			time->tm_min,
			time->tm_sec
			);

		if (log_stat.file) fclose(log_stat.file);
		log_stat.file = fopen(fileName, "a");
	}
	if (log_stat.file != NULL)
	{
		fwrite(message, 1, strlen(message), log_stat.file);
		fflush(log_stat.file);
	}

#ifdef WIN32
	// 将输出文字属性还原
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), ScreenInfo.wAttributes);
#endif // WIN32
}

static int user_log(int level, const char *module, BOOL pureFormat, const char *msgfmt, va_list ap)
{
	char buffer[8192];

	char *walk = buffer;
	time_t tt;
	time(&tt);
	struct tm* t = localtime(&tt);
	if (!pureFormat)
	{
		walk += sprintf(walk, "[%s - %s] %4d.%02d.%02d %02d:%02d:%02d ",
			module,
			getLevelStr(level),
			t->tm_year + 1900,
			t->tm_mon + 1,
			t->tm_mday,
			t->tm_hour,
			t->tm_min,
			t->tm_sec
			);
	}

#ifdef _WIN32
	//_locale_t locale = _create_locale(); // (LC_ALL, "C");
	// vsnprintf在windows和linux下行为不一致，当缓冲区不够长时，linux把最后一个字符填写为'\0',windows不会
	//  两者返回的长度在任何情况下都不包含最后的'\0'
	//walk += _vsnprintf_l(walk, sizeof(buffer) - strlen(buffer) - (pureFormat ? 0 : 4), msgfmt, locale, ap);
	walk += sprintf(walk, "%s", msgfmt); // 脚本层已经进行字符串格式化了，所以这里不需要 ap
	//_free_locale(locale);
#else
	walk += (int)(_vsnprintf(walk, sizeof(buffer) - strlen(buffer) - (pureFormat ? 0 : 4), msgfmt, ap));
#endif // _WIN32

	if (!pureFormat)
		walk += sprintf(walk, "\r\n");
	// 添加结束符
	*walk++ = '\0';

	flushLogMessage(level, buffer, t);

	return walk - buffer;
}

int lualog(int level, const char *module, const char *msgfmt, ...)
{
	va_list ap;
	va_start(ap, msgfmt);
	int result = user_log(level, module, FALSE, msgfmt, ap);
	va_end(ap);
	return result;
}

static int
lmust(lua_State *L) {

	const char * msgfmt = luaL_checkstring(L, 1);

	lualog(kLogMust, "SCRIPT", msgfmt);

	return 1;
}

static int
lwarn(lua_State *L) {

	const char * msgfmt = luaL_checkstring(L, 1);

	lualog(kLogWarning, "SCRIPT", msgfmt);

	return 1;
}

static int
lerror(lua_State *L) {

	const char * msgfmt = luaL_checkstring(L, 1);

	lualog(kLogError, "SCRIPT", msgfmt);

	return 1;
}

static int 
lcolorPrint(lua_State *L){
#ifdef WIN32
	const char * msgfmt = luaL_checkstring(L, 1);
	int color = lua_tointeger(L, 2);
	// 记录默认的屏幕设置（颜色+背景颜色）
	if (!initialized)
	{
		initialized = TRUE;
		// 保存原来的Console信息
		GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ScreenInfo);
	}
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
	WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), msgfmt, strlen(msgfmt), NULL, NULL);
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), ScreenInfo.wAttributes);
#endif // WIN32

	return 1;
}

int
luaopen_lualog(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "logInit", logInit },
		{ "logUninit", logUninit },
		{ "must", lmust },
		{ "warn", lwarn },
		{ "error", lerror },
		{ "colorPrint", lcolorPrint },
		{ NULL, NULL },
	};
	luaL_newlib(L, l);

	return 1;
}
