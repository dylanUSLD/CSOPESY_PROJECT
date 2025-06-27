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

// clamp function
uint16_t clampUint16(int value) {
    return static_cast<uint16_t>(max(0, min(value, 65535)));
}


struct SystemConfig {
    int numCPU = -1;                     // Sentinel: -1 means "not set"
    string scheduler = "";               // Empty string = "not set"
    uint64_t quantumCycles = 0;          // 0 = "not set"
    uint64_t batchProcessFreq = 0;
    uint64_t minInstructions = 0;
    uint64_t maxInstructions = 0;
    uint64_t delayPerExec = 0;
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
            uint64_t value;
            file >> value;
            GLOBAL_CONFIG.quantumCycles = value;
        }
        else if (key == "batch-process-freq") {
            uint64_t value;
            file >> value;
            GLOBAL_CONFIG.batchProcessFreq = value;
        }
        else if (key == "min-ins") {
            uint64_t value;
            file >> value;
            GLOBAL_CONFIG.minInstructions = value;
        }
        else if (key == "max-ins") {
            uint64_t value;
            file >> value;
            GLOBAL_CONFIG.maxInstructions = value;
        }
        else if (key == "delay-per-exec") {
            uint64_t value;
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
#ifdef _WIN32   
    localtime_s(&localTime, &now); // Windows
#else
    localtime_r(&now, &localTime); // POSIX (macOS, Linux)
#endif
    stringstream ss;
    ss << put_time(&localTime, "%m/%d/%Y %I:%M:%S%p");
    return ss.str();
}

uint64_t cpuBurstGenerator() {
    std::random_device rd;
    std::mt19937_64 gen(rd()); // use 64-bit generator
    std::uniform_int_distribution<uint64_t> distrib(GLOBAL_CONFIG.minInstructions, GLOBAL_CONFIG.maxInstructions);

    return distrib(gen);
}


vector<string> process_instructions(uint64_t cpuBurst) {
    vector<string> instructions;
    unordered_map<string, uint16_t> declaredVars;
    vector<string> varNames;

    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> cmdDistrib(0, 5);
    uniform_int_distribution<> valDistrib(1, 100);

    for (uint64_t i = 0; i < cpuBurst; ++i) {
        int cmd = cmdDistrib(gen);
        stringstream ss;

        if (cmd == 1 || varNames.empty()) {
            // DECLARE
            string var = "v" + to_string(varNames.size());
            uint16_t val = valDistrib(gen);
            declaredVars[var] = val;
            varNames.push_back(var);
            ss << "DECLARE " << var << " " << val;
        }
        else if (cmd == 0 && !varNames.empty()) {
            // PRINT
            string var = varNames[gen() % varNames.size()];
            ss << "PRINT " << var;
        }
        else if (cmd == 2 && varNames.size() >= 2) {
            // ADD
            string a = varNames[gen() % varNames.size()];
            string b = varNames[gen() % varNames.size()];
            ss << "ADD " << a << " " << b;
        }
        else if (cmd == 3 && varNames.size() >= 2) {
            // SUBTRACT
            string a = varNames[gen() % varNames.size()];
            string b = varNames[gen() % varNames.size()];
            ss << "SUBTRACT " << a << " " << b;
        }
        else if (cmd == 4) {
            // SLEEP
            ss << "SLEEP 100";
        }
        else {
            // FOR loop
            if (!varNames.empty()) {
                string var = varNames[gen() % varNames.size()];
                ss << "FOR " << var << " 3";
            }
            else {
                string var = "v" + to_string(varNames.size());
                uint16_t val = valDistrib(gen);
                declaredVars[var] = val;
                varNames.push_back(var);
                ss << "DECLARE " << var << " " << val;
            }
        }

        instructions.push_back(ss.str());
    }
    return instructions;
}

