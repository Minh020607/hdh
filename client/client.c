#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <time.h>
#include <errno.h>  
#include <limits.h>
#include <sys/select.h>

#define PORT 8080
#define BUFFER_SIZE MAX_PATH
#define MAX_PATH    PATH_MAX
#define TRUE    1
#define FALSE   0
#define INOTIFY_BUFFER_SIZE (1024 * (sizeof(struct inotify_event) + 16))

#define S(x) #x
#define S_(x) S(x)
#define PATH_MAX_STR S_(PATH_MAX)

uint8_t folder_exist = TRUE;
uint8_t file_exist = TRUE;
uint8_t file_no_change = TRUE;
uint8_t receive_folder_done = FALSE;

typedef struct {
    char filename[PATH_MAX];    
    char filepath[PATH_MAX];        
    long filesize;             
    unsigned char hash[16];
    time_t timestamp;           
} FileInfo;

typedef struct {
    uint32_t cookie;
    int wd;
    char old_relative_path[PATH_MAX];
    char old_full_path[PATH_MAX];
    uint8_t is_dir;
    uint8_t active;
} PendingMoveEvent;

PendingMoveEvent pending_move = {0}; // Global variable

int receive_response(int client_fd);
int send_response(int client_socket, const char *message);
int receive_file_info(int client_fd, FileInfo *file_info);
int receive_file(int socket_fd, const char *base_path);
int send_file(int socket_fd, const char *path_file, const char *file_path);

int create_directory_recursively(const char *path) {
    char temp_path[512];
    char *p = NULL;
    size_t len;

    snprintf(temp_path, sizeof(temp_path), "%s", path);

    len = strlen(temp_path);
    if (temp_path[len - 1] == '/') {
        temp_path[len - 1] = '\0';
    }

    // Tạo từng thư mục một
    for (p = temp_path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0'; // Tạm thời thay '/' thành '\0' để tạo thư mục
            if (mkdir(temp_path, 0777) == -1 && errno != EEXIST) {
                perror("Lỗi tạo thư mục");
                return -1;
            }
            *p = '/'; // Khôi phục lại dấu '/'
        }
    }

    // Tạo thư mục cuối cùng
    if (mkdir(temp_path, 0777) == -1 && errno != EEXIST) {
        perror("Lỗi tạo thư mục");
        return -1;
    }

    return 0;
}

int receive_file_info(int client_fd, FileInfo *file_info) {
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) {
        if (recv(client_fd, buffer, sizeof(buffer), 0) == 0) printf("Server disconnected during file path reception.\n");
        else perror("Error receiving file path");
        return -1;
    }
    strncpy(file_info->filepath, buffer, sizeof(file_info->filepath) - 1);
    file_info->filepath[sizeof(file_info->filepath) - 1] = '\0';
    printf("Client: Received full relative file path: %s\n", file_info->filepath);
    if (send_response(client_fd, "Full file path received") == -1) return -1;


    const char *last_slash = strrchr(file_info->filepath, '/');
    if (last_slash) {
        strncpy(file_info->filename, last_slash + 1, sizeof(file_info->filename) - 1);
    } else {
        strncpy(file_info->filename, file_info->filepath, sizeof(file_info->filename) - 1);
    }
    file_info->filename[sizeof(file_info->filename) - 1] = '\0';
    printf("Client: Extracted filename: %s\n", file_info->filename);


    long network_file_size;
    if (recv(client_fd, &network_file_size, sizeof(network_file_size), 0) <= 0) {
        if (recv(client_fd, &network_file_size, sizeof(network_file_size), 0) == 0) printf("Server disconnected during file size reception.\n");
        else perror("Error receiving file size");
        return -1;
    }
    file_info->filesize = ntohl(network_file_size);
    printf("Client: Received filesize: %ld bytes\n", file_info->filesize);
    if (send_response(client_fd, "Filesize received") == -1) return -1;

    // Receive hash
    if (recv(client_fd, file_info->hash, sizeof(file_info->hash), 0) <= 0) {
        if (recv(client_fd, file_info->hash, sizeof(file_info->hash), 0) == 0) printf("Server disconnected during hash reception.\n");
        else perror("Error receiving hash");
        return -1;
    }
    printf("Client: Received hash (raw bytes)\n");
    if (send_response(client_fd, "Hash received") == -1) return -1;

    long network_timestamp;
    if (recv(client_fd, &network_timestamp, sizeof(network_timestamp), 0) <= 0) {
        if (recv(client_fd, &network_timestamp, sizeof(network_timestamp), 0) == 0) printf("Server disconnected during timestamp reception.\n");
        else perror("Error receiving timestamp");
        return -1;
    }
    file_info->timestamp = (time_t)ntohl(network_timestamp);
    printf("Client: Received timestamp: %ld\n", (long)file_info->timestamp);
    if (send_response(client_fd, "Timestamp received") == -1) return -1;

    return 0;
}

