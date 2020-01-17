// OpenCppCoverage is an open source code coverage for C++.
// Copyright (C) 2014 OpenCppCoverage
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "stdafx.h"
#include "Debugger.hpp"

#include "tools/Log.hpp"
#include "tools/ScopedAction.hpp"

#include "Process.hpp"
#include "CppCoverageException.hpp"
#include "IDebugEventsHandler.hpp"

#include "Tools/Tool.hpp"

#include <tlhelp32.h>
#include <tchar.h>
#include <psapi.h>

#pragma comment( lib, "psapi.lib" )

namespace
{
	std::vector<std::pair<std::wstring, void *>> GetModules(DWORD processID)
	{
		std::vector<std::pair<std::wstring, void *>> res;

		HMODULE hMods[1024];
		HANDLE hProcess;
		DWORD cbNeeded;
		unsigned int i;

		// Print the process identifier.

		//printf("\nProcess ID: %u\n", processID);

		// Get a handle to the process.

		hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
			PROCESS_VM_READ,
			FALSE, processID);
		if (NULL == hProcess)
			return {};

		// Get a list of all the modules in this process.

		if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded))
		{
			for (i = 0; i < (cbNeeded / sizeof(HMODULE)); i++)
			{
				TCHAR szModName[MAX_PATH];

				// Get the full path to the module's file.

				if (GetModuleFileNameEx(hProcess, hMods[i], szModName,
					sizeof(szModName) / sizeof(TCHAR)))
				{
					// Print the module name and handle value.

					//_tprintf(TEXT("\t%s (0x%08X)\n"), szModName, hMods[i]);
					res.push_back({ szModName, hMods[i] });
				}
			}
		}

		// Release the handle to the process.

		CloseHandle(hProcess);

		return res;
	}

	void ListProcessThreads(DWORD dwOwnerPID, std::vector<DWORD> *outThreadIds)
	{
		if (!outThreadIds)
			throw std::runtime_error("outThreadIds was nullptr");
		outThreadIds->clear();

		HANDLE hThreadSnap = INVALID_HANDLE_VALUE;
		THREADENTRY32 te32;

		// Take a snapshot of all running threads  
		hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (hThreadSnap == INVALID_HANDLE_VALUE)
			throw std::runtime_error("Invalid handle value");

		// Fill in the size of the structure before using it. 
		te32.dwSize = sizeof(THREADENTRY32);

		// Retrieve information about the first thread,
		// and exit if unsuccessful
		if (!Thread32First(hThreadSnap, &te32))
		{
			CloseHandle(hThreadSnap);     // Must clean up the snapshot object!
			throw std::runtime_error("Thread32First");
		}

		// Now walk the thread list of the system,
		// and display information about each thread
		// associated with the specified process
		do
		{
			if (te32.th32OwnerProcessID == dwOwnerPID)
			{
				outThreadIds->push_back(te32.th32ThreadID);;
			}
		} while (Thread32Next(hThreadSnap, &te32));

		//  Don't forget to clean up the snapshot object.
		CloseHandle(hThreadSnap);
	}
}

namespace CppCoverage
{
	//-------------------------------------------------------------------------
	namespace
	{
		void OnRip(const RIP_INFO& ripInfo)
		{
			LOG_ERROR << "Debugee process terminate unexpectedly:"
				<< "(type:" << ripInfo.dwType << ")"
				<< GetErrorMessage(ripInfo.dwError);
		}
	}	

	//-------------------------------------------------------------------------
	struct Debugger::ProcessStatus
	{
		ProcessStatus() = default;

		ProcessStatus(
			boost::optional<int> exitCode,
			boost::optional<DWORD> continueStatus)
			: exitCode_{ exitCode }
			, continueStatus_{ continueStatus }
		{
		}

		boost::optional<int> exitCode_;
		boost::optional<DWORD> continueStatus_;
	};

	//-------------------------------------------------------------------------
	Debugger::Debugger(
		bool coverChildren,
		bool continueAfterCppException,
        bool stopOnAssert)
		: coverChildren_{ coverChildren }
		, continueAfterCppException_{ continueAfterCppException }
        , stopOnAssert_{ stopOnAssert }
	{
	}

