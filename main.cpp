#include <iostream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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
uint8_t clampCPUs(int value) {
    return static_cast<uint8_t>(max(1, min(value, 128)));
}

uint64_t clampUint32Range(uint64_t value) {
    return static_cast<uint64_t>(max<int64_t>(1, min<int64_t>(value, 4294967296ULL)));
}

uint64_t clampDelayPerExec(uint64_t value) {
    return min(value, 4294967296ULL);
}

uint64_t clampMemPow2(uint64_t value) {
    static const vector<uint64_t> powers = {
        64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
    };
    for (uint64_t allowed : powers) {
        if (value == allowed) return value;
    }
    throw invalid_argument("Value must be a power of two between 2^6 and 2^16");
}

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
    uint64_t maxOverallMem = 0;
    uint64_t memPerFrame = 0;
    uint64_t minMemPerProc = 0;
    uint64_t maxMemPerProc = 0;
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
            GLOBAL_CONFIG.numCPU = clampCPUs(value);
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
            int64_t value;
            file >> value;
            GLOBAL_CONFIG.quantumCycles = clampUint32Range(value);
        }
        else if (key == "batch-process-freq") {
            int64_t value;
            file >> value;
            GLOBAL_CONFIG.batchProcessFreq = clampUint32Range(value);
        }
        else if (key == "min-ins") {
            int64_t value;
            file >> value;
            GLOBAL_CONFIG.minInstructions = clampUint32Range(value);
        }
        else if (key == "max-ins") {
            int64_t value;
            file >> value;
            GLOBAL_CONFIG.maxInstructions = clampUint32Range(value);
        }
        else if (key == "delay-per-exec") {
            uint64_t value;
            file >> value;
            GLOBAL_CONFIG.delayPerExec = clampDelayPerExec(value);
        }
        else if (key == "max-overall-mem") {
            uint64_t value;
            file >> value;
            GLOBAL_CONFIG.maxOverallMem = clampMemPow2(value);
        }
        else if (key == "mem-per-frame") {
            uint64_t value;
            file >> value;
            GLOBAL_CONFIG.memPerFrame = clampMemPow2(value);
        }
        else if (key == "min-mem-per-proc") {
            uint64_t value;
            file >> value;
            GLOBAL_CONFIG.minMemPerProc = clampMemPow2(value);
        }
        else if (key == "max-mem-per-proc") {
            uint64_t value;
            file >> value;
            GLOBAL_CONFIG.maxMemPerProc = clampMemPow2(value);
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

    if (GLOBAL_CONFIG.minMemPerProc > GLOBAL_CONFIG.maxMemPerProc) {
        cerr << "min-mem-per-proc cannot be greater than max-mem-per-proc." << endl;
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

struct PageTableEntry {
    uint64_t pageNumber;
    int64_t frameNumber; // -1 if not in memory
    bool valid;
    bool dirty; // optional: for WRITE detection
    uint64_t lastUsed; // for LRU or clock algorithms
};

unordered_map<string, unordered_map<uint64_t, unordered_map<string, uint16_t>>> backingStore;
// Format: backingStore[processName][pageNumber][symbol/hexAddr] = value;


struct Frame {
    string processName;
    uint64_t pageNumber;
    unordered_map<string, uint16_t> data;
};

vector<Frame> physicalMemory;
unordered_map<int, bool> frameUsed;



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
    uint64_t totalmemory;
    vector<PageTableEntry> pageTable;
};

class ProcessManager {
private:
    unordered_map<string, unique_ptr<Process>> processes;
    int nextProcessID = 1;

    uint64_t generateProcessMemory() {
        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<uint64_t> distrib(GLOBAL_CONFIG.minMemPerProc, GLOBAL_CONFIG.maxMemPerProc);
        return distrib(gen);
    }

public:
    void createProcess(string name) {
        if (processes.find(name) != processes.end()) {
            cout << "Process " << name << " already exists." << endl;
            return;
        }
        uint64_t cpuBurst = cpuBurstGenerator();
        vector<string> instructions;
        uint64_t memRequired = generateProcessMemory();
        uint64_t pagesNeeded = memRequired / GLOBAL_CONFIG.memPerFrame;
        vector<PageTableEntry> pageTable;

        for (uint64_t i = 0; i < pagesNeeded; ++i) {
            pageTable.push_back(PageTableEntry{ i, -1, false, false, 0 }); // not yet loaded into memory
        }

        processes[name] = make_unique<Process>(Process{
            nextProcessID++,
            name,
            0,
            cpuBurst,
            generateTimestamp(),
            -1,
            false,
            "",
            instructions,
            {},
            memRequired,
            pageTable
            });
    }

    Process* retrieveProcess(const string& name) {
        auto it = processes.find(name);
        return it != processes.end() ? it->second.get() : nullptr;
    }

    void listProcesses() {
        cout << "-----------------------------\n";

        // Track cores being used
        unordered_set<int> coresUsedSet;
        for (const auto& [name, proc] : processes) {
            if (!proc->isFinished && proc->coreAssigned != -1) {
                coresUsedSet.insert(proc->coreAssigned);
            }
        }

        int coresAvailable = GLOBAL_CONFIG.numCPU;
        int coresUsed = static_cast<int>(coresUsedSet.size());
        double utilization = (coresAvailable > 0) ? (static_cast<double>(coresUsed) / coresAvailable) * 100.0 : 0.0;
        coresAvailable = coresAvailable - coresUsed;

        // Display core usage stats
        cout << fixed << setprecision(2);
        cout << "CPU Utilization: " << utilization << "%\n";
        cout << "Cores Used:      " << coresUsed << "\n";
        cout << "Cores Available: " << coresAvailable << "\n";
        cout << "-----------------------------\n";

        // Running processes
        cout << "Running processes:\n";
        for (const auto& [name, proc] : processes) {
            if (!proc->isFinished && proc->coreAssigned != -1) {
                cout << name << "\033[33m  (" << proc->timestamp << ") \033[0m"
                    << "Core: " << proc->coreAssigned << " \033[33m"
                    << proc->currentLine << " / " << proc->totalLine << "\033[0m" << endl;
            }
        }

        // Finished processes
        cout << "\nFinished processes:\n";
        for (const auto& [name, proc] : processes) {
            if (proc->isFinished) {
                cout << name << " (" << proc->finishedTime << ") Finished "
                    << proc->totalLine << " / " << proc->totalLine << endl;
            }
        }

        cout << "-----------------------------\n";
    }

    void logProcesses(const string& filename) {
        ofstream logFile(filename);
        if (!logFile.is_open()) {
            cerr << "Failed to create log file: " << filename << endl;
            return;
        }

        logFile << "-----------------------------\n";

        unordered_set<int> coresUsedSet;
        for (const auto& [name, proc] : processes) {
            if (!proc->isFinished && proc->coreAssigned != -1) {
                coresUsedSet.insert(proc->coreAssigned);
            }
        }

        int coresAvailable = GLOBAL_CONFIG.numCPU;
        int coresUsed = static_cast<int>(coresUsedSet.size());
        double utilization = (coresAvailable > 0) ? (static_cast<double>(coresUsed) / coresAvailable) * 100.0 : 0.0;
        coresAvailable = coresAvailable - coresUsed;

        logFile << fixed << setprecision(2);
        logFile << "CPU Utilization: " << utilization << "%\n";
        logFile << "Cores Used:      " << coresUsed << "\n";
        logFile << "Cores Available: " << coresAvailable << "\n";
        logFile << "-----------------------------\n";

        logFile << "Running processes:\n";
        for (const auto& [name, proc] : processes) {
            if (!proc->isFinished && proc->coreAssigned != -1) {
                logFile << name << " (" << proc->timestamp << ") "
                    << "Core: " << proc->coreAssigned << " "
                    << proc->currentLine << " / " << proc->totalLine << endl;
            }
        }

        logFile << "\nFinished processes:\n";
        for (const auto& [name, proc] : processes) {
            if (proc->isFinished) {
                logFile << name << " (" << proc->finishedTime << ") Finished "
                    << proc->totalLine << " / " << proc->totalLine << endl;
            }
        }

        logFile << "-----------------------------\n";
        logFile.close();
        cout << "Report saved to csopesy-log.txt\n";
    }

};

unordered_map<string, uint16_t>& getPageData(Process& proc, uint64_t pageNum, ProcessManager& manager) {
    // Check if page is valid in memory
    if (proc.pageTable[pageNum].valid) {
        int frameIdx = static_cast<int>(proc.pageTable[pageNum].frameNumber);

        if (frameIdx < 0 || frameIdx >= static_cast<int>(physicalMemory.size())) {
            cerr << "Error: Invalid frameIdx = " << frameIdx << ", exceeds physical memory.\n";
            exit(1);
        }

        physicalMemory[frameIdx].data[proc.name]; // update last used
        proc.pageTable[pageNum].lastUsed = chrono::steady_clock::now().time_since_epoch().count();
        return physicalMemory[frameIdx].data;
    }


    // If not valid: handle page fault
    int victimIdx = -1;
    for (int i = 0; i < physicalMemory.size(); ++i) {
        if (physicalMemory[i].data.empty()) {
            victimIdx = i; break;
        }
    }

    if (victimIdx == -1) {
        // No free frame — use LRU replacement
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < physicalMemory.size(); ++i) {
            const Frame& f = physicalMemory[i];
            const auto& table = proc.pageTable;
            for (const auto& p : table) {
                if (p.valid && p.frameNumber == i && p.lastUsed < oldest) {
                    oldest = p.lastUsed;
                    victimIdx = i;
                }
            }
        }

        // Save victim to backing store
        Frame& victim = physicalMemory[victimIdx];
        backingStore[victim.processName][victim.pageNumber] = victim.data;

        // Invalidate page table of victim process
        Process* victimProc = manager.retrieveProcess(victim.processName);
        if (victimProc) {
            victimProc->pageTable[victim.pageNumber].valid = false;
            victimProc->pageTable[victim.pageNumber].frameNumber = -1;
        }
    }

    // Load page from backing store or empty
    Frame& frame = physicalMemory[victimIdx];
    frame.processName = proc.name;
    frame.pageNumber = static_cast<uint64_t>(pageNum);

    if (backingStore[proc.name].count(pageNum)) {
        frame.data = backingStore[proc.name][pageNum];
    }
    else {
        frame.data.clear();
    }

    // Update page table
    proc.pageTable[pageNum].valid = true;
    proc.pageTable[pageNum].frameNumber = victimIdx;
    proc.pageTable[pageNum].lastUsed = chrono::steady_clock::now().time_since_epoch().count();

    return frame.data;
}


