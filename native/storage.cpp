// Lecture POSIX de l’espace utilisable : le code C++ évite de reproduire l’ABI statvfs en Rust.
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <fcntl.h>
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

// STATX_MNT_ID conserve l’identité du montage bind et ne suit pas le lien final.
// L’ancien sondage par descripteur reste disponible côté Rust si le noyau ne le fournit pas.
extern "C" int sc_path_mount_id(const char *path,std::uint64_t *mount) {
#ifdef STATX_MNT_ID
    struct statx info{};
    // L’identifiant est fourni par le noyau : ne pas forcer d’accès réseau ni d’automontage.
    if(statx(AT_FDCWD,path,AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT|AT_STATX_DONT_SYNC,STATX_MNT_ID,&info)!=0)return errno;
    if(!(info.stx_mask & STATX_MNT_ID))return ENOTSUP;
    *mount=info.stx_mnt_id;
    return 0;
#else
    (void)path;(void)mount;
    return ENOSYS;
#endif
}
