// EventLoop 平台适配契约：真实唤醒、无空转以及单调定时器。
//
// Windows 版 muduo 以前的 wakeup() / handleRead() 是空操作:poll 超时取最近定时器的剩余时间,上限
// kPollTimeMs = 100ms,跨线程 queueInLoop 与 pending functor 里再排的任务都要等 poll 自己超时才执行。
// 回环 UDP 唤醒 socket 必须真正唤醒 poll。下面每条用例的 loop 上只挂一个远期的兜底 quit 定时器(用例自己的收尾定时器除外),
// 让 poll 每次都按满上限阻塞:唤醒失效时每个样本约 100ms,生效时在 1ms 量级。
// Linux 版用真 eventfd,这几条用例在两边都应通过。
//
// 跨线程投递本身就是被测对象,所以第一、三条用例各起一个投递线程;
// 它只调用线程安全的 queueInLoop,等待一律带超时,loop() 返回后先 join 再断言(join 之前不放
// ASSERT,免得 std::thread 析构时 terminate)。回调只在 loop 线程上读写用例体的局部变量;loop 由夹具持有,
// 最先构造、最后析构。

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

// muduo 头放在所有标准库头之后(它的 Windows 适配头 #define 了 off_t / pid_t 等名字)。
#include "muduo/base/Logging.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/InetAddress.h"
#include "muduo/net/Socket.h"
#include "muduo/net/SocketsOps.h"
#include "muduo/net/TcpClient.h"
#include "muduo/net/TimerQueue.h"

namespace
{

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// 唤醒失效时样本约 100ms(Windows 的 kPollTimeMs);阈值远低于它,同时给 CI 的调度抖动留余量。
constexpr Clock::duration kMaxWakeupLatency = 30ms;
// 兜底:用例出错时也能退出 loop。离得足够远,不会把 poll 超时压到 100ms 以下。
constexpr double kSafetyQuitSeconds = 5.0;
// 投递线程等某一轮执行完的上限;超过说明 loop 已被兜底定时器停掉。
constexpr auto kPosterWait = 2s;
constexpr int kRounds = 5;

// 空闲窗口:Windows 上 poll 每 100ms 超时一次,窗口里约 3~4 轮;Linux 由 timerfd 唤醒,约 1~2 轮。
// 唤醒数据报没读干净时,水平触发的 WSAPoll 每轮立刻返回:每次只读一个的实现,轮数接近积压的数据报个数
// (至多 kBurst + 1,受接收缓冲限制);完全不读则一直空转到窗口结束。
constexpr double kIdleSeconds = 0.3;
constexpr int64_t kMaxIdleIterations = 20;
constexpr int kBurst = 200;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

class SocketRuntimeTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        muduo::Logger::setLogLevel(muduo::Logger::WARN);
#ifdef WIN32
        WSADATA data;
        ASSERT_EQ(0, ::WSAStartup(MAKEWORD(2, 2), &data));
#endif
    }

    static void TearDownTestSuite()
    {
#ifdef WIN32
        ::WSACleanup();
#endif
    }
};

class EventLoopWakeupTest : public SocketRuntimeTest
{
protected:
    void SetUp() override
    {
        loop_.runAfter(kSafetyQuitSeconds, [this] { loop_.quit(); });
    }

    muduo::net::EventLoop loop_;
};

// 清理回归需要自行控制 EventLoop 的构造与析构，复用同一套套件级 socket 初始化。
class TimerQueueCleanupTest : public SocketRuntimeTest
{
};

