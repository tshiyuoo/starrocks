// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fs/hdfs/fs_hdfs.h"

#include <fmt/format.h>
#include <hdfs/hdfs.h>

#include <exception>
#include <utility>

#include "fs/encrypt_file.h"
#include "fs/fs_util.h"
#include "fs/hdfs/hdfs_fs_cache.h"
#include "gutil/strings/substitute.h"
#include "runtime/file_result_writer.h"
#include "service/backend_options.h"
#include "testutil/sync_point.h"
#include "udf/java/utils.h"
#include "util/failpoint/fail_point.h"
#include "util/hdfs_util.h"

using namespace fmt::literals;

namespace starrocks {

class GetHdfsFileReadOnlyHandle {
public:
    GetHdfsFileReadOnlyHandle(const FSOptions& options, std::string path, int buffer_size)
            : _options(std::move(options)), _path(std::move(path)), _buffer_size(buffer_size) {}

    StatusOr<hdfsFS> getOrCreateFS() {
        if (_hdfs_client == nullptr) {
            SCOPED_RAW_TIMER(&_total_open_fs_time_ns);
            std::string namenode;
            RETURN_IF_ERROR(get_namenode_from_path(_path, &namenode));
            RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, _hdfs_client, _options));
        }
        return _hdfs_client->hdfs_fs;
    }

    StatusOr<hdfsFile> getOrCreateFile() {
        if (_file == nullptr) {
            auto st = getOrCreateFS();
            SCOPED_RAW_TIMER(&_total_open_file_time_ns);
            if (!st.ok()) return st.status();
            _file = hdfsOpenFile(st.value(), _path.c_str(), O_RDONLY, _buffer_size, 0, 0);
            if (_file == nullptr) {
                if (errno == ENOENT) {
                    return Status::RemoteFileNotFound(fmt::format("hdfsOpenFile failed, backend={}, file={}",
                                                                  BackendOptions::get_localhost(), _path));
                } else {
                    return Status::InternalError(fmt::format("hdfsOpenFile failed, backend={}, file={}. err_msg: {}",
                                                             BackendOptions::get_localhost(), _path,
                                                             get_hdfs_err_msg()));
                }
            }
        }
        return _file;
    }

    hdfsFS getFS() { return _hdfs_client->hdfs_fs; }
    hdfsFile getFile() { return _file; }
    int64_t getTotalOpenFSTimeNs() const { return _total_open_fs_time_ns; }
    int64_t getTotalOpenFileTimeNs() const { return _total_open_file_time_ns; }
    const std::string& getPath() const { return _path; }
    void setOffset(int64_t offset) { _offset = offset; }

    Status seek(int64_t offset) {
        hdfsFS fs = getFS();
        int ret = hdfsSeek(fs, _file, offset);
        if (ret == -1) {
            return Status::IOError(fmt::format("fail to hdfdSeek {}: {}", _path, get_hdfs_err_msg()));
        }
        return Status::OK();
    }

    Status ensureOpened() {
        RETURN_IF_ERROR(getOrCreateFS());
        RETURN_IF_ERROR(getOrCreateFile());
        return Status::OK();
    }

    int close() {
        int r = 0;
        if (_file != nullptr) {
            hdfsFS fs = getFS();
            r = hdfsCloseFile(fs, _file);
            _file = nullptr;
        }
        return r;
    }

    StatusOr<int64_t> pread(uint8_t* data, int64_t size, int retry = 0) {
        RETURN_IF_ERROR(ensureOpened());
        hdfsFS fs = getFS();
        for (int i = 0; i < (retry + 1); i++) {
            tSize r = hdfsPread(fs, _file, _offset, data, static_cast<tSize>(size));
            if (r == -1) {
                (void)close();
                RETURN_IF_ERROR(ensureOpened());
            } else {
                _offset += r;
                return r;
            }
        }
        return Status::IOError(fmt::format("fail to hdfsPread {}: {}", _path, get_hdfs_err_msg()));
    }

    StatusOr<int64_t> read(uint8_t* data, int64_t size, int retry = 0) {
        RETURN_IF_ERROR(ensureOpened());
        RETURN_IF_ERROR(seek(_offset));

        hdfsFS fs = getFS();
        int64_t now = 0;
        uint8_t* buf = data;

        while (now < size) {
            tSize r = 0;
            for (int i = 0; i < (retry + 1); i++) {
                r = hdfsRead(fs, _file, buf + now, size - now);
                if (r != -1) break;
                if (i == retry) {
                    return Status::IOError(fmt::format("fail to hdfsRead {}: {}", _path, get_hdfs_err_msg()));
                } else {
                    (void)close();
                    RETURN_IF_ERROR(ensureOpened());
                    RETURN_IF_ERROR(seek(_offset));
                }
            }
            if (r == 0) break;
            now += r;
            _offset += r;
        }
        return now;
    }

    StatusOr<int64_t> getSize() {
        RETURN_IF_ERROR(getOrCreateFS());
        hdfsFS fs = getFS();
        auto info = hdfsGetPathInfo(fs, _path.c_str());
        if (UNLIKELY(info == nullptr)) {
            return Status::IOError(fmt::format("Fail to get path info of {}: {}", _path, get_hdfs_err_msg()));
        }
        int64_t size = info->mSize;
        hdfsFreeFileInfo(info, 1);
        return size;
    }

