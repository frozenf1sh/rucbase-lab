# CLAUDE.md - Rucbase 项目协作规范

本文档定义了使用 Claude Code 进行 Rucbase 数据库项目开发时必须遵循的任务完成约束。所有协作行为、代码修改、测试和提交必须遵守本规范。

## 一、代码风格与注释规范

### 1.1 代码风格

- 严格遵循项目现有 `.clang-format` 配置（Google 风格，4 空格缩进）
- 保持与参考实现（`disk_manager.cpp`、`lru_replacer.cpp`）一致的代码组织方式
- 大括号在同一行（K&R 风格）：
  ```cpp
  if (condition) {
      do_something();
  }
  ```
- 指针与引用符号紧贴类型：`char*`、`int&`
- 成员变量以下划线结尾：`LRUlist_`、`LRUhash_`、`latch_`

### 1.2 注释规范

所有新增/修改的代码必须包含以下两种注释：

#### (1) 功能块头注释（英文）

在每个函数实现前以多行注释形式说明：
- 核心数据结构与不变量（如适用）
- 关键设计决策与权衡（如适用）

示例：
```cpp
/*
 * LRUReplacer 维护"可被淘汰的 frame"集合（即所有 unpin 状态的 frame），
 * 以"最近最少使用 (Least Recently Used)"为淘汰策略。
 *
 * 核心数据结构：
 *   - LRUlist_ : std::list<frame_id_t>
 *       头部 = 最新被 unpin 的 frame（"最近被使用"，最不应淘汰）
 *       尾部 = 最久未被 unpin 的 frame（"最旧"，最应淘汰）
 *   - LRUhash_ : frame_id -> 该 frame 在 LRUlist_ 中的迭代器
 *
 * 并发：每个 public 方法都通过 std::scoped_lock 把整个临界区包住，
 *       保证 victim/pin/unpin 三者两两互斥。
 */
```

#### (2) 行内说明注释（中文）

在关键逻辑、微妙边界条件、设计决策处添加中文注释：
- 说明"为什么"而非"是什么"
- 解释非显而易见的边界条件
- 记录与测试用例相关的约束

示例：
```cpp
// 关键：unpin 必须是幂等的 —— 如果 frame 已经在 LRU 集合里
// 不要把它移到首部，也不要重复插入。这样符合"unpin 只在 pin_count
// 由 1 减到 0 的瞬间被 BufferPoolManager 调用一次"的语义。
if (LRUhash_.count(frame_id) != 0) {
    return;
}
```

### 1.3 Doxygen 文档

- 保持已有函数的 Doxygen 注释（`@description`、`@param`、`@return`）不变
- 新增的辅助函数也应添加相应的 Doxygen 注释

## 二、任务完成的严格约束

### 2.1 公有接口不可修改

**严格禁止**修改任何公有函数的声明：
- 函数签名（参数类型、返回类型）必须保持原样
- 公有成员函数的访问级别不可改变
- 不可增删公有成员函数

若需要辅助功能，可添加私有/保护成员函数。

### 2.2 实现与测试的闭环

每个任务的完成必须按以下顺序进行：

1. **理解任务文档**：完整阅读 `docs/Rucbase-LabX[...].md` 中对应任务描述
2. **查看测试代码**：先阅读 `src/test/` 目录下对应的测试文件，明确测试点
3. **实现功能**：根据任务文档和测试用例实现代码
4. **本地验证**：必须先在本地编译通过，且相关测试全部通过
5. **提交**：确认无误后再提交代码

### 2.3 编译约束

- **必须使用 CMake Preset**：`cmake --preset debug`（Linux）或 `cmake --preset debug-mac`（macOS）
- **必须零警告编译通过**：所有警告必须修复，不可忽略
- **逐步编译**：优先实现独立模块，每完成一个小功能立即编译验证
- **单次构建**：完成任务后必须执行完整构建 `cmake --build build/debug -j` 确认无问题

### 2.4 测试约束

- **单测优先**：修改代码前，先确认能成功编译和运行对应的测试
- **单个测试构建**：使用 `cmake --build build/debug --target <test_name>` 仅构建所需测试
- **测试必须全部通过**：任务完成时，对应测试用例必须 100% 通过
- **不修改测试代码**：除非测试代码本身有 bug，否则禁止修改测试文件