void receive_file_count(int client_fd, int *file_count) {
    int network_file_count;

    if (recv(client_fd, &network_file_count, sizeof(network_file_count), 0) > 0) {
        // Chuyển đổi thứ tự byte từ network byte order sang host byte order
        *file_count = ntohl(network_file_count);
        printf("Nhận file_count: %d\n", *file_count);
    } else {
        perror("Nhận file_count thất bại");
    }
}

int receive_file(int socket_fd, const char *base_path) {
    char path_buffer[MAX_PATH];

    // Nhận đường dẫn tương đối đầy đủ từ server (bao gồm cả tên file)
    ssize_t path_len = recv(socket_fd, path_buffer, sizeof(path_buffer) - 1, 0);

    if (path_len <= 0) {
        if (path_len == 0) printf("Server disconnected during path reception.\n");
        else perror("Lỗi nhận đường dẫn");
        return -1;
    }
    path_buffer[path_len] = '\0';

    printf("Client: Received relative file path for file content: %s\n", path_buffer);

    // Kiểm tra tín hiệu kết thúc
    if (strcmp(path_buffer, "END_OF_TRANSFER\n") == 0) {
        receive_folder_done = TRUE;
        return 0;
    }

    if (send_response(socket_fd, "Receive path successful") == -1) { return -1; }

    long file_size;
    long network_file_size;
    if (recv(socket_fd, &network_file_size, sizeof(network_file_size), 0) <= 0) {
        if (recv(socket_fd, &network_file_size, sizeof(network_file_size), 0) == 0) printf("Server disconnected during file size reception.\n");
        else perror("Lỗi nhận kích thước file");
        return -1;
    }
    file_size = ntohl(network_file_size);
    printf("Client: Received filesize for file content: %ld bytes\n", file_size);

    if (send_response(socket_fd, "Receive file size ok!") == -1) { return -1; }

    // Tạo đường dẫn đầy đủ cho file (kết hợp base_path của client với đường dẫn tương đối nhận được)
    char full_file_path[PATH_MAX];
    snprintf(full_file_path, sizeof(full_file_path), "%s/%s", base_path, path_buffer);
    printf("Client: Full file path to write to: %s\n", full_file_path);

    // Đảm bảo thư mục đích tồn tại
    char target_directory_for_file[PATH_MAX];
    strncpy(target_directory_for_file, full_file_path, sizeof(target_directory_for_file) - 1);
    target_directory_for_file[sizeof(target_directory_for_file) - 1] = '\0';

    char *last_slash = strrchr(target_directory_for_file, '/');
    if (last_slash != NULL) {
        *last_slash = '\0'; // Cắt bỏ tên file để có đường dẫn thư mục
    } else {
        target_directory_for_file[0] = '\0'; // Nếu không có dấu gạch chéo, không có thư mục cha
    }

    if (strlen(target_directory_for_file) > 0 && create_directory_recursively(target_directory_for_file) == -1) {
        printf("Client: Có lỗi khi tạo thư mục cho file.\n");
        return -1;
    }
    printf("Client: Directory created/exists: %s\n", target_directory_for_file);

    // Mở file để ghi dữ liệu nhận được
    FILE *file = fopen(full_file_path, "wb");
    if (!file) {
        perror("Client: Lỗi mở file để ghi");
        return -1;
    }
    printf("Client: Receiving file content for: %s\n", full_file_path);

    char buffer2[BUFFER_SIZE];
    long total_received = 0;
    
    while (total_received < file_size) {
        ssize_t bytes_received_data = recv(socket_fd, buffer2, sizeof(buffer2), 0);
        if (bytes_received_data <= 0) {
            if (bytes_received_data == 0) printf("Server disconnected during file data reception.\n");
            else perror("Client: Lỗi nhận dữ liệu file");
            fclose(file);
            return -1;
        }
        fwrite(buffer2, 1, bytes_received_data, file);
        total_received += bytes_received_data;
    }

    fclose(file);
    printf("Client: File '%s' received successfully!\n", full_file_path);
    return 0;
}

