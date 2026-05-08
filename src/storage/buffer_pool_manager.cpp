/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

/*
 * BufferPoolManager 负责"内存帧 (frame) <-> 磁盘页 (page)"之间的调度。
 *
 * 关键内部状态：
 *   pages_       : 长度为 pool_size_ 的 Page 数组，每个数组下标 i 即是 frame_id i
 *   page_table_  : PageId -> frame_id  当前内存里有哪些磁盘页
 *   free_list_   : 还没装载任何磁盘页的空闲帧编号
 *   replacer_    : 在所有"已装载 + pin_count==0"的帧里挑一个被淘汰
 *   latch_       : 整个 buffer pool 的大锁，所有 public 方法整段加锁
 *
 * 不变式（必须始终成立）：
 *   1. 每个 frame 要么在 free_list_，要么在 page_table_ 中（且对应的 Page::id_ 合法）；
 *   2. 一个 frame 在被 replacer_ 跟踪 ⇔ 它的 pin_count_ == 0；
 *   3. 当一个 frame 被 victim 出局时，若其原 Page 为 dirty 必须先写回磁盘；
 *   4. 任意时刻 page_table_.size() + free_list_.size() <= pool_size_。
 */

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id
 * @return {bool} true: 可替换帧查找成功 , false: 可替换帧查找失败
 * @param {frame_id_t*} frame_id 帧页id指针,返回成功找到的可替换帧id
 *
 * 注意：本函数假设调用方已持有 latch_，自身不再加锁——否则会与外层 public 方法
 *       重复加同一把 mutex 造成死锁。
 */
bool BufferPoolManager::find_victim_page(frame_id_t *frame_id) {
    // 优先从 free_list_ 取——这些帧从未承载过页面，无需 dirty 写回，最便宜。
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    // free_list_ 空了，意味着 pool 已满。让 replacer_ 按 LRU 策略选一个"unpinned"帧淘汰。
    // 若所有帧都被 pin 住，replacer_ 会返回 false——表示当前没有可用帧。
    return replacer_->victim(frame_id);
}

/**
 * @description: 更新页面数据, 如果为脏页则需写入磁盘，再更新为新页面，更新page元数据(data, is_dirty, page_id)和page table
 * @param {Page*} page 写回页指针
 * @param {PageId} new_page_id 新的page_id
 * @param {frame_id_t} new_frame_id 新的帧frame_id
 *
 * 此函数把"准备给 new_page_id 使用"前的所有清理工作集中处理：
 *   (a) 旧 page 若脏，按"旧" PageId 写回磁盘；
 *   (b) 把旧 PageId 从 page_table_ 中摘除；
 *   (c) 把 new_page_id 注册进 page_table_（若 new_page_id 合法）；
 *   (d) reset_memory + 切换 page->id_ + 清 dirty 标志。
 *
 * 注意：本函数同样假设调用方持有 latch_。
 */
void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    // (a) 脏页回写：必须使用 page->id_（旧 PageId）来定位磁盘位置，而不是 new_page_id。
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }

    // (b) 摘除旧映射。仅在旧 page_id 合法时才 erase；从 free_list_ 取出来的帧，
    // 其 page->id_.page_no == INVALID_PAGE_ID，本来就不在 page_table_ 中。
    if (page->id_.page_no != INVALID_PAGE_ID) {
        page_table_.erase(page->id_);
    }

    // (c) 建立新映射。当 new_page_id 表示"无目标页"（INVALID_PAGE_ID，例如 delete_page 走这里
    //     仅做清理）时不要把 INVALID 项写进 page_table_，否则会污染哈希表。
    if (new_page_id.page_no != INVALID_PAGE_ID) {
        page_table_[new_page_id] = new_frame_id;
    }

    // (d) reset_memory 把 PAGE_SIZE 字节清零；调用方如需把磁盘内容读进来，会随后再 read_page 覆盖。
    page->reset_memory();
    page->id_ = new_page_id;
}

/**
 * @description: 从buffer pool获取需要的页。
 *              如果页表中存在page_id（说明该page在缓冲池中），并且pin_count++。
 *              如果页表不存在page_id（说明该page在磁盘中），则找缓冲池victim page，将其替换为磁盘中读取的page，pin_count置1。
 * @return {Page*} 若获得了需要的页则将其返回，否则返回nullptr
 * @param {PageId} page_id 需要获取的页的PageId
 */
Page *BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    // 1. 命中：直接 pin 一下、引用计数 +1 即可，不涉及磁盘 I/O。
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        Page *page = &pages_[frame_id];
        page->pin_count_++;
        // pin replacer：从"可淘汰集合"里把它移走，避免下一次 victim 误选这个正在用的 frame。
        replacer_->pin(frame_id);
        return page;
    }

    // 2. 未命中：得先腾出一个空帧。
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        // 既无空闲帧、replacer 又选不出 victim → 整池都被 pin 住了，无能为力。
        return nullptr;
    }

    // 3. 把腾出来的 frame 从"承载旧页"切换到"承载 page_id 这一页"。
    //    update_page 会负责脏页回写、page_table_ 增删、reset_memory、id_ 切换。
    Page *page = &pages_[frame_id];
    update_page(page, page_id, frame_id);

    // 4. 从磁盘读真实数据进来。注意必须在 update_page 之后做，因为 reset_memory 会把 data_ 清零。
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);

    // 5. 这是上层第一次"获取"这个 page，引用计数从 0 升到 1，并 pin 该 frame。
    page->pin_count_ = 1;
    replacer_->pin(frame_id);

    return page;
}

