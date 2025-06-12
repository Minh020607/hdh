#include <stdio.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>  
#include <dirent.h>
#include <openssl/sha.h>
#include <time.h>
#include <limits.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define MAX_PATH PATH_MAX
#define STORAGE_PATH "../storage3"
#define HASH_SIZE 16

#define TRUE 1
#define FALSE 0

uint8_t folder_exist = TRUE;
uint8_t file_exist = TRUE;
uint8_t file_in_client_exist = FALSE;
uint8_t file_no_change = TRUE;
uint8_t folder_client_exist = TRUE;
uint8_t file_in_client_no_change = TRUE;
uint8_t receive_folder_done = FALSE;



char current_client_sync_base_path[PATH_MAX] = {0};

typedef struct {
    char filename[PATH_MAX];      
    char filepath[PATH_MAX];   
    long filesize;              
    unsigned char hash[16];     
    time_t timestamp;
} FileInfo;


FileInfo global_file_list[1000];
FileInfo global_file_info[1000];

time_t get_file_timestamp(const char *filename);
void send_file_count(int client_fd, int file_count);
void print_tree(const char *path, int depth, int client_socket);
void receive_file_count(int client_fd, int *file_count);
int check_file_exists(const char *filepath);
void create_storage();
int calculate_file_hash(const char *filepath, unsigned char *hash_out);
void list_files(const char *dir_path, FileInfo *file_list, int *file_count);
int send_response(int client_socket, const char *message);
int receive_response(int client_socket);
int create_directory_recursively(const char *path);
int receive_file(int socket_fd, const char *target_directory);
void get_different_path(const char *base_path, const char *file_path, char *result);
int send_file(int socket_fd, const char *path_file, const char *file_path);
int check_path_exists(const char *base_path, const char *relative_path);
int check_directory_exists(const char *parent_dir, const char *dir_name);
int is_directory_exists(const char *path);
void remove_last_component(char *path);
void get_last_directory(const char *path, char *last_directory);
int receive_directory_path(int client_socket, char *dir_path, size_t size);
int create_server_socket();
void bind_server_socket(int server_fd, struct sockaddr_in *server_addr);
void listen_for_connections(int server_fd);
int handle_command(int client_socket);
int remove_directory_recursively(const char *path);

int receive_file_content(int socket_fd, const char *final_file_path, long file_size);

int receive_file_info(int client_fd, FileInfo *file_info) {
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));

    // Receive filepath (which contains the full relative path including filename)
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) {
        if (recv(client_fd, buffer, sizeof(buffer), 0) == 0) printf("Client disconnected during file path reception.\n");
        else perror("Error receiving file path");
        return -1;
    }
    strncpy(file_info->filepath, buffer, sizeof(file_info->filepath) - 1);
    file_info->filepath[sizeof(file_info->filepath) - 1] = '\0';
    printf("Server: Received full relative file path: %s\n", file_info->filepath);
    if (send_response(client_fd, "Full file path received") == -1) return -1;

    const char *last_slash = strrchr(file_info->filepath, '/');
    if (last_slash) {
        strncpy(file_info->filename, last_slash + 1, sizeof(file_info->filename) - 1);
    } else {
        strncpy(file_info->filename, file_info->filepath, sizeof(file_info->filename) - 1);
    }
    file_info->filename[sizeof(file_info->filename) - 1] = '\0';
    printf("Server: Extracted filename: %s\n", file_info->filename);

    // Receive filesize
    long network_file_size;
    if (recv(client_fd, &network_file_size, sizeof(network_file_size), 0) <= 0) {
        if (recv(client_fd, &network_file_size, sizeof(network_file_size), 0) == 0) printf("Client disconnected during file size reception.\n");
        else perror("Error receiving file size");
        return -1;
    }
    file_info->filesize = ntohl(network_file_size); // Convert from network byte order to host byte order
    printf("Server: Received filesize: %ld bytes\n", file_info->filesize);
    if (send_response(client_fd, "Filesize received") == -1) return -1;

    if (recv(client_fd, file_info->hash, sizeof(file_info->hash), 0) <= 0) {
        if (recv(client_fd, file_info->hash, sizeof(file_info->hash), 0) == 0) printf("Client disconnected during hash reception.\n");
        else perror("Error receiving hash");
        return -1;
    }
    printf("Server: Received hash (raw bytes)\n");
    if (send_response(client_fd, "Hash received") == -1) return -1;

    long network_timestamp;
    if (recv(client_fd, &network_timestamp, sizeof(network_timestamp), 0) <= 0) {
        if (recv(client_fd, &network_timestamp, sizeof(network_timestamp), 0) == 0) printf("Client disconnected during timestamp reception.\n");
        else perror("Error receiving timestamp");
        return -1;
    }
    file_info->timestamp = (time_t)ntohl(network_timestamp);
    printf("Server: Received timestamp: %ld\n", (long)file_info->timestamp);
    if (send_response(client_fd, "Timestamp received") == -1) return -1;

    return 0;
}

