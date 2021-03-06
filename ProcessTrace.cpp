/*
 * ProcessTrace implementation 
 */

/* 
 * File:   ProcessTrace.cpp
 * Author: Mike Goss <mikegoss@cs.du.edu>
 * 
 */

#include "ProcessTrace.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace mem;

using std::cin;
using std::cout;
using std::cerr;
using std::getline;
using std::istringstream;
using std::string;
using std::vector;

ProcessTrace::ProcessTrace(MMU &memory_,
        PageFrameAllocator &allocator_,
        string file_name_, int id)
: memory(memory_), allocator(allocator_), file_name(file_name_),
line_number(0), id_number(id), allocated_pages(0) {
    // Open the trace file.  Abort program if can't open.
    trace.open(file_name, std::ios_base::in);
    if (!trace.is_open()) {
        cerr << "ERROR: failed to open trace file: " << file_name << "\n";
        exit(2);
    }

    vector<mem::Addr> allocated;
    memory.set_PMCB(pmem_pmcb);
    allocator.Allocate(1, allocated);
    vmem_pmcb = mem::PMCB(true, allocated[0]); // initialize PMCB
    memory.set_PMCB(vmem_pmcb);
}

ProcessTrace::~ProcessTrace() {
    trace.close();
}

int ProcessTrace::Execute(int num_lines) {
    // Read and process commands
    string line; // text line read
    string cmd; // command from line
    vector<uint32_t> cmdArgs; // arguments from line

    //make sure MMU is in virtual mode
    memory.set_PMCB(vmem_pmcb);

    // Select the command to execute
    for (int i = 0; i < num_lines; ++i) {
        if (ParseCommand(line, cmd, cmdArgs)) {
            if (cmd == "quota") {
                CmdQuota(line, cmd, cmdArgs); // allocate memory
            } else if (cmd == "compare") {
                CmdCompare(line, cmd, cmdArgs); // get and compare multiple bytes
            } else if (cmd == "put") {
                if (!CmdPut(line, cmd, cmdArgs)) {
                    cout << "ERROR: memory quota " << std::hex << QUOTA << " exceeded" << std::endl;
                    return i;
                } // put bytes
            } else if (cmd == "fill") {
                if (!CmdFill(line, cmd, cmdArgs)) {
                    cout << "ERROR: memory quota " << std::hex << QUOTA << " exceeded" << std::endl;
                    return i;
                } // fill bytes with value
            } else if (cmd == "copy") {
                if (!CmdCopy(line, cmd, cmdArgs)) {
                    cout << "ERROR: memory quota " << std::hex << QUOTA << " exceeded" << std::endl;
                    return i;
                } // copy bytes to dest from source
            } else if (cmd == "dump") {
                CmdDump(line, cmd, cmdArgs); // dump byte values to output
            } else if (cmd == "writable") {
                CmdWritable(line, cmd, cmdArgs); // change writable status of page(s)
            } else {
                if (!cmd.empty()) { // if not comment
                    cerr << "ERROR: invalid command at line " << line_number << ":\n"
                            << line << "\n";
                    exit(2);
                }
            }
        } else {
            return i; //lines executed before termination
        }
    }
    memory.get_PMCB(vmem_pmcb);
    return num_lines;
}

bool ProcessTrace::ParseCommand(
        string &line, string &cmd, vector<uint32_t> &cmdArgs) {
    cmdArgs.clear();
    line.clear();
    cmd.clear();

    // Read next line
    if (std::getline(trace, line)) {
        ++line_number;
        cout << std::dec << line_number
                << ":" << id_number << ":" << line << "\n";


        // If not comment
        if (line.at(0) != '#') {

            // Make a string stream from command line
            istringstream lineStream(line);

            // Get command
            lineStream >> cmd;

            // Get arguments
            uint32_t arg;
            while (lineStream >> std::hex >> arg) {
                cmdArgs.push_back(arg);
            }
        }
        return true;
    } else if (trace.eof()) {
        return false;
    } else {
        cerr << "ERROR: getline failed on trace file: " << file_name
                << "at line " << line_number << "\n";
        exit(2);
    }
}

void ProcessTrace::CmdQuota(const std::string& line,
        const std::string& cmd,
        const std::vector<uint32_t>& cmdArgs) {
    QUOTA = cmdArgs.at(0);
}

