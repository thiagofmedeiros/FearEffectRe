// CD-ROM controller at 1F801800h, as seen by the game's CD library:
// command/parameter/response FIFOs, INT1..INT5 with acknowledge, sector
// delivery at drive speed, and the data FIFO read through DMA channel 3.
#pragma once

#include <deque>
#include <memory>
#include <vector>

#include "disc/DiscImage.h"
#include "psx/Machine.h"

namespace fe {

struct Cdrom {
    explicit Cdrom(std::shared_ptr<DiscImage> image) : disc(std::move(image)) {}

    u8 read(Machine& m, u32 port);
    void write(Machine& m, u32 port, u8 value);
    void tick(Machine& m);                 // deliver whatever is due at m.clock
    u64 nextEvent() const;                 // clock value of the next thing to do, ~0 if idle
    void dmaRead(Machine& m, u32 addr, u32 bytes);
    // Disc change: the lid reads as open for two seconds, then the new disc is in.
    void insert(Machine& m, std::shared_ptr<DiscImage> image);

    std::shared_ptr<DiscImage> disc;
    u64 sectorsRead = 0, xaSectors = 0, commands = 0;
    int traceLeft = 0;
    int speed = 1;      // data reads and seeks this many times as fast (streams excepted)  // register accesses still to be written to the machine log

    void state(StateIO& io);
private:
    struct Pending {
        u64 time;
        u8 irq;
        std::vector<u8> bytes;
    };
    void command(Machine& m, u8 cmd);
    void push(u64 time, u8 irq, std::vector<u8> bytes) { queue.push_back({time, irq, std::move(bytes)}); }
    void deliver(Machine& m, u8 irq, std::vector<u8> bytes);

    u8 index = 0, ifl = 0;
    u8 ie = 0x1F;  // the BIOS enables all CD interrupts before it starts the game
    u8 stat = 0x02, mode = 0, filterFile = 0, filterChan = 0;
    bool busy = false, reading = false, setlocPending = false;
    u32 setloc = 0, lba = 0;
    u64 nextSector = 0;
    std::vector<u8> params;
    std::vector<u8> response;
    size_t responsePos = 0;
    std::deque<Pending> queue;
    std::vector<u8> sectorBuf;  // what request-data loads into the FIFO
    std::vector<u8> data;
    size_t dataPos = 0;
    bool fresh = false;
    u64 lidOpenUntil = 0;
    bool lidWasOpen = false;    // "shell open" stays in the status until Getstat reports it         // a sector arrived that the FIFO has not been loaded with
    u8 header[8]{};             // header + subheader of the last sector read
};

}  // namespace fe