private:
    const FSOptions _options;
    std::string _path;
    int _buffer_size;
    std::shared_ptr<HdfsFsClient> _hdfs_client = nullptr;
    hdfsFile _file = nullptr;
    int64_t _total_open_fs_time_ns = 0;
    int64_t _total_open_file_time_ns = 0;
    int64_t _offset = 0;
};

// ==================================  HdfsInputStream  ==========================================

// TODO: move this class to directory 'be/srcio/'
// class for remote read hdfs file
// Now this is not thread-safe.
class HdfsInputStream : public io::SeekableInputStream {
public:
    HdfsInputStream(std::unique_ptr<GetHdfsFileReadOnlyHandle> handle) : _handle(std::move(handle)) {}

    ~HdfsInputStream() override;

    StatusOr<int64_t> read(void* data, int64_t size) override;
    StatusOr<int64_t> get_size() override;
    StatusOr<int64_t> position() override { return _offset; }
    StatusOr<std::unique_ptr<io::NumericStatistics>> get_numeric_statistics() override;
    Status seek(int64_t offset) override;
    void set_size(int64_t size) override;

private:
    std::unique_ptr<GetHdfsFileReadOnlyHandle> _handle;
    int64_t _offset{0};
    int64_t _file_size{0};
};

HdfsInputStream::~HdfsInputStream() {
    auto ret = call_hdfs_scan_function_in_pthread([this]() {
        int r = _handle->close();
        if (r == -1) {
            auto error_msg = fmt::format("Fail to close file {}: {}", _handle->getPath(), get_hdfs_err_msg());
            LOG(WARNING) << error_msg;
            return Status::IOError(error_msg);
        }
        return Status::OK();
    });
    Status st = ret->get_future().get();
    PLOG_IF(ERROR, !st.ok()) << "close " << _handle->getPath() << " failed";
}

StatusOr<int64_t> HdfsInputStream::read(void* data, int64_t size) {
    if (UNLIKELY(size > std::numeric_limits<tSize>::max())) {
        size = std::numeric_limits<tSize>::max();
    }
    return _handle->pread(static_cast<uint8_t*>(data), size, config::hdfs_client_io_read_retry);
    // return _handle->read(static_cast<uint8_t*>(data), size, config::hdfs_client_io_read_retry);
}

Status HdfsInputStream::seek(int64_t offset) {
    if (offset < 0) return Status::InvalidArgument(fmt::format("Invalid offset {}", offset));
    _handle->setOffset(offset);
    return Status::OK();
}

StatusOr<int64_t> HdfsInputStream::get_size() {
    if (_file_size == 0) {
        auto ret = call_hdfs_scan_function_in_pthread([this] {
            auto st = _handle->getSize();
            if (!st.ok()) return st.status();
            this->_file_size = st.value();
            return Status::OK();
        });
        RETURN_IF_ERROR(ret->get_future().get());
    }
    return _file_size;
}

