#include "talsa.h"

#include <cerrno>
#include <deque>
#include <gtest/gtest.h>
#include <string>

namespace {
struct Result { int frames; int error; };
std::deque<Result> results;
std::string calls;
int stopResult;
int prepareResult;
int16_t samples[64]{};
int token;
pcm* device = reinterpret_cast<pcm*>(&token);
using android::hardware::audio::CPP_VERSION::implementation::talsa::pcmWrite;
using android::hardware::audio::CPP_VERSION::implementation::talsa::pcmRead;

class PcmTransferTest : public testing::Test {
    void SetUp() override {
        results.clear();
        calls.clear();
        stopResult = prepareResult = 0;
    }
};
using PcmWriteTest = PcmTransferTest;
using PcmReadTest = PcmTransferTest;
}

extern "C" int __wrap_pcm_writei(pcm* handle, const void*, unsigned int frames) {
    EXPECT_EQ(device, handle);
    EXPECT_EQ(32u, frames);
    calls += 'W';
    if (results.empty()) {
        ADD_FAILURE() << "Unexpected write retry";
        errno = EINVAL;
        return -1;
    }
    const auto result = results.front();
    results.pop_front();
    errno = result.error;
    return result.frames;
}
extern "C" int __wrap_pcm_readi(pcm* handle, void*, unsigned int frames) {
    EXPECT_EQ(device, handle);
    EXPECT_EQ(32u, frames);
    calls += 'R';
    if (results.empty()) {
        ADD_FAILURE() << "Unexpected read retry";
        errno = EINVAL;
        return -1;
    }
    const auto result = results.front();
    results.pop_front();
    errno = result.error;
    return result.frames;
}
extern "C" int __wrap_pcm_stop(pcm* handle) {
    EXPECT_EQ(device, handle);
    calls += 'S';
    return stopResult;
}
extern "C" int __wrap_pcm_prepare(pcm* handle) {
    EXPECT_EQ(device, handle);
    calls += 'P';
    return prepareResult;
}
extern "C" const char* __wrap_pcm_get_error(const pcm*) {
    return "injected PCM failure";
}

TEST_F(PcmWriteTest, RecoversErrnoIoFailureBeforeRetrying) {
    results = {{-1, EIO}, {32, 0}};
    EXPECT_EQ(128, pcmWrite(device, samples, sizeof(samples), 4));
    EXPECT_EQ("WSPW", calls);
}
TEST_F(PcmWriteTest, AcceptsDirectNegativeErrorCodes) {
    results = {{-EIO, EINVAL}, {32, 0}};
    EXPECT_EQ(128, pcmWrite(device, samples, sizeof(samples), 4));
    EXPECT_EQ("WSPW", calls);
}
TEST_F(PcmWriteTest, TransientFailureDoesNotResetPlayback) {
    results = {{-1, EAGAIN}, {32, 0}};
    EXPECT_EQ(128, pcmWrite(device, samples, sizeof(samples), 4));
    EXPECT_EQ("WW", calls);
}
TEST_F(PcmWriteTest, PersistentFailureHasBoundedRetries) {
    results = {{-1, EIO}, {-1, EIO}, {-1, EIO}};
    EXPECT_EQ(-1, pcmWrite(device, samples, sizeof(samples), 4));
    EXPECT_EQ("WSPWSPW", calls);
}
TEST_F(PcmWriteTest, FailedStopDoesNotPrepareRunningStream) {
    stopResult = -1;
    results = {{-1, EIO}};
    EXPECT_EQ(-1, pcmWrite(device, samples, sizeof(samples), 4));
    EXPECT_EQ("WS", calls);
}
TEST_F(PcmWriteTest, FailedPrepareDoesNotRetryWrite) {
    prepareResult = -1;
    results = {{-1, EIO}};
    EXPECT_EQ(-1, pcmWrite(device, samples, sizeof(samples), 4));
    EXPECT_EQ("WSP", calls);
}
TEST_F(PcmWriteTest, PartialWriteReportsOnlyTransferredBytes) {
    results = {{8, 0}};
    EXPECT_EQ(32, pcmWrite(device, samples, sizeof(samples), 4));
    EXPECT_EQ("W", calls);
}
TEST_F(PcmWriteTest, OtherErrorsAndZeroProgressDoNotLoop) {
    results = {{-1, ENODEV}, {0, 0}};
    EXPECT_EQ(-1, pcmWrite(device, samples, sizeof(samples), 4));
    EXPECT_EQ(-1, pcmWrite(device, samples, sizeof(samples), 4));
    EXPECT_EQ("WW", calls);
}
TEST_F(PcmWriteTest, EmptyWriteDoesNotTouchDevice) {
    EXPECT_EQ(0, pcmWrite(device, samples, 0, 4));
    EXPECT_EQ("", calls);
}

TEST_F(PcmReadTest, RecoversErrnoIoFailureBeforeRetrying) {
    results = {{-1, EIO}, {32, 0}};
    EXPECT_EQ(128, pcmRead(device, samples, sizeof(samples), 4));
    EXPECT_EQ("RSPR", calls);
}
TEST_F(PcmReadTest, AcceptsDirectNegativeErrorCodes) {
    results = {{-EIO, EINVAL}, {32, 0}};
    EXPECT_EQ(128, pcmRead(device, samples, sizeof(samples), 4));
    EXPECT_EQ("RSPR", calls);
}
TEST_F(PcmReadTest, TransientFailureDoesNotResetCapture) {
    results = {{-1, EAGAIN}, {32, 0}};
    EXPECT_EQ(128, pcmRead(device, samples, sizeof(samples), 4));
    EXPECT_EQ("RR", calls);
}
TEST_F(PcmReadTest, PersistentFailureHasBoundedRetries) {
    results = {{-1, EIO}, {-1, EIO}, {-1, EIO}};
    EXPECT_EQ(-1, pcmRead(device, samples, sizeof(samples), 4));
    EXPECT_EQ("RSPRSPR", calls);
}
TEST_F(PcmReadTest, FailedStopDoesNotPrepareRunningStream) {
    stopResult = -1;
    results = {{-1, EIO}};
    EXPECT_EQ(-1, pcmRead(device, samples, sizeof(samples), 4));
    EXPECT_EQ("RS", calls);
}
TEST_F(PcmReadTest, FailedPrepareDoesNotRetryRead) {
    prepareResult = -1;
    results = {{-1, EIO}};
    EXPECT_EQ(-1, pcmRead(device, samples, sizeof(samples), 4));
    EXPECT_EQ("RSP", calls);
}
TEST_F(PcmReadTest, PartialReadReportsOnlyTransferredBytes) {
    results = {{8, 0}};
    EXPECT_EQ(32, pcmRead(device, samples, sizeof(samples), 4));
    EXPECT_EQ("R", calls);
}
TEST_F(PcmReadTest, OtherErrorsAndZeroProgressDoNotLoop) {
    results = {{-1, ENODEV}, {0, 0}};
    EXPECT_EQ(-1, pcmRead(device, samples, sizeof(samples), 4));
    EXPECT_EQ(-1, pcmRead(device, samples, sizeof(samples), 4));
    EXPECT_EQ("RR", calls);
}
TEST_F(PcmReadTest, EmptyReadDoesNotTouchDevice) {
    EXPECT_EQ(0, pcmRead(device, samples, 0, 4));
    EXPECT_EQ("", calls);
}
