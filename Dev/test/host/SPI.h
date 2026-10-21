#pragma once
struct MockSPI {
    void begin(int, int, int, int) {}
    void end() {}
    void setFrequency(int) {}
};
inline MockSPI SPI;
