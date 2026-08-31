#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
    ============================================================
                         SHELLTRACE
               Windows Process Execution Tracer
    ============================================================

    Features:
        - Trace .bat / .cmd files
        - Trace .ps1 PowerShell scripts
        - Trace .exe programs
        - Detect process creation
        - Detect process termination
        - Record PID
        - Record PPID
        - Record executable path
        - Record exit code
        - Record execution duration
        - Detect exceptions
        - Build process tree

    Windows APIs used:
        CreateProcessA()
        WaitForDebugEvent()
        ContinueDebugEvent()
        QueryFullProcessImageNameA()
        GetProcAddress()

    No Linux APIs.
    No ptrace().
    No /proc.
    No tlhelp32.h.
*/


#define MAX_PROCESSES 1024
#define MAX_PATH_LEN 1024
#define MAX_COMMAND 4096


/* ============================================================
                         PROCESS RECORD
   ============================================================ */

typedef struct
{
    DWORD pid;
    DWORD ppid;

    char name[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];

    ULONGLONG start_time;
    ULONGLONG end_time;

    DWORD exit_code;

    int active;
    int exited;

} ProcessRecord;


/* ============================================================
                         GLOBAL DATA
   ============================================================ */

ProcessRecord process_table[MAX_PROCESSES];

int process_count = 0;

DWORD root_pid = 0;


/* ============================================================
                         TIME
   ============================================================ */

ULONGLONG current_time_ms()
{
    return GetTickCount64();
}


/* ============================================================
                         FIND PROCESS
   ============================================================ */

ProcessRecord *find_process(DWORD pid)
{
    int i;

    for (i = 0; i < process_count; i++)
    {
        if (process_table[i].pid == pid)
        {
            return &process_table[i];
        }
    }

    return NULL;
}


/* ============================================================
                         GET FILE NAME
   ============================================================ */

void extract_filename(
    const char *path,
    char *name
)
{
    const char *last_backslash;
    const char *last_slash;
    const char *last;

    last_backslash = strrchr(path, '\\');
    last_slash = strrchr(path, '/');

    last = last_backslash;

    if (last_slash != NULL)
    {
        if (last == NULL || last_slash > last)
        {
            last = last_slash;
        }
    }

    if (last != NULL)
    {
        strcpy(name, last + 1);
    }
    else
    {
        strcpy(name, path);
    }

    if (name[0] == '\0')
    {
        strcpy(name, "?");
    }
}


/* ============================================================
               WINDOWS INTERNAL PROCESS INFORMATION

    We dynamically access NtQueryInformationProcess so that
    we don't require additional Windows SDK headers.

    This avoids the PROCESSENTRY32/TLHELP32 problem in old
    Dev-C++ / TDM-GCC installations.
   ============================================================ */

typedef LONG NTSTATUS;

#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)

typedef struct
{
    PVOID Reserved1;
    PVOID PebBaseAddress;
    PVOID Reserved2[2];
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;

} PROCESS_BASIC_INFORMATION_CUSTOM;


/*
    Function pointer for NtQueryInformationProcess.
*/

typedef NTSTATUS (WINAPI *NtQueryInformationProcessFunc)(
    HANDLE,
    ULONG,
    PVOID,
    ULONG,
    PULONG
);


/* ============================================================
                         GET PARENT PID
   ============================================================ */

DWORD get_parent_pid(DWORD pid)
{
    HMODULE ntdll;

    NtQueryInformationProcessFunc query_process;

    PROCESS_BASIC_INFORMATION_CUSTOM info;

    NTSTATUS status;

    ULONG return_length = 0;

    HANDLE process;

    DWORD parent_pid;


    /*
        Open process with query permission.
    */

    process = OpenProcess(
        PROCESS_QUERY_INFORMATION,
        FALSE,
        pid
    );

    if (process == NULL)
    {
        /*
            Try the limited permission available on newer
            Windows versions.
        */

        process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            pid
        );
    }

    if (process == NULL)
    {
        return 0;
    }


    /*
        Load NtQueryInformationProcess dynamically.
    */

    ntdll = GetModuleHandleA("ntdll.dll");

    if (ntdll == NULL)
    {
        CloseHandle(process);
        return 0;
    }


    query_process =
        (NtQueryInformationProcessFunc)
        GetProcAddress(
            ntdll,
            "NtQueryInformationProcess"
        );

    if (query_process == NULL)
    {
        CloseHandle(process);
        return 0;
    }


    memset(
        &info,
        0,
        sizeof(info)
    );


    status =
        query_process(
            process,
            0,
            &info,
            sizeof(info),
            &return_length
        );


    if (status != STATUS_SUCCESS)
    {
        CloseHandle(process);
        return 0;
    }


    parent_pid =
        (DWORD)
        info.InheritedFromUniqueProcessId;


    CloseHandle(process);

    return parent_pid;
}


