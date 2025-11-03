#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <fcntl.h>
#include <limits.h>
#include <ctime>
#include <cstring>
#include <signal.h>
#include "Tokenizer.h"

using namespace std;

string getPrompt() {
    const char* user = getenv("USER");
    if (!user) user = "root"; // fallback

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");

    time_t now = time(nullptr);
    char* timeStr = ctime(&now); // "Sun Nov  2 22:16:34 2025\n"
    string t(timeStr);
    if (!t.empty() && t.back() == '\n') t.pop_back();

    string formattedTime = t.substr(4, 15); // "Nov  2 22:16:34"

    return formattedTime + " " + user + ":" + cwd + "$ ";
}

int main() {
    string prevDir;
    signal(SIGCHLD, SIG_IGN); // auto-reap background children

    while (true) {
        cout << getPrompt();
        cout.flush();

        string input;
        if (!getline(cin, input)) {
            cout << endl;
            break;
        }
        if (input.empty()) continue;
        if (input == "exit") {
            cout << "Now exiting shell...\nGoodbye\n";
            break;
        }

        Tokenizer tokenizer(input);
        if (tokenizer.hasError() || tokenizer.commands.empty()) continue;

        int numCmds = tokenizer.commands.size();

        // ----- Built-in cd -----
        if (numCmds == 1) {
            Command* cmd = tokenizer.commands[0];
            if (!cmd->args.empty() && cmd->args[0] == "cd") {
                char cwdBuf[PATH_MAX];
                getcwd(cwdBuf, sizeof(cwdBuf));
                string currentDir(cwdBuf);

                if (cmd->args.size() > 1 && cmd->args[1] == "-") {
                    if (!prevDir.empty() && chdir(prevDir.c_str()) == 0)
                        cout << prevDir << endl;
                } else {
                    const char* path = (cmd->args.size() > 1) ? cmd->args[1].c_str() : getenv("HOME");
                    if (chdir(path) == 0) prevDir = currentDir;
                }
                continue;
            }
        }

        // ----- Set up pipes -----
        vector<int> pipes(2 * (numCmds - 1), -1);
        for (int i = 0; i < numCmds - 1; ++i)
            if (pipe(&pipes[2*i]) < 0) perror("pipe");

        vector<pid_t> pids;

        // ----- Fork each command -----
        for (int i = 0; i < numCmds; ++i) {
            Command* cmd = tokenizer.commands[i];
            if (cmd->args.empty()) continue;

            vector<char*> argv;
            for (auto& arg : cmd->args) argv.push_back((char*)arg.c_str());
            argv.push_back(nullptr);

            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                continue;
            }

            if (pid == 0) { // CHILD
                // Input redirection (first command only)
                if (i == 0 && cmd->hasInput()) {
                    int fd_in = open(cmd->in_file.c_str(), O_RDONLY);
                    if (fd_in < 0) { perror("input failed"); _exit(1); }
                    dup2(fd_in, STDIN_FILENO);
                    close(fd_in);
                }
                // Output redirection (last command only)
                if (i == numCmds - 1 && cmd->hasOutput()) {
                    int fd_out = open(cmd->out_file.c_str(),
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd_out < 0) { perror("output failed"); _exit(1); }
                    dup2(fd_out, STDOUT_FILENO);
                    close(fd_out);
                }

                // Pipe connections
                if (i > 0) dup2(pipes[2*(i-1)], STDIN_FILENO);
                if (i < numCmds - 1) dup2(pipes[2*i+1], STDOUT_FILENO);

                // Close all pipes in child
                for (int fd : pipes) if (fd != -1) close(fd);

                execvp(argv[0], argv.data());
                perror("execvp failed");
                _exit(1);
            } else {
                // Parent
                pids.push_back(pid);
            }
        }

        // Close all pipes in parent
        for (int fd : pipes) if (fd != -1) close(fd);

        // Background check
        bool background = tokenizer.commands.back()->isBackground();

        if (background) {
            // Parent returns immediately
        } else {
            for (pid_t pid : pids)
                waitpid(pid, nullptr, 0);
        }
    }
    return 0;
}