void receive_and_print_tree(int sock) {
    char buffer[2048];

    while (1) {
        memset(buffer, 0, sizeof(buffer)); // Xóa nội dung buffer
        int bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0);

        buffer[bytes_read] = '\0'; // Đảm bảo chuỗi kết thúc

        // Kiểm tra tín hiệu kết thúc
        if (strstr(buffer, "END_OF_TREE") != NULL) {
            break;
        }

        printf("%s", buffer); // Hiển thị dữ liệu

        if (bytes_read <= 0) {
            // Kết thúc khi không còn dữ liệu
            break;
        }
    }
}

int check_directory_exists(const char *dir_path) {
    struct stat info;

    // Kiểm tra xem thư mục có tồn tại hay không
    if (stat(dir_path, &info) != 0) {
        // Thư mục không tồn tại hoặc không thể truy cập
        return 0;
    } else if (S_ISDIR(info.st_mode)) {
        // Đây là một thư mục
        return 1;
    } else {
        // Đây không phải là thư mục (có thể là file thông thường)
        return 0;
    }
}

int check_path_exists(const char *base_path, const char *relative_path) {
    char full_path[PATH_MAX];
    struct stat path_stat;

    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, relative_path);

    if (stat(full_path, &path_stat) == 0) {
        return 1;
    } else {
        perror("Lỗi kiểm tra đường dẫn");
        return 0;
    }
}

time_t get_file_timestamp(const char *filename) {
    struct stat file_info;

    // Lấy thông tin về file
    if (stat(filename, &file_info) == 0) {
        return file_info.st_mtime;
    } else {
        perror("Không thể lấy thông tin file");
        return -1;
    }
}

void send_file_count(int client_fd, int file_count) {
    int network_file_count = htonl(file_count);

    if (send(client_fd, &network_file_count, sizeof(network_file_count), 0) < 0) {
        perror("Gửi file_count thất bại");
    } else {
        printf("Đã gửi file_count: %d\n", file_count);
    }
}

void get_different_path(const char *base_path, const char *full_path, char *result) {
    size_t base_len = strlen(base_path);
    size_t full_len = strlen(full_path);

    if (strncmp(full_path, base_path, base_len) != 0) {
        result[0] = '\0';
        return;
    }

    if (full_len == base_len) {
        result[0] = '\0';
        return;
    }

    // Xử lý dấu gạch chéo cuối cùng của base_path
    const char *path_after_base = full_path + base_len;
    if (*path_after_base == '/') {
        path_after_base++;
    }
    
    strncpy(result, path_after_base, PATH_MAX - 1);
    result[PATH_MAX - 1] = '\0';
}

int send_file(int socket_fd, const char *path_file, const char *file_path) {
    printf("path file: %s\n", path_file);
    FILE *file = fopen(path_file, "rb");
    if (!file) {
        perror("Lỗi mở file");
        return -1;
    }

    const char *file_name = strrchr(path_file, '/');
    if (file_name) {
        file_name++; // Bỏ qua dấu '/' để lấy tên file
    } else {
        file_name = path_file;
    }

    // Gửi đường dẫn thư mục tương đối
    printf("Client: About to send relative file path: '%s' (length: %zu)\n", file_path, strlen(file_path)); fflush(stdout);
    if (send(socket_fd, file_path, strlen(file_path) + 1, 0) == -1) {
        perror("Lỗi gửi đường dẫn file");
        fclose(file);
        return -1;
    }
    printf("Client: Sent relative file path. Waiting for server response.\n"); fflush(stdout);
    if (receive_response(socket_fd) == -1) {
        fclose(file);
        return -1;
    }

    // Tính toán và gửi kích thước file
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    printf("Client: Kích thước file: %ld bytes\n", file_size);
    printf("Client: sizeof(long) = %zu\n", sizeof(long));
    long network_file_size = htonl(file_size); // Convert to network byte order
    if (send(socket_fd, &network_file_size, sizeof(network_file_size), 0) < 0) {
        perror("Lỗi gửi kích thước file");
        fclose(file);
        return -1;
    }
    printf("Client: Sent file size.\n"); fflush(stdout);
    if (receive_response(socket_fd) == -1) {
        fclose(file);
        return -1;
    }

    // Gửi nội dung file
    char buffer2[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = fread(buffer2, 1, BUFFER_SIZE, file)) > 0) {
        if (send(socket_fd, buffer2, bytes_read, 0) < 0) {
            perror("Failed to send file");
            fclose(file);
            return -1;
        }
    }
    fclose(file);
    printf("Client: File đã được gửi thành công.\n"); fflush(stdout);
    return 0;
}