void instructions_manager(uint64_t currentLine, vector<string>& instructions, unordered_map<string, uint16_t>& memory, const string& processName, int coreId) {
    // Ensure instructions has enough space to store the line
    if (instructions.size() <= currentLine)
        instructions.resize(currentLine + 1);

    string prefix = "(" + generateTimestamp() + ") Core: " + to_string(coreId) + " ";

    // Random instruction generation
    static thread_local mt19937 gen(random_device{}());
    uniform_int_distribution<> cmdDistrib(0, 5);
    uniform_int_distribution<> valDistrib(1, 100);

    stringstream ss;
    stringstream log;
    int cmd = cmdDistrib(gen);

    // Track declared vars
    static thread_local vector<string> varNames;

    if (cmd == 1 || varNames.empty()) {
        // DECLARE
        string var = "v" + to_string(varNames.size());
        uint16_t val = valDistrib(gen);
        memory[var] = val;
        varNames.push_back(var);
        log << "DECLARE " << var << " = " << val;
    }
    else if (cmd == 0 && !varNames.empty()) {
        // PRINT
        string var = varNames[gen() % varNames.size()];
        uint16_t val = memory.count(var) ? memory[var] : 0;
        log << "PRINT " << var << " = " << val;
    }
    else if (cmd == 2 && varNames.size() >= 2) {
        // ADD
        string a = varNames[gen() % varNames.size()];
        string b = varNames[gen() % varNames.size()];
        uint16_t valA = memory.count(a) ? memory[a] : 0;
        uint16_t valB = memory.count(b) ? memory[b] : 0;
        uint16_t result = clampUint16(valA + valB);
        string resultVar = "res" + to_string(currentLine);
        memory[resultVar] = result;
        log << "ADD " << a << "(" << valA << ") + " << b << "(" << valB << ") = " << result;
    }
    else if (cmd == 3 && varNames.size() >= 2) {
        // SUBTRACT
        string a = varNames[gen() % varNames.size()];
        string b = varNames[gen() % varNames.size()];
        uint16_t valA = memory.count(a) ? memory[a] : 0;
        uint16_t valB = memory.count(b) ? memory[b] : 0;
        uint16_t result = clampUint16(valA - valB);
        string resultVar = "res" + to_string(currentLine);
        memory[resultVar] = result;
        log << "SUBTRACT " << a << "(" << valA << ") - " << b << "(" << valB << ") = " << result;
    }
    else if (cmd == 4) {
        // SLEEP
        int ms = 100;
        this_thread::sleep_for(chrono::milliseconds(ms));
        log << "SLEPT for " << ms << "ms";
    }
    else {
        // FOR
        if (varNames.empty()) {
            string var = "v" + to_string(varNames.size());
            uint16_t val = valDistrib(gen);
            memory[var] = val;
            varNames.push_back(var);
        }

        string var = varNames[gen() % varNames.size()];
        int count = 3;
        if (!memory.count(var)) memory[var] = 0;

        log << "FOR loop on " << var << ": ";
        for (int i = 0; i < count; ++i) {
            memory[var]++;
            log << "[" << i + 1 << "]=" << memory[var] << " ";
        }
    }

    // Save result in instruction log
    instructions[currentLine] = prefix + "\"" + log.str() + "\"";
}


struct Process {
    int id;
    string name;
    uint64_t currentLine = 0;
    uint64_t totalLine = 100;
    string timestamp;
    int coreAssigned = -1;
    bool isFinished = false;
    string finishedTime;
    vector<string> instructions;
    unordered_map<string, uint16_t> memory;
};

void printProcessDetails(const Process& proc) {
    cout << "Process: " << proc.name << endl;
    cout << "ID: " << proc.id << endl;
    cout << "Instruction: " << proc.currentLine << " of " << proc.totalLine << endl;
    cout << "Created: " << proc.timestamp << endl;

    // Print only finished instructions
    for (uint64_t i = 0; i < proc.currentLine && i < proc.instructions.size(); ++i) {
        cout << "  - " << proc.instructions[i] << endl;
    }

    cout << "\033[33m";
    cout << "Type 'exit' to quit, 'clear' to clear the screen" << endl;
    cout << "\033[0m";
}

void displayProcess(const Process& proc) {
    //printProcessDetails(proc);
    string subCommand;
    while (true) {
        cout << "Enter a command: ";
        getline(cin, subCommand);
        if (subCommand == "exit") break;
        else if (subCommand == "clear") {
            clearScreen();
            printProcessDetails(proc);
        }
        else if (subCommand == "process-smi") {
            cout << "\nprocess_name: " << proc.name << endl;
            cout << "ID: " << proc.coreAssigned << endl;
            cout << "Logs:\n(" << proc.timestamp << ") Core: " << proc.coreAssigned << endl;
            printProcessDetails(proc);
            cout << "\nCurrent instruction line " << proc.currentLine << endl;
            cout << "Lines of code: " << proc.totalLine << endl;
            if (proc.isFinished) {
                cout << "\nStatus: finished\n";
            }
            cout << endl;
        }
        else {
            cout << "Unknown command inside process view." << endl;
        }
    }
}

