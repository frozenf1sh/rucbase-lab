/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/disk_manager.h"

#include <assert.h>    // for assert
#include <string.h>    // for memset
#include <sys/stat.h>  // for stat
#include <unistd.h>    // for lseek

#include "defs.h"

DiskManager::DiskManager() { memset(fd2pageno_, 0, MAX_FD * (sizeof(std::atomic<page_id_t>) / sizeof(char))); }

/**
 * @description: 将数据写入文件的指定磁盘页面中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 写入目标页面的page_id
 * @param {char} *offset 要写入磁盘的数据
 * @param {int} num_bytes 要写入磁盘的数据大小
 */
void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes) {
    // 一个磁盘文件由若干 PAGE_SIZE 大小的"页"线性排布组成；
    // 第 page_no 页在文件中的字节偏移即 page_no * PAGE_SIZE。
    // 1) 用 lseek 把文件读写指针定位到目标页的首字节
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    if (lseek(fd, file_offset, SEEK_SET) == -1) {
        throw InternalError("DiskManager::write_page Error: lseek failed");
    }
    // 2) write 真正把数据写入；num_bytes 可能等于 PAGE_SIZE（整页）也可能更小（如只刷页头）
    ssize_t bytes_written = write(fd, offset, num_bytes);
    // write 返回值小于 num_bytes 视为出错（磁盘满 / fd 被关 / signal 中断等）
    if (bytes_written != num_bytes) {
        throw InternalError("DiskManager::write_page Error");
    }
}

/**
 * @description: 读取文件中指定编号的页面中的部分数据到内存中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 指定的页面编号
 * @param {char} *offset 读取的内容写入到offset中
 * @param {int} num_bytes 读取的数据量大小
 */
void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes) {
    // 与 write_page 对称：先用 lseek 定位到目标页起始位置，再 read。
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    if (lseek(fd, file_offset, SEEK_SET) == -1) {
        throw InternalError("DiskManager::read_page Error: lseek failed");
    }
    ssize_t bytes_read = read(fd, offset, num_bytes);
    // 这里要求"恰好"读到 num_bytes 字节。读到 EOF（返回 0 或小于 num_bytes）也视作错误，
    // 上层（BufferPoolManager）必须保证只对已分配过的页面发起 read_page。
    if (bytes_read != num_bytes) {
        throw InternalError("DiskManager::read_page Error");
    }
}

/**
 * @description: 分配一个新的页号
 * @return {page_id_t} 分配的新页号
 * @param {int} fd 指定文件的文件句柄
 */
page_id_t DiskManager::allocate_page(int fd) {
    // 简单的自增分配策略，指定文件的页面编号加1
    assert(fd >= 0 && fd < MAX_FD);
    return fd2pageno_[fd]++;
}

void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {}

bool DiskManager::is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void DiskManager::create_dir(const std::string &path) {
    // Create a subdirectory
    std::string cmd = "mkdir " + path;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为path的目录
        throw UnixError();
    }
}

