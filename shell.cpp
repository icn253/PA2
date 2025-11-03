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
    //Get username
    const char* user = getenv("USER");
    if (!user) user = "unknown";

    //Get current working directory
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        strcpy(cwd, "?");
    }

    //Get current date/time
    time_t now = time(nullptr);
    char* timeStr = ctime(&now);

    string timeString(timeStr);
    if (!timeString.empty() && timeString.back() == '\n') {
        timeString.pop_back();
    }

    //Combine into prompt string
    string prompt = timeString + " " + user + ":" + cwd + "$ ";
    return prompt;
}

int main() {
    string prevDir; // store previous directory for 'cd -'

    for (;;) {
        cout << GREEN << getPrompt() << YELLOW << "Shell$ " << NC;

        string input;
        if (!getline(cin, input)) {
            cout << endl;
            break; // EOF received
        }

        if (input.empty()) continue; // ignore empty input
        if (input == "exit") {
            cout << RED << "Now exiting shell..." << endl << "Goodbye" << NC << endl;
            break;
        }

        Tokenizer tknr(input); 
        if (tknr.hasError() || tknr.commands.empty()) continue; // skip on error or no commands

        int numCmds = (int)tknr.commands.size();

        // Handle built-in 'cd' if it's a single command with 'cd'
        if (numCmds == 1) { 
            Command* only = tknr.commands[0]; 
            if (!only->args.empty() && only->args[0] == "cd") {
                char cwdBuf[PATH_MAX];
                getcwd(cwdBuf, sizeof(cwdBuf));
                string currentDir(cwdBuf);

                if (only->args.size() > 1 && only->args[1] == "-") {
                    if (!prevDir.empty()) {
                        if (chdir(prevDir.c_str()) != 0) perror("cd failed");
                        else cout << prevDir << endl;
                    }
                } else {
                    const char* path = only->args.size() > 1 ? only->args[1].c_str() : getenv("HOME");
                    if (chdir(path) != 0) perror("cd failed");
                    else prevDir = currentDir;
                }
                continue;
            }
        }

        // Set up pipes
        vector<int> pipes;
        if (numCmds > 1) pipes.resize(2 * (numCmds - 1));
        for (int i = 0; i < numCmds - 1; ++i) {
            if (pipe(&pipes[2*i]) < 0) {
                perror("pipe");
                
                for (int j = 0; j < 2*i; ++j){
                    if (pipes[j] != 0) close(pipes[j]);
                }
                pipes.clear();
                break;
            }
        }

        // Store child pids
        vector<pid_t> pids;
        pids.reserve(numCmds);

        // Fork and execute each command
        for (int i = 0; i < numCmds; ++i) {
            Command* cmd = tknr.commands[i];
            if (cmd->args.empty()) {
                continue;
            }

            // Prepare argv for execvp
            vector<char*> argv;
            for (auto &arg : cmd->args) argv.push_back((char*)arg.c_str());
            argv.push_back(nullptr);

            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                continue;
            }

            if (pid == 0) { // CHILD process
                // Set up pipes for stdin/stdout
                if (!pipes.empty()) {
                    // If not first command, set stdin to previous pipe read end
                    if (i > 0) {
                        int read_end = pipes[2*(i-1)];
                        dup2(read_end, STDIN_FILENO);
                    }
                    // If not last command, set stdout to this pipe write end
                    if (i < numCmds - 1) {
                        int write_end = pipes[2*i + 1];
                        dup2(write_end, STDOUT_FILENO);
                    }

                    // Close all pipe fds (file descriptors )in child
                    for (int fd : pipes) {
                        close(fd);
                    }
                }

                // Handle input redirection
                if (cmd->hasInput()) {
                    int fd_in = open(cmd->in_file.c_str(), O_RDONLY);
                    if (fd_in < 0) { perror("open input"); exit(1); }
                    dup2(fd_in, STDIN_FILENO);
                    close(fd_in);
                }

                // Handle output redirection
                if (cmd->hasOutput()) {
                    int fd_out = open(cmd->out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd_out < 0) { perror("open output"); exit(1); }
                    dup2(fd_out, STDOUT_FILENO);
                    close(fd_out);
                }

                // Execute command
                execvp(argv[0], argv.data());
                // If execvp returns, it failed
                perror("execvp failed");
                _exit(1);
            } else {
                // PARENT process
                pids.push_back(pid); 
            }
        }

        // Close all pipe fds in parent
        for (int fd : pipes) {
            if (fd != 0) close(fd);
        }

        // Check if last command is background
        bool background = false;
        if (!tknr.commands.empty()) {
            Command* last = tknr.commands.back();
            background = last->isBackground();
        }


        if (background) {
            for (pid_t background_pid : pids) {
                cout << "Process running in background: " << background_pid << endl;
            }
        } else {
            for (pid_t child : pids) {
                int status;
                waitpid(child, &status, 0); // wait for each child at very end
            }
        }
    }

    return 0;
}
