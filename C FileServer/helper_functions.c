#include <winsock2.h>  
#include <fileapi.h>
#include <windows.h>         
#include "folder_content.h" 
//"F:\\Programme\\Heroes3 + Heroes3HotA\\Heroes 3 - HotA\\*.*";//test this server by sending this
const WCHAR* BaseFolderPathW = L"C:\\Users\\Klauke\\Documents\\My Games\\Corivi\\LauncherServer\\*.*";

int CreateFileUpdatePackage(int* bufferPosition, FileToDistribute** server_files, char* package_buffer) {
    
    FileList files = { 0 };  
    GetFilesInFolder(&files, BaseFolderPathW);

    //we need to convert our list into an array to have simpler access 
    *server_files = malloc(files.Count * sizeof(FileToDistribute));  

    FileToDistribute* current_file = files.first;
    for (int i = 0; i < files.Count; i++) //convert list to array
    {
        FileToDistribute* second_element = (*server_files + i); 
        memcpy((*server_files+i), current_file, sizeof(FileToDistribute));
        FileToDistribute* next = current_file->next;
        free(current_file);
        current_file = next;
    }

    //write all files to a byte[] that we can instantly send over to anyone who wants a complete fililist
    *bufferPosition = 4;
    for (int i = 0; i < files.Count; i++)
    {
        wprintf(L"  %i  %.*ls \n", (*server_files +i)->byte_count /2, (*server_files + i)->byte_count /2, (*server_files + i)->rel_path);// print NON terminated string with ".*"

        memcpy(&package_buffer[*bufferPosition], &((*server_files + i)->byte_count), 4);
        *bufferPosition += 4; //write how many character
        memcpy(&package_buffer[*bufferPosition], ((*server_files + i)->rel_path), (*server_files + i)->byte_count);
        *bufferPosition += (*server_files + i)->byte_count;//write the characters        
        memcpy(&package_buffer[*bufferPosition], &((*server_files + i)->creation_time), 8);
        *bufferPosition += 8;//write the creationTime, this is for version control on the client side
    } 

    int size_excluding_packagenumber = *bufferPosition - 4;//write packageID and packagesize to the front, 
    memcpy(&package_buffer[0], &size_excluding_packagenumber, 4);

    printf("%d\n", files.Count);

    return files.Count;
}

void GetFilesInFolder(FileList* File_List, WCHAR* BasePath) {

    int baseLen = wcslen(BasePath);
    WCHAR searchPath[MAX_PATH];
    memcpy(searchPath, BasePath, baseLen * 2);
     
    searchPath[baseLen] = '\0';
    wprintf(L"%ls \n", BasePath);

    WIN32_FIND_DATA findFileData;
    HANDLE hFind = FindFirstFileW(searchPath, &findFileData);  
    
    if (hFind == INVALID_HANDLE_VALUE) {
        wprintf(L"Invalid path. %ls \n ...........", BasePath); 
        return;
    }

    do {
        if (findFileData.cFileName[0] == '.') //ignore things that start with a . as those may be neither files nor folders
            continue;  
        
        int nameLen = wcslen(findFileData.cFileName);
        WCHAR full_file_path[MAX_PATH] = { 0 }; //now create a path that contains the previous path + file/folder name. many folders add up
        memcpy(&full_file_path[0], BasePath, baseLen *2);//copy basePath 
        memcpy(&full_file_path[baseLen-3], &findFileData.cFileName[0], nameLen*2); //and combine it with fileName
        
        int is_regular_file = !(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);//check if the directoryFile bit is set by & with a checker
        if (is_regular_file) {
                
            int base_path_len = (int)wcslen(BaseFolderPathW) - 3;
            int partPathLen = (int)wcslen(full_file_path) - base_path_len; 

            FileToDistribute* file_information = malloc(sizeof(FileToDistribute));  
            file_information->rel_path = malloc(partPathLen*2);  

            memcpy(&(file_information->rel_path[0]), &full_file_path[base_path_len], partPathLen*2); 
            file_information->byte_count = (int)partPathLen*2; //null terminator is not part of the string info.
            file_information->creation_time = (((ULONGLONG)findFileData.ftLastWriteTime.dwHighDateTime) << 32) + findFileData.ftLastWriteTime.dwLowDateTime;
            file_information->fsize = (((ULONGLONG)findFileData.nFileSizeHigh) << 32) + findFileData.nFileSizeLow;
            file_information->handle = hFind;

            if (File_List->first == NULL)//if first and last are 0 we are adding the first element
            {
                File_List->first = file_information;
                File_List->last = file_information;
                File_List->Count = 0;
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
            size_t len_of_path = wcslen(full_file_path); 
            full_file_path[len_of_path] = '\\';
            full_file_path[len_of_path + 1] = '*';
            full_file_path[len_of_path + 2] = '.';
            full_file_path[len_of_path + 3] = '*';
            full_file_path[len_of_path + 4] = '\0';
            GetFilesInFolder(File_List, &full_file_path);
            //add //to the end and a 0 and call this method recursively 
        }
    } while (FindNextFileW(hFind, &findFileData) != 0);

    FindClose(hFind);
}