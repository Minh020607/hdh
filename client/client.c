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
#define BUFFER_SIZE 1024
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
    char filename[PATH_MAX];        // Tên tệp tin
    char filepath[PATH_MAX];        // Đường dẫn đầy đủ đến tệp tin
    long filesize;              // Kích thước tệp tin
    unsigned char hash[16];     // Hash của tệp tin (MD5)
    time_t timestamp;           //Thời gian chỉnh sửa file
} FileInfo;

int receive_response(int client_fd);
int send_response(int client_socket, const char *message);
int receive_file_info(int client_fd, FileInfo *file_info);
int receive_file(int socket_fd, const char *base_path);
int send_file(int socket_fd, const char *path_file, const char *file_path);

int create_directory_recursively(const char *path) {
    char temp_path[512];
    char *p = NULL;
    size_t len;

    // Sao chép đường dẫn vào biến tạm
    snprintf(temp_path, sizeof(temp_path), "%s", path);

    // Lặp qua các phần của đường dẫn và tạo từng thư mục một
    len = strlen(temp_path);
    if (temp_path[len - 1] == '/') {
        temp_path[len - 1] = '\0'; // Xóa ký tự '/' cuối nếu có
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

    // Nhận tên file
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) { return -1; }
    printf("Nhận tên file: %s\n", buffer);
    strncpy(file_info->filename, buffer, sizeof(file_info->filename) - 1);
    file_info->filename[sizeof(file_info->filename) - 1] = '\0';
    // Phản hồi cho client
    const char *sp1 = "Receive file name ok!";
    if (send_response(client_fd, sp1) == -1) { return -1; }

    // Nhận đường dẫn file
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) { return -1; }
    printf("Nhận đường dẫn file: %s\n", buffer);
    strncpy(file_info->filepath, buffer, sizeof(file_info->filepath) - 1);
    file_info->filepath[sizeof(file_info->filepath) - 1] = '\0';
    // Phản hồi cho client
    const char *sp2 = "Receive path file ok!";
    if (send_response(client_fd, sp2) == -1) { return -1; }

    // Nhận kích thước file
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) { return -1; }
    printf("Nhận kích thước file: %s\n", buffer);
    file_info->filesize = atol(buffer);
    const char *sp3 = "Receive file size ok!";
    if (send_response(client_fd, sp3) == -1) { return -1; }

    // Nhận hash của file
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) { return -1; }
    printf("Nhận hash: %s\n", buffer);

    // Xử lý và lưu hash vào struct
    int hash_index = 0;
    while (hash_index < 32 && sscanf(buffer + hash_index * 2, "%02hhx", &file_info->hash[hash_index]) == 1) {
        hash_index++;
    }

    // Phản hồi cho client
    const char *sp4 = "Receive hash file ok!";
    if (send_response(client_fd, sp4) == -1) { return -1; }

    // Nhận timestamp
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) { return -1; }
    printf("Nhận timestamp: %s\n", buffer);
    file_info->timestamp = (time_t)atol(buffer);
    const char *sp5 = "Receive timestamp ok!";
    if (send_response(client_fd, sp5) == -1) { return -1; }
    return 0;
}

