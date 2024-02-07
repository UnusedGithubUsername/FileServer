#include <stdio.h>
#include <winsock2.h>
#include <stdbool.h>
#include <assert.h>  
#include <timeapi.h>          
#include "folder_content.h"

#pragma comment(lib, "ws2_32.lib") 
#pragma comment(lib, "winmm.lib") //for timers that dont sleep() and block a thread

#define PORT 16502
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
    } fileDataPacket;
    char data[BUFFER_SIZE];
} DataUnion;

typedef struct { //we keep an array of those structs global
    int file_id;
    int packages_send;
    int bytes_sent;
    int filebytes_len;
    char* filebytes;
    char stream_isActive;

    int* fileIDs_requested;
    int current_fileindex_in_fileIDs_requested;
    int f_count;
}ActivelySentFile;

//functions
void BindSocket(SOCKET* socket);
void _stdcall SendFiles(UINT uTimerID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dw1, DWORD_PTR dw2);
void DisconnectClient(int index_of_socket, int errorVal);
int HandleClientReq(SOCKET skt, int index, int index_of_socket, int file_updatepackage_length, char* current_file_update);

DataUnion buffer;
int filecount = 0;
FileToDistribute* server_files;
HANDLE hMutex; 
SOCKET client_sockets[MAX_CLIENTS] = { 0 };
ActivelySentFile files_currently_sent[MAX_CLIENTS] = { 0 }; 
int id_counter = 1; //a guid, so that the client notices when the file he recieves switches

int main() {
    // Create a socket
    SOCKET server_socket;
    BindSocket(&server_socket);

    int file_updatepackage_length;
    char* current_file_update = CreateFileUpdatePackage(&file_updatepackage_length, &server_files, &filecount);

    //server_files = (FileToDistribute*)server_filesAdress;
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

            int errorVal = HandleClientReq(skt, index_of_socket, file_updatepackage_length, filecount, current_file_update);
            if (errorVal != 0)
                DisconnectClient(index_of_socket, errorVal);
        }
    }
    return 0;
}

int HandleClientReq(SOCKET skt, int i, int file_updatepackage_length, int filecount, char* current_file_update) {
    //TODO rename sending data buffer after closing debugger. pls dont forget in 2 seconds
    //TODO. CHECK IF WE HAVE ENOUGH DATA TO RECIEVE BEFORE RECIEVING IT !!!!: OTHERWISAE BIG BUGS
    int valread = recv(skt, &buffer, BUFFER_SIZE, MSG_PEEK);// valread says how much data was recieved 
    if (valread < 4) //valread 0 is dc, valread -1 is an error // we never need to handle a "too much data error" because this server only expects requests to retrieve files. Errors need to instantly disconnect
        return 1; //normal disconnect

    int packageType;
    memcpy(&packageType, &buffer, 4);
    int bytecount_package;

    switch (packageType)
    {
    case(1)://client requests a file updatePackage so he can check if his data is up to date 
        recv(skt, &buffer, BUFFER_SIZE, 0); //discard the buffer
        send(skt, current_file_update, file_updatepackage_length, 0);
        return 0;

    case(2)://client requests a certain number of files  
        memcpy(&bytecount_package, &buffer.data[4], 4);
        if (valread < bytecount_package)//if not all the data was recieved by the server
            return 0; //return until later so that all data may arrive

        valread = recv(skt, &buffer, BUFFER_SIZE, 0); //recieve data from the kernels network buffer into the Buffer.  
        memcpy(&files_currently_sent[i].f_count, &buffer.data[8], 4); // read the requested filecount

        if (files_currently_sent[i].f_count > filecount || filecount <= 0)
            return 2; //invalid filecount

        files_currently_sent[i].fileIDs_requested = malloc(files_currently_sent[i].f_count * sizeof(int));
        assert(files_currently_sent[i].fileIDs_requested != NULL);
        memcpy(files_currently_sent[i].fileIDs_requested, &buffer.data[12], files_currently_sent[i].f_count * 4);

        for (int j = 0; j < files_currently_sent[i].f_count; j++) //check requested files
        {
            if (files_currently_sent[i].fileIDs_requested[j] > filecount || files_currently_sent[i].fileIDs_requested[j] < 0)
                return 3; //invalid fileID
        }
        return 0;
    }
    return 4; // unknown packet type, (switchcase didnt catch any case)
}