// 别的线程 queueInLoop 时 loop 正阻塞在 poll 里(或刚跑完上一轮的 pending functor、即将进入 poll):
// queueInLoop 跨线程必须 wakeup,任务应当立刻执行,而不是等 poll 超时。
TEST_F(EventLoopWakeupTest, CrossThreadQueueInLoopWakesBlockedPoll)
{
    std::vector<Clock::duration> latencies;  // 只在 loop 线程上写;loop() 返回、投递线程 join 之后才读
    muduo::net::EventLoop &loop = loop_;

    std::thread poster([&loop, &latencies]
    {
        // 第 0 轮预热:等 loop 真正转起来,不计入样本。之后每一轮都在上一轮的任务执行完后才投递,
        // 投递时 loop 要么还在本轮 doPendingFunctors 里,要么已回到 poll,两种情况都只能靠 wakeup 及时执行。
        for (int round = 0; round <= kRounds; ++round)
        {
            auto executed = std::make_shared<std::promise<void>>();
            std::future<void> executedFuture = executed->get_future();
            const bool measured = round > 0;
            const Clock::time_point postedAt = Clock::now();
            loop.queueInLoop([executed, measured, postedAt, &latencies]
            {
                if (measured)
                {
                    latencies.push_back(Clock::now() - postedAt);
                }
                executed->set_value();
            });
            if (executedFuture.wait_for(kPosterWait) != std::future_status::ready)
            {
                break;  // loop 已停,样本不足由主线程断言
            }
        }
        loop.queueInLoop([&loop] { loop.quit(); });
    });
    loop.loop();
    poster.join();

    ASSERT_EQ(latencies.size(), static_cast<size_t>(kRounds)) << "loop stopped before every round ran";
    for (size_t i = 0; i < latencies.size(); ++i)
    {
        EXPECT_LT(latencies[i], kMaxWakeupLatency)
            << "round " << i + 1 << ": cross-thread queueInLoop waited " << ToMilliseconds(latencies[i])
            << " ms; poll was not woken up (Windows caps the poll timeout at 100 ms)";
    }
}

// pending functor 里再 queueInLoop:此时 callingPendingFunctors_ 为真,本轮已经 swap 过队列,
// 新任务只能在下一轮执行,queueInLoop 必须 wakeup 让下一轮 poll 立即返回。
TEST_F(EventLoopWakeupTest, ChainedQueueInLoopFromPendingFunctorRunsPromptly)
{
    std::vector<Clock::duration> latencies;
    muduo::net::EventLoop &loop = loop_;

    std::function<void(int, Clock::time_point)> link;
    link = [&loop, &latencies, &link](int remaining, Clock::time_point queuedAt)
    {
        latencies.push_back(Clock::now() - queuedAt);
        if (remaining == 0)
        {
            loop.quit();
            return;
        }
        const Clock::time_point now = Clock::now();
        loop.queueInLoop([&link, remaining, now] { link(remaining - 1, now); });
    };
    // 起点放在立即到期的定时器里:定时器回调不在 pending functor 里,它排的第一环本轮就会执行;
    // 之后每一环都是「pending functor 里再排」。这个定时器触发一次就没了,之后 loop 上只剩兜底定时器。
    loop.runAfter(0.0, [&loop, &link]
    {
        const Clock::time_point now = Clock::now();
        loop.queueInLoop([&link, now] { link(kRounds, now); });
    });
    loop.loop();

    ASSERT_EQ(latencies.size(), static_cast<size_t>(kRounds + 1)) << "loop stopped before the chain finished";
    for (size_t i = 0; i < latencies.size(); ++i)
    {
        EXPECT_LT(latencies[i], kMaxWakeupLatency)
            << "link " << i << ": queueInLoop from a pending functor waited " << ToMilliseconds(latencies[i])
            << " ms; the next poll was not woken up";
    }
}

// 一阵密集的跨线程投递会在唤醒 socket 里积压很多个唤醒数据报。handleRead 必须一次读干净,
// 否则水平触发的 WSAPoll 在之后的空闲期里每轮立刻返回,loop 空转吃满一个核。
// 边投边读时积压多少取决于两个线程谁快,「每次只读一个」的实现可能恰好跟得上而漏检;所以投递期间先用一个
// 任务把 loop 线程挡住(等待带超时),让 kBurst 个唤醒确定堆在 socket 里,放行后的下一轮 handleRead 面对整批积压。
TEST_F(EventLoopWakeupTest, BurstOfWakeupsIsDrainedAndIdleLoopDoesNotSpin)
{
    int64_t idleStartIteration = -1;
    int64_t idleIterations = -1;
    muduo::net::EventLoop &loop = loop_;

    const auto blockerStarted = std::make_shared<std::promise<void>>();
    std::future<void> blockerStartedFuture = blockerStarted->get_future();
    const auto release = std::make_shared<std::promise<void>>();
    const std::shared_future<void> releaseFuture = release->get_future().share();

    std::thread poster([&loop, &idleStartIteration, &idleIterations, &blockerStartedFuture, blockerStarted, release,
                        releaseFuture]
    {
        loop.queueInLoop([blockerStarted, releaseFuture]
        {
            blockerStarted->set_value();
            (void)releaseFuture.wait_for(kPosterWait);
        });
        // loop 没在时限内转起来也照投:积压不再确定,但断言照常进行,不会卡住。
        (void)blockerStartedFuture.wait_for(kPosterWait);
        for (int i = 0; i < kBurst; ++i)
        {
            loop.queueInLoop([] {});
        }
        // 最后一个任务在 loop 线程上开始计空闲窗口;窗口里没有任何投递,只有兜底定时器和收尾定时器。
        loop.queueInLoop([&loop, &idleStartIteration, &idleIterations]
        {
            idleStartIteration = loop.iteration();
            loop.runAfter(kIdleSeconds, [&loop, &idleStartIteration, &idleIterations]
            {
                idleIterations = loop.iteration() - idleStartIteration;
                loop.quit();
            });
        });
        release->set_value();
    });
    loop.loop();
    poster.join();

    ASSERT_GE(idleIterations, 0) << "idle window never finished";
    EXPECT_LT(idleIterations, kMaxIdleIterations)
        << "loop iterated " << idleIterations << " times in a " << kIdleSeconds * 1000
        << " ms idle window; the wakeup fd was left readable and poll kept returning at once";
}

