#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>

#if defined(ENABLE_MP4)
#include "Common/config.h"
#include "Record/MP4.h"
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace mediakit;
using namespace toolkit;

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

struct TempFile {
    std::string path;
    TempFile() {
#if defined(_WIN32)
        char folder[MAX_PATH + 1] = {}, name[MAX_PATH + 1] = {};
        const DWORD length = GetTempPathA(sizeof(folder), folder);
        check(length && length < sizeof(folder), "시험 임시 폴더 조회 실패");
        check(GetTempFileNameA(folder, "efp", 0, name) != 0, "시험 임시 파일 생성 실패");
        path = name;
#else
        char name[] = "/tmp/edgeflow-prealloc-XXXXXX";
        const int fd = mkstemp(name);
        check(fd >= 0, "시험 임시 파일 생성 실패");
        path = name;
        check(close(fd) == 0, "시험 임시 파일 닫기 실패");
#endif
    }
    ~TempFile() {
        if (std::remove(path.c_str()) != 0) std::cerr << "시험 임시 파일 삭제 실패: " << path << '\n';
    }
};

static void setting(const std::string &value) {
    mINI::Instance()[Record::kPreallocMB] = value;
    NOTICE_EMIT(BroadcastReloadConfigArgs, Broadcast::kBroadcastReloadConfig);
}

struct ConfigRestore {
    const std::string original = mINI::Instance()[Record::kPreallocMB];
    ~ConfigRestore() { setting(original); }
};

// 운영 코드의 OS 호출 경계를 대체해 0·범위 보정·예약 실패 뒤 쓰기 지속을 검사한다.
struct Probe : MP4FileDisk {
    using MP4FileDisk::onRead;
    using MP4FileDisk::onWrite;
    using MP4FileDisk::onSeek;
    using MP4FileDisk::onTell;
    unsigned calls = 0;
    uint64_t requested = 0;
    uint32_t failure = 0;
    uint32_t result = 0;
    bool real = false;
    bool eof_preserved = false;
#if defined(_WIN32)
    LONGLONG allocation_before_close = 0;
#endif
protected:
    uint32_t preallocateFile(FILE *fp, uint64_t bytes) override {
        ++calls;
        requested = bytes;
        if (!real) {
            result = failure;
            return result;
        }
        const uint32_t error = MP4FileDisk::preallocateFile(fp, bytes);
        result = error;
#if defined(_WIN32)
        FILE_STANDARD_INFO info = {};
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(fp)));
        const bool queried = GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof(info)) != 0;
        allocation_before_close = info.AllocationSize.QuadPart;
        eof_preserved = queried && info.EndOfFile.QuadPart == 0 && _ftelli64(fp) == 0;
#else
        eof_preserved = std::ftell(fp) == 0 && std::fseek(fp, 0, SEEK_END) == 0 && std::ftell(fp) == 0;
#endif
        return error;
    }
};

static std::string writeAndClose(Probe &disk) {
    std::string bytes(4099, 'r');
    check(disk.onTell() == 0, "예약 후 쓰기 커서가 0이 아님");
    check(disk.onWrite(bytes.data(), bytes.size()) == 0, "예약 후 작은 파일 쓰기 실패");
    // 일반 MP4가 mdat 길이를 앞쪽에서 보정하는 경우와 같은 재탐색을 검사한다.
    check(disk.onSeek(0) == 0 && disk.onWrite("size", 4) == 0, "앞쪽 크기 보정 쓰기 실패");
    bytes.replace(0, 4, "size");
    check(disk.onSeek(-1) == 0 && disk.onWrite("z", 1) == 0, "EOF 기준 재탐색 쓰기 실패");
    bytes.back() = 'z';
    disk.closeFile();
    return bytes;
}