int calculate_file_hash(const char *filepath, unsigned char *hash_out) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("Không thể mở tệp tin");
        return -1;
    }

    SHA256_CTX sha256Context;
    unsigned char data[1024];
    size_t bytesRead;

    SHA256_Init(&sha256Context);
    
    while ((bytesRead = fread(data, 1, sizeof(data), file)) > 0) {
        SHA256_Update(&sha256Context, data, bytesRead);
    }

    SHA256_Final(hash_out, &sha256Context);

    fclose(file);
    return 0;
}

int create_client_socket() {
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    return client_fd;
}

void connect_to_server(int client_fd, struct sockaddr_in *server_addr) {
    if (connect(client_fd, (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0) {
        perror("Connection failed");
        close(client_fd);
        exit(EXIT_FAILURE);
    }
}

void send_request(int client_fd, const char *message) {
    send(client_fd, message, strlen(message), 0);
}

int send_response(int client_socket, const char *message) {
    if (send(client_socket, message, strlen(message) + 1, 0) < 0) {
        perror("Failed to send response");
        return -1;
    } else {
        printf("Response sent to server: %s\n", message);
    }
    return 0;
}

int receive_response(int client_fd) {
    char buffer[BUFFER_SIZE];
    int bytes_received;
    
    bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            printf("Server disconnected.\n");
        } else {
            perror("Error receiving data");
        }
        return -1;
    }
    buffer[bytes_received] = '\0';

    char *token;
    char *rest = buffer;
    int processed_any_response = 0;

    while ((token = strtok_r(rest, "\0", &rest)) != NULL) {
        printf("Received from server: %s\n", token);
        processed_any_response = 1;

        if (strcmp(token, "folder no exists") == 0) {
            folder_exist = FALSE;
        }
        else if(strcmp(token, "folder exists") == 0){
            folder_exist = TRUE;
        }
        else if(strcmp(token, "File no exist") == 0)
        {
            printf("Server_File no exist\n");
            file_exist = FALSE;
        }
        else if(strcmp(token, "File exist") == 0)
        {
            printf("Server_File exist\n");
            file_exist = TRUE;
        }
        else if(strcmp(token, "File change") == 0)
        {
            printf("Server_File change!\n");
            file_no_change = FALSE;
        }
        else if(strcmp(token, "File no change") == 0)
        {
            printf("Server_File no change!\n");
            file_no_change = TRUE;
        }
        else if(strcmp(token, "Receive END_OF_TRANSFER successful") == 0)
        {
            printf("Client: Received END_OF_TRANSFER successful.\n");
        }
        else if(strcmp(token, "Command received") == 0)
        {
            printf("Client: Received Command received confirmation.\n");
        }
        else if(strcmp(token, "Receive path successful") == 0)
        {
            printf("Client: Received Receive path successful confirmation.\n");
        }
        else if(strcmp(token, "receive file name successful") == 0)
        {
            printf("Client: Received file name successful confirmation.\n");
        }
        else if(strcmp(token, "Receive file size ok!") == 0)
        {
            printf("Client: Received file size ok! confirmation.\n");
        }
        else if(strcmp(token, "File received successfully.") == 0)
        {
            printf("Client: Received file received successfully confirmation.\n");
        }
    }

    if (!processed_any_response) {
        printf("Client: No recognizable response processed from server.\n");
        return -1;
    }

    return 0;
}

void list_files(const char *dir_path, FileInfo *file_list, int *file_count) {
    DIR *dir = opendir(dir_path);
    struct dirent *entry;

    if (dir == NULL) {
        perror("Không thể mở thư mục");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char full_path[PATH_MAX];
        
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat statbuf;
        if (stat(full_path, &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                // Nếu là thư mục, gọi đệ quy
                list_files(full_path, file_list, file_count); 
            } else {
                // Nếu là tệp tin, lưu thông tin tệp tin vào mảng
                strncpy(file_list[*file_count].filename, entry->d_name, sizeof(file_list[*file_count].filename) - 1);
                file_list[*file_count].filename[sizeof(file_list[*file_count].filename) - 1] = '\0';
                strncpy(file_list[*file_count].filepath, full_path, sizeof(file_list[*file_count].filepath) - 1);
                file_list[*file_count].filepath[sizeof(file_list[*file_count].filepath) - 1] = '\0';
                file_list[*file_count].filesize = statbuf.st_size;

                // Tính hash cho tệp tin
                if (calculate_file_hash(full_path, file_list[*file_count].hash) == 0) {
                    file_list[*file_count].timestamp = get_file_timestamp(full_path);
                    (*file_count)++;
                }
            }
        }
    }

    closedir(dir);
}