TEST_F(EventLoopWakeupTest, ShortRepeatingTimerUsesNearestDeadline)
{
    int fires = 0;
    const auto started = Clock::now();
    const auto timer = loop_.runEvery(0.005, [this, &fires]
    {
        if (++fires == 4)
            loop_.quit();
    });
    loop_.loop();
    const auto elapsed = Clock::now() - started;
    loop_.cancel(timer);

    EXPECT_EQ(4, fires);
    // 固定 100ms poll 的旧实现需要约 400ms；保留调度抖动余量。
    EXPECT_LT(elapsed, 250ms);
}

TEST_F(EventLoopWakeupTest, CancelledTimerInSameExpiredBatchDoesNotRun)
{
    int cancelledFires = 0;
    muduo::net::TimerId victim;
    const auto now = muduo::Timestamp::now();
    loop_.runAt(muduo::addTime(now, -2.0), [this, &victim]
    {
        loop_.cancel(victim);
        loop_.queueInLoop([this] { loop_.quit(); });
    });
    victim = loop_.runAt(muduo::addTime(now, -1.0), [&cancelledFires] { ++cancelledFires; });
    loop_.loop();
    EXPECT_EQ(0, cancelledFires);
}

TEST_F(EventLoopWakeupTest, RefusedConnectionReportsFailureAndStopSuppressesRetry)
{
    // 保留系统分配的端口但不 listen，整个测试期间不会被其他进程抢作监听端口。
    muduo::net::Socket reservedPort(muduo::net::sockets::createNonblockingOrDie(AF_INET));
    reservedPort.bindAddress(muduo::net::InetAddress("127.0.0.1", 0));
    const muduo::net::InetAddress refusedAddress(muduo::net::sockets::getLocalAddr(reservedPort.fd()));

    struct Observation
    {
        int failures = 0;
        int error = 0;
        bool ranInLoop = false;
        bool observationFinished = false;
        Clock::time_point failedAt;
        Clock::duration observationDuration{};
    };
    auto observed = std::make_shared<Observation>();
    {
        auto client = std::make_shared<muduo::net::TcpClient>(&loop_, refusedAddress, "refused-contract");
        std::weak_ptr<muduo::net::TcpClient> weakClient(client);
        client->setConnectFailedCallback([this, weakClient, observed](int savedErrno)
        {
            ++observed->failures;
            observed->error = savedErrno;
            observed->ranInLoop = loop_.isInLoopThread();
            if (const auto activeClient = weakClient.lock())
                activeClient->stop();
            if (observed->failures == 1)
            {
                observed->failedAt = Clock::now();
                // 首轮内部重试延迟是 500ms；观察到 800ms 仍只能收到一次失败。
                loop_.runAfter(0.8, [this, observed]
                {
                    observed->observationDuration = Clock::now() - observed->failedAt;
                    observed->observationFinished = true;
                    loop_.quit();
                });
            }
        });
        client->connect();
        loop_.loop();
    }

    // ~TcpClient 的排队 stop/resetChannel 与 1s Connector 续命都在 EventLoop 销毁前清理。
    loop_.runAfter(1.2, [this] { loop_.quit(); });
    loop_.loop();

    EXPECT_TRUE(observed->observationFinished) << "拒连失败通知必须在安全超时前发生";
    EXPECT_GE(observed->observationDuration, 700ms) << "必须覆盖首轮 500ms 内部重试窗口";
    EXPECT_EQ(1, observed->failures) << "回调中 stop 后不得再进行内部重试";
    EXPECT_NE(0, observed->error);
    EXPECT_TRUE(observed->ranInLoop);
}