void receive_file_count(int client_fd, int *file_count) {
    int network_file_count;

    // Nhận dữ liệu
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
    char file_name[MAX_PATH];

    // Nhận đường dẫn thư mục từ server
    ssize_t path_len = recv(socket_fd, path_buffer, sizeof(path_buffer), 0);

    if (path_len <= 0) {
        if (path_len == 0) printf("Server disconnected during path reception.\n");
        else perror("Lỗi nhận đường dẫn");
        return -1;
    }
    path_buffer[path_len] = '\0';

    printf("path_buffer: %s\n", path_buffer);

    // Kiểm tra tín hiệu kết thúc
    if (strstr(path_buffer, "END_OF_TRANSFER") != NULL) {
        receive_folder_done = TRUE;
        return 0;
    }

    // Nhận tên file từ client
    ssize_t file_name_len = recv(socket_fd, file_name, sizeof(file_name), 0);
    if (file_name_len <= 0) {
        if (file_name_len == 0) printf("Server disconnected during file name reception.\n");
        else perror("Lỗi nhận tên file");
        return -1;
    }
    file_name[file_name_len] = '\0';  // Đảm bảo kết thúc chuỗi
    const char *response_message = "receive file name successful";
    if (send_response(socket_fd, response_message) == -1) { return -1; }
    printf("file name: %s\n", file_name);

    // Tạo đường dẫn đầy đủ cho file (bao gồm thư mục base_path, đường dẫn thư mục từ client và tên file)
    char full_file_path[PATH_MAX];

    if(path_len == 1)
    {
        snprintf(full_file_path, sizeof(full_file_path), "%s", base_path);
    }
    else{
        snprintf(full_file_path, sizeof(full_file_path), "%s%s", base_path, path_buffer);
    }
    printf("đường dẫn thư mục tạo để ghi: %s\n", full_file_path);

    if (create_directory_recursively(full_file_path) == -1) {
        printf("Có lỗi khi tạo thư mục.\n");
        return -1;
    } else {
        printf("Các thư mục đã được tạo thành công.\n");
    }

    char full_file_path2[PATH_MAX];
    snprintf(full_file_path2, sizeof(full_file_path2), "%s%s", full_file_path, file_name);

    printf("file name: %s\n", file_name);
    printf("đường dẫn file để ghi là: %s\n", full_file_path2);

    // Nhận kích thước file
    long file_size;
    if (recv(socket_fd, &file_size, sizeof(file_size), 0) <= 0) {
        if (recv(socket_fd, &file_size, sizeof(file_size), 0) == 0) printf("Server disconnected during file size reception.\n");
        else perror("Lỗi nhận kích thước file");
        return -1;
    }
    printf("Kích thước file: %ld bytes\n", file_size);

    // Mở file để ghi dữ liệu nhận được
    int bytes_received_data;
    FILE *file = fopen(full_file_path2, "wb");
    if (!file) {
        perror("Lỗi mở file để ghi");
        return -1;
    }
    printf("Receiving file: %s\n", file_name);

    char buffer2[BUFFER_SIZE];
    long total_received = 0;
    
    while (total_received < file_size) {
        bytes_received_data = recv(socket_fd, buffer2, sizeof(buffer2), 0);
        if (bytes_received_data <= 0) {
            if (bytes_received_data == 0) printf("Server disconnected during file data reception.\n");
            else perror("Lỗi nhận dữ liệu file");
            fclose(file);
            return -1;
        }
        fwrite(buffer2, 1, bytes_received_data, file);
        total_received += bytes_received_data;
    }

    fclose(file);
    printf("File '%s' received successfully!\n", file_name);
    return 0;
}

void receive_and_print_tree(int sock) {
    char buffer[2048];

    int bytes_read_unused = 0; // Renamed to avoid warning
    
    // Nhận dữ liệu từ server và hiển thị
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
        return 0; // False
    } else if (S_ISDIR(info.st_mode)) {
        // Đây là một thư mục
        return 1; // True
    } else {
        // Đây không phải là thư mục (có thể là file thông thường)
        return 0; // False
    }
}

int check_path_exists(const char *base_path, const char *relative_path) {
    char full_path[PATH_MAX];
    struct stat path_stat;

    // Kết hợp đường dẫn cơ sở với đường dẫn tương đối
    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, relative_path);

    // Kiểm tra sự tồn tại của đường dẫn
    if (stat(full_path, &path_stat) == 0) {
        // Đường dẫn tồn tại
        return 1;
    } else {
        // Đường dẫn không tồn tại
        perror("Lỗi kiểm tra đường dẫn");
        return 0;
    }
}

time_t get_file_timestamp(const char *filename) {
    struct stat file_info;

    // Lấy thông tin về file
    if (stat(filename, &file_info) == 0) {
        return file_info.st_mtime;  // Trả về thời gian sửa đổi của file
    } else {
        perror("Không thể lấy thông tin file");
        return -1;  // Trả về -1 nếu có lỗi
    }
}