void HdfsInputStream::set_size(int64_t value) {
    _file_size = value;
}

StatusOr<std::unique_ptr<io::NumericStatistics>> HdfsInputStream::get_numeric_statistics() {
    // `GetReadStatistics` is only supported in HDFS input stream
    if (!fs::is_hdfs_uri(_handle->getPath())) {
        return nullptr;
    }

    auto statistics = std::make_unique<io::NumericStatistics>();
    io::NumericStatistics* stats = statistics.get();
    auto ret = call_hdfs_scan_function_in_pthread([this, stats] {
        hdfsFile file = _handle->getFile();
        if (file == nullptr) {
            return Status::OK();
        }
        stats->append(HdfsReadMetricsKey::kTotalOpenFSTimeNs, _handle->getTotalOpenFSTimeNs());
        stats->append(HdfsReadMetricsKey::kTotalOpenFileTimeNs, _handle->getTotalOpenFileTimeNs());

        struct hdfsReadStatistics* hdfs_statistics = nullptr;
        auto r = hdfsFileGetReadStatistics(file, &hdfs_statistics);
        if (r == -1) {
            return Status::IOError(fmt::format("Fail to get read statistics of {}: {}", r, get_hdfs_err_msg()));
        }
        stats->append(HdfsReadMetricsKey::kTotalBytesRead, hdfs_statistics->totalBytesRead);
        stats->append(HdfsReadMetricsKey::kTotalLocalBytesRead, hdfs_statistics->totalLocalBytesRead);
        stats->append(HdfsReadMetricsKey::kTotalShortCircuitBytesRead, hdfs_statistics->totalShortCircuitBytesRead);
        stats->append(HdfsReadMetricsKey::kTotalZeroCopyBytesRead, hdfs_statistics->totalZeroCopyBytesRead);
        hdfsFileFreeReadStatistics(hdfs_statistics);

        if (config::hdfs_client_enable_hedged_read) {
            struct hdfsHedgedReadMetrics* hdfs_hedged_read_statistics = nullptr;
            r = hdfsGetHedgedReadMetrics(_handle->getFS(), &hdfs_hedged_read_statistics);
            if (r == 0) {
                stats->append(HdfsReadMetricsKey::kTotalHedgedReadOps, hdfs_hedged_read_statistics->hedgedReadOps);
                stats->append(HdfsReadMetricsKey::kTotalHedgedReadOpsInCurThread,
                              hdfs_hedged_read_statistics->hedgedReadOpsInCurThread);
                stats->append(HdfsReadMetricsKey::kTotalHedgedReadOpsWin,
                              hdfs_hedged_read_statistics->hedgedReadOpsWin);
                hdfsFreeHedgedReadMetrics(hdfs_hedged_read_statistics);
            }
        }
        return Status::OK();
    });
    Status st = ret->get_future().get();
    if (!st.ok()) return st;
    return std::move(statistics);
}

class HDFSWritableFile : public WritableFile {
public:
    HDFSWritableFile(hdfsFS fs, hdfsFile file, std::string path, size_t offset)
            : _fs(fs), _file(file), _path(std::move(path)), _offset(offset) {
        FileSystem::on_file_write_open(this);
    }

    ~HDFSWritableFile() override { (void)HDFSWritableFile::close(); }

    Status append(const Slice& data) override;

    Status appendv(const Slice* data, size_t cnt) override;

    Status close() override;

    Status pre_allocate(uint64_t size) override { return Status::NotSupported("HDFS file pre_allocate not supported"); }

    Status flush(FlushMode mode) override {
        int status = hdfsHFlush(_fs, _file);
        return status == 0 ? Status::OK()
                           : Status::IOError(fmt::format("Fail to flush {}: {}", _path, get_hdfs_err_msg()));
    }

    Status sync() override {
        int status = hdfsHSync(_fs, _file);
        return status == 0 ? Status::OK()
                           : Status::IOError(fmt::format("Fail to sync {}: {}", _path, get_hdfs_err_msg()));
    }

    uint64_t size() const override { return _offset; }

    const std::string& filename() const override { return _path; }

private:
    hdfsFS _fs;
    hdfsFile _file;
    std::string _path;
    size_t _offset;
    bool _closed{false};
};

