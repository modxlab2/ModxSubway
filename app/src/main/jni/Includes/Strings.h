DWORD getAbsoluteAddress(DWORD relativeAddr, const char *libraryName = "") {
    if (libBase == 0)
        libBase = findLibrary(libraryName);
    if (libBase != 0)
        return (reinterpret_cast<DWORD>(libBase + relativeAddr));
    else
        return 0;
}
typedef struct _monoString {
    void *klass;
    void *monitor;
    int length;
    char chars[1];

    int getLength() {
        return length;
    }

    char *getChars() {
        return chars;
    }
} monoString;