TEST_F(EventLoopWakeupTest, PastRunAtTimersKeepWallClockOrder)
{
    std::vector<int> order;
    const auto now = muduo::Timestamp::now();
    loop_.runAt(muduo::addTime(now, -1.0), [&order] { order.push_back(3); });
    loop_.runAt(muduo::addTime(now, -3.0), [&order] { order.push_back(1); });
    loop_.runAt(muduo::addTime(now, -2.0), [&order] { order.push_back(2); });
    loop_.runAt(muduo::addTime(now, -0.5), [this] { loop_.quit(); });
    loop_.loop();
    EXPECT_EQ((std::vector<int>{1, 2, 3}), order);
}

TEST_F(EventLoopWakeupTest, LongPastWallTimeFiresBeforeLaterTimer)
{
    int fires = 0;
    loop_.runAt(muduo::Timestamp::fromUnixTime(1), [&fires] { ++fires; });
    loop_.runAfter(0.02, [this] { loop_.quit(); });
    loop_.loop();
    EXPECT_EQ(1, fires);
}

TEST_F(EventLoopWakeupTest, RunAtAndRunAfterShareTheSameClockDomain)
{
    std::vector<int> order;
    const auto now = muduo::Timestamp::now();
    loop_.runAt(muduo::addTime(now, 0.06), [&order] { order.push_back(2); });
    loop_.runAfter(0.02, [&order] { order.push_back(1); });
    loop_.runAt(muduo::addTime(now, 0.10), [this, &order]
    {
        order.push_back(3);
        loop_.quit();
    });
    loop_.loop();
    EXPECT_EQ((std::vector<int>{1, 2, 3}), order);
}

#ifdef WIN32
TEST_F(EventLoopWakeupTest, NextPollTimeoutUsesTheMonotonicDeadline)
{
    muduo::net::TimerQueue queue(&loop_);
    EXPECT_EQ(60000, queue.nextTimeoutMs(60000));
    queue.addTimer([] {}, muduo::addTime(muduo::net::TimerQueue::monotonicNow(), 10.0), 0.0);
    const int timeoutMs = queue.nextTimeoutMs(60000);
    EXPECT_GT(timeoutMs, 9000);
    EXPECT_LE(timeoutMs, 10000);
    EXPECT_EQ(100, queue.nextTimeoutMs(100));
    queue.addTimer([] {}, muduo::addTime(muduo::net::TimerQueue::monotonicNow(), -1.0), 0.0);
    EXPECT_EQ(0, queue.nextTimeoutMs(60000));
}
#endif

