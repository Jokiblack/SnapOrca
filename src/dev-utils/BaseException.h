#pragma once
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include "StackWalker.h"
#include <eh.h>
#include <cstddef>
#include <string>

class CBaseException : public CStackWalker
{
public:
	CBaseException(HANDLE hProcess = GetCurrentProcess(), WORD wPID = GetCurrentProcessId(), LPCTSTR lpSymbolPath = NULL, PEXCEPTION_POINTERS pEp = NULL);
	~CBaseException(void);
	virtual void OutputString(LPCTSTR lpszFormat, ...);
	virtual void ShowLoadModules();
	virtual void ShowCallstack(HANDLE hThread = GetCurrentThread(), const CONTEXT* context = NULL);
	virtual void ShowExceptionResoult(DWORD dwExceptionCode);
	virtual BOOL GetLogicalAddress(PVOID addr, PTSTR szModule, DWORD len, DWORD& section, DWORD& offset );
	virtual void ShowRegistorInformation(PCONTEXT pCtx);
	virtual void ShowExceptionInformation(HANDLE hThread = GetCurrentThread());
	// Writes <crash report>.dmp next to the text report (nothing is sent anywhere).
	void WriteMiniDump(DWORD dwThreadId);
	static LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS pExceptionInfo);
	static LONG WINAPI UnhandledExceptionFilter2(PEXCEPTION_POINTERS pExceptionInfo);
	static void STF(unsigned int ui,  PEXCEPTION_POINTERS pEp);
	//BBS set crash log folder
	static void set_log_folder(std::string log_folder);
	// Snapmaker: local crash reports for builds without Sentry. Registers UnhandledExceptionFilter
	// (chained to any filter already installed, e.g. crashpad) and hooks std::terminate, abort(),
	// CRT invalid-parameter and pure-call failures, which bypass the filter by failing fast.
	static void install_crash_handlers();
	// Writes the text report, the minidump and a fatal log line for the calling thread.
	static void report_fatal(PEXCEPTION_POINTERS pEp, const std::string& reason);
protected:
	static DWORD WINAPI CrashReportThread(LPVOID param);
	PEXCEPTION_POINTERS m_pEp;
	boost::nowide::ofstream *output_file;
	std::string m_report_path; // crash report path without extension; empty if no log folder
};

#define SET_DEFAUL_EXCEPTION() _set_se_translator(CBaseException::STF)