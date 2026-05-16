# 03 · Lab2 任务一：B+树索引管理器（查询模块）

> 文档目标：详解 B+ 树索引管理器中的查询过程。从单结点内的二分查找到树级别的逐层深入，剖析如何从根结点定位到包含目标 Key 的叶子结点。

本任务实现的代码全部位于：
- `src/index/ix_index_handle.cpp` — 包含结点的搜索（`lower_bound`, `upper_bound`）以及树级别搜索（`find_leaf_page`, `get_value`）

---

## 1. 关键概念：B+ 树结点的内部查找

在本系统中，B+ 树的结点的 key 数组是有序存储的，这允许我们利用二分查找加速查找效率。

### 1.1 `lower_bound`（寻找第一个 >= target 的键）

在实现上，这和标准库的 `std::lower_bound` 类似，维护一个左闭右开区间 `[0, num_key)`。
- 我们使用 `ix_compare()` 进行数据类型的泛型比较。
- 当所有的 key 都小于 target 时，循环将引导 `left` 到达 `num_key`，完美兼容边界。

这里的返回值代表了我们要存取或继续向下层访问的索引槽位(slot no)。

### 1.2 `upper_bound` （寻找第一个 > target 的键）

和 `lower_bound` 代码结构极其相似，但主要用于**内部节点**：
- B+树里每个内部节点的第 $i$ 个 key($i \ge 1$)，代表着右侧第 $i$ 棵子树的所有键的最小值。
- `upper_bound` 通过比较找出严格大于 target 的首个区间边界位置。
- 注意区间通常从 `[1, num_key)` 考察即可。

### 1.3 `internal_lookup` 的精妙之处

内部结点查找子树，是基于 `upper_bound` 实现的：
```cpp
int pos = upper_bound(key);
return value_at(pos - 1);
```
由于 `upper_bound(key)` 返回的是第一个大过 `key` 的右侧哨兵位置 `pos`，而处于该哨兵左侧相邻的子树恰好满足包含 `[key[pos-1], key[pos])`。这解释了为什么要选用 `pos - 1` 返回下一层将要 `fetch` 的 `page_id_t`。

---

## 2. 关键概念：树维度的纵向查找

### 2.1 `find_leaf_page` 的沿树滑降

在树级层面查找特定的叶子结点包含三个步骤：
1. **获取根部页面**：通过 `file_hdr_->root_page_` 和 `fetch_node`。
2. **向下递归**：检查当前 node 是否为 `is_leaf_page()`，如果不是，则利用上文构建的 `internal_lookup` 获得下一个孩子节点的编号。
3. **及时释放**：使用缓冲池管理器 `fetch_node` 出页面进行分析后，一定要将其 **`unpin_page` 并使脏位取 false（读操作不脏）**，否则很快将导致 BufferPool 耗尽并发生死锁（特别在并发环境下）。一直到进入目标叶子节点后才完成查询。此时会向上层抛出获取到带有 `pin` 存在的叶子级 `node` 以供操作。

### 2.2 `get_value` 结果提取

1. 通过调用 `find_leaf_page` 操作得到带有被 `latched/pinned` 的节点。
2. 内部用 `leaf_lookup` 寻找是否完美匹配到对应 key（也就是存在，而不是单纯的处于区间内落空了），并取出目标 Rid（记录坐标）。
3. 推入传出参数 `std::vector<Rid> *result` 之中。
4. 返回前务必 **最后一次执行 `unpin_page(false)`** 释放这个叶子节点控制权。

---

## 3. 编码规约小结

由于 B+ 的结点和 BufferPool 高度结合，所以需遵从纪律：
1. **谁 fetch，谁 unpin**：每一层查找经过的父级别节点不仅要用到完毕后第一时间立即释放，最后的叶节点用完必须最终在外围也确保被释放。`get_value` 中已经落实。由于是查询请求，参数 `is_dirty` 必须为 false。
2. **边界的严谨处理**：通过使用左闭右开区间 `[X, num_key)` 防止溢出内存页外的无效内存引发段错误。