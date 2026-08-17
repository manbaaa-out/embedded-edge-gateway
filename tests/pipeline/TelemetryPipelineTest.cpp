// TelemetryPipeline 的异步队列契约测试：生产线程提交 Reading 批次，单一写线程负责
// SQLite 事务和数据库切换。验证析构排空、空批次、机会合并及切换命令的严格队列顺序。

#include "gateway/pipeline/TelemetryPipeline.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace gateway;

namespace {

// 由用例标签 tag 生成独立数据库路径，防止并行 CTest 共享 WAL 文件。
std::string dbPath(const std::string& tag) {
    return "/tmp/gateway_pipeline_test_" + tag + ".db";
}

// 数据库文件 RAII。path 是主文件；构造和析构都调用 remove()，后者同时删除 SQLite
// WAL/SHM 伴随文件，保证上一次异常退出不会污染本次计数。
struct DbFile {
    std::string path; // 当前用例数据库主文件的绝对路径。
    explicit DbFile(const std::string& tag) : path(dbPath(tag)) { remove(); }
    ~DbFile() { remove(); }
    void remove() const {
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
        std::remove((path + "-shm").c_str());
    }
};

// 将 {设备名, 数值} 列表转换为 submit 所需的 Reading vector；items 中字符串在此复制。
std::vector<Reading> readings(std::initializer_list<std::pair<const char*, double>> items) {
    std::vector<Reading> out; // 保持 items 原有顺序的结果容器。
    for (const auto& it : items)
        out.push_back(Reading{it.first, it.second});
    return out;
}

// 以只读 Database 打开 path 并返回总行数；只在 pipeline 析构并 join 写线程后调用。
long countRows(const std::string& path) {
    Database ro(path, /*readonly=*/true); // 不改变 WAL 或测试数据的观察连接。
    return ro.count();
}

} // namespace

// submit 返回后数据可能仍在队列中；pipeline 析构必须排空队列后才结束写线程。
TEST(TelemetryPipeline, SubmitPersistsReadingsAfterShutdown) {
    DbFile f("persist"); // 本用例独占数据库及其伴随文件。
    {
        TelemetryPipeline p(std::make_shared<Database>(f.path)); // 作用域结束触发排空和 join。
        EXPECT_TRUE(p.submit(readings({{"temperature", 25.3}, {"humidity", 61.0}}), 1000));
    } // 关闭队列，写完剩余记录，再 join 写线程。

    EXPECT_EQ(countRows(f.path), 2) << "在飞记录不得因停机而丢失";

    Database ro(f.path, /*readonly=*/true);  // 检查具体内容的只读连接。
    auto rows = ro.query("temperature", 10); // 预期唯一温度记录。
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0].value, 25.3);
    EXPECT_EQ(rows[0].ts, 1000);
}

// 空读数批次可被接受，但不产生数据库记录。
TEST(TelemetryPipeline, EmptyReadingsAreAcceptedAndWriteNothing) {
    DbFile f("empty"); // 独立数据库用于区分空批次和随后正常批次。
    {
        TelemetryPipeline p(std::make_shared<Database>(f.path)); // 被测异步写线程。
        EXPECT_TRUE(p.submit({}, 1000));
        EXPECT_TRUE(p.submit(readings({{"illuminance", 300.0}}), 1001));
    }
    EXPECT_EQ(countRows(f.path), 1);
}

// 连续投递促使写线程合并事务，但每条读数仍必须持久化一次。
TEST(TelemetryPipeline, OpportunisticBatchingPreservesEveryRow) {
    DbFile f("batch");            // 批处理压力用数据库。
    constexpr int kBatches = 500; // 连续提交批次数，每批固定两条读数。
    {
        TelemetryPipeline p(std::make_shared<Database>(f.path)); // 快速生产使消费者有机会合并事务。
        for (int i = 0; i < kBatches; ++i) {
            ASSERT_TRUE(
                p.submit(readings({{"temperature", double(i)}, {"humidity", 50.0}}), 2000 + i))
                << "第 " << i << " 批被丢弃,队列容量不足以承载本用例";
        }
    }
    EXPECT_EQ(countRows(f.path), kBatches * 2);
}

// 数据库切换命令与数据批次共用队列，切换前后的记录应落入各自数据库。
TEST(TelemetryPipeline, SwapDatabaseSplitsRowsAtTheSwapBoundary) {
    DbFile before("swap_old"); // swapDatabase 命令之前的目标库。
    DbFile after("swap_new");  // swapDatabase 命令之后的目标库。
    {
        TelemetryPipeline p(std::make_shared<Database>(before.path)); // 初始写向旧库。
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

    Database ro_new(after.path, /*readonly=*/true); // 验证旧类型未跨越切换边界。
    EXPECT_TRUE(ro_new.query("temperature", 10).empty()) << "旧库的记录不得漏进新库";
}
