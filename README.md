# muduo windows old version from chensuo's muduo

Windows 平台实现集中在兼容层，公共调用保留 Muduo 原有契约：

- `Condition` 构造时绑定互斥锁，使用 `wait()` 或 `waitForSeconds(double)`；后者保留小数秒，仅实际超时返回 `true`。
- `Thread::start()` 返回前完成线程 ID 发布；`join()` 成功返回 `0`，之后 `started()` 仍为 `true`；未显式 join 的线程在析构时 detach。
- `Timestamp` 保留原版六种比较运算。MSVC 的 `empty_bases` 属性仅保持多重空基类优化，值的大小与对齐仍与 `int64_t` 相同。
- `EventLoop` 通过独立 `WindowsWakeup` 实现真实跨线程唤醒。定时器按单调时钟计时，`runAt` 在调用时将墙钟时刻换算一次。
- 定时器的闭包清理也允许重入取消：先从两个索引摘除再销毁，整批到期回调清理完毕后才退出取消窗口。没有调用者的旧唤醒、HUP 返回和 poller 选择兼容入口已移除。
- `TcpClient::setConnectFailedCallback` 在连接尝试失败时通知调用者；回调可调用 `stop()` 阻止内部重试，未设置时保留原行为。

修改过的 `Thread`、`Condition` 布局与旧二进制不兼容，必须一起重编库和所有使用者。

CMake 默认保留 protobuf 模块和测试。只构建基础库、网络库及接口回归时，可以设置 `MUDUO_BUILD_PROTOBUF=OFF`、`MUDUO_BUILD_TESTS=ON`、`CMAKE_BUILD_NO_EXAMPLES=ON`。
`FETCHCONTENT_SOURCE_DIR_BOOST`、`FETCHCONTENT_SOURCE_DIR_GOOGLETEST` 和 `FETCHCONTENT_SOURCE_DIR_ZLIB` 可指定已有依赖源码；离线构建另设 `FETCHCONTENT_FULLY_DISCONNECTED=ON`。

新增测试目标为 `muduo_base_contract_test` 和 `muduo_net_contract_test`，通过 CTest 运行。Windows 的 MSBuild 构建使用 `/m:1`；旧 vcxproj 保留原工具集与依赖路径配置，切换已安装的工具集应由调用者显式指定。

当前两组契约回归共 22 条：7 条基础契约与 15 条网络契约，包含 Timestamp 的完整比较、值布局和三种闭包清理重入取消场景。Linux 使用方的固定上游与 overlay 另行构建验证，不将 Windows 库的验证结果替代 Linux 构建结果。

Windows 源码统一使用 UTF-8；CMake 显式设置 `/utf-8`，旧 vcxproj 通过库根 `Directory.Build.targets` 继承同一编码选项。