### 2.5 并发控制约束

- 涉及并发的类（`LRUReplacer`、`BufferPoolManager`、`IxIndexHandle`）的公有方法必须保证线程安全
- 优先使用 `std::scoped_lock`（C++17）而非手动 `lock/unlock`
- 锁粒度要合理：既保证正确性，也不过度影响并发度

### 2.6 错误处理约束

- 使用项目定义的异常类：`FileExistsError`、`FileNotFoundError`、`FileNotClosedError`、`FileNotOpenError`、`UnixError`、`InternalError`
- 系统调用错误需检查返回值并抛出相应异常
- 不可用 `assert` 代替错误处理（assert 仅用于调试期检查内部不变量）

## 三、常用工作流程

### 3.1 任务开发流程

```bash
# 1. 更新子模块（首次或拉取后）
git submodule update --init --recursive

# 2. 配置项目
cmake --preset debug

# 3. 查看有哪些测试目标
ls build/debug/bin/

# 4. 仅构建和运行某一个测试（推荐）
cmake --build build/debug --target disk_manager_test
./build/debug/bin/disk_manager_test

# 5. 完成任务后完整构建
cmake --build build/debug -j
```

### 3.2 调试流程

1. 先确保代码能正常编译
2. 运行测试定位失败的测试用例
3. 用 `printf` 或调试器（gdb/lldb）定位问题
4. 修复问题后重新运行测试确认
5. 确保不引入新的警告

## 四、文件修改范围

### 4.1 允许修改的文件

| 任务 | 可修改文件 |
|------|-----------|
| 任务 1.1 磁盘存储管理器 | `src/storage/disk_manager.cpp` |
| 任务 1.2 缓冲池替换策略 | `src/replacer/lru_replacer.cpp` |
| 任务 1.3 缓冲池管理器 | `src/storage/buffer_pool_manager.cpp` |
| 任务 2 记录管理器 | `src/record/rm_file_handle.cpp` |
| 任务 3-5 索引管理器 | `src/index/ix_index_handle.cpp` |

### 4.2 允许新增的文件

- 可在相应目录下添加私有辅助函数的实现（通常不需要，直接在 .cpp 中实现即可）

### 4.3 禁止修改的文件

- 所有 `.h` 头文件（除非文档明确要求）
- 所有测试文件 `src/test/**`
- 第三方依赖 `deps/`
- 构建配置文件 `CMakeLists.txt`、`CMakePresets.json`（除非是跨平台适配）

## 五、任务完成检查清单

在声称任务完成前，必须确认以下所有项：

- [ ] 代码已按任务文档要求完整实现
- [ ] 代码风格与项目一致，注释清晰
- [ ] 零警告编译通过
- [ ] 对应测试用例 100% 通过
- [ ] 没有修改不应该修改的文件
- [ ] Git 工作区干净（必要的修改已暂存/提交）

## 六、常见任务操作速查

### 6.1 构建单个测试

| 测试目标 | 命令 |
|---------|------|
| 磁盘管理器 | `cmake --build build/debug --target disk_manager_test` |
| LRU 替换器 | `cmake --build build/debug --target lru_replacer_test` |
| 缓冲池管理器 | `cmake --build build/debug --target buffer_pool_manager_test` |
| 记录管理器 | `cmake --build build/debug --target record_manager_test` |
| B+ 树插入 | `cmake --build build/debug --target b_plus_tree_insert_test` |
| B+ 树删除 | `cmake --build build/debug --target b_plus_tree_delete_test` |
| B+ 树并发 | `cmake --build build/debug --target b_plus_tree_concurrent_test` |

### 6.2 清理构建

```bash
# 完全清理
rm -rf build/debug

# 重新配置和构建
cmake --preset debug
cmake --build build/debug -j
```

### 6.3 查看所有可构建目标

```bash
cd build/debug
ninja -t targets | grep test
# 或
cmake --build build/debug --target help
```

---

**重要**：本规范可能根据项目进展更新，请定期查阅最新版本。
