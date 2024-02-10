#ifndef getFolderContent
#define getFolderContent
 
 
extern const WCHAR* BaseFolderPathW;

typedef struct FileToDistribute {
    WCHAR* rel_path;
    int byte_count;
    UINT64 creation_time;
    ULONGLONG fsize;
    HANDLE handle;
    struct FileToDistribute* next;
} FileToDistribute;

typedef struct FileList { //for counting files in the folders at startup
    FileToDistribute* first;
    FileToDistribute* last;
    int Count;
}FileList;

void GetFilesInFolder(FileList* FileList, WCHAR* BasePath);

int CreateFileUpdatePackage(int* bufferPosition, FileToDistribute** server_files, char* package_buffer);

#endif