Status HDFSWritableFile::append(const Slice& data) {
    FAIL_POINT_TRIGGER_RETURN(output_stream_io_error, Status::IOError("injected output_stream_io_error"));
    tSize r = hdfsWrite(_fs, _file, data.data, data.size);
    if (r == -1) { // error
        auto error_msg = fmt::format("Fail to append {}: {}", _path, get_hdfs_err_msg());
        LOG(WARNING) << error_msg;
        return Status::IOError(error_msg);
    }
    if (r != data.size) {
        auto error_msg =
                fmt::format("Fail to append {}, expect written size: {}, actual written size {} ", _path, data.size, r);
        LOG(WARNING) << error_msg;
        return Status::IOError(error_msg);
    }
    _offset += data.size;
    return Status::OK();
}

Status HDFSWritableFile::appendv(const Slice* data, size_t cnt) {
    FAIL_POINT_TRIGGER_RETURN(output_stream_io_error, Status::IOError("injected output_stream_io_error"));
    for (size_t i = 0; i < cnt; i++) {
        RETURN_IF_ERROR(append(data[i]));
    }
    return Status::OK();
}

Status HDFSWritableFile::close() {
    if (_closed) {
        return Status::OK();
    }
    FileSystem::on_file_write_close(this);
    auto ret = call_hdfs_scan_function_in_pthread([this]() {
        FAIL_POINT_TRIGGER_RETURN(output_stream_io_error, Status::IOError("injected output_stream_io_error"));
        int r = hdfsHSync(_fs, _file);
        TEST_SYNC_POINT_CALLBACK("HDFSWritableFile::close", &r);
        auto st = Status::OK();
        if (r == -1) {
            auto error_msg = fmt::format("Fail to sync file {}: {}", _path, get_hdfs_err_msg());
            LOG(WARNING) << error_msg;
            st.update(Status::IOError(error_msg));
        }

        FAIL_POINT_TRIGGER_RETURN(output_stream_io_error, Status::IOError("injected output_stream_io_error"));
        r = hdfsCloseFile(_fs, _file);
        if (r == -1) {
            auto error_msg = fmt::format("Fail to close file {}: {}", _path, get_hdfs_err_msg());
            LOG(WARNING) << error_msg;
            st.update(Status::IOError(error_msg));
        }
        return st;
    });
    Status st = ret->get_future().get();
    PLOG_IF(ERROR, !st.ok()) << "close " << _path << " failed";
    _closed = true;
    return st;
}

class HdfsFileSystem : public FileSystem {
public:
    HdfsFileSystem(const FSOptions& options) : _options(std::move(options)) {}
    ~HdfsFileSystem() override = default;

    HdfsFileSystem(const HdfsFileSystem&) = delete;
    void operator=(const HdfsFileSystem&) = delete;
    HdfsFileSystem(HdfsFileSystem&&) = delete;
    void operator=(HdfsFileSystem&&) = delete;

    Type type() const override { return HDFS; }

    using FileSystem::new_sequential_file;
    using FileSystem::new_random_access_file;

    StatusOr<std::unique_ptr<RandomAccessFile>> new_random_access_file(const RandomAccessFileOptions& opts,
                                                                       const std::string& path) override;

    StatusOr<std::unique_ptr<SequentialFile>> new_sequential_file(const SequentialFileOptions& opts,
                                                                  const std::string& path) override;

    StatusOr<std::unique_ptr<WritableFile>> new_writable_file(const std::string& path) override;

    StatusOr<std::unique_ptr<WritableFile>> new_writable_file(const WritableFileOptions& opts,
                                                              const std::string& path) override;

    Status path_exists(const std::string& path) override;

    Status get_children(const std::string& dir, std::vector<std::string>* file) override {
        return Status::NotSupported("HdfsFileSystem::get_children");
    }

    Status iterate_dir(const std::string& dir, const std::function<bool(std::string_view)>& cb) override;

    Status iterate_dir2(const std::string& dir, const std::function<bool(DirEntry)>& cb) override;