/* ============================================================
                    GET PROCESS EXECUTABLE PATH
   ============================================================ */

int get_process_path(
    HANDLE process,
    char *buffer,
    DWORD buffer_size
)
{
    DWORD size;

    buffer[0] = '\0';

    if (process == NULL)
    {
        strcpy(buffer, "?");
        return 0;
    }

    size = buffer_size;

    if (QueryFullProcessImageNameA(
            process,
            0,
            buffer,
            &size))
    {
        return 1;
    }

    strcpy(buffer, "?");

    return 0;
}


/* ============================================================
                         ADD PROCESS
   ============================================================ */

ProcessRecord *add_process(
    DWORD pid,
    DWORD ppid,
    const char *path
)
{
    ProcessRecord *p;


    if (process_count >= MAX_PROCESSES)
    {
        printf(
            "\n[WARNING] Maximum process limit reached.\n"
        );

        return NULL;
    }


    p =
        &process_table[process_count];


    memset(
        p,
        0,
        sizeof(ProcessRecord)
    );


    p->pid = pid;

    p->ppid = ppid;


    strncpy(
        p->path,
        path,
        MAX_PATH_LEN - 1
    );

    p->path[MAX_PATH_LEN - 1] =
        '\0';


    extract_filename(
        p->path,
        p->name
    );


    p->start_time =
        current_time_ms();


    p->active = 1;

    p->exited = 0;


    process_count++;


    return p;
}


/* ============================================================
                    PROCESS START OUTPUT
   ============================================================ */

void print_process_start(
    ProcessRecord *p
)
{
    if (p == NULL)
    {
        return;
    }


    printf(
        "\n"
        "[PROCESS START]\n"
        "    PID      : %lu\n"
        "    PPID     : %lu\n"
        "    Name     : %s\n"
        "    Path     : %s\n",
        (unsigned long)p->pid,
        (unsigned long)p->ppid,
        p->name,
        p->path
    );
}


/* ============================================================
                    PROCESS EXIT OUTPUT
   ============================================================ */

void print_process_exit(
    ProcessRecord *p,
    DWORD exit_code
)
{
    ULONGLONG duration;


    if (p == NULL)
    {
        return;
    }


    p->end_time =
        current_time_ms();


    p->exit_code =
        exit_code;


    p->active = 0;

    p->exited = 1;


    duration =
        p->end_time -
        p->start_time;


    printf(
        "\n"
        "[PROCESS EXIT]\n"
        "    PID       : %lu\n"
        "    Name      : %s\n"
        "    Exit code : %lu\n"
        "    Duration  : %llu ms\n",
        (unsigned long)p->pid,
        p->name,
        (unsigned long)p->exit_code,
        duration
    );


    if (exit_code == 0)
    {
        printf(
            "    Result    : SUCCESS\n"
        );
    }
    else
    {
        printf(
            "    Result    : FAILED\n"
        );
    }
}


/* ============================================================
                         EXCEPTION NAME
   ============================================================ */

const char *get_exception_name(
    DWORD code
)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:
            return "ACCESS_VIOLATION";

        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return "ARRAY_BOUNDS_EXCEEDED";

        case EXCEPTION_BREAKPOINT:
            return "BREAKPOINT";

        case EXCEPTION_DATATYPE_MISALIGNMENT:
            return "DATATYPE_MISALIGNMENT";

        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            return "FLOAT_DIVIDE_BY_ZERO";

        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "ILLEGAL_INSTRUCTION";

        case EXCEPTION_IN_PAGE_ERROR:
            return "IN_PAGE_ERROR";

        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "INTEGER_DIVIDE_BY_ZERO";

        case EXCEPTION_INT_OVERFLOW:
            return "INTEGER_OVERFLOW";

        case EXCEPTION_PRIV_INSTRUCTION:
            return "PRIVILEGED_INSTRUCTION";

        case EXCEPTION_STACK_OVERFLOW:
            return "STACK_OVERFLOW";

        default:
            return "UNKNOWN_EXCEPTION";
    }
}


