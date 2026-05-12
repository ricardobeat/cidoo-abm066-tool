module cidoo_clock_c3;

const int CLOCK_MONOTONIC = 6;
const int CC_SHA256_DIGEST_LENGTH = 32;

alias FilePtr = void*;
alias TimeT = long;

extern fn int printf(char* format, ...);
extern fn void perror(char* text);
extern fn void exit(int status);
extern fn void* memset(void* ptr, int value, usz size);
extern fn void* memcpy(void* dst, void* src, usz size);
extern fn int memcmp(void* a, void* b, usz size);
extern fn void* calloc(usz count, usz size);
extern fn void free(void* ptr);
extern fn FilePtr fopen(char* path, char* mode);
extern fn usz fread(void* ptr, usz size, usz count, FilePtr stream);
extern fn usz fwrite(void* ptr, usz size, usz count, FilePtr stream);
extern fn int ferror(FilePtr stream);
extern fn int fclose(FilePtr stream);
extern fn TimeT time(TimeT* out);
extern fn Tm* localtime_r(TimeT* timep, Tm* result);
extern fn TimeT mktime(Tm* tm);
extern fn int nanosleep(Timespec* request, Timespec* remain);
extern fn int clock_gettime(int clock_id, Timespec* ts);
extern fn char* CC_SHA256(char* data, uint len, char* digest);

struct Tm @cname("tm") {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    char* tm_zone;
}

struct Timespec @cname("timespec") {
    long tv_sec;
    long tv_nsec;
}
