/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

/*
 * RmFileHandle —— 一个表对应一个数据文件，本类把"按页存储"的物理细节
 * 包装成"按记录"的逻辑接口（get / insert / delete / update + scan）。
 *
 * 数据文件页面布局：
 *   page_no = 0          : 文件头页 RM_FILE_HDR_PAGE，存 RmFileHdr，由 RmManager 直写直读，
 *                          不经过缓冲池。
 *   page_no >= 1         : record-data 页，按下面的"段式布局"存放若干条等长记录：
 *
 *     ┌─────────────┬─────────────────┬─────────────────────────────────┐
 *     │  RmPageHdr  │  bitmap         │  slots[0..num_records_per_page) │
 *     │  (8 bytes)  │  bitmap_size B  │  每个 slot = file_hdr.record_size │
 *     └─────────────┴─────────────────┴─────────────────────────────────┘
 *     注：page->data_ 的最前 4 字节其实是 LSN 区（Page::OFFSET_LSN=0,
 *         OFFSET_PAGE_HDR=4），因此 RmPageHandle 计算 page_hdr 时用了
 *         OFFSET_PAGE_HDR 偏移。本任务无需关心 LSN（Lab4 才用）。
 *
 * "未满页面单链表"：
 *   - file_hdr_.first_free_page_no  : 链表头页号；空时为 RM_NO_PAGE (= -1)
 *   - 每个未满页的 page_hdr->next_free_page_no : 链表后继；尾节点为 RM_NO_PAGE
 *   - 一个页面"满"的判定：page_hdr->num_records == file_hdr_.num_records_per_page
 *   - 一个页面"在链表中"⇔ 它当前未满
 *   该单链表让 insert_record 能 O(1) 找到一个有空位的页面，避免线性扫所有页。
 *
 * 不变式：
 *   I1  page_hdr->num_records == 当前 bitmap 中 1 位的个数
 *   I2  free 链表只串"未满页面"
 *   I3  bitmap[slot] == 1  ⇔  slots 区第 slot 个槽位有有效记录
 *   I4  对所有有效 rid，0 < rid.page_no < file_hdr_.num_pages
 *                       0 <= rid.slot_no < file_hdr_.num_records_per_page
 */

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    // 1. 把所在 page 取进缓冲池，构造一个 RmPageHandle 拿到 bitmap / slots 的内部指针。
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    // 2. 防御：bitmap 中对应位为 0 表示该 slot 已被删除/从未分配，应当抛错。
    //    这样能在调用方传入悬空 Rid（例如查询过期游标）时立刻暴露问题。
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        // 注意：抛之前要把刚才 fetch 的页 unpin 掉，避免这一帧泄漏。
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    // 3. 把目标 slot 的内容拷贝一份返回给上层；上层拿到的是独立内存，不受 buffer pool 淘汰影响。
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size);
    memcpy(record->data, page_handle.get_slot(rid.slot_no), file_hdr_.record_size);

    // 4. 这次访问只读，不脏；归还该帧的 pin。
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    return record;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(char* buf, Context* context) {
    // 1. 找一个"有空位"的页面：要么从 first_free_page_no 链上拿，要么新建一页。
    //    create_page_handle 内部会 fetch / new，并保持 page 处于 pinned 状态。
    RmPageHandle page_handle = create_page_handle();

    // 2. bitmap 里第一个 0 位就是空闲 slot；num_records 不会满（因为该页本来就在 free 链上）。
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    assert(slot_no < file_hdr_.num_records_per_page);

    // 3. 把记录写入对应 slot，再标记 bitmap，更新页头计数。
    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, slot_no);
    page_handle.page_hdr->num_records++;

    // 4. 维护"未满页面单链表"：如果这次插入恰好把页面填满，需要把它从 free 链头摘掉。
    //    （删除中间节点的代价更高，但这里它一定就是当前 first_free_page_no，因为
    //    create_page_handle 优先返回 first_free_page_no 指向的页。）
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
    }

    // 5. 该页脏了（写过 slot/bitmap/page_hdr），unpin 时必须传 true 让 BPM 知道要回写。
    Rid rid{page_handle.page->get_page_id().page_no, slot_no};
    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    return rid;
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {char*} buf 要插入记录的数据
 *
 * 该重载主要给"日志重做 (redo)"使用：必须把记录恰好放回 rid 指定的位置。
 * 当前 Lab1 的测试不调用本接口，但实现上完整对称地处理 free 链表和 bitmap。
 */