void DisconnectClient(int index_of_socket, int errorVal)//Free the memory related to the client (outstanding files) and reset some things
{
    printf("Client disconnected with errorVal %i,  (1 = good, 2+ = problem)\n", errorVal);
    closesocket(client_sockets[index_of_socket]);
    files_currently_sent[index_of_socket].f_count = 0;
    files_currently_sent[index_of_socket].current_fileindex_in_fileIDs_requested = 0;

    if (files_currently_sent[index_of_socket].fileIDs_requested != NULL) //free the File_ids[] pointer and set it to 0
    {
        free(files_currently_sent[index_of_socket].fileIDs_requested);
        files_currently_sent[index_of_socket].fileIDs_requested = NULL;
    }

    client_sockets[index_of_socket] = 0;// mark socket spot as unused
    if (files_currently_sent[index_of_socket].stream_isActive)//if a file is currently mid sending, free the file
    {
        free(files_currently_sent[index_of_socket].filebytes);
        files_currently_sent[index_of_socket].stream_isActive = 0;
    } 
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

            if (client_sockets[i] == 0 || files_currently_sent[i].f_count <= files_currently_sent[i].current_fileindex_in_fileIDs_requested) //if no reciever or nothing to be recieved
                continue;

            int buffer_position = 0;
            if (files_currently_sent[i].stream_isActive == 0)
            {// we need to send a header
                int f_index = files_currently_sent[i].fileIDs_requested[files_currently_sent[i].current_fileindex_in_fileIDs_requested];

                char full_path[200] = { 0 };// combine the path parts into the full filepath
                memcpy(&full_path[0], BaseFolderPath, strlen(BaseFolderPath) - 3);//network that for the header file
                memcpy(&full_path[strlen(BaseFolderPath) - 3], server_files[f_index].rel_path, server_files[f_index].fname_len);

                files_currently_sent[i].filebytes = malloc(server_files[f_index].fsize); //create a buffer to hold the file in memory
                assert(files_currently_sent[i].filebytes != NULL);
                files_currently_sent[i].stream_isActive = 1;
                files_currently_sent[i].filebytes_len = server_files[f_index].fsize;
                files_currently_sent[i].bytes_sent = 0;
                files_currently_sent[i].packages_send = 0;

                printf("sending file: %s \n ", full_path);

                FILE* file;
                errno_t errorval = fopen_s(&file, full_path, "rb"); //r means read with newline translations. rb stands for read(binary)
                assert(file != NULL);
                int filebytes_read = fread(&files_currently_sent[i].filebytes[0], 1, server_files[f_index].fsize, file);
                fclose(file);

                //Create a header
                files_currently_sent[i].file_id = id_counter;
                id_counter++;//if we always count up, we never reuse a fileID

                //filenameLen
                int fNLen = server_files[f_index].fname_len;
                memcpy(&buffer.fileDataPacket.file_data[buffer_position], &fNLen, 4);
                buffer_position += 4;

                //filename
                memcpy(&buffer.fileDataPacket.file_data[buffer_position], &(server_files[f_index].rel_path[0]), server_files[f_index].fname_len);
                buffer_position += server_files[f_index].fname_len;

                //filesize
                memcpy(&buffer.fileDataPacket.file_data[buffer_position], &(server_files[f_index].fsize), 4);
                buffer_position += 4;

                //fileCreatingTime (long)
                memcpy(&buffer.fileDataPacket.file_data[buffer_position], &(server_files[f_index].creation_time), 8);
                buffer_position += 8;
            }

            int MAX_BYTECOUNT = 65536 - 12 - buffer_position; // TODO, say where this synce, and maybe update this

            //TODO, dont send entire file, only send part, filebytes_len needs replacement
            int bytes_left = files_currently_sent[i].filebytes_len - files_currently_sent[i].bytes_sent;
            int bytecount_we_send_this_client = MAX_BYTECOUNT < bytes_left ? MAX_BYTECOUNT : bytes_left; // send max number of bytes unless fewer are left to send

            //now the actual data
            memcpy(&buffer.fileDataPacket.file_data[buffer_position], &files_currently_sent[i].filebytes[files_currently_sent[i].bytes_sent], bytecount_we_send_this_client);
            buffer_position += bytecount_we_send_this_client;

            buffer.fileDataPacket.package_size = buffer_position - 4 + 12;// size not including the size int but including 3 fields
            buffer.fileDataPacket.package_number = files_currently_sent[i].packages_send;
            buffer.fileDataPacket.file_guid = files_currently_sent[i].file_id;

            files_currently_sent[i].packages_send++;

            send(client_sockets[i], buffer.data, buffer_position + 12, 0);//+16 so that we include the front 16 bytes (or rather dont cut out the last 16

            files_currently_sent[i].bytes_sent += bytecount_we_send_this_client;
            int filesend_finished = files_currently_sent[i].bytes_sent == files_currently_sent[i].filebytes_len;
            if (filesend_finished)
            {
                files_currently_sent[i].current_fileindex_in_fileIDs_requested++;

                free(files_currently_sent[0].filebytes);
                files_currently_sent[0].stream_isActive = 0;

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