void normalize_path_segment(char *dest, const char *path1, const char *path2) {
    size_t len1 = strlen(path1);
    size_t len2 = strlen(path2);

    if (len1 == 0) {
        strncpy(dest, path2, PATH_MAX - 1);
        dest[PATH_MAX - 1] = '\0';
        return;
    }
    if (len2 == 0) {
        strncpy(dest, path1, PATH_MAX - 1);
        dest[PATH_MAX - 1] = '\0';
        return;
    }

    int has_slash1 = (path1[len1 - 1] == '/');
    int has_slash2 = (path2[0] == '/');

    if (has_slash1 && has_slash2) {
        // Both have slashes, remove one
        snprintf(dest, PATH_MAX, "%s%s", path1, path2 + 1);
    } else if (!has_slash1 && !has_slash2) {
        // Neither has slashes, add one
        snprintf(dest, PATH_MAX, "%s/%s", path1, path2);
    } else {
        // One has a slash, concatenate directly
        snprintf(dest, PATH_MAX, "%s%s", path1, path2);
    }
    dest[PATH_MAX - 1] = '\0'; 
}

int remove_directory_recursively(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        perror("Cannot open directory for removal");
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat statbuf;
        if (stat(full_path, &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                remove_directory_recursively(full_path);
            } else {
                if (remove(full_path) != 0) {
                    perror("Error deleting file");
                    closedir(dir);
                    return -1;
                }
            }
        }
    }
    closedir(dir);

    if (rmdir(path) != 0) {
        perror("Error deleting directory");
        return -1;
    }
    printf("Deleted directory: %s\n", path);
    return 0;
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

void print_tree(const char *path, int depth, int client_socket) {
    struct dirent *entry;
    DIR *dir = opendir(path);

    if (dir == NULL) {
        perror("Không thể mở thư mục");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char full_path[PATH_MAX];
        struct stat info;

        // Bỏ qua các mục đặc biệt "." và ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Tạo đường dẫn đầy đủ
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        // Lấy thông tin mục hiện tại
        if (stat(full_path, &info) == 0) {
            char buffer[BUFFER_SIZE];

            // Tạo chuỗi kết quả thụt lề theo độ sâu
            memset(buffer, 0, sizeof(buffer));
            for (int i = 0; i < depth; i++) {
                strcat(buffer, "│   ");
            }

            if (S_ISDIR(info.st_mode)) {
                // Nếu là thư mục
                snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), "├── %s/\n", entry->d_name);
                send(client_socket, buffer, strlen(buffer), 0); // Gửi tới client
                print_tree(full_path, depth + 1, client_socket); // Đệ quy
            } else if (S_ISREG(info.st_mode)) {
                // Nếu là file
                snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer), "├── %s\n", entry->d_name);
                send(client_socket, buffer, strlen(buffer), 0); // Gửi tới client
            }
        }
    }
    closedir(dir);

    // Gửi tín hiệu kết thúc sau khi hoàn thành
    if (depth == 0) { // Chỉ gửi khi hoàn tất toàn bộ cây thư mục
        char end_signal[] = "END_OF_TREE\n";
        printf("Sending end signal: %s\n", end_signal);
        send(client_socket, end_signal, strlen(end_signal), 0);
    }
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

int check_file_exists(const char *filepath) {
    if (access(filepath, F_OK) != -1) {
        // File tồn tại
        return 1;
    } else {
        // File không tồn tại
        return 0;
    }
}


void create_storage() 
{
    /* struct stat là 1 cấu trúc trong trong thư viện sys/stat.h dùng để lưu trữ thông tin về file hoặc thư mục.
    Cấu trúc này sẽ chứa thông tin về kích thước file, quyền truy cập, thời gian sửa đổi,... 
    Ban đầu thì ta sẽ khởi tạo mọi thành phần trong cấu trúc này là 0*/
    struct stat st = {0};

    /* Ta sẽ kiểm tra xem liệu storage đã được tạo hay chưa. hàm stat sẽ lấy thư mục STORAGE_PATH gán vào st.
    Nếu st == 0 tức là folder đã tồn tại và thông tin đã được lấy thành công. Ngược lại là chưa tồn tại */
    if (stat(STORAGE_PATH, &st) == -1) 
    {
        /* Chưa tồn tại thì tạo mới thư mục Storage.
        mkdir để tạo thư mục mới có tên như macro STORAGE_PATH.
         */
        if (mkdir(STORAGE_PATH, 0777) == 0) 
        {
            printf("Thư mục lưu trữ đã được tạo: %s\n", STORAGE_PATH);
        } 
        else 
        {
            perror("Lỗi khi tạo thư mục lưu trữ");
            exit(EXIT_FAILURE);  // Thoát chương trình nếu lỗi
        }
    } 
    else 
    {
        printf("Thư mục lưu trữ đã tồn tại: %s\n", STORAGE_PATH);
    }
}

