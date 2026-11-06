// Lecture POSIX de l’espace utilisable : le code C++ évite de reproduire l’ABI statvfs en Rust.
#include <sys/statvfs.h>
#include <cstdint>
#include <cerrno>
#include <limits>
extern "C" int sc_available_space(const char *path,std::uint64_t *available,std::uint64_t *block) {
    struct statvfs info{};
    if(statvfs(path,&info)!=0)return errno;
    *block=info.f_frsize?info.f_frsize:info.f_bsize;
    if(!*block)return EIO;
    const auto count=std::uint64_t(info.f_bavail);
    *available=count>std::numeric_limits<std::uint64_t>::max()/ *block
        ?std::numeric_limits<std::uint64_t>::max():count* *block;
    return 0;
}
