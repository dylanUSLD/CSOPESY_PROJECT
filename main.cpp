#include <iostream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <memory>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <chrono>

using namespace std;

void printHeader() {
    cout << " _____  _____   ____  _____  ______  _______     __" << endl;
    cout << "/ ____|/ ____| / __ \\|  __ \\|  ____|/ ____\\ \\   / /" << endl;
    cout << "| |    | (___ | |  | | |__) | |__  | (___  \\ \\_/ /" << endl;
    cout << "| |     \\___ \\| |  | |  ___/|  __|  \\___ \\  \\   /" << endl;
    cout << "| |____ ____) | |__| | |    | |____ ____) |  | |" << endl;
    cout << " \\_____|_____/ \\____/|_|    |______|_____/   |_|" << endl;
    cout << "\033[32m";
    cout << "Hello, Welcome to CSOPESY command line!" << endl;
    cout << "\033[33m";
    cout << "Type 'exit' to quit, 'clear' to clear the screen" << endl;
    cout << "\033[0m";
}

void clearScreen() {
    cout << "\033[2J\033[1;1H";
}

string generateTimestamp() {
    auto now = time(nullptr);
    tm localTime;
    localtime_r(&now, &localTime);
    stringstream ss;
    ss << put_time(&localTime, "%m/%d/%Y %I:%M:%S%p");
    return ss.str();
}

struct Process {
    string name;
    int currentLine = 0;
    int totalLine = 100;
    string timestamp;
    int coreAssigned = -1;
    bool isFinished = false;
    string finishedTime;
};

void printProcessDetails(const Process& proc) {
    cout << "Process: " << proc.name << endl;
    cout << "Instruction: " << proc.currentLine << " of " << proc.totalLine << endl;
    cout << "Created: " << proc.timestamp << endl;
    cout << "\033[33m";
    cout << "Type 'exit' to quit, 'clear' to clear the screen" << endl;
    cout << "\033[0m";
}

void displayProcess(const Process& proc) {
    printProcessDetails(proc);
    string subCommand;
    while (true) {
        cout << "Enter a command: ";
        getline(cin, subCommand);
        if (subCommand == "exit") break;
        else if (subCommand == "clear") {
            clearScreen();
            printProcessDetails(proc);
        }
        else {
            cout << "Unknown command inside process view." << endl;
        }
    }
}

class ProcessManager {
private:
    unordered_map<string, unique_ptr<Process>> processes;
public:
    void createProcess(string name) {
        if (processes.find(name) != processes.end()) {
            cout << "Process " << name << " already exists." << endl;
            return;
        }
        processes[name] = make_unique<Process>(Process{ name, 0, 100, generateTimestamp() });
    }

    Process* retrieveProcess(const string& name) {
        auto it = processes.find(name);
        return it != processes.end() ? it->second.get() : nullptr;
    }

    void listProcesses() {
        cout << "-----------------------------\n";
        cout << "Running processes:\n";
        for (const auto& [name, proc] : processes) {
            if (!proc->isFinished && proc->coreAssigned != -1) {
                cout << name << " (" << proc->timestamp << ") "
                    << "Core: " << proc->coreAssigned << " "
                    << proc->currentLine << " / " << proc->totalLine << endl;
            }
        }
        cout << "\nFinished processes:\n";
        for (const auto& [name, proc] : processes) {
            if (proc->isFinished) {
                cout << name << " (" << proc->finishedTime << ") Finished "
                    << proc->totalLine << " / " << proc->totalLine << endl;
            }
        }
        cout << "-----------------------------\n";
    }
};

queue<Process*> fcfsQueue;
mutex queueMutex;
condition_variable cv;
bool stopScheduler = false;

void cpuWorker(int coreId) {
    while (!stopScheduler) {
        Process* proc = nullptr;
        {
            unique_lock<mutex> lock(queueMutex);
            cv.wait(lock, [] { return !fcfsQueue.empty() || stopScheduler; });
            if (!fcfsQueue.empty()) {
                proc = fcfsQueue.front();
                fcfsQueue.pop();
            }
        }
        if (proc) {
            proc->coreAssigned = coreId;
            for (int i = 0; i < proc->totalLine; ++i) {
                proc->currentLine++;
                this_thread::sleep_for(chrono::milliseconds(40));
            }
            proc->isFinished = true;
            proc->finishedTime = generateTimestamp();
        }
    }
}