int calculate_file_hash(const char *filepath, unsigned char *hash_out) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("Không thể mở tệp tin");
        return -1;
    }

    EVP_MD_CTX *mdctx;
    const EVP_MD *md = EVP_sha256();
    unsigned char data[1024];
    size_t bytesRead;
    unsigned int hash_len;

    mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, md, NULL);
    
    while ((bytesRead = fread(data, 1, sizeof(data), file)) > 0) {
        EVP_DigestUpdate(mdctx, data, bytesRead);
    }

    EVP_DigestFinal_ex(mdctx, hash_out, &hash_len);
    EVP_MD_CTX_free(mdctx);

    fclose(file);
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
        
        // Bỏ qua thư mục "." và ".."
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
                strcpy(file_list[*file_count].filename, entry->d_name);
                strcpy(file_list[*file_count].filepath, full_path);
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

int send_response(int client_socket, const char *message) {
    if (send(client_socket, message, strlen(message), 0) < 0) {
        perror("Failed to send response");
        return -1;
    } else {
        printf("Response sent to client: %s\n", message);
    }
    return 0;
}

// Hàm nhận phản hồi từ client
int receive_response(int client_socket) {
    char buffer[BUFFER_SIZE];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            printf("Client disconnected.\n");
        } else {
            perror("Error receiving data");
        }
        return -1;
    }
    buffer[bytes_received] = '\0';  // Đảm bảo chuỗi kết thúc
    printf("Received from client: %s\n", buffer);

    // So sánh với chuỗi "folder no exists"
    if (strcmp(buffer, "folder in client no exists") == 0) {
        folder_client_exist = FALSE;
    }
    else if (strcmp(buffer, "folder in client exists") == 0) {
        folder_client_exist = TRUE;
    }
    else if (strcmp(buffer, "File no exists") == 0) {
        file_in_client_exist = FALSE;
    }
    else if (strcmp(buffer, "File exists") == 0) {
        file_in_client_exist = TRUE;
    }
    else if(strcmp(buffer, "File no change") == 0)
    {
        file_in_client_no_change = TRUE;
    }
    else if(strcmp(buffer, "File change") == 0)
    {
        file_in_client_no_change = FALSE;
    }
    else if(strcmp(buffer, "Send folder done") == 0)
    {
        receive_folder_done = TRUE;
    }
    return 0;
}