int send_file_info(int client_fd, FileInfo *file_info) {
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    // Gửi đường dẫn file (đã chứa tên file)
    snprintf(buffer, sizeof(buffer), "%s", file_info->filepath);
    if (send(client_fd, buffer, strlen(buffer) + 1, 0) < 0) { return -1; }
    printf("Đã gửi đường dẫn đầy đủ: %s\n", file_info->filepath);

    // Đợi phản hồi từ server
    char response[BUFFER_SIZE];
    memset(response, 0, sizeof(response));
    if (recv(client_fd, response, sizeof(response), 0) <= 0) { return -1; }
    printf("Phản hồi từ server: %s\n", response);

    // Gửi kích thước file
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%ld", file_info->filesize);
    if (send(client_fd, buffer, strlen(buffer) + 1, 0) < 0) { return -1; }
    printf("Đã gửi kích thước file: %ld\n", file_info->filesize);

    // Đợi phản hồi từ server
    memset(response, 0, sizeof(response));
    if (recv(client_fd, response, sizeof(response), 0) <= 0) { return -1; }
    printf("Phản hồi từ server: %s\n", response);

    // Gửi hash của file dưới dạng hex
    if (send(client_fd, file_info->hash, sizeof(file_info->hash), 0) < 0) { return -1; }
    printf("Đã gửi hash (raw bytes)\n");

    // Đợi phản hồi từ server sau khi gửi hết hash
    memset(response, 0, sizeof(response));
    if (recv(client_fd, response, sizeof(response), 0) <= 0) { return -1; }
    printf("Phản hồi từ server: %s\n", response);

    // Gửi timestamp
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%ld", (long)file_info->timestamp);
    if (send(client_fd, buffer, strlen(buffer) + 1, 0) < 0) { return -1; }
    printf("Đã gửi timestamp: %ld\n", (long)file_info->timestamp);

    // Đợi phản hồi từ server sau khi gửi timestamp
    memset(response, 0, sizeof(response));
    if (recv(client_fd, response, sizeof(response), 0) <= 0) { return -1; }
    printf("Phản hồi từ server: %s\n", response);
    return 0;
}

void add_watch_recursive(int inotify_fd, const char *path, int *wd_map, int *wd_count, char **wd_paths) {
    DIR *dir = opendir(path);
    if (!dir) {
        perror("Không thể mở thư mục");
        return;
    }

    int wd = inotify_add_watch(inotify_fd, path, IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE);
    if (wd == -1) {
        perror("Không thể thêm watch");
        closedir(dir);
        return;
    }
    printf("Added watch for path: %s, wd: %d\n", path, wd);
    fflush(stdout);
    wd_map[*wd_count] = wd;
    wd_paths[*wd_count] = strdup(path);
    (*wd_count)++;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        struct stat statbuf;
        if (stat(full_path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
            add_watch_recursive(inotify_fd, full_path, wd_map, wd_count, wd_paths);
        }
    }
    closedir(dir);
}