/* ============================================================
                         EXCEPTION OUTPUT
   ============================================================ */

void print_exception(
    DEBUG_EVENT *event
)
{
    DWORD code;


    code =
        event
            ->u
            .Exception
            .ExceptionRecord
            .ExceptionCode;


    printf(
        "\n"
        "[EXCEPTION]\n"
        "    PID       : %lu\n"
        "    TID       : %lu\n"
        "    Code      : 0x%08lX\n"
        "    Type      : %s\n"
        "    First     : %s\n",
        (unsigned long)event->dwProcessId,
        (unsigned long)event->dwThreadId,
        (unsigned long)code,
        get_exception_name(code),
        event
            ->u
            .Exception
            .dwFirstChance
            ? "YES"
            : "NO"
    );
}


/* ============================================================
                         PROCESS TREE
   ============================================================ */

void print_tree(
    DWORD pid,
    int depth
)
{
    int i;

    ProcessRecord *p;


    p =
        find_process(pid);


    if (p == NULL)
    {
        return;
    }


    for (i = 0; i < depth; i++)
    {
        printf("    ");
    }


    printf(
        "%s [PID=%lu",
        p->name,
        (unsigned long)p->pid
    );


    if (p->exited)
    {
        printf(
            ", exit=%lu",
            (unsigned long)p->exit_code
        );
    }


    printf("]\n");


    /*
        Print children.
    */

    for (i = 0; i < process_count; i++)
    {
        if (process_table[i].ppid == pid)
        {
            print_tree(
                process_table[i].pid,
                depth + 1
            );
        }
    }
}


/* ============================================================
                         SUMMARY
   ============================================================ */

void print_summary()
{
    int i;

    int total = 0;
    int success = 0;
    int failed = 0;


    printf(
        "\n"
        "================================================\n"
        "                  TRACE SUMMARY\n"
        "================================================\n"
    );


    for (i = 0; i < process_count; i++)
    {
        ProcessRecord *p =
            &process_table[i];


        if (!p->exited)
        {
            continue;
        }


        total++;


        if (p->exit_code == 0)
        {
            success++;
        }
        else
        {
            failed++;
        }
    }


    printf(
        "Processes observed : %d\n",
        total
    );


    printf(
        "Successful         : %d\n",
        success
    );


    printf(
        "Failed             : %d\n",
        failed
    );


    printf(
        "\nProcess tree:\n\n"
    );


    print_tree(
        root_pid,
        0
    );


    printf(
        "\n"
        "================================================\n"
    );
}


/* ============================================================
                         EXTENSION CHECK
   ============================================================ */

int has_extension(
    const char *filename,
    const char *extension
)
{
    size_t filename_len;
    size_t extension_len;

    filename_len =
        strlen(filename);

    extension_len =
        strlen(extension);


    if (filename_len < extension_len)
    {
        return 0;
    }


    filename +=
        filename_len -
        extension_len;


    while (*filename &&
           *extension)
    {
        char a = *filename;
        char b = *extension;


        if (a >= 'A' && a <= 'Z')
        {
            a =
                a -
                'A' +
                'a';
        }


        if (b >= 'A' && b <= 'Z')
        {
            b =
                b -
                'A' +
                'a';
        }


        if (a != b)
        {
            return 0;
        }


        filename++;
        extension++;
    }


    return 1;
}


/* ============================================================
                         QUOTE ARGUMENT
   ============================================================ */

void append_quoted(
    char *buffer,
    const char *text
)
{
    strcat(
        buffer,
        "\""
    );


    strcat(
        buffer,
        text
    );


    strcat(
        buffer,
        "\""
    );
}


/* ============================================================
                    BUILD TARGET COMMAND
   ============================================================ */

