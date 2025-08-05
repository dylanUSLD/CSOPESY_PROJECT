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
#include <algorithm>
#include <regex>

using namespace std;

std::mutex memMutex;

static constexpr char BACKING_FILENAME[] = "csopesy-backing-store.txt";

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

atomic<uint64_t> totalCpuTicks = 0;
atomic<uint64_t> activeCpuTicks = 0;
atomic<uint64_t> idleCpuTicks = 0;

atomic<uint64_t> pageInCount = 0;
atomic<uint64_t> pageOutCount = 0;


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

    return true;
}

struct Frame {
    int processId = -1;       // -1 means unused
    int pageNumber = -1;      // -1 means unassigned
    string data = "";         // contents in the frame
};

vector<Frame> physicalMemory;

struct pair_hash {
    template<typename T1, typename T2>
    size_t operator()(const pair<T1, T2>& p) const {
        auto h1 = hash<T1>{}(p.first);
        auto h2 = hash<T2>{}(p.second);
        return h1 ^ (h2 << 1); // Combine the hashes
    }
};

unordered_map<pair<int, int>, string, pair_hash> backingStore;

void syncBackingStoreToFile() {
    std::ofstream out(BACKING_FILENAME, std::ios::trunc);
    for (auto& [key, val] : backingStore) {
        out << key.first << ' ' << key.second << ' '
            << std::quoted(val) << "\n";
    }
}

struct PageTableEntry {
    bool inMemory = false;
    int frameIndex = -1;  // -1 means not loaded
};

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
    uint64_t memorySize; // memory size in bytes (must be power of 2)
    vector<PageTableEntry> pageTable;
    vector<string> customInstrList;
    bool   isShutdown = false;
    string shutdownReason;
    string shutdownTime;
};

queue<pair<int, int>> pageLoadOrder;  // FIFO queue: (processId, pageNumber)
unordered_map<int, Process*> processLookup;  // pid -> Process*, for eviction tracking