    // Implementation Notes:
    // To avoid an unnecessary HDFS API call for path type validation, the target path’s type (file or directory)
    // is not checked prior to issuing the delete request. Consequently, the hdfsDelete() function successfully
    // handles both files and empty directories. This means the delete operation will complete successfully even
    // if the target path is an empty directory rather than a file. This is similar to `delete_dir()`.
    Status delete_file(const std::string& path) override {
        std::string namenode;
        RETURN_IF_ERROR(get_namenode_from_path(path, &namenode));
        std::shared_ptr<HdfsFsClient> hdfs_client;
        RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));

        if (hdfsDelete(hdfs_client->hdfs_fs, path.c_str(), 0) == 0) {
            return Status::OK();
        }
        int err_number = errno;

        Status exists_status = _path_exists(hdfs_client->hdfs_fs, path);
        if (!exists_status.ok()) {
            return exists_status;
        }

        ASSIGN_OR_RETURN(bool is_dir, _is_directory(hdfs_client->hdfs_fs, path));
        if (is_dir) {
            return Status::InvalidArgument(fmt::format("path {} is a directory, not a file", path));
        }
        return hdfs_error_to_status(fmt::format("Failed to delete HDFS file: {}.", path), err_number);
    }

    Status create_dir(const std::string& dirname) override {
        std::string namenode;
        RETURN_IF_ERROR(get_namenode_from_path(dirname, &namenode));
        std::shared_ptr<HdfsFsClient> hdfs_client;
        RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));

        if (hdfsCreateDirectory(hdfs_client->hdfs_fs, dirname.c_str()) != 0) {
            int err_number = errno;
            if (err_number == EEXIST) {
                return Status::OK();
            } else {
                return hdfs_error_to_status(fmt::format("Failed to create HDFS directory: {}.", dirname), err_number);
            }
        }
        return Status::OK();
    }

    Status create_dir_if_missing(const std::string& dirname, bool* created) override {
        std::string namenode;
        RETURN_IF_ERROR(get_namenode_from_path(dirname, &namenode));
        std::shared_ptr<HdfsFsClient> hdfs_client;
        RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));

        // Check if directory already exists
        Status exists_status = _path_exists(hdfs_client->hdfs_fs, dirname);
        if (exists_status.ok()) {
            // Path exists, check if it's a directory
            ASSIGN_OR_RETURN(bool is_dir, _is_directory(hdfs_client->hdfs_fs, dirname));
            if (is_dir) {
                if (created) *created = false;
                return Status::OK();
            } else {
                return Status::InvalidArgument(fmt::format("Path {} exists but is not a directory", dirname));
            }
        }

        // Directory doesn't exist, create it
        if (hdfsCreateDirectory(hdfs_client->hdfs_fs, dirname.c_str()) != 0) {
            return hdfs_error_to_status(fmt::format("Failed to create HDFS directory: {}.", dirname), errno);
        }

        if (created) *created = true;
        return Status::OK();
    }

    Status create_dir_recursive(const std::string& dirname) override {
        // For HDFS, hdfsCreateDirectory already creates parent directories recursively
        // This is the default behavior in HDFS - it works like "mkdir -p"
        return create_dir_if_missing(dirname, nullptr);
    }

    // Implementation Notes:
    // To avoid an unnecessary HDFS API call for path type validation, the target path’s type (file or directory)
    // is not checked prior to issuing the delete request. Consequently, the hdfsDelete() function successfully
    // handles both files and empty directories. This means the delete operation will complete successfully even
    // if the target path is a file rather than an empty directory. This is similar to `delete_file()`.
    Status delete_dir(const std::string& dirname) override {
        std::string namenode;
        RETURN_IF_ERROR(get_namenode_from_path(dirname, &namenode));
        std::shared_ptr<HdfsFsClient> hdfs_client;
        RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));

        if (hdfsDelete(hdfs_client->hdfs_fs, dirname.c_str(), 0) == 0) {
            return Status::OK();
        }
        int err_number = errno;

        Status exists_status = _path_exists(hdfs_client->hdfs_fs, dirname);
        if (!exists_status.ok()) {
            return exists_status;
        }

        ASSIGN_OR_RETURN(bool is_dir, _is_directory(hdfs_client->hdfs_fs, dirname));
        if (!is_dir) {
            return Status::InvalidArgument(fmt::format("path {} is not a directory", dirname));
        }
        return hdfs_error_to_status(fmt::format("Failed to delete HDFS directory: {}.", dirname), err_number);
    }

    Status delete_dir_recursive(const std::string& dirname) override {
        std::string namenode;
        RETURN_IF_ERROR(get_namenode_from_path(dirname, &namenode));
        std::shared_ptr<HdfsFsClient> hdfs_client;
        RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));

        if (hdfsDelete(hdfs_client->hdfs_fs, dirname.c_str(), 1) == 0) {
            return Status::OK();
        }
        int err_number = errno;

        Status exists_status = _path_exists(hdfs_client->hdfs_fs, dirname);
        if (exists_status.is_not_found()) {
            return Status::OK();
        }
        if (!exists_status.ok()) {
            return exists_status;
        }

        ASSIGN_OR_RETURN(bool is_dir, _is_directory(hdfs_client->hdfs_fs, dirname));
        if (!is_dir) {
            return Status::InvalidArgument(fmt::format("path {} is not a directory", dirname));
        }
        return hdfs_error_to_status(fmt::format("Failed to recursively delete HDFS directory: {}.", dirname),
                                    err_number);
    }

    Status sync_dir(const std::string& dirname) override { return Status::NotSupported("HdfsFileSystem::sync_dir"); }

    StatusOr<bool> is_directory(const std::string& path) override {
        std::string namenode;
        RETURN_IF_ERROR(get_namenode_from_path(path, &namenode));
        std::shared_ptr<HdfsFsClient> hdfs_client;
        RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));

        return _is_directory(hdfs_client->hdfs_fs, path);
    }

    Status canonicalize(const std::string& path, std::string* file) override {
        return Status::NotSupported("HdfsFileSystem::canonicalize");
    }

    StatusOr<uint64_t> get_file_size(const std::string& path) override {
        return Status::NotSupported("HdfsFileSystem::get_file_size");
    }

    StatusOr<uint64_t> get_file_modified_time(const std::string& path) override {
        return Status::NotSupported("HdfsFileSystem::get_file_modified_time");
    }

    Status rename_file(const std::string& src, const std::string& target) override;

    Status link_file(const std::string& old_path, const std::string& new_path) override {
        return Status::NotSupported("HdfsFileSystem::link_file");
    }