class ProcessManager {
private:
    unordered_map<string, unique_ptr<Process>> processes;
    int nextProcessID = 1;
public:
    void createProcess(string name) {
        if (processes.find(name) != processes.end()) {
            cout << "Process " << name << " already exists." << endl;
            return;
        }
        uint64_t cpuBurst = cpuBurstGenerator();
        vector<string> instructions;
        processes[name] = make_unique<Process>(Process{
            nextProcessID++,
            name,
            0,
            cpuBurst,
            generateTimestamp(),
            -1,
            false,
            "",
            instructions
            });
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
bool stopProcessCreation = false;

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
                    instructions_manager(proc->currentLine, proc->instructions, proc->memory, proc->name, coreId);
                    proc->currentLine++;
                    this_thread::sleep_for(chrono::milliseconds(GLOBAL_CONFIG.delayPerExec));
                }

            }
            else if (GLOBAL_CONFIG.scheduler == "rr") {
                uint64_t executedInstructions = 0;
                while (proc->currentLine < proc->totalLine &&
                    executedInstructions < GLOBAL_CONFIG.quantumCycles &&
                    !stopScheduler) {
                    instructions_manager(proc->currentLine, proc->instructions, proc->memory, proc->name, coreId);
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
            }
            else if (GLOBAL_CONFIG.scheduler == "rr") {
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
        for (int frequency = 0; frequency < GLOBAL_CONFIG.batchProcessFreq && !stopProcessCreation; ++frequency) {
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        if (stopProcessCreation) break;

        while (!stopProcessCreation) {
            string procName = "process" + (processCountName < 10 ? "0" + to_string(processCountName) : to_string(processCountName));

            if (manager.retrieveProcess(procName) == nullptr) {
                manager.createProcess(procName);
                Process* proc = manager.retrieveProcess(procName);
                if (proc) {
                    lock_guard<mutex> lock(queueMutex);
                    if (GLOBAL_CONFIG.scheduler == "fcfs") {
                        fcfsQueue.push(proc);
                    }
                    else if (GLOBAL_CONFIG.scheduler == "rr") {
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
                cout << "- min-ins:            " << GLOBAL_CONFIG.minInstructions << "\n";
                cout << "- max-ins:            " << GLOBAL_CONFIG.maxInstructions << "\n";
                cout << "- delay-per-exec:     " << GLOBAL_CONFIG.delayPerExec << " ms\n";
                cout << "--------------------------------------------\n";

                // Stop old threads if already initialized
                if (confirmInitialize) {
                    cout << "Reinitializing system...\n";
                    stopScheduler = true;
                    stopProcessCreation = true;
                    cv.notify_all();
                    for (auto& t : cpuThreads) {
                        if (t.joinable()) t.join();
                    }
                    cpuThreads.clear();  // Important: clear thread list
                    stopScheduler = false;
                    stopProcessCreation = false;
                }

                // Start new CPU threads based on updated config
                for (int i = 0; i < GLOBAL_CONFIG.numCPU; ++i) {
                    cpuThreads.emplace_back(cpuWorker, i + 1);
                }

                confirmInitialize = true;
                cout << "System config loaded and CPU threads restarted.\n";

            }
            else {
                cout << " Failed to load system configuration.\n";
            }
        }
        else if (command.rfind("screen", 0) == 0) {
            if (confirmInitialize) {
                handleScreenCommand(command, manager);
            }
            else {
                cout << "Please initialize first.\n";
            }
        }
        else if (command == "scheduler-start") {
            if (!confirmInitialize) {
                cout << "Please initialize first.\n";
                continue;
            }
            if (!schedulerRunning) {
                stopProcessCreation = false;
                schedulerRunning = true;
                scheduler_start_thread = thread(scheduler_start, ref(manager));
            }
            else {
                cout << "Scheduler is already running!\n";
            }
        }
        else if (command == "scheduler-stop") {
            if (schedulerRunning) {
                cout << "Stopping scheduler...\n";

                /*stopScheduler = true;
                schedulerRunning = false;
                cv.notify_all();
                scheduler_start_thread.join();
                stopScheduler = false;*/

                stopProcessCreation = true;
                schedulerRunning = false;
                if (scheduler_start_thread.joinable()) {
                    scheduler_start_thread.join();
                }
            }
            else {
                cout << "Scheduler is not running.\n";
            }
        }
        else if (command == "clear") {
            clearScreen();
            printHeader();
        }
        else if (command == "exit") {
            cout << "Exiting CSOPESY command line.\n";
            break;
        }
        else {
            cout << "Unknown command.\n";
        }
    }

    stopScheduler = true;
    stopProcessCreation = true;
    cv.notify_all();
    for (auto& t : cpuThreads) t.join();

    return 0;
}