void instructions_manager(Process* proc, int coreId, ProcessManager& manager) {
    if (proc->instructions.size() <= proc->currentLine)
        proc->instructions.resize(proc->currentLine + 1);

    string prefix = "(" + generateTimestamp() + ") Core: " + to_string(coreId) + " ";

    static thread_local mt19937 gen(random_device{}());
    uniform_int_distribution<> cmdDistrib(0, 7); // Extended to 8 types of commands
    uniform_int_distribution<> valDistrib(1, 100);

    uint64_t maxAddr = 64 + proc->totalmemory - 1;
    uniform_int_distribution<uint64_t> addrDistrib(64, maxAddr);

    stringstream log;
    int cmd = cmdDistrib(gen);
    static thread_local vector<string> varNames;

    if (cmd == 1 || varNames.empty()) {
        string var = "v" + to_string(varNames.size());
        uint16_t val = valDistrib(gen);
        proc->memory[var] = val;
        varNames.push_back(var);
        log << "DECLARE " << var << " = " << val;
    }
    else if (cmd == 0 && !varNames.empty()) {
        string var = varNames[gen() % varNames.size()];
        uint16_t val = proc->memory.count(var) ? proc->memory[var] : 0;
        log << "PRINT " << var << " = " << val;
    }
    else if (cmd == 2 && varNames.size() >= 2) {
        string a = varNames[gen() % varNames.size()];
        string b = varNames[gen() % varNames.size()];
        uint16_t valA = proc->memory.count(a) ? proc->memory[a] : 0;
        uint16_t valB = proc->memory.count(b) ? proc->memory[b] : 0;
        uint16_t result = clampUint16(valA + valB);
        string resultVar = "res" + to_string(proc->currentLine);
        proc->memory[resultVar] = result;
        log << "ADD " << a << "(" << valA << ") + " << b << "(" << valB << ") = " << result;
    }
    else if (cmd == 3 && varNames.size() >= 2) {
        string a = varNames[gen() % varNames.size()];
        string b = varNames[gen() % varNames.size()];
        uint16_t valA = proc->memory.count(a) ? proc->memory[a] : 0;
        uint16_t valB = proc->memory.count(b) ? proc->memory[b] : 0;
        uint16_t result = clampUint16(valA - valB);
        string resultVar = "res" + to_string(proc->currentLine);
        proc->memory[resultVar] = result;
        log << "SUBTRACT " << a << "(" << valA << ") - " << b << "(" << valB << ") = " << result;
    }
    else if (cmd == 4) {
        int ms = 100;
        this_thread::sleep_for(chrono::milliseconds(ms));
        log << "SLEPT for " << ms << "ms";
    }
    else if (cmd == 5 && !varNames.empty()) {
        string var = varNames[gen() % varNames.size()];
        int count = 3;
        if (!proc->memory.count(var)) proc->memory[var] = 0;
        log << "FOR loop on " << var << ": ";
        for (int i = 0; i < count; ++i) {
            proc->memory[var]++;
            log << "[" << i + 1 << "]=" << proc->memory[var] << " ";
        }
    }
    else if (cmd == 6 && !varNames.empty()) {
        string var = varNames[gen() % varNames.size()];
        uint64_t address = addrDistrib(gen);
        uint64_t pageNum = address / GLOBAL_CONFIG.memPerFrame;
        stringstream ss;
        ss << "0x" << hex << uppercase << address;
        string hexAddr = ss.str();

        unordered_map<string, uint16_t>& pageData = getPageData(*proc, pageNum, manager);
        uint16_t val = pageData.count(hexAddr) ? pageData[hexAddr] : 0;
        proc->memory[var] = val;

        log << "READ " << var << " <- mem[" << hexAddr << "] = " << val;
    }
    else if (cmd == 7) {
        uint64_t address = addrDistrib(gen);
        uint64_t pageNum = address / GLOBAL_CONFIG.memPerFrame;
        uint16_t val = valDistrib(gen);
        stringstream ss;
        ss << "0x" << hex << uppercase << address;
        string hexAddr = ss.str();

        unordered_map<string, uint16_t>& pageData = getPageData(*proc, pageNum, manager);
        pageData[hexAddr] = val;
        proc->pageTable[pageNum].dirty = true;

        log << "WRITE mem[" << hexAddr << "] = " << val;
    }

    proc->instructions[proc->currentLine] = prefix + "\"" + log.str() + "\"";
}