	//-------------------------------------------------------------------------
	int Debugger::Debug(
		const StartInfo& startInfo,
		IDebugEventsHandler& debugEventsHandler)
	{
		Process process(startInfo);
		process.Start((coverChildren_) ? DEBUG_PROCESS: DEBUG_ONLY_THIS_PROCESS);

		HANDLE rootProcessHandle = INVALID_HANDLE_VALUE;

		if (auto pidPtr = process.GetAttachedProcessId()) {
			DEBUG_EVENT debugEvent;
			debugEvent.dwDebugEventCode = CREATE_PROCESS_DEBUG_EVENT;
			debugEvent.dwProcessId = *pidPtr;

			std::vector<DWORD> thrIds;
			ListProcessThreads(*pidPtr, &thrIds);

			std::vector<HANDLE> thrHandles;
			for (auto thrId : thrIds)
			{
				auto handle = OpenThread(THREAD_QUERY_INFORMATION, false, thrId);
				if (handle == INVALID_HANDLE_VALUE)
					throw std::runtime_error("OpenThread returned INVALID_HANDLE_VALUE");
				thrHandles.push_back(handle);
			}
			/*Tools::ScopedAction closeThrHandles([=] {
				for (auto h : thrHandles)
					CloseHandle(h);
			});*/

			int bestThreadIdx = -1;
			uint64_t bestTime = ~0;

			for (int i = 0; i < thrHandles.size(); ++i)
			{
				const auto h = thrHandles[i];

				FILETIME creation, dummy;
				auto ok = GetThreadTimes(h, &creation, &dummy, &dummy, &dummy);
				if (!ok)
					throw std::runtime_error("GetThreadTimes failed");

				ULARGE_INTEGER ulargeCreation;
				ulargeCreation.HighPart = creation.dwHighDateTime;
				ulargeCreation.LowPart = creation.dwLowDateTime;

				auto v = ulargeCreation.QuadPart;
				if (v < bestTime)
				{
					bestTime = v;
					bestThreadIdx = i;
				}
			}

			auto p = startInfo.GetPath().wstring();
			OFSTRUCT ofstruct;
			
			debugEvent.dwThreadId = thrIds[bestThreadIdx];
			debugEvent.u.CreateProcessInfo.hFile = CreateFile(p.data(), GENERIC_READ,
				FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

			if (debugEvent.u.CreateProcessInfo.hFile == INVALID_HANDLE_VALUE)
			{
				throw std::runtime_error("CreateFile failed");
			}

			debugEvent.u.CreateProcessInfo.hThread = thrHandles[bestThreadIdx];
			rootProcessHandle = debugEvent.u.CreateProcessInfo.hProcess = 
				OpenProcess(PROCESS_ALL_ACCESS, false, *pidPtr);
			if (!rootProcessHandle)
				throw std::runtime_error("OpenProcess failed");

			debugEvent.u.CreateProcessInfo.lpBaseOfImage = 0;
			// ...

			///HandleDebugEvent(debugEvent, debugEventsHandler);

			// Threads
			for (int i = 0; i < thrHandles.size(); ++i)
			{
				if (i != bestThreadIdx)
				{
					DEBUG_EVENT de;
					de.dwDebugEventCode = CREATE_THREAD_DEBUG_EVENT;
					de.dwProcessId = *pidPtr;
					de.dwThreadId = thrIds[i];
					de.u.CreateThread.hThread = thrHandles[i];
					de.u.CreateThread.lpStartAddress = 0;
					de.u.CreateThread.lpThreadLocalBase = 0;
					//HandleDebugEvent(de, debugEventsHandler);
				}
			
			}

			auto modules = GetModules(*pidPtr);

			for (auto mod : modules)
			{
				if (mod.second == GetModuleHandle(NULL)) continue;

				// DLLs
				DEBUG_EVENT dllEvent;
				dllEvent.dwDebugEventCode = LOAD_DLL_DEBUG_EVENT;
				dllEvent.dwProcessId = *pidPtr;
				dllEvent.dwThreadId = thrIds[bestThreadIdx];
				dllEvent.u.LoadDll.lpBaseOfDll = mod.second;
				dllEvent.u.LoadDll.hFile = CreateFile(mod.first.data(), GENERIC_READ,
					FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
					NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if (dllEvent.u.LoadDll.hFile == INVALID_HANDLE_VALUE)
				{
					throw std::runtime_error("CreateFile for dll failed");
				}

				//dllEvent.u.LoadDll.
				///HandleDebugEvent(dllEvent, debugEventsHandler);
			}
		}
		
		DEBUG_EVENT debugEvent;
		boost::optional<int> exitCode;

		processHandles_.clear();
		threadHandles_.clear();
		rootProcessId_ = boost::none;

		while (!exitCode || !processHandles_.empty())
		{
			bool stopped = false;
			while (1) 
			{
				if (!WaitForDebugEvent(&debugEvent, 1000))
				{
					auto err = GetLastError();
					if (err != ERROR_SEM_TIMEOUT)
					{
						THROW_LAST_ERROR(L"Error WaitForDebugEvent:", GetLastError());
					}

					DWORD code;
					if (!GetExitCodeProcess(rootProcessHandle, &code))
					{
						throw std::runtime_error("Error GetExitCodeProcess:" + std::to_string(GetLastError()));
					}

					if (code != STILL_ACTIVE)
					{
						printf("It seems process has exited\n");
						exitCode = 108;
						stopped = true;
						break;
					}
				}
				else {
					break;
				}
			}

			if (stopped)
				break;

			printf("Debug event %d\n", debugEvent.dwDebugEventCode);

			ProcessStatus processStatus = HandleDebugEvent(debugEvent, debugEventsHandler);
			
			// Get the exit code of the root process
			// Set once as we do not want EXCEPTION_BREAKPOINT to be override
			if (processStatus.exitCode_ && rootProcessId_ == debugEvent.dwProcessId && !exitCode)
				exitCode = processStatus.exitCode_;

			auto continueStatus = boost::get_optional_value_or(processStatus.continueStatus_, DBG_CONTINUE);

			if (!ContinueDebugEvent(debugEvent.dwProcessId, debugEvent.dwThreadId, continueStatus))
				THROW_LAST_ERROR("Error in ContinueDebugEvent:", GetLastError());
		}

		return *exitCode;
	}
	
	//-------------------------------------------------------------------------
	Debugger::ProcessStatus Debugger::HandleDebugEvent(
		const DEBUG_EVENT& debugEvent,
		IDebugEventsHandler& debugEventsHandler)
	{
		auto dwProcessId = debugEvent.dwProcessId;
		auto dwThreadId = debugEvent.dwThreadId;

		switch (debugEvent.dwDebugEventCode)
		{
			case CREATE_PROCESS_DEBUG_EVENT: OnCreateProcess(debugEvent, debugEventsHandler); break;
			case CREATE_THREAD_DEBUG_EVENT: OnCreateThread(debugEvent.u.CreateThread.hThread, dwThreadId); break;
			default:
			{
				auto hProcess = GetProcessHandle(dwProcessId);
				auto hThread = GetThreadHandle(dwThreadId);
				return HandleNotCreationalEvent(debugEvent, debugEventsHandler, hProcess, hThread, dwThreadId);
			}
		}

		return{};
	}

	//-------------------------------------------------------------------------
	Debugger::ProcessStatus
		Debugger::HandleNotCreationalEvent(
		const DEBUG_EVENT& debugEvent,
		IDebugEventsHandler& debugEventsHandler,
		HANDLE hProcess,
		HANDLE hThread,
		DWORD dwThreadId)
	{
		switch (debugEvent.dwDebugEventCode)
		{
			case EXIT_PROCESS_DEBUG_EVENT:
			{
				auto exitCode = OnExitProcess(debugEvent, hProcess, hThread, debugEventsHandler);
				return ProcessStatus{exitCode, boost::none};
			}
			case EXIT_THREAD_DEBUG_EVENT: OnExitThread(dwThreadId); break;
			case LOAD_DLL_DEBUG_EVENT:
			{
				const auto& loadDll = debugEvent.u.LoadDll;
				Tools::ScopedAction scopedAction{ [&loadDll]{ CloseHandle(loadDll.hFile); } };
				debugEventsHandler.OnLoadDll(hProcess, hThread, loadDll);
				break;
			}
			case UNLOAD_DLL_DEBUG_EVENT:
			{
				debugEventsHandler.OnUnloadDll(hProcess, hThread, debugEvent.u.UnloadDll);
				break;
			}
			case EXCEPTION_DEBUG_EVENT: return OnException(debugEvent, debugEventsHandler, hProcess, hThread);
			case RIP_EVENT: OnRip(debugEvent.u.RipInfo); break;
			default: LOG_DEBUG << "Debug event:" << debugEvent.dwDebugEventCode; break;
		}

		return ProcessStatus{};
	}
	
	//-------------------------------------------------------------------------
	Debugger::ProcessStatus
		Debugger::OnException(
		const DEBUG_EVENT& debugEvent,
		IDebugEventsHandler& debugEventsHandler,
		HANDLE hProcess,
		HANDLE hThread) const
	{
		const auto& exception = debugEvent.u.Exception;
		auto exceptionType = debugEventsHandler.OnException(hProcess, hThread, exception);

		switch (exceptionType)
		{
			case IDebugEventsHandler::ExceptionType::BreakPoint:
			{
				return ProcessStatus{ boost::none, DBG_CONTINUE };
			}
			case IDebugEventsHandler::ExceptionType::InvalidBreakPoint:
			{
				LOG_WARNING << Tools::GetSeparatorLine();
				LOG_WARNING << "It seems there is an assertion failure or you call DebugBreak() in your program.";
				LOG_WARNING << Tools::GetSeparatorLine();

                if (stopOnAssert_)
                {
                  LOG_WARNING << "Stop on assertion.";
                  return ProcessStatus{ boost::none, DBG_EXCEPTION_NOT_HANDLED };
                }
                else
                {
                  return ProcessStatus(EXCEPTION_BREAKPOINT, DBG_CONTINUE);
                }
			}
			case IDebugEventsHandler::ExceptionType::NotHandled:
			{
				return ProcessStatus{ boost::none, DBG_EXCEPTION_NOT_HANDLED };
			}
			case IDebugEventsHandler::ExceptionType::Error:
			{
				return ProcessStatus{ boost::none, DBG_EXCEPTION_NOT_HANDLED };
			}
			case IDebugEventsHandler::ExceptionType::CppError:
			{
				if (continueAfterCppException_)
				{
					const auto& exceptionRecord = exception.ExceptionRecord;
					LOG_WARNING << "Continue after a C++ exception.";
					return ProcessStatus{ static_cast<int>(exceptionRecord.ExceptionCode), DBG_CONTINUE };
				}
				return ProcessStatus{ boost::none, DBG_EXCEPTION_NOT_HANDLED };
			}
		}
		THROW("Invalid exception Type.");
	}

	//-------------------------------------------------------------------------
	void Debugger::OnCreateProcess(
		const DEBUG_EVENT& debugEvent,
		IDebugEventsHandler& debugEventsHandler)
	{		
		const auto& processInfo = debugEvent.u.CreateProcessInfo;
		Tools::ScopedAction scopedAction{ [&processInfo]{ CloseHandle(processInfo.hFile); } };

		LOG_DEBUG << "Create Process:" << debugEvent.dwProcessId;

		if (!rootProcessId_ && processHandles_.empty())
			rootProcessId_ = debugEvent.dwProcessId;

		if (!processHandles_.emplace(debugEvent.dwProcessId, processInfo.hProcess).second)
			THROW("Process id already exist");
				
		debugEventsHandler.OnCreateProcess(processInfo);

		OnCreateThread(processInfo.hThread, debugEvent.dwThreadId);
	}

	//-------------------------------------------------------------------------
	int Debugger::OnExitProcess(
		const DEBUG_EVENT& debugEvent,
		HANDLE hProcess,
		HANDLE hThread,
		IDebugEventsHandler& debugEventsHandler)
	{
		OnExitThread(debugEvent.dwThreadId);
		auto processId = debugEvent.dwProcessId;

		LOG_DEBUG << "Exit Process:" << processId;

		auto exitProcess = debugEvent.u.ExitProcess;
		debugEventsHandler.OnExitProcess(hProcess, hThread, exitProcess);

		if (processHandles_.erase(processId) != 1)
			THROW("Cannot find exited process.");

		return exitProcess.dwExitCode;
	}

	//-------------------------------------------------------------------------
	void Debugger::OnCreateThread(
		HANDLE hThread,
		DWORD dwThreadId)
	{
		LOG_DEBUG << "Create Thread:" << dwThreadId;

		if (!threadHandles_.emplace(dwThreadId, hThread).second)
			THROW("Thread id already exist");
	}
	
	//-------------------------------------------------------------------------
	void Debugger::OnExitThread(DWORD dwThreadId)
	{	
		LOG_DEBUG << "Exit thread:" << dwThreadId;

		if (threadHandles_.erase(dwThreadId) != 1)
			THROW("Cannot find exited thread.");
	}

	//-------------------------------------------------------------------------
	HANDLE Debugger::GetProcessHandle(DWORD dwProcessId) const
	{
		return processHandles_.at(dwProcessId);
	}

	//-------------------------------------------------------------------------
	HANDLE Debugger::GetThreadHandle(DWORD dwThreadId) const
	{
		return threadHandles_.at(dwThreadId);
	}

	//-------------------------------------------------------------------------
	size_t Debugger::GetRunningProcesses() const
	{
		return processHandles_.size();
	}

	//-------------------------------------------------------------------------
	size_t Debugger::GetRunningThreads() const
	{
		return threadHandles_.size();
	}
}