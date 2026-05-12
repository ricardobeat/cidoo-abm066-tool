module cidoo_clock_c3;

enum TransportKind : int {
    NONE,
    MACOS_IOKIT,
}

struct ClockTransport {
    TransportKind kind;
    void* impl;
}
