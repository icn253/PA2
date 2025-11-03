#include <iostream>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>
#include <string>

#include "Tokenizer.h"
#include <limits.h>
#include <ctime>
#include <cstring>
#include <fcntl.h>  // For open(), O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC



// all the basic colours for a shell prompt
#define RED     "\033[1;31m"
#define GREEN	"\033[1;32m"
#define YELLOW  "\033[1;33m"
#define BLUE	"\033[1;34m"
#define WHITE	"\033[1;37m"
#define NC      "\033[0m"

using namespace std;

string getPrompt() {
    // Get username
    const char* user = getenv("USER");
    if (!user) user = "root"; // fallback for Gradescope

    // Get current working directory
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        // don't print perror here in normal operation; fallback path
        strcpy(cwd, "?");
    }

    // Use time + strftime to guarantee leading zero on day
    time_t now = time(nullptr);
    struct tm *tm_info = localtime(&now);
    char timeBuf[32];
    // Format exactly "Nov 02 18:31:46"
    strftime(timeBuf, sizeof(timeBuf), "%b %d %H:%M:%S", tm_info);

    string prompt = string(timeBuf) + " " + user + ":" + cwd + "$ ";
    return prompt;
}


int main() {
    string prevDir; // for cd -
    signal(SIGCHLD, SIG_IGN); // auto-clean background processes

    while (true) {
        cout << getPrompt(); // print prompt only once per loop

        string input;
        if (!getline(cin, input)) {
            cout << endl;
            break; // EOF or Ctrl+D
        }

        if (input.empty()) continue;

        if (input == "exit") {
            cout << "Now exiting shell...\nGoodbye\n";
            break;
        }

        Tokenizer tokenizer(input);
        if (tokenizer.hasError() || tokenizer.commands.empty())
            continue;

        int numCmds = tokenizer.commands.size();

        // ----- Handle built-in cd -----
        if (numCmds == 1) {
            Command* cmd = tokenizer.commands[0];
            if (!cmd->args.empty() && cmd->args[0] == "cd") {
                char cwdBuf[PATH_MAX];
                getcwd(cwdBuf, sizeof(cwdBuf));
                string currentDir(cwdBuf);

                if (cmd->args.size() > 1 && cmd->args[1] == "-") {
                    if (!prevDir.empty()) {
                        if (chdir(prevDir.c_str()) != 0)
                            perror("cd failed");
                        else
                            cout << prevDir << endl;
                    }
                } else {
                    const char* path = (cmd->args.size() > 1)
                        ? cmd->args[1].c_str()
                        : getenv("HOME");
                    if (chdir(path) != 0)
                        perror("cd failed");
                    else
                        prevDir = currentDir;
                }
                continue;
            }
        }

        // ----- Set up pipes -----
        vector<int> pipes;
        if (numCmds > 1) pipes.resize(2 * (numCmds - 1));
        for (int i = 0; i < numCmds - 1; ++i) {
            if (pipe(&pipes[2 * i]) < 0) {
                perror("pipe");
                exit(1);
            }
        }

        vector<pid_t> pids;

        // ----- Fork and execute each command -----
        for (int i = 0; i < numCmds; ++i) {
            Command* cmd = tokenizer.commands[i];
            if (cmd->args.empty()) continue;

            // Build argv for execvp
            vector<char*> argv;
            for (auto &arg : cmd->args)
                argv.push_back((char*)arg.c_str());
            argv.push_back(nullptr);

            pid_t pid = fork();
            if (pid < 0) {
                perror("fork failed");
                continue;
            }

            if (pid == 0) { // CHILD
                // ----- Pipe connections -----
                if (i > 0) { // not first command → get input from previous pipe
                    dup2(pipes[2 * (i - 1)], STDIN_FILENO);
                }
                if (i < numCmds - 1) { // not last command → output to next pipe
                    dup2(pipes[2 * i + 1], STDOUT_FILENO);
                }

                // Close all pipes in child
                for (int fd : pipes) close(fd);

                // ----- Input redirection -----
                if (cmd->hasInput()) {
                    int fd_in = open(cmd->in_file.c_str(), O_RDONLY);
                    if (fd_in < 0) {
                        perror("input redirection failed");
                        exit(1);
                    }
                    dup2(fd_in, STDIN_FILENO);
                    close(fd_in);
                }

                // ----- Output redirection -----
                if (cmd->hasOutput()) {
                    int fd_out = open(cmd->out_file.c_str(),
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd_out < 0) {
                        perror("output redirection failed");
                        exit(1);
                    }
                    dup2(fd_out, STDOUT_FILENO);
                    close(fd_out);
                }

                execvp(argv[0], argv.data());
                perror("execvp failed");
                _exit(1);
            } else {
                // PARENT
                pids.push_back(pid);
            }
        }

        // Close all pipe file descriptors in parent
        for (int fd : pipes) close(fd);

        // ----- Check background -----
        bool background = false;
        if (!tokenizer.commands.empty()) {
            Command* last = tokenizer.commands.back();
            background = last->isBackground();
        }

        // ----- Wait for processes -----
        if (!background) {
            for (pid_t pid : pids)
                waitpid(pid, nullptr, 0);
        }
        // if background: do nothing (children are reaped by SIGCHLD handler)

    }

    return 0;
}