void RmFileHandle::insert_record(const Rid& rid, char* buf) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    // 重做语义：若指定 slot 已经被占用就什么都不做（幂等）。
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
        Bitmap::set(page_handle.bitmap, rid.slot_no);
        page_handle.page_hdr->num_records++;

        // 同样的"插满则脱链"逻辑。
        if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
            // 注意：这里要从链表中 *精确* 移除 rid.page_no，可能它不在链表头。
            // 教学场景下我们用一次线性扫描把它摘掉，简洁可靠。
            int prev = RM_NO_PAGE;
            int cur = file_hdr_.first_free_page_no;
            while (cur != RM_NO_PAGE && cur != rid.page_no) {
                RmPageHandle ph = fetch_page_handle(cur);
                int next = ph.page_hdr->next_free_page_no;
                buffer_pool_manager_->unpin_page(ph.page->get_page_id(), false);
                prev = cur;
                cur = next;
            }
            if (cur == rid.page_no) {
                if (prev == RM_NO_PAGE) {
                    file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
                } else {
                    RmPageHandle prev_ph = fetch_page_handle(prev);
                    prev_ph.page_hdr->next_free_page_no = page_handle.page_hdr->next_free_page_no;
                    buffer_pool_manager_->unpin_page(prev_ph.page->get_page_id(), true);
                }
            }
        }
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
    } else {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
    }
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    // 防御：bitmap 已为 0 视为重复删除，直接返回（保持幂等）。
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        return;
    }

    // 关键：先记下"删之前是不是满的"，因为下一行就会改变 num_records，无法事后判断。
    bool was_full = (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page);

    // 真正的删除：bitmap 置 0 即可，slot 内的字节可以留作"垃圾"，下次 insert 会覆盖。
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;

    // 如果删除之前页面是满的，那么 *现在* 它从"满"变"未满"——必须把它挂回 free 链。
    if (was_full) {
        release_page_handle(page_handle);
    }

    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}


/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);

    // 更新一条不存在的记录在语义上是错的，按"找不到"抛异常。
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    // 定长记录场景下，更新 = 原地覆盖即可，不影响 bitmap / num_records / free 链。
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);

    buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), true);
}

/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 *
 * 注意：本函数会 pin 该页（fetch_page 的副作用），调用方负责在用完后 unpin。
 *       不在内部 unpin 是因为返回的 page_handle 内的指针都指向 page->data_，
 *       一旦 unpin 该页可能被 victim 出局，handle 立刻悬空。
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    // 合法性检查：page_no 必须在 (0, num_pages) 范围内。
    //   - page_no = 0 是文件头页，不通过 buffer pool 访问；
    //   - page_no >= num_pages 表示尚未分配。
    if (page_no <= 0 || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError("", page_no);
    }
    Page* page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    if (page == nullptr) {
        // 缓冲池满且全部 pinned —— 这种情况在测试场景里不会发生，但留下防御。
        throw PageNotExistError("", page_no);
    }
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 *
 * 步骤：
 *   1) 让 buffer pool 在我们的 fd 对应文件上分配并 pin 一个新 frame；
 *   2) 把这块新 page 的内容初始化为空 page（page_hdr 清零、bitmap 清零）；
 *   3) 更新 file_hdr_：num_pages++，并把这新页串到 free 链头部，因为它当然是未满的。
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    // 1) 在该文件追加一个新 page。BPM 内部会调用 disk_manager->allocate_page(fd_)
    //    自增分配 page_no（应当 == 旧的 num_pages）。
    PageId new_pid{fd_, INVALID_PAGE_ID};
    Page* page = buffer_pool_manager_->new_page(&new_pid);
    if (page == nullptr) {
        // 如果缓冲池没有可用 frame（全 pinned），无法新建；直接 panic 由上层兜底。
        throw InternalError("RmFileHandle::create_new_page_handle: buffer pool full");
    }

    // 2) 初始化 page handle 内部的元数据视图。new_page 已经把 data_ 全部清零，
    //    严格来说 page_hdr 已经是 {0, 0}；但 next_free_page_no 应该是 RM_NO_PAGE (= -1) 而非 0，
    //    所以必须显式赋值，避免被误解读为"指向 page 0"。
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->num_records = 0;
    page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);

    // 3) 更新文件级元数据：
    //    - 多了一页 → num_pages 自增；
    //    - 这页是空的（必然未满）→ 串到 free 链表头部。
    //      把它接到 free 链头而不是尾，可以让 create_page_handle 的"取头节点"是 O(1)。
    file_hdr_.num_pages++;
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = new_pid.page_no;

    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    // 1) 链表头 == RM_NO_PAGE 表示当前没有任何"未满"页面，必须分配新页面。
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        return create_new_page_handle();
    }
    // 2) 否则，链头那页就是有空位的，直接 fetch 出来用即可。
    //    （注意：调用方把记录插入后如果导致这页满了，会负责把它从链头摘除。）
    return fetch_page_handle(file_hdr_.first_free_page_no);
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 *
 * 这个动作只发生在 delete_record 中：删除前页面是满的（不在 free 链上），
 * 删除后变为未满，需要把它**插入** free 链头部。
 *
 * 设计上为什么是头插而不是尾插？
 *   - 头插 O(1)，尾插需要遍历到链尾或额外维护 last 指针；
 *   - 头插带有一种"局部性"启发：刚被改动的页大概率还在 buffer pool 里，
 *     下一次 insert_record 复用它能命中缓存，进一步降低 I/O。
 */
void RmFileHandle::release_page_handle(RmPageHandle& page_handle) {
    // 把当前页 next 指针指向旧链头，再把旧链头替换为本页页号，即头插。
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}
