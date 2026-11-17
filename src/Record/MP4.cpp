/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#if defined(__linux__) && !defined(_GNU_SOURCE)
// fallocate 선언은 첫 시스템 헤더보다 앞에서 GNU 기능을 켜야 보장된다.
#define _GNU_SOURCE
#endif

#if defined(ENABLE_MP4)

#include "MP4.h"
#include "Util/File.h"
#include "Util/logger.h"
#include "Common/config.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace toolkit;
using namespace std;

namespace mediakit {

static struct mov_buffer_t s_io = {
        [](void *ctx, void *data, uint64_t bytes) {
            MP4FileIO *thiz = (MP4FileIO *) ctx;
            return thiz->onRead(data, bytes);
        },
        [](void *ctx, const void *data, uint64_t bytes) {
            MP4FileIO *thiz = (MP4FileIO *) ctx;
            return thiz->onWrite(data, bytes);
        },
        [](void *ctx, int64_t offset) {
            MP4FileIO *thiz = (MP4FileIO *) ctx;
            return thiz->onSeek(offset);
        },
        [](void *ctx) {
            MP4FileIO *thiz = (MP4FileIO *) ctx;
            return thiz->onTell();
        }
};

MP4FileIO::Writer MP4FileIO::createWriter(int flags, bool is_fmp4){
    Writer writer;
    Ptr self = shared_from_this();
    // 保存自己的强引用，防止提前释放  [AUTO-TRANSLATED:e8e14f60]
    // Save a strong reference to itself to prevent premature release
    writer.reset(mp4_writer_create(is_fmp4, &s_io,this, flags),[self](mp4_writer_t *ptr){
        if(ptr){
            mp4_writer_destroy(ptr);
        }
    });
    if(!writer){
        throw std::runtime_error("写入mp4文件失败!");
    }
    return writer;
}

MP4FileIO::Reader MP4FileIO::createReader(){
    Reader reader;
    Ptr self = shared_from_this();
    // 保存自己的强引用，防止提前释放  [AUTO-TRANSLATED:e8e14f60]
    // Save a strong reference to itself to prevent premature release
    reader.reset(mov_reader_create(&s_io,this),[self](mov_reader_t *ptr){
        if(ptr){
            mov_reader_destroy(ptr);
        }
    });
    if(!reader){
        throw std::runtime_error("读取mp4文件失败!");
    }
    return reader;
}

/////////////////////////////////////////////////////MP4FileDisk/////////////////////////////////////////////////////////

#if defined(_WIN32) || defined(_WIN64)
    #define fseek64 _fseeki64
    #define ftell64 _ftelli64
#else
    #define fseek64 fseek
    #define ftell64 ftell
#endif

static uint64_t recordPreallocBytes(const std::string &raw) {
    size_t pos = 0;
    const bool negative = !raw.empty() && raw[0] == '-';
    if (!raw.empty() && (raw[0] == '-' || raw[0] == '+')) ++pos;
    bool valid = pos < raw.size();
    unsigned mb = 0;
    for (; pos < raw.size(); ++pos) {
        const char c = raw[pos];
        if (c < '0' || c > '9') { valid = false; break; }
        // 임의 길이 숫자도 곱셈 전에 상한을 묶어 정수 오버플로를 피한다.
        if (mb <= 2048) mb = (std::min)(2049u, mb * 10 + static_cast<unsigned>(c - '0'));
    }
    const unsigned clamped = !valid || negative ? 0u : (std::min)(2048u, mb);
    if (!valid || (negative && mb) || mb > 2048) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set()) {
            WarnL << "[recording-io] 미리 할당 설정 보정: record.preallocMB=" << raw.substr(0, 64)
                  << " 허용범위=정수[0,2048] 적용_MB=" << clamped << " 경고=프로세스당1회";
        }
    }
    return static_cast<uint64_t>(clamped) * 1024 * 1024;
}

uint32_t MP4FileDisk::preallocateFile(FILE *fp, uint64_t bytes) {
    if (!bytes) return 0;
#if defined(_WIN32)
    const auto handle = _get_osfhandle(_fileno(fp));
    if (handle == -1) return ERROR_INVALID_HANDLE;
    FILE_ALLOCATION_INFO allocation = {};
    allocation.AllocationSize.QuadPart = static_cast<LONGLONG>(bytes);
    // CRT가 소유한 핸들이다. EOF 변경·자르기·별도 CloseHandle을 하지 않는다.
    return SetFileInformationByHandle(reinterpret_cast<HANDLE>(handle), FileAllocationInfo,
                                      &allocation, sizeof(allocation)) ? 0 : GetLastError();
#elif defined(__linux__)
    if (bytes > static_cast<uint64_t>((std::numeric_limits<off_t>::max)())) return EOVERFLOW;
    return fallocate(fileno(fp), FALLOC_FL_KEEP_SIZE, 0, static_cast<off_t>(bytes)) == 0 ? 0 : errno;
#else
    // 지원 API가 없는 OS에서는 기존 순차 쓰기를 그대로 사용한다.
    (void)fp;
    return 0;
#endif
}