void build_command_line(
    int argc,
    char *argv[],
    char *command_line
)
{
    int i;


    command_line[0] =
        '\0';


    /*
        ----------------------------------------------------
        BATCH FILE
        ----------------------------------------------------
    */

    if (has_extension(argv[1], ".bat") ||
        has_extension(argv[1], ".cmd"))
    {
        strcat(
            command_line,
            "cmd.exe /d /c \""
        );


        /*
            Script name.
        */

        strcat(
            command_line,
            argv[1]
        );


        /*
            Additional arguments.
        */

        for (i = 2; i < argc; i++)
        {
            strcat(
                command_line,
                " "
            );

            append_quoted(
                command_line,
                argv[i]
            );
        }


        strcat(
            command_line,
            "\""
        );


        return;
    }


    /*
        ----------------------------------------------------
        POWERSHELL SCRIPT
        ----------------------------------------------------
    */

    if (has_extension(argv[1], ".ps1"))
    {
        strcat(
            command_line,
            "powershell.exe "
        );


        strcat(
            command_line,
            "-NoProfile "
        );


        strcat(
            command_line,
            "-ExecutionPolicy Bypass "
        );


        strcat(
            command_line,
            "-File "
        );


        append_quoted(
            command_line,
            argv[1]
        );


        for (i = 2; i < argc; i++)
        {
            strcat(
                command_line,
                " "
            );


            append_quoted(
                command_line,
                argv[i]
            );
        }


        return;
    }


    /*
        ----------------------------------------------------
        NORMAL EXECUTABLE
        ----------------------------------------------------
    */

    append_quoted(
        command_line,
        argv[1]
    );


    for (i = 2; i < argc; i++)
    {
        strcat(
            command_line,
            " "
        );


        append_quoted(
            command_line,
            argv[i]
        );
    }
}


/* ============================================================
                         DEBUG LOOP
   ============================================================ */

int run_debugger()
{
    DEBUG_EVENT event;

    int running = 1;


    while (running)
    {
        DWORD continue_status;


        memset(
            &event,
            0,
            sizeof(event)
        );


        /*
            Wait for Windows to send a debugging event.
        */

        if (!WaitForDebugEvent(
                &event,
                INFINITE))
        {
            printf(
                "\n"
                "[ERROR] WaitForDebugEvent failed: %lu\n",
                (unsigned long)GetLastError()
            );

            return 1;
        }


        /*
            Default response.
        */

        continue_status =
            DBG_CONTINUE;


        /* =================================================
                         PROCESS CREATED
           ================================================= */

        if (event.dwDebugEventCode ==
            CREATE_PROCESS_DEBUG_EVENT)
        {
            char path[MAX_PATH_LEN];

            DWORD ppid;

            ProcessRecord *p;


            memset(
                path,
                0,
                sizeof(path)
            );


            /*
                Obtain executable path.
            */

            get_process_path(
                event
                    .u
                    .CreateProcessInfo
                    .hProcess,
                path,
                MAX_PATH_LEN
            );


            /*
                Determine parent PID.

                The root process is the process
                launched by ShellTrace.

                Child processes are queried through
                NtQueryInformationProcess.
            */

            if (event.dwProcessId ==
                root_pid)
            {
                ppid = 0;
            }
            else
            {
                ppid =
                    get_parent_pid(
                        event.dwProcessId
                    );
            }


            /*
                Add process.
            */

            p =
                add_process(
                    event.dwProcessId,
                    ppid,
                    path
                );


            /*
                Display process.
            */

            if (p != NULL)
            {
                print_process_start(p);
            }


            /*
                Windows gives the debugger an image
                file handle during CREATE_PROCESS.
            */

            if (event
                    .u
                    .CreateProcessInfo
                    .hFile != NULL)
            {
                CloseHandle(
                    event
                        .u
                        .CreateProcessInfo
                        .hFile
                );
            }
        }


        /* =================================================
                         PROCESS EXITED
           ================================================= */

        else if (
            event.dwDebugEventCode ==
            EXIT_PROCESS_DEBUG_EVENT)
        {
            ProcessRecord *p;


            p =
                find_process(
                    event.dwProcessId
                );


            if (p != NULL)
            {
                print_process_exit(
                    p,
                    event
                        .u
                        .ExitProcess
                        .dwExitCode
                );
            }


            /*
                Root process exited.
            */

            if (event.dwProcessId ==
                root_pid)
            {
                running = 0;
            }
        }


        /* =================================================
                           EXCEPTION
           ================================================= */

        else if (
            event.dwDebugEventCode ==
            EXCEPTION_DEBUG_EVENT)
        {
            print_exception(
                &event
            );


            /*
                Give the exception back to the
                application's normal exception handler.
            */

            continue_status =
                DBG_EXCEPTION_NOT_HANDLED;
        }


        /* =================================================
                         THREAD CREATED
           ================================================= */

        else if (
            event.dwDebugEventCode ==
            CREATE_THREAD_DEBUG_EVENT)
        {
            /*
                ShellTrace currently tracks processes,
                not individual threads.
            */
        }


        /* =================================================
                         THREAD EXITED
           ================================================= */

        else if (
            event.dwDebugEventCode ==
            EXIT_THREAD_DEBUG_EVENT)
        {
            /*
                Intentionally ignored.
            */
        }


        /* =================================================
                            DLL LOAD
           ================================================= */

        else if (
            event.dwDebugEventCode ==
            LOAD_DLL_DEBUG_EVENT)
        {
            /*
                Intentionally ignored in v1.
            */
        }


        /* =================================================
                           DLL UNLOAD
           ================================================= */

        else if (
            event.dwDebugEventCode ==
            UNLOAD_DLL_DEBUG_EVENT)
        {
            /*
                Intentionally ignored.
            */
        }


        /* =================================================
                       DEBUG STRING
           ================================================= */

        else if (
            event.dwDebugEventCode ==
            OUTPUT_DEBUG_STRING_EVENT)
        {
            /*
                Reserved for future implementation.
            */
        }


        /* =================================================
                          RIP EVENT
           ================================================= */

        else if (
            event.dwDebugEventCode ==
            RIP_EVENT)
        {
            printf(
                "\n[RIP EVENT]\n"
            );
        }


        /*
            Resume execution after processing
            the debug event.
        */

        ContinueDebugEvent(
            event.dwProcessId,
            event.dwThreadId,
            continue_status
        );
    }


    return 0;
}


