# Linux 环境配置说明

本文档说明如何在 Linux 环境下配置和构建 Rucbase 项目。

## 环境要求

- GCC 15 或更高版本（支持 C++17）
- CMake 3.21 或更高版本
- Ninja 构建系统
- Bison 3.8 或更高版本
- Flex 2.6 或更高版本
- Readline 开发库

## 依赖安装

### Ubuntu/Debian 系统

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build bison flex libreadline-dev git
```

### 验证安装

```bash
gcc --version
cmake --version
ninja --version
bison --version
flex --version
```

## 构建步骤

### 1. 克隆项目并拉取子模块

```bash
cd /path/to/rucbase-lab2
git submodule update --init --recursive
```

### 2. 配置项目

使用 CMake Preset 进行配置（已配置好 Linux 环境）：

```bash
cmake --preset debug
```

或者使用 Release 模式：

```bash
cmake --preset release
```

### 3. 编译项目

```bash
# 编译所有目标
cmake --build build/debug -j
```

#### 只构建单个测试或目标

如果只需要构建某个特定的测试，可以使用 `--target` 参数：

```bash
# 只构建磁盘管理器测试
cmake --build build/debug --target disk_manager_test

# 只构建 LRU 替换策略测试
cmake --build build/debug --target lru_replacer_test

# 只构建缓冲池管理器测试
cmake --build build/debug --target buffer_pool_manager_test

# 只构建记录管理器测试
cmake --build build/debug --target record_manager_test

# 只构建 B+ 树插入测试
cmake --build build/debug --target b_plus_tree_insert_test

# 只构建数据库主程序
cmake --build build/debug --target rmdb
```

**可用的目标列表**：
- `disk_manager_test`
- `lru_replacer_test`
- `buffer_pool_manager_test`
- `record_manager_test`
- `b_plus_tree_insert_test`
- `b_plus_tree_delete_test`
- `b_plus_tree_concurrent_test`
- `query_test`
- `transaction_test`
- `regress_test`
- `concurrency_test`
- `test_parser`
- `rmdb`

### 4. 运行测试

```bash
# 运行所有测试
cd build/debug
ctest

# 或运行单个测试
./bin/disk_manager_test
./bin/lru_replacer_test
./bin/buffer_pool_manager_test
./bin/record_manager_test
```

## CMake Presets 说明

项目提供了以下构建预设：

| Preset 名称 | 说明 | 编译器 |
|------------|------|--------|
| `debug` | Linux 调试模式 | GCC |
| `release` | Linux 发布模式 | GCC |
| `debug-mac` | macOS 调试模式 | Clang |
| `release-mac` | macOS 发布模式 | Clang |

## 构建产物

构建完成后，可执行文件位于：
- `build/debug/bin/` - 调试版本
- `build/release/bin/` - 发布版本

主要可执行文件：
- `rmdb` - 数据库主程序
- `lru_replacer_test` - LRU 替换策略测试
- `disk_manager_test` - 磁盘管理器测试
- `buffer_pool_manager_test` - 缓冲池管理器测试
- `record_manager_test` - 记录管理器测试
- 以及其他测试程序...

## 常见问题

### 1. 找不到 `readline` 库

确保已安装 `libreadline-dev` 包：

```bash
sudo apt install libreadline-dev
```

### 2. Bison/Flex 版本过低

检查版本是否满足要求：

```bash
bison --version  # 需要 >= 3.8
flex --version   # 需要 >= 2.6
```

如果系统版本过低，可以从源码编译安装。

### 3. GCC 版本问题

如果系统默认 GCC 版本过低，可以安装新版本并指定使用：

```bash
sudo apt install gcc-15 g++-15
export CC=gcc-15
export CXX=g++-15
cmake --preset debug
```

### 4. 清理构建

如需完全重新构建：

```bash
rm -rf build/debug
cmake --preset debug
cmake --build build/debug -j
```

## 开发提示

- 使用 `compile_commands.json` 为 IDE 提供代码补全（已自动生成在 `build/debug/` 目录）
- 可以使用 `ccmake build/debug` 或 `cmake-gui` 进行图形化配置
- 多线程编译使用 `-j` 参数，后跟线程数（如 `-j4`）

## 目录结构

```
rucbase-lab2/
├── src/              # 源代码
├── deps/             # 第三方依赖（googletest）
├── build/            # 构建目录
│   ├── debug/        # 调试构建
│   └── release/      # 发布构建
├── docs/             # 文档
│   ├── linux/        # Linux 配置文档
│   └── mac/          # macOS 配置文档
└── CMakeLists.txt    # CMake 构建脚本
```

## 参考资料

- [Rucbase 使用文档](../Rucbase使用文档.md)
- [Rucbase 环境配置文档](../Rucbase环境配置文档.md)
- [macOS 配置文档](../mac/00-mac-环境搭建与构建配置.md)