void ProcessTrace::CmdCompare(const string &line,
        const string &cmd,
        const vector<uint32_t> &cmdArgs) {
    uint32_t addr = cmdArgs.at(0);

    // Compare specified byte values
    size_t num_bytes = cmdArgs.size() - 1;
    uint8_t buffer[num_bytes];
    try {
        memory.get_bytes(buffer, addr, num_bytes);
        for (int i = 1; i < cmdArgs.size(); ++i) {
            if (buffer[i - 1] != cmdArgs.at(i)) {
                cout << "compare error at address " << std::hex << addr
                        << ", expected " << static_cast<uint32_t> (cmdArgs.at(i))
                        << ", actual is " << static_cast<uint32_t> (buffer[i - 1]) << "\n";
            }
            ++addr;
        }
    } catch (PageFaultException e) {
        PrintAndClearException("PageFaultException", e);
    }
}

bool ProcessTrace::CmdPut(const string &line,
        const string &cmd,
        const vector<uint32_t> &cmdArgs) {
    // Put multiple bytes starting at specified address
    uint32_t addr = cmdArgs.at(0);
    size_t num_bytes = cmdArgs.size() - 1;
    uint8_t buffer[num_bytes];
    int num_pages = (num_bytes + kPageSize - 1) / kPageSize;
    Addr bytes_written = 0;

    for (int i = 1; i < cmdArgs.size(); ++i) {
        buffer[i - 1] = cmdArgs.at(i);
    }
    bool complete = false;
    while (!complete) {
        //clear previous operation of pmcb
        vmem_pmcb.operation_state = mem::PMCB::NONE;
        memory.set_PMCB(vmem_pmcb);
        try {
            memory.put_bytes(addr, num_bytes, buffer);
            bytes_written = num_bytes; //all bytes were written successfully
        } catch (PageFaultException e) {
            memory.get_PMCB(vmem_pmcb);
            bytes_written = vmem_pmcb.next_vaddress - addr; //total number of successfully written bytes
            if (bytes_written != num_bytes) { //need to allocate a page
                if (allocated_pages == QUOTA) { //check process's quota
                    return false;
                } else { 
                    memory.set_PMCB(pmem_pmcb); //switch to physical mode
                    AllocateAndMapPage(vmem_pmcb.next_vaddress & mem::kPageNumberMask);
                    ++allocated_pages;
                }
            }
        } catch (WritePermissionFaultException e) {
            PrintAndClearException("WritePermissionFaultException", e);
            complete = true;
        }
        if (bytes_written == num_bytes) { //check if command completed
            complete = true;
        }
    }
    //make sure the MMU is in virtual mode before returning
    memory.set_PMCB(vmem_pmcb);
    return true;
}

bool ProcessTrace::CmdCopy(const string &line,
        const string &cmd,
        const vector<uint32_t> &cmdArgs) {
    // Copy specified number of bytes to destination from source
    Addr dst = cmdArgs.at(0);
    Addr src = cmdArgs.at(1);
    Addr num_bytes = cmdArgs.at(2);
    uint8_t buffer[num_bytes];
    Addr bytes_written = 0;
    int num_pages = num_bytes / kPageSize;

    // Try reading bytes
    Addr bytes_read = 0; // number of successfully read bytes
    try {
        memory.get_bytes(buffer, src, num_bytes);
    } catch (PageFaultException e) {
        PrintAndClearException("PageFaultException on read", e);
    }
    memory.get_PMCB(vmem_pmcb);
    bytes_read = vmem_pmcb.next_vaddress - src;
    
    // Try writing bytes
    bool filled = false;
    while (!filled) {
        //clear previous operation of pmcb
        vmem_pmcb.operation_state = mem::PMCB::NONE;
        memory.set_PMCB(vmem_pmcb);
        try {
            memory.put_bytes(dst, bytes_read, buffer);
            bytes_written = bytes_read; //all bytes written successfully
        } catch (PageFaultException e) {
            memory.get_PMCB(vmem_pmcb);
            bytes_written = vmem_pmcb.next_vaddress - dst; //total number of successfully written bytes
            if (bytes_written != bytes_read) { //check if need to allocate a page frame
                if (allocated_pages == QUOTA) { //check process's quota
                    return false;
                } else {
                    //switch to physical mode for allocation
                    memory.set_PMCB(pmem_pmcb);
                    AllocateAndMapPage(vmem_pmcb.next_vaddress & mem::kPageNumberMask);
                    ++allocated_pages;
                }
            }
        } catch (WritePermissionFaultException e) {
            PrintAndClearException("WritePermissionFaultException", e);
            filled = true;
        }
        if (bytes_written == bytes_read) { //check if command completed
            filled = true;
        }
    }
    //make sure MMU is in virtual mode before returning
    memory.set_PMCB(vmem_pmcb);
    return true;
}