void DiskManager::destroy_dir(const std::string &path) {
    std::string cmd = "rm -r " + path;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 判断指定路径文件是否存在
 * @return {bool} 若指定路径文件存在则返回true 
 * @param {string} &path 指定路径文件
 */
bool DiskManager::is_file(const std::string &path) {
    // 用struct stat获取文件信息
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * @description: 用于创建指定路径文件
 * @return {*}
 * @param {string} &path
 */
void DiskManager::create_file(const std::string &path) {
    // 创建一个空文件；要求：若文件已存在则抛 FileExistsError。
    // 用 O_CREAT | O_EXCL 让 open 在文件已存在时返回 -1（errno=EEXIST），从而避免 race；
    // 这比"先 stat 后 open"更可靠（后者存在 TOCTOU 时间窗）。
    if (is_file(path)) {
        throw FileExistsError(path);
    }
    // 0600：文件权限 rw-------（只允许当前用户读写）
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        throw UnixError();
    }
    // 这里只是创建，不需要持有 fd（上层会再用 open_file 显式打开），所以直接 close。
    if (close(fd) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除指定路径的文件
 * @param {string} &path 文件所在路径
 */
void DiskManager::destroy_file(const std::string &path) {
    // 删除文件前的两个前置检查：
    //   (a) 文件必须存在 —— 否则抛 FileNotFoundError；
    //   (b) 文件必须未处于"已打开"状态 —— DiskManager 通过 path2fd_ 维护打开列表，
    //       若该路径还在表里说明它的 fd 没被 close_file 释放，禁止销毁，避免悬空 fd。
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    if (path2fd_.count(path) > 0) {
        throw FileNotClosedError(path);
    }
    // unlink 仅删除目录项，若有进程仍持有该 fd 则文件 inode 会等所有 fd 关闭后再回收；
    // 但这里我们已确保没有打开 fd，所以 unlink 之后磁盘空间会立即释放。
    if (unlink(path.c_str()) < 0) {
        throw UnixError();
    }
}


/**
 * @description: 打开指定路径文件 
 * @return {int} 返回打开的文件的文件句柄
 * @param {string} &path 文件所在路径
 */
int DiskManager::open_file(const std::string &path) {
    // 1) 文件必须存在
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    // 2) 不允许重复打开（同一路径只持有一个 fd），保持文件打开列表的一一映射
    if (path2fd_.count(path) > 0) {
        throw FileNotClosedError(path);
    }
    // 3) 以读写方式打开
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        throw UnixError();
    }
    // 4) 维护 path<->fd 的双向映射，便于后续通过 fd 反查 path（disk_manager 内部某些
    //    模块需要用文件名做日志/恢复），也便于 destroy_file 阶段判断"文件是否仍处于打开态"。
    path2fd_[path] = fd;
    fd2path_[fd] = path;
    return fd;
}

/**
 * @description:用于关闭指定路径文件 
 * @param {int} fd 打开的文件的文件句柄
 */
void DiskManager::close_file(int fd) {
    // 关闭一个未打开的 fd 是错误：若 fd2path_ 里没有记录说明它不是 disk_manager 管理的合法 fd。
    if (fd2path_.count(fd) == 0) {
        throw FileNotOpenError(fd);
    }
    // 真正关闭内核 fd
    if (close(fd) < 0) {
        throw UnixError();
    }
    // 同步移除两张表中的记录，使 open_file 能再次成功打开同一路径，destroy_file 也才能放心删。
    path2fd_.erase(fd2path_[fd]);
    fd2path_.erase(fd);
}


/**
 * @description: 获得文件的大小
 * @return {int} 文件的大小
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_size(const std::string &file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

/**
 * @description: 根据文件句柄获得文件名
 * @return {string} 文件句柄对应文件的文件名
 * @param {int} fd 文件句柄
 */
std::string DiskManager::get_file_name(int fd) {
    if (!fd2path_.count(fd)) {
        throw FileNotOpenError(fd);
    }
    return fd2path_[fd];
}

/**
 * @description:  获得文件名对应的文件句柄
 * @return {int} 文件句柄
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_fd(const std::string &file_name) {
    if (!path2fd_.count(file_name)) {
        return open_file(file_name);
    }
    return path2fd_[file_name];
}


/**
 * @description:  读取日志文件内容
 * @return {int} 返回读取的数据量，若为-1说明读取数据的起始位置超过了文件大小
 * @param {char} *log_data 读取内容到log_data中
 * @param {int} size 读取的数据量大小
 * @param {int} offset 读取的内容在文件中的位置
 */
int DiskManager::read_log(char *log_data, int size, int offset) {
    // read log file from the previous end
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    int file_size = get_file_size(LOG_FILE_NAME);
    if (offset > file_size) {
        return -1;
    }

    size = std::min(size, file_size - offset);
    if(size == 0) return 0;
    lseek(log_fd_, offset, SEEK_SET);
    ssize_t bytes_read = read(log_fd_, log_data, size);
    assert(bytes_read == size);
    return bytes_read;
}


/**
 * @description: 写日志内容
 * @param {char} *log_data 要写入的日志内容
 * @param {int} size 要写入的内容大小
 */
void DiskManager::write_log(char *log_data, int size) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }

    // write from the file_end
    lseek(log_fd_, 0, SEEK_END);
    ssize_t bytes_write = write(log_fd_, log_data, size);
    if (bytes_write != size) {
        throw UnixError();
    }
}