private:
    Status _path_exists(hdfsFS fs, const std::string& path);
    static StatusOr<bool> _is_directory(hdfsFS fs, const std::string& path);

    FSOptions _options;
};

Status HdfsFileSystem::path_exists(const std::string& path) {
    std::string namenode;
    RETURN_IF_ERROR(get_namenode_from_path(path, &namenode));
    std::shared_ptr<HdfsFsClient> hdfs_client;
    RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));
    return _path_exists(hdfs_client->hdfs_fs, path);
}

Status HdfsFileSystem::iterate_dir(const std::string& dir, const std::function<bool(std::string_view)>& cb) {
    std::string namenode;
    RETURN_IF_ERROR(get_namenode_from_path(dir, &namenode));
    std::shared_ptr<HdfsFsClient> hdfs_client;
    RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));
    Status status = _path_exists(hdfs_client->hdfs_fs, dir);
    if (!status.ok()) {
        return status;
    }

    hdfsFileInfo* fileinfo;
    int numEntries;
    fileinfo = hdfsListDirectory(hdfs_client->hdfs_fs, dir.data(), &numEntries);
    if (fileinfo == nullptr) {
        return Status::InvalidArgument(fmt::format("hdfs list directory error {}", dir));
    }
    for (int i = 0; i < numEntries && fileinfo; ++i) {
        // obj_key.data() + uri.key().size(), obj_key.size() - uri.key().size()
        int32_t dir_size;
        if (dir[dir.size() - 1] == '/') {
            dir_size = dir.size();
        } else {
            dir_size = dir.size() + 1;
        }
        std::string_view name(fileinfo[i].mName + dir_size);
        if (!cb(name)) {
            break;
        }
    }
    if (fileinfo) {
        hdfsFreeFileInfo(fileinfo, numEntries);
    }
    return Status::OK();
}