void send_file_count(int client_fd, int file_count) {
    // Chuyển đổi thứ tự byte nếu cần (network byte order)
    int network_file_count = htonl(file_count);

    // Gửi dữ liệu
    if (send(client_fd, &network_file_count, sizeof(network_file_count), 0) < 0) {
        perror("Gửi file_count thất bại");
    } else {
        printf("Đã gửi file_count: %d\n", file_count);
    }
}

void get_different_path(const char *base_path, const char *file_path, char *result) {
    // Tìm vị trí của tên file trong file_path
    const char *last_slash = strrchr(file_path, '/');
    if (!last_slash) {
        // Nếu không tìm thấy '/', không thể lấy đường dẫn
        result[0] = '\0';
        return;
    }

    // Sao chép phần đường dẫn (bỏ qua tên file)
    char file_dir[PATH_MAX];
    strncpy(file_dir, file_path, last_slash - file_path + 1);
    file_dir[last_slash - file_path + 1] = '\0';

    // Copy the remaining part of file_dir (the different part)
    // If file_dir is essentially the same as base_path (or base_path with a trailing slash), result is empty.
    size_t base_len = strlen(base_path);
    size_t file_dir_len = strlen(file_dir);

    if (strncmp(base_path, file_dir, base_len) == 0 && 
        (file_dir_len == base_len || (file_dir_len == base_len + 1 && file_dir[base_len] == '/'))) {
        result[0] = '\0'; // Empty string for files directly in base directory
    } else {
        // Find the differing part
        const char *base_ptr = base_path;
        const char *file_ptr = file_dir;

        while (*base_ptr && *file_ptr && *base_ptr == *file_ptr) {
            base_ptr++;
            file_ptr++;
        }
        strcpy(result, file_ptr);
    }
}

int send_file(int socket_fd, const char *path_file, const char *file_path) {
    printf("path file: %s\n", path_file);
    FILE *file = fopen(path_file, "rb");
    if (!file) {
        perror("Lỗi mở file");
        return -1;
    }

    // Tách tên file từ đường dẫn
    const char *file_name = strrchr(path_file, '/');
    if (file_name) {
        file_name++; // Bỏ qua dấu '/' để lấy tên file
    } else {
        file_name = path_file;
    }

    // Gửi đường dẫn thư mục tương đối
    printf("Client: About to send relative directory path: '%s' (length: %zu)\n", file_path, strlen(file_path)); fflush(stdout);
    if (send(socket_fd, file_path, strlen(file_path) + 1, 0) == -1) {
        perror("Lỗi gửi đường dẫn file");
        fclose(file);
        return -1;
    }
    printf("Client: Sent relative directory path. Waiting for server response.\n"); fflush(stdout);
    if (receive_response(socket_fd) == -1) {
        fclose(file);
        return -1;
    }
    printf("Client: Received response for relative directory path. About to send file name.\n"); fflush(stdout);

    // Gửi tên file
    printf("Client: About to send file name: '%s' (length: %zu)\n", file_name, strlen(file_name)); fflush(stdout);
    if (send(socket_fd, file_name, strlen(file_name) + 1, 0) < 0) {
        perror("Lỗi gửi tên file");
        fclose(file);
        return -1;
    }
    printf("Client: Sent file name. Waiting for server response.\n"); fflush(stdout);
    if (receive_response(socket_fd) == -1) {
        fclose(file);
        return -1;
    }
    printf("Client: Received response for file name. About to send file size.\n"); fflush(stdout);

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
    if (send(client_socket, message, strlen(message), 0) < 0) {
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
    
    // Read all available data, but be mindful of buffer size
    bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            printf("Server disconnected.\n");
        } else {
            perror("Error receiving data");
        }
        return -1;
    }
    buffer[bytes_received] = '\0';  // Đảm bảo chuỗi kết thúc

    // Process potentially concatenated responses
    char *token;
    char *rest = buffer;
    int processed_any_response = 0;

    while ((token = strtok_r(rest, "\0", &rest)) != NULL) {
        printf("Received from server: %s\n", token);
        processed_any_response = 1;

        // So sánh với chuỗi "folder no exists"
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
        return -1; // Indicate no valid response was processed
    }

    return 0;
}

