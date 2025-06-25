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
#include <random>

using namespace std;

struct SystemConfig {
    int numCPU = -1;                     // Sentinel: -1 means "not set"
    string scheduler = "";               // Empty string = "not set"
    uint32_t quantumCycles = 0;          // 0 = "not set"
    uint32_t batchProcessFreq = 0;
    uint32_t minInstructions = 0;
    uint32_t maxInstructions = 0;
    uint32_t delayPerExec = 0;
};

// Declare the global instance
SystemConfig GLOBAL_CONFIG;

bool loadSystemConfig(const string& filename = "config.txt") {
    ifstream file(filename);
    if (!file.is_open()) {
        cerr << "Error: Could not open config.txt" << endl;
        return false;
    }

    string key;
    while (file >> key) {
        if (key == "num-cpu") {
            int value;
            file >> value;
            if (value < 1 || value > 128) {
                cerr << "Invalid num-cpu value. Must be 1–128." << endl;
                return false;
            }
            GLOBAL_CONFIG.numCPU = value;
        }
        else if (key == "scheduler") {
            string value;
            file >> value;
            if (value != "fcfs" && value != "rr") {
                cerr << "Invalid scheduler. Must be 'fcfs' or 'rr'." << endl;
                return false;
            }
            GLOBAL_CONFIG.scheduler = value;
        }
        else if (key == "quantum-cycles") {
            uint32_t value;
            file >> value;
            GLOBAL_CONFIG.quantumCycles = value;
        }
        else if (key == "batch-process-freq") {
            uint32_t value;
            file >> value;
            GLOBAL_CONFIG.batchProcessFreq = value;
        }
        else if (key == "min-ins") {
            uint32_t value;
            file >> value;
            GLOBAL_CONFIG.minInstructions = value;
        }
        else if (key == "max-ins") {
            uint32_t value;
            file >> value;
            GLOBAL_CONFIG.maxInstructions = value;
        }
        else if (key == "delay-per-exec") {
            uint32_t value;
            file >> value;
            GLOBAL_CONFIG.delayPerExec = value;
        }
        else {
            cerr << "Unknown config key: " << key << endl;
            return false;
        }
    }

    // Final validation
    if (GLOBAL_CONFIG.minInstructions > GLOBAL_CONFIG.maxInstructions) {
        cerr << "min-ins cannot be greater than max-ins." << endl;
        return false;
    }

    return true;
}

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
    localtime_s(&localTime , &now);
    stringstream ss;
    ss << put_time(&localTime, "%m/%d/%Y %I:%M:%S%p");
    return ss.str();
}

