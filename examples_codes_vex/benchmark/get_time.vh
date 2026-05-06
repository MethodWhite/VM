extern "kernel32.dll" {
    fn GetTickCount64() -> u64;
}

u64 now_ms() {
    return GetTickCount64();
}