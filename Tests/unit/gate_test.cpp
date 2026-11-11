// The import gate: gallery imports share a cap on OCCT workers; a file being opened never waits.
#include "library/gate.h"

#include <atomic>
#include <doctest/doctest.h>
#include <thread>
#include <vector>

using geo::library::ImportGate;

TEST_CASE("gallery imports share the gate's workers, and enter again as tickets return") {
    ImportGate gate(8);
    ImportGate::Ticket a = gate.tryEnter(3);
    ImportGate::Ticket b = gate.tryEnter(3);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(gate.held() == 6);
    CHECK_FALSE(gate.tryEnter(3)); // 9 would pass the cap
    ImportGate::Ticket c = gate.tryEnter(2);
    CHECK(c);
    a.release();
    CHECK(gate.held() == 5);
    CHECK(gate.tryEnter(3)); // a temporary, returned at once
    CHECK(gate.held() == 5);
}

TEST_CASE("asking for more than the cap asks for the cap") {
    ImportGate gate(4);
    ImportGate::Ticket all = gate.tryEnter(99);
    REQUIRE(all);
    CHECK(all.workers() == 4);
    CHECK_FALSE(gate.tryEnter(1));
}

TEST_CASE("a file being opened is admitted at once, and holds gallery imports back meanwhile") {
    ImportGate gate(4);
    ImportGate::Ticket gallery = gate.tryEnter(4);
    REQUIRE(gallery);
    std::atomic<int> told = 0;
    const geo::u64 listener = gate.listen([&] { ++told; });
    {
        ImportGate::Ticket opening = gate.open();
        CHECK(opening);
        CHECK(told == 1); // so the gallery can make way
        gallery.release();
        CHECK_FALSE(gate.tryEnter(1)); // nothing starts while a file is being opened
    }
    CHECK(gate.opens() == 0);
    CHECK(gate.tryEnter(1));
    gate.unlisten(listener);
    const int before = told;
    {
        ImportGate::Ticket opening = gate.open();
    }
    CHECK(told == before);
}

TEST_CASE("tickets taken and returned on many threads balance") {
    ImportGate gate(6);
    std::atomic<int> admitted = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) {
                if (ImportGate::Ticket ticket = gate.tryEnter(2)) {
                    CHECK(gate.held() <= 6);
                    ++admitted;
                }
                if (i % 100 == 0) {
                    ImportGate::Ticket opening = gate.open();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(admitted > 0);
    CHECK(gate.held() == 0);
    CHECK(gate.opens() == 0);
}