int cpuBurstGenerator() {//if func will be use in scheduler start, then change void to int and return cpuBurst
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(GLOBAL_CONFIG.minInstructions, GLOBAL_CONFIG.maxInstructions);

    int cpuBurst = distrib(gen);

    //cout << "Generated CPU Burst: " << cpuBurst << endl;

    return cpuBurst;
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
        processes[name] = make_unique<Process>(Process{ name, 0, cpuBurstGenerator(), generateTimestamp()});
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
queue<Process*> rrQueue;
mutex queueMutex;
condition_variable cv;
bool stopScheduler = false;

void cpuWorker(int coreId) {
    while (!stopScheduler) {
        Process* proc = nullptr;
        {
            unique_lock<mutex> lock(queueMutex);
            cv.wait(lock, [] { return (!fcfsQueue.empty() || !rrQueue.empty()) || stopScheduler; });
            
            if (GLOBAL_CONFIG.scheduler == "fcfs" && !fcfsQueue.empty()) {
                proc = fcfsQueue.front();
                fcfsQueue.pop();
            }
            else if (GLOBAL_CONFIG.scheduler == "rr" && !rrQueue.empty()) {
                proc = rrQueue.front();
                rrQueue.pop();
            }
        }

        if (proc) {
            proc->coreAssigned = coreId;
            
            if (GLOBAL_CONFIG.scheduler == "fcfs") {
                while (proc->currentLine < proc->totalLine && !stopScheduler) {
                    proc->currentLine++;
                    this_thread::sleep_for(chrono::milliseconds(GLOBAL_CONFIG.delayPerExec));
                }
            }
            else if (GLOBAL_CONFIG.scheduler == "rr") {
                int executedInstructions = 0;
                while (proc->currentLine < proc->totalLine && 
                       executedInstructions < GLOBAL_CONFIG.quantumCycles && 
                       !stopScheduler) {
                    proc->currentLine++;
                    executedInstructions++;
                    this_thread::sleep_for(chrono::milliseconds(GLOBAL_CONFIG.delayPerExec));
                }
                if (proc->currentLine < proc->totalLine) {
                    lock_guard<mutex> lock(queueMutex);
                    rrQueue.push(proc);
                    cv.notify_one();
                    continue;
                }
            }

            proc->isFinished = true;
            proc->finishedTime = generateTimestamp();
        }
        manager.listProcesses();
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
            if (GLOBAL_CONFIG.scheduler == "fcfs") {
                fcfsQueue.push(proc);
            } else if (GLOBAL_CONFIG.scheduler == "rr") {
                rrQueue.push(proc);
            }
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

void scheduler_start(ProcessManager& manager) {
    // Automatically create N processes and queue them for running
    int processCountName = 1;
    while (!stopScheduler) {
        // Interruptible sleep/frequency
        for (int frequency = 0; frequency < 2 && !stopScheduler; ++frequency) {
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        if (stopScheduler) break;

        while (!stopScheduler) {
            string procName = "process" + (processCountName < 10 ? "0" + to_string(processCountName) : to_string(processCountName));

            if (manager.retrieveProcess(procName) == nullptr) {
                manager.createProcess(procName);
                Process* proc = manager.retrieveProcess(procName);
                if (proc) {
                    lock_guard<mutex> lock(queueMutex);
                    if (GLOBAL_CONFIG.scheduler == "fcfs") {
                        fcfsQueue.push(proc);
                    } else if (GLOBAL_CONFIG.scheduler == "rr") {
                        rrQueue.push(proc);
                    }
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
    for (int i = 0; i < 1; ++i) {
        cpuThreads.emplace_back(cpuWorker, i);
    }

    bool confirmInitialize = false;
    string command;

    while (true) {
        cout << "Enter a command: ";
        getline(cin, command);

        if (command == "initialize") {
            if (loadSystemConfig()) {
                cout << "\n System configuration loaded successfully:\n";
                cout << "--------------------------------------------\n";
                cout << "- num-cpu:            " << GLOBAL_CONFIG.numCPU << "\n";
                cout << "- scheduler:          " << GLOBAL_CONFIG.scheduler << "\n";
                cout << "- quantum-cycles:     " << GLOBAL_CONFIG.quantumCycles << "\n";
                cout << "- batch-process-freq: " << GLOBAL_CONFIG.batchProcessFreq << "\n";
                cout << "- min-ins:            " << GLOBAL_CONFIG.minInstructions << "\n";//event for min ins larger than max ins
                cout << "- max-ins:            " << GLOBAL_CONFIG.maxInstructions << "\n";
                cout << "- delay-per-exec:     " << GLOBAL_CONFIG.delayPerExec << " ms\n";
                cout << "--------------------------------------------\n";
                confirmInitialize = true;
                break;
            }
            else {
                cout << " Failed to load system configuration.\n";
            }

        }
        else if (command == "exit") {
            cout << "exit command recognized. Exiting CSOPESY command line." << endl;
            break;
        }
        else {
            cout << "Unknown command." << endl;
        }
    }

    while (true && confirmInitialize == true) {
        cout << "Enter a command: ";
        getline(cin, command);

        if (command.rfind("screen", 0) == 0) {
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
            if (schedulerRunning) {
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