/**
 * @description: 取消固定pin_count>0的在缓冲池中的page
 * @return {bool} 如果目标页的pin_count<=0则返回false，否则返回true
 * @param {PageId} page_id 目标page的page_id
 * @param {bool} is_dirty 若目标page应该被标记为dirty则为true，否则为false
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock{latch_};

    // 1. 查页表。不在缓冲池里就没法 unpin。
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];

    // 2. 已经是 0 了仍来 unpin 视为非法（往往说明上层多 unpin 了一次）。
    if (page->pin_count_ <= 0) {
        return false;
    }

    // 3. 引用计数减一。一个 page 可能被多个游标 / 查询同时 fetch，
    //    每一次 fetch 都对应未来一次 unpin；只有真正没人再用时（pin_count==0），
    //    才允许 replacer 把它放回"可淘汰集合"。
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        replacer_->unpin(frame_id);
    }

    // 4. dirty 标志只能由 false → true，不能被 false 覆盖回去。
    //    比如：线程A unpin(true)（写过），线程B unpin(false)（仅读），结果应该仍是 dirty。
    if (is_dirty) {
        page->is_dirty_ = true;
    }
    return true;
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 * @return {bool} 成功则返回true，否则返回false(只有page_table_中没有目标页时)
 * @param {PageId} page_id 目标页的page_id，不能为INVALID_PAGE_ID
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    // 不在缓冲池里则没什么可刷的。
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];

    // "强制刷盘" —— 即便 is_dirty_ 是 false 也要写一次磁盘（题目明确要求），
    // 这样可以保证写后磁盘内容与缓冲池内存内容完全一致。
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE);
    page->is_dirty_ = false;
    return true;
}

/**
 * @description: 创建一个新的page，即从磁盘中移动一个新建的空page到缓冲池某个位置。
 * @return {Page*} 返回新创建的page，若创建失败则返回nullptr
 * @param {PageId*} page_id 当成功创建一个新的page时存储其page_id
 */
Page *BufferPoolManager::new_page(PageId *page_id) {
    std::scoped_lock lock{latch_};

    // 1. 先看有没有可用的 frame；没有就新页面无家可归。
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }

    // 2. 让 DiskManager 在该 fd 对应的文件里给我们分配一个新的 page_no（自增）。
    //    注意这里的 fd 由调用方通过 page_id->fd 提供。
    PageId new_pid;
    new_pid.fd = page_id->fd;
    new_pid.page_no = disk_manager_->allocate_page(page_id->fd);

    // 3. 用 update_page 把 frame 从"承载旧页"过渡到"承载 new_pid"：
    //    - 旧页若脏会先被写回；
    //    - page_table_ 增删；
    //    - reset_memory 把数据置 0（新页的初始内容应是空白）；
    //    - page->id_ 切换为 new_pid。
    Page *page = &pages_[frame_id];
    update_page(page, new_pid, frame_id);

    // 4. 上层马上就要用这页（往里写数据），所以 pin_count 从 0 -> 1，并通知 replacer。
    page->pin_count_ = 1;
    replacer_->pin(frame_id);

    // 5. 回传新页的 PageId 给调用方，以便后续 fetch / unpin / delete。
    *page_id = new_pid;
    return page;
}

/**
 * @description: 从buffer_pool删除目标页
 * @return {bool} 如果目标页不存在于buffer_pool或者成功被删除则返回true，若其存在于buffer_pool但无法删除则返回false
 * @param {PageId} page_id 目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock{latch_};

    // 1. 不在缓冲池里 —— 题目语义直接视为"删除成功"，因为缓冲池层面无事可做。
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }
    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];

    // 2. 还有人在用就不让删（防止用户拿到悬空指针）。
    if (page->pin_count_ != 0) {
        return false;
    }

    // 3. 用 update_page 完成"脏页回写 + 摘除映射 + reset_memory"；
    //    new_page_id 用 INVALID_PAGE_ID 表示这个 frame 之后将放回 free_list_，
    //    update_page 内部已经处理了"不把 INVALID 写进 page_table_"。
    PageId invalid_pid{page_id.fd, INVALID_PAGE_ID};
    update_page(page, invalid_pid, frame_id);

    // 4. 该 frame 此刻完全干净，丢回 free_list_ 给后续的 new_page / fetch_page 使用。
    free_list_.push_back(frame_id);

    // 5. 同时也要从 replacer_ 里把它摘掉 —— 否则 replacer_ 可能再次"淘汰"一个其实
    //    已经在 free_list_ 里的 frame，造成同一帧被重复发放。
    replacer_->pin(frame_id);

    return true;
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 *
 * 注意：内部直接对 page_table_ 遍历并写回，不复用 flush_page，因为
 *       flush_page 会重复加 latch_ 造成死锁（std::mutex 不可重入）。
 */
void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock{latch_};

    for (auto &kv : page_table_) {
        const PageId &pid = kv.first;
        // 只刷指定文件的页，跨文件不动。
        if (pid.fd != fd) continue;
        Page *page = &pages_[kv.second];
        // 这里同样按"强制刷盘"语义处理：无论是否 dirty 都写一次。
        disk_manager_->write_page(pid.fd, pid.page_no, page->data_, PAGE_SIZE);
        page->is_dirty_ = false;
    }
}