TEST(TimerClockContractTest, MonotonicConversionPreservesOffsetsAndOrder)
{
    constexpr int64_t wallNowUs = 1'760'000'000LL * muduo::Timestamp::kMicroSecondsPerSecond;
    constexpr int64_t monoNowUs = 5LL * muduo::Timestamp::kMicroSecondsPerSecond;
    const int64_t offsetsUs[] = {-86'400'000'000LL, -10'000'000, -1, 0, 1, 50'000, 3'600'000'000LL};
    bool first = true;
    int64_t previousUs = 0;
    for (const auto offsetUs : offsetsUs)
    {
        const auto converted = muduo::net::TimerQueue::monotonicFromWall(
            muduo::Timestamp(wallNowUs + offsetUs), muduo::Timestamp(wallNowUs), muduo::Timestamp(monoNowUs));
        const auto currentUs = converted.microSecondsSinceEpoch();
        EXPECT_EQ(monoNowUs + offsetUs, currentUs);
        if (!first)
            EXPECT_LT(previousUs, currentUs);
        first = false;
        previousUs = currentUs;
    }
}

TEST(TimerClockContractTest, MonotonicNowDoesNotMoveBackwards)
{
    auto previous = muduo::net::TimerQueue::monotonicNow();
    for (int sample = 0; sample < 1000; ++sample)
    {
        const auto current = muduo::net::TimerQueue::monotonicNow();
        ASSERT_LE(previous.microSecondsSinceEpoch(), current.microSecondsSinceEpoch());
        previous = current;
    }
}

TEST_F(TimerQueueCleanupTest, CancellationCallbackCleanupCanCancelAnotherActiveTimer)
{
    muduo::net::EventLoop loop;
    int victimFires = 0;
    int cleanupCount = 0;
    const muduo::net::TimerId victimId = loop.runAfter(0.01, [&victimFires] { ++victimFires; });
    muduo::net::TimerId cancelledId;

    // 唯一控制块由被取消 Timer 的闭包持有；释放闭包时重入取消自己与另一只活动 Timer。
    auto cleanup = std::shared_ptr<void>(nullptr, [&](void*) {
        ++cleanupCount;
        loop.cancel(cancelledId);
        loop.cancel(victimId);
    });
    cancelledId = loop.runAfter(1.0, [cleanup = std::move(cleanup)] {
        (void)cleanup;
    });

    loop.cancel(cancelledId);
    loop.runAfter(0.02, [&loop] { loop.quit(); });
    loop.loop();

    EXPECT_EQ(1, cleanupCount);
    EXPECT_EQ(0, victimFires);
}

TEST_F(TimerQueueCleanupTest, EventLoopDestructionCanCancelOwnAndAnotherTimer)
{
    int cleanupCount = 0;
    int victimCleanupCount = 0;
    muduo::net::TimerId cancelledId;
    {
        muduo::net::EventLoop loop;
        auto victimCleanup = std::shared_ptr<void>(nullptr, [&](void*) {
            ++victimCleanupCount;
        });
        const muduo::net::TimerId victimId = loop.runAfter(120.0, [cleanup = std::move(victimCleanup)] {
            (void)cleanup;
        });
        auto cleanup = std::shared_ptr<void>(nullptr, [&loop, &cancelledId, victimId, &cleanupCount](void*) {
            ++cleanupCount;
            // loop 仍在析构栈上，不依赖已清空的线程局部 EventLoop 句柄。
            loop.cancel(cancelledId);
            loop.cancel(victimId);
        });
        cancelledId = loop.runAfter(60.0, [cleanup = std::move(cleanup)] {
            (void)cleanup;
        });
        // 不运行 loop；只由 TimerQueue 析构清理尚未到期的回调。
    }

    EXPECT_EQ(1, cleanupCount);
    EXPECT_EQ(1, victimCleanupCount);
}

TEST_F(TimerQueueCleanupTest, ExpiredCallbackCleanupCanCancelRepeatingTimer)
{
    muduo::net::EventLoop loop;
    int fires = 0;
    int firesAfterDestruction = 0;
    int ownerDestructions = 0;
    muduo::net::TimerId repeatingId;
    // 小于 1us 的正间隔保留重复属性，首次到期增量截为 0，保证与过去时刻的一次性 Timer 同批。
    constexpr double kImmediateRepeatIntervalSeconds = 0.0000001;
    repeatingId = loop.runEvery(kImmediateRepeatIntervalSeconds, [&]() {
        ++fires;
        if (fires == 1) {
            // 等本批 reset 完成再安排下一批退出；错误重入队的 B 必须先于退出 Timer 被执行。
            loop.queueInLoop([&loop] {
                loop.runAfter(0.0, [&loop] { loop.quit(); });
            });
        }
        if (ownerDestructions != 0) {
            ++firesAfterDestruction;
            loop.cancel(repeatingId);
        }
    });

    struct Owner
    {
        Owner(muduo::net::EventLoop& eventLoop, muduo::net::TimerId timerId, int& destructionCount)
            : loop(eventLoop), timer(timerId), destructions(destructionCount) {}
        ~Owner()
        {
            ++destructions;
            loop.cancel(timer);
        }
        muduo::net::EventLoop& loop;
        muduo::net::TimerId timer;
        int& destructions;
    };

    auto owner = std::make_shared<Owner>(loop, repeatingId, ownerDestructions);
    loop.runAt(muduo::addTime(muduo::Timestamp::now(), -1.0), [owner = std::move(owner)] {
        (void)owner;
    });
    loop.loop();

    // 仅观察外置计数器，不解引用已销毁宿主；检测不得因固定退出窗口提前而漏过错误重入队。
    EXPECT_EQ(1, ownerDestructions);
    EXPECT_EQ(1, fires);
    EXPECT_EQ(0, firesAfterDestruction);
}

} // namespace
