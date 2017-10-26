#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#else
#include <paths.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// fail if condition is not true.
#define CHECK(c) do { if (!(c)) { std::cerr << "Failed: " #c "\n"; exit(1); } } while (0)

void usage(char * const *argv) {
    std::cerr << "Usage: " << argv[0] << " [--success|--error|--warning] [--dir TMPDIR] [--echo_output] test_executable_path\n";
    exit(1);
}

#ifdef _WIN32
void read_handle(HANDLE h, std::ostringstream* out) {
    CHAR buf[BUFSIZE];
    std::ostringstream out;
    for (;;) {
        DWORD read_count = 0;
        if (!ReadFile(h, buf, BUFSIZE, &read_count, NULL) || read_count == 0) {
            break;
        }
        out->write(buf, read_count);
    }
}
#endif

int run_test(const std::string &test_executable,
             const std::string &working_dir,
             std::string *test_output) {
    std::ostringstream out, err;
#ifdef _WIN32
    HANDLE child_stdout_rd = NULL;
    HANDLE child_stdout_wr = NULL;
    HANDLE child_stderr_rd = NULL;
    HANDLE child_stderr_wr = NULL;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    CHECK(::CreatePipe(&child_stderr_rd, &child_stderr_wr, &sa, 0));
    CHECK(::CreatePipe(&child_stdout_rd, &child_stdout_wr, &sa, 0));
    // Prevent inheritance.
    CHECK(::SetHandleInformation(child_stderr_rd, HANDLE_FLAG_INHERIT, 0));
    CHECK(::SetHandleInformation(child_stdout_rd, HANDLE_FLAG_INHERIT, 0));

    STARTUPINFOA startup_info;
    memset(&startup_info, 0, sizeof(STARTUPINFOA));
    startup_info.cb = sizeof(STARTUPINFOA);
    startup_info.dwFlags |= STARTF_USESTDHANDLES;
    startup_info.hStdOutput = child_stdout_wr;
    startup_info.hStdError = child_stderr_wr;

    PROCESS_INFORMATION process_info;
    CHECK(::CreateProcessA(
        const_cast<char*>(test_executable.c_str()),
        NULL,
        NULL,
        NULL,
        TRUE,
        0x0,
        NULL,
        NULL,
        &startup_info,
        &process_info));
    ::CloseHandle(process_info.hThread);
    ::CloseHandle(child_stderr_wr);
    ::CloseHandle(child_stdout_wr);

    // if (::WaitForSingleObject(process_info.hProcess, INFINITE) != WAIT_OBJECT_0) {
    //     std::cerr << "WaitForSingleObject failed.\n";
    //     exit(1);
    // }

    read_handle(child_stdout_rd, &out);
    read_handle(child_stderr_rd, &err);
    if (test_output) *test_output = out.str() + err.str();

    DWORD status_code;
    CHECK(::GetExitCodeProcess(process_info.hProcess, &status_code));
    ::CloseHandle(process_info.hProcess);

    return (int) status_code;
#else
    std::string cmd = test_executable + " 2>&1";

    if (!working_dir.empty()) {
        cmd = "cd " + working_dir + " && " + cmd;
    }

    FILE *f = popen(cmd.c_str(), "r");
    CHECK(f != NULL);

    constexpr int BUF_MAX = 256;
    char buf[BUF_MAX+2];
    while (fgets(buf, BUF_MAX, f) != NULL) {
        out << buf;
    }

    int status_code = pclose(f);
    if (test_output) *test_output = out.str();
    return WEXITSTATUS(status_code);
#endif
}

}  // namespace

int main(int argc, char * const *argv) {
    enum TestMode {
        ExpectSuccess,
        ExpectFailure,
        ExpectWarning,
        Unknown
    };

    std::string test_executable, dir;
    TestMode test_mode = Unknown;
    bool echo_output = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg[0] == '-') {
            if (arg == "--success") {
                test_mode = ExpectSuccess;
            } else if (arg == "--error") {
                test_mode = ExpectFailure;
            } else if (arg == "--warning") {
                test_mode = ExpectWarning;
            } else if (arg == "--echo_output") {
                echo_output = true;
            } else if (arg == "--dir") {
                if (!dir.empty()) {
                    usage(argv);
                }
                CHECK(i+1 < argc);
                dir = argv[i+1];
                ++i;
            } else {
                usage(argv);
            }
        } else {
            if (!test_executable.empty()) {
                usage(argv);
            }
            test_executable = arg;
        }
    }
    if (test_mode == Unknown || test_executable.empty()) {
        usage(argv);
    }

    const std::string warning_prefix = "Warning:";
    std::string test_output;
    int result = run_test(test_executable, dir, &test_output);
    switch (test_mode) {
    case ExpectSuccess:
        break;
    case ExpectFailure:
        result = result ? 0 : -1;
        break;
    case ExpectWarning:
        // Either stdout or stderr must begin with "Warning:"
        if (test_output.substr(0, warning_prefix.size()) != warning_prefix) {
            result = -1;
        }
        break;
    case Unknown:
        abort();
        break;
    }

    std::string fname = test_executable;
    std::replace_if(fname.begin(), fname.end(), [](int i) -> bool { return i == '\\'; }, '/');
    size_t last_of = fname.find_last_of("/");
    if (last_of != std::string::npos) {
        fname = fname.substr(last_of + 1);
    }
    if (result == 0) {
        if (echo_output) {
            std::cout << test_output;
            if (test_output.back() != '\n') std::cout << '\n';
        }
        std::cout << "\nTest Success: " << fname << "\n";
    } else {
        if (echo_output) {
            std::cerr << test_output;
            if (test_output.back() != '\n') std::cerr << '\n';
        }
        std::cerr << "\n*** Test Failure: " << fname << " ***\n";
    }

    return result;
}
