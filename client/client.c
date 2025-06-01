#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/inotify.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define MAX_PATH 1024
#define TRUE 1
#define FALSE 0
#define MAX_CLIENTS 10
#define MSG_TYPE_SIZE 4
#define MSG_LEN_SIZE 4

int client_fd;

// Định nghĩa các biến toàn cục để theo dõi trạng thái của quá trình đồng bộ hóa.
uint8_t folder_existed = TRUE;
uint8_t file_existed = TRUE;
uint8_t file_no_change = TRUE;
uint8_t receive_folder_done = FALSE;

// Đây là struct chứa đầy đủ thông tin để kiểm tra thay đổi file: tên, đường dẫn, kích thước, hash, thời gian chỉnh sửa.
typedef struct {
    char filename[MAX_PATH];
    char filepath[MAX_PATH];
    int64_t filesize;
    unsigned char hash[32];
    time_t timestamp;
} FileInfo; 

// Hàm nhận thông tin file từ server và lưu vào file_info
void receive_file_info(int client_fd, FileInfo *file_info) {
    char buffer[BUFFER_SIZE];

    // Nhận tên file
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) {
        perror("Lỗi nhận dữ liệu");
        return;
    }

    printf("Nhận tên file: %s\n", buffer);
    strncpy(file_info->filename, buffer, sizeof(file_info->filename) - 1);  // Lưu tên file vào struct
    // Phản hồi cho client
    const char *sp1 = "Đã nhận được tên file!";
    send_response(client_fd, sp1);

    // Nhận đường dẫn file
    //memset(buffer, 0, sizeof(buffer));

    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) {
        perror("Lỗi nhận dữ liệu");
        return;
    }

    printf("Nhận đường dẫn file: %s\n", buffer);
    strncpy(file_info->filepath, buffer, sizeof(file_info->filepath) - 1);  // Lưu đường dẫn vào struct
    // Phản hồi cho client
    const char *sp2 = "Đã nhận được đường dẫn tới file!";
    send_response(client_fd, sp2);

    // Nhận kích thước file
    //memset(buffer, 0, sizeof(buffer));
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) {
        perror("Lỗi nhận dữ liệu");
        return;
    }
    printf("Nhận kích thước file: %s\n", buffer);
    file_info->filesize = atol(buffer);  // Lưu kích thước file vào struct
    const char *sp3 = "Đã nhận được kích thước file!";
    send_response(client_fd, sp3);

    // Nhận hash của file
    //memset(buffer, 0, sizeof(buffer));
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) {
        perror("Lỗi nhận dữ liệu");
        return;
    }

    printf("Nhận hash: %s\n", buffer);

    // Xử lý và lưu hash vào struct
    int hash_index = 0;
    while (hash_index < 32 && sscanf(buffer + hash_index * 2, "%02hhx", &file_info->hash[hash_index]) == 1) {
        hash_index++;
    }

    // Phản hồi cho client
    const char *sp4 = "Đã hash file thành công!";
    send_response(client_fd, sp4);

    // Nhận timestamp
    if (recv(client_fd, buffer, sizeof(buffer), 0) <= 0) {
        perror("Lỗi nhận dữ liệu");
        return;
    }
    printf("Nhận timestamp: %s\n", buffer);
    file_info->timestamp = (time_t)atol(buffer);  // Lưu timestamp vào struct
    const char *sp5 = "Đã nhận được timestamp!";
    send_response(client_fd, sp5);
}

// Hàm để tạo thư mục con nếu nó không tồn tại. Hàm này sẽ tạo từng thư mục trong đường dẫn đã cho.
int create_child_directory(const char *path) {
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

// Hàm theo dõi thư mục và đồng bộ hóa các thay đổi với server.
void *watch_directory(void *arg) {
    if (arg == NULL) {
        fprintf(stderr, "Chưa truyền thư mục theo dõi.\n");
        pthread_exit(NULL);
    }

    const char *watch_path = (const char *)arg;

    struct stat st;
    if (stat(watch_path, &st) == -1 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Đường dẫn '%s' không tồn tại hoặc không phải thư mục.\n", watch_path);
        pthread_exit(NULL);
    }

    int inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd == -1) {
        perror("Không thể khởi tạo inotify");
        pthread_exit(NULL);
    }

    int wd = inotify_add_watch(inotify_fd, watch_path, IN_CREATE | IN_MODIFY | IN_DELETE);
    if (wd == -1) {
        perror("Không thể thêm watch");
        close(inotify_fd);
        pthread_exit(NULL);
    }

    printf("Đang theo dõi thư mục: %s\n", watch_path);

    char buffer[BUFFER_SIZE];
    while (1) {
        int length = read(inotify_fd, buffer, BUFFER_SIZE);
        if (length <= 0) {
            sleep(1);
            continue;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];

            if (event->len && (event->mask & (IN_CREATE | IN_MODIFY | IN_DELETE))) {
                printf("Sự kiện: %s %s\n",
                       (event->mask & IN_CREATE) ? "Tạo" :
                       (event->mask & IN_MODIFY) ? "Sửa" : "Xóa",
                       event->name);

                char full_path[MAX_PATH];
                snprintf(full_path, sizeof(full_path), "%s/%s", watch_path, event->name);

                    // Gửi file đến server
                sync_to_server(full_path, watch_path, client_fd);
            }

            i += sizeof(struct inotify_event) + event->len;
        }

        sleep(1);
    }

    inotify_rm_watch(inotify_fd, wd);
    close(inotify_fd);
    pthread_exit(NULL);
}

