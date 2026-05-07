# 00 · macOS (Apple Silicon) 环境搭建与构建配置

> 适用机型：MacBook (Apple Silicon, 例如 M3) · macOS 14+
> 工具链：Apple Clang 21 + CMake 4.x + Ninja 1.13 + clangd 21
> 状态：已在本机一次性 configure + build 通过 37/37 个 target

本文档记录将上游为 Ubuntu/GCC 设计的 RUCBase 工程一次性迁移到 macOS / Apple Silicon
原生工具链上的全部改动，便于后续回溯与他人复用。

---

## 1. 环境前置依赖

| 工具 | 来源 | 备注 |
|------|------|------|
| Xcode Command Line Tools | `xcode-select --install` | 提供 Apple Clang / libc++ / lldb |
| CMake ≥ 3.21 | brew | 工程要求 ≥ 3.21（preset v3） |
| Ninja | brew | 构建生成器 |
| clangd | 来自 CLT 或 brew llvm | IDE 索引 |
| **bison** (≥ 3.x) | `brew install bison` | macOS 自带是 2.3，必须用 brew 版 |
| **flex** | `brew install flex` | macOS 自带 flex 老旧 |
| **readline** | `brew install readline` | mac 系统 readline 是 BSD libedit 兼容层，不可用 |

bison / flex / readline 都是 **keg-only**，brew 不会软链到 `/opt/homebrew`，
本工程通过 `CMakePresets.json` 显式注入路径，**用户无需手动改 PATH/LDFLAGS**。

```bash
xcode-select --install
brew install bison flex readline
```

验证：
```bash
/opt/homebrew/opt/bison/bin/bison --version  # 应为 3.8+
/opt/homebrew/opt/flex/bin/flex --version    # 应为 2.6+
```

---

## 2. 子模块拉取

工程依赖 GoogleTest（位于 `deps/googletest`），第一次拿到代码必须执行：

```bash
git submodule update --init --recursive
```

> 注意：**不要**按官方 README 把 googletest 系统级 `make install`。
> 本工程通过 `add_subdirectory(deps)` 把 gtest 内嵌进构建，避免污染系统目录。

---

## 3. 一次性构建命令

```bash
cmake --preset debug                  # configure
cmake --build build/debug -j          # 全量编译
./build/debug/bin/lru_replacer_test   # 运行任一单元测试
./build/debug/bin/rmdb testdb         # 启动服务端
```

构建产物位于：
- 可执行：`build/debug/bin/`
- 静态库：`build/debug/lib/`
- compile_commands.json：根目录已建软链 → `build/debug/compile_commands.json`

---

## 4. 改动清单（含原因）

### 4.1 `CMakePresets.json`（重写）

- 抽出 `base-mac` hidden preset，集中管理 mac/brew 路径，`debug`/`release`
  通过 `inherits` 复用，避免重复。
- `binaryDir` 由 `${sourceDir}/build/${presetName}` 决定，与 `.gitignore`
  里的 `/build/` 对齐。
- 关键 cache 变量：
  - `CMAKE_OSX_ARCHITECTURES=arm64` —— 强制原生 arm64，避免 Rosetta 路径混入。
  - `CMAKE_POLICY_VERSION_MINIMUM=3.10` —— CMake 4.x 不再兼容
    `cmake_minimum_required(VERSION 3.5)`，而 `deps/googletest`
    内部仍写着 3.5。本变量让父项目在不修改子模块源码的前提下兜底兼容。
  - `BISON_EXECUTABLE` / `FLEX_EXECUTABLE` / `FLEX_INCLUDE_DIR` —— 直接指向
    brew 的 keg-only 路径，`find_package(BISON|FLEX)` 命中 brew 版而非 `/usr/bin/` 老版本。
  - `CMAKE_PREFIX_PATH` —— 让 `find_library(readline)` 等命中 brew。

### 4.2 顶层 `CMakeLists.txt`（重写）

- `cmake_minimum_required(VERSION 3.21)`：和 preset v3 对齐。
- 移除原先硬编码的 `-O0 -g -ggdb3` 全局 flag，改为按 `BUILD_TYPE` 决定优化级别；
  默认 `Debug`，便于实验调试。`-ggdb3` 在 lldb 上无意义，去除。
- 新增 `find_library(READLINE_LIBRARY ...)` 与 `find_path(READLINE_INCLUDE_DIR ...)`，
  下游 target 通过 `${READLINE_LIBRARY}` 链接，告别 `target_link_libraries(... readline)`
  这种依赖系统默认搜索路径的写法。