bool loadPageIfNotInMemory(Process* proc, int pageNumber) {
    std::lock_guard<std::mutex> lock(memMutex);

    if (!proc || pageNumber < 0 || pageNumber >= static_cast<int>(proc->pageTable.size())) {
        return false; // Invalid process or page number
    }

    PageTableEntry& entry = proc->pageTable[pageNumber];
    if (entry.inMemory) return true; // Already in memory

    // === Try to find a free frame ===
    for (size_t i = 0; i < physicalMemory.size(); ++i) {
        if (physicalMemory[i].processId == -1) {
            entry.inMemory = true;
            entry.frameIndex = static_cast<int>(i);

            physicalMemory[i].processId = proc->id;
            physicalMemory[i].pageNumber = pageNumber;

            pageInCount++;

            // Restore from backing store if available
            auto it = backingStore.find({ proc->id, pageNumber });
            if (it != backingStore.end()) {
                physicalMemory[i].data = it->second;
                backingStore.erase(it);
            }
            else {
                physicalMemory[i].data = "";
            }

            // Avoid duplicate entries in pageLoadOrder
            bool alreadyQueued = false;
            queue<pair<int, int>> tempQueue;
            while (!pageLoadOrder.empty()) {
                auto front = pageLoadOrder.front(); pageLoadOrder.pop();
                if (front == make_pair(proc->id, pageNumber)) {
                    alreadyQueued = true;
                }
                tempQueue.push(front);
            }
            swap(pageLoadOrder, tempQueue);

            if (!alreadyQueued) {
                pageLoadOrder.emplace(proc->id, pageNumber);
            }

            return true;
        }
    }

    // === No free frame: apply FIFO eviction ===
    if (!pageLoadOrder.empty()) {
        auto [evictedPID, evictedPageNum] = pageLoadOrder.front();
        pageLoadOrder.pop();

        Process* evictedProc = processLookup.count(evictedPID) ? processLookup[evictedPID] : nullptr;
        if (!evictedProc) return false;

        PageTableEntry& evictedEntry = evictedProc->pageTable[evictedPageNum];
        int victimFrameIdx = evictedEntry.frameIndex;

        // Save evicted content to backing store
        backingStore[{evictedPID, evictedPageNum}] = physicalMemory[victimFrameIdx].data;

        pageOutCount++;
        syncBackingStoreToFile();

        // Invalidate evicted page
        evictedEntry.inMemory = false;
        evictedEntry.frameIndex = -1;

        // Load new page into the evicted frame
        entry.inMemory = true;
        entry.frameIndex = victimFrameIdx;

        pageInCount++;

        physicalMemory[victimFrameIdx].processId = proc->id;
        physicalMemory[victimFrameIdx].pageNumber = pageNumber;

        // Restore data from backing store
        auto it = backingStore.find({ proc->id, pageNumber });
        if (it != backingStore.end()) {
            physicalMemory[victimFrameIdx].data = it->second;
            backingStore.erase(it);
            syncBackingStoreToFile();
        }
        else {
            physicalMemory[victimFrameIdx].data = "";
        }

        // Avoid duplicate entries in pageLoadOrder
        bool alreadyQueued = false;
        queue<pair<int, int>> tempQueue;
        while (!pageLoadOrder.empty()) {
            auto front = pageLoadOrder.front(); pageLoadOrder.pop();
            if (front == make_pair(proc->id, pageNumber)) {
                alreadyQueued = true;
            }
            tempQueue.push(front);
        }
        swap(pageLoadOrder, tempQueue);

        if (!alreadyQueued) {
            pageLoadOrder.emplace(proc->id, pageNumber);
        }

        return true;
    }

    return false; // No free frame and nothing to evict
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

uint64_t generateRandomMemSize() {
    static const vector<uint64_t> pow2 = {
        64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
    };

    vector<uint64_t> filtered;
    for (uint64_t val : pow2) {
        if (val >= GLOBAL_CONFIG.minMemPerProc && val <= GLOBAL_CONFIG.maxMemPerProc) {
            filtered.push_back(val);
        }
    }

    if (filtered.empty()) {
        throw runtime_error("No valid power-of-2 memory size within range.");
    }

    static thread_local mt19937 gen(random_device{}());
    uniform_int_distribution<> dist(0, static_cast<int>(filtered.size()) - 1);

    return filtered[dist(gen)];
}

uint64_t generateRandomDataAddress(uint64_t minAddr, uint64_t maxAddr) {
    static thread_local mt19937 gen(random_device{}());
    uniform_int_distribution<uint64_t> dist(minAddr, maxAddr);
    return dist(gen);
}


void instructions_manager(
    uint64_t currentLine,
    vector<string>& instructions,
    unordered_map<string, uint16_t>& memory,
    const string& processName,
    int coreId,
    Process* proc
) {
    // 0) If already shutdown, do nothing
    if (proc->isShutdown) return;

    // 1) Ensure instructions has enough space
    if (instructions.size() <= currentLine)
        instructions.resize(currentLine + 1);

    // 2) Common prefix
    string prefix = "(" + generateTimestamp() + ") Core: " + to_string(coreId) + " ";

    // 3) If we still have custom instructions queued, run those first:
    if (currentLine < proc->customInstrList.size()) {
        const string& instr = proc->customInstrList[currentLine];
        smatch m;
        stringstream log;

        // --- DECLARE <var> <value> ---
        if (regex_match(instr, m, regex(R"(DECLARE\s+([A-Za-z_]\w*)\s+(\d+))"))) {
            string var = m[1], val = m[2];
            memory[var] = static_cast<uint16_t>(stoi(val));
            if (loadPageIfNotInMemory(proc, 0)) {
                int f = proc->pageTable[0].frameIndex;
                lock_guard<mutex> L(memMutex);
                physicalMemory[f].data += "(" + var + " " + val + ")";
            }
            log << "DECLARE " << var << " = " << val;
        }

        // --- ADD <dst> <a> <b> ---
        else if (regex_match(instr, m, regex(R"(ADD\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*))"))) {
            string dst = m[1], a = m[2], b = m[3];
            uint16_t res = clampUint16(memory[a] + memory[b]);
            memory[dst] = res;
            log << "ADD " << a << "(" << memory[a] << ") + "
                << b << "(" << memory[b] << ") = " << res;
        }

        // --- SUBTRACT <dst> <a> <b> ---
        else if (regex_match(instr, m, regex(R"(SUBTRACT\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*))"))) {
            string dst = m[1], a = m[2], b = m[3];
            uint16_t res = clampUint16(memory[a] - memory[b]);
            memory[dst] = res;
            log << "SUBTRACT " << a << "(" << memory[a] << ") - "
                << b << "(" << memory[b] << ") = " << res;
        }

        // --- WRITE 0xHEXADDR <value|var> ---
        else if (regex_match(instr, m, regex(R"(WRITE\s+(0x[0-9A-Fa-f]+)\s+(\d+|[A-Za-z_]\w*))"))) {
            string addrHex = m[1];
            string tok = m[2];
            uint64_t address = stoull(addrHex, nullptr, 16);

            // --- bounds check ---
            if (address < 64 || address >= proc->memorySize) {
                proc->isShutdown = true;
                proc->shutdownReason = "Memory access violation at " + addrHex;
                proc->shutdownTime = generateTimestamp();
                instructions[currentLine]
                    = prefix + "\"" + proc->shutdownReason + "\"";
                return;
            }

            uint16_t val = isdigit(tok[0])
                ? static_cast<uint16_t>(stoi(tok))
                : memory[tok];

            size_t pageNum = min<uint64_t>(
                address / GLOBAL_CONFIG.memPerFrame,
                proc->pageTable.size() - 1
            );
            bool loaded = loadPageIfNotInMemory(proc, pageNum);
            if (loaded) {
                int f = proc->pageTable[pageNum].frameIndex;
                lock_guard<mutex> L(memMutex);
                physicalMemory[f].data += "(" + addrHex + " " + to_string(val) + ")";
            }
            memory[addrHex] = val;
            log << "WRITE " << addrHex << " " << val;
        }

        // --- READ <var> 0xHEXADDR ---
        else if (regex_match(instr, m, regex(R"(READ\s+([A-Za-z_]\w*)\s+(0x[0-9A-Fa-f]+))"))) {
            string var = m[1];
            string addrHex = m[2];
            uint64_t address = stoull(addrHex, nullptr, 16);

            // --- bounds check ---
            if (address < 64 || address >= proc->memorySize) {
                proc->isShutdown = true;
                proc->shutdownReason = "Memory access violation at " + addrHex;
                proc->shutdownTime = generateTimestamp();
                instructions[currentLine]
                    = prefix + "\"" + proc->shutdownReason + "\"";
                return;
            }

            size_t pageNum = min<uint64_t>(
                address / GLOBAL_CONFIG.memPerFrame,
                proc->pageTable.size() - 1
            );
            bool loaded = loadPageIfNotInMemory(proc, pageNum);
            uint16_t val = memory.count(addrHex) ? memory[addrHex] : 0;
            memory[var] = val;
            log << "READ " << var << " = " << val
                << " from " << addrHex
                << (loaded ? " (loaded)" : " (not loaded)");
        }

        // --- PRINT("Result: " + var) ---
        else if (regex_match(instr, m, regex(R"(PRINT\(\s*"Result: "\s*\+\s*([A-Za-z_]\w*)\s*\))"))) {
            string var = m[1];
            log << "PRINT(\"Result: \" + " << var << ") = " << memory[var];
        }

        // should never hit this if validation was correct
        else {
            log << "UNKNOWN_INSTR";
        }

        // commit log and return
        instructions[currentLine] = prefix + "\"" + log.str() + "\"";
        return;
    }

    // Random instruction generation
    static thread_local mt19937 gen(random_device{}());
    uniform_int_distribution<> cmdDistrib(0, 6);
    uniform_int_distribution<> valDistrib(1, 100);

    stringstream ss;
    stringstream log;
    int cmd = cmdDistrib(gen);

    // Track declared vars
    static thread_local vector<string> varNames;

    // inside instructions_manager, replace the DECLARE case with:
    static constexpr size_t MAX_DECLARED_VARS = 32;


    if (cmd == 1 || varNames.empty()) {
        // DECLARE
        string var = "v" + to_string(varNames.size());
        uint16_t val = valDistrib(gen);

        if (varNames.size() < MAX_DECLARED_VARS) {
            // — original behavior —
            /*if (loadPageIfNotInMemory(proc, 0)) {
                int frameIdx = proc->pageTable[0].frameIndex;
                std::lock_guard<std::mutex> lock(memMutex);
                physicalMemory[frameIdx].data += "(" + var + " " + to_string(val) + ")";
            }*/
            if (loadPageIfNotInMemory(proc, 0)) {
                int frameIdx = proc->pageTable[0].frameIndex;
                if (frameIdx >= 0 && frameIdx < (int)physicalMemory.size()) {
                    std::lock_guard<std::mutex> lock(memMutex);
                    physicalMemory[frameIdx].data += "(" + var + " " + to_string(val) + ")";
                }
            }
            else {
                log << "WARNING: Page 0 not loaded; DECLARE attempted without memory.";
            }

            memory[var] = val;
            varNames.push_back(var);
            log << "DECLARE " << var << " = " << val;
        }
        else {
            // — we've hit the 32‐var limit: ignore further DECLAREs —
            log << "DECLARE ignored";
            // (do *not* touch proc->pageTable[0].data, memory or varNames)
        }
    }
    else if (cmd == 0 && !varNames.empty()) {
        // PRINT
        if (!loadPageIfNotInMemory(proc, 0)) {
            log << "WARNING: Page 0 not loaded; DECLARE attempted without memory.";
        }

        string var = varNames[gen() % varNames.size()];
        uint16_t val = memory.count(var) ? memory[var] : 0;
        log << "PRINT " << var << " = " << val;
    }
    else if (cmd == 2 && varNames.size() >= 2) {
        // ADD
        if (!loadPageIfNotInMemory(proc, 0)) {
            log << "WARNING: Page 0 not loaded; DECLARE attempted without memory.";
        }

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
        if (!loadPageIfNotInMemory(proc, 0)) {
            log << "WARNING: Page 0 not loaded; DECLARE attempted without memory.";
        }

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
    else if (cmd == 5) {
        // READ
        if (!varNames.empty()) {
            // 1) Pick a target variable
            string var = varNames[gen() % varNames.size()];

            // 2) Pick a random address (same as before)
            uint64_t minAddr = GLOBAL_CONFIG.memPerFrame;  // skip page 0
            uint64_t maxAddr = proc->memorySize - 1;
            if (maxAddr < minAddr) maxAddr = minAddr;
            /*
            uint64_t address = generateRandomDataAddress(minAddr, maxAddr);
            int pageNumber = static_cast<int>(address / GLOBAL_CONFIG.memPerFrame);*/
            uint64_t address = generateRandomDataAddress(minAddr, maxAddr);
            size_t   rawPage = address / GLOBAL_CONFIG.memPerFrame;
            size_t   lastPage = proc->pageTable.size() - 1;
            size_t   pageNumber = std::min(rawPage, lastPage);


            // 3) Ensure the page is loaded
            bool pageLoaded = loadPageIfNotInMemory(proc, pageNumber);

            // 4) Build the hex‐string key
            stringstream addrHex;
            addrHex << "0x" << hex << address;
            string addrKey = addrHex.str();

            // 5) Look up the value (or default to 0) and store it in your map
            uint16_t readValue = memory.count(addrKey) ? memory[addrKey] : 0;
            memory[var] = readValue;

            // 6) Log with the loaded‐page info
            if (pageLoaded) {
                log << "READ " << var << " = " << readValue
                    << " from " << addrKey
                    << " (Page " << dec << pageNumber << " loaded)";
            }
            else {
                log << "READ " << var << " = " << readValue
                    << " from " << addrKey
                    << " (Page " << dec << pageNumber << " not loaded - memory full)";
            }
        }
    }

    else if (cmd == 6) {
        // WRITE
        uint64_t minAddr = GLOBAL_CONFIG.memPerFrame;         // skip page 0
        uint64_t maxAddr = proc->memorySize - 1;
        if (maxAddr < minAddr) maxAddr = minAddr;
        /*
        uint64_t address = generateRandomDataAddress(minAddr, maxAddr);
        int      pageNumber = address / GLOBAL_CONFIG.memPerFrame;*/
        uint64_t address = generateRandomDataAddress(minAddr, maxAddr);
        size_t   rawPage = address / GLOBAL_CONFIG.memPerFrame;
        size_t   lastPage = proc->pageTable.size() - 1;
        size_t   pageNumber = std::min(rawPage, lastPage);
        uint16_t value = valDistrib(gen);

        // Load the page if needed
        bool pageLoaded = loadPageIfNotInMemory(proc, pageNumber);

        stringstream addrHex;
        addrHex << "0x" << hex << address;

        if (pageLoaded) {
            // **Store into your "variable" map**
            memory[addrHex.str()] = value;


            int frameIdx = proc->pageTable[pageNumber].frameIndex;
            if (frameIdx >= 0 && frameIdx < (int)physicalMemory.size()) {
                std::lock_guard<std::mutex> lock(memMutex);
                physicalMemory[frameIdx].data += "(" + addrHex.str() + " " + to_string(value) + ")";
            }

            log << "WRITE " << addrHex.str() << " " << dec << value
                << " (Page " << pageNumber << " loaded)";
        }
        else {
            log << "WRITE " << addrHex.str() << " " << dec << value
                << " (Page " << pageNumber << " not loaded - memory full)";
        }
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

void printProcessDetails(const Process& proc) {
    // If the process was shutdown, show the violation message and return
    if (proc.isShutdown) {
        cout << "Process " << proc.name
            << " shutdown due to memory access violation error that occurred at "
            << proc.shutdownTime << ". "
            << proc.shutdownReason << " invalid."
            << endl;
        return;
    }

    // Otherwise, show the normal details
    cout << "Process: " << proc.name << endl;
    cout << "ID: " << proc.id << endl;
    cout << "Memory Size: " << proc.memorySize << " bytes" << endl;
    cout << "Instruction: " << proc.currentLine << " of " << proc.totalLine << endl;
    cout << "Created: " << proc.timestamp << endl;

    cout << "Page Table (" << proc.pageTable.size() << " pages):\n";
    for (size_t i = 0; i < proc.pageTable.size(); ++i) {
        cout << "  Page " << i
            << ": inMemory=" << boolalpha << proc.pageTable[i].inMemory
            << ", frameIndex=" << proc.pageTable[i].frameIndex
            << endl;
    }

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

class ProcessManager {
private:
    unordered_map<string, unique_ptr<Process>> processes;
    int nextProcessID = 1;
public:
    /// Expose all processes so stats can iterate them
    const unordered_map<string, unique_ptr<Process>>& getProcesses() const {
        return processes;
    }
public:
    void createProcess(string name) {
        if (processes.find(name) != processes.end()) {
            cout << "Process " << name << " already exists." << endl;
            return;
        }
        uint64_t cpuBurst = cpuBurstGenerator();
        vector<string> instructions;
        uint64_t memSize = generateRandomMemSize();
        uint64_t pageCount = memSize / GLOBAL_CONFIG.memPerFrame;
        vector<PageTableEntry> table(pageCount);
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
            memSize,
            table
            });
        processLookup[processes[name]->id] = processes[name].get();
    }

    Process* retrieveProcess(const string& name) {
        auto it = processes.find(name);
        return it != processes.end() ? it->second.get() : nullptr;
    }

    void listProcesses() {
        cout << "-----------------------------\n";

        // --- CPU Utilization Stats ---
        unordered_set<int> coresUsedSet;
        for (auto& [name, proc] : processes) {
            if (!proc->isFinished && !proc->isShutdown && proc->coreAssigned != -1) {
                coresUsedSet.insert(proc->coreAssigned);
            }
        }
        int coresAvailable = GLOBAL_CONFIG.numCPU;
        int coresUsed = static_cast<int>(coresUsedSet.size());
        double utilization = coresAvailable > 0
            ? (static_cast<double>(coresUsed) / coresAvailable) * 100.0
            : 0.0;
        coresAvailable -= coresUsed;

        cout << fixed << setprecision(2)
            << "CPU Utilization: " << utilization << "%\n"
            << "Cores Used:      " << coresUsed << "\n"
            << "Cores Available: " << coresAvailable << "\n"
            << "-----------------------------\n";

        // --- Running Processes ---
        cout << "Running processes:\n";
        for (auto& [name, proc] : processes) {
            if (!proc->isFinished && !proc->isShutdown && proc->coreAssigned != -1) {
                cout << name
                    << "\033[33m  (" << proc->timestamp << ") \033[0m"
                    << "Core: " << proc->coreAssigned
                    << " \033[33m" << proc->currentLine << " / " << proc->totalLine << "\033[0m"
                    << endl;
            }
        }

        // --- Finished Processes ---
        cout << "\nFinished processes:\n";
        for (auto& [name, proc] : processes) {
            if (proc->isFinished && !proc->isShutdown) {
                cout << name
                    << " (" << proc->finishedTime << ") Finished "
                    << proc->totalLine << " / " << proc->totalLine
                    << endl;
            }
        }

        // --- Shutdown Processes ---
        cout << "\nShutdown processes:\n";
        for (auto& [name, proc] : processes) {
            if (proc->isShutdown) {
                cout << name
                    << " (" << proc->shutdownTime << ") "
                    << proc->shutdownReason
                    << endl;
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

void displaySystemStats(const ProcessManager& manager) {
    std::lock_guard<std::mutex> lock(memMutex);

    // --- CPU Utilization ---
    unordered_set<int> coresInUse;
    for (auto& [name, procPtr] : manager.getProcesses()) {
        if (!procPtr->isFinished && procPtr->coreAssigned != -1)
            coresInUse.insert(procPtr->coreAssigned);
    }
    int usedCores = (int)coresInUse.size();
    int totalCores = GLOBAL_CONFIG.numCPU;
    double cpuUtil = totalCores ? (100.0 * usedCores / totalCores) : 0.0;

    cout << fixed << setprecision(2)
        << "CPU Utilization: " << cpuUtil << "% ("
        << usedCores << " / " << totalCores << " cores)\n";

    // --- Physical Memory Usage ---
    size_t usedFrames = 0;
    for (auto& frame : physicalMemory) {
        if (frame.processId != -1) ++usedFrames;
    }
    uint64_t frameSize = GLOBAL_CONFIG.memPerFrame;
    uint64_t usedBytes = usedFrames * frameSize;
    uint64_t totalBytes = GLOBAL_CONFIG.maxOverallMem;
    double   memUtilPct = totalBytes ? (100.0 * usedBytes / totalBytes) : 0.0;

    cout << "Memory Usage:    "
        << usedBytes << " bytes / "
        << totalBytes << " bytes ("
        << memUtilPct << "%)\n\n";

    // --- Per‐Process Memory Usage ---
    cout << "Running Processes Memory Usage:\n";
    for (auto& [name, procPtr] : manager.getProcesses()) {
        if (procPtr->isFinished) continue;
        size_t loadedPages = 0;
        for (auto& e : procPtr->pageTable)
            if (e.inMemory) ++loadedPages;
        uint64_t procUsedBytes = loadedPages * frameSize;
        cout << "  " << name << ": "
            << procUsedBytes << " / "
            << procPtr->memorySize << " bytes\n";
    }
    cout << endl;
}


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
            cv.wait_for(lock, chrono::milliseconds(1), [] {
                return (!fcfsQueue.empty() || !rrQueue.empty()) || stopScheduler;
                });

            // Count total CPU tick regardless of whether a process is found
            totalCpuTicks++;

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
            // Core is working this cycle
            activeCpuTicks++;

            proc->coreAssigned = coreId;

            if (GLOBAL_CONFIG.scheduler == "fcfs") {
                while (proc->currentLine < proc->totalLine && !stopScheduler) {
                    instructions_manager(proc->currentLine, proc->instructions, proc->memory, proc->name, coreId, proc);
                    proc->currentLine++;
                    this_thread::sleep_for(chrono::milliseconds(GLOBAL_CONFIG.delayPerExec));
                }
            }
            else if (GLOBAL_CONFIG.scheduler == "rr") {
                uint64_t executedInstructions = 0;
                while (proc->currentLine < proc->totalLine &&
                    executedInstructions < GLOBAL_CONFIG.quantumCycles &&
                    !stopScheduler) {

                    instructions_manager(proc->currentLine, proc->instructions, proc->memory, proc->name, coreId, proc);
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
        else {
            // Core idle this cycle
            idleCpuTicks++;
            this_thread::sleep_for(chrono::milliseconds(GLOBAL_CONFIG.delayPerExec));
        }
    }
}

bool validateCustomInstructions(const string& raw) {
    // Split on ‘;’
    istringstream splitter(raw);
    string instr;
    // pre-compile all your allowed patterns:
    static const vector<regex> patterns = {
        regex(R"(^DECLARE\s+[A-Za-z_]\w*\s+\d+$)"),
        regex(R"(^ADD\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*$)"),
        regex(R"(^SUBTRACT\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*\s+[A-Za-z_]\w*$)"),
        regex(R"(^PRINT\(\s*"Result: "\s*\+\s*[A-Za-z_]\w*\s*\)$)"),
        regex(R"(^WRITE\s+0x[0-9A-Fa-f]+\s+(?:\d+|[A-Za-z_]\w*)$)"),
        regex(R"(^READ\s+[A-Za-z_]\w*\s+0x[0-9A-Fa-f]+$)")
    };

    while (getline(splitter, instr, ';')) {
        // trim leading/trailing whitespace
        size_t start = instr.find_first_not_of(" \t");
        if (start == string::npos) continue;              // skip empty
        size_t end = instr.find_last_not_of(" \t");
        string t = instr.substr(start, end - start + 1);
        bool ok = false;
        for (auto& re : patterns) {
            if (regex_match(t, re)) { ok = true; break; }
        }
        if (!ok) return false;
    }
    return true;
}

void handleScreenCommand(const string& command, ProcessManager& manager) {
    istringstream iss(command);
    string cmd, option, processName;
    iss >> cmd >> option >> processName;
    // Try to read optional memSize argument:
    uint64_t requestedMem = 0;
    if (option == "-s") {
        if (!(iss >> requestedMem)) {
            // no extra arg, requestedMem stays 0
        }
    }

    if (option == "-c" && !processName.empty()) {
        uint64_t requestedMem = 0;
        iss >> requestedMem;

        // Read the rest of the line as one quoted string
        string raw;
        getline(iss, raw);
        // Trim leading spaces
        if (!raw.empty() && raw.front() == ' ') raw.erase(0, 1);
        // Remove surrounding quotes if present
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
            raw = raw.substr(1, raw.size() - 2);
        }

        if (!validateCustomInstructions(raw)) {
            cout << "Error: one or more instructions are malformed.\n"
                << "Allowed forms:\n"
                << "  DECLARE <var> <value>\n"
                << "  ADD <v1> <v2> <v3>\n"
                << "  SUBTRACT <v1> <v2> <v3>\n"
                << "  PRINT(\"Result: \" + <var>)\n"
                << "  WRITE <0xHEXADDR> <value>\n"
                << "  READ <var> <0xHEXADDR>\n";
            return;
        }

        // Validate the requested memory (must be a power of two in range)
        try {
            requestedMem = clampMemPow2(requestedMem);
        }
        catch (const invalid_argument&) {
            cout << "Error: invalid memory size `" << requestedMem
                << "`. Must be a power of two between 64 and 65536.\n";
            return;
        }
        if (requestedMem < GLOBAL_CONFIG.minMemPerProc
            || requestedMem > GLOBAL_CONFIG.maxMemPerProc) {
            cout << "Error: requested memory " << requestedMem
                << " outside allowed range ["
                << GLOBAL_CONFIG.minMemPerProc << "-"
                << GLOBAL_CONFIG.maxMemPerProc << "].\n";
            return;
        }

        // Create the process
        manager.createProcess(processName);
        Process* proc = manager.retrieveProcess(processName);
        if (!proc) {
            cout << "Failed to create process " << processName << ".\n";
            return;
        }

        // Override its memory size & page table
        proc->memorySize = requestedMem;
        proc->pageTable.clear();
        proc->pageTable.resize(requestedMem / GLOBAL_CONFIG.memPerFrame);

        // Store the custom instructions
        //proc->customInstructions = raw;
        // split on ‘;’ and trim whitespace
        istringstream split(raw);
        string piece;
        while (getline(split, piece, ';')) {
            // trim
            size_t a = piece.find_first_not_of(" \t");
            size_t b = piece.find_last_not_of(" \t");
            if (a != string::npos) {
                proc->customInstrList.push_back(piece.substr(a, b - a + 1));
            }
        }

        // Enqueue and display
        {
            lock_guard<mutex> lock(queueMutex);
            if (GLOBAL_CONFIG.scheduler == "fcfs") {
                fcfsQueue.push(proc);
            }
            else {
                rrQueue.push(proc);
            }
        }
        displayProcess(*proc);
        printHeader();
        cv.notify_one();
    }
    else if (option == "-ls") {
        manager.listProcesses();
    }
    else if (option == "-s" && !processName.empty()) {
        // If they passed a mem-size, validate it now:
        if (requestedMem != 0) {
            try {
                requestedMem = clampMemPow2(requestedMem);
            }
            catch (const invalid_argument& ex) {
                cout << "Error: invalid memory size `" << requestedMem
                    << "`. Must be a power of two between 64 and 65536.\n";
                return;
            }
            // Also enforce it’s within min/max-per-proc:
            if (requestedMem < GLOBAL_CONFIG.minMemPerProc
                || requestedMem > GLOBAL_CONFIG.maxMemPerProc) {
                cout << "Error: requested memory " << requestedMem
                    << " outside allowed range ["
                    << GLOBAL_CONFIG.minMemPerProc << "-"
                    << GLOBAL_CONFIG.maxMemPerProc << "].\n";
                return;
            }
        }

        // Create with the normal random size first
        manager.createProcess(processName);
        Process* proc = manager.retrieveProcess(processName);
        if (!proc) {
            cout << "Failed to create process " << processName << ".\n";
            return;
        }

        // If the user provided a size, override it here:
        if (requestedMem != 0) {
            proc->memorySize = requestedMem;
            size_t newPageCount = requestedMem / GLOBAL_CONFIG.memPerFrame;
            proc->pageTable.clear();
            proc->pageTable.resize(newPageCount);
        }

        // Enqueue & display
        {
            lock_guard<mutex> lock(queueMutex);
            if (GLOBAL_CONFIG.scheduler == "fcfs") {
                fcfsQueue.push(proc);
            }
            else {
                rrQueue.push(proc);
            }
        }
        displayProcess(*proc);
        printHeader();
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
            this_thread::sleep_for(chrono::milliseconds(1750));
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

void printPhysicalMemory() {
    std::lock_guard<std::mutex> lock(memMutex);
    cout << "\n[Physical Memory State]\n";
    for (size_t i = 0; i < physicalMemory.size(); ++i) {
        const Frame& f = physicalMemory[i];
        cout << "Frame " << i << ": ";
        if (f.processId == -1) {
            cout << "FREE\n";
        }
        else {
            cout << "PID=" << f.processId << ", Page=" << f.pageNumber
                << ", Data=\"" << f.data << "\"\n";
        }
    }
    cout << "-----------------------------\n";
}

void printMemorySummary() {
    uint64_t totalMemory = GLOBAL_CONFIG.maxOverallMem;
    uint64_t usedMemory = 0;

    for (const auto& frame : physicalMemory) {
        if (frame.processId != -1) {
            usedMemory += GLOBAL_CONFIG.memPerFrame;
        }
    }

    uint64_t freeMemory = totalMemory > usedMemory ? totalMemory - usedMemory : 0;

    cout << "\n[Memory Summary]\n";
    cout << "Total memory     : " << totalMemory << " bytes\n";
    cout << "Used  memory     : " << usedMemory << " bytes\n";
    cout << "Free  memory     : " << freeMemory << " bytes\n";

    cout << "\n[CPU Tick Summary]\n";
    cout << "Active CPU ticks : " << activeCpuTicks.load() << endl;
    cout << "Idle   CPU ticks : " << idleCpuTicks.load() << endl;
    cout << "Total  CPU ticks : " << totalCpuTicks.load() << endl;

    cout << "\n[Paging Summary]\n";
    cout << "Num paged in     : " << pageInCount.load() << endl;
    cout << "Num paged out    : " << pageOutCount.load() << endl;

    cout << "-----------------------------\n";
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
                size_t numFrames = GLOBAL_CONFIG.maxOverallMem / GLOBAL_CONFIG.memPerFrame;
                physicalMemory.assign(numFrames, Frame());

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
                cout << "Initialized physical memory with " << numFrames << " frames.\n";
                cout << "--------------------------------------------\n";

                printPhysicalMemory();

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
        else if (command == "check") {
            printPhysicalMemory();
        }
        else if (command == "backing") {
            std::lock_guard<std::mutex> lock(memMutex);
            cout << "\n[Backing Store Contents]\n";
            for (const auto& [key, val] : backingStore) {
                cout << "Process " << key.first << ", Page " << key.second
                    << " => \"" << val << "\"\n";
            }
            cout << "-----------------------------\n";

            printPhysicalMemory();
        }
        else if (command == "stats") {
            displaySystemStats(manager);
        }
        else if (command == "vmstats") {
            printMemorySummary();
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