/* ============================================================
                              MAIN
   ============================================================ */

int main(
    int argc,
    char *argv[]
)
{
    STARTUPINFOA startup;

    PROCESS_INFORMATION process;

    char command_line[MAX_COMMAND];

    int result;


    /*
        ----------------------------------------------------
        HEADER
        ----------------------------------------------------
    */

    printf(
        "\n"
        "================================================\n"
        "                 SHELLTRACE\n"
        "        Windows Process Execution Tracer\n"
        "================================================\n\n"
    );


    /*
        ----------------------------------------------------
        ARGUMENT CHECK
        ----------------------------------------------------
    */

    if (argc < 2)
    {
        printf(
            "Usage:\n\n"
            "  shelltrace.exe script.bat\n"
            "  shelltrace.exe script.cmd\n"
            "  shelltrace.exe script.ps1\n"
            "  shelltrace.exe program.exe\n\n"
        );

        return 1;
    }


    /*
        ----------------------------------------------------
        BUILD COMMAND
        ----------------------------------------------------
    */

    build_command_line(
        argc,
        argv,
        command_line
    );


    printf(
        "Target:\n"
        "  %s\n\n",
        command_line
    );


    /*
        ----------------------------------------------------
        INITIALIZE WINDOWS STRUCTURES
        ----------------------------------------------------
    */

    memset(
        &startup,
        0,
        sizeof(startup)
    );


    memset(
        &process,
        0,
        sizeof(process)
    );


    startup.cb =
        sizeof(startup);


    /*
        ----------------------------------------------------
        START TARGET UNDER WINDOWS DEBUGGER
        ----------------------------------------------------

        DEBUG_PROCESS causes Windows to send debugging
        events to ShellTrace.
    */

    if (!CreateProcessA(
            NULL,
            command_line,
            NULL,
            NULL,
            FALSE,
            DEBUG_PROCESS,
            NULL,
            NULL,
            &startup,
            &process))
    {
        printf(
            "\n"
            "[ERROR] CreateProcess failed.\n"
            "Windows error code: %lu\n",
            (unsigned long)GetLastError()
        );

        return 1;
    }


    /*
        Store root PID.
    */

    root_pid =
        process.dwProcessId;


    printf(
        "Root PID:\n"
        "  %lu\n",
        (unsigned long)root_pid
    );


    printf(
        "\n"
        "Tracing...\n"
    );


    /*
        ----------------------------------------------------
        DEBUG EVENT LOOP
        ----------------------------------------------------
    */

    result =
        run_debugger();


    /*
        The debugger owns the process/thread handles
        associated with debug events.
    */

    printf(
        "\n"
        "================================================\n"
        "                 TRACE COMPLETE\n"
        "================================================\n"
    );


    /*
        ----------------------------------------------------
        SUMMARY
        ----------------------------------------------------
    */

    print_summary();


    return result;
}