void list_files(const char *dir_path, FileInfo *file_list, int *file_count) {
    DIR *dir = opendir(dir_path);  // Mở thư mục
    struct dirent *entry;

    if (dir == NULL) {
        perror("Không thể mở thư mục");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char full_path[PATH_MAX];
        
        // Bỏ qua thư mục '.' và '..'
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Tạo đường dẫn đầy đủ của tệp hoặc thư mục
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

    closedir(dir);  // Đóng thư mục sau khi duyệt xong
}

int send_file_info(int client_fd, FileInfo *file_info) {
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    // Gửi tên file
    snprintf(buffer, sizeof(buffer), "%s", file_info->filename);
    if (send(client_fd, buffer, strlen(buffer) + 1, 0) < 0) { return -1; }
    printf("Đã gửi tên file: %s\n", file_info->filename);

    // Đợi phản hồi từ server
    char response[BUFFER_SIZE];
    memset(response, 0, sizeof(response));
    if (recv(client_fd, response, sizeof(response), 0) <= 0) { return -1; }
    printf("Phản hồi từ server: %s\n", response);

    printf("\n");
    // Gửi đường dẫn file
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%s", file_info->filepath);
    if (send(client_fd, buffer, strlen(buffer) + 1, 0) < 0) { return -1; }
    printf("Đã gửi đường dẫn: %s\n", file_info->filepath);

    printf("\n");
    // Đợi phản hồi từ server
    memset(response, 0, sizeof(response));
    if (recv(client_fd, response, sizeof(response), 0) <= 0) { return -1; }
    printf("Phản hồi từ server: %s\n", response);

    printf("\n");
    // Gửi kích thước file
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%ld bytes", file_info->filesize);
    if (send(client_fd, buffer, strlen(buffer) + 1, 0) < 0) { return -1; }
    printf("Đã gửi kích thước file: %ld bytes\n", file_info->filesize);

    // Đợi phản hồi từ server
    memset(response, 0, sizeof(response));
    if (recv(client_fd, response, sizeof(response), 0) <= 0) { return -1; }
    printf("Phản hồi từ server: %s\n", response);

    // Gửi hash của file dưới dạng hex
    // Khởi tạo buffer
    char hash_buffer[65]; // 32 bytes * 2 ký tự cho mỗi byte + 1 ký tự null terminator
    hash_buffer[0] = '\0'; // Đảm bảo chuỗi bắt đầu trống

    // Ghép các giá trị hash vào buffer
    for (int i = 0; i < 16; i++) {
        snprintf(hash_buffer + strlen(hash_buffer), sizeof(hash_buffer) - strlen(hash_buffer), "%02x", file_info->hash[i]);
    }

    // Gửi toàn bộ hash dưới dạng hex
    if (send(client_fd, hash_buffer, strlen(hash_buffer) + 1, 0) < 0) { return -1; }
    printf("Đã gửi hash: %s\n", hash_buffer);

    // Đợi phản hồi từ server sau khi gửi hết hash
    memset(response, 0, sizeof(response));
    if (recv(client_fd, response, sizeof(response), 0) <= 0) { return -1; }
    printf("Phản hồi từ server: %s\n", response);

    // Gửi timestamp
    memset(buffer, 0, sizeof(buffer));
    snprintf(buffer, sizeof(buffer), "%ld", (long)file_info->timestamp); // Chuyển `time_t` thành `long` để in
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
    printf("Added watch for path: %s, wd: %d\n", path, wd); // Debug print
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

void handle_inotify_event(int client_fd, int inotify_fd, const char *base_path, const char *dest_path_unused, int *wd_map, int *wd_count, char **wd_paths) {
    char buffer[INOTIFY_BUFFER_SIZE];
    ssize_t len = read(inotify_fd, buffer, INOTIFY_BUFFER_SIZE);
    printf("Read from inotify: len=%zd\n", len); // Debug print
    fflush(stdout);
    if (len < 0) {
        perror("Error reading inotify events");
        return;
    }

    char command[16];
    for (char *p = buffer; p < buffer + len;) {
        struct inotify_event *event = (struct inotify_event *)p;
        printf("  Event name: %s, mask: %x\n", event->name, event->mask); // Debug print
        fflush(stdout);
        char full_path[PATH_MAX];
        char relative_path[PATH_MAX];

        // Find the directory path associated with the watch descriptor
        for (int i = 0; i < *wd_count; i++) {
            if (wd_map[i] == event->wd) {
                snprintf(full_path, sizeof(full_path), "%s/%s", wd_paths[i], event->name);
                get_different_path(base_path, full_path, relative_path);
                break;
            }
        }

        if (event->mask & IN_CREATE) {
            printf("IN_CREATE: %s\n", full_path);
            struct stat statbuf;
            if (stat(full_path, &statbuf) == 0) {
                if (S_ISDIR(statbuf.st_mode)) {
                    // Add watch for new directory
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
                perror("Error stat-ing created file"); // Debug print for stat failure
            }
        } else if (event->mask & IN_MODIFY) {
            printf("IN_MODIFY: %s\n", full_path);
            strcpy(command, "MODIFY_FILE");
            if (send(client_fd, command, strlen(command), 0) == -1) return;
            if (receive_response(client_fd) == -1) return;
            if (send_file(client_fd, full_path, relative_path) == -1) return;
            if (receive_response(client_fd) == -1) return;
        } else if (event->mask & IN_DELETE) {
            printf("IN_DELETE: %s\n", full_path);
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
            char different_path[MAX_PATH];
            // Lọc phần đường dẫn khác nhau
            printf("dir path: %s\n", dir_path);
            printf("file path: %s\n", file_list[i].filepath);
            get_different_path(dir_path, file_list[i].filepath, different_path);
            printf("different path: %s\n", different_path);
            if (send_file(client_fd, file_list[i].filepath, different_path) == -1) return;
            // Add a small delay to allow server to process and respond
            usleep(50000); // 50 milliseconds
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
            char different_path[MAX_PATH];
            get_different_path(dir_path, file_list[i].filepath, different_path);
            printf("different path: %s\n", different_path);
            strncpy(file_list[i].filepath, different_path, sizeof(file_list[i].filepath) - 1);
            file_list[i].filepath[sizeof(file_list[i].filepath) - 1] = '\0';
            if (send_file_info(client_fd, &file_list[i]) == -1) return;
            // Removed direct receive_response here, managed by handle_command
            if (receive_response(client_fd) == -1) return; // Nhận phản hồi từ server sau mỗi tệp tin

            if (receive_response(client_fd) == -1) return; //nhận phản hồi từ server để kiểm tra file có tồn tại hay không
            
            if(file_exist == FALSE)
            {
                printf("gửi thông tin file!\n");
                file_exist = TRUE;
                printf("temp2: %s\n", temp);
                if (send_file(client_fd, temp, different_path) == -1) return;
                usleep(50000); // 50 milliseconds
                if (receive_response(client_fd) == -1) return;
            }

            else
            {
                if (receive_response(client_fd) == -1) return;
                if(file_no_change == FALSE)
                {
                    if (send_file(client_fd, temp, different_path) == -1) return;
                    usleep(50000); // 50 milliseconds
                    if (receive_response(client_fd) == -1) return;
                }
            }
            printf("\n\n");
        }
        folder_exist = FALSE;
        //getchar();
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

    // Prompt for directory paths
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

    // Perform initial rsync
    perform_rsync(client_fd, dir_path, dir_path_2);

    // Initialize inotify
    int inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        perror("inotify_init failed");
        close(client_fd);
        return 1;
    }
    printf("Inotify initialized with fd: %d\n", inotify_fd); // Debug print
    fflush(stdout);

    int wd_map[1000];
    char *wd_paths[1000];
    int wd_count = 0;
    add_watch_recursive(inotify_fd, dir_path, wd_map, &wd_count, wd_paths);
    printf("Initial watches added. Total watches: %d\n", wd_count); // Debug print
    fflush(stdout);

    // Main loop for inotify events
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);

        // Wait for activity on inotify_fd with an infinite timeout
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

    // Cleanup
    for (int i = 0; i < wd_count; i++) {
        inotify_rm_watch(inotify_fd, wd_map[i]);
        free(wd_paths[i]);
    }
    close(inotify_fd);
    close(client_fd);
    return 0;
}