// Test-only module, never shipped. Models SLSsteam's existing SendAndRecv hook.
#include <cstdint>
static unsigned calls;
extern "C" bool SlsTestSendWait(void*, void*, int login, int timeout, void*, uint32_t msg) {
    ++calls;
    return login == 1 && timeout == 10 && msg == 821;
}
extern "C" unsigned SlsTestCallCount() { return calls; }