void handleScreenCommand(const string& command, ProcessManager& manager) {
    istringstream iss(command);
    string cmd, option, processName;
    iss >> cmd >> option >> processName;

    if (option == "-ls") {
        manager.listProcesses();
    }
    else if (option == "-s" && !processName.empty()) {
        manager.createProcess(processName);
        Process* proc = manager.retrieveProcess(processName);
        if (proc) {
            lock_guard<mutex> lock(queueMutex);
            fcfsQueue.push(proc);
            displayProcess(*proc);
            printHeader();
        }
        cv.notify_one();
    }
    else if (option == "-r" && !processName.empty()) {
        Process* proc = manager.retrieveProcess(processName);
        if (proc) {
            displayProcess(*proc);
            printHeader();
        }
        else {
            cout << "Process " << processName << " not found." << endl;
        }
    }
    else {
        cout << "[screen] Invalid usage." << endl;
    }
}

void scheduler_start(ProcessManager& manager){
    // Automatically create N processes and queue them for running
    int processCountName = 1;
    while (!stopScheduler) {
        // Interruptible sleep/frequency
        for (int frequency = 0; frequency < 30 && !stopScheduler; ++frequency) {
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        if (stopScheduler) break;

        while (!stopScheduler) {
            string procName = "process" + (processCountName < 10 ? "0" + to_string(processCountName) : to_string(processCountName));
            
                // checks if process already exists
            if (manager.retrieveProcess(procName) == nullptr){
                    // creates dummy process 
                manager.createProcess(procName);
                    // adds the created process to the queue
                Process* proc = manager.retrieveProcess(procName);
                if (proc) {
                    lock_guard<mutex> lock(queueMutex);
                    fcfsQueue.push(proc);
                }
                cv.notify_one();
                ++processCountName;
                break;
            } 
            else {
                ++processCountName;
            }
        }
    }
}

int main() {
    ProcessManager manager;
    thread scheduler_start_thread;
    bool schedulerRunning = false;

    printHeader();

    vector<thread> cpuThreads;
    for (int i = 0; i < 4; ++i) {
        cpuThreads.emplace_back(cpuWorker, i);
    }

    string command;
    while (true) {
        cout << "Enter a command: ";
        getline(cin, command);

        if (command == "initialize") {
            cout << "initialize command recognized. Doing something." << endl;
        }
        else if (command.rfind("screen", 0) == 0) {
            handleScreenCommand(command, manager);
        }
        else if (command == "scheduler-start") {
            if (!schedulerRunning) {
                schedulerRunning = true;
                scheduler_start_thread = thread(scheduler_start, ref(manager));
            }
            else {
                cout << "Scheduler is already running!" << endl;
            }
        }
        else if (command == "scheduler-stop") {
            cout << "scheduler-stop command recognized. Doing something." << endl;
            if (schedulerRunning){
                stopScheduler = true;
                schedulerRunning = false;
                cv.notify_all();
                scheduler_start_thread.join();
                stopScheduler = false;
            }
            else {
                cout << "Scheduler is not running." << endl;
            }
        }
        else if (command == "clear") {
            clearScreen();
            printHeader();
        }
        else if (command == "exit") {
            cout << "exit command recognized. Exiting CSOPESY command line." << endl;
            break;
        }
        else if (command.rfind("print ", 0) == 0) {
            string procName = command.substr(6);
            Process* proc = manager.retrieveProcess(procName);
            if (proc) {
                {
                    lock_guard<mutex> lock(queueMutex);
                    fcfsQueue.push(proc);
                }
                cv.notify_one();
            }
            else {
                cout << "Process " << procName << " not found." << endl;
            }
        }
        else {
            cout << "Unknown command." << endl;
        }
    }
    stopScheduler = true;
    cv.notify_all();
    for (auto& t : cpuThreads) t.join();
    

    return 0;
}