void MP4FileDisk::openFile(const char *file, const char *mode) {
    // 创建文件  [AUTO-TRANSLATED:bd145ed5]
    // Create a file
    _io_trace = std::make_shared<RecordIoTrace>(RecordIoTrace::volume(file), [](const RecordIoSample &s) {
        WarnL << "[recording-io] op=" << recordIoName(s.op) << " file_id=" << s.file_id << " volume=" << s.volume
              << " elapsed_ms=" << s.elapsed_ms << " bytes=" << s.bytes << " failed=" << s.failed
              << " errno=" << s.error << " suppressed=" << s.suppressed;
    });
    auto trace = _io_trace;
    auto fp = trace->measure(RecordIoOp::Create, 0, [&]() { return File::create_file(file, mode); }, [](FILE *p) { return !p; });
    if(!fp){
        throw std::runtime_error(string("打开文件失败:") + file);
    }

    GET_CONFIG(uint32_t,mp4BufSize,Record::kFileBufSize);

    // 新建文件io缓存  [AUTO-TRANSLATED:fda9ff47]
    // Create a new file io cache
    std::shared_ptr<char> file_buf(new char[mp4BufSize],[](char *ptr){
        if(ptr){
            delete [] ptr;
        }
    });

    if(file_buf){
        // 设置文件io缓存  [AUTO-TRANSLATED:0ed9c8ad]
        // Set the file io cache
        setvbuf(fp, file_buf.get(), _IOFBF, mp4BufSize);
    }

    // 새로 쓰는 MP4만 대상이다. rb/rb+ 읽기·기존 파일 갱신·추가 열기는 예약하지 않는다.
    if (mode && std::strcmp(mode, "wb+") == 0) {
        GET_CONFIG_FUNC(uint64_t, preallocBytes, Record::kPreallocMB, recordPreallocBytes);
        const uint64_t bytes = preallocBytes;
        if (bytes) {
            const int saved_errno = errno;
#if defined(_WIN32)
            const DWORD saved_error = GetLastError();
#endif
            const uint32_t error = preallocateFile(fp, bytes);
            if (error) {
                // 프로세스 전체 예약 실패는 60초당 한 줄로 제한하고 생략 건수를 다음 경고에 합친다.
                static RecordIoLimiter limiter;
                uint64_t suppressed = 0;
                if (limiter.claim(RecordIoOp::Create, RecordIoTrace::now(), suppressed)) {
                    WarnL << "[recording-io] 미리 할당 실패: 작업=미리할당 경로=" << file << " 요청_바이트=" << bytes
                          << " 오류코드=" << error << " 생략_건수=" << suppressed << " 녹화=기존쓰기계속";
                }
            }
            errno = saved_errno;
#if defined(_WIN32)
            SetLastError(saved_error);
#endif
        }
    }

    // 创建智能指针  [AUTO-TRANSLATED:e7920ab2]
    // Create a smart pointer
    _file.reset(fp,[file_buf, trace](FILE *fp) {
        trace->measure(RecordIoOp::Flush, 0, [&]() { return fflush(fp); }, [](int r) { return r != 0; });
        trace->measure(RecordIoOp::Close, 0, [&]() { return fclose(fp); }, [](int r) { return r != 0; });
    });
}

void MP4FileDisk::closeFile() {
    _file = nullptr;
}

int MP4FileDisk::onRead(void *data, size_t bytes) {
    const auto count = _io_trace->measure(RecordIoOp::Read, bytes, [&]() { return fread(data, 1, bytes, _file.get()); }, [&](size_t) { return ferror(_file.get()) != 0; });
    if (bytes == count){
        return 0;
    }
    return 0 != ferror(_file.get()) ? ferror(_file.get()) : -1 /*EOF*/;
}

int MP4FileDisk::onWrite(const void *data, size_t bytes) {
    const auto count = _io_trace->measure(RecordIoOp::Write, bytes, [&]() { return fwrite(data, 1, bytes, _file.get()); }, [&](size_t n) { return n != bytes; });
    return bytes == count ? 0 : ferror(_file.get());
}

int MP4FileDisk::onSeek(int64_t offset) {
    return _io_trace->measure(RecordIoOp::Seek, 0, [&]() { return fseek64(_file.get(), offset, offset >= 0 ? SEEK_SET : SEEK_END); }, [](int r) { return r != 0; });
}

int64_t MP4FileDisk::onTell() {
    return _io_trace->measure(RecordIoOp::Tell, 0, [&]() { return ftell64(_file.get()); }, [](int64_t r) { return r < 0; });
}

/////////////////////////////////////////////////////MP4FileMemory/////////////////////////////////////////////////////////

string MP4FileMemory::getAndClearMemory(){
    string ret;
    ret.swap(_memory);
    _offset = 0;
    return ret;
}

size_t MP4FileMemory::fileSize() const{
    return _memory.size();
}

int64_t MP4FileMemory::onTell(){
    return _offset;
}

int MP4FileMemory::onSeek(int64_t offset){
    if (offset < 0) {
        offset += _memory.size();
        if (offset < 0) {
            return -1;
        }
        _offset = offset;
    } else {
        if (offset > _memory.size()) {
            return -1;
        }
        _offset = offset;
    }
    return 0;
}

int MP4FileMemory::onRead(void *data, size_t bytes){
    if (_offset >= _memory.size()) {
        //EOF
        return -1;
    }
    bytes = MIN(bytes, _memory.size() - _offset);
    memcpy(data, _memory.data() + _offset, bytes);
    _offset += bytes;
    return 0;
}

int MP4FileMemory::onWrite(const void *data, size_t bytes){
    if (_offset + bytes > _memory.size()) {
        // 需要扩容  [AUTO-TRANSLATED:211c91e3]
        // Need to expand
        _memory.resize(_offset + bytes);
    }
    memcpy((uint8_t *) _memory.data() + _offset, data, bytes);
    _offset += bytes;
    return 0;
}

}//namespace mediakit
#endif // defined(ENABLE_MP4)