void handle_inotify_event(int client_fd, int inotify_fd, const char *base_path, const char *dest_path_unused_param, int *wd_map, int *wd_count, char **wd_paths) {
    char buffer[INOTIFY_BUFFER_SIZE];
    ssize_t len = read(inotify_fd, buffer, INOTIFY_BUFFER_SIZE);
    printf("Read from inotify: len=%zd\n", len);
    fflush(stdout);
    if (len < 0) {
        perror("Error reading inotify events");
        return;
    }

    char command[16];
    for (char *p = buffer; p < buffer + len;) {
        struct inotify_event *event = (struct inotify_event *)p;
        printf("  Event name: %s, mask: %x\n", event->name, event->mask);
        fflush(stdout);
        char full_path[PATH_MAX];
        char relative_path[PATH_MAX];

        for (int i = 0; i < *wd_count; i++) {
            if (wd_map[i] == event->wd) {
                snprintf(full_path, sizeof(full_path), "%s/%s", wd_paths[i], event->name);
                get_different_path(base_path, full_path, relative_path);
                break;
            }
        }

        if (event->mask & IN_MOVED_FROM) {
            printf("IN_MOVED_FROM: %s\n", full_path);
            // If there's an active pending move, and it's not related to this new event (different cookie),
            // then the previous pending move must have been a move-out. Process it.
            if (pending_move.active && pending_move.cookie != event->cookie) {
                printf("Processing previous IN_MOVED_FROM as DELETE (move-out): %s\n", pending_move.old_relative_path);
                char cmd_out[16];
                if (pending_move.is_dir) strcpy(cmd_out, "DELETE_DIR");
                else strcpy(cmd_out, "DELETE_FILE");

                if (send(client_fd, cmd_out, strlen(cmd_out), 0) == -1) return;
                if (receive_response(client_fd) == -1) return;
                if (send(client_fd, pending_move.old_relative_path, strlen(pending_move.old_relative_path) + 1, 0) == -1) return;
                if (receive_response(client_fd) == -1) return;
                memset(&pending_move, 0, sizeof(PendingMoveEvent)); // Clear after processing
            }

            // Store current IN_MOVED_FROM event
            pending_move.cookie = event->cookie;
            pending_move.wd = event->wd;
            strncpy(pending_move.old_relative_path, relative_path, sizeof(pending_move.old_relative_path) - 1);
            pending_move.old_relative_path[sizeof(pending_move.old_relative_path) - 1] = '\0';
            strncpy(pending_move.old_full_path, full_path, sizeof(pending_move.old_full_path) - 1);
            pending_move.old_full_path[sizeof(pending_move.old_full_path) - 1] = '\0';
            pending_move.is_dir = (event->mask & IN_ISDIR) ? 1 : 0;
            pending_move.active = 1;

        } else if (event->mask & IN_MOVED_TO) {
            printf("IN_MOVED_TO: %s\n", full_path);
            if (pending_move.active && pending_move.cookie == event->cookie) {
                // This is a rename operation
                printf("RENAME detected: %s -> %s\n", pending_move.old_relative_path, relative_path);
                strcpy(command, "RENAME");
                if (send(client_fd, command, strlen(command), 0) == -1) return;
                if (receive_response(client_fd) == -1) return;

                // Send old path
                if (send(client_fd, pending_move.old_relative_path, strlen(pending_move.old_relative_path) + 1, 0) == -1) return;
                if (receive_response(client_fd) == -1) return;

                // Send new path
                if (send(client_fd, relative_path, strlen(relative_path) + 1, 0) == -1) return;
                if (receive_response(client_fd) == -1) return;

                // If a directory was renamed, update its mapped path
                if (pending_move.is_dir) {
                    for (int i = 0; i < *wd_count; i++) {
                        if (wd_map[i] == pending_move.wd) {
                            free(wd_paths[i]);
                            wd_paths[i] = strdup(full_path);
                            break;
                        }
                    }
                }
                memset(&pending_move, 0, sizeof(PendingMoveEvent)); // Clear after rename
            } else {
                // This is a move-in from outside. Treat as create.
                printf("IN_MOVED_TO (from outside): %s\n", full_path);
                struct stat statbuf;
                if (stat(full_path, &statbuf) == 0) {
                    if (S_ISDIR(statbuf.st_mode)) {
                        add_watch_recursive(inotify_fd, full_path, wd_map, wd_count, wd_paths);
                        strcpy(command, "CREATE_DIR");
                        if (send(client_fd, command, strlen(command), 0) == -1) return;
                        if (receive_response(client_fd) == -1) return;
                        if (send(client_fd, relative_path, strlen(relative_path) + 1, 0) == -1) return;
                        if (receive_response(client_fd) == -1) return;
                    } else {
                        strcpy(command, "CREATE_FILE");
                        if (send(client_fd, command, strlen(command), 0) == -1) return;
                        if (receive_response(client_fd) == -1) return;
                        if (send_file(client_fd, full_path, relative_path) == -1) return;
                        if (receive_response(client_fd) == -1) return;
                    }
                } else {
                    perror("Error stat-ing moved-to file");
                }
                // Clear pending_move if it was an unrelated move-in.
                memset(&pending_move, 0, sizeof(PendingMoveEvent));
            }
        } else if (event->mask & IN_CREATE) {
            printf("IN_CREATE: %s\n", full_path);
            struct stat statbuf;
            if (stat(full_path, &statbuf) == 0) {
                if (S_ISDIR(statbuf.st_mode)) {
                    add_watch_recursive(inotify_fd, full_path, wd_map, wd_count, wd_paths);
                    strcpy(command, "CREATE_DIR");
                    if (send(client_fd, command, strlen(command), 0) == -1) return;
                    if (receive_response(client_fd) == -1) return;
                    if (send(client_fd, relative_path, strlen(relative_path) + 1, 0) == -1) return;
                    if (receive_response(client_fd) == -1) return;
                } else {
                    strcpy(command, "CREATE_FILE");
                    if (send(client_fd, command, strlen(command), 0) == -1) return;
                    if (receive_response(client_fd) == -1) return;
                    if (send_file(client_fd, full_path, relative_path) == -1) return;
                    if (receive_response(client_fd) == -1) return;
                }
            } else {
                perror("Error stat-ing created file");
            }
            // Clear any pending move-from, as this is a new create.
            memset(&pending_move, 0, sizeof(PendingMoveEvent));
        } else if (event->mask & IN_MODIFY) {
            printf("IN_MODIFY: %s\n", full_path);
            strcpy(command, "MODIFY_FILE");
            if (send(client_fd, command, strlen(command), 0) == -1) return;
            if (receive_response(client_fd) == -1) return;
            if (send_file(client_fd, full_path, relative_path) == -1) return;
            if (receive_response(client_fd) == -1) return;
            // Clear any pending move-from, as this is a modify.
            memset(&pending_move, 0, sizeof(PendingMoveEvent));
        } else if (event->mask & IN_DELETE) {
            printf("IN_DELETE: %s\n", full_path);
            // If there's a pending move related to this path, clear it.
            // This scenario means an actual delete happened, not a move.
            if (pending_move.active && strcmp(pending_move.old_relative_path, relative_path) == 0) {
                 memset(&pending_move, 0, sizeof(PendingMoveEvent));
            }

            if (event->mask & IN_ISDIR) {
                strcpy(command, "DELETE_DIR");
                if (send(client_fd, command, strlen(command), 0) == -1) return;
                if (receive_response(client_fd) == -1) return;
                if (send(client_fd, relative_path, strlen(relative_path) + 1, 0) == -1) return;
                if (receive_response(client_fd) == -1) return;
            } else {
                strcpy(command, "DELETE_FILE");
                if (send(client_fd, command, strlen(command), 0) == -1) return;
                if (receive_response(client_fd) == -1) return;
                if (send(client_fd, relative_path, strlen(relative_path) + 1, 0) == -1) return;
                if (receive_response(client_fd) == -1) return;
            }
        }
        p += sizeof(struct inotify_event) + event->len;
    }

    // After processing all events in the buffer, if a pending_move is still active,
    // it means it was a move-out that wasn't paired with a MOVED_TO.
    if (pending_move.active) {
        printf("Final check: Processing remaining IN_MOVED_FROM as DELETE (move-out): %s\n", pending_move.old_relative_path);
        char cmd_out[16];
        if (pending_move.is_dir) strcpy(cmd_out, "DELETE_DIR");
        else strcpy(cmd_out, "DELETE_FILE");

        if (send(client_fd, cmd_out, strlen(cmd_out), 0) == -1) return;
        if (receive_response(client_fd) == -1) return;
        if (send(client_fd, pending_move.old_relative_path, strlen(pending_move.old_relative_path) + 1, 0) == -1) return;
        if (receive_response(client_fd) == -1) return;
        memset(&pending_move, 0, sizeof(PendingMoveEvent)); // Clear after processing
    }
}

