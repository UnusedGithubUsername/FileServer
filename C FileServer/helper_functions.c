#include <winsock2.h> 
#include <assert.h>
#include <fileapi.h>
#include <windows.h>         
#include "folder_content.h"
const char* BaseFolderPath = "C:\\Users\\Klauke\\Documents\\My Games\\Corivi\\LauncherServer\\*.*";
const char* BaseFolderPath2 = "F:\\Programme\\Heroes3 + Heroes3HotA\\Heroes 3 - HotA\\*.*";


char* CreateFileUpdatePackage(int* bufferPosition, FileToDistribute* server_files, int* filecount) {
    char* package_buffer = malloc(BUFFER_SIZE);
    assert(package_buffer != NULL);

    *bufferPosition = 4;
    FileList files;

    files.first = 0;
    files.last = 0;
    files.Count = 0;


    GetFilesInFolder(&files, BaseFolderPath);
    if (files.first == 0) {
        return;
    }

    //we need to convert our list into an array to have simpler access
    server_files = malloc(files.Count * sizeof(FileToDistribute));
    assert(server_files != 0);
    filecount = files.Count;

    FileToDistribute* current_file = files.first;
    for (int i = 0; i < filecount; i++) //convert list to array
    {
        memcpy(&server_files[i], current_file, sizeof(FileToDistribute));
        FileToDistribute* next = current_file->next;
        free(current_file);
        current_file = next;
    }

    //write all files to a byte[] that we can instantly send over to anyone who wants a complete fililist
    for (int i = 0; i < files.Count; i++)
    {
        printf("    %.*s \n", server_files[i].fname_len, (server_files[i].rel_path));// print NON terminated string with ".*"

        memcpy(&package_buffer[*bufferPosition], &(server_files[i].fname_len), 4);
        *bufferPosition += 4; //write how many character
        memcpy(&package_buffer[*bufferPosition], &(server_files[i].rel_path[0]), server_files[i].fname_len);
        *bufferPosition += server_files[i].fname_len;//write the characters        
        memcpy(&package_buffer[*bufferPosition], &(server_files[i].creation_time), 8);
        *bufferPosition += 8;//write the creationTime, this is for version control on the client side
    }
    package_buffer[*bufferPosition] = '\0';

    int packagesize_excluding_packagenumber = *bufferPosition - 4;//write packageID and packagesize to the front, 
    memcpy(&package_buffer[0], &packagesize_excluding_packagenumber, 4);

    printf("%d\n", files.Count);

    return package_buffer;
}

void GetFilesInFolder(FileList* File_List, char* BasePath) {

    char searchPath[MAX_PATH];
    memcpy(searchPath, BasePath, strlen(BasePath));
    searchPath[strlen(BasePath)] = '\0';
    printf("%s \n", BasePath);

    WIN32_FIND_DATA findFileData;
    HANDLE hFind = FindFirstFileA(searchPath, &findFileData);
    FindNextFileW(hFind, &findFileData);
    FindNextFileW(hFind, &findFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        printf("Invalid directory path. Exiting............................. %s/n", BasePath);
        printf(" ............................. %s/n", BasePath);
        printf(" ............................. %s/n", BasePath);
        printf(" ............................. %s/n", BasePath);
        return;
    }

    do {
        if (findFileData.cFileName[0] == '.') {//ignore things that start with a . as those may be neither files nor folders
            continue;
        }
        char full_file_path[200] = { 0 }; //now create a path that contains the previous path + file/folder name. many folders add up
        memcpy(&full_file_path[0], BasePath, strlen(BasePath));//copy basePath

        int nameLen = 0;        //fileData name is in format 16bit WCHAR, we need to convert it to 8 bit. Thats why strcpy does not work here
        while (findFileData.cFileName[nameLen] != '\0') {   //convert cFilename to char[];
            full_file_path[nameLen + strlen(BasePath) - 3] = (char)findFileData.cFileName[nameLen];
            nameLen++;
        }

        int is_regular_file = !(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);//check if the directoryFile bit is set by & with a checker
        if (is_regular_file) {

            WIN32_FILE_ATTRIBUTE_DATA fileInfo;
            GetFileAttributesExA(full_file_path, GetFileExInfoStandard, &fileInfo);
            size_t base_path_len = strlen(BaseFolderPath) - 3;
            size_t partPathLen = strlen(full_file_path) - base_path_len;

            FileToDistribute* file_information = (FileToDistribute*)malloc(sizeof(FileToDistribute));
            assert(file_information != NULL);
            file_information->rel_path = malloc(partPathLen);
            assert(file_information->rel_path != 0);

            memcpy(&(file_information->rel_path[0]), &full_file_path[base_path_len], partPathLen);
            file_information->fname_len = (int)partPathLen; //null terminator is not part of the string info.
            file_information->creation_time = (((ULONGLONG)fileInfo.ftLastWriteTime.dwHighDateTime) << 32) + fileInfo.ftLastWriteTime.dwLowDateTime;
            file_information->fsize = (((ULONGLONG)fileInfo.nFileSizeHigh) << 32) + fileInfo.nFileSizeLow;

            if (File_List->last == NULL)//if first and last are 0 we are adding the first element
            {
                File_List->first = file_information;
                File_List->last = file_information;
            }
            else
            {
                File_List->last->next = file_information;
                File_List->last = file_information;
            }
            File_List->Count++;
        }
        else // the filehande is a folder
        {
            size_t len_of_path = strlen(full_file_path);
            full_file_path[len_of_path] = '\\';
            full_file_path[len_of_path + 1] = '*';
            full_file_path[len_of_path + 2] = '.';
            full_file_path[len_of_path + 3] = '*';
            full_file_path[len_of_path + 4] = '\0';
            GetFilesInFolder(File_List, &full_file_path);
            //add //to the end and a 0 and call this method recursively 
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);
}