static void verifyFile(const std::string &path, const std::string &expected, bool released = false,
                       uint64_t reserved = 0) {
    FILE *raw = std::fopen(path.c_str(), "rb");
    check(raw != nullptr, "완료 파일 재열기 실패");
    std::unique_ptr<FILE, int (*)(FILE *)> fp(raw, std::fclose);
    check(std::fseek(raw, 0, SEEK_END) == 0 && std::ftell(raw) == static_cast<long>(expected.size()),
          "완료 EOF가 실제 기록 바이트와 다름");
#if defined(_WIN32)
    if (released) {
        FILE_STANDARD_INFO info = {};
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(raw)));
        check(GetFileInformationByHandleEx(handle, FileStandardInfo, &info, sizeof(info)) != 0,
              "재열기 후 파일 할당 정보 조회 실패");
        check(info.EndOfFile.QuadPart == static_cast<LONGLONG>(expected.size()), "재열기 후 논리 EOF 불일치");
        check(info.AllocationSize.QuadPart >= info.EndOfFile.QuadPart
                  && static_cast<uint64_t>(info.AllocationSize.QuadPart) < reserved,
              "닫기 후 EOF 밖 초과 할당이 반환되지 않음");
    }
#else
    // Linux KEEP_SIZE의 EOF 밖 할당 반환 여부는 파일시스템별 검증 대상이다.
    (void)released;
    (void)reserved;
#endif
    check(std::fseek(raw, 0, SEEK_SET) == 0, "완료 파일 읽기 위치 초기화 실패");
    std::string actual(expected.size(), '\0');
    check(std::fread(&actual[0], 1, actual.size(), raw) == actual.size() && actual == expected,
          "완료 파일 바이트가 원본과 다름");
    check(std::fgetc(raw) == EOF && !std::ferror(raw), "완료 파일 꼬리에 미기록 바이트가 남음");
}

static void settingsAndModes() {
    const struct { const char *raw; uint64_t mb; } cases[] = {
        {"0", 0}, {"1", 1}, {"2048", 2048}, {"2049", 2048}, {"-1", 0},
        {"999999999999999999999999999999", 2048}, {"-999999999999999999999999999999", 0},
        {"1.5", 0}, {"잘못된값", 0}
    };
    for (const auto &item : cases) {
        setting(item.raw);
        TempFile temp;
        Probe disk;
        disk.openFile(temp.path.c_str(), "wb+");
        check(disk.calls == (item.mb ? 1u : 0u) && disk.requested == item.mb * 1024 * 1024,
              "설정 보정 또는 0일 때 OS 예약 호출 생략 계약 위반");
        disk.closeFile();
    }
    setting("0");
    TempFile temp;
    Probe seed;
    seed.openFile(temp.path.c_str(), "wb+");
    const auto expected = writeAndClose(seed);
    setting("4");
    for (const char *mode : {"rb", "rb+", "ab+"}) {
        Probe disk;
        disk.openFile(temp.path.c_str(), mode);
        check(disk.calls == 0, "읽기 또는 기존 파일 열기에 미리 할당이 적용됨");
        disk.closeFile();
        verifyFile(temp.path, expected);
    }
}

static void allocationFailureContinues() {
    setting("4");
    for (const uint32_t error : {static_cast<uint32_t>(ENOSPC), static_cast<uint32_t>(EACCES)}) {
        TempFile temp;
        Probe disk;
        disk.failure = error;
        disk.openFile(temp.path.c_str(), "wb+");
        check(disk.calls == 1 && disk.result == error, "공간 부족·권한 실패 주입이 예약 호출에 도달하지 않음");
        const auto expected = writeAndClose(disk);
        verifyFile(temp.path, expected);
    }
}

static void realAllocation() {
    setting("4");
    TempFile temp;
    Probe disk;
    disk.real = true;
    disk.openFile(temp.path.c_str(), "wb+");
    check(disk.result == 0, "실제 OS 미리 할당 실패");
    check(disk.calls == 1 && disk.eof_preserved, "실제 예약이 EOF 또는 커서를 변경함");
#if defined(_WIN32)
    check(disk.allocation_before_close >= static_cast<LONGLONG>(disk.requested), "Windows 예약 크기 확보 실패");
#endif
    const auto expected = writeAndClose(disk);
    verifyFile(temp.path, expected, true, disk.requested);
}

int main() {
    try {
        ConfigRestore restore;
        check(restore.original == "0", "미리 할당 기본값이 0이 아님");
        settingsAndModes();
        allocationFailureContinues();
        realAllocation();
        std::cout << "미리 할당 설정·호출 생략·실패 후 쓰기·EOF 유지 검사 통과\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "미리 할당 검사 실패: " << error.what() << '\n';
        return 1;
    }
}
#else
int main() {
    std::cout << "MP4 기능이 꺼져 있어 미리 할당 검사를 생략합니다.\n";
    return 0;
}
#endif