bool ProcessTrace::CmdFill(const string &line,
        const string &cmd,
        const vector<uint32_t> &cmdArgs) {
    // Fill a sequence of bytes with the specified value
    Addr addr = cmdArgs.at(0);
    Addr num_bytes = cmdArgs.at(1);
    uint8_t val = cmdArgs.at(2);
    Addr bytes_written = 0;
    Addr starting_addr = cmdArgs.at(0);
    bool filled = false;
    while (!filled) {
        //clear previous operation of pmcb
        vmem_pmcb.operation_state = mem::PMCB::NONE;
        memory.set_PMCB(vmem_pmcb);
        try {
            int i = bytes_written;
            for (; i < num_bytes; ++i) {
                memory.put_byte(addr, &val);
                ++addr;
                ++bytes_written;
            }
        } catch (PageFaultException e) {
            memory.get_PMCB(vmem_pmcb);
            bytes_written = vmem_pmcb.next_vaddress - starting_addr; //total number of bytes written successfully
            if (bytes_written != num_bytes) { //check if need to allocate a page frame
                if (allocated_pages == QUOTA) { //check process's quota
                    return false;
                } else {
                    //switch to physical mode for allocation
                    memory.set_PMCB(pmem_pmcb);
                    AllocateAndMapPage(vmem_pmcb.next_vaddress & mem::kPageNumberMask);
                    ++allocated_pages;
                }
            }
        } catch (WritePermissionFaultException e) {
            PrintAndClearException("WritePermissionFaultException", e);
            filled = true;
        }
        if (bytes_written == num_bytes) { //check if command is completed
            filled = true;
        }
    }
    //make sure MMU is in virtual mode before returning
    memory.set_PMCB(vmem_pmcb);
    return true;
}

void ProcessTrace::CmdDump(const string &line,
        const string &cmd,
        const vector<uint32_t> &cmdArgs) {
    uint32_t addr = cmdArgs.at(0);
    uint32_t count = cmdArgs.at(1);

    // Output the address
    cout << std::hex << addr;

    // Output the specified number of bytes starting at the address
    try {
        for (int i = 0; i < count; ++i) {
            if ((i % 16) == 0) { // line break every 16 bytes
                cout << "\n";
            }
            uint8_t byte_val;
            memory.get_byte(&byte_val, addr++);
            cout << " " << std::setfill('0') << std::setw(2)
                    << static_cast<uint32_t> (byte_val);
        }
        cout << "\n";
    } catch (PageFaultException e) {
        cout << "\n";
        PrintAndClearException("PageFaultException", e);
    }
}

void ProcessTrace::CmdWritable(const string &line,
        const string &cmd,
        const vector<uint32_t> &cmdArgs) {
    // Get arguments
    Addr vaddr = cmdArgs.at(0);
    int count = cmdArgs.at(1) / kPageSize;
    bool writable = cmdArgs.at(2) != 0;

    // Switch to physical mode
    memory.get_PMCB(vmem_pmcb);
    memory.set_PMCB(pmem_pmcb);

    Addr pt_base = vmem_pmcb.page_table_base;

    // Modify pages in range
    while (count-- > 0) {
        SetWritableStatus(vaddr, writable);
        vaddr += 0x1000;
    }

    // Switch back to virtual mode
    memory.set_PMCB(vmem_pmcb);
}

