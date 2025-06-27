#include "lua.hpp"
#include <string>
#include <vector>
#ifdef _WIN32
#include <tchar.h>
#include <windows.h>
#else
#include <cstring>
#include <dirent.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#define METANAME "lprocess"
#define PROCESS_HANDLE_META "ProcessHandleMeta"

#ifdef _WIN32
std::string GetLastErrorMessage(DWORD errCode) {
    LPSTR msgBuf = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, errCode, 0, (LPSTR)&msgBuf, 0, NULL);
    std::string msg(msgBuf);
    LocalFree(msgBuf);
    return msg;
}
#endif

typedef struct {
#ifdef _WIN32
    HANDLE handle;
    DWORD pid;
#else
    pid_t pid;
#endif
} ProcessHandle;

static int lgetpid(lua_State* L) {
#ifdef _WIN32
    lua_pushinteger(L, GetCurrentProcessId());
#else
    lua_pushinteger(L, getpid());
#endif
    return 1;
}

static int ltopid(lua_State* L) {
    ProcessHandle* ph = (ProcessHandle*)luaL_checkudata(L, 1, PROCESS_HANDLE_META);
    if (!ph) {
        return lua_error(L);
    }

    lua_pushinteger(L, (lua_Integer)ph->pid);
    return 1;
}

static int lgethandle(lua_State* L) {
    int pid = (int)luaL_checkinteger(L, 1);

    // First verify the process exists
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "Process does not exist or access denied");
        return 2;
    }
    CloseHandle(hProcess);
#else
    // On Unix, check via /proc or kill(pid, 0)
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    if (access(path, F_OK) != 0) {
        if (kill(pid, 0) != 0) {
            lua_pushnil(L);
            lua_pushstring(L, "Process does not exist");
            return 2;
        }
    }
#endif

    ProcessHandle* ph = (ProcessHandle*)lua_newuserdata(L, sizeof(ProcessHandle));
    luaL_getmetatable(L, PROCESS_HANDLE_META);
    lua_setmetatable(L, -2);

#ifdef _WIN32
    ph->handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    ph->pid = pid;
#else
    ph->pid = pid;
#endif

    return 1;
}

static int lcreate_proc(lua_State* L) {
    const char* URL = luaL_checkstring(L, 1);
    const char* Parms = luaL_optstring(L, 2, NULL);

    ProcessHandle* ph = (ProcessHandle*)lua_newuserdata(L, sizeof(ProcessHandle));
    luaL_getmetatable(L, PROCESS_HANDLE_META);
    lua_setmetatable(L, -2);

#ifdef _WIN32
    bool bLaunchDetached = lua_toboolean(L, 3);
    bool bLaunchHidden = lua_toboolean(L, 4);
    bool bLaunchReallyHidden = lua_toboolean(L, 5);
    const char* OptionalWorkingDirectory = luaL_optstring(L, 6, NULL);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (bLaunchReallyHidden) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    } else if (bLaunchHidden) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_MINIMIZE;
    }

    std::wstring wURL(URL, URL + strlen(URL));
    std::wstring wParms = Parms ? std::wstring(Parms, Parms + strlen(Parms)) : L"";
    std::wstring cmdLine = wURL;
    
    std::wstring workingDir = OptionalWorkingDirectory ? std::wstring(OptionalWorkingDirectory, OptionalWorkingDirectory + strlen(OptionalWorkingDirectory)) : L"";
    const wchar_t* lpWorkingDir = workingDir.empty() ? NULL : workingDir.c_str();

    if (!wParms.empty()) cmdLine += L" " + wParms;

    DWORD dwCreationFlags = 0;
    if (bLaunchDetached) dwCreationFlags |= DETACHED_PROCESS;

    if (!CreateProcessW(NULL, &cmdLine[0], NULL, NULL, FALSE, dwCreationFlags, NULL, lpWorkingDir, &si, &pi)) {
        DWORD err = GetLastError();
        lua_pushnil(L);
        lua_pushfstring(L, "CreateProcess failed (Error %d): %s", err, GetLastErrorMessage(err).c_str());
        return 2;
    }

    ph->handle = pi.hProcess;
    ph->pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
#else
    pid_t pid;
    char* argv[64] = { NULL };
    int argc = 0;

    argv[argc++] = strdup(URL);
    if (Parms) {
        char* token = strtok(strdup(Parms), " ");
        while (token && argc < 63) {
            argv[argc++] = token;
            token = strtok(NULL, " ");
        }
    }

    if (posix_spawnp(&pid, URL, NULL, NULL, argv, environ) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "posix_spawnp failed");
        return 2;
    }

    ph->pid = pid;
#endif

    return 1;
}

static int lclose_proc(lua_State* L) {
    ProcessHandle* ph = (ProcessHandle*)luaL_checkudata(L, 1, PROCESS_HANDLE_META);
    if (!ph) {
        return lua_error(L);
    }

#ifdef _WIN32
    if (ph->handle) {
        TerminateProcess(ph->handle, 0);
        CloseHandle(ph->handle);
    }
#else
    kill(ph->pid, SIGTERM);
    waitpid(ph->pid, NULL, 0);
#endif

    lua_pushboolean(L, true);
    return 1;
}

static int lis_running(lua_State* L) {
    ProcessHandle* ph = (ProcessHandle*)luaL_checkudata(L, 1, PROCESS_HANDLE_META);
    if (!ph) {
        lua_pushboolean(L, false);
        return 1;
    }

#ifdef _WIN32
    if (ph->handle) {
        DWORD exitCode;
        if (GetExitCodeProcess(ph->handle, &exitCode)) {
            lua_pushboolean(L, exitCode == STILL_ACTIVE);
        } else {
            lua_pushboolean(L, false);
        }
    } else {
        lua_pushboolean(L, false);
    }
#else
    int status;
    pid_t result = waitpid(ph->pid, &status, WNOHANG);
    if (result == 0) {
        lua_pushboolean(L, true);
    } else if (result == -1) {
        if (errno == ECHILD) {
            char path[256];
            snprintf(path, sizeof(path), "/proc/%d/status", ph->pid);
            if (access(path, F_OK) == 0) {
                lua_pushboolean(L, true);
            } else {
                lua_pushboolean(L, false);
            }
        } else {
            lua_pushboolean(L, false);
        }
    } else {
        lua_pushboolean(L, false);
    }
#endif

    return 1;
}

extern "C" {
int luaopen_process(lua_State* L) {
    luaL_newmetatable(L, PROCESS_HANDLE_META);
    lua_pop(L, 1);

    luaL_Reg l[] = {
        { "getpid", lgetpid },
        { "topid", ltopid },
        { "gethandle", lgethandle },
        { "create_proc", lcreate_proc },
        { "close_proc", lclose_proc },
        { "is_running", lis_running },
        { NULL, NULL }
    };
    luaL_checkversion(L);
    luaL_newlib(L, l);
    return 1;
}
}