#include <stdio.h>
#include <winsock2.h>
#include <stdbool.h>
#include <assert.h>
#include <fileapi.h>
#include <windows.h> 
#include <timeapi.h>    
#include <stdint.h>    

#pragma comment(lib, "ws2_32.lib") 
#pragma comment(lib, "winmm.lib") //for timers that dont sleep() and block a thread

#define PORT 16502
#define BUFFER_SIZE 65536
#define MAX_CLIENTS 30 
#define UPDATE_PERIOD 10//in milliseconds
#define MAX_BYTECOUNT_PER_PERIOD (12500*UPDATE_PERIOD)//how many bytes can i send per update()? I have a 100mbit internet connection
//thats 12.5 mbyte per second or 12.5 kbte per ms, but if i run update only every 10 ms i can buffer 10x the amount of data

//structs 
typedef union {
    struct {
        int package_size;
        int package_number;
        int file_guid;//  
        char file_data[BUFFER_SIZE - 12];
    } fields;
    char raw_data[BUFFER_SIZE];
} DataUnion;

typedef struct FileToDistribute {
    char* rel_path;
    int fname_len;
    UINT64 creation_time;
    ULONGLONG filesize;
    struct FileToDistribute* next;
} FileToDistribute;

typedef struct FileList {
    FileToDistribute* first;
    FileToDistribute* last;
    int Count;
}FileList;

typedef struct FileListNode {
    struct FileListNode* Next;
    int FileName;
}FileListNode;

typedef struct ActivelySentFile {
    int file_id;
    int packages_send;
    int bytes_sent;
    int filebytes_len;
    char* filebytes;
    char stream_isActive;

    FileListNode* First;
    FileListNode* Last;
}ActivelySentFile;

//functions
char* CreateFileUpdatePackage(int* bufferPosition);
void GetFilesInFolder(FileList* FileList, char* BasePath);
void BindSocket(SOCKET* socket);
void _stdcall SendFiles(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2);

void DisconnectClient(int index_of_socket);

//global variables
const char* BaseFolderPath = "C:\\Users\\Klauke\\Documents\\My Games\\Corivi\\LauncherServer\\*.*";
const char* BaseFolderPath2 = "F:\\Programme\\Heroes3 + Heroes3HotA\\Heroes 3 - HotA\\*.*";

DataUnion sending_data_buffer;
int filecount = 0;
FileToDistribute* server_files;
HANDLE hMutex;

SOCKET client_sockets[MAX_CLIENTS] = { 0 };
ActivelySentFile files_currently_sent[MAX_CLIENTS] = { 0 };


int num_of_files_sent = 1; //a guid, so that the client notices when the file he recieves switches