int create_directory_recursively(const char *path) {
    char temp_path[PATH_MAX];
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

int receive_file_content(int socket_fd, const char *final_file_path, long file_size) {
    printf("Đường dẫn file để ghi là: %s\n", final_file_path);

    // Mở file để ghi dữ liệu nhận được
    int bytes_received_data;
    FILE *file = fopen(final_file_path, "wb"); // Declare and assign file pointer
    if (!file) {
        perror("Lỗi mở file để ghi");
        return -1;
    }
    printf("Receiving file content for: %s (size: %ld bytes)\n", final_file_path, file_size);

    char buffer2[BUFFER_SIZE];
    long total_received = 0;
    
    while (total_received < file_size) {
        bytes_received_data = recv(socket_fd, buffer2, sizeof(buffer2), 0);
        if (bytes_received_data <= 0) {
            if (bytes_received_data == 0) printf("Client disconnected during file data reception.\n");
            else perror("Lỗi nhận dữ liệu file");
            fclose(file);
            return -1;
        }
        fwrite(buffer2, 1, bytes_received_data, file);
        total_received += bytes_received_data;
    }

    fclose(file);
    printf("File content received successfully for '%s'!\n", final_file_path);
    return 0;
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

    // So sánh base_path với file_dir để tìm phần khác biệt
    const char *base_ptr = base_path;
    const char *file_ptr = file_dir;

    // Bỏ qua các ký tự giống nhau ban đầu
    while (*base_ptr && *file_ptr && *base_ptr == *file_ptr) {
        base_ptr++;
        file_ptr++;
    }

    // Copy phần còn lại của file_dir (phần khác biệt)
    if (*base_ptr == '\0' && *file_ptr == '/') {
        strcpy(result, file_ptr);
    } else {
        result[0] = '\0';
    }
}

int send_file(int socket_fd, const char *path_file, const char *file_path) {
    char different_path[PATH_MAX];
    get_different_path(file_path, path_file, different_path);
    FILE *file = fopen(path_file, "rb");
    if (!file) {
        perror("Lỗi mở file");
        return -1;
    }

    // Tách tên file từ đường dẫn
    const char *file_name = strrchr(path_file, '/');
    if (file_name) {
        file_name++;
    } else {
        file_name = path_file;
    }

    // Gửi đường dẫn thư mục và tên file
    printf("different_path: %s\n", different_path);
    if (send(socket_fd, different_path, strlen(different_path) + 1, 0) == -1) {
        perror("Lỗi gửi đường dẫn file");
        fclose(file);
        return -1;
    }

    printf("file name: %s\n", file_name);
    if (send(socket_fd, file_name, strlen(file_name) + 1, 0) < 0) {
        perror("Lỗi gửi tên file");
        fclose(file);
        return -1;
    }
    if (receive_response(socket_fd) == -1) {
        fclose(file);
        return -1;
    }

    // Tính toán và gửi kích thước file
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    printf("Kích thước file: %ld bytes\n", file_size);
    if (send(socket_fd, &file_size, sizeof(file_size), 0) < 0) {
        perror("Lỗi gửi kích thước file");
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
    printf("File đã được gửi thành công.\n");
    return 0;
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

int check_directory_exists(const char *parent_dir, const char *dir_name) {
    struct dirent *entry;
    struct stat entry_stat;
    char full_path[PATH_MAX];

    // Mở thư mục cha
    DIR *dir = opendir(parent_dir);
    if (!dir) {
        perror("Không thể mở thư mục");
        return 0;
    }

    // Duyệt qua các mục trong thư mục
    while ((entry = readdir(dir)) != NULL) {
        // Bỏ qua "." và ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Tạo đường dẫn đầy đủ
        snprintf(full_path, sizeof(full_path), "%s/%s", parent_dir, entry->d_name);

        // Kiểm tra loại mục (file/thư mục)
        if (stat(full_path, &entry_stat) == 0 && S_ISDIR(entry_stat.st_mode)) {
            // Kiểm tra xem có trùng với tên thư mục cần tìm không
            if (strcmp(entry->d_name, dir_name) == 0) {
                closedir(dir);
                return 1;
            }
        }
    }

    // Đóng thư mục sau khi duyệt
    closedir(dir);
    return 0;
}

int is_directory_exists(const char *path) {
    struct stat info;

    // Sử dụng stat để kiểm tra thông tin của đường dẫn
    if (stat(path, &info) != 0) {
        // Không thể lấy thông tin về đường dẫn
        return 0;
    }

    // Kiểm tra xem đường dẫn có phải là thư mục hay không
    return (info.st_mode & S_IFDIR) != 0;
}

void remove_last_component(char *path) {
    char *last_slash = strrchr(path, '/');
    if (last_slash != NULL) {
        *last_slash = '\0';
    }
}

void get_last_directory(const char *path, char *last_directory) {
    // Tạo bản sao của đường dẫn để tránh thay đổi đường dẫn gốc
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    // Dùng strtok để tách đường dẫn tại dấu '/'
    char *token = strtok(path_copy, "/");

    // Lặp qua tất cả các phần của đường dẫn và lấy phần cuối cùng
    while (token != NULL) {
        strncpy(last_directory, token, PATH_MAX - 1);
        last_directory[PATH_MAX - 1] = '\0';
        token = strtok(NULL, "/");
    }
}

int receive_directory_path(int client_socket, char *dir_path, size_t size) {
    // Đọc dữ liệu từ client
    int bytes_received = recv(client_socket, dir_path, size - 1, 0);
    if (bytes_received < 0) {
        perror("Lỗi khi nhận dữ liệu");
        return -1;
    }
    
    if (bytes_received == 0) {
        printf("Client đã đóng kết nối\n");
        return -1;
    }

    // Đảm bảo kết thúc chuỗi đúng cách
    dir_path[bytes_received] = '\0';

    return 0;
}

int create_server_socket() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {  
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    return server_fd;
}

void bind_server_socket(int server_fd, struct sockaddr_in *server_addr) {
    if (bind(server_fd, (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
}

void listen_for_connections(int server_fd) {
    if (listen(server_fd, 5) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    printf("Server listening on port %d...\n", PORT);
}

int handle_command(int client_socket) {
    char command[16];
    char temp_dir_path[PATH_MAX]; // Use temp_dir_path for incoming client paths
    char absolute_path[PATH_MAX];
    char full_path[PATH_MAX];

    int command_receive = recv(client_socket, command, sizeof(command) - 1, 0);
    if (command_receive <= 0) {
        if (command_receive == 0) {
            printf("Client disconnected from handle_command.\n");
        } else {
            perror("Error receiving command in handle_command");
        }
        return -1;
    }
    command[command_receive] = '\0';
    printf("Đã nhận command từ client: %s\n", command);
    if (send_response(client_socket, "Command received") == -1) return -1; // Gửi phản hồi chung sau khi nhận command

    if (strcmp(command, "rsync") == 0) {
        if (receive_directory_path(client_socket, temp_dir_path, sizeof(temp_dir_path)) == -1) return -1; // Client source path
        if (send_response(client_socket, "Receive path successful") == -1) return -1;
            printf("Đường dẫn nhận được từ client (source): %s\n", temp_dir_path);

        // Receive the actual destination path on the server for rsync
        char rsync_dest_path_from_client[PATH_MAX];
        if (receive_directory_path(client_socket, rsync_dest_path_from_client, sizeof(rsync_dest_path_from_client)) == -1) return -1;
        if (send_response(client_socket, "Receive path successful") == -1) return -1;
        printf("Đường dẫn nhận được từ client (destination for rsync): %s\n", rsync_dest_path_from_client);


        if (realpath(STORAGE_PATH, absolute_path) == NULL) {
            perror("Lỗi khi lấy đường dẫn tuyệt đối");
            return -1;
        }
        if (strlen(absolute_path) > 1 && absolute_path[strlen(absolute_path) - 1] == '/') {
            absolute_path[strlen(absolute_path) - 1] = '\0';
        }
        printf("DEBUG: absolute_path normalized: '%s'\n", absolute_path); fflush(stdout);

        char normalized_rsync_dest_path[PATH_MAX];
        normalize_path_segment(normalized_rsync_dest_path, "", rsync_dest_path_from_client);
        printf("DEBUG: normalized_rsync_dest_path: '%s'\n", normalized_rsync_dest_path); fflush(stdout);

        normalize_path_segment(current_client_sync_base_path, absolute_path, normalized_rsync_dest_path);
        printf("Đường dẫn thư mục sẽ lưu trên server: %s\n", current_client_sync_base_path);

        if (is_directory_exists(current_client_sync_base_path)) {
            printf("Đường dẫn %s tồn tại.\n", current_client_sync_base_path);
            if (send_response(client_socket, "folder exists") == -1) return -1;
            int file_count = 0;

            list_files(current_client_sync_base_path, global_file_list, &file_count);

            int file_count_from_client;
            receive_file_count(client_socket, &file_count_from_client);
            if (send_response(client_socket, "Received file count") == -1) return -1;

            for (int i = 0; i < file_count_from_client; i++) {
                if (receive_file_info(client_socket, &global_file_info[i]) == -1) return -1;
                if (send_response(client_socket, "Receive file info ok!") == -1) return -1;

                // global_file_info[i].filepath currently holds the relative path from client.
                // Convert it to the absolute path on the server.
                char absolute_file_path_on_server[PATH_MAX];
                normalize_path_segment(absolute_file_path_on_server, current_client_sync_base_path, global_file_info[i].filepath);
                
                // Now overwrite global_file_info[i].filepath with the absolute path for consistent use.
                strncpy(global_file_info[i].filepath, absolute_file_path_on_server, sizeof(global_file_info[i].filepath) - 1);
                global_file_info[i].filepath[sizeof(global_file_info[i].filepath) - 1] = '\0';

                printf("ClientToServer_Tệp tin: %s\n", global_file_info[i].filename);
                printf("ClientToServer_Đường dẫn: %s\n", global_file_info[i].filepath);
                printf("ClientToServer_Kích thước: %ld bytes\n", global_file_info[i].filesize);
                printf("ClientToServer_Hash (MD5): ");
                for (int j = 0; j < 16; j++) {
                    printf("%02x", global_file_info[i].hash[j]);
                }
                printf("\n");

                if (!check_file_exists(global_file_info[i].filepath)) {
                    printf("%s không tồn tại!\n", global_file_info[i].filename);
                    if (send_response(client_socket, "File no exist") == -1) return -1;
                    if (receive_file_content(client_socket, global_file_info[i].filepath, global_file_info[i].filesize) == -1) return -1;
                    if (send_response(client_socket, "File received successfully.") == -1) return -1;
                } else {
                    printf("%s tồn tại!\n", global_file_info[i].filename);
                    if (send_response(client_socket, "File exist") == -1) return -1;

                    for (int index = 0; index < file_count; index++) {
                        // Correctly compare file paths for existing files on the server
                        char relative_path_on_server[PATH_MAX]; // Relative path of file on server compared to current_client_sync_base_path
                        
                        get_different_path(current_client_sync_base_path, global_file_list[index].filepath, relative_path_on_server);

                        // Compare the relative paths received from client with the relative paths of files on server
                        if (strcmp(relative_path_on_server, global_file_info[i].filepath) == 0 &&
                            strcmp(global_file_list[index].filename, global_file_info[i].filename) == 0) {
                            if (memcmp(global_file_list[index].hash, global_file_info[i].hash, sizeof(global_file_list[index].hash)) != 0) {
                                printf("File %s có thay đổi\n", global_file_list[index].filename);
                                if (send_response(client_socket, "File change") == -1) return -1;
                                if (receive_file_content(client_socket, global_file_info[i].filepath, global_file_info[i].filesize) == -1) return -1;
                                if (send_response(client_socket, "File received successfully.") == -1) return -1;
                                break;
                            } else {
                                printf("File %s không thay đổi\n", global_file_list[index].filename);
                                if (send_response(client_socket, "File no change") == -1) return -1;
                                break;
                            }
                        }
                    }
                }
            }
        } else {
            printf("Đường dẫn %s không tồn tại.\n", current_client_sync_base_path);
            if (send_response(client_socket, "folder no exists") == -1) return -1;
            while (1) {
                char current_relative_path_from_client[PATH_MAX];
                ssize_t bytes_received_path = recv(client_socket, current_relative_path_from_client, sizeof(current_relative_path_from_client) - 1, 0);
                if (bytes_received_path <= 0) {
                    if (bytes_received_path == 0) printf("Client disconnected during rsync relative path reception.\n");
                    else perror("Error receiving rsync relative path");
                    return -1;
                }
                current_relative_path_from_client[bytes_received_path] = '\0';
                printf("Server: Received rsync relative path: '%s'\n", current_relative_path_from_client); fflush(stdout);

                // Check for end signal
                if (strcmp(current_relative_path_from_client, "END_OF_TRANSFER\n") == 0) {
                    receive_folder_done = TRUE;
                    printf("Server: Received END_OF_TRANSFER for rsync.\n"); fflush(stdout);
                    if (send_response(client_socket, "Receive END_OF_TRANSFER successful") == -1) return -1;
                    break;
                }
                
                // Send response for receiving the relative path (now includes filename)
                if (send_response(client_socket, "Receive path successful") == -1) return -1;

                // Now, receive filesize from client
                long file_size_from_client;
                if (recv(client_socket, &file_size_from_client, sizeof(file_size_from_client), 0) <= 0) {
                    if (recv(client_socket, &file_size_from_client, sizeof(file_size_from_client), 0) == 0) printf("Client disconnected during file size reception for rsync.\n");
                    else perror("Lỗi nhận kích thước file cho rsync");
                    return -1;
                }
                file_size_from_client = ntohl(file_size_from_client); // Convert from network byte order to host byte order
                printf("Server: Received filesize for rsync: %ld bytes\n", file_size_from_client); fflush(stdout);
                if (send_response(client_socket, "Receive file size ok!") == -1) return -1;

                // Construct the absolute path where this specific file should be stored
                char final_file_full_path[PATH_MAX];
                // current_relative_path_from_client now contains the full relative path including filename
                normalize_path_segment(final_file_full_path, current_client_sync_base_path, current_relative_path_from_client);
                printf("DEBUG: final_file_full_path: '%s'\n", final_file_full_path); fflush(stdout);
                
                // Ensure target directory exists for this file
                char target_directory_for_file[PATH_MAX];
                strncpy(target_directory_for_file, final_file_full_path, sizeof(target_directory_for_file) - 1);
    target_directory_for_file[sizeof(target_directory_for_file) - 1] = '\0';
                remove_last_component(target_directory_for_file); // Get the directory part of the final path
                printf("DEBUG: target_directory_for_file (before create): '%s'\n", target_directory_for_file); fflush(stdout);

                if (create_directory_recursively(target_directory_for_file) == -1) {
                    perror("Error creating directory for rsync file target");
                    if (send_response(client_socket, "Error creating rsync directory") == -1) return -1;
                    return -1;
                }
                printf("DEBUG: Directory created/exists: '%s'\n", target_directory_for_file); fflush(stdout);

                // Now call receive_file_content with the full path and size
                if (receive_file_content(client_socket, final_file_full_path, file_size_from_client) == -1) return -1;
                if (send_response(client_socket, "File received successfully.") == -1) return -1;
            } // end while loop
        } // end else (folder no exists)
    } else if (strcmp(command, "ls") == 0) {
        printf("Hiển thị cây thư mục!\n");
        // Print tree from current_client_sync_base_path if it's set, otherwise STORAGE_PATH
        if (strlen(current_client_sync_base_path) > 0) {
            print_tree(current_client_sync_base_path, 0, client_socket);
        } else {
            print_tree(STORAGE_PATH, 0, client_socket);
        }
        printf("Gửi xong cây thư mục!\n");
    } else if (strcmp(command, "clone") == 0) {
        if (receive_directory_path(client_socket, temp_dir_path, sizeof(temp_dir_path)) == -1) return -1;
        if (send_response(client_socket, "Receive path successful") == -1) return -1;

        if (realpath(STORAGE_PATH, absolute_path) == NULL) {
            perror("Lỗi khi lấy đường dẫn tuyệt đối");
            return -1;
        }

        // Normalize absolute_path and temp_dir_path for clone command
        if (strlen(absolute_path) > 1 && absolute_path[strlen(absolute_path) - 1] == '/') {
            absolute_path[strlen(absolute_path) - 1] = '\0';
        }
        printf("DEBUG: absolute_path normalized (clone): '%s'\n", absolute_path); fflush(stdout);

        char normalized_client_clone_path[PATH_MAX];
        normalize_path_segment(normalized_client_clone_path, "", temp_dir_path); // Ensure temp_dir_path is normalized
        printf("DEBUG: temp_dir_path normalized (clone): '%s'\n", normalized_client_clone_path); fflush(stdout);

        normalize_path_segment(full_path, absolute_path, normalized_client_clone_path);
        printf("Đường dẫn nhận được từ client (clone): %s\n", full_path);

        // For clone, the 'full_path' here is the server's source, not client's destination.
        // So, this is the path that should be listed on the server.
        // We also need to store this path as the current client's sync base path for future operations
        // IF the client wants to sync to this same folder.
        // For now, let's assume clone doesn't implicitly set the sync base path for subsequent operations
        // unless the user explicitly uses 'rsync' with the same folder.
        // However, given the nature of the problem, the current_client_sync_base_path should be consistent.
        strncpy(current_client_sync_base_path, full_path, sizeof(current_client_sync_base_path) - 1);
        current_client_sync_base_path[sizeof(current_client_sync_base_path) - 1] = '\0';


        if (is_directory_exists(full_path)) {
            printf("Thư mục '%s' tồn tại.\n", full_path);
            if (send_response(client_socket, "folder exists") == -1) return -1;
            int file_count = 0;
            list_files(full_path, global_file_list, &file_count); // List files from server's source directory
            if (send_response(client_socket, "Received file count") == -1) return -1;
            if (receive_response(client_socket) == -1) return -1;

            if (folder_client_exist == FALSE) {
                for (int i = 0; i < file_count; i++) {
                    // Send file with its relative path from the server's clone source directory
                    if (send_file(client_socket, global_file_list[i].filepath, full_path) == -1) return -1;
                    if (receive_response(client_socket) == -1) return -1;
                }
                char end_signal[] = "END_OF_TRANSFER\n";
                printf("Sending end signal: %s\n", end_signal);
                if (send(client_socket, end_signal, strlen(end_signal), 0) < 0) {
                    perror("Failed to send END_OF_TRANSFER signal");
                    return -1;
                }
                folder_client_exist = TRUE;
            } else {
                send_file_count(client_socket, file_count);
                if (send_response(client_socket, "Sent file count for clone") == -1) return -1;
                if (receive_response(client_socket) == -1) return -1;

                char child_path[PATH_MAX];
                char temp[PATH_MAX];
                for (int i = 0; i < file_count; i++) {
                    memset(child_path, 0, sizeof(child_path));
                    memset(temp, 0, sizeof(temp));
                    // Get relative path of the file on the server from the clone source directory
                    get_different_path(full_path, global_file_list[i].filepath, temp);
                    strncpy(child_path, temp, sizeof(child_path) - 1);
                    child_path[sizeof(child_path) - 1] = '\0';

                    // Store the original full path temporarily before modifying global_file_list[i].filepath
                    char original_full_filepath_server[PATH_MAX];
                    strncpy(original_full_filepath_server, global_file_list[i].filepath, sizeof(original_full_filepath_server) - 1);
                    original_full_filepath_server[sizeof(original_full_filepath_server) - 1] = '\0';

                    strncpy(global_file_list[i].filepath, child_path, sizeof(global_file_list[i].filepath) - 1);
                    global_file_list[i].filepath[sizeof(global_file_list[i].filepath) - 1] = '\0';
                    printf("child_path: %s\n", global_file_list[i].filepath);
                    if (send_response(client_socket, "Sent file info for clone") == -1) return -1;
                    if (receive_response(client_socket) == -1) return -1;
                    if (file_in_client_exist == FALSE) {
                        printf("file đó ở client không tồn tại!\n");
                        // Use the original full path on the server to send the file
                        if (send_file(client_socket, original_full_filepath_server, full_path) == -1) return -1;
                        if (send_response(client_socket, "Sent file for clone (new)") == -1) return -1;
                        if (receive_response(client_socket) == -1) return -1;
                    } else {
                        printf("file đó ở client có tồn tại!\n");
                        if (receive_response(client_socket) == -1) return -1;
                        if (file_in_client_no_change == FALSE) {
                            // Use the original full path on the server to send the file
                            if (send_file(client_socket, original_full_filepath_server, full_path) == -1) return -1;
                            if (send_response(client_socket, "Sent file for clone (changed)") == -1) return -1;
                            if (receive_response(client_socket) == -1) return -1;
                        } else {
                            printf("File ở client không thay đổi gì so với server\n");
                            if (send_response(client_socket, "File not changed for clone") == -1) return -1;
                        }
                    }
                }
            }
        } else {
            printf("Thư mục '%s' không tồn tại.\n", full_path);
            if (send_response(client_socket, "folder no exists") == -1) return -1;
        }
    } else if (strcmp(command, "CREATE_FILE") == 0 || strcmp(command, "MODIFY_FILE") == 0) {
        char relative_path_from_client[PATH_MAX]; // This is the relative path (including filename)
        if (receive_directory_path(client_socket, relative_path_from_client, sizeof(relative_path_from_client)) == -1) return -1;
        if (send_response(client_socket, "Receive path successful") == -1) return -1;

        // Now, receive filesize from client
        long file_size_from_client;
        if (recv(client_socket, &file_size_from_client, sizeof(file_size_from_client), 0) <= 0) {
            if (recv(client_socket, &file_size_from_client, sizeof(file_size_from_client), 0) == 0) printf("Client disconnected during file size reception for CREATE_FILE/MODIFY_FILE.\n");
            else perror("Lỗi nhận kích thước file cho CREATE_FILE/MODIFY_FILE");
            return -1;
        }
        file_size_from_client = ntohl(file_size_from_client); // Convert from network byte order to host byte order
        printf("Server: Received filesize for CREATE_FILE/MODIFY_FILE: %ld bytes\n", file_size_from_client); fflush(stdout);

        if (send_response(client_socket, "Receive file size ok!") == -1) return -1;

        // Construct the full absolute path to the target file on the server.
        char final_file_full_path[PATH_MAX];
        // relative_path_from_client now contains the full relative path including filename
        normalize_path_segment(final_file_full_path, current_client_sync_base_path, relative_path_from_client);
        printf("DEBUG: final_file_full_path (CREATE/MODIFY): '%s'\n", final_file_full_path); fflush(stdout);

        // Ensure target directory exists for this file
        char target_directory_for_file[PATH_MAX];
        strncpy(target_directory_for_file, final_file_full_path, sizeof(target_directory_for_file) - 1);
        target_directory_for_file[sizeof(target_directory_for_file) - 1] = '\0';
        remove_last_component(target_directory_for_file); // Get the directory part of the final path

        if (create_directory_recursively(target_directory_for_file) == -1) {
            perror("Error creating target directory for file");
            if (send_response(client_socket, "Error creating target directory") == -1) return -1;
            return -1;
        }
        printf("DEBUG: Directory created/exists (CREATE/MODIFY): '%s'\n", target_directory_for_file); fflush(stdout);

        // Now call receive_file_content, passing the absolute target file path and size.
        if (receive_file_content(client_socket, final_file_full_path, file_size_from_client) == -1) return -1;
        if (send_response(client_socket, "File received successfully.") == -1) return -1;
    } else if (strcmp(command, "DELETE_FILE") == 0 || strcmp(command, "DELETE_DIR") == 0) {
        char relative_path_from_client[PATH_MAX]; // This is the relative path (including filename/dirname)
        if (receive_directory_path(client_socket, relative_path_from_client, sizeof(relative_path_from_client)) == -1) return -1;
        if (send_response(client_socket, "Receive path successful") == -1) return -1;

        // Use current_client_sync_base_path to construct the full path
        normalize_path_segment(full_path, current_client_sync_base_path, relative_path_from_client);
        printf("Đường dẫn đầy đủ để xóa: %s\n", full_path);

        if (strcmp(command, "DELETE_FILE") == 0) {
            if (remove(full_path) == 0) {
                printf("Deleted file: %s\n", full_path);
                if (send_response(client_socket, "File deleted") == -1) return -1;
            } else {
                perror("Error deleting file");
                if (send_response(client_socket, "Error deleting file") == -1) return -1;
                return -1;
            }
        } else { // DELETE_DIR
            if (remove_directory_recursively(full_path) == 0) {
                printf("Deleted directory: %s\n", full_path);
                if (send_response(client_socket, "Directory deleted") == -1) return -1;
            } else {
                perror("Error deleting directory recursively");
                if (send_response(client_socket, "Error deleting directory") == -1) return -1;
                return -1;
            }
        }
    } else if (strcmp(command, "RENAME") == 0) {
        char old_relative_path[PATH_MAX];
        char new_relative_path[PATH_MAX];
        char old_full_path[PATH_MAX];
        char new_full_path[PATH_MAX];

        // Receive old relative path
        if (receive_directory_path(client_socket, old_relative_path, sizeof(old_relative_path)) == -1) return -1;
        if (send_response(client_socket, "Received old path") == -1) return -1;

        // Receive new relative path
        if (receive_directory_path(client_socket, new_relative_path, sizeof(new_relative_path)) == -1) return -1;
        if (send_response(client_socket, "Received new path") == -1) return -1;

        // Construct full paths on server
        normalize_path_segment(old_full_path, current_client_sync_base_path, old_relative_path);
        normalize_path_segment(new_full_path, current_client_sync_base_path, new_relative_path);
        
        printf("Server: Renaming %s to %s\n", old_full_path, new_full_path);

        if (rename(old_full_path, new_full_path) == 0) {
            printf("Server: Renamed successfully.\n");
            if (send_response(client_socket, "Renamed successfully") == -1) return -1;
        } else {
            perror("Server: Error renaming file/directory");
            if (send_response(client_socket, "Error renaming") == -1) return -1;
            return -1;
        }
    }
    return 0;
}

int main() {
    //tạo thư mục lưu trữ
    create_storage();

    int server_fd, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // Cấu hình địa chỉ server
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Tạo socket
    server_fd = create_server_socket();

    // Liên kết socket với địa chỉ
    bind_server_socket(server_fd, &server_addr);

    // Lắng nghe kết nối từ client
    listen_for_connections(server_fd);

    while(1)
    {
        client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket < 0) {
            perror("Lỗi khi chấp nhận kết nối");
            close(server_fd);
            exit(EXIT_FAILURE);
        }
        printf("Client connected\n");

        // Clear current_client_sync_base_path for new client connection
        memset(current_client_sync_base_path, 0, sizeof(current_client_sync_base_path));

        int status;
        do
        {
            status = handle_command(client_socket);
        } while (status == 0);

        close(client_socket);
        printf("Client disconnected. Waiting for new connections...\n");
    }

    // Đóng server socket
    close(server_fd);
    return 0;
}