// 遥测落库写线程的测试。
//
// 关注点是本类独有的三条契约,而非 SQLite 本身:
//   1. 投递的记录最终一定落库,且停机时在飞的记录不丢;
//   2. 攒批不改变落库结果 —— 无论写线程把多少批合进一个事务,总量与内容不变;
//   3. 换库指令与落库严格有序 —— 换库之前投的记录进旧库,之后投的进新库。
// 第 3 条是这套设计相对「跨线程改指针」的核心优势,必须被测到。
//
// 每个用例用独立库文件:gtest_discover_tests 会把用例注册成独立 ctest 条目,
// 并行执行时共用文件会互相干扰。

#include "gateway/pipeline/TelemetryPipeline.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace gateway;

namespace {

std::string dbPath(const std::string& tag) {
    return "/tmp/gateway_pipeline_test_" + tag + ".db";
}

// SQLite 在 WAL 模式下会另外生成 -wal / -shm,一并删掉才算干净
struct DbFile {
    std::string path;
    explicit DbFile(const std::string& tag) : path(dbPath(tag)) { remove(); }
    ~DbFile() { remove(); }
    void remove() const {
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
        std::remove((path + "-shm").c_str());
    }
};

std::vector<Reading> readings(std::initializer_list<std::pair<const char*, double>> items) {
    std::vector<Reading> out;
    for (const auto& it : items) out.push_back(Reading{it.first, it.second});
    return out;
}

// 写线程已 join(pipeline 析构完成)之后才调用,故读到的就是最终状态
long countRows(const std::string& path) {
    Database ro(path, /*readonly=*/true);
    return ro.count();
}

}  // namespace

TEST(TelemetryPipeline, SubmitPersistsReadingsAfterShutdown) {
    DbFile f("persist");
    {
        TelemetryPipeline p(std::make_shared<Database>(f.path));
        EXPECT_TRUE(p.submit(readings({{"temperature", 25.3}, {"humidity", 61.0}}), 1000));
    }   // 析构:关队列 → 写线程落完剩余记录 → join

    EXPECT_EQ(countRows(f.path), 2) << "在飞记录不得因停机而丢失";

    Database ro(f.path, /*readonly=*/true);
    auto     rows = ro.query("temperature", 10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0].value, 25.3);
    EXPECT_EQ(rows[0].ts, 1000);
}

// 心跳帧解码出空集,不是失败,也不应产生空事务
TEST(TelemetryPipeline, EmptyReadingsAreAcceptedAndWriteNothing) {
    DbFile f("empty");
    {
        TelemetryPipeline p(std::make_shared<Database>(f.path));
        EXPECT_TRUE(p.submit({}, 1000));
        EXPECT_TRUE(p.submit(readings({{"illuminance", 300.0}}), 1001));
    }
    EXPECT_EQ(countRows(f.path), 1);
}

// 攒批是性能优化,不得改变可观测结果。连续快速投递会让写线程把多批合进
// 同一个事务,总量与内容仍须与逐条写完全一致。
TEST(TelemetryPipeline, OpportunisticBatchingPreservesEveryRow) {
    DbFile f("batch");
    constexpr int kBatches = 500;
    {
        TelemetryPipeline p(std::make_shared<Database>(f.path));
        for (int i = 0; i < kBatches; ++i) {
            ASSERT_TRUE(p.submit(readings({{"temperature", double(i)}, {"humidity", 50.0}}),
                                 2000 + i))
                << "第 " << i << " 批被丢弃,队列容量不足以承载本用例";
        }
    }
    EXPECT_EQ(countRows(f.path), kBatches * 2);
}

// 换库必须与落库严格有序:换库之前投的记录属于旧库,之后的属于新库。
// 这正是「走队列」相对「跨线程改 shared_ptr」的价值 —— 后者无法保证这个边界。
TEST(TelemetryPipeline, SwapDatabaseSplitsRowsAtTheSwapBoundary) {
    DbFile before("swap_old");
    DbFile after("swap_new");
    {
        TelemetryPipeline p(std::make_shared<Database>(before.path));
        for (int i = 0; i < 20; ++i) {
            ASSERT_TRUE(p.submit(readings({{"temperature", double(i)}}), 3000 + i));
        }

        p.swapDatabase(std::make_shared<Database>(after.path));

        for (int i = 0; i < 7; ++i) {
            ASSERT_TRUE(p.submit(readings({{"humidity", double(i)}}), 4000 + i));
        }
    }

    EXPECT_EQ(countRows(before.path), 20) << "换库之前的记录必须留在旧库";
    EXPECT_EQ(countRows(after.path), 7) << "换库之后的记录必须写进新库";

    Database ro_new(after.path, /*readonly=*/true);
    EXPECT_TRUE(ro_new.query("temperature", 10).empty())
        << "旧库的记录不得漏进新库";
}