void ProcessTrace::PrintAndClearException(const string &type,
        MemorySubsystemException e) {
    memory.get_PMCB(vmem_pmcb);
    cout << "Exception type " << type
            << " occurred at input line " << std::dec << std::setw(1)
            << line_number << " at virtual address 0x"
            << std::hex << std::setw(8) << std::setfill('0')
            << vmem_pmcb.next_vaddress
            << ": " << e.what() << "\n";
    vmem_pmcb.operation_state = PMCB::NONE;
    memory.set_PMCB(vmem_pmcb);
}

void ProcessTrace::AllocateAndMapPage(Addr vaddr) {
    // Get offset in L1 table of L2 entry for vaddr  
    Addr pt_base = vmem_pmcb.page_table_base;
    Addr pt_l1_offset = vaddr >> (kPageSizeBits + kPageTableSizeBits);

    // Get L1 page table entry
    Addr l1_entry_addr = pt_base + sizeof (PageTableEntry) * pt_l1_offset;
    PageTableEntry l1_entry;
    memory.get_bytes(reinterpret_cast<uint8_t*> (&l1_entry),
            l1_entry_addr, sizeof (PageTableEntry));

    // If no L1 entry for page, allocate and map one
    if ((l1_entry & kPTE_PresentMask) == 0) {
        vector<Addr> allocated;
        allocator.Allocate(1, allocated);
        l1_entry = allocated[0] | kPTE_PresentMask | kPTE_WritableMask;
        memory.put_bytes(l1_entry_addr, sizeof (PageTableEntry),
                reinterpret_cast<uint8_t*> (&l1_entry));
    }

    // Get L2 page table entry
    Addr pt_l2_addr = l1_entry & kPageNumberMask;
    Addr pt_l2_offset = (vaddr >> kPageSizeBits) & kPageTableIndexMask;
    Addr l2_entry_addr = pt_l2_addr + sizeof (PageTableEntry) * pt_l2_offset;
    PageTableEntry l2_entry;
    memory.get_bytes(reinterpret_cast<uint8_t*> (&l2_entry),
            l2_entry_addr, sizeof (PageTableEntry));

    // Error if page already allocated
    if ((l2_entry & kPTE_PresentMask) != 0) {
        cerr << "ERROR: duplicate allocated at vaddr = 0x"
                << std::hex << vaddr << "\n";
        throw std::bad_alloc();
    }

    // Allocate a page and set up page table entry
    vector<Addr> allocated;
    allocator.Allocate(1, allocated);
    l2_entry = allocated[0] | kPTE_PresentMask | kPTE_WritableMask;
    memory.put_bytes(l2_entry_addr, sizeof (PageTableEntry),
            reinterpret_cast<uint8_t*> (&l2_entry));
}

void ProcessTrace::SetWritableStatus(Addr vaddr, bool writable) {
    // Get offset in L1 table of L2 entry for vaddr  
    Addr pt_base = vmem_pmcb.page_table_base;
    Addr pt_l1_offset = vaddr >> (kPageSizeBits + kPageTableSizeBits);

    // Get L1 page table entry
    Addr l1_entry_addr = pt_base + sizeof (PageTableEntry) * pt_l1_offset;
    PageTableEntry l1_entry;
    memory.get_bytes(reinterpret_cast<uint8_t*> (&l1_entry),
            l1_entry_addr, sizeof (PageTableEntry));

    // If no L1 entry for page, ignore request
    if ((l1_entry & kPTE_PresentMask) == 0) {
        return;
    }

    // Get L2 page table entry
    Addr pt_l2_addr = l1_entry & kPageNumberMask;
    Addr pt_l2_offset = (vaddr >> kPageSizeBits) & kPageTableIndexMask;
    Addr l2_entry_addr = pt_l2_addr + sizeof (PageTableEntry) * pt_l2_offset;
    PageTableEntry l2_entry;
    memory.get_bytes(reinterpret_cast<uint8_t*> (&l2_entry),
            l2_entry_addr, sizeof (PageTableEntry));

    // Ignore request if page not present
    if ((l2_entry & kPTE_PresentMask) == 0) {
        return;
    }

    // Set status to requested value and rewrite entry
    l2_entry = (l2_entry & ~kPTE_WritableMask) | (writable ? kPTE_WritableMask : 0);
    memory.put_bytes(l2_entry_addr, sizeof (PageTableEntry),
            reinterpret_cast<uint8_t*> (&l2_entry));
}