void printProcessDetails(const Process& proc) {
    cout << "Process: " << proc.name << endl;
    cout << "ID: " << proc.id << endl;
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
        else if (subCommand == "process-smi") {
            cout << "\nprocess_name: " << proc.name << endl;
            cout << "ID: " << proc.id << endl;
            cout << "Logs:\n(" << proc.timestamp << ") Core: " << proc.coreAssigned << endl;
            cout << "\nCurrent instruction line " << proc.currentLine << endl;
            cout << "Lines of code: " << proc.totalLine << endl;
            // Print only finished instructions
            if (!proc.isFinished) {
                for (uint64_t i = 0; i < proc.currentLine && i < proc.instructions.size(); ++i) {
                    cout << "  - " << proc.instructions[i] << endl;
                }
            }
            else {
                cout << "\nStatus: finished\n";
            }
            cout << endl;
        }
        else {
            cout << "Unknown command inside process view." << endl;
        }
    }
}



queue<Process*> fcfsQueue;
queue<Process*> rrQueue;
mutex queueMutex;
condition_variable cv;
bool stopScheduler = false;
bool stopProcessCreation = false;

void cpuWorker(int coreId, ProcessManager& manager) {
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
                    instructions_manager(proc, coreId, manager);
                    proc->currentLine++;
                    this_thread::sleep_for(chrono::milliseconds(GLOBAL_CONFIG.delayPerExec));
                }

            }
            else if (GLOBAL_CONFIG.scheduler == "rr") {
                uint64_t executedInstructions = 0;
                while (proc->currentLine < proc->totalLine &&
                    executedInstructions < GLOBAL_CONFIG.quantumCycles &&
                    !stopScheduler) {
                    instructions_manager(proc, coreId, manager);
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
                cout << "- delay-per-exec:     " << GLOBAL_CONFIG.delayPerExec << "\n";
                cout << "- max-overall-mem:    " << GLOBAL_CONFIG.maxOverallMem << "\n";
                cout << "- mem-per-frame:      " << GLOBAL_CONFIG.memPerFrame << "\n";
                cout << "- min-mem-per-proc:   " << GLOBAL_CONFIG.minMemPerProc << "\n";
                cout << "- max-mem-per-proc:   " << GLOBAL_CONFIG.maxMemPerProc << "\n";
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
                    cpuThreads.emplace_back(cpuWorker, i + 1, ref(manager));
                }

                confirmInitialize = true;
                cout << "System config loaded and CPU threads restarted.\n";

                physicalMemory.resize(GLOBAL_CONFIG.maxOverallMem / GLOBAL_CONFIG.memPerFrame);

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
        else if (command == "report-util") {
            //Create csopesy-log.txt
            //Save in the text file the same printed outputs listProcess function
            if (!confirmInitialize) {
                cout << "Please initialize first.\n";
            }
            else {
                manager.logProcesses("csopesy-log.txt");
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
                cout << "Scheduler is running!\n";
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

            if (schedulerRunning) {
                cout << "Stopping scheduler...\n";

                stopProcessCreation = true;
                schedulerRunning = false;
                if (scheduler_start_thread.joinable()) {
                    scheduler_start_thread.join();
                }
            }

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