void perform_rsync(int client_fd, const char *dir_path, const char *dir_path_2) {
    FileInfo file_list[1000];
    int file_count = 0;

    if (send(client_fd, "rsync", strlen("rsync"), 0) == -1) return;
    printf("Đã gửi command đến server: rsync\n");
    if (receive_response(client_fd) == -1) return;

    if (send(client_fd, dir_path, strlen(dir_path), 0) == -1) return;
    printf("Đã gửi đường dẫn thư mục đến server: %s\n", dir_path);
    if (receive_response(client_fd) == -1) return;

    if (send(client_fd, dir_path_2, strlen(dir_path_2), 0) == -1) return;
    printf("Đã gửi đường dẫn thư mục đến server: %s\n", dir_path_2);
    if (receive_response(client_fd) == -1) return;

    if (receive_response(client_fd) == -1) return;

    list_files(dir_path, file_list, &file_count);

    if (folder_exist == FALSE)
    {
        folder_exist = TRUE;
        for (int i = 0; i < file_count; i++)
        {
            char different_path[PATH_MAX];
            // Lọc phần đường dẫn khác nhau
            printf("dir path: %s\n", dir_path);
            printf("file path: %s\n", file_list[i].filepath);
            get_different_path(dir_path, file_list[i].filepath, different_path);
            printf("different path: %s\n", different_path);
            if (send_file(client_fd, file_list[i].filepath, different_path) == -1) return;
            usleep(50000);
            if (receive_response(client_fd) == -1) return;
        }
        char end_signal[] = "END_OF_TRANSFER\n";
        printf("Sending end signal: %s\n", end_signal);
        if (send(client_fd, end_signal, strlen(end_signal), 0) == -1) return;
    }
    else
    {
        //Đầu tiên phải gửi file count đến cho server
        send_file_count(client_fd, file_count);
        printf("Đã gửi file count %d\n", file_count);
        if (receive_response(client_fd) == -1) return;

        //Gửi thông tin của các tệp tin cho server
        for (int i = 0; i < file_count; i++) {
            char temp[PATH_MAX];
            strncpy(temp, file_list[i].filepath, sizeof(temp) - 1);
            temp[sizeof(temp) - 1] = '\0';
            printf("temp1: %s\n", temp);
            char different_path[PATH_MAX];
            get_different_path(dir_path, file_list[i].filepath, different_path);
            printf("different path: %s\n", different_path);
            strncpy(file_list[i].filepath, different_path, sizeof(file_list[i].filepath) - 1);
            file_list[i].filepath[sizeof(file_list[i].filepath) - 1] = '\0';
            if (send_file_info(client_fd, &file_list[i]) == -1) return;
           
            if (receive_response(client_fd) == -1) return; // Nhận phản hồi từ server sau mỗi tệp tin

            if (receive_response(client_fd) == -1) return; //nhận phản hồi từ server để kiểm tra file có tồn tại hay không
            
            if(file_exist == FALSE)
            {
                printf("gửi thông tin file!\n");
                file_exist = TRUE;
                printf("temp2: %s\n", temp);
                if (send_file(client_fd, temp, different_path) == -1) return;
                usleep(50000);
                if (receive_response(client_fd) == -1) return;
            }

            else
            {
                if (receive_response(client_fd) == -1) return;
                if(file_no_change == FALSE)
                {
                    if (send_file(client_fd, temp, different_path) == -1) return;
                    usleep(50000);
                    if (receive_response(client_fd) == -1) return;
                }
            }
            printf("\n\n");
        }
        folder_exist = FALSE;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Ban chua cung cap IP.\n");
        return 1;
    }
    const char *SERVER_ADDR = argv[1];
    int client_fd;
    struct sockaddr_in server_addr;
    char dir_path[PATH_MAX];
    char dir_path_2[PATH_MAX];

    printf("Nhập đường dẫn thư mục nguồn (client): ");
    fgets(dir_path, sizeof(dir_path), stdin);
    dir_path[strcspn(dir_path, "\n")] = '\0';

    printf("Nhập đường dẫn thư mục đích (server): ");
    fgets(dir_path_2, sizeof(dir_path_2), stdin);
    dir_path_2[strcspn(dir_path_2, "\n")] = '\0';

    // Initialize socket
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);

    client_fd = create_client_socket();
    connect_to_server(client_fd, &server_addr);

    perform_rsync(client_fd, dir_path, dir_path_2);

    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        perror("inotify_init failed");
        close(client_fd);
        return 1;
    }
    printf("Inotify initialized with fd: %d\n", inotify_fd);
    fflush(stdout);

    int wd_map[1000];
    char *wd_paths[1000];
    int wd_count = 0;
    add_watch_recursive(inotify_fd, dir_path, wd_map, &wd_count, wd_paths);
    printf("Initial watches added. Total watches: %d\n", wd_count);
    fflush(stdout);

    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);

        int ret = select(inotify_fd + 1, &fds, NULL, NULL, NULL);

        if (ret == -1) {
            perror("select");
            close(client_fd);
            close(inotify_fd);
            return 1;
        } else if (ret > 0) {
            if (FD_ISSET(inotify_fd, &fds)) {
                handle_inotify_event(client_fd, inotify_fd, dir_path, dir_path_2, wd_map, &wd_count, wd_paths);
            }
        }
    }

    for (int i = 0; i < wd_count; i++) {
        inotify_rm_watch(inotify_fd, wd_map[i]);
        free(wd_paths[i]);
    }
    close(inotify_fd);
    close(client_fd);
    return 0;
}