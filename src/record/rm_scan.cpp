/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/*
 * RmScan —— 表数据文件的全表扫描游标。
 *
 * 它顺序遍历从 page_no = RM_FIRST_RECORD_PAGE (= 1) 开始的所有"有效记录"。
 * 在 page 内通过 bitmap 找下一个 1 位；当前页找完就跳到下一页继续找。
 *
 * 终止条件：rid_.page_no == RM_NO_PAGE（约定的"end" sentinel，等价于 -1）。
 *
 * 内部约定：rid_ 始终指向"当前游标位置"。
 *   - 构造之后，rid_ 应当指向第一条有效记录（若文件为空则为 end）。
 *   - next() 之后，rid_ 指向下一条有效记录或 end。
 *
 * 性能注意：每次 fetch_page_handle 都要走 buffer pool；为避免在一个长扫描里
 * 反复 fetch/unpin 同一页，可以在 next() 内部做"页内继续找"的优化。
 */

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // 起点定在 (page_no=1, slot_no=-1)，然后调用 next()：next() 会从 (slot_no+1)=0 开始
    // 在第 1 页中找 bitmap 的第一个 1 位；若整个文件为空，next() 会一路扫到末尾把 page_no 置为 RM_NO_PAGE。
    rid_.page_no = RM_FIRST_RECORD_PAGE;
    rid_.slot_no = -1;
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // 已经处于 end 状态时不应再前进。
    if (rid_.page_no == RM_NO_PAGE) {
        return;
    }

    const auto& file_hdr = file_handle_->file_hdr_;
    // 从当前 page 开始往后找；外层循环遍历"页"，内层借助 Bitmap::next_bit 在页内找下一个 1。
    for (int page_no = rid_.page_no; page_no < file_hdr.num_pages; page_no++) {
        // 当从下一页开始时，slot_no 应从头扫，而 next_bit 的语义是"从 curr+1 开始找"，
        // 因此把 curr 设为 -1。
        int start_slot = (page_no == rid_.page_no) ? rid_.slot_no : -1;

        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);
        int next_slot = Bitmap::next_bit(true, page_handle.bitmap,
                                         file_hdr.num_records_per_page, start_slot);
        // 这页只读，不脏；找到与否都先把 page unpin 掉再做下一步决定。
        file_handle_->buffer_pool_manager_->unpin_page(page_handle.page->get_page_id(), false);

        if (next_slot < file_hdr.num_records_per_page) {
            // 找到一条有效记录：更新游标并返回。
            rid_.page_no = page_no;
            rid_.slot_no = next_slot;
            return;
        }
        // 当前页扫完没找到，进入下一页（继续 for 循环）。
    }

    // 扫到文件末尾仍没有有效记录 —— 标记为 end。
    rid_.page_no = RM_NO_PAGE;
    rid_.slot_no = -1;
}

/**
 * @brief 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // 约定：page_no 为 RM_NO_PAGE (= -1) 即代表游标已扫完整个文件。
    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}