Status HdfsFileSystem::iterate_dir2(const std::string& dir, const std::function<bool(DirEntry)>& cb) {
    std::string namenode;
    RETURN_IF_ERROR(get_namenode_from_path(dir, &namenode));
    std::shared_ptr<HdfsFsClient> hdfs_client;
    RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));
    Status status = _path_exists(hdfs_client->hdfs_fs, dir);
    if (!status.ok()) {
        return status;
    }

    hdfsFileInfo* fileinfo;
    int numEntries;
    fileinfo = hdfsListDirectory(hdfs_client->hdfs_fs, dir.data(), &numEntries);
    if (fileinfo == nullptr) {
        return Status::InvalidArgument(fmt::format("hdfs list directory error {}", dir));
    }
    for (int i = 0; i < numEntries && fileinfo; ++i) {
        // obj_key.data() + uri.key().size(), obj_key.size() - uri.key().size()
        int32_t dir_size;
        std::string mName(fileinfo[i].mName);
        std::size_t found = mName.rfind('/');
        if (found == std::string::npos) {
            dir_size = 0;
        } else {
            dir_size = found + 1;
        }

        std::string_view name(fileinfo[i].mName + dir_size);
        DirEntry entry{.name = name,
                       .mtime = fileinfo[i].mLastMod,
                       .size = fileinfo[i].mSize,
                       .is_dir = fileinfo[i].mKind == tObjectKind::kObjectKindDirectory};
        if (!cb(entry)) {
            break;
        }
    }
    if (fileinfo) {
        hdfsFreeFileInfo(fileinfo, numEntries);
    }
    return Status::OK();
}

Status HdfsFileSystem::_path_exists(hdfsFS fs, const std::string& path) {
    int status = hdfsExists(fs, path.data());
    return status == 0 ? Status::OK() : Status::NotFound(path);
}

StatusOr<bool> HdfsFileSystem::_is_directory(hdfsFS fs, const std::string& path) {
    hdfsFileInfo* file_info = hdfsGetPathInfo(fs, path.c_str());
    if (file_info == nullptr) {
        return hdfs_error_to_status(fmt::format("Failed to get HDFS path info: {}.", path), errno);
    }

    bool is_dir = (file_info->mKind == kObjectKindDirectory);
    hdfsFreeFileInfo(file_info, 1);
    return is_dir;
}

StatusOr<std::unique_ptr<WritableFile>> HdfsFileSystem::new_writable_file(const std::string& path) {
    return HdfsFileSystem::new_writable_file(WritableFileOptions(), path);
}

StatusOr<std::unique_ptr<WritableFile>> HdfsFileSystem::new_writable_file(const WritableFileOptions& opts,
                                                                          const std::string& path) {
    std::string namenode;
    RETURN_IF_ERROR(get_namenode_from_path(path, &namenode));
    std::shared_ptr<HdfsFsClient> hdfs_client;
    RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));
    int flags = O_WRONLY;
    // O_WRONLY means create or overwrite for hdfsOpenFile, which is exactly CREATE_OR_OPEN_WITH_TRUNCATE
    if (opts.mode == MUST_CREATE) {
        if (auto st = _path_exists(hdfs_client->hdfs_fs, path); st.ok()) {
            return Status::AlreadyExist(path);
        }
    } else if (opts.mode == MUST_EXIST) {
        return Status::NotSupported("Open with MUST_EXIST not supported by hdfs writer");
    } else if (opts.mode == CREATE_OR_OPEN) {
        return Status::NotSupported("Open with CREATE_OR_OPEN not supported by hdfs writer");
    } else if (opts.mode != CREATE_OR_OPEN_WITH_TRUNCATE) {
        auto msg = strings::Substitute("Unsupported open mode $0", opts.mode);
        return Status::NotSupported(msg);
    }

    // `io.file.buffer.size` of https://apache.github.io/hadoop/hadoop-project-dist/hadoop-common/core-default.xml
    int hdfs_write_buffer_size = 0;
    // pass zero to hdfsOpenFile will use the default hdfs_write_buffer_size
    if (_options.result_file_options != nullptr) {
        hdfs_write_buffer_size = _options.result_file_options->write_buffer_size_kb;
    }
    if (_options.export_sink != nullptr && _options.export_sink->__isset.hdfs_write_buffer_size_kb) {
        hdfs_write_buffer_size = _options.export_sink->hdfs_write_buffer_size_kb;
    }
    if (_options.upload != nullptr && _options.upload->__isset.hdfs_write_buffer_size_kb) {
        hdfs_write_buffer_size = _options.upload->hdfs_write_buffer_size_kb;
    }

    hdfsFile file = hdfsOpenFile(hdfs_client->hdfs_fs, path.c_str(), flags, hdfs_write_buffer_size, 0, 0);
    if (file == nullptr) {
        if (errno == ENOENT) {
            return Status::RemoteFileNotFound(fmt::format("hdfsOpenFile failed, file={}", path));
        } else {
            return Status::InternalError(
                    fmt::format("hdfsOpenFile failed, file={}. err_msg: {}", path, get_hdfs_err_msg()));
        }
    }
    return wrap_encrypted(std::make_unique<HDFSWritableFile>(hdfs_client->hdfs_fs, file, path, 0),
                          opts.encryption_info);
}