- **关键顺序调整**：把 `add_subdirectory(deps)` 提到 `add_subdirectory(src)` 之前。
  因为 `src/test` 引用了 `gtest_main` target，必须先注册。

### 4.3 `src/CMakeLists.txt`

- `rmdb` 改为链接 `${READLINE_LIBRARY}`。
- 在 `APPLE` 平台下给 `rmdb` 加 `-include cstdio`：brew 的 readline 头文件
  使用了 `FILE` 类型却不自包含 `<stdio.h>`，预包含一行可避免修改业务源码
  （`src/rmdb.cpp` 保持原状，方便上游同步）。

### 4.4 `src/parser/CMakeLists.txt`

- `add_library(parser ...)` 后追加 `target_include_directories(parser PUBLIC ${FLEX_INCLUDE_DIRS})`。
  `FlexLexer.h` 在 brew flex 的 keg-only include 目录里，必须显式加进去。

### 4.5 `src/test/CMakeLists.txt`

- `add_compile_options(-include cstdio)`（仅 mac），让 `transaction_test` /
  `query_test` 等用到 readline 头的 test 能正常编译。
- `query_test` / `transaction_test` 显式链接 `${READLINE_LIBRARY}`。

### 4.6 `rucbase_client/CMakeLists.txt`

- 升 `cmake_minimum_required` 至 3.21。
- 用 `find_library(READLINE_LIBRARY)` 代替直接 `readline` 字面量。
- 改用 `Threads::Threads` 替代裸 `pthread`，跨平台更稳。

### 4.7 `.vscode/settings.json`

- 加 `"cmake.useCMakePresets": "always"`，让 cmake-tools 走 preset 流程。
- 已存在的 `cmake.copyCompileCommands` 会在每次 configure 后把
  `compile_commands.json` 复制到工程根，clangd 即可零配置工作。

### 4.8 `.gitignore`

- 新增 `/compile_commands.json` 与 `.cache/`（clangd 后台索引产物）。

---

## 5. 验证结果

```text
$ cmake --preset debug
-- Found readline: /opt/homebrew/opt/readline/lib/libreadline.dylib
-- Found BISON:   /opt/homebrew/opt/bison/bin/bison (3.8.2)
-- Found FLEX:    /opt/homebrew/opt/flex/bin/flex  (2.6.4)
-- Configuring done
-- Generating done

$ cmake --build build/debug -j
[37/37] Linking CXX executable bin/b_plus_tree_concurrent_test
```

37 个 target 全部编译通过。运行 `./build/debug/bin/lru_replacer_test`
返回 FAILED 是 **预期行为**：Lab1 待实现的 `lru_replacer.cpp` 仍是
`// Todo:` 占位，本就该不通过测试 —— 这也证明从 configure 到 link
到 runtime 的整条链路已打通。

---

## 6. 常见问题速查

| 现象 | 原因 | 解决 |
|------|------|------|
| `Command "bison --version" failed` | 只装了 brew 元数据未真正安装 | `brew install bison` |
| `unknown type name 'FILE'` 来自 readline.h | brew readline 头不自包含 stdio | 已通过 `-include cstdio` 自动处理 |
| `cmake_minimum_required(VERSION 3.5)` 报错 | CMake 4 删除 ≤3.5 兼容 | preset 中已设 `CMAKE_POLICY_VERSION_MINIMUM=3.10` |
| clangd 提示找不到 `FlexLexer.h` | clangd 没拿到 flex include 路径 | 根目录 `compile_commands.json` 软链已生成；如失效执行 `ln -sf build/debug/compile_commands.json compile_commands.json` |
| 链接时 `ld: warning: ignoring duplicate libraries` | `target_link_libraries` 中重复列了静态库 | 上游遗留问题，仅警告不影响产物，可忽略 |

---

## 7. 后续动作建议

1. 开始做 Lab1：编辑 `src/replacer/lru_replacer.cpp` 与 `src/storage/buffer_pool_manager.cpp`，
   每改一次跑 `cmake --build build/debug --target lru_replacer_test && ./build/debug/bin/lru_replacer_test`。
2. 切 Release 时直接 `cmake --preset release && cmake --build build/release -j`，
   不影响 debug 产物。
3. 上游若推送了对 `rmdb.cpp` 等业务代码的更新，本工程的所有改动都集中在
   构建脚本与 preset，可直接 `git pull` 不冲突。
