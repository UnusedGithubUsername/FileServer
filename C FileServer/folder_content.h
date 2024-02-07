#ifndef getFolderContent
#define getFolderContent


#define BUFFER_SIZE 65536


//global variables

extern const char* BaseFolderPath; 

typedef struct FileToDistribute {
    char* rel_path;
    int fname_len;
    UINT64 creation_time;
    ULONGLONG fsize;
    struct FileToDistribute* next;
} FileToDistribute;

typedef struct FileList { //for counting files in the folders at startup
    FileToDistribute* first;
    FileToDistribute* last;
    int Count;
}FileList;

void GetFilesInFolder(FileList* FileList, char* BasePath);

char* CreateFileUpdatePackage(int* bufferPosition, FileToDistribute** server_files, int* filecount);

#endif