StatusOr<std::unique_ptr<SequentialFile>> HdfsFileSystem::new_sequential_file(const SequentialFileOptions& opts,
                                                                              const std::string& path) {
    // pass zero to hdfsOpenFile will use the default hdfs_read_buffer_size
    int hdfs_read_buffer_size = 0;
    if (_options.scan_range_params != nullptr && _options.scan_range_params->__isset.hdfs_read_buffer_size_kb) {
        hdfs_read_buffer_size = _options.scan_range_params->hdfs_read_buffer_size_kb;
    }
    if (_options.download != nullptr && _options.download->__isset.hdfs_read_buffer_size_kb) {
        hdfs_read_buffer_size = _options.download->hdfs_read_buffer_size_kb;
    }
    auto handle = std::make_unique<GetHdfsFileReadOnlyHandle>(_options, path, hdfs_read_buffer_size);
    auto stream = std::make_unique<HdfsInputStream>(std::move(handle));
    return SequentialFile::from(std::move(stream), path, opts.encryption_info);
}

StatusOr<std::unique_ptr<RandomAccessFile>> HdfsFileSystem::new_random_access_file(const RandomAccessFileOptions& opts,
                                                                                   const std::string& path) {
    // pass zero to hdfsOpenFile will use the default hdfs_read_buffer_size
    int hdfs_read_buffer_size = 0;
    if (_options.scan_range_params != nullptr && _options.scan_range_params->__isset.hdfs_read_buffer_size_kb) {
        hdfs_read_buffer_size = _options.scan_range_params->hdfs_read_buffer_size_kb;
    }
    if (_options.download != nullptr && _options.download->__isset.hdfs_read_buffer_size_kb) {
        hdfs_read_buffer_size = _options.download->hdfs_read_buffer_size_kb;
    }
    auto handle = std::make_unique<GetHdfsFileReadOnlyHandle>(_options, path, hdfs_read_buffer_size);
    auto stream = std::make_unique<HdfsInputStream>(std::move(handle));
    return RandomAccessFile::from(std::move(stream), path, false, opts.encryption_info);
}

Status HdfsFileSystem::rename_file(const std::string& src, const std::string& target) {
    std::string namenode;
    RETURN_IF_ERROR(get_namenode_from_path(src, &namenode));
    std::shared_ptr<HdfsFsClient> hdfs_client;
    RETURN_IF_ERROR(HdfsFsCache::instance()->get_connection(namenode, hdfs_client, _options));
    int ret = hdfsRename(hdfs_client->hdfs_fs, src.data(), target.data());
    if (ret != 0) {
        return Status::InvalidArgument(fmt::format("rename file from {} to {} error", src, target));
    }
    return Status::OK();
}

std::unique_ptr<FileSystem> new_fs_hdfs(const FSOptions& options) {
    return std::make_unique<HdfsFileSystem>(options);
}

} // namespace starrocks
