#pragma once

// Test only: `xerces_crash_test handlerfault` and `dumpfault` (crash_test.cpp,
// with -xerces-crash-test) crash the game and have the crash handler fault
// itself on the way: what a crash while memory is short does to it.
namespace xerces_crash_test {
// Where the crash handler is asked to fault: 0 nowhere, 1 in its unguarded
// tail (the handler is then entered again on the same thread), 2 inside the
// guarded in-memory dump (caught there; the handler goes on)
int handler_fault();
} // namespace xerces_crash_test
