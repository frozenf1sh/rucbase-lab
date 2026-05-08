/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lru_replacer.h"

/*
 * LRUReplacer 维护"可被淘汰的 frame"集合（即所有 unpin 状态的 frame），
 * 以"最近最少使用 (Least Recently Used)"为淘汰策略。
 *
 * 核心数据结构：
 *   - LRUlist_ : std::list<frame_id_t>
 *       头部 = 最新被 unpin 的 frame（"最近被使用"，最不应淘汰）
 *       尾部 = 最久未被 unpin 的 frame（"最旧"，最应淘汰）
 *   - LRUhash_ : frame_id -> 该 frame 在 LRUlist_ 中的迭代器
 *       让 pin / 重复 unpin 等"按 frame_id 查找"的操作可以 O(1) 完成。
 *
 * 之所以用 list+hash 这套经典组合，是为了同时拿到：
 *   1. list 头尾插入/删除 O(1) —— 满足"最新放头"、"最旧从尾出"；
 *   2. hash 按 key 查 O(1) —— 满足 pin(frame_id) 的快速删除；
 *   3. list 节点地址在 splice/erase 之外保持稳定，可作为 hash 的 value。
 *
 * 并发：每个 public 方法都通过 std::scoped_lock 把整个临界区包住，
 *       保证 victim/pin/unpin 三者两两互斥。
 */

LRUReplacer::LRUReplacer(size_t num_pages) { max_size_ = num_pages; }

LRUReplacer::~LRUReplacer() = default;

/**
 * @description: 使用LRU策略删除一个victim frame，并返回该frame的id
 * @param {frame_id_t*} frame_id 被移除的frame的id，如果没有frame被移除返回nullptr
 * @return {bool} 如果成功淘汰了一个页面则返回true，否则返回false
 */
bool LRUReplacer::victim(frame_id_t *frame_id) {
    std::scoped_lock lock{latch_};

    // 没有任何"可淘汰"的 frame 时直接失败，由调用方决定如何处理
    // （BufferPoolManager::find_victim_page 会因此返回 false → new_page/fetch_page 返回 nullptr）。
    if (LRUlist_.empty()) {
        return false;
    }

    // 取链表尾部 —— "最早被 unpin 且至今没再被使用过"的 frame，即标准的 LRU victim。
    frame_id_t victim_id = LRUlist_.back();
    LRUlist_.pop_back();
    LRUhash_.erase(victim_id);

    *frame_id = victim_id;
    return true;
}

/**
 * @description: 固定指定的frame，即该页面无法被淘汰
 * @param {frame_id_t} 需要固定的frame的id
 */
void LRUReplacer::pin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};

    // 语义：把 frame 从"可淘汰集合"里移除。
    // 如果它本来就不在集合中（说明上层早已 pin 过 / 还从未 unpin 过），则什么都不做。
    auto it = LRUhash_.find(frame_id);
    if (it == LRUhash_.end()) {
        return;
    }
    // 通过 hash 直接拿到 list 中的迭代器，做 O(1) erase。
    LRUlist_.erase(it->second);
    LRUhash_.erase(it);
}

/**
 * @description: 取消固定一个frame，代表该页面可以被淘汰
 * @param {frame_id_t} frame_id 取消固定的frame的id
 */
void LRUReplacer::unpin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};

    // 关键：unpin 必须是幂等的 —— 如果 frame 已经在 LRU 集合里（即已经处于"可淘汰"状态），
    // 不要把它移到首部，也不要重复插入。这样符合"unpin 只在 pin_count 由 1 减到 0 的瞬间被
    // BufferPoolManager 调用一次"的语义；同时也通过了 lru_replacer_test SimpleTest 中
    // "连续两次 unpin(1) 后 Size 仍为 6 / victim 顺序保持 1,2,3" 的断言。
    if (LRUhash_.count(frame_id) != 0) {
        return;
    }

    // 防御：理论上 BufferPoolManager 会保证 |LRUlist| ≤ pool_size，但万一上层逻辑出错
    // 也不让 replacer 内部数据膨胀；超过容量时直接拒绝。
    if (LRUlist_.size() >= max_size_) {
        return;
    }

    // 插入到头部，表示"最新被 unpin"——最不应当被淘汰。
    LRUlist_.push_front(frame_id);
    LRUhash_[frame_id] = LRUlist_.begin();
}

/**
 * @description: 获取当前replacer中可以被淘汰的页面数量
 */
size_t LRUReplacer::Size() {
    std::scoped_lock lock{latch_};
    return LRUlist_.size();
}