// Hàm đồng bộ hóa file với server. Hàm này sẽ gửi thông tin file và nội dung của nó đến server.
void sync_to_server(const char *filepath, const char *watch_path, int sockfd) {
    // Kiểm tra file có tồn tại không
    struct stat st;
    if (stat(filepath, &st) == -1 || !S_ISREG(st.st_mode)) {
        printf("Không phải file hoặc đã bị xóa: %s\n", filepath);
        return;
    }

    // Mở file để đọc nội dung
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        perror("Không thể mở file để gửi !");
        return;
    }

    // Tính hash SHA-256
    unsigned char hash[32];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);

    char file_buf[BUFFER_SIZE];
    size_t bytes_read;
    long total_size = 0;

    while ((bytes_read = fread(file_buf, 1, sizeof(file_buf), fp)) > 0) {
        SHA256_Update(&sha256, file_buf, bytes_read);
        total_size += bytes_read;
    }

    SHA256_Final(hash, &sha256);
    rewind(fp); // Quay lại đầu file để gửi sau

    // Lấy timestamp
    time_t modified_time = st.st_mtime;

    // Gửi lệnh 'rsync'
    send(sockfd, "rsync", strlen("rsync") + 1, 0);
    receive_response(sockfd); // Giả sử bạn có hàm nhận phản hồi

    // Gửi tên thư mục gốc
    send(sockfd, watch_path, strlen(watch_path) + 1, 0);
    receive_response(sockfd);

    // Gửi tên thư mục đích (tạm thời giống tên gốc)
    send(sockfd, watch_path, strlen(watch_path) + 1, 0);
    receive_response(sockfd);

    // Gửi số lượng file = 1
    int count = htonl(1);
    send(sockfd, &count, sizeof(count), 0);
    receive_response(sockfd);

    // Chuẩn bị và gửi FileInfo
    char filename[MAX_PATH];
    const char *base = strrchr(filepath, '/');
    strcpy(filename, base ? base + 1 : filepath);

    char relative_path[MAX_PATH];
    if (strncmp(filepath, watch_path, strlen(watch_path)) == 0) {
        snprintf(relative_path, sizeof(relative_path), "%s/", filepath + strlen(watch_path));
    } else {
        strcpy(relative_path, "./");
    }

    // Gửi từng thành phần thông tin
    send(sockfd, filename, strlen(filename) + 1, 0);
    receive_response(sockfd);
    send(sockfd, relative_path, strlen(relative_path) + 1, 0);
    receive_response(sockfd);

    char size_buf[64];
    snprintf(size_buf, sizeof(size_buf), "%ld", total_size);
    send(sockfd, size_buf, strlen(size_buf) + 1, 0);
    receive_response(sockfd);

    char hash_buf[65] = {0};
    for (int i = 0; i < 32; i++) {
        snprintf(hash_buf + i * 2, 3, "%02x", hash[i]);
    }
    send(sockfd, hash_buf, strlen(hash_buf) + 1, 0);
    receive_response(sockfd);

    char time_buf[64];
    snprintf(time_buf, sizeof(time_buf), "%ld", (long)modified_time);
    send(sockfd, time_buf, strlen(time_buf) + 1, 0);
    receive_response(sockfd);

    // Gửi nội dung file
    send(sockfd, &total_size, sizeof(total_size), 0);
    while ((bytes_read = fread(file_buf, 1, sizeof(file_buf), fp)) > 0) {
        send(sockfd, file_buf, bytes_read, 0);
    }

    fclose(fp);
    printf("File đã được gửi thành công: %s\n", filename);
}


void send_response(int client_socket, const char *message) {
    if (send(client_socket, message, strlen(message), 0) < 0) {
        perror("Failed to send response");
    } else {
        printf("Response sent to server: %s\n", message);
    }
}

void receive_response(int sockfd) {
    char buf[256];
    int n = recv(sockfd, buf, sizeof(buf), 0);
    if (n > 0) {
        buf[n] = '\0';
        printf("Server: %s\n", buf);
    }
}


int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Cách dùng: %s <IP_SERVER> <THU_MUC_CAN_THEO_DOI>\n", argv[0]);
        return 1;
    }

    const char *SERVER_ADDR = argv[1];
    const char *watch_path = argv[2]; // Thư mục cần theo dõi
    int option;
    struct sockaddr_in server_addr;

    // Tạo thread để theo dõi thư mục
    pthread_t watch_thread;
    if (pthread_create(&watch_thread, NULL, watch_directory, (void *)watch_path) != 0) {
        perror("Lỗi khi tạo thread theo dõi");
        return 1;
    }

    // Cấu hình địa chỉ server
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);  // Địa chỉ IP của server

    // Tạo socket
    client_fd = create_client_socket();

    // Kết nối đến server
    connect_to_server(client_fd, &server_addr);

    // Menu chính
    while (1) {
        handle_option(client_fd);
    }

    return 0;
}