int main() {

    // Create a socket
    SOCKET server_socket;
    BindSocket(&server_socket);

    int file_updatepackage_length = 0;
    char* current_file_update = CreateFileUpdatePackage(&file_updatepackage_length);

    //now declare a different thread that sends long files over time
    TIMECAPS timeCaps;
    UINT resolution = 1; // 1 ms resolution
    timeGetDevCaps(&timeCaps, sizeof(TIMECAPS));
    if (resolution < timeCaps.wPeriodMin) {
        resolution = timeCaps.wPeriodMin;
    }

    hMutex = CreateMutex(NULL, FALSE, NULL);
    assert(hMutex != NULL);
    timeBeginPeriod(resolution);

    // Set up a multimedia timer
    timeSetEvent(UPDATE_PERIOD, 0, SendFiles, 0, TIME_PERIODIC);

    int connection_count = 0;
    while (1) {

        for (int i = 0; i < connection_count; i++)//clear empty positions, concentrate the array
        {
            if (client_sockets[i] < 1) {
                client_sockets[i] = client_sockets[connection_count - 1];//condense the array
                client_sockets[connection_count - 1] = 0;//set it to zero for debugging purporse. should never matter though
                connection_count--;
                i--;//we need to check the new value again
            }
        }
        fd_set readfds;//this initialized an array with a length of 64 (unless redefined with #define FD_SETSIZE xxx)
        readfds.fd_array[0] = server_socket; //this array describes which sockets are potentially ready to read
        readfds.fd_count = 1;//the struct has an int and an array. the int needs to be initialized to some value. 1 because we added the server

        for (int i = 0; i < connection_count; i++) {//add all clients to the array
            readfds.fd_array[readfds.fd_count] = client_sockets[i];
            readfds.fd_count++;
        }

        ReleaseMutex(hMutex);

        //EXECUTION IS BLOCKED UNTIL SELECT FINDS SOMETHING TO DO
        //select modifies the readfds and removes every socket that can NOT be read from because no data is available
        int activity_count = select(0, &readfds, NULL, NULL, NULL);//check which sockets have data to read
        WaitForSingleObject(hMutex, INFINITE);
        //now accept new connections if available
        bool server_data_available = readfds.fd_array[0] == server_socket; //if select did not remove the socket, it is active
        if (server_data_available && connection_count < MAX_CLIENTS) {//accept only if we dont handle way to many cons already

            SOCKET new_socket = accept(server_socket, NULL, NULL); //cautiously accept 1 connection, just to not issue to many blockign calls like accept()
            if ((int)new_socket < 0) {
                printf("accept() failed. closing program");
                exit(1);//unsucessful termination
            }

            printf("New connection , socket fd is %d at client_sockets[%d]\n", (int)new_socket, connection_count);
            client_sockets[connection_count] = new_socket;
            connection_count++;

            activity_count--;
            readfds.fd_array[0] = readfds.fd_array[activity_count]; //that server socket was handled, so remove it
            readfds.fd_array[activity_count] = 0;

        }

        //handle each client that has readops to do
        for (int i = 0; i < activity_count; i++) {//another area where counting to maxClients may be nonsense if i just countet all active conections

            SOCKET skt = readfds.fd_array[i];
            int index_of_socket = 0;

            for (int j = 0; j < connection_count; j++) //calc index of socket in case we need to set it to 0
            {
                if (skt == client_sockets[j])
                {
                    index_of_socket = j;
                }
            }
            //TODO rename sending data buffer after closing debugger. pls dont forget in 2 seconds
            //TODO. CHECK IF WE HAVE ENOUGH DATA TO RECIEVE BEFORE RECIEVING IT !!!!: OTHERWISAE BIG BUGS
            int attempted_values_read = recv(skt, &sending_data_buffer, BUFFER_SIZE, MSG_PEEK);


            if (attempted_values_read < 4) { //valread 0 is dc, valread -1 is an error // we never need to handle a "too much data error" because this server only expects requests to retrieve files. Errors need to instantly disconnect

                printf("Client disconnected \n");

                DisconnectClient(index_of_socket);
                continue;
            }

            int packageType;
            memcpy(&packageType, &sending_data_buffer, 4);

            switch (packageType)
            {
            case(1)://client requests a file updatePackage so he can check if his data is up to date
            {
                recv(skt, &sending_data_buffer, BUFFER_SIZE, 0); //discard the buffer
                send(skt, current_file_update, file_updatepackage_length, 0);
                break;
            }
            case(2)://client requests a certain number of files
            {
                int bytecount_package;
                memcpy(&bytecount_package, &sending_data_buffer.raw_data[4], 4);
                if (attempted_values_read < bytecount_package)
                {
                    break;
                }

                int valread = recv(skt, &sending_data_buffer, BUFFER_SIZE, 0); //recieve data from the kernels network buffer into the Buffer. valread says how much data was recieved

                char name[300] = { 0 };
                int number_of_files_requested;
                memcpy(&number_of_files_requested, &sending_data_buffer.raw_data[8], 4);
                printf("num of file %d\n", number_of_files_requested);
                int buffer_position = 12;

                for (int i = 0; i < number_of_files_requested; i++)
                {
                    //int fileArray[number_of_files_requested] = { 0 };
                    int filename_length;
                    memcpy(&filename_length, &sending_data_buffer.raw_data[buffer_position], 4);
                    buffer_position += 4;

                    if (filename_length < 0) {
                        break;
                    }

                    FileListNode* file_node = malloc(sizeof(struct FileListNode));
                    assert(file_node != NULL);
                    //file_node->FileName = malloc(filename_length + 1);
                    //assert(file_node->FileName != NULL);
                    file_node->Next = NULL;

                    //memcpy(&(file_node->FileName[0]), &sending_data_buffer.raw_data[buffer_position], filename_length);
                    //buffer_position += filename_length;
                    //file_node->FileName[filename_length] = 0;

                    file_node->FileName = filename_length;
                    if (files_currently_sent[index_of_socket].Last == NULL)//if first and last are 0 we are adding the first element
                    {
                        files_currently_sent[index_of_socket].First = file_node;
                        files_currently_sent[index_of_socket].Last = file_node;
                    }
                    else
                    {
                        files_currently_sent[index_of_socket].Last->Next = file_node;
                        files_currently_sent[index_of_socket].Last = file_node;
                    }

                    //printf("    %.*s \n", filename_length, &(file_node->FileName[0]));

                }
                break;
            }
            default: //if the client sends unexpected data, DC him
                DisconnectClient(index_of_socket);
                break;
            }
        }
    }

    return 0;
}

void DisconnectClient(int index_of_socket)//Free the memory related to the client (outstanding files) and reset some things
{
    closesocket(client_sockets[index_of_socket]);

    client_sockets[index_of_socket] = 0;// mark socket spot as unused
    if (files_currently_sent[index_of_socket].stream_isActive)//if a file is currently mid sending, free the file
    {
        free(files_currently_sent[index_of_socket].filebytes);
        files_currently_sent[index_of_socket].stream_isActive = 0;
    }
    while (files_currently_sent[index_of_socket].First != NULL) { //free() the list of filenames we would have sent
        //printf("    %s \n", (files_currently_sent[index_of_socket].First->FileName));
        FileListNode* next = files_currently_sent[index_of_socket].First->Next;
        //free(files_currently_sent[index_of_socket].First->FileName);
        free(files_currently_sent[index_of_socket].First);
        files_currently_sent[index_of_socket].First = next;
    }
    files_currently_sent[index_of_socket].Last = NULL;
}


void _stdcall SendFiles(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2) {
    DWORD result = WaitForSingleObject(hMutex, 0); // return 
    if (result == WAIT_TIMEOUT) {
        return;
    }

    //before the data there is NumOfPacketsSent, fileGuid, firstPackageHeader (maybe), data
    //stuff like the name does not have to be sent, because that is inside the data
    int bytes_sent_this_Cycle = 0;
    bool files_remaining = false;

    do {
        for (size_t i = 0; i < MAX_CLIENTS; i++)
        {
            if ((bytes_sent_this_Cycle / MAX_BYTECOUNT_PER_PERIOD) > 0.9f)
                break;

            if (client_sockets[i] == 0 || files_currently_sent[i].First == NULL) //if no reciever or nothing to be recieved
                continue;

            int buffer_position = 0;
            if (files_currently_sent[i].stream_isActive == 0)
            {// we need to send a header
                int f_index = files_currently_sent[i].First->FileName;//-1;
                //for (size_t j = 0; j < filecount; j++)
                //{
                //    int fname_len_given_by_client = strlen(files_currently_sent[i].First->FileName);
                //    if (server_files[j].fname_len != fname_len_given_by_client) //if filenames are equally long
                //    {//filenames have diff len, so they are not the same string
                //        continue;
                //    }
                //
                //    //if strings are equal
                //    if (0 == memcmp(files_currently_sent[i].First->FileName, server_files[j].rel_path, server_files[j].fname_len))
                //    {
                //        f_index = j;
                //        break;
                //    }
                //}
                //if (f_index == -1) {
                //    printf("%s\n", "FILE WAS NOT FOUND INSIDE THE FILEARRAY");
                //    continue;
                //}

                char full_path[200] = { 0 };// combine the path parts into the full filepath
                memcpy(&full_path[0], BaseFolderPath, strlen(BaseFolderPath) - 3);
                memcpy(&full_path[strlen(BaseFolderPath) - 3], server_files[f_index].rel_path, server_files[f_index].fname_len);

                printf(full_path);
                printf("\n");

                files_currently_sent[i].filebytes = malloc(server_files[f_index].filesize); //create a buffer to hold the file in memory
                assert(files_currently_sent[i].filebytes != NULL);
                files_currently_sent[i].stream_isActive = 1;
                files_currently_sent[i].filebytes_len = server_files[f_index].filesize;
                files_currently_sent[i].bytes_sent = 0;
                files_currently_sent[i].packages_send = 0;

                printf("sending next file.\n");

                FILE* file;
                errno_t errorval = fopen_s(&file, full_path, "rb"); //r means read with newline translations. rb stands for read(binary)
                assert(file != NULL);
                int filebytes_read = fread(&files_currently_sent[i].filebytes[0], 1, server_files[f_index].filesize, file);
                fclose(file);



                //file has packageLen,
                // then packageType = 4
                //ok we now have the file.
                //now create the header and send
                //header:

                //before the data there is NumOfPacketsSent, fileGuid, firstPackageHeader (maybe), data
                //stuff like the name does not have to be sent, because that is inside the data

                //totalCombinedPacket length


                //Create a header
                files_currently_sent[i].file_id = num_of_files_sent;
                num_of_files_sent++;



                //filenameLen
                int fNLen = server_files[f_index].fname_len;
                memcpy(&sending_data_buffer.fields.file_data[buffer_position], &fNLen, 4);
                buffer_position += 4;

                //filename
                memcpy(&sending_data_buffer.fields.file_data[buffer_position], &(server_files[f_index].rel_path[0]), server_files[f_index].fname_len);
                buffer_position += server_files[f_index].fname_len;

                //filesize
                memcpy(&sending_data_buffer.fields.file_data[buffer_position], &(server_files[f_index].filesize), 4);
                buffer_position += 4;

                //fileCreatingTime (long)
                memcpy(&sending_data_buffer.fields.file_data[buffer_position], &(server_files[f_index].creation_time), 8);
                buffer_position += 8;
            }

            int MAX_BYTECOUNT = 65536 - 12 - buffer_position; // TODO, say where this synce, and maybe update this

            //TODO, dont send entire file, only send part, filebytes_len needs replacement
            int bytes_left = files_currently_sent[i].filebytes_len - files_currently_sent[i].bytes_sent;
            int bytecount_we_send_this_client = MAX_BYTECOUNT < bytes_left ? MAX_BYTECOUNT : bytes_left; // send max number of bytes unless fewer are left to send

            //now the actual data
            memcpy(&sending_data_buffer.fields.file_data[buffer_position], &files_currently_sent[i].filebytes[files_currently_sent[i].bytes_sent], bytecount_we_send_this_client);
            buffer_position += bytecount_we_send_this_client;

            sending_data_buffer.fields.package_size = buffer_position - 4 + 12;// size not including the size int but including 3 fields
            sending_data_buffer.fields.package_number = files_currently_sent[i].packages_send;
            sending_data_buffer.fields.file_guid = files_currently_sent[i].file_id;

            files_currently_sent[i].packages_send++;

            send(client_sockets[i], sending_data_buffer.raw_data, buffer_position + 12, 0);//+16 so that we include the front 16 bytes (or rather dont cut out the last 16

            files_currently_sent[i].bytes_sent += bytecount_we_send_this_client;
            int filesend_finished = files_currently_sent[i].bytes_sent == files_currently_sent[i].filebytes_len;
            if (filesend_finished)
            {
                free(files_currently_sent[0].filebytes);
                files_currently_sent[0].stream_isActive = 0;

                FileListNode* finished_file = files_currently_sent[i].First;
                files_currently_sent[i].First = files_currently_sent[i].First->Next;
                //free(finished_file->FileName);
                free(finished_file);
                if (files_currently_sent[i].First == NULL)
                {
                    files_currently_sent[i].Last = NULL;
                }
            }
            else //file is left unfinished
            {
                files_remaining = true;
            }

            bytes_sent_this_Cycle += bytecount_we_send_this_client;

        }
    } while ((bytes_sent_this_Cycle / MAX_BYTECOUNT_PER_PERIOD) > 0.5f && files_remaining && false);

    ReleaseMutex(hMutex);
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

        bool is_regular_file = !(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);//check if the directoryFile bit is set by & with a checker
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
            file_information->filesize = (((ULONGLONG)fileInfo.nFileSizeHigh) << 32) + fileInfo.nFileSizeLow;

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

char* CreateFileUpdatePackage(int* bufferPosition) {
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

void BindSocket(SOCKET* server_socket) {

    WSADATA wsa; //Write return data to this value,  //latest version is 2.2, so request to use that version of sockets
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Failed to start Server");
        return;
    }

    *server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (*server_socket == INVALID_SOCKET) {
        printf("Could not create socket : %d", WSAGetLastError());
    }
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(PORT);

    if (bind(*server_socket, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
        printf("Bind failed with error code : %d", WSAGetLastError());
        exit(EXIT_FAILURE);
    }

    if (listen(*server_socket, 32) == SOCKET_ERROR) { //32 means 32 incoming connectins can queue up
        printf("Listen failed with error code : %d", WSAGetLastError());
        exit(EXIT_FAILURE